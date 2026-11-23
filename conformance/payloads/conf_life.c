/*
 * Conformance payload for C-LIFE and part of C-CLIENT (spec 5, 9): the app whose
 * lifecycle the runner observes. It judges nothing and prints no AAR-CONF line;
 * what it does is chosen by conf:variant=<name> (conf_life.h lists them). The
 * runner reads the end state, the exit code, the fault report and the messages.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_life.h"

/* The groups are optional: the display-open variant asks for the display when the
 * platform has it. */
AAR_NOTE(0, 0, 0, AAR_GROUP_DISPLAY)

extern void aar_start(void);

/* ---- Small helpers -------------------------------------------------------------------- */

static int starts(const char *s, const char *prefix)
{
    return conf_str_prefix(s, prefix);
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t number(const char *s)
{
    uint32_t n = 0;

    while (*s >= '0' && *s <= '9')
        n = n * 10u + (uint32_t)(*s++ - '0');
    return n;
}

/* Sends a message with a 64-bit pair; a full queue is retried for 100 ms. */
static void report(const char *keyword, uint64_t a, uint64_t b)
{
    uint8_t m[CONF_LIFE_MSG_BYTES];
    uint64_t start = aar_time_us();
    int rc;

    conf_life_build(m, keyword, (uint32_t)a, (uint32_t)(a >> 32), (uint32_t)b,
                    (uint32_t)(b >> 32));
    do {
        rc = aar_msg_send(m);
        if (rc != AAR_E_BUSY)
            break;
        aar_sleep_us(1000);
    } while (aar_time_us() - start < 100000u);
}

/* Waits for a message of the runner for at most timeout_us; 1 when one came (in
 * m), 0 for none or a stop. */
static int wait_message(uint8_t *m, uint64_t timeout_us)
{
    uint64_t start = aar_time_us();

    while (aar_time_us() - start < timeout_us) {
        int rc = aar_msg_poll(m);

        if (rc == 1)
            return 1;
        if (rc < 0)
            return 0;
        if (aar_msg_wait(50000u) == AAR_E_IO)
            return aar_msg_poll(m) == 1;
    }
    return 0;
}

/* ---- Computing without a table call ----------------------------------------------------- */

static volatile uint32_t spin_sink;

static void spin_loops(uint64_t n)
{
    uint64_t i;

    for (i = 0; i < n; i++)
        spin_sink = (uint32_t)i;
}

/* Loops per microsecond, measured over at least 500 ms (the share of the CPU
 * that the app gets settles after the first 100 ms), times 1024. */
static uint64_t spin_rate(void)
{
    uint64_t n = 400000u;
    uint64_t dt;

    for (;;) {
        uint64_t t0 = aar_time_us();

        spin_loops(n);
        dt = aar_time_us() - t0;
        if (dt >= 500000u)
            break;
        n *= 2u;
    }
    return n * 1024u / dt;
}

/* Computes for about us microseconds with no table call (rate from spin_rate). */
static void spin_for(uint64_t rate, uint64_t us)
{
    spin_loops(rate * us / 1024u);
}

/* ---- Faults ------------------------------------------------------------------------------- */

/* True when a lies within margin of [lo, lo + len). */
static int near_range(uint64_t a, uint64_t lo, uint64_t len, uint64_t margin)
{
    return a + margin >= lo && a < lo + len + margin;
}

/* An address that no part of the app's own memory can contain (spec 5.3, 4.4): the first of a
 * short list that lies at least 16 MiB away from the image, the heap and the stack. The image
 * is taken as the 64 MiB from aar_start (offset 0 of the image) and the stack as 16 MiB around
 * the frame of this function. The default is CONF_LIFE_FAULT_ADDR_*; the runner reads the address
 * the app chose from its first report. */
static uint64_t fault_address(void)
{
    static const uint64_t margin = 16ull << 20;
#if defined(__aarch64__)
    static const uint64_t list[] = {
        ((uint64_t)CONF_LIFE_FAULT_ADDR_HI64 << 32) | CONF_LIFE_FAULT_ADDR_LO64,
        0x00007EAD00000000ull, 0x0000BEAD00000000ull, 0x00003EAD00000000ull,
    };
#else
    static const uint64_t list[] = {
        ((uint64_t)CONF_LIFE_FAULT_ADDR_HI32 << 32) | CONF_LIFE_FAULT_ADDR_LO32,
        0x7EAD0000ull, 0xBEAD0000ull, 0x3EAD0000ull,
    };
#endif
    void *heap = 0;
    uint64_t heap_bytes = 0;
    uint64_t image = (uint64_t)(uintptr_t)aar_start;
    uint64_t stack = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    uint32_t i;

    aar_heap_range(&heap, &heap_bytes);
    for (i = 0; i < sizeof list / sizeof list[0]; i++) {
        uint64_t a = list[i];

        if (near_range(a, image, 64ull << 20, margin))
            continue;
        if (near_range(a, (uint64_t)(uintptr_t)heap, heap_bytes, margin))
            continue;
        if (near_range(a, stack - margin, 2u * margin, margin))
            continue;
        return a;
    }
    return list[0];
}

/* The instruction labels: where the app faults. */
extern const char conf_life_data_insn[] __attribute__((visibility("hidden")));
extern const char conf_life_illegal_insn[] __attribute__((visibility("hidden")));
extern const char conf_life_align_insn[] __attribute__((visibility("hidden")));

/* An address the exclusive access of the align variant touches: 1 byte past an
 * aligned word. */
static volatile uint64_t exclusive_cell[2];

static void report_regs(uint64_t address, uint64_t pc, uint64_t sp)
{
    report(CONF_LIFE_FAULT_A, address, pc);
    report(CONF_LIFE_FAULT_B, sp, CONF_LIFE_LR_MAGIC);
    report(CONF_LIFE_FAULT_C, (uint64_t)(uintptr_t)aar_start, 0);
    /* The runner takes the messages while the app is still alive. */
    aar_sleep_us(100000u);
}

/* The stack pointer of the function that calls this: always inlined, so that it
 * is the sp of the faulting function. */
static inline __attribute__((always_inline)) uintptr_t read_sp(void)
{
    uintptr_t sp;

    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    return sp;
}

static void __attribute__((noinline, noreturn)) fault_data(void)
{
    uintptr_t addr = (uintptr_t)fault_address();
    uintptr_t sp = read_sp();

    report_regs(addr, (uint64_t)(uintptr_t)conf_life_data_insn, sp);
#if defined(__aarch64__)
    __asm__ __volatile__("movz w30, #0x4645\n\t"
                         "movk w30, #0x4C49, lsl #16\n\t"
                         ".globl conf_life_data_insn\n\t"
                         ".hidden conf_life_data_insn\n"
                         "conf_life_data_insn:\n\t"
                         "str x0, [%0]\n\t"
                         :
                         : "r"(addr)
                         : "x30", "memory");
#else
    __asm__ __volatile__("movw lr, #0x4645\n\t"
                         "movt lr, #0x4C49\n\t"
                         ".globl conf_life_data_insn\n\t"
                         ".hidden conf_life_data_insn\n"
                         "conf_life_data_insn:\n\t"
                         "str r0, [%0]\n\t"
                         :
                         : "r"(addr)
                         : "lr", "memory");
#endif
    __builtin_unreachable();
}

/* A call to an address that is not mapped: the program counter is that address. */
static void __attribute__((noinline, noreturn)) fault_fetch(void)
{
    uintptr_t addr = (uintptr_t)fault_address();

    report_regs(addr, addr, read_sp());
#if defined(__aarch64__)
    __asm__ __volatile__("blr %0\n\t" : : "r"(addr) : "x30", "memory");
#else
    __asm__ __volatile__("blx %0\n\t" : : "r"(addr) : "lr", "memory");
#endif
    __builtin_unreachable();
}

static void __attribute__((noinline, noreturn)) fault_illegal(void)
{
    report_regs(0, (uint64_t)(uintptr_t)conf_life_illegal_insn, read_sp());
#if defined(__aarch64__)
    __asm__ __volatile__(".globl conf_life_illegal_insn\n\t"
                         ".hidden conf_life_illegal_insn\n"
                         "conf_life_illegal_insn:\n\t"
                         ".inst 0x00000000\n\t" /* udf #0 */
                         : : : "memory");
#else
    __asm__ __volatile__(".globl conf_life_illegal_insn\n\t"
                         ".hidden conf_life_illegal_insn\n"
                         "conf_life_illegal_insn:\n\t"
                         ".inst 0xe7f000f0\n\t" /* udf #0 */
                         : : : "memory");
#endif
    __builtin_unreachable();
}

/* An exclusive load from an address that is not aligned to its size. */
static void __attribute__((noinline, noreturn)) fault_align(void)
{
    uintptr_t addr = (uintptr_t)&exclusive_cell[0] + 1u;

    report_regs(addr, (uint64_t)(uintptr_t)conf_life_align_insn, read_sp());
#if defined(__aarch64__)
    __asm__ __volatile__(".globl conf_life_align_insn\n\t"
                         ".hidden conf_life_align_insn\n"
                         "conf_life_align_insn:\n\t"
                         "ldxr x2, [%0]\n\t"
                         :
                         : "r"(addr)
                         : "x2", "memory");
#else
    __asm__ __volatile__(".globl conf_life_align_insn\n\t"
                         ".hidden conf_life_align_insn\n"
                         "conf_life_align_insn:\n\t"
                         "ldrex r2, [%0]\n\t"
                         :
                         : "r"(addr)
                         : "r2", "memory");
#endif
    __builtin_unreachable();
}

/* An instruction that needs privilege: a write of a system control register (C-ENTRY.23 and
 * C-ENTRY.24, spec 3.2). In user mode (EL0) it is an undefined instruction. */
static void __attribute__((noinline, noreturn)) fault_privileged(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("msr vbar_el1, %0\n\t" : : "r"((uint64_t)0) : "memory");
#else
    __asm__ __volatile__("mcr p15, 0, %0, c1, c0, 0\n\t" : : "r"(0u) : "memory");
#endif
    __builtin_unreachable();
}

/* ---- The variants ------------------------------------------------------------------------- */

static int v_stop(void)
{
    aar_event ev;
    uint32_t n = 0;
    int saw_event = 0;
    int flag;
    uint64_t t0;

    while (!aar_should_stop())
        aar_sleep_us(10000u);
    flag = aar_should_stop();
    /* Clean up: busy for 200 ms, which keeps the app in STOPPING for a while. */
    t0 = aar_time_us();
    while (aar_time_us() - t0 < CONF_LIFE_STOP_CLEANUP_US)
        aar_yield();
    while (aar_event_poll(&ev, 1, &n) == AAR_OK && n == 1) {
        if (ev.type == AAR_EV_STOP)
            saw_event = 1;
    }
    if (flag != 1)
        return CONF_LIFE_STOP_NO_FLAG;
    return saw_event ? CONF_LIFE_STOP_OK : CONF_LIFE_STOP_NO_EVENT;
}

static int v_wait_msg(void)
{
    uint8_t m[CONF_LIFE_MSG_BYTES];

    for (;;) {
        int rc = aar_msg_wait(100000u);

        if (aar_msg_poll(m) == 1)
            return 0;
        if (rc == AAR_E_IO)
            return 5;
    }
}

static int v_gap(int use_sleep, uint32_t ms)
{
    uint64_t rate = use_sleep ? 0 : spin_rate();
    uint64_t t0 = aar_time_us();
    uint64_t t1;

    if (use_sleep)
        aar_sleep_us(ms * 1000u);
    else
        spin_for(rate, (uint64_t)ms * 1000u);
    t1 = aar_time_us();
    report(CONF_LIFE_GAP, t1 - t0, (uint64_t)ms * 1000u);
    return 0;
}

/* One table call of the six that count as activity. */
static void activity_call(uint32_t kind)
{
    aar_stat st;
    uint32_t wrote = 0;

    switch (kind) {
    case 0:
        aar_yield();
        break;
    case 1:
        (void)aar_should_stop();
        break;
    case 2:
        (void)aar_time_us();
        break;
    case 3:
        (void)aar_msg_wait(1000u);
        break;
    case 4:
        (void)aar_file_stat("conf-life-none", &st);
        break;
    default:
        (void)aar_console_write(".", 1, &wrote);
        break;
    }
}

static int v_activity(void)
{
    uint64_t rate = spin_rate();
    uint32_t kind;
    uint32_t round;

    for (kind = 0; kind < 6u; kind++) {
        for (round = 0; round < 3u; round++) {
            spin_for(rate, 4000000u);
            activity_call(kind);
        }
    }
    return 0;
}

static int v_hold_shared(void)
{
    aar_shared sh;
    uint32_t i;
    int rc = aar_shared_alloc(CONF_LIFE_SHARED_BYTES, 0, 0, &sh);

    if (rc != AAR_OK)
        return 2;
    for (i = 0; i < CONF_LIFE_SHARED_BYTES; i++)
        ((volatile uint8_t *)sh.ptr)[i] = (uint8_t)i;
    (void)aar_publish(sh.ptr, CONF_LIFE_SHARED_BYTES);
    report(CONF_LIFE_TOKEN, sh.haddr, sh.bytes);
    while (!aar_should_stop())
        aar_sleep_us(20000u);
    return 0;
}

#define FILL_MAX 64u

static int v_fill_shared(void)
{
    aar_shared blocks[FILL_MAX];
    uint32_t count = 0;
    uint32_t n;
    uint32_t i;
    uint64_t t0;

    while (count < FILL_MAX &&
           aar_shared_alloc(CONF_LIFE_SHARED_BYTES, 0, 0, &blocks[count]) == AAR_OK)
        count++;
    if (count == 0)
        return 2;
    for (n = 0; n < count; n++) {
        volatile uint8_t *p = (volatile uint8_t *)blocks[n].ptr;

        for (i = 0; i < CONF_LIFE_SHARED_BYTES; i++)
            p[i] = conf_life_byte(n, i);
        (void)aar_publish(blocks[n].ptr, CONF_LIFE_SHARED_BYTES);
    }
    t0 = aar_time_us();
    while (aar_time_us() - t0 < 4000000u) {
        aar_sleep_us(50000u);
        for (n = 0; n < count; n++) {
            volatile uint8_t *p = (volatile uint8_t *)blocks[n].ptr;

            (void)aar_acquire(blocks[n].ptr, CONF_LIFE_SHARED_BYTES);
            for (i = 0; i < CONF_LIFE_SHARED_BYTES; i += 64u) {
                if (p[i] != conf_life_byte(n, i))
                    return 1;
            }
        }
    }
    return 0;
}

static int v_burst(uint32_t count)
{
    uint8_t m[CONF_LIFE_MSG_BYTES];
    aar_stat st;
    uint32_t i;

    if (count == 0)
        count = 1;
    aar_sleep_us(500000u);
    for (i = 0; i < count; i++)
        report(CONF_LIFE_MSG, i, 0);
    if (!wait_message(m, 10000000u))
        return 9;
    /* A file call parks the app until the client answers; a console write does not
     * (the ZZ9000 buffers it), and the app would end before the request is served. */
    (void)aar_file_stat("conf-life-none", &st);
    return 0;
}

static int v_ping(void)
{
    uint8_t m[CONF_LIFE_MSG_BYTES];

    while (!aar_should_stop()) {
        (void)aar_msg_wait(100000u);
        while (aar_msg_poll(m) == 1) {
            if (conf_life_is(m, CONF_LIFE_PING))
                report(CONF_LIFE_PONG, conf_life_get(m, 0), 0);
        }
    }
    return 0;
}

static int v_beat(void)
{
    uint32_t n = 0;

    while (!aar_should_stop()) {
        report(CONF_LIFE_BEAT, n++, 0);
        aar_sleep_us(50000u);
    }
    return 0;
}

static uint8_t bss_probe[4096];

static int v_hello(int argc, char **argv)
{
    struct conf_buf c;
    uint32_t nonzero = 0;
    uint32_t sum = 0;
    uint32_t i;
    int a;

    for (i = 0; i < sizeof bss_probe; i++)
        nonzero += bss_probe[i] != 0;
    for (a = 0; a < argc; a++) {
        const char *s = argv[a];

        while (*s)
            sum = sum * 31u + (uint32_t)(uint8_t)*s++;
    }
    conf_buf_clear(&c);
    conf_buf_str(&c, "conf_life hello bss=");
    conf_buf_u64(&c, nonzero);
    conf_buf_str(&c, " argc=");
    conf_buf_u64(&c, (uint32_t)argc);
    conf_buf_str(&c, " argsum=");
    conf_buf_u64(&c, sum);
    conf_emit(&c);
    return nonzero ? 1 : 0;
}

static int v_display(void)
{
    aar_surface s;
    int rc = aar_display_open(320, 200, AAR_FMT_RGB565, 0, &s);

    if (rc >= 0) {
        (void)aar_display_close(s.id);
        return 1;
    }
    return rc == AAR_E_NOSYS ? 0 : 1;
}

static int v_serve_all(void)
{
    static const char text[] = "hello";
    static const char name[] = "conf-life-serve.tmp";
    char buf[16];
    uint32_t n = 0;
    aar_stat st;
    aar_dirent de;
    int64_t pos = 0;
    int fd;
    int list;
    int found = 0;
    int rc;
    uint8_t m[CONF_LIFE_MSG_BYTES];

    fd = aar_file_open(name, AAR_FILE_WRITE | AAR_FILE_CREATE | AAR_FILE_TRUNCATE);
    if (fd < 0)
        return 1;
    if (aar_file_write(fd, text, 5, &n) != AAR_OK || n != 5u)
        return 2;
    if (aar_file_close(fd) != AAR_OK)
        return 3;
    fd = aar_file_open(name, AAR_FILE_READ);
    if (fd < 0)
        return 4;
    if (aar_file_seek(fd, 1, AAR_SEEK_SET, &pos) != AAR_OK || pos != 1)
        return 5;
    if (aar_file_read(fd, buf, 4, &n) != AAR_OK || n != 4u || buf[0] != 'e' ||
        buf[3] != 'o')
        return 6;
    if (aar_file_close(fd) != AAR_OK)
        return 7;
    if (aar_file_stat(name, &st) != AAR_OK || st.size != 5u)
        return 8;
    list = aar_file_list_open(".");
    if (list < 0)
        return 9;
    while ((rc = aar_file_list_next(list, &de)) == AAR_OK) {
        if (same(de.name, name))
            found = 1;
    }
    if (rc != AAR_E_NOENT || !found)
        return 10;
    if (aar_file_list_close(list) != AAR_OK)
        return 11;
    if (aar_console_write("serve-all console\n", 18, &n) != AAR_OK || n != 18u)
        return 12;
    aar_log("serve-all log", 13);
    conf_life_build(m, CONF_LIFE_MSG, 7, 0, 0, 0);
    if (aar_msg_send(m) != AAR_OK)
        return 13;
    return 0;
}

int aar_main(int argc, char **argv)
{
    const char *v = conf_arg(argc, argv, "conf:variant");
    const char *ms = conf_arg(argc, argv, "conf:ms");
    uint32_t n = ms ? number(ms) : 0;

    if (!v)
        return CONF_LIFE_UNKNOWN_VARIANT;
    if (starts(v, CONF_LIFE_V_RET))
        return (int)number(v + 4);
    if (starts(v, CONF_LIFE_V_EXIT))
        aar_exit((int)number(v + 5));
    if (same(v, CONF_LIFE_V_STOP))
        return v_stop();
    if (same(v, CONF_LIFE_V_IGNORE)) {
        for (;;)
            aar_yield();
    }
    if (same(v, CONF_LIFE_V_FAULT_DATA))
        fault_data();
    if (same(v, CONF_LIFE_V_FAULT_FETCH))
        fault_fetch();
    if (same(v, CONF_LIFE_V_FAULT_ILLEGAL))
        fault_illegal();
    if (same(v, CONF_LIFE_V_FAULT_ALIGN))
        fault_align();
    if (same(v, CONF_LIFE_V_FAULT_QUIET)) {
        aar_sleep_us(500000u);
        *(volatile uint32_t *)(uintptr_t)fault_address() = 0;
        return 0;
    }
    if (same(v, CONF_LIFE_V_FAULT_PRIVILEGED))
        fault_privileged();
    if (same(v, CONF_LIFE_V_WAIT_MSG))
        return v_wait_msg();
    if (same(v, CONF_LIFE_V_SPIN))
        return v_gap(0, n);
    if (same(v, CONF_LIFE_V_SLEEP))
        return v_gap(1, n);
    if (same(v, CONF_LIFE_V_ACTIVITY))
        return v_activity();
    if (same(v, CONF_LIFE_V_HOLD_SHARED))
        return v_hold_shared();
    if (same(v, CONF_LIFE_V_FILL_SHARED))
        return v_fill_shared();
    if (same(v, CONF_LIFE_V_BURST))
        return v_burst(n);
    if (same(v, CONF_LIFE_V_PING))
        return v_ping();
    if (same(v, CONF_LIFE_V_BEAT))
        return v_beat();
    if (same(v, CONF_LIFE_V_HELLO))
        return v_hello(argc, argv);
    if (same(v, CONF_LIFE_V_DISPLAY))
        return v_display();
    if (same(v, CONF_LIFE_V_SERVE_ALL))
        return v_serve_all();
    return CONF_LIFE_UNKNOWN_VARIANT;
}
