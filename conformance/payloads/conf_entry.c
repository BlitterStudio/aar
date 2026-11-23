/*
 * Conformance payload for C-ENTRY (spec 3.2, entry contract): the state the
 * runtime gives the app. See conformance/TESTS.md for the ids and
 * conformance/payloads/conf.h for the protocol.
 *
 * The payload owns the ELF entry symbol. conf_entry_start (linked with
 * -Wl,-e,conf_entry_start, see conf_entry.ldlibs) is the first code the runtime
 * enters: it stores every register the contract talks about into a frame on the
 * stack, copies the frame to a static structure, counts the non-zero words of a
 * 64 KiB static array (BSS at entry), puts the registers back and jumps to
 * aar_start, the crt0 of libaar. Nothing else changes, so the app runs as any
 * other. aar_main then judges the snapshot. The host build has no entry state:
 * its register tests are not printed (the profile ids) or skipped.
 *
 * Variants (the runner passes conf:variant=<name>, conformance/runner/r_entry.c):
 *   main   everything but C-ENTRY.4
 *   long   C-ENTRY.4 only: 16 arguments of 127 bytes (conf_entry.h)
 * Run without a runner (host runner, qemu loader) the payload does the `main`
 * tests that make sense there and skips C-ENTRY.4 and what needs the runner.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_entry.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

AAR_NOTE(0, 0, 0, 0)

#if (defined(__arm__) || defined(__aarch64__)) && !defined(AAR_HOST)
#define ENTRY_ARM 1
#endif

/* ---- The snapshot ------------------------------------------------------------- */

#define PROBE_BYTES 65536u
/* 64 KiB of BSS. Nothing writes it: conf_entry_capture counts its non-zero words
 * before any constructor runs. Not static, and read through volatile, so the
 * compiler cannot fold the reads into zeros. */
uint32_t conf_entry_bss_probe[PROBE_BYTES / 4u];

#ifdef ENTRY_ARM

#define SNAP_MAGIC 0x454e5452u

#if defined(__aarch64__)
/* x0 to x30, sp, fpcr, fpsr, v0 to v31 (as 64 low and high words). 784 bytes. */
struct entry_regs {
    uint64_t x[31];
    uint64_t sp, fpcr, fpsr;
    uint64_t v[64];
};
_Static_assert(sizeof(struct entry_regs) == 784, "A64 entry frame");
#else
/* r0 to r12, lr, sp, fpscr, d0 to d31. 320 bytes. */
struct entry_regs {
    uint32_t r[13];
    uint32_t lr, sp, fpscr;
    uint64_t d[32];
};
_Static_assert(sizeof(struct entry_regs) == 320, "A32 entry frame");
#endif

struct entry_state {
    struct entry_regs regs;
    uint32_t valid;
    uint32_t bss_bad;       /* non-zero words in conf_entry_bss_probe at entry */
    uint32_t bss_first_bad; /* index of the first one */
};
static struct entry_state snap;

/* Called by conf_entry_start with the frame it stored. */
__attribute__((used, noinline)) void conf_entry_capture(const struct entry_regs *frame)
{
    const uint32_t *src = (const uint32_t *)frame;
    uint32_t *dst = (uint32_t *)&snap.regs;
    const volatile uint32_t *probe = conf_entry_bss_probe;
    uint32_t i;

    for (i = 0; i < sizeof(struct entry_regs) / 4u; i++)
        dst[i] = src[i];
    for (i = 0; i < PROBE_BYTES / 4u; i++) {
        if (probe[i] != 0u) {
            if (snap.bss_bad == 0u)
                snap.bss_first_bad = i;
            snap.bss_bad++;
        }
    }
    snap.valid = SNAP_MAGIC;
}

#if defined(__aarch64__)
__asm__(
    ".section .text.conf_entry_start, \"ax\", %progbits\n"
    ".balign 4\n"
    ".global conf_entry_start\n"
    ".type conf_entry_start, %function\n"
    "conf_entry_start:\n"
    "    sub  sp, sp, #784\n"
    "    stp  x0, x1, [sp, #0]\n"
    "    stp  x2, x3, [sp, #16]\n"
    "    stp  x4, x5, [sp, #32]\n"
    "    stp  x6, x7, [sp, #48]\n"
    "    stp  x8, x9, [sp, #64]\n"
    "    stp  x10, x11, [sp, #80]\n"
    "    stp  x12, x13, [sp, #96]\n"
    "    stp  x14, x15, [sp, #112]\n"
    "    stp  x16, x17, [sp, #128]\n"
    "    stp  x18, x19, [sp, #144]\n"
    "    stp  x20, x21, [sp, #160]\n"
    "    stp  x22, x23, [sp, #176]\n"
    "    stp  x24, x25, [sp, #192]\n"
    "    stp  x26, x27, [sp, #208]\n"
    "    stp  x28, x29, [sp, #224]\n"
    "    str  x30, [sp, #240]\n"
    "    add  x3, sp, #784\n"
    "    str  x3, [sp, #248]\n"
    "    mrs  x3, fpcr\n"
    "    mrs  x4, fpsr\n"
    "    stp  x3, x4, [sp, #256]\n"
    "    add  x3, sp, #272\n"
    "    stp  q0, q1, [x3, #0]\n"
    "    stp  q2, q3, [x3, #32]\n"
    "    stp  q4, q5, [x3, #64]\n"
    "    stp  q6, q7, [x3, #96]\n"
    "    stp  q8, q9, [x3, #128]\n"
    "    stp  q10, q11, [x3, #160]\n"
    "    stp  q12, q13, [x3, #192]\n"
    "    stp  q14, q15, [x3, #224]\n"
    "    stp  q16, q17, [x3, #256]\n"
    "    stp  q18, q19, [x3, #288]\n"
    "    stp  q20, q21, [x3, #320]\n"
    "    stp  q22, q23, [x3, #352]\n"
    "    stp  q24, q25, [x3, #384]\n"
    "    stp  q26, q27, [x3, #416]\n"
    "    stp  q28, q29, [x3, #448]\n"
    "    stp  q30, q31, [x3, #480]\n"
    "    mov  x0, sp\n"
    "    bl   conf_entry_capture\n"
    "    add  x3, sp, #272\n"
    "    ldp  q0, q1, [x3, #0]\n"
    "    ldp  q2, q3, [x3, #32]\n"
    "    ldp  q4, q5, [x3, #64]\n"
    "    ldp  q6, q7, [x3, #96]\n"
    "    ldp  q8, q9, [x3, #128]\n"
    "    ldp  q10, q11, [x3, #160]\n"
    "    ldp  q12, q13, [x3, #192]\n"
    "    ldp  q14, q15, [x3, #224]\n"
    "    ldp  q16, q17, [x3, #256]\n"
    "    ldp  q18, q19, [x3, #288]\n"
    "    ldp  q20, q21, [x3, #320]\n"
    "    ldp  q22, q23, [x3, #352]\n"
    "    ldp  q24, q25, [x3, #384]\n"
    "    ldp  q26, q27, [x3, #416]\n"
    "    ldp  q28, q29, [x3, #448]\n"
    "    ldp  q30, q31, [x3, #480]\n"
    "    ldp  x3, x4, [sp, #256]\n"
    "    msr  fpcr, x3\n"
    "    msr  fpsr, x4\n"
    "    ldr  x30, [sp, #240]\n"
    "    ldp  x28, x29, [sp, #224]\n"
    "    ldp  x26, x27, [sp, #208]\n"
    "    ldp  x24, x25, [sp, #192]\n"
    "    ldp  x22, x23, [sp, #176]\n"
    "    ldp  x20, x21, [sp, #160]\n"
    "    ldp  x18, x19, [sp, #144]\n"
    "    ldp  x16, x17, [sp, #128]\n"
    "    ldp  x14, x15, [sp, #112]\n"
    "    ldp  x12, x13, [sp, #96]\n"
    "    ldp  x10, x11, [sp, #80]\n"
    "    ldp  x8, x9, [sp, #64]\n"
    "    ldp  x6, x7, [sp, #48]\n"
    "    ldp  x4, x5, [sp, #32]\n"
    "    ldp  x2, x3, [sp, #16]\n"
    "    ldp  x0, x1, [sp, #0]\n"
    "    add  sp, sp, #784\n"
    "    b    aar_start\n"
    ".size conf_entry_start, . - conf_entry_start\n");
#else
__asm__(
    ".section .text.conf_entry_start, \"ax\", %progbits\n"
    ".arm\n"
    ".balign 4\n"
    ".fpu neon\n"
    ".global conf_entry_start\n"
    ".type conf_entry_start, %function\n"
    "conf_entry_start:\n"
    "    sub    sp, sp, #320\n"
    "    stmia  sp, {r0-r12}\n"
    "    str    lr, [sp, #52]\n"
    "    add    r3, sp, #320\n"
    "    str    r3, [sp, #56]\n"
    "    vmrs   r3, fpscr\n"
    "    str    r3, [sp, #60]\n"
    "    add    r3, sp, #64\n"
    "    vstmia r3, {d0-d15}\n"
    "    add    r3, sp, #192\n"
    "    vstmia r3, {d16-d31}\n"
    "    mov    r0, sp\n"
    "    bl     conf_entry_capture\n"
    "    add    r3, sp, #192\n"
    "    vldmia r3, {d16-d31}\n"
    "    add    r3, sp, #64\n"
    "    vldmia r3, {d0-d15}\n"
    "    ldr    r3, [sp, #60]\n"
    "    vmsr   fpscr, r3\n"
    "    ldr    lr, [sp, #52]\n"
    "    ldmia  sp, {r0-r12}\n"
    "    add    sp, sp, #320\n"
    "    b      aar_start\n"
    ".size conf_entry_start, . - conf_entry_start\n");
#endif

#endif /* ENTRY_ARM */

/* ---- Constructors and init arrays (C-ENTRY.20 to C-ENTRY.22) ---------------------- */

static volatile uint32_t order; /* the order the init functions ran in */
static uint32_t early_at, early_runs; /* constructor(101) */
static uint32_t late_at, late_runs;   /* constructor, no priority */

__attribute__((constructor(101), used)) static void ctor_early(void)
{
    early_at = ++order;
    early_runs++;
}

__attribute__((constructor, used)) static void ctor_late(void)
{
    late_at = ++order;
    late_runs++;
}

#ifdef ENTRY_ARM
static uint32_t pre_at, pre_api_set;

static void preinit_fn(void)
{
    pre_at = ++order;
    pre_api_set = aar_api_ptr != 0;
}

/* A writable pointer in .preinit_array: the loader relocates it like the
 * init_array entries. */
static void (*preinit_ptr)(void) __attribute__((section(".preinit_array"), used)) =
    preinit_fn;
#endif

/* ---- Small helpers --------------------------------------------------------------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Length up to max; max + 1 when there is no NUL within max bytes. */
static uint32_t str_len(const char *s, uint32_t max)
{
    uint32_t n = 0;

    while (n <= max && s[n])
        n++;
    return n;
}

/* The last path component: after the last '/' or ':'. */
static const char *last_component(const char *path)
{
    const char *last = path;

    for (; *path; path++) {
        if (*path == '/' || *path == ':')
            last = path + 1;
    }
    return last;
}

static int starts_with(const char *s, const char *prefix)
{
    return conf_str_prefix(s, prefix);
}

enum { MODE_HOST, MODE_MAIN, MODE_LONG };

/* One test that ends in a skip when it passed but could not check everything
 * because the environment has no runner or no entry state. */
static void end_or_skip(struct conf_test *t, int partial, const char *why)
{
    if (!t->failed && partial)
        conf_skip(t->id, why);
    else
        conf_end(t);
}

#ifdef ENTRY_ARM
static void need_snapshot(struct conf_test *t)
{
    conf_check(t, snap.valid == SNAP_MAGIC,
               "no entry snapshot: the image was not linked with -Wl,-e,conf_entry_start");
}

static uint64_t gpr(uint32_t n)
{
#if defined(__aarch64__)
    return snap.regs.x[n];
#else
    return snap.regs.r[n];
#endif
}
#endif

/* ---- C-ENTRY.1 to C-ENTRY.3: table, argc, argv ---------------------------------------- */

static void test_table_pointer(void)
{
    struct conf_test t;
    const aar_api *api = aar_api_ptr;

    conf_begin(&t, "C-ENTRY.1");
    conf_check(&t, api != 0, "aar_api_ptr is NULL");
    if (api)
        conf_check(&t, AAR_API_COVERS(api, msg_wait),
                   "the table size does not cover the core slots (exit to msg_wait)");
#ifdef ENTRY_ARM
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC) {
        conf_check(&t, gpr(0) != 0, "the table pointer at entry (r0/x0) is 0");
        conf_check(&t, gpr(0) == (uint64_t)(uintptr_t)api,
                   "the table pointer at entry (r0/x0) differs from aar_api_ptr");
    }
    conf_end(&t);
#else
    end_or_skip(&t, 1, "host: the entry registers are not observable");
#endif
}

static void test_argc_argv(int argc, char **argv, int mode)
{
    struct conf_test t;
    int i;
    int runner_args;

    conf_begin(&t, "C-ENTRY.2");
    conf_check(&t, argc >= 1 && argv != 0, "argc is below 1 or argv is NULL");
#ifdef ENTRY_ARM
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC) {
        conf_check(&t, (uint32_t)gpr(1) == (uint32_t)argc,
                   "argc at entry (r1/w1) differs from the argc of aar_main");
        conf_check(&t, gpr(2) == (uint64_t)(uintptr_t)argv,
                   "argv at entry (r2/x2) differs from the argv of aar_main");
    }
#endif
    runner_args = argc >= 3 && str_eq(argv[1], "alpha");
    if (runner_args) {
        conf_check(&t, str_eq(argv[2], "beta gamma"),
                   "argv[2] is not \"beta gamma\"");
        if (mode == MODE_MAIN) {
            conf_check(&t, argc >= 4 && str_eq(argv[3], "conf:peer=1"),
                       "argv[3] is not conf:peer=1");
            for (i = 4; i < argc; i++)
                conf_check(&t, starts_with(argv[i], "info:") ||
                               starts_with(argv[i], "conf:"),
                           "an argument after conf:peer=1 is not an info: or conf: word");
        }
        conf_buf_str(&t.note, "argc ");
        conf_buf_u64(&t.note, (uint64_t)argc);
    } else if (mode == MODE_MAIN) {
        conf_check(&t, 0, "argv[1] is not \"alpha\": the runner's arguments did not arrive");
    }
#ifdef ENTRY_ARM
    end_or_skip(&t, !runner_args, "host: no runner arguments");
#else
    end_or_skip(&t, 1, "host: the entry registers are not observable");
#endif
}

static void test_argv0(int argc, char **argv, int mode)
{
    struct conf_test t;
    const char *name;

    conf_begin(&t, "C-ENTRY.3");
    if (!conf_check(&t, argc >= 1 && argv && argv[0] && argv[0][0],
                    "argv[0] is missing or empty")) {
        conf_end(&t);
        return;
    }
    name = last_component(argv[0]);
    if (mode == MODE_MAIN) {
        conf_check(&t, str_eq(name, CONF_ENTRY_FILE),
                   "the last component of argv[0] is not conf_entry.elf");
    } else {
        conf_check(&t, starts_with(name, "conf_entry") || str_eq(name, "app"),
                   "the last component of argv[0] is not the program's name");
    }
    conf_buf_str(&t.note, "argv[0] ends in ");
    conf_buf_str(&t.note, name);
    conf_end(&t);
}

/* ---- C-ENTRY.4: 16 long arguments ------------------------------------------------------ */

static void test_long_args(int argc, char **argv)
{
    struct conf_test t;
    uint32_t i;
    uint32_t j;

    conf_begin(&t, "C-ENTRY.4");
    conf_check(&t, argc == (int)CONF_ENTRY_LONG_ARGS, "argc is not 16");
    for (i = 0; i < CONF_ENTRY_LONG_ARGS && (int)i < argc; i++) {
        const char *a = argv[i];
        uint32_t n;

        if (!conf_check(&t, a != 0, "an argument is NULL"))
            break;
        n = str_len(a, CONF_ENTRY_LONG_BYTES + 1u);
        if (n != CONF_ENTRY_LONG_BYTES) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "argument ");
                conf_buf_u64(&t.why, i);
                conf_buf_str(&t.why, " is ");
                conf_buf_u64(&t.why, n);
                conf_buf_str(&t.why, " bytes, not 127");
            }
            continue;
        }
        if (i == 0) {
            const char *name = a + CONF_ENTRY_LONG_BYTES - (sizeof CONF_ENTRY_FILE - 1u);

            conf_check(&t, str_eq(name, CONF_ENTRY_FILE) && name[-1] == '/',
                       "argv[0] does not end in /conf_entry.elf");
            continue;
        }
        for (j = 0; j < CONF_ENTRY_LONG_BYTES; j++) {
            if (a[j] != conf_entry_long_char(i, j)) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "argument ");
                    conf_buf_u64(&t.why, i);
                    conf_buf_str(&t.why, " differs at byte ");
                    conf_buf_u64(&t.why, j);
                }
                break;
            }
        }
    }
    conf_buf_str(&t.note, "16 arguments of 127 bytes intact");
    conf_end(&t);
}

/* ---- C-ENTRY.5 to C-ENTRY.14: registers ------------------------------------------------- */

#ifdef ENTRY_ARM

/* The first non-zero word among the registers [from, to] of a general register
 * range: names the register in t->why. */
static void check_gpr_zero(struct conf_test *t, uint32_t from, uint32_t to,
                           const char *prefix)
{
    uint32_t n;

    for (n = from; n <= to; n++) {
        if (gpr(n) != 0 && conf_fail_first(t)) {
            conf_buf_str(&t->why, prefix);
            conf_buf_u64(&t->why, n);
            conf_buf_str(&t->why, " is ");
            conf_buf_hex(&t->why, gpr(n));
            conf_buf_str(&t->why, " at entry, not 0");
        }
    }
}

#if defined(__aarch64__)

static void test_sp(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.6");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC && (snap.regs.sp & 15u) != 0 && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "sp at entry is ");
        conf_buf_hex(&t.why, snap.regs.sp);
        conf_buf_str(&t.why, ", not 16-byte aligned");
    }
    conf_buf_str(&t.note, "sp ");
    conf_buf_hex(&t.note, snap.regs.sp);
    conf_end(&t);
}

static void test_link(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.8");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC) {
        conf_check(&t, snap.regs.x[30] == 0, "x30 is not 0 at entry");
        conf_check(&t, snap.regs.x[29] == 0, "x29 is not 0 at entry");
    }
    conf_end(&t);
}

static void test_scratch(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.10");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC)
        check_gpr_zero(&t, 3, 28, "x");
    conf_end(&t);
}

static void test_fpstate(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.12");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC) {
        conf_check(&t, snap.regs.fpcr == 0, "FPCR is not 0 at entry");
        conf_check(&t, snap.regs.fpsr == 0, "FPSR is not 0 at entry");
    }
    conf_end(&t);
}

static void test_vregs(void)
{
    struct conf_test t;
    uint32_t n;

    conf_begin(&t, "C-ENTRY.14");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC) {
        for (n = 0; n < 32; n++) {
            if ((snap.regs.v[2 * n] | snap.regs.v[2 * n + 1]) != 0 &&
                conf_fail_first(&t)) {
                conf_buf_str(&t.why, "v");
                conf_buf_u64(&t.why, n);
                conf_buf_str(&t.why, " is not 0 at entry");
            }
        }
    }
    conf_end(&t);
}

#else /* A32 */

static void test_sp(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.5");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC && (snap.regs.sp & 7u) != 0 && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "sp at entry is ");
        conf_buf_hex(&t.why, snap.regs.sp);
        conf_buf_str(&t.why, ", not 8-byte aligned");
    }
    conf_buf_str(&t.note, "sp ");
    conf_buf_hex(&t.note, snap.regs.sp);
    conf_end(&t);
}

static void test_link(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.7");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC)
        conf_check(&t, snap.regs.lr == 0, "lr is not 0 at entry");
    conf_end(&t);
}

static void test_scratch(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.9");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC)
        check_gpr_zero(&t, 3, 12, "r");
    conf_end(&t);
}

static void test_fpstate(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.11");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC)
        conf_check(&t, snap.regs.fpscr == 0, "FPSCR is not 0 at entry");
    conf_end(&t);
}

static void test_vregs(void)
{
    struct conf_test t;
    uint32_t n;

    conf_begin(&t, "C-ENTRY.13");
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC) {
        for (n = 0; n < 32; n++) {
            if (snap.regs.d[n] != 0 && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "d");
                conf_buf_u64(&t.why, n);
                conf_buf_str(&t.why, " is not 0 at entry");
            }
        }
    }
    conf_end(&t);
}

#endif /* A32 */

#endif /* ENTRY_ARM */

/* ---- C-ENTRY.15: floating point and SIMD run ---------------------------------------------- */

static void test_fp_simd(void)
{
#if defined(ENTRY_ARM) && defined(__ARM_NEON)
    struct conf_test t;
    volatile float fa = 1.5f, fb = 2.0f;
    volatile double da = 0.25, db = 4.0;
    volatile uint32_t in[4] = { 1u, 2u, 3u, 4u };
    volatile float fin[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    uint32_t tmp[4];
    float ftmp[4];
    uint32_t out[4];
    float fout[4];
    float f;
    double d;
    uint32_t i;

    conf_begin(&t, "C-ENTRY.15");
    f = fa * fb;                    /* 3.0, single precision */
    d = da * db + (double)f;        /* 1.0 + 3.0, double precision */
    conf_check(&t, f == 3.0f, "single-precision multiply gave the wrong result");
    conf_check(&t, d == 4.0, "double-precision multiply and add gave the wrong result");
    for (i = 0; i < 4; i++) {
        tmp[i] = in[i];
        ftmp[i] = fin[i];
    }
    vst1q_u32(out, vaddq_u32(vld1q_u32(tmp), vld1q_u32(tmp)));
    vst1q_f32(fout, vmulq_f32(vld1q_f32(ftmp), vld1q_f32(ftmp)));
    for (i = 0; i < 4; i++) {
        conf_check(&t, out[i] == 2u * (i + 1u), "an integer SIMD add gave the wrong result");
        conf_check(&t, fout[i] == (float)((i + 1u) * (i + 1u)),
                   "a SIMD float multiply gave the wrong result");
    }
    conf_buf_str(&t.note, "scalar single and double, SIMD integer and float");
    conf_end(&t);
#else
    conf_skip("C-ENTRY.15", "host: no Arm floating-point and SIMD unit");
#endif
}

/* ---- C-ENTRY.16 and C-ENTRY.17: byte order, relocation -------------------------------------- */

static void test_endian(void)
{
    struct conf_test t;
    volatile uint32_t word = 0u;
    const volatile uint8_t *bytes = (const volatile uint8_t *)&word;

    conf_begin(&t, "C-ENTRY.16");
    word = 0x04030201u;
    conf_check(&t, bytes[0] == 0x01 && bytes[1] == 0x02 && bytes[2] == 0x03 &&
                       bytes[3] == 0x04,
               "a stored 32-bit word does not read back low byte first");
    conf_end(&t);
}

static int fn_one(void)
{
    return 101;
}

static int fn_two(void)
{
    return 202;
}

static int fn_three(int x)
{
    return x * 3;
}

static int data_cell = 4242;
static const char text_one[] = "relocated";
static const char text_two[] = "pointers";

struct reloc_entry {
    int (*fn)(void);
    const char *text;
    int *cell;
    struct reloc_entry *next;
};

static struct reloc_entry reloc_tab_a = { fn_one, text_one, &data_cell, &reloc_tab_a };
static struct reloc_entry reloc_tab_b = { fn_two, text_two, &data_cell, &reloc_tab_a };
static int (*const fn_table[])(int) = { fn_three };
static const char *const text_table[] = { text_one, text_two, "literal" };

static void test_relocation(void)
{
    struct conf_test t;
    struct reloc_entry *volatile a = &reloc_tab_a;
    struct reloc_entry *volatile b = &reloc_tab_b;
    int (*const *volatile fns)(int) = fn_table;
    const char *const *volatile texts = text_table;

    conf_begin(&t, "C-ENTRY.17");
    conf_check(&t, a->fn() == 101 && b->fn() == 202,
               "a relocated function pointer in a data table does not call its function");
    conf_check(&t, str_eq(a->text, "relocated") && str_eq(b->text, "pointers"),
               "a relocated string pointer in a data table is wrong");
    conf_check(&t, *a->cell == 4242 && a->cell == b->cell,
               "a relocated data pointer in a data table is wrong");
    conf_check(&t, b->next == a && a->next == a,
               "a relocated pointer to a table entry is wrong");
    conf_check(&t, fns[0](14) == 42, "a relocated const function table is wrong");
    conf_check(&t, str_eq(texts[0], "relocated") && str_eq(texts[2], "literal"),
               "a relocated const string table is wrong");
    conf_end(&t);
}

/* ---- C-ENTRY.18: BSS ------------------------------------------------------------------------- */

static void test_bss(void)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.18");
#ifdef ENTRY_ARM
    need_snapshot(&t);
    if (snap.valid == SNAP_MAGIC && snap.bss_bad != 0 && conf_fail_first(&t)) {
        conf_buf_u64(&t.why, snap.bss_bad);
        conf_buf_str(&t.why, " of the 16384 words of a 64 KiB BSS array were not 0 at entry, the first at word ");
        conf_buf_u64(&t.why, snap.bss_first_bad);
    }
#else
    {
        const volatile uint32_t *probe = conf_entry_bss_probe;
        uint32_t i;

        for (i = 0; i < PROBE_BYTES / 4u; i++) {
            if (probe[i] != 0u) {
                conf_check(&t, 0, "the 64 KiB static array is not all zero");
                break;
            }
        }
    }
#endif
    conf_buf_str(&t.note, "64 KiB all zero");
    conf_end(&t);
}

/* ---- C-ENTRY.19: argv strings ------------------------------------------------------------------ */

static void test_argv_strings(int argc, char **argv)
{
    struct conf_test t;
    int i;

    conf_begin(&t, "C-ENTRY.19");
    if (!conf_check(&t, argv != 0, "argv is NULL")) {
        conf_end(&t);
        return;
    }
    for (i = 0; i < argc; i++) {
        uint32_t n;

        if (!conf_check(&t, argv[i] != 0, "an argv entry below argc is NULL"))
            break;
        /* The longest argument a runtime must carry is 127 bytes; a string with
         * no NUL within 4 KiB is not a string. */
        n = str_len(argv[i], 4096u);
        conf_check(&t, n <= 4096u, "an argv string has no NUL within 4096 bytes");
    }
    conf_buf_u64(&t.note, (uint64_t)argc);
    conf_buf_str(&t.note, " strings read and NUL-terminated");
    conf_end(&t);
}

/* ---- C-ENTRY.20 to C-ENTRY.22: constructors and init arrays -------------------------------------- */

/* Sampled at the start of aar_main, before anything else could change them. */
struct init_seen {
    uint32_t early_at, early_runs, late_at, late_runs;
#ifdef ENTRY_ARM
    uint32_t pre_at, pre_api_set;
#endif
};

static void test_ctor_once(const struct init_seen *s)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.20");
    conf_check(&t, s->late_runs == 1u, "a constructor did not run exactly once before aar_main");
    conf_check(&t, s->early_runs == 1u,
               "a constructor with a priority did not run exactly once before aar_main");
    conf_end(&t);
}

static void test_preinit(const struct init_seen *s)
{
#ifdef ENTRY_ARM
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.21");
    conf_check(&t, s->pre_at != 0, "the .preinit_array entry did not run");
    conf_check(&t, s->pre_api_set == 1u, "aar_api_ptr was not set when .preinit_array ran");
    conf_check(&t, s->pre_at != 0 && s->pre_at < s->early_at && s->pre_at < s->late_at,
               ".preinit_array did not run before the .init_array entries");
    conf_end(&t);
#else
    (void)s;
    conf_skip("C-ENTRY.21", "host: the host runner calls aar_main after the program's own constructors");
#endif
}

static void test_ctor_priority(const struct init_seen *s)
{
    struct conf_test t;

    conf_begin(&t, "C-ENTRY.22");
    conf_check(&t, s->early_at != 0 && s->late_at != 0, "an init function did not run");
    conf_check(&t, s->early_at < s->late_at,
               "constructor(101) did not run before the constructor without a priority");
    conf_end(&t);
}

/* ---- main ---------------------------------------------------------------------------------------- */

int aar_main(int argc, char **argv)
{
    struct init_seen seen;
    int mode;

    seen.early_at = early_at;
    seen.early_runs = early_runs;
    seen.late_at = late_at;
    seen.late_runs = late_runs;
#ifdef ENTRY_ARM
    seen.pre_at = pre_at;
    seen.pre_api_set = pre_api_set;
#endif

    /* The runner's variant `long` hands over 16 arguments of 127 bytes; argv[1]
     * is then 127 bytes. Otherwise a runner passes conf:peer=1, and a run
     * without a runner (host, qemu loader) has neither. */
    if (argc >= 2 && argv[1] && str_len(argv[1], CONF_ENTRY_LONG_BYTES + 1u) == CONF_ENTRY_LONG_BYTES)
        mode = MODE_LONG;
    else if (conf_have_peer(argc, argv))
        mode = MODE_MAIN;
    else
        mode = MODE_HOST;

    if (mode == MODE_LONG) {
        test_long_args(argc, argv);
        return conf_finish("C-ENTRY");
    }

    test_table_pointer();
    test_argc_argv(argc, argv, mode);
    test_argv0(argc, argv, mode);
    if (mode == MODE_HOST)
        conf_skip("C-ENTRY.4", "host: the runner's 16 long arguments are not there");
#ifdef ENTRY_ARM
    test_sp();
    test_link();
    test_scratch();
    test_fpstate();
    test_vregs();
#endif
    test_fp_simd();
    test_endian();
    test_relocation();
    test_bss();
    test_argv_strings(argc, argv);
    test_ctor_once(&seen);
    test_preinit(&seen);
    test_ctor_priority(&seen);
    return conf_finish("C-ENTRY");
}
