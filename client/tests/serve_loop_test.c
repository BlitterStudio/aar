/*
 * Host test of the serving loop (client/serve/aar_serve.c and its backends) on
 * the fake system of serve_host.h. The loop calls aar.library through the
 * portable core (aar_core.c) on the reference platform (refplat.c); the test
 * plays the app side through the refplat_app_* functions: it posts requests,
 * takes completions and events, sets the app's state, and the fake machine
 * answers the loop's DOS packets, screens, AHI and timer calls in virtual time.
 *
 * Every check names the test it belongs to, so that the mutation run
 * (client/tests/mutants.py) can require that a deliberately broken loop makes
 * exactly that test fail.
 *
 * Build with -no-pie (or -m32): the fake memory, the arena and the words of a
 * DOS packet are below 4 GiB. -Wno-pointer-to-int-cast: the Amiga code passes
 * addresses through 32-bit words.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#ifdef __linux__
#include <execinfo.h>
#endif
#include <signal.h>
#include <unistd.h>
#define AAR_SERVE_HOST 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aar_core.h"
#include "../src/refplat.h"
#include "../serve/aar_serve.c"

struct WBArg {
    BPTR wa_Lock;
    STRPTR wa_Name;
};
#define NameFromLock(lock, name, size) X_NAME_FROM_LOCK(lock, name, size)
#include "../run/aar_run_workbench.h"
#undef NameFromLock

/* The tests call the display open of the app side by this name; the backend's
 * open_display is the loop's. */
#define open_display(c, format, flags) disp_open((c), (format), (flags))

static unsigned long checks, failures;
static const char *cur_test = "";

#define CHECK(cond)                                                                \
    do {                                                                           \
        checks++;                                                                  \
        if (!(cond)) {                                                             \
            failures++;                                                            \
            printf("FAIL [%s] %s:%d: %s\n", cur_test, __FILE__, __LINE__, #cond);  \
        }                                                                          \
    } while (0)

/* ---- The environment of the core ------------------------------------------------------- */

static aar_core core;
static struct aar_core_env env;
static struct { int pad; } task_obj;

static void *t_alloc(void *user, uint32_t bytes)
{
    (void)user;
    return hx_alloc(bytes, 1);
}

static void t_release(void *user, void *p, uint32_t bytes)
{
    (void)user;
    (void)bytes;
    hx_free(p);
}

static void t_self(void *user, aar_owner_stamp *s)
{
    (void)user;
    s->id = &task_obj;
    s->name = NULL;
    s->number = 0;
    s->kind = 1;
}

/* ---- The test's view of the world ----------------------------------------------------------- */

static AARHandle *H;
static int SLOT;
static aar_serve *S;
static aar_serve_config cfg;
static BPTR root_lock;           /* the lock the test made for cfg.root, or 0 */
static unsigned nev;             /* the events of the app's CQ seen so far, in evs[] below */

static void app_state(AARHandle *h, uint32_t state, uint32_t end)
{
    refplat_app_set_state(refplat_slot(h->claim.handle), state, end);
}

static AARHandle *open_claim(void)
{
    AARHandle *h = NULL;
    ULONG reason = 99;

    CHECK(aar_core_open(&core, 0x10000, NULL, NULL, &h, &reason) == AAR_OK);
    if (h)
        app_state(h, AAR_STATE_RUNNING, AAR_END_NONE);
    return h;
}

static void setup(const char *name)
{
    cur_test = name;
    printf("== %s\n", name);
    fflush(stdout);
    refplat_reset();
    hx_reset();
    memset(&env, 0, sizeof(env));
    env.size = sizeof(env);
    env.alloc = t_alloc;
    env.release = t_release;
    env.self = t_self;
    env.ticks_per_sec = 50;
    CHECK(aar_core_init(&core, aar_plat_get(), &env) == AAR_OK);
    CHECK(aar_core_attach(&core) == AAR_OK);
    hx_aar = &core;
    H = open_claim();
    SLOT = refplat_slot(H->claim.handle);
    S = NULL;
    root_lock = 0;
    nev = 0;
    aar_serve_config_default(&cfg);
    /* The process's current directory is the volume's root. */
    hx_current_dir_bptr = MKBADDR(hx_lock_new(&hx_vol_port, 1));
}

static void make_serve(void)
{
    app_state(H, AAR_STATE_RUNNING, AAR_END_NONE);
    CHECK(aar_serve_create(H, &cfg, &S) == AAR_OK);
    CHECK(S != NULL);
}

/* Every machine resource is back and nothing is left to happen. */
static void finish(void)
{
    int i, pending = 0;

    if (S) {
        aar_serve_destroy(S);
        S = NULL;
    }
    if (H) {
        CHECK(aar_core_close(&core, H) == AAR_OK);
        H = NULL;
    }
    aar_core_shutdown(&core);
    aar_core_detach(&core);
    if (root_lock)
        hx_unlock(root_lock);
    hx_unlock(hx_current_dir_bptr);
    hx_current_dir_bptr = 0;
    for (i = 0; i < HX_EVS; i++)
        if (hx_evs[i].used)
            pending++;
    CHECK(pending == 0);
    CHECK(refplat_live() == 0);
    CHECK(hx_live_locks == 0);
    CHECK(hx_live_handles == 0 && hx_live_fibs == 0 && hx_live_devprocs == 0);
    CHECK(hx_open_files_now == 0);
    CHECK(hx_screens == 0 && hx_buffers_live == 0 && hx_windows == 0);
    CHECK(hx_timer_open_count == 0 && hx_ahi_open_count == 0);
    CHECK(hx_pub_locks == 0);
    CHECK(hx_host_libs_open == 0 && hx_locale_open_now == 0 && hx_clip_handles == 0);
    CHECK(hx_live_blocks == 0);
    CHECK(hx_bitmaps_live == 0 && hx_pointers_live == 0);
    CHECK(refplat.surface_live == 0 && hx_arena_mem_live == 0);
    CHECK(hx_ll_open == 0 && hx_keymap_open == 0);
}

static volatile uint8_t *buf(uint32_t off)
{
    return refplat_ptr(refplat_buf_token(SLOT, off));
}

static void put_str(uint32_t off, const char *s)
{
    memcpy((void *)(uintptr_t)buf(off), s, strlen(s) + 1);
}

static void put_le(volatile uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A request from the app. */
static int post(uint32_t op, uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                uint32_t off, uint32_t len)
{
    uint32_t arg[6];

    memset(arg, 0, sizeof(arg));
    arg[0] = a0;
    arg[1] = a1;
    arg[2] = a2;
    arg[3] = a3;
    return refplat_app_request(SLOT, op, 0, id, arg, len ? refplat_buf_token(SLOT, 0) : 0, off, len);
}

struct comp {
    uint32_t op, id, flags;
    int32_t result;
    uint32_t out[6];
};

static aar_event evs[128];

/* The next completion off the CQ; events on the way are kept in evs[]. */
static int next_comp(struct comp *c)
{
    uint8_t e[AAR_ENTRY_BYTES];

    while (refplat_app_cq_pop(SLOT, e)) {
        unsigned i;

        if (aar_get_be16(e + AAR_ENTRY_OP) == AAR_OP_EVENT) {
            if (nev < 128)
                aar_entry_get_event(e, &evs[nev++]);
            continue;
        }
        c->op = aar_get_be16(e + AAR_ENTRY_OP);
        c->flags = aar_get_be16(e + AAR_ENTRY_FLAGS);
        c->id = aar_get_be32(e + AAR_ENTRY_ID);
        c->result = (int32_t)aar_get_be32(e + AAR_ENTRY_RESULT);
        for (i = 0; i < 6; i++)
            c->out[i] = aar_get_be32(e + AAR_ENTRY_OUT(i));
        return 1;
    }
    return 0;
}

static int event_seen(uint16_t type, uint16_t code)
{
    unsigned i;

    for (i = 0; i < nev; i++)
        if (evs[i].type == type && evs[i].code == code)
            return 1;
    return 0;
}

static unsigned events_of(uint16_t type)
{
    unsigned i, n = 0;

    for (i = 0; i < nev; i++)
        if (evs[i].type == type)
            n++;
    return n;
}

/* One pass of the loop of aar_serve_run. */
static LONG tick(void)
{
    return aar_serve_step(S, X_WAIT(aar_serve_sigmask(S)));
}

static LONG ticks(int n)
{
    LONG rc = AAR_SERVE_CONTINUE;

    while (n-- > 0 && rc == AAR_SERVE_CONTINUE)
        rc = tick();
    return rc;
}

/* Step until the app's request has its completion. */
static int wait_comp(struct comp *c, int max)
{
    while (max-- > 0) {
        if (next_comp(c))
            return 1;
        if (tick() != AAR_SERVE_CONTINUE)
            return next_comp(c);
    }
    return 0;
}

/* Request and completion in one. */
static int call(uint32_t op, uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                uint32_t off, uint32_t len, struct comp *c)
{
    if (post(op, id, a0, a1, a2, a3, off, len) != AAR_OK)
        return 0;
    return wait_comp(c, 400);
}

static void exit_cb(void *arg)
{
    app_state((AARHandle *)arg, AAR_STATE_IDLE, AAR_END_EXIT);
}

static void ctrl_c_cb(void *arg)
{
    (void)arg;
    hx_sig |= SIGBREAKF_CTRL_C;
}

static void revoke_cb(void *arg)
{
    (void)arg;
    refplat_revoke(SLOT);
}

/* The app ends by itself after us microseconds. */
static void app_exits_in(uint64_t us)
{
    hx_sched(us, exit_cb, H);
}

static int con_has(const char *text)
{
    hx_con_out[hx_con_out_len < sizeof(hx_con_out) ? hx_con_out_len : sizeof(hx_con_out) - 1] = 0;
    return strstr(hx_con_out, text) != NULL;
}

/* ---- Tests: the console ------------------------------------------------------------------------ */

static void test_hello(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("hello");
    make_serve();
    put_str(0x100, "hello\n");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 1, 0, 0, 0, 0, 0x100, 6, &c));
    CHECK(c.op == AAR_OP_CONSOLE_WRITE && c.id == 1 && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CONSOLE_COUNT] == 6);
    CHECK(hx_con_out_len == 6 && memcmp(hx_con_out, "hello\n", 6) == 0);
    /* The app exits with 0; the loop ends when the output is written. */
    app_exits_in(3000);
    CHECK(aar_serve_run(S) == AAR_SERVE_ENDED);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK);
    CHECK(res.state == AAR_STATE_IDLE && res.end_reason == AAR_END_EXIT && res.exit_code == 0);
    CHECK(res.flags == 0);
    finish();
}

static void test_console_long_write_is_chunked_in_order(void)
{
    struct comp c;
    unsigned i;

    setup("console_long_write");
    make_serve();
    for (i = 0; i < 3000; i++)
        buf(0x1000)[i] = (uint8_t)('a' + i % 26);
    CHECK(call(AAR_OP_CONSOLE_WRITE, 2, 0, 0, 0, 0, 0x1000, 3000, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 3000);
    CHECK(hx_con_out_len == 3000);
    for (i = 0; i < 3000; i++)
        if ((uint8_t)hx_con_out[i] != (uint8_t)('a' + i % 26))
            break;
    CHECK(i == 3000);
    CHECK(hx_packets_by[6] == 3); /* 1024 + 1024 + 952: one write packet in flight at a time */
    finish();
}

static void test_launcher_text_comes_before_app_text(void)
{
    struct comp c;

    setup("launcher_text_first");
    make_serve();
    aar_serve_status(S, "note");
    put_str(0x100, "abc");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 3, 0, 0, 0, 0, 0x100, 3, &c));
    CHECK(c.result == AAR_OK);
    CHECK(hx_con_out_len == 8 && memcmp(hx_con_out, "note\nabc", 8) == 0);
    finish();
}

static void test_console_read_gets_a_line(void)
{
    struct comp c;

    setup("console_read");
    make_serve();
    memcpy(hx_in_data, "hi\n", 3);
    hx_in_len = 3;
    CHECK(call(AAR_OP_CONSOLE_READ, 4, 0, 0, 0, 0, 0x200, 64, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 3);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x200), "hi\n", 3) == 0);
    /* End of input is a count of 0 and stays so. */
    CHECK(call(AAR_OP_CONSOLE_READ, 5, 0, 0, 0, 0, 0x200, 64, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 0);
    CHECK(call(AAR_OP_CONSOLE_READ, 6, 0, 0, 0, 0, 0x200, 64, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 0);
    finish();
}

/* The card revokes the claim and the loop's next call to the library finds out
 * (a library call that reaches the platform answers AAR_E_STALE). */
static void revoke_notice_cb(void *arg)
{
    aar_event ev;

    revoke_cb(arg);
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_KEY_UP;
    (void)aar_serve_post_event(S, &ev);
}

/* The claim is revoked while a console read is out: nothing is written into the
 * request's buffer afterwards (the buffer belongs to the card by then). */
static void test_revoke_with_a_console_read_pending_leaves_the_buffer_alone(void)
{
    struct comp c;
    unsigned i, changed = 0;

    setup("revoke_read_pending");
    make_serve();
    memcpy(hx_in_data, "late\n", 5);
    hx_in_len = 5;
    hx_con_delay_us = 8000;
    for (i = 0; i < 64; i++)
        buf(0x200)[i] = 0xEE;
    CHECK(post(AAR_OP_CONSOLE_READ, 7, 0, 0, 0, 0, 0x200, 64) == AAR_OK);
    hx_sched(3000, revoke_notice_cb, NULL);
    CHECK(ticks(40) == AAR_SERVE_LOST);
    hx_run_for(20000);
    aar_serve_destroy(S); /* the read's packet is reaped here: its bytes go nowhere */
    S = NULL;
    for (i = 0; i < 64; i++)
        if (buf(0x200)[i] != 0xEE) {
            if (!changed)
                printf("  first changed byte %u = %02x\n", i, buf(0x200)[i]);
            changed++;
        }
    CHECK(changed == 0);
    if (next_comp(&c))
        printf("  a completion: op %x id %u result %d\n", c.op, c.id, c.result);
    CHECK(refplat_app_cq_used(SLOT) == 0);
    finish();
}

static void test_output_failure_stops_the_app(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("output_failure");
    make_serve();
    hx_con_fail_after = 1;
    put_str(0x100, "hello");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 8, 0, 0, 0, 0, 0x100, 5, &c));
    CHECK(c.result == AAR_E_IO);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_CONSOLE_FAILED) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    ticks(3);
    (void)next_comp(&c);
    CHECK(event_seen(AAR_EV_STOP, 0));
    finish();
}

/* ---- Tests: the files ------------------------------------------------------------------------------ */

static void data_dir(void)
{
    int d = hx_node_add(1, "data", 1, NULL, 0);

    hx_node_add(d, "hello.txt", 0, "Hello, world!", 13);
    hx_node_add(d, "second", 0, "two", 3);
    hx_node_add(1, "secret", 0, "no", 2);
    root_lock = MKBADDR(hx_lock_new(&hx_vol_port, d));
    cfg.root = root_lock;
}

static void test_files_open_read_seek_stat_close(void)
{
    struct comp c;

    setup("files");
    data_dir();
    make_serve();
    put_str(0x100, "hello.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 3, 3, AAR_FILE_READ, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_READ, 4, 3, 5, 0, 0, 0x400, 5, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_FILE_COUNT] == 5);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), "Hello", 5) == 0);
    CHECK(call(AAR_OP_FILE_SEEK, 5, 3, 0, 7, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_FILE_POS_LO] == 7);
    CHECK(call(AAR_OP_FILE_READ, 6, 3, 100, 0, 0, 0x400, 100, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_FILE_COUNT] == 6);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), "world!", 6) == 0);
    CHECK(call(AAR_OP_FILE_STAT, 7, 0, 0, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_STAT_SIZE_LO] == 13);
    CHECK(call(AAR_OP_FILE_CLOSE, 8, 3, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK);
    CHECK(hx_open_files_now == 0);
    put_str(0x100, "missing");
    CHECK(call(AAR_OP_FILE_OPEN, 9, 3, AAR_FILE_READ, 0, 0, 0x100, 8, &c));
    CHECK(c.result == AAR_E_NOENT);
    finish();
}

static void test_files_write_creates_and_truncates(void)
{
    struct comp c;
    int n;

    setup("files_write");
    data_dir();
    make_serve();
    put_str(0x100, "out.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 3, 3, AAR_FILE_WRITE | AAR_FILE_CREATE | AAR_FILE_TRUNCATE, 0, 0, 0x100, 8, &c));
    CHECK(c.result == AAR_OK);
    put_str(0x400, "abcdef");
    CHECK(call(AAR_OP_FILE_WRITE, 4, 3, 6, 0, 0, 0x400, 6, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_FILE_COUNT] == 6);
    CHECK(call(AAR_OP_FILE_CLOSE, 5, 3, 0, 0, 0, 0, 0, &c));
    n = hx_node_find(2, "out.txt");
    CHECK(n != 0 && hx_nodes[n].size == 6 && memcmp(hx_nodes[n].data, "abcdef", 6) == 0);
    finish();
}

static void test_files_stay_under_the_root(void)
{
    struct comp c;

    setup("files_root");
    data_dir();
    make_serve();
    put_str(0x100, "/secret");
    CHECK(call(AAR_OP_FILE_OPEN, 3, 3, AAR_FILE_READ, 0, 0, 0x100, 8, &c));
    CHECK(c.result == AAR_E_ACCES);
    put_str(0x100, "RAM:data/hello.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 4, 3, AAR_FILE_READ, 0, 0, 0x100, 19, &c));
    CHECK(c.result == AAR_E_ACCES); /* an absolute path needs the ABS flag */
    CHECK(hx_open_files_now == 0);
    finish();
}

static void test_files_absolute_paths_with_abs_through_two_directories(void)
{
    struct comp c;
    int d, d2;

    setup("files_abs");
    d = hx_node_add(1, "one", 1, NULL, 0);
    d2 = hx_node_add(1, "two", 1, NULL, 0);
    hx_node_add(d, "a.txt", 0, "in one", 6);
    hx_node_add(d2, "b.txt", 0, "in two", 6);
    /* An assign of two directories: the name is tried in each. */
    hx_devs[1].name = "ASSIGN";
    hx_devs[1].port[0] = &hx_vol_port;
    hx_devs[1].node[0] = d;
    hx_devs[1].port[1] = &hx_vol_port;
    hx_devs[1].node[1] = d2;
    hx_devs[1].count = 2;
    hx_ndevs = 2;
    cfg.flags |= AAR_SERVEF_ABS_PATHS;
    make_serve();
    put_str(0x100, "ASSIGN:b.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 3, 3, AAR_FILE_READ, 0, 0, 0x100, 13, &c));
    CHECK(c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_READ, 4, 3, 6, 0, 0, 0x400, 6, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_FILE_COUNT] == 6);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), "in two", 6) == 0);
    CHECK(call(AAR_OP_FILE_CLOSE, 5, 3, 0, 0, 0, 0, 0, &c));
    finish();
}

static void test_files_list_a_directory(void)
{
    struct comp c;
    unsigned seen = 0;

    setup("files_list");
    data_dir();
    make_serve();
    put_str(0x100, ".");
    CHECK(call(AAR_OP_LIST_OPEN, 3, 4, 0, 0, 0, 0x100, 2, &c));
    CHECK(c.result == AAR_OK);
    for (;;) {
        CHECK(call(AAR_OP_LIST_NEXT, 4, 4, 0, 0, 0, 0x400, 256, &c));
        if (c.result != AAR_OK)
            break;
        seen++;
        if (seen > 4)
            break;
    }
    CHECK(c.result == AAR_E_NOENT && seen == 2);
    CHECK(call(AAR_OP_LIST_CLOSE, 5, 4, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK);
    finish();
}

/* A slow handler: the packet engine waits for its reply and Ctrl-C stops the app
 * without freeing the packet before the handler has answered. */
static void test_ctrl_c_during_a_dos_wait_stops_and_waits_for_the_reply(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("ctrl_c_in_dos_wait");
    data_dir();
    make_serve();
    hx_vol_delay_us = 30000;
    put_str(0x100, "hello.txt");
    CHECK(post(AAR_OP_FILE_OPEN, 3, 3, AAR_FILE_READ, 0, 0, 0x100, 10) == AAR_OK);
    hx_sched(5000, ctrl_c_cb, NULL);
    ticks(4); /* the open's lock packet is out; Ctrl-C arrives while it waits */
    CHECK(hx_now >= 30000); /* the engine waited for the handler's reply */
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_USER_STOP) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    CHECK(wait_comp(&c, 100));
    CHECK(c.result != AAR_OK);
    ticks(5);
    CHECK(hx_open_files_now == 0);
    finish();
}

/* 100 requests while the app takes no completions: none is lost, none repeats. */
static void test_a_full_completion_queue_loses_no_completion(void)
{
    struct comp c;
    unsigned sent = 0, got = 0, seen[128];
    int guard = 0;

    setup("full_cq");
    data_dir();
    make_serve();
    memset(seen, 0, sizeof(seen));
    put_str(0x100, "hello.txt");
    /* The app posts until the SQ is full and takes nothing back for a while. */
    while (sent < 100 && post(AAR_OP_FILE_STAT, 100 + sent, 0, 0, 0, 0, 0x100, 10) == AAR_OK)
        sent++;
    ticks(300);
    CHECK(refplat_app_cq_used(SLOT) == AAR_QUEUE_DEPTH_DEFAULT - 1 || refplat_app_cq_used(SLOT) == AAR_QUEUE_DEPTH_DEFAULT);
    while (got < 100 && guard++ < 4000) {
        while (next_comp(&c)) {
            CHECK(c.result == AAR_OK && c.id >= 100 && c.id < 200);
            if (c.id >= 100 && c.id < 200)
                seen[c.id - 100]++;
            got++;
        }
        while (sent < 100 && post(AAR_OP_FILE_STAT, 100 + sent, 0, 0, 0, 0, 0x100, 10) == AAR_OK)
            sent++;
        tick();
    }
    CHECK(got == 100);
    for (sent = 0; sent < 100; sent++)
        if (seen[sent] != 1)
            break;
    CHECK(sent == 100);
    CHECK(!con_has("completion queue full"));
    finish();
}

/* A request the app is waiting on (a console read) completes while the CQ is
 * full: its completion waits for the app to take the others. */
static void test_a_completion_of_a_held_request_waits_for_a_full_cq(void)
{
    struct comp c;
    unsigned sent = 0, got = 0, got_read = 0, guard;

    setup("full_cq_held");
    data_dir();
    make_serve();
    memcpy(hx_in_data, "late\n", 5);
    hx_in_len = 5;
    hx_con_delay_us = 1000000;
    put_str(0x100, "hello.txt");
    CHECK(post(AAR_OP_CONSOLE_READ, 99, 0, 0, 0, 0, 0x200, 64) == AAR_OK);
    for (guard = 0; guard < 200; guard++) {
        while (sent < 120 && post(AAR_OP_FILE_STAT, 100 + sent, 0, 0, 0, 0, 0x100, 10) == AAR_OK)
            sent++;
        ticks(2);
    }
    CHECK(refplat_app_cq_used(SLOT) >= AAR_QUEUE_DEPTH_DEFAULT - 1);
    hx_run_for(1500000);
    ticks(10); /* the read is done; its completion has no room */
    for (guard = 0; guard < 4000 && (got < sent || !got_read); guard++) {
        while (next_comp(&c)) {
            if (c.id == 99) {
                CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 5);
                got_read++;
            } else {
                CHECK(c.result == AAR_OK && c.id >= 100);
                got++;
            }
        }
        tick();
    }
    CHECK(got == sent && got_read == 1);
    CHECK(!con_has("completion queue full"));
    finish();
}

/* ---- Tests: the display -------------------------------------------------------------------------------- */

#define DW 64u
#define DH 32u

static int disp_open(struct comp *c, uint32_t format, uint32_t flags)
{
    /* the fake drawing area has the pixel size of the surface the display will draw */
    hx_canvas_bpp = format == AAR_FMT_BGRA8888 ? 4u : (format == AAR_FMT_CLUT8 ? 1u : 2u);
    return call(AAR_OP_DISPLAY_OPEN, 20, DW, DH, format, flags, 0, 0, c);
}

static void test_display_with_screen_buffers(void)
{
    struct comp c;
    uint32_t front, back, f2, b2;

    setup("display_buffers");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC));
    CHECK(c.result == AAR_OK);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_ID] == AAR_SERVE_DISPLAY_ID);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_PITCH] == DW * 2);
    CHECK((c.flags & AAR_DISPCAP_ASYNC) && (c.flags & AAR_DISPCAP_CLUT8));
    front = c.out[AAR_R_DISPLAY_OPEN_FRONT];
    back = c.out[AAR_R_DISPLAY_OPEN_BACK];
    CHECK(front != 0 && back != 0 && front != back && c.out[AAR_R_DISPLAY_OPEN_SPARE] == 0);
    CHECK(hx_screens == 1 && hx_buffers_live == 2 && hx_windows == 1);
    /* A second open while one is open is busy. */
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_E_BUSY);
    /* Each flip is completed by the SafeMessage of its change. */
    CHECK(call(AAR_OP_DISPLAY_FLIP, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && hx_changes == 1 && hx_safe_msgs == 1);
    f2 = c.out[AAR_R_DISPLAY_FLIP_FRONT];
    b2 = c.out[AAR_R_DISPLAY_FLIP_BACK];
    CHECK(f2 == back && b2 == front);
    CHECK(call(AAR_OP_DISPLAY_FLIP, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_FLIP_FRONT] == front);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK);
    CHECK(hx_screens == 0 && hx_windows == 0 && hx_buffers_live == 0);
    CHECK(hx_pub_front == 1); /* the public screen is back in front */
    finish();
}

static void test_display_falls_back_to_a_panned_tall_screen(void)
{
    struct comp c;
    uint32_t pitch;

    setup("display_pan3");
    hx_p96_fail_buffers = 1;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK);
    pitch = c.out[AAR_R_DISPLAY_OPEN_PITCH];
    CHECK(c.out[AAR_R_DISPLAY_OPEN_BACK] == c.out[AAR_R_DISPLAY_OPEN_FRONT] + pitch * DH);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_SPARE] == c.out[AAR_R_DISPLAY_OPEN_BACK] + pitch * DH);
    CHECK(hx_cur_screen && hx_cur_screen->Height == (WORD)(DH * 3));
    CHECK(call(AAR_OP_DISPLAY_FLIP, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && (c.flags & AAR_CFLAG_FLIP_SUBMITTED) && hx_pans == 1);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

/* A screen that opens shorter than three pages cannot be panned: it is closed
 * and the two page screen is tried. */
static void test_a_tall_screen_that_opens_too_short_is_not_used(void)
{
    struct comp c;

    setup("display_pan3_short");
    hx_p96_fail_buffers = 1;
    hx_p96_clamp_height = DH * 2;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_OPEN_SPARE] == 0 && c.out[AAR_R_DISPLAY_OPEN_BACK] != 0);
    CHECK(hx_screens == 1 && hx_cur_screen->Height == (WORD)(DH * 2));
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

static void test_display_falls_back_to_two_pages_then_one_buffer(void)
{
    struct comp c;

    setup("display_pan2_single");
    hx_p96_fail_buffers = 1;
    hx_p96_max_height = DH * 2; /* the three page screen does not open */
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_OPEN_SPARE] == 0 && c.out[AAR_R_DISPLAY_OPEN_BACK] != 0);
    CHECK(hx_cur_screen->Height == (WORD)(DH * 2));
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    hx_p96_max_height = DH; /* neither does the two page one */
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_OPEN_BACK] == 0 && c.out[AAR_R_DISPLAY_OPEN_SPARE] == 0);
    CHECK(hx_cur_screen->Height == (WORD)DH);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

static void test_display_strategy_starts_the_walk_later(void)
{
    struct comp c;

    setup("display_strategy");
    cfg.display_strategy = AAR_SERVE_DISPLAY_PAN2;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK && hx_cur_screen->Height == (WORD)(DH * 2) && hx_buffers_live == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    aar_serve_destroy(S);
    S = NULL;
    cfg.display_strategy = AAR_SERVE_DISPLAY_SINGLE;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK && hx_cur_screen->Height == (WORD)DH && c.out[AAR_R_DISPLAY_OPEN_BACK] == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

static void test_display_palette_reaches_loadrgb32(void)
{
    struct comp c;
    uint8_t *p;

    setup("display_palette");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_CLUT8, 0));
    CHECK(c.result == AAR_OK);
    p = (uint8_t *)(uintptr_t)buf(0x100);
    p[0] = 0;
    p[1] = 0x10;
    p[2] = 0x20;
    p[3] = 0x30;
    p[4] = 0;
    p[5] = 0xff;
    p[6] = 0x80;
    p[7] = 0x00;
    CHECK(call(AAR_OP_DISPLAY_PALETTE, 24, AAR_SERVE_DISPLAY_ID, 5, 2, 0, 0x100, 8, &c));
    CHECK(c.result == AAR_OK && hx_palettes == 1);
    CHECK(hx_last_palette[0] == ((2u << 16) | 5u));
    CHECK(hx_last_palette[1] == 0x10101010u && hx_last_palette[2] == 0x20202020u && hx_last_palette[3] == 0x30303030u);
    CHECK(hx_last_palette[7] == 0);
    /* A palette beyond the 256 colours is refused. */
    CHECK(call(AAR_OP_DISPLAY_PALETTE, 25, AAR_SERVE_DISPLAY_ID, 255, 2, 0, 0x100, 8, &c));
    CHECK(c.result == AAR_E_INVAL);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 26, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

static void test_display_that_cannot_open_stops_the_app(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("display_open_fails");
    hx_p96_no_mode = 1;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_E_IO);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_DISPLAY_FAILED) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    CHECK(hx_screens == 0 && hx_windows == 0);
    finish();
}

static void test_the_raw_key_ctrl_c_in_the_window_stops_and_the_key_goes_to_the_app(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("raw_key_ctrl_c");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    hx_window_post(IDCMP_RAWKEY, 0x33, IEQUALIFIER_CONTROL, 0, 0);
    hx_window_post(IDCMP_RAWKEY, 0x33 | IECODE_UP_PREFIX, 0, 0, 0);
    hx_window_post(IDCMP_RAWKEY, 0x22, IEQUALIFIER_CONTROL, 0, 0); /* Ctrl-D */
    ticks(3);
    nev = 0;
    (void)next_comp(&c);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_USER_STOP) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    CHECK(event_seen(AAR_EV_KEY_DOWN, 0x33) && event_seen(AAR_EV_KEY_UP, 0x33));
    CHECK(event_seen(AAR_EV_BREAK, AAR_BREAK_CTRL_D));
    CHECK(event_seen(AAR_EV_STOP, 0));
    CHECK(refplat.trace_len > 0);
    finish();
}

static void test_mouse_buttons_and_close_gadget(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("mouse_and_close");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    hx_window_post(IDCMP_MOUSEBUTTONS, SELECTDOWN, 0, 0, 0);
    hx_window_post(IDCMP_MOUSEBUTTONS, MENUDOWN, 0, 0, 0);
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, 5, -3);
    ticks(3);
    (void)next_comp(&c);
    CHECK(events_of(AAR_EV_MOUSE) >= 2);
    {
        unsigned i, both = 0;

        for (i = 0; i < nev; i++)
            if (evs[i].type == AAR_EV_MOUSE && evs[i].buttons == 3)
                both++;
        CHECK(both >= 1); /* the left and the right button are down */
    }
    CHECK(refplat_app_cq_used(SLOT) == 0);
    hx_window_post(IDCMP_CLOSEWINDOW, 0, 0, 0, 0);
    ticks(3);
    (void)next_comp(&c);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_STOP_ASKED) && !(res.flags & AAR_SERVE_R_USER_STOP));
    finish();
}

static void test_joystick_port_one_by_default_and_port_zero_by_config(void)
{
    struct comp c;

    setup("joystick");
    hx_joydat[1] = 0x0002; /* right on port 1 */
    hx_joydat[0] = 0x0200; /* left on port 0 */
    hx_ciaa_pra = 0x7F;    /* fire 1 */
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    ticks(3);
    (void)next_comp(&c);
    CHECK(events_of(AAR_EV_JOY) == 1);
    CHECK(nev > 0 && evs[nev - 1].type == AAR_EV_JOY && evs[nev - 1].code == 1 && evs[nev - 1].dx == 1 && evs[nev - 1].buttons == 1);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 26, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    aar_serve_destroy(S);
    S = NULL;
    (void)next_comp(&c);
    nev = 0;
    cfg.joy_ports = 3;
    make_serve();
    hx_joydat[1] = 0;
    hx_ciaa_pra = 0xFF;
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    hx_joydat[1] = 0;
    hx_ciaa_pra = 0xFF;
    hx_joydat[0] = 0x0200;
    ticks(3);
    (void)next_comp(&c);
    CHECK(event_seen(AAR_EV_JOY, 0));
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 27, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

/* ---- Tests: the audio --------------------------------------------------------------------------------------- */

static void make_ring(uint32_t off, uint32_t frames, uint32_t written)
{
    volatile uint8_t *r = buf(off);
    uint32_t f;

    memset((void *)(uintptr_t)r, 0, 0x80 + frames * 4);
    put_le(r + AAR_AUDIO_RING_RATE, 8000);
    put_le(r + AAR_AUDIO_RING_CHANNELS, 2);
    put_le(r + AAR_AUDIO_RING_FORMAT, AAR_AUDIO_FORMAT_S16LE);
    for (f = 0; f < written; f++) {
        r[0x80 + f * 4] = (uint8_t)f;
        r[0x80 + f * 4 + 1] = (uint8_t)(f >> 8);
        r[0x80 + f * 4 + 2] = (uint8_t)(f + 1);
        r[0x80 + f * 4 + 3] = 0;
    }
    put_le(r + AAR_AUDIO_RING_WRITE_FRAME, written);
}

static int open_audio(struct comp *c, uint32_t off, uint32_t frames)
{
    uint32_t arg[6] = { 8000, 2, AAR_AUDIO_ROUTE_AHI, 0, 0, 0 };

    /* The ring is the request's buffer: its token, offset 0. */
    if (refplat_app_request(SLOT, AAR_OP_AUDIO_OPEN, 0, 30, arg, refplat_buf_token(SLOT, off), 0, 0x80 + frames * 4) != AAR_OK)
        return 0;
    return wait_comp(c, 400);
}

static void test_ahi_audio_open_feed_underrun_close(void)
{
    struct comp c;
    volatile uint8_t *ring;
    uint32_t i;

    setup("ahi_audio");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    make_serve();
    make_ring(0x4000, 64, 40);
    ring = buf(0x4000);
    CHECK(open_audio(&c, 0x4000, 64));
    CHECK(c.result == AAR_OK && c.out[AAR_R_AUDIO_OPEN_FRAMES] == 64 && c.out[AAR_R_AUDIO_OPEN_RATE] == 8000);
    CHECK(hx_ahi_open_count == 1);
    ticks(40); /* the app wrote 40 frames and stopped: they are played as one request */
    /* 40 frames of S16 stereo reach AHI in one buffer, in order. */
    CHECK(hx_ahi_writes == 1 && hx_ahi_bytes == 160);
    for (i = 0; i < 40; i++)
        if (hx_ahi_last[i * 4] != (uint8_t)i || hx_ahi_last[i * 4 + 2] != (uint8_t)(i + 1))
            break;
    CHECK(i == 40);
    CHECK(get_le(ring + AAR_AUDIO_RING_READ_FRAME) == 40);
    hx_run_for(30000);
    ticks(3);
    /* the end of a sound is neither a gap nor an underrun */
    CHECK(get_le(ring + AAR_AUDIO_RING_UNDERRUNS) == 0);
    {
        struct aar_serve_result res;

        memset(&res, 0, sizeof(res));
        res.size = sizeof(res);
        CHECK(aar_serve_result(S, &res) == AAR_OK && res.ahi_underruns == 0 && res.ahi_gaps == 0);
        CHECK(res.ahi_requests == 1 && res.ahi_min_frames == 40 && res.ahi_avg_frames == 40);
    }
    CHECK(call(AAR_OP_AUDIO_CLOSE, 31, 0, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && hx_ahi_open_count == 0);
    {
        struct aar_serve_result res;

        memset(&res, 0, sizeof(res));
        res.size = sizeof(res);
        CHECK(aar_serve_result(S, &res) == AAR_OK && res.ahi_requests == 1);
        app_state(H, AAR_STATE_IDLE, AAR_END_EXIT);
        CHECK(aar_serve_result(S, &res) == AAR_OK && res.ahi_requests == 1 && res.ahi_min_frames == 40);
    }
    finish();
}

static void test_audio_chunk_is_a_fiftieth_of_a_second(void)
{
    struct comp c;

    setup("audio_chunk");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    make_serve();
    make_ring(0x4000, 1024, 1000);
    CHECK(open_audio(&c, 0x4000, 1024));
    CHECK(c.result == AAR_OK);
    ticks(10);
    CHECK(hx_ahi_writes >= 1 && hx_ahi_bytes / hx_ahi_writes <= 8000 / 50 * 3 * 4); /* 3/50 s at most */
    CHECK(hx_ahi_max_pending <= (int)AAR_SERVE_AHI_BUFFER_COUNT);
    CHECK(call(AAR_OP_AUDIO_CLOSE, 31, 0, 0, 0, 0, 0, 0, &c));
    hx_run_for(300000);
    ticks(3);
    hx_run_for(300000);
    ticks(3);
    finish();
}

static void test_ahi_that_cannot_open_stops_the_app(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("ahi_open_fails");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    hx_ahi_fail_open = 1;
    make_serve();
    make_ring(0x4000, 64, 0);
    CHECK(open_audio(&c, 0x4000, 64));
    CHECK(c.result == AAR_E_IO);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_AUDIO_FAILED) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    CHECK(hx_ahi_open_count == 0);
    finish();
}

static void test_a_group_the_config_leaves_out_answers_nosys(void)
{
    struct comp c;

    setup("groups");
    cfg.groups = AAR_GROUP_AUDIO;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_E_NOSYS && hx_screens == 0);
    put_str(0x100, "x");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 40, 0, 0, 0, 0, 0x100, 1, &c));
    CHECK(c.result == AAR_OK);
    finish();
}

/* ---- Tests: the loop ---------------------------------------------------------------------------------------- */

static void test_ctrl_c_stops_and_the_loop_ends_after_the_grace(void)
{
    struct aar_serve_result res;
    uint64_t t0;

    setup("ctrl_c_stop");
    make_serve();
    hx_sched(5000, ctrl_c_cb, NULL);
    hx_sched(40000, exit_cb, H);
    CHECK(aar_serve_run(S) == AAR_SERVE_ENDED);
    t0 = hx_now;
    CHECK(t0 >= 5000 + AAR_SERVE_GRACE_MS * 1000u); /* the stop is honoured, the grace is waited out */
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK);
    CHECK((res.flags & AAR_SERVE_R_USER_STOP) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    {
        struct comp c;

        (void)next_comp(&c);
    }
    CHECK(event_seen(AAR_EV_STOP, 0));
    CHECK(refplat.trace_len > 0);
    finish();
}

static void test_ctrl_d_e_f_become_break_events(void)
{
    struct comp c;

    setup("ctrl_def");
    make_serve();
    hx_sig |= SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_F;
    ticks(2);
    (void)next_comp(&c);
    CHECK(event_seen(AAR_EV_BREAK, AAR_BREAK_CTRL_D) && event_seen(AAR_EV_BREAK, AAR_BREAK_CTRL_F));
    CHECK(!event_seen(AAR_EV_BREAK, AAR_BREAK_CTRL_E));
    /* The program that owns its signals does not lose them. */
    aar_serve_destroy(S);
    S = NULL;
    cfg.break_mask = 0;
    (void)next_comp(&c); /* the stop event of the first loop's destroy */
    nev = 0;
    make_serve();
    hx_sig |= SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_C;
    ticks(2);
    (void)next_comp(&c);
    CHECK(nev == 0);
    CHECK((hx_sig & (SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_C)) == (SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_C));
    hx_sig &= ~(SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_C);
    finish();
}

static void test_a_revoked_claim_ends_the_loop_as_lost(void)
{
    struct aar_serve_result res;

    setup("revoked");
    make_serve();
    hx_sched(4000, revoke_notice_cb, NULL);
    CHECK(aar_serve_run(S) == AAR_SERVE_LOST);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_E_STALE);
    CHECK(res.flags & AAR_SERVE_R_CLAIM_LOST);
    finish();
}

static void test_a_corrupt_request_queue_stops_the_app(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("queue_corrupt");
    make_serve();
    /* The app side's tail runs past the ring: the queue is corrupt. */
    aar_put_be32(refplat_cb(SLOT) + AAR_CB_SQ_TAIL, 1000);
    ticks(3);
    (void)next_comp(&c);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_QUEUE_CORRUPT) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    CHECK(con_has("the request queue is corrupt"));
    finish();
}

static void test_logs_of_the_app_are_queued_as_lines(void)
{
    struct comp c;

    setup("log_sink");
    make_serve();
    put_str(0x300, "app log line");
    CHECK(refplat_app_log(SLOT, refplat_buf_token(SLOT, 0), 0x300, 12) == AAR_OK);
    ticks(6);
    (void)next_comp(&c);
    CHECK(con_has("app log line\n"));
    /* A record without a newline gets one; a record with one does not get two. */
    put_str(0x300, "two\n");
    CHECK(refplat_app_log(SLOT, refplat_buf_token(SLOT, 0), 0x300, 4) == AAR_OK);
    ticks(6);
    CHECK(con_has("two\n") && !con_has("two\n\n"));
    finish();
}

static void test_two_instances_in_one_task(void)
{
    AARHandle *h2;
    aar_serve *s2 = NULL;
    struct comp c;
    int slot2;

    setup("two_instances");
    make_serve();
    h2 = open_claim();
    slot2 = refplat_slot(h2->claim.handle);
    CHECK(slot2 != SLOT);
    CHECK(aar_serve_create(h2, &cfg, &s2) == AAR_OK);
    put_str(0x100, "A");
    CHECK(post(AAR_OP_CONSOLE_WRITE, 1, 0, 0, 0, 0, 0x100, 1) == AAR_OK);
    {
        uint32_t arg[6] = { 0 };
        uint32_t i;

        memcpy((void *)(uintptr_t)refplat_ptr(refplat_buf_token(slot2, 0x100)), "B", 2);
        CHECK(refplat_app_request(slot2, AAR_OP_CONSOLE_WRITE, 0, 2, arg, refplat_buf_token(slot2, 0), 0x100, 1) == AAR_OK);
        for (i = 0; i < 12; i++) {
            uint32_t got = X_WAIT(aar_serve_sigmask(S) | aar_serve_sigmask(s2));

            aar_serve_step(S, got);
            aar_serve_step(s2, got);
        }
    }
    CHECK(next_comp(&c) && c.result == AAR_OK && c.id == 1);
    {
        uint8_t e[AAR_ENTRY_BYTES];

        CHECK(refplat_app_cq_pop(slot2, e) == 1 && aar_get_be32(e + AAR_ENTRY_ID) == 2 && (int32_t)aar_get_be32(e + AAR_ENTRY_RESULT) == AAR_OK);
    }
    CHECK(hx_con_out_len == 2 && strchr(hx_con_out, 'A') && strchr(hx_con_out, 'B'));
    aar_serve_destroy(s2);
    CHECK(aar_core_close(&core, h2) == AAR_OK);
    finish();
}

/* ---- The hooks and the configuration -------------------------------------------------------------------------------- */

static unsigned hook_status_calls, hook_stop_calls;
static ULONG hook_stop_why;
static char hook_status_text[128];

static void hook_status(ULONG user, const char *text)
{
    (void)user;
    hook_status_calls++;
    snprintf(hook_status_text, sizeof(hook_status_text), "%s", text);
}

static void hook_stop_wanted(ULONG user, ULONG why)
{
    (void)user;
    hook_stop_calls++;
    hook_stop_why = why;
}

static unsigned hook_log_calls;
static ULONG hook_log_len;

static void hook_log(ULONG user, const void *data, ULONG len)
{
    (void)user;
    (void)data;
    hook_log_calls++;
    hook_log_len = len;
}

static void test_hooks_replace_the_defaults(void)
{
    aar_serve_hooks hooks;
    struct comp c;
    struct aar_serve_result res;

    setup("hooks");
    memset(&hooks, 0, sizeof(hooks));
    hooks.size = sizeof(hooks);
    hooks.status = hook_status;
    hooks.stop_wanted = hook_stop_wanted;
    hooks.log = hook_log;
    cfg.hooks = &hooks;
    cfg.user = 7;
    hook_status_calls = hook_stop_calls = 0;
    hook_log_calls = 0;
    make_serve();
    aar_serve_status(S, "from the program");
    CHECK(hook_status_calls == 1 && strcmp(hook_status_text, "from the program") == 0);
    CHECK(hx_con_out_len == 0);
    /* Ctrl-C goes to the hook, which decides not to stop. */
    hx_sig |= SIGBREAKF_CTRL_C;
    ticks(2);
    (void)next_comp(&c);
    CHECK(hook_stop_calls == 1 && hook_stop_why == AAR_SERVE_WHY_BREAK);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK(!(res.flags & AAR_SERVE_R_STOP_ASKED));
    put_str(0x300, "abcd");
    CHECK(refplat_app_log(SLOT, refplat_buf_token(SLOT, 0), 0x300, 4) == AAR_OK);
    ticks(3);
    (void)next_comp(&c);
    CHECK(hook_log_calls == 1 && hook_log_len == 4 && hx_con_out_len == 0);
    /* The program stops it itself. */
    aar_serve_stop(S);
    aar_serve_stop(S);
    (void)aar_serve_result(S, &res);
    CHECK(res.flags & AAR_SERVE_R_STOP_ASKED);
    finish();
}

/* A hook table is size-first: an older caller's table has no later hooks, and
 * whatever follows it in memory is not read as one. */
static void test_a_hook_table_from_an_older_header_has_only_the_hooks_it_has(void)
{
    aar_serve_hooks hooks;
    struct comp c;
    struct aar_serve_result res;

    setup("hooks_size");
    memset(&hooks, 0, sizeof(hooks));
    hooks.size = (ULONG)offsetof(aar_serve_hooks, log);
    hooks.status = hook_status;
    hooks.log = hook_log;
    hooks.stop_wanted = hook_stop_wanted;
    cfg.hooks = &hooks;
    hook_status_calls = hook_stop_calls = hook_log_calls = 0;
    make_serve();
    aar_serve_status(S, "x");
    CHECK(hook_status_calls == 1);
    hx_sig |= SIGBREAKF_CTRL_C;
    ticks(2);
    (void)next_comp(&c);
    CHECK(hook_stop_calls == 0);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK(res.flags & AAR_SERVE_R_STOP_ASKED);
    put_str(0x300, "abcd");
    CHECK(refplat_app_log(SLOT, refplat_buf_token(SLOT, 0), 0x300, 4) == AAR_OK);
    ticks(3);
    (void)next_comp(&c);
    CHECK(hook_log_calls == 0);
    finish();
}

static void test_the_configuration_is_checked(void)
{
    aar_serve_config bad;
    aar_serve *s = (aar_serve *)(uintptr_t)1;

    setup("config");
    bad = cfg;
    bad.size = 4;
    CHECK(aar_serve_create(H, &bad, &s) == AAR_E_INVAL && s == NULL);
    bad = cfg;
    bad.flags = 0x80;
    CHECK(aar_serve_create(H, &bad, &s) == AAR_E_INVAL);
    bad = cfg;
    bad.audio_route = 9;
    CHECK(aar_serve_create(H, &bad, &s) == AAR_E_INVAL);
    bad = cfg;
    bad.joy_ports = 4;
    CHECK(aar_serve_create(H, &bad, &s) == AAR_E_INVAL);
    bad = cfg;
    bad.display_strategy = 9;
    CHECK(aar_serve_create(H, &bad, &s) == AAR_E_INVAL);
    bad = cfg;
    bad.platform = 1;
    CHECK(aar_serve_create(H, &bad, &s) == AAR_E_INVAL);
    CHECK(aar_serve_create(NULL, &cfg, &s) == AAR_E_INVAL);
    CHECK(aar_serve_create(H, NULL, &s) == AAR_E_INVAL);
    CHECK(aar_serve_create(H, &cfg, NULL) == AAR_E_INVAL);
    /* No timer.device: nothing is left behind. */
    hx_timer_fail_open = 1;
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_NOSYS && s == NULL);
    hx_timer_fail_open = 0;
    /* A claim that is not live. */
    CHECK(aar_core_close(&core, H) == AAR_OK);
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_STALE);
    H = NULL;
    finish();
}

static void test_the_workbench_console_opens_at_the_first_use(void)
{
    struct comp c;

    setup("wb_console");
    cfg.console_spec = (CONST_STRPTR)"CON:0/0/640/160/aar-run/CLOSE/WAIT";
    make_serve();
    CHECK(hx_live_handles == 0);
    put_str(0x100, "x");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 50, 0, 0, 0, 0, 0x100, 1, &c));
    CHECK(c.result == AAR_OK && hx_con_out_len == 1);
    CHECK(hx_live_handles == 1);
    finish(); /* destroy closes the console it opened */
}

/* console_in, console_out and console_spec each have one default that does not
 * depend on the others: an output stream alone leaves reads on Input(). */
static void test_an_output_stream_alone_does_not_take_the_input(void)
{
    struct comp c;

    setup("con_defaults_out");
    cfg.console_out = MKBADDR(&hx_nil_fh_store); /* console_in 0: Input() */
    memcpy(hx_in_data, "abc\n", 4);
    hx_in_len = 4;
    make_serve();
    CHECK(call(AAR_OP_CONSOLE_READ, 1, 0, 0, 0, 0, 0x200, 64, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 4);
    finish();
}

/* ... an input stream alone leaves writes on Output() ... */
static void test_an_input_stream_alone_does_not_take_the_output(void)
{
    struct comp c;

    setup("con_defaults_in");
    cfg.console_in = MKBADDR(&hx_nil_fh_store); /* console_out 0: Output() */
    make_serve();
    put_str(0x100, "x");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 1, 0, 0, 0, 0, 0x100, 1, &c));
    CHECK(c.result == AAR_OK && hx_con_out_len == 1);
    finish();
}

/* ... and the window aar_serve opens from console_spec is bidirectional: a
 * program that gave no console_in reads from it, not from Input(). */
static void test_the_window_it_opened_is_read_as_well_as_written(void)
{
    struct comp c;

    setup("con_defaults_spec");
    cfg.console_spec = (CONST_STRPTR)"CON:0/0/640/160/aar-run/CLOSE/WAIT";
    hx_input_bptr = MKBADDR(&hx_nil_fh_store); /* Input() would give end of file */
    memcpy(hx_in_data, "abc\n", 4);
    hx_in_len = 4;
    make_serve();
    CHECK(call(AAR_OP_CONSOLE_READ, 1, 0, 0, 0, 0, 0x200, 64, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 4);
    finish();
}

/* The text of a requester (the screen title, a path) is an argument of a fixed
 * "%s" format, never the format: a '%' in it is shown as it is. */
static void test_a_requester_text_is_never_a_format(void)
{
    struct comp c;

    setup("requester_format");
    cfg.screen_title = (CONST_STRPTR)"50%s%n done";
    cfg.console_spec = (CONST_STRPTR)"NOPE:unopenable"; /* the window cannot be opened */
    make_serve();
    put_str(0x100, "x");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 50, 0, 0, 0, 0, 0x100, 1, &c));
    CHECK(c.result == AAR_E_IO);
    CHECK(hx_requesters >= 1);
    CHECK(strcmp(hx_requester_format, "%s") == 0);
    CHECK(strstr(hx_requester_text, "50%s%n done") != NULL);
    finish();
}

/* Once any stop is requested (aar_serve_stop, a stop from the input window, a
 * break, the platform ending the app) a queued request that changes the file
 * system no longer runs: no create, no truncate, no write. */
static void test_a_stop_cancels_the_requests_that_change_files(void)
{
    struct comp c;
    int n;

    setup("stop_files");
    data_dir();
    make_serve();
    put_str(0x100, "out.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 3, 3, AAR_FILE_WRITE | AAR_FILE_CREATE, 0, 0, 0x100, 8, &c));
    CHECK(c.result == AAR_OK);
    put_str(0x400, "abc");
    CHECK(call(AAR_OP_FILE_WRITE, 4, 3, 3, 0, 0, 0x400, 3, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_FILE_COUNT] == 3);
    aar_serve_stop(S);
    put_str(0x400, "defgh");
    CHECK(call(AAR_OP_FILE_WRITE, 5, 3, 5, 0, 0, 0x400, 5, &c));
    CHECK(c.result == AAR_E_IO && c.out[AAR_R_FILE_COUNT] == 0);
    put_str(0x100, "hello.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 6, 4, AAR_FILE_WRITE | AAR_FILE_TRUNCATE, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_E_IO);
    put_str(0x100, "late.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 7, 5, AAR_FILE_WRITE | AAR_FILE_CREATE, 0, 0, 0x100, 9, &c));
    CHECK(c.result == AAR_E_IO);
    CHECK(call(AAR_OP_FILE_CLOSE, 8, 3, 0, 0, 0, 0, 0, &c)); /* a close still runs */
    CHECK(c.result == AAR_OK);
    n = hx_node_find(2, "out.txt");
    CHECK(n != 0 && hx_nodes[n].size == 3);
    n = hx_node_find(2, "hello.txt");
    CHECK(n != 0 && hx_nodes[n].size == 13 && memcmp(hx_nodes[n].data, "Hello, world!", 13) == 0);
    CHECK(hx_node_find(2, "late.txt") == 0);
    finish();
}

/* ... also when only the platform ended the app (no stop was asked here). */
static void test_the_platform_ending_the_app_cancels_the_requests_that_change_files(void)
{
    struct comp c;
    int n;

    setup("stop_platform_end");
    data_dir();
    make_serve();
    put_str(0x100, "out.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 3, 3, AAR_FILE_WRITE | AAR_FILE_CREATE, 0, 0, 0x100, 8, &c));
    CHECK(c.result == AAR_OK);
    app_state(H, AAR_STATE_STOPPING, 0);
    put_str(0x400, "abc");
    CHECK(call(AAR_OP_FILE_WRITE, 4, 3, 3, 0, 0, 0x400, 3, &c));
    CHECK(c.result == AAR_E_IO);
    put_str(0x100, "hello.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 6, 4, AAR_FILE_WRITE | AAR_FILE_TRUNCATE, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_E_IO);
    n = hx_node_find(2, "out.txt");
    CHECK(n != 0 && hx_nodes[n].size == 0);
    n = hx_node_find(2, "hello.txt");
    CHECK(n != 0 && hx_nodes[n].size == 13);
    CHECK(call(AAR_OP_FILE_CLOSE, 8, 3, 0, 0, 0, 0, 0, &c)); /* a close still runs */
    CHECK(c.result == AAR_OK);
    finish();
}

/* ---- Tests: the fixes of the move ------------------------------------------------------------------------------- */

static int sync_has(const char *text)
{
    hx_sync_out[hx_sync_len < sizeof(hx_sync_out) ? hx_sync_len : sizeof(hx_sync_out) - 1] = 0;
    return strstr(hx_sync_out, text) != NULL;
}

/*
 * Output to >NIL: (a handle with no handler port) used to fail the whole output
 * path: the first console byte stopped the app and the run returned 20, and the
 * stop cancelled the app's parked file calls (swquake's failed seeks). NIL: takes
 * everything through a plain Write(), which cannot block; nothing fails.
 */
static void test_nil_output_is_written_synchronously_and_the_app_is_not_stopped(void)
{
    struct comp c;
    struct aar_serve_result res;
    unsigned i;

    setup("nil_output");
    data_dir();
    hx_output_bptr = MKBADDR(&hx_nil_fh_store);
    hx_input_bptr = MKBADDR(&hx_nil_fh_store);
    make_serve();
    put_str(0x100, "hello");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 1, 0, 0, 0, 0, 0x100, 5, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 5);
    /* A file seek in between, as the app does while it prints. */
    put_str(0x100, "hello.txt");
    CHECK(call(AAR_OP_FILE_OPEN, 2, 3, AAR_FILE_READ, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_SEEK, 3, 3, 0, 4, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_FILE_POS_LO] == 4);
    for (i = 0; i < 3000; i++)
        buf(0x1000)[i] = 'x';
    CHECK(call(AAR_OP_CONSOLE_WRITE, 4, 0, 0, 0, 0, 0x1000, 3000, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 3000);
    CHECK(call(AAR_OP_FILE_CLOSE, 5, 3, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK);
    /* The input is NIL: too: end of input. */
    CHECK(call(AAR_OP_CONSOLE_READ, 6, 0, 0, 0, 0, 0x200, 64, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 0);
    CHECK(hx_sync_bytes == 3005);   /* all of it through Write() */
    CHECK(hx_packets_by[6] == 0);   /* no console write packet */
    app_exits_in(3000);
    CHECK(aar_serve_run(S) == AAR_SERVE_ENDED);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK);
    CHECK(res.flags == 0 && res.exit_code == 0);
    finish();
}

/* A destination that really fails still stops the app, and the note about it
 * goes to the user's console, not into the destination that failed. */
static void test_the_output_error_note_goes_to_the_user_console(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("output_error_note");
    hx_output_bptr = MKBADDR(&hx_pipe_fh_store);
    hx_pipe_ok_bytes = 3;
    make_serve();
    put_str(0x100, "hello");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 1, 0, 0, 0, 0, 0x100, 5, &c));
    CHECK(c.result == AAR_E_IO);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_CONSOLE_FAILED) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    CHECK(sync_has("cannot write app console output"));
    hx_pipe_out[hx_pipe_len < sizeof(hx_pipe_out) ? hx_pipe_len : sizeof(hx_pipe_out) - 1] = 0;
    CHECK(strstr(hx_pipe_out, "cannot write") == NULL);
    finish();
}

/*
 * The library turns an AUDIO_OPEN completion whose ring does not check out into
 * a refusal. AHI was opened for it: it is closed again and the loop's audio
 * state is cleared, so a later open starts clean.
 */
static void test_a_refused_audio_ring_closes_ahi(void)
{
    struct comp c;

    setup("refused_ring");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    make_serve();
    make_ring(0x4000, 64, 0);
    put_le(buf(0x4000) + AAR_AUDIO_RING_FORMAT, 7); /* not S16LE */
    CHECK(open_audio(&c, 0x4000, 64));
    CHECK(c.result == AAR_E_IO);
    CHECK(hx_ahi_open_count == 0);
    ticks(5);
    CHECK(hx_ahi_writes == 0);
    /* A good ring opens afterwards. */
    make_ring(0x8000, 64, 40);
    CHECK(open_audio(&c, 0x8000, 64));
    CHECK(c.result == AAR_OK && hx_ahi_open_count == 1);
    ticks(40);
    CHECK(hx_ahi_writes == 1);
    CHECK(call(AAR_OP_AUDIO_CLOSE, 31, 0, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK);
    hx_run_for(300000);
    ticks(3);
    CHECK(hx_ahi_open_count == 0);
    finish();
}

/* The platform validates the framebuffer spans; the client no longer has a
 * window of its own for the tokens (a platform whose tokens lie elsewhere). */
static void test_a_bitmap_token_outside_the_old_window_is_accepted(void)
{
    struct comp c;
    uint32_t front;

    setup("token_window");
    hx_token_add = 0x08000000UL; /* beyond 0x06010000 */
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK);
    front = c.out[AAR_R_DISPLAY_OPEN_FRONT];
    CHECK(front >= 0x08000000UL);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

/* ---- Tests: the stop grace -------------------------------------------------------------------------------------- */

/* The time an app gets to honour a stop is AARInfo.stop_grace_ms, not a number
 * of the launcher: the loop waits that long (and no longer) after a stop. */
static void stop_grace_run(uint32_t grace_ms, uint64_t *ended_us)
{
    struct comp c;

    setup("stop_grace");
    refplat.info.stop_grace_ms = grace_ms;
    make_serve();
    hx_sched(5000, ctrl_c_cb, NULL);
    hx_sched(20000, exit_cb, H);
    CHECK(aar_serve_run(S) == AAR_SERVE_ENDED);
    *ended_us = hx_now;
    (void)next_comp(&c);
    finish();
}

static void test_the_stop_grace_is_the_platforms(void)
{
    uint64_t t;

    stop_grace_run(2000, &t);
    CHECK(t >= 5000 + 2000 * 1000ULL && t < 5000 + 2000 * 1000ULL + 50000);
    stop_grace_run(100, &t);
    CHECK(t >= 5000 + 100 * 1000ULL && t < 5000 + 100 * 1000ULL + 50000);
    /* A platform that reports none: the default of the loop. */
    stop_grace_run(0, &t);
    CHECK(t >= 5000 + AAR_SERVE_GRACE_MS * 1000ULL && t < 5000 + AAR_SERVE_GRACE_MS * 1000ULL + 50000);
}

/* ---- Tests: the app's messages ------------------------------------------------------------------------------------ */

/* The app sends n messages (the first byte counts them); the test posts them as
 * the SQ has room, with the loop running. */
static unsigned send_messages(unsigned n)
{
    unsigned sent = 0;
    int guard = 0;
    uint8_t raw[AAR_MSG_BYTES];

    while (sent < n && guard++ < 2000) {
        while (sent < n) {
            memset(raw, 0, sizeof(raw));
            raw[0] = (uint8_t)sent;
            raw[1] = (uint8_t)(sent >> 8);
            if (refplat_app_message(SLOT, raw) != AAR_OK)
                break;
            sent++;
        }
        tick();
    }
    return sent;
}

/*
 * The library holds 64 unread messages. With nobody taking them, a message at the
 * head of the SQ that the full inbox cannot take blocks every request behind it:
 * an app that sends more than 64 messages hung. The loop takes the messages and
 * drops them (a launcher has no use for them), and counts them.
 */
static void test_app_messages_are_drained_and_counted_by_default(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("messages_drained");
    make_serve();
    CHECK(send_messages(150) == 150);
    put_str(0x100, "ok");
    CHECK(call(AAR_OP_CONSOLE_WRITE, 1, 0, 0, 0, 0, 0x100, 2, &c));
    CHECK(c.result == AAR_OK);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK);
    CHECK(res.dropped_messages == 150);
    finish();
}

static unsigned hook_messages, hook_first_bytes[8];
static void hook_message(ULONG user, const AARMessage *msg)
{
    (void)user;
    if (hook_messages < 8)
        hook_first_bytes[hook_messages] = msg->bytes[0];
    hook_messages++;
}

static void test_the_message_hook_gets_the_messages(void)
{
    aar_serve_hooks hooks;
    struct aar_serve_result res;

    setup("message_hook");
    memset(&hooks, 0, sizeof(hooks));
    hooks.size = sizeof(hooks);
    hooks.message = hook_message;
    cfg.hooks = &hooks;
    hook_messages = 0;
    make_serve();
    CHECK(send_messages(10) == 10);
    ticks(3);
    CHECK(hook_messages == 10);
    CHECK(hook_first_bytes[0] == 0 && hook_first_bytes[1] == 1 && hook_first_bytes[7] == 7);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK);
    CHECK(res.dropped_messages == 0); /* the hook took them */
    finish();
}

/* A program that calls AARGetMessage itself keeps its messages. */
static void test_keep_messages_leaves_them_for_the_program(void)
{
    AARMessage m;
    struct aar_serve_result res;
    unsigned got = 0;

    setup("keep_messages");
    cfg.flags |= AAR_SERVEF_KEEP_MESSAGES;
    make_serve();
    CHECK(send_messages(10) == 10);
    ticks(3);
    while (aar_core_get_message(&core, H, &m) == AAR_OK) {
        CHECK(m.bytes[0] == got);
        got++;
    }
    CHECK(got == 10);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK);
    CHECK(res.dropped_messages == 0);
    finish();
}

/* ---- Tests: destroy, the signal mask and the heartbeat ----------------------------------------------------------- */

/*
 * Every request the loop took is answered exactly once: with its result, or with
 * AAR_E_IO at destroy. Here a console read that never started (the app is not
 * running) and a flip whose SafeMessage had not come are held at destroy.
 */
static void test_destroy_answers_every_held_request_once(void)
{
    struct comp c;
    unsigned n_read = 0, n_flip = 0, n_other = 0;
    int32_t r_read = 0, r_flip = 0;

    setup("destroy_held");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK);
    app_state(H, AAR_STATE_STARTING, AAR_END_NONE); /* a read is not started while the app does not run */
    ticks(2);                                       /* the loop has seen it */
    CHECK(post(AAR_OP_CONSOLE_READ, 70, 0, 0, 0, 0, 0x200, 64) == AAR_OK);
    CHECK(post(AAR_OP_DISPLAY_FLIP, 71, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0) == AAR_OK);
    ticks(1);
    CHECK(hx_changes == 1 && hx_safe_msgs == 0); /* the flip is out, its SafeMessage is not back */
    CHECK(!next_comp(&c));
    aar_serve_destroy(S);
    S = NULL;
    while (next_comp(&c)) {
        if (c.id == 70) {
            n_read++;
            r_read = c.result;
        } else if (c.id == 71) {
            n_flip++;
            r_flip = c.result;
        } else {
            n_other++;
        }
    }
    CHECK(n_read == 1 && r_read == AAR_E_IO);
    CHECK(n_flip == 1 && r_flip == AAR_E_IO);
    CHECK(n_other == 0);
    CHECK(aar_get_be32(refplat_cb(SLOT) + AAR_CB_STOP_REQUEST) != 0); /* a running app is stopped */
    finish();
}

/* The screen buffer that flipped last still owns its SafeMessage when the loop
 * is destroyed: it is waited for before the buffer is freed. */
static void test_destroy_waits_for_the_flip_that_is_out(void)
{
    struct comp c;

    setup("destroy_flip_out");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(call(AAR_OP_DISPLAY_FLIP, 71, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK);
    CHECK(post(AAR_OP_DISPLAY_FLIP, 72, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0) == AAR_OK);
    ticks(1);
    CHECK(hx_changes == 2 && hx_safe_msgs == 1); /* the second flip's SafeMessage is still to come */
    aar_serve_destroy(S);
    S = NULL;
    while (next_comp(&c))
        ;
    CHECK(hx_buffers_live == 0 && hx_screens == 0);
    finish();
}

static void test_the_signal_mask_includes_the_flip_and_ahi_ports(void)
{
    struct comp c;
    ULONG mask;

    setup("sigmask");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK && S->gfx.safe_port != NULL);
    mask = aar_serve_sigmask(S);
    CHECK(mask & (1UL << S->gfx.safe_port->mp_SigBit));
    CHECK(mask & (1UL << S->tm.port->mp_SigBit));
    CHECK(mask & (1UL << S->gfx.input_win->UserPort->mp_SigBit));
    CHECK(mask & AAR_SERVE_BREAK_ALL);
    make_ring(0x4000, 64, 0);
    CHECK(open_audio(&c, 0x4000, 64));
    CHECK(c.result == AAR_OK && S->ahi.port != NULL);
    CHECK(aar_serve_sigmask(S) & (1UL << S->ahi.port->mp_SigBit));
    CHECK(call(AAR_OP_AUDIO_CLOSE, 31, 0, 0, 0, 0, 0, 0, &c));
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 32, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

/* A program with no heartbeat (poll_us 0) waits only on the loop's signals: the
 * SafeMessage of a flip has to be one of them, or the flip is never completed. */
static void test_a_flip_completes_with_no_heartbeat(void)
{
    struct comp c;
    int i;

    setup("flip_no_heartbeat");
    cfg.poll_us = 0;
    make_serve();
    CHECK(post(AAR_OP_DISPLAY_OPEN, 20, DW, DH, AAR_FMT_RGB565, 0, 0, 0) == AAR_OK);
    hx_sched(2000, ctrl_c_cb, NULL); /* something to wake the first Wait */
    CHECK(ticks(1) == AAR_SERVE_CONTINUE);
    CHECK(next_comp(&c) && c.result == AAR_OK);
    CHECK(post(AAR_OP_DISPLAY_FLIP, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0) == AAR_OK);
    hx_sched(1000, ctrl_c_cb, NULL);
    for (i = 0; i < 20 && !next_comp(&c); i++)
        tick();
    CHECK(i < 20 && c.id == 21 && c.result == AAR_OK);
    hx_sig &= ~SIGBREAKF_CTRL_C;
    finish();
}

/* The feeder takes at most a fiftieth of a second per pass: it needs a heartbeat
 * of that period or faster. */
static void test_ahi_needs_a_heartbeat_of_at_most_a_fiftieth_of_a_second(void)
{
    aar_serve *s = NULL;

    setup("ahi_heartbeat");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    cfg.poll_us = 20001;
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_NOSYS && s == NULL);
    cfg.poll_us = 0;
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_NOSYS && s == NULL);
    cfg.poll_us = 20000;
    make_serve();
    finish();
}

/* ---- Tests: the rest of the guards ---------------------------------------------------------------------------------- */

static void test_no_input_window_when_the_program_has_its_own(void)
{
    struct comp c;

    setup("no_input_window");
    cfg.flags |= AAR_SERVEF_NO_INPUT_WINDOW;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK && hx_screens == 1 && hx_windows == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && hx_screens == 0);
    finish();
}

/* Two screen buffers need one pitch: a second buffer with another pitch is not
 * used, the screen is panned instead. */
static void test_buffers_with_another_pitch_fall_back_to_panning(void)
{
    struct comp c;

    setup("buffer_pitch");
    hx_p96_buffer_pitch_differs = 1;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_OK && hx_buffers_live == 0);
    CHECK(hx_cur_screen && hx_cur_screen->Height == (WORD)(DH * 3));
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

/* The app's last flip showed the back buffer: closing puts the front one back,
 * then the public screen. */
static void test_close_shows_the_front_buffer_again(void)
{
    struct comp c;

    setup("close_restores");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(call(AAR_OP_DISPLAY_FLIP, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && hx_changes == 1);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && hx_changes == 2 && hx_safe_msgs == 2);
    finish();
}

/* The sink is removed at destroy: the handle outlives the loop, and its user
 * pointer is the loop's memory. */
static void test_the_log_sink_is_gone_after_destroy(void)
{
    struct comp c;

    setup("log_sink_removed");
    make_serve();
    put_str(0x300, "before");
    CHECK(refplat_app_log(SLOT, refplat_buf_token(SLOT, 0), 0x300, 6) == AAR_OK);
    ticks(3);
    (void)next_comp(&c);
    CHECK(H->stats.logs == 1 && H->stats.logs_unclaimed == 0);
    aar_serve_destroy(S);
    S = NULL;
    put_str(0x300, "after");
    CHECK(refplat_app_log(SLOT, refplat_buf_token(SLOT, 0), 0x300, 5) == AAR_OK);
    {
        AARServiceReq r;

        memset(&r, 0, sizeof(r));
        r.size = sizeof(r);
        (void)aar_core_service_next(&core, H, &r);
    }
    CHECK(H->stats.logs == 1 && H->stats.logs_unclaimed == 1);
    finish();
}

/* The card hang: aar-run locates the ELF (a DOS wait that reaps the heartbeat
 * timer) between create and the first Wait. Nothing re-armed the timer until
 * some other signal arrived, so a program that only waits on aar_serve_sigmask
 * never served the app. */
static void test_the_heartbeat_runs_after_a_call_made_outside_the_loop(void)
{
    struct comp c;
    char full[128];

    setup("heartbeat_after_locate");
    data_dir();
    make_serve();
    CHECK(aar_serve_locate(S, root_lock, "hello.txt", full, sizeof(full)) == AAR_OK);
    put_str(0x100, "hello.txt");
    CHECK(call(AAR_OP_FILE_STAT, 3, 0, 0, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_OK);
    /* and the same with no DOS call in between: the heartbeat keeps running */
    CHECK(call(AAR_OP_FILE_STAT, 4, 0, 0, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_OK);
    /* destroy closes an open file with a DOS wait of its own: that wait must not
     * leave the timer armed */
    CHECK(call(AAR_OP_FILE_OPEN, 5, 3, AAR_FILE_READ, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_OK);
    aar_serve_destroy(S);
    S = NULL;
    (void)next_comp(&c);
    finish();
}

/* The runtime drops a log record when the SQ is full and counts it in the
 * control block; the launcher's own console queue counts separately. The
 * result reports both, and keeps the runtime's count once the claim is gone. */
static void test_the_result_counts_the_logs_the_runtime_dropped(void)
{
    struct aar_serve_result res;

    setup("log_dropped");
    make_serve();
    aar_put_be32(refplat_cb(SLOT) + AAR_CB_LOG_DROPPED, 10000);
    app_state(H, AAR_STATE_IDLE, AAR_END_EXIT);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK);
    CHECK(res.runtime_dropped_logs == 10000 && res.dropped_logs == 0);
    CHECK(res.status.log_dropped == 10000);
    revoke_notice_cb(NULL);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_E_STALE);
    CHECK(res.runtime_dropped_logs == 10000);
    finish();
}

/* NameFromLock succeeds with DOSTRUE (-1). Exercise the same project-argument
 * path builder the Workbench branch calls, then locate it through the loop. */
static void test_workbench_project_uses_dostrue_from_name_from_lock(void)
{
    struct WBArg project;
    char path[256], full[256], directory[256];

    setup("workbench_project");
    data_dir();
    hx_node_add(((struct FileLock *)BADDR(root_lock))->fl_Key,
                "hello.elf", 0, "elf", 3);
    make_serve();
    project.wa_Lock = root_lock;
    project.wa_Name = (STRPTR)"hello.elf";
    CHECK(hx_name_from_lock(project.wa_Lock, directory, sizeof(directory)) == -1);
    CHECK(aar_run_project_path(&project, path, sizeof(path)));
    CHECK(strcmp(path, "RAM:data/hello.elf") == 0);
    if (path[0])
        CHECK(aar_serve_locate(S, 0, path, full, sizeof(full)) == AAR_OK);
    CHECK(!aar_run_project_path(&project, path, 1) && path[0] == 0);
    /* a drawer (an empty name) is the directory itself, without a slash */
    project.wa_Name = (STRPTR)"";
    CHECK(aar_run_project_path(&project, path, sizeof(path)));
    CHECK(strcmp(path, "RAM:data") == 0);
    finish();
}

/* The app writes slower than AHI plays (2 frames per ms against 8): when the two
 * requests have played AHI runs dry with the reserve short of a request. That is
 * counted (ran dry, and a ring underrun, the ring being empty), the stream is
 * buffered again, and what the app wrote is played as a last, short request once
 * it stops writing. */
static void test_ahi_that_runs_dry_is_counted_and_buffered_again(void)
{
    struct comp c;
    struct aar_serve_result res;
    unsigned i;

    setup("ahi_gaps");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    make_serve();
    make_ring(0x4000, 1024, 960);
    CHECK(open_audio(&c, 0x4000, 1024));
    ticks(10); /* prebuffered: two requests of 60 ms are playing */
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK && res.ahi_gaps == 0 && res.ahi_underruns == 0 && res.ahi_requests == 2);
    for (i = 1; i <= 140; i++) {
        put_le(buf(0x4000) + AAR_AUDIO_RING_WRITE_FRAME, 960 + 2 * i);
        tick();
    }
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK && res.ahi_gaps == 1 && res.ahi_underruns == 1);
    CHECK(res.ahi_requests == 2 && res.ahi_min_frames == 480);
    ticks(60); /* the app stopped: the 280 frames it wrote are played, short, once */
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK && res.ahi_requests == 3 && res.ahi_min_frames < 480);
    CHECK(res.ahi_gaps == 1 && res.ahi_underruns == 1);
    CHECK(call(AAR_OP_AUDIO_CLOSE, 31, 0, 0, 0, 0, 0, 0, &c));
    hx_run_for(300000);
    ticks(3);
    hx_run_for(300000);
    ticks(3);
    finish();
}

/* A request that needs several DOS packets: the timer is armed once per packet
 * by the wait itself and aborted once; the end-of-wait arm is only for calls
 * made outside the loop (aar_serve_locate). */
static void test_a_multi_packet_read_costs_one_timer_abort_per_packet(void)
{
    struct comp c;
    static char big[24576];
    unsigned long before;

    setup("timer_ops");
    data_dir();
    memset(big, 'x', sizeof(big));
    hx_node_add(((struct FileLock *)BADDR(root_lock))->fl_Key, "big", 0, big, sizeof(big));
    make_serve();
    put_str(0x100, "big");
    CHECK(call(AAR_OP_FILE_OPEN, 3, 3, AAR_FILE_READ, 0, 0, 0x100, 4, &c));
    CHECK(c.result == AAR_OK);
    before = hx_timer_aborts;
    CHECK(call(AAR_OP_FILE_READ, 4, 3, sizeof(big), 0, 0, 0x2000, sizeof(big), &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_FILE_COUNT] == sizeof(big));
    printf("  timer aborts for 3 packets: %lu\n", hx_timer_aborts - before);
    CHECK(hx_timer_aborts - before <= 3);
    finish();
}

/* The loop can be late (a DOS wait, a low priority task on a busy machine): AHI
 * keeps playing out of its two 60 ms requests, so a 60 ms stall leaves AHI busy
 * and the counters at 0. */
static void test_a_sixty_ms_loop_stall_does_not_starve_ahi(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("ahi_stall");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    make_serve();
    make_ring(0x4000, 1024, 1024);
    CHECK(open_audio(&c, 0x4000, 1024));
    CHECK(c.result == AAR_OK);
    ticks(10); /* prebuffered: both requests are in AHI */
    CHECK(hx_ahi_max_pending == (int)AAR_SERVE_AHI_BUFFER_COUNT);
    hx_run_for(60000);
    ticks(1);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK && res.ahi_gaps == 0 && res.ahi_underruns == 0);
    CHECK(call(AAR_OP_AUDIO_CLOSE, 31, 0, 0, 0, 0, 0, 0, &c));
    hx_run_for(300000);
    ticks(3);
    hx_run_for(300000);
    ticks(3);
    finish();
}

/* The app writes its ring in real time, 8 frames (1 ms at 8000 Hz) at a time, just
 * ahead of the player. AHI is started after the prebuffer and then gets whole
 * requests only: a driver that leaves a gap at every short request boundary (the
 * fake counts them) plays this without one. */
static void test_a_real_time_ring_gets_whole_requests_and_no_gap(void)
{
    struct comp c;
    struct aar_serve_result res;
    unsigned i;

    setup("ahi_slow_ring");
    cfg.audio_route = AAR_AUDIO_ROUTE_AHI;
    make_serve();
    make_ring(0x4000, 4096, 0);
    CHECK(open_audio(&c, 0x4000, 4096));
    CHECK(c.result == AAR_OK);
    for (i = 1; i <= 100; i++) {
        put_le(buf(0x4000) + AAR_AUDIO_RING_WRITE_FRAME, 8 * i);
        tick();
    }
    CHECK(hx_ahi_writes == 0); /* prebuffering: 120 ms are not there yet */
    for (i = 101; i <= 600; i++) {
        put_le(buf(0x4000) + AAR_AUDIO_RING_WRITE_FRAME, 8 * i);
        tick();
    }
    CHECK(hx_ahi_writes >= 6);
    CHECK(hx_ahi_short == 0); /* every request is a whole one: 60 ms */
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    CHECK(aar_serve_result(S, &res) == AAR_OK && res.ahi_gaps == 0 && res.ahi_underruns == 0);
    CHECK(res.ahi_requests == hx_ahi_writes && res.ahi_min_frames == 480 && res.ahi_avg_frames == 480);
    CHECK(call(AAR_OP_AUDIO_CLOSE, 31, 0, 0, 0, 0, 0, 0, &c));
    CHECK(hx_ahi_open_count == 1); /* the queued audio plays out before AHI is closed */
    hx_run_for(300000);
    ticks(3);
    hx_run_for(300000);
    ticks(3);
    CHECK(hx_ahi_open_count == 0);
    finish();
}

/* ---- Tests: the embedder's contract ---------------------------------------------------------------------------------- */

/* A replacement display (hooks.display): open succeeds, the loop records the
 * screen from its completion, and flips, palettes and the close work. */
static struct {
    unsigned opens, changes, pans, tofs, palettes, closes;
    int last_to_back, fail_change, no_front;
    uint32_t pal0;
} rd;

static void rd_open(void *ctx, const struct aar_serve_req *req, struct aar_serve_done *done)
{
    (void)ctx;
    (void)req;
    rd.opens++;
    done->result = AAR_OK;
    done->out[AAR_R_DISPLAY_OPEN_PITCH] = 128;
    done->out[AAR_R_DISPLAY_OPEN_FRONT] = rd.no_front ? 0u : 0x1000u;
    done->out[AAR_R_DISPLAY_OPEN_BACK] = 0x2000;
}

static int rd_change(void *ctx, int to_back)
{
    (void)ctx;
    rd.changes++;
    rd.last_to_back = to_back;
    return !rd.fail_change;
}

static void rd_pan(void *ctx, uint32_t page)
{
    (void)ctx;
    (void)page;
    rd.pans++;
}

static void rd_tof(void *ctx)
{
    (void)ctx;
    rd.tofs++;
}

static void rd_palette(void *ctx, const uint32_t *table)
{
    (void)ctx;
    rd.palettes++;
    rd.pal0 = table[0];
}

static void rd_close(void *ctx)
{
    (void)ctx;
    rd.closes++;
}

static const struct aar_serve_display_ops rd_full = {
    NULL, rd_open, rd_change, rd_pan, rd_tof, rd_palette, rd_close,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
static const struct aar_serve_display_ops rd_only_open = { .open = rd_open };
static aar_serve_hooks table_hooks;

static void use_hooks(const struct aar_serve_display_ops *display, const struct aar_serve_console_ops *console)
{
    memset(&table_hooks, 0, sizeof(table_hooks));
    table_hooks.size = sizeof(table_hooks);
    table_hooks.display = display;
    table_hooks.console = console;
    cfg.hooks = &table_hooks;
}

static void test_a_replacement_display_is_recorded_and_completable(void)
{
    struct comp c;

    setup("repl_display");
    memset(&rd, 0, sizeof(rd));
    use_hooks(&rd_full, NULL);
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC));
    CHECK(c.result == AAR_OK && rd.opens == 1);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_ID] == AAR_SERVE_DISPLAY_ID);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_PITCH] == 128 && c.out[AAR_R_DISPLAY_OPEN_FRONT] == 0x1000 &&
          c.out[AAR_R_DISPLAY_OPEN_BACK] == 0x2000);
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_E_BUSY && rd.opens == 1);
    /* a flip is completed by the program's aar_serve_flip_shown */
    CHECK(post(AAR_OP_DISPLAY_FLIP, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0) == AAR_OK);
    ticks(2);
    CHECK(rd.changes == 1 && rd.last_to_back == 1);
    CHECK(!next_comp(&c));
    aar_serve_flip_shown(S);
    CHECK(wait_comp(&c, 20));
    CHECK(c.id == 21 && c.result == AAR_OK && c.out[AAR_R_DISPLAY_FLIP_FRONT] == 0x2000 &&
          c.out[AAR_R_DISPLAY_FLIP_BACK] == 0x1000);
    /* a change that fails fails the flip */
    rd.fail_change = 1;
    CHECK(call(AAR_OP_DISPLAY_FLIP, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_E_IO);
    rd.fail_change = 0;
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && rd.closes == 1);
    /* a palette reaches the table */
    CHECK(open_display(&c, AAR_FMT_CLUT8, 0));
    CHECK(c.result == AAR_OK && rd.opens == 2);
    CHECK(call(AAR_OP_DISPLAY_PALETTE, 24, AAR_SERVE_DISPLAY_ID, 5, 2, 0, 0x100, 8, &c));
    CHECK(c.result == AAR_OK && rd.palettes == 1 && rd.pal0 == ((2u << 16) | 5u));
    /* destroy closes a screen that is still open, through the table */
    aar_serve_destroy(S);
    S = NULL;
    CHECK(rd.closes == 2);
    while (next_comp(&c))
        ;
    finish();
}

static void test_a_minimal_replacement_display_still_flips_and_closes(void)
{
    struct comp c;

    setup("repl_display_min");
    memset(&rd, 0, sizeof(rd));
    use_hooks(&rd_only_open, NULL);
    make_serve();
    CHECK(open_display(&c, AAR_FMT_CLUT8, AAR_DISPLAY_VSYNC));
    CHECK(c.result == AAR_OK);
    /* one buffer: the flip completes at once, the table has no wait_tof or pan */
    CHECK(call(AAR_OP_DISPLAY_FLIP, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_FLIP_FRONT] == 0x1000 && rd.changes == 0 && rd.pans == 0);
    CHECK(call(AAR_OP_DISPLAY_PALETTE, 22, AAR_SERVE_DISPLAY_ID, 0, 1, 0, 0x100, 4, &c));
    CHECK(c.result == AAR_E_NOSYS); /* no load_rgb32 */
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && rd.closes == 0);
    /* an open that gives no front token has opened nothing */
    rd.no_front = 1;
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.result == AAR_E_IO);
    rd.no_front = 0;
    CHECK(open_display(&c, AAR_FMT_RGB565, 0)); /* and it is not recorded as open */
    CHECK(c.result == AAR_OK);
    finish();
}

/* A replacement console (hooks.console): the program completes what it started
 * through aar_serve_write_done and aar_serve_read_done. */
static struct {
    unsigned write_starts, read_starts, errors;
    uint8_t data[256];
    uint32_t bytes, read_max;
    int live;
} rc;

static int rc_write_start(void *ctx, const uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    rc.write_starts++;
    rc.bytes = bytes;
    memset(rc.data, 0, sizeof(rc.data));
    memcpy(rc.data, data, bytes < sizeof(rc.data) ? bytes : sizeof(rc.data));
    return 1;
}

static int rc_read_start(void *ctx, uint32_t max)
{
    (void)ctx;
    rc.read_starts++;
    rc.read_max = max;
    return 1;
}

static int rc_live(void *ctx)
{
    (void)ctx;
    return rc.live;
}

static void rc_error(void *ctx)
{
    (void)ctx;
    rc.errors++;
}

/* order: ctx, open, write_start, read_start, read_allowed, live, output_error;
 * open and read_allowed are left out: they are optional */
static const struct aar_serve_console_ops rc_full = {
    NULL, NULL, rc_write_start, rc_read_start, NULL, rc_live, rc_error
};

static void test_a_replacement_console_is_completed_through_the_public_calls(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("repl_console");
    memset(&rc, 0, sizeof(rc));
    rc.live = 1;
    use_hooks(NULL, &rc_full);
    make_serve();
    aar_serve_write_done(S, 3); /* nothing was started: ignored */
    ticks(2);
    CHECK(rc.write_starts == 0 && !next_comp(&c));
    /* the launcher's own line goes out first, through write_start */
    aar_serve_status(S, "note");
    ticks(2);
    CHECK(rc.write_starts == 1 && strstr((const char *)rc.data, "note") != NULL);
    aar_serve_write_done(S, (LONG)rc.bytes);
    /* the app's write follows and completes with the program's completion */
    put_str(0x100, "hello");
    CHECK(post(AAR_OP_CONSOLE_WRITE, 8, 0, 0, 0, 0, 0x100, 5) == AAR_OK);
    ticks(2);
    CHECK(rc.write_starts == 2 && rc.bytes == 5 && memcmp(rc.data, "hello", 5) == 0);
    CHECK(!next_comp(&c));
    aar_serve_write_done(S, 5);
    CHECK(wait_comp(&c, 20));
    CHECK(c.id == 8 && c.result == AAR_OK);
    /* a short write fails the output: the app is stopped, once */
    put_str(0x100, "world!");
    CHECK(post(AAR_OP_CONSOLE_WRITE, 9, 0, 0, 0, 0, 0x100, 6) == AAR_OK);
    ticks(2);
    CHECK(rc.write_starts == 3);
    aar_serve_write_done(S, 2);
    CHECK(wait_comp(&c, 20));
    CHECK(c.id == 9 && c.result == AAR_E_IO && rc.errors == 1);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_CONSOLE_FAILED) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    CHECK(call(AAR_OP_CONSOLE_WRITE, 10, 0, 0, 0, 0, 0x100, 6, &c));
    CHECK(c.result == AAR_E_IO && rc.write_starts == 3);
    aar_serve_destroy(S); /* does not wait for a console it does not own */
    S = NULL;
    while (next_comp(&c))
        ;
    finish();
}

static void test_a_replacement_console_read_and_end_of_input(void)
{
    struct comp c;

    setup("repl_console_read");
    memset(&rc, 0, sizeof(rc));
    rc.live = 1;
    use_hooks(NULL, &rc_full);
    make_serve();
    CHECK(post(AAR_OP_CONSOLE_READ, 7, 0, 0, 0, 0, 0x200, 64) == AAR_OK);
    ticks(2);
    CHECK(rc.read_starts == 1 && rc.read_max == 64);
    CHECK(!next_comp(&c));
    aar_serve_read_done(S, 5, "typed");
    CHECK(wait_comp(&c, 20));
    CHECK(c.id == 7 && c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 5);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x200), "typed", 5) == 0);
    /* the end of input: 0 bytes, and it stays the end */
    CHECK(post(AAR_OP_CONSOLE_READ, 8, 0, 0, 0, 0, 0x200, 64) == AAR_OK);
    ticks(2);
    CHECK(rc.read_starts == 2);
    aar_serve_read_done(S, 0, NULL);
    CHECK(wait_comp(&c, 20));
    CHECK(c.id == 8 && c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 0);
    CHECK(call(AAR_OP_CONSOLE_READ, 9, 0, 0, 0, 0, 0x200, 64, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 0 && rc.read_starts == 2);
    finish();
}

/* A launcher line the program's console never completes does not hold the
 * destroy: the console is the program's. */
static void test_destroy_does_not_wait_for_a_replacement_console(void)
{
    struct comp c;

    setup("repl_console_destroy");
    memset(&rc, 0, sizeof(rc));
    rc.live = 1;
    use_hooks(NULL, &rc_full);
    make_serve();
    aar_serve_status(S, "unfinished");
    ticks(2);
    CHECK(rc.write_starts == 1);
    aar_serve_destroy(S);
    S = NULL;
    while (next_comp(&c))
        ;
    finish();
}

/* A table is checked when the loop is created: open for a display, write_start,
 * read_start and live for a console. */
static void test_replacement_tables_are_checked_at_create(void)
{
    static const struct aar_serve_display_ops no_open = { 0 };
    static const struct aar_serve_console_ops no_live = { NULL, NULL, rc_write_start, rc_read_start, NULL, NULL, NULL };
    static const struct aar_serve_console_ops no_write = { NULL, NULL, NULL, rc_read_start, NULL, rc_live, NULL };
    static const struct aar_serve_console_ops no_read = { NULL, NULL, rc_write_start, NULL, NULL, rc_live, NULL };
    aar_serve *s = (aar_serve *)(uintptr_t)1;

    setup("hooks_tables");
    use_hooks(&no_open, NULL);
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_INVAL && s == NULL);
    use_hooks(NULL, &no_live);
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_INVAL);
    use_hooks(NULL, &no_write);
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_INVAL);
    use_hooks(NULL, &no_read);
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_INVAL);
    use_hooks(&rd_only_open, &rc_full);
    app_state(H, AAR_STATE_RUNNING, AAR_END_NONE);
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_OK && s != NULL);
    aar_serve_destroy(s);
    finish();
}

/* With poll_us 0 and only the library's signal, a stop that was asked still ends
 * the run at its grace deadline: a timed wake of at most 10 ms is armed for it. */
static void stop_cb(void *arg)
{
    (void)arg;
    aar_serve_stop(S);
}

static void test_a_stop_deadline_is_met_without_a_heartbeat(void)
{
    uint64_t t0;

    setup("poll0_stop");
    cfg.poll_us = 0;
    cfg.flags |= AAR_SERVEF_USE_SIGNAL;
    make_serve();
    /* the armed timer is in the mask, whatever poll_us says */
    CHECK((aar_serve_sigmask(S) & (1UL << S->tm.port->mp_SigBit)) != 0);
    (void)t0;
    hx_sched(70000, stop_cb, NULL); /* between two watchdog ticks */
    CHECK(aar_serve_run(S) == AAR_SERVE_ENDED);
    CHECK(hx_now < 70000u + 1100000u); /* two graces of 500 ms and a tick, not a slow watchdog */
    finish();
}

/* ... and a claim that is revoked while the app is quiet is noticed within the
 * watchdog's 250 ms. */
static void test_a_revoked_claim_is_noticed_without_a_heartbeat(void)
{
    setup("poll0_claim");
    cfg.poll_us = 0;
    cfg.flags |= AAR_SERVEF_USE_SIGNAL;
    make_serve();
    hx_sched(300000, revoke_notice_cb, NULL); /* the loop learns of it at its next pass */
    CHECK(aar_serve_run(S) == AAR_SERVE_LOST);
    CHECK(hx_now < 600000u);
    finish();
}

/* ... and a console write to NIL: is still delivered. */
static void test_a_nil_write_is_delivered_without_a_heartbeat(void)
{
    struct comp c;

    setup("poll0_nil");
    cfg.poll_us = 0;
    cfg.flags |= AAR_SERVEF_USE_SIGNAL;
    hx_output_bptr = MKBADDR(&hx_nil_fh_store);
    hx_input_bptr = MKBADDR(&hx_nil_fh_store);
    make_serve();
    put_str(0x100, "hello");
    aar_serve_status(S, "note"); /* a launcher line goes first */
    CHECK(call(AAR_OP_CONSOLE_WRITE, 1, 0, 0, 0, 0, 0x100, 5, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CONSOLE_COUNT] == 5);
    finish();
}

/* ... and a completion that found the completion queue full is posted soon after
 * the app has taken the others (a 1 ms wake), not a watchdog period later. */
static void test_a_completion_backlog_is_flushed_without_a_heartbeat(void)
{
    struct comp c;
    unsigned sent = 0, got = 0, got_read = 0, guard;
    uint64_t t, t99 = 0;

    setup("poll0_backlog");
    cfg.poll_us = 0;
    cfg.flags |= AAR_SERVEF_USE_SIGNAL;
    data_dir();
    make_serve();
    memcpy(hx_in_data, "late\n", 5);
    hx_in_len = 5;
    hx_con_delay_us = 1000000;
    put_str(0x100, "hello.txt");
    CHECK(post(AAR_OP_CONSOLE_READ, 99, 0, 0, 0, 0, 0x200, 64) == AAR_OK);
    for (guard = 0; guard < 200; guard++) {
        while (sent < 120 && post(AAR_OP_FILE_STAT, 100 + sent, 0, 0, 0, 0, 0x100, 10) == AAR_OK)
            sent++;
        ticks(2);
    }
    CHECK(refplat_app_cq_used(SLOT) >= AAR_QUEUE_DEPTH_DEFAULT - 1);
    hx_run_for(1500000);
    ticks(10); /* the read is done; its completion has no room */
    t = hx_now;
    for (guard = 0; guard < 4000 && (got < sent || !got_read); guard++) {
        while (next_comp(&c)) {
            if (c.id == 99) {
                got_read++;
                t99 = hx_now;
            } else {
                got++;
            }
        }
        tick();
    }
    CHECK(got == sent && got_read == 1);
    CHECK(t99 - t < 100000u); /* the held completion, not the rest of the requests */
    finish();
}

/* Without USE_SIGNAL the timed wakes are all that wakes a loop without a
 * heartbeat: the armed timer is in the mask. */
static void test_the_timer_is_in_the_mask_without_a_heartbeat(void)
{
    setup("poll0_mask");
    cfg.poll_us = 0;
    make_serve();
    CHECK((aar_serve_sigmask(S) & (1UL << S->tm.port->mp_SigBit)) != 0);
    finish();
}

/* A replacement display that only pans (no change_buffer) pans its pages. */
static void test_a_replacement_display_that_pans(void)
{
    static const struct aar_serve_display_ops pan_only = { .open = rd_open, .pan = rd_pan, .wait_tof = rd_tof };
    struct comp c;

    setup("repl_display_pan");
    memset(&rd, 0, sizeof(rd));
    use_hooks(&pan_only, NULL);
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC));
    CHECK(c.result == AAR_OK);
    CHECK(call(AAR_OP_DISPLAY_FLIP, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && (c.flags & AAR_CFLAG_FLIP_SUBMITTED) && rd.pans == 1 && rd.changes == 0);
    CHECK(c.out[AAR_R_DISPLAY_FLIP_FRONT] == 0x1000 + 128u * DH);
    CHECK(call(AAR_OP_DISPLAY_FLIP, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK && rd.pans == 2 && c.out[AAR_R_DISPLAY_FLIP_FRONT] == 0x1000);
    finish();
}

/* A replacement console without an output_error still has its failure handled. */
static void test_a_replacement_console_without_an_error_hook(void)
{
    static const struct aar_serve_console_ops quiet = { NULL, NULL, rc_write_start, rc_read_start, NULL, rc_live, NULL };
    struct comp c;
    struct aar_serve_result res;

    setup("repl_console_quiet");
    memset(&rc, 0, sizeof(rc));
    rc.live = 1;
    use_hooks(NULL, &quiet);
    make_serve();
    put_str(0x100, "hello");
    CHECK(post(AAR_OP_CONSOLE_WRITE, 8, 0, 0, 0, 0, 0x100, 5) == AAR_OK);
    ticks(2);
    aar_serve_write_done(S, 1);
    CHECK(wait_comp(&c, 20));
    CHECK(c.result == AAR_E_IO);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_CONSOLE_FAILED) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    finish();
}

/* The window's raw keys follow break_mask too: with only Ctrl-E left to the loop,
 * Ctrl-C does not stop the app and Ctrl-D is an ordinary key, Ctrl-E is a break. */
static void test_the_window_keys_follow_break_mask(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("break_mask_keys");
    cfg.break_mask = SIGBREAKF_CTRL_E;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    hx_window_post(IDCMP_RAWKEY, 0x33, IEQUALIFIER_CONTROL, 0, 0); /* Ctrl-C */
    hx_window_post(IDCMP_RAWKEY, 0x22, IEQUALIFIER_CONTROL, 0, 0); /* Ctrl-D */
    hx_window_post(IDCMP_RAWKEY, 0x12, IEQUALIFIER_CONTROL, 0, 0); /* Ctrl-E */
    ticks(3);
    nev = 0;
    (void)next_comp(&c);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK(!(res.flags & (AAR_SERVE_R_USER_STOP | AAR_SERVE_R_STOP_ASKED)));
    CHECK(event_seen(AAR_EV_KEY_DOWN, 0x33) && event_seen(AAR_EV_KEY_DOWN, 0x22));
    CHECK(!event_seen(AAR_EV_BREAK, AAR_BREAK_CTRL_D));
    CHECK(event_seen(AAR_EV_BREAK, AAR_BREAK_CTRL_E));
    finish();
}

/* break_mask leaves Ctrl-C to the program: a DOS wait neither waits on it, nor
 * stops the request, nor stops the app, nor consumes it. */
static void test_a_break_the_program_owns_is_not_consumed_by_a_dos_wait(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("break_mask_dos");
    cfg.break_mask = 0;
    data_dir();
    make_serve();
    hx_sig |= SIGBREAKF_CTRL_C;
    put_str(0x100, "hello.txt");
    CHECK(call(AAR_OP_FILE_STAT, 3, 0, 0, 0, 0, 0x100, 10, &c));
    CHECK(c.result == AAR_OK);
    CHECK((hx_sig & SIGBREAKF_CTRL_C) != 0);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK(!(res.flags & (AAR_SERVE_R_USER_STOP | AAR_SERVE_R_STOP_ASKED)));
    hx_sig &= ~SIGBREAKF_CTRL_C;
    finish();
}

/* ---- Tests: windows, scaling, input and the calls of API minor 6 ---------------------------------------- */

static void drain(void)
{
    struct comp c;

    while (next_comp(&c))
        ;
}

static const aar_event *last_event(uint16_t type)
{
    unsigned i;

    for (i = nev; i > 0; i--)
        if (evs[i - 1].type == type)
            return &evs[i - 1];
    return NULL;
}

static uint32_t get_be(const volatile uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be(volatile uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A request with up to six arguments. */
static int post6(uint32_t op, uint32_t id, const uint32_t arg[6], uint32_t off, uint32_t len)
{
    return refplat_app_request(SLOT, op, 0, id, arg, len ? refplat_buf_token(SLOT, 0) : 0, off, len);
}

static int call6(uint32_t op, uint32_t id, const uint32_t arg[6], uint32_t off, uint32_t len,
                 struct comp *c)
{
    if (post6(op, id, arg, off, len) != AAR_OK)
        return 0;
    return wait_comp(c, 400);
}

/* The block the platform allocated for the front token of an open or resize reply: its pixels,
 * NULL when the token is no live surface. */
static uint8_t *surface_mem(const struct comp *c)
{
    uint32_t i, front = c->out[AAR_R_DISPLAY_OPEN_FRONT];

    for (i = 0; i < REFPLAT_SURFACES; i++)
        if (refplat.surfaces[i].handle != 0 &&
            REFPLAT_SURFACE_TOKEN_BASE + refplat.surfaces[i].off == front)
            return (uint8_t *)(uintptr_t)refplat_ptr(front);
    return NULL;
}

/* The rows of that block at the pitch of the reply. */
static uint32_t surface_rows(const struct comp *c)
{
    uint32_t i, front = c->out[AAR_R_DISPLAY_OPEN_FRONT];

    for (i = 0; i < REFPLAT_SURFACES; i++)
        if (refplat.surfaces[i].handle != 0 &&
            REFPLAT_SURFACE_TOKEN_BASE + refplat.surfaces[i].off == front &&
            c->out[AAR_R_DISPLAY_OPEN_PITCH] != 0)
            return refplat.surfaces[i].bytes / c->out[AAR_R_DISPLAY_OPEN_PITCH];
    return 0;
}

/* The surface of the last open or resize: the bytes of pixel (x, y) start with x, then y. */
static void fill_surface(const struct comp *c, uint32_t bpp)
{
    uint8_t *px = surface_mem(c);
    uint32_t pitch = c->out[AAR_R_DISPLAY_OPEN_PITCH], rows = surface_rows(c), x, y;

    CHECK(px != NULL && pitch != 0 && rows != 0);
    if (!px || !pitch || !rows)
        return;
    for (y = 0; y < rows; y++)
        for (x = 0; x < pitch / bpp && x < 256u; x++) {
            px[y * pitch + x * bpp] = (uint8_t)x;
            px[y * pitch + x * bpp + 1u] = (uint8_t)y;
        }
}

/* A pixel of the fake drawing area (two bytes per pixel). */
static uint32_t canvas_px(uint32_t x, uint32_t y)
{
    const uint8_t *p = hx_canvas + y * HX_CANVAS_W * 4u + x * 2u;

    return ((uint32_t)p[0] << 8) | p[1];
}

static void flip_once(uint32_t id)
{
    struct comp c;

    CHECK(call(AAR_OP_DISPLAY_FLIP, id, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK);
}

#define WINF_ALL (AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE | AAR_DISPLAY_POINTER_ABS | \
                  AAR_DISPLAY_TEXT_INPUT | AAR_DISPLAY_CLOSE_REQUEST)
#define CAPS_BASE (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8)

static void test_a_window_display_shows_its_surface_in_a_window_on_the_public_screen(void)
{
    struct comp c;
    struct Window *w;
    uint32_t front;

    setup("display_window");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, WINF_ALL));
    CHECK(c.result == AAR_OK);
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE | AAR_DISPCAP_POINTER_ABS |
                      AAR_DISPCAP_TEXT_INPUT | AAR_DISPCAP_CLOSE_REQUEST));
    /* no screen of its own: a window on the public screen, with a surface bitmap of the format */
    CHECK(hx_screens == 0 && hx_windows == 1 && hx_buffers_live == 0 && refplat.surface_live == 1);
    CHECK(hx_pub_locks == 1 && refplat.surface_allocs == 1);
    w = hx_cur_window;
    CHECK(w && w->hx_pub == &hx_pub_screen);
    CHECK(w->Width == (WORD)(DW + 8u) && w->Height == (WORD)(DH + 11u + 10u)); /* a size gadget below */
    CHECK(w->hx_close_gadget && w->hx_size_gadget);
    CHECK(strcmp(w->hx_title, "aar-serve") == 0);
    CHECK((w->hx_idcmp & IDCMP_NEWSIZE) && (w->hx_idcmp & IDCMP_ACTIVEWINDOW) &&
          (w->hx_idcmp & IDCMP_INACTIVEWINDOW) && (w->hx_idcmp & IDCMP_CLOSEWINDOW));
    CHECK(!(w->hx_idcmp & IDCMP_DELTAMOVE)); /* absolute positions */
    front = c.out[AAR_R_DISPLAY_OPEN_FRONT];
    CHECK(front != 0 && c.out[AAR_R_DISPLAY_OPEN_BACK] == 0 && c.out[AAR_R_DISPLAY_OPEN_SPARE] == 0);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_PITCH] == ((DW * 2u + 15u) & ~15u));
    CHECK(c.out[AAR_R_DISPLAY_OPEN_LAYOUT] == AAR_LAYOUT_RGB565_BE);
    /* the window was cleared once; a flip draws the surface at the top-left of the inner area */
    CHECK(hx_rectfills == 1 && hx_pwa_calls == 0);
    fill_surface(&c, 2);
    flip_once(21);
    /* the flip draws from the allocation itself: one allocation, no copy of the frame */
    CHECK(hx_pwa_calls == 1 && hx_pwa_last_mem == (const void *)surface_mem(&c) && refplat.surface_allocs == 1);
    CHECK(canvas_px(4, 11) == 0x0000u);
    CHECK(canvas_px(4 + 10, 11 + 5) == ((10u << 8) | 5u));
    CHECK(canvas_px(4 + DW - 1u, 11 + DH - 1u) == (((DW - 1u) << 8) | (DH - 1u)));
    CHECK(canvas_px(4 + DW, 11) == 0xCDCDu); /* nothing outside the surface */
    CHECK(hx_windows == 1 && refplat.surface_live == 1 && refplat.surface_frees == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(c.result == AAR_OK);
    CHECK(hx_windows == 0 && refplat.surface_live == 0 && refplat.surface_frees == 1 && hx_pub_locks == 0);
    /* it opens again */
    CHECK(open_display(&c, AAR_FMT_BGRA8888, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW));
    CHECK(hx_cur_window && !hx_cur_window->hx_size_gadget && hx_cur_window->BorderBottom == 4);
    finish();
}

static void test_a_window_that_the_public_screen_cannot_show_falls_back_to_a_screen(void)
{
    struct comp c;

    setup("display_window_fallback");
    make_serve();
    /* the public screen is not a Picasso96 screen */
    hx_pub_is_p96 = 0;
    CHECK(open_display(&c, AAR_FMT_RGB565, WINF_ALL));
    CHECK(c.result == AAR_OK);
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_POINTER_ABS | AAR_DISPCAP_TEXT_INPUT));
    CHECK(hx_screens == 1 && hx_pub_locks == 0 && refplat.surface_live == 0);
    CHECK(hx_cur_window && hx_cur_window->hx_custom_screen);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* a public screen of 8 bits */
    hx_pub_is_p96 = 1;
    hx_pub_rgb = RGBFB_CLUT;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE));
    CHECK(c.flags == CAPS_BASE && hx_screens == 1);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* a CLUT8 surface needs a screen of its own: its palette is the screen's */
    hx_pub_rgb = RGBFB_B8G8R8A8;
    CHECK(open_display(&c, AAR_FMT_CLUT8, AAR_DISPLAY_WINDOW));
    CHECK(c.flags == CAPS_BASE && hx_screens == 1 && hx_pub_locks == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 23, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* a surface the platform cannot allocate */
    refplat.surface_alloc_rc = AAR_E_NOMEM;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE));
    CHECK(c.result == AAR_OK && c.flags == CAPS_BASE && hx_screens == 1 && hx_pub_locks == 0);
    CHECK(refplat.surface_allocs == 0 && refplat.surface_live == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 24, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    refplat.surface_alloc_rc = AAR_OK;
    /* a surface whose token the platform refuses is given back at once */
    hx_token_fail = 1;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && c.flags == CAPS_BASE && hx_screens == 1 && hx_pub_locks == 0);
    CHECK(refplat.surface_allocs == 1 && refplat.surface_frees == 1 && refplat.surface_live == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 25, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* a library whose table ends at lease_pause, and one with no table: the surface would be Amiga
     * memory, which AARSharedToken does not name, so the screen is the fallback and the memory
     * that was tried is let go */
    hx_surface_ext = 1;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && c.flags == CAPS_BASE && hx_screens == 1 && hx_pub_locks == 0);
    CHECK(refplat.surface_allocs == 1 && refplat.surface_live == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 26, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    hx_surface_ext = 2;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && c.flags == CAPS_BASE && hx_screens == 1 && hx_pub_locks == 0);
    CHECK(refplat.surface_allocs == 1 && refplat.surface_live == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 27, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    hx_surface_ext = 0;
    /* the flags that need a window say nothing without one: RESIZABLE alone */
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_RESIZABLE | AAR_DISPLAY_CLOSE_REQUEST));
    CHECK(c.flags == CAPS_BASE && hx_screens == 1);
    finish();
}

/* The surface of a window or scaled display is the platform's stable allocation, in the app's own
 * format and stride whatever the public screen's pixel format is, and nothing of Picasso96 is
 * made for it. Without the platform's table, Amiga memory that AARSharedToken names is it. */
static void test_the_window_surface_is_the_platforms_allocation_in_the_apps_format(void)
{
    struct comp c;
    uint32_t arg[6], allocs;
    uint8_t *pixels;
    const uint8_t *px;

    setup("display_window_surface");
    cfg.flags |= AAR_SERVEF_NO_INPUT_WINDOW;
    make_serve();
    memset(arg, 0, sizeof(arg));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW));
    CHECK(hx_cur_window && !hx_cur_window->hx_custom_screen);
    /* the window of the display takes the pointer, with no input window */
    arg[1] = AAR_POINTER_DEFAULT;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 60, arg, 0, 0, &c) && c.result == AAR_OK);
    arg[1] = AAR_POINTER_BUSY;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 61, arg, 0, 0, &c) && c.result == AAR_OK && hx_ptr_kind == 1);
    arg[1] = AAR_POINTER_HIDDEN;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 62, arg, 0, 0, &c) && c.result == AAR_OK && hx_ptr_kind == 2);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 63, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(refplat.surface_live == 0 && hx_pointers_live == 0 && hx_pub_locks == 0);
    /* a 16 bit public screen and a 32 bit surface: the surface is the app's, 4 bytes a pixel */
    hx_pub_rgb = RGBFB_R5G6B5;
    CHECK(open_display(&c, AAR_FMT_BGRA8888, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && (c.flags & AAR_DISPCAP_WINDOW));
    CHECK(c.out[AAR_R_DISPLAY_OPEN_PITCH] == ((DW * 4u + 15u) & ~15u));
    CHECK(c.out[AAR_R_DISPLAY_OPEN_FRONT] >= REFPLAT_SURFACE_TOKEN_BASE && refplat.surface_live == 1);
    pixels = surface_mem(&c);
    CHECK(pixels != NULL);
    if (pixels) {
        size_t off = 3u * c.out[AAR_R_DISPLAY_OPEN_PITCH] + 5u * 4u;

        pixels[off] = 0x01u;
        pixels[off + 1u] = 0x02u;
        pixels[off + 2u] = 0x03u;
        pixels[off + 3u] = 0x04u;
    }
    flip_once(66);
    px = hx_canvas + (11u + 3u) * HX_CANVAS_W * 4u + (4u + 5u) * 4u;
    CHECK(hx_pwa_last_rgb == RGBFB_B8G8R8A8 && hx_pwa_last_mem == (const void *)pixels);
    CHECK(px[0] == 0x01u && px[1] == 0x02u && px[2] == 0x03u && px[3] == 0x04u);
    arg[1] = AAR_POINTER_BUSY;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 64, arg, 0, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 65, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(refplat.surface_live == 0 && refplat.surface_frees == 2 && hx_pub_locks == 0);
    /* no surface calls in the library, and Amiga memory that AARSharedToken names (a platform that
     * shares it): that memory is the surface, and FreeMem gives it back */
    hx_surface_ext = 2;
    hx_exec_mem_in_arena = 1;
    allocs = refplat.surface_allocs;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW));
    CHECK(refplat.surface_allocs == allocs && hx_arena_mem_live == 1);
    pixels = (uint8_t *)(uintptr_t)refplat_ptr(c.out[AAR_R_DISPLAY_OPEN_FRONT]);
    pixels[3u * c.out[AAR_R_DISPLAY_OPEN_PITCH] + 5u * 2u] = 0x07u;
    pixels[3u * c.out[AAR_R_DISPLAY_OPEN_PITCH] + 5u * 2u + 1u] = 0x09u;
    flip_once(67);
    CHECK(hx_pwa_last_mem == (const void *)pixels && canvas_px(4 + 5, 11 + 3) == 0x0709u);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 68, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(hx_arena_mem_live == 0);
    hx_surface_ext = 0;
    hx_exec_mem_in_arena = 0;
    /* the platform's allocation is named with the scanout of the screen the display is shown on:
     * the public screen's bitmap for a window. It is read without a lock, and it is no app buffer:
     * the display records no spare, the token is only in this reply */
    hx_pub_w = 100;
    hx_pub_h = 60;
    hx_pub_rgb = RGBFB_B8G8R8A8;
    hx_pub_scanout = 1;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW | AAR_DISP_REPLY_SCANOUT));
    CHECK(c.out[AAR_R_DISPLAY_OPEN_FRONT] >= REFPLAT_SURFACE_TOKEN_BASE);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_SPARE] != 0 && S->core.display.spare == 0);
    CHECK((const void *)(uintptr_t)refplat_ptr(c.out[AAR_R_DISPLAY_OPEN_SPARE]) ==
          (const void *)hx_pub_screen.bm.Planes[0]);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 69, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* without the platform's allocation there is no scanout to name */
    hx_surface_ext = 2;
    hx_exec_mem_in_arena = 1;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.result == AAR_OK && c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW) &&
          c.out[AAR_R_DISPLAY_OPEN_SPARE] == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 70, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    hx_surface_ext = 0;
    hx_exec_mem_in_arena = 0;
    finish();
}

static void test_a_scaled_window_keeps_the_aspect_and_follows_its_size(void)
{
    struct comp c;
    const aar_event *e;
    uint32_t x, y, ok = 1;

    setup("display_scale_window");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565,
                       AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE | AAR_DISPLAY_RESIZABLE |
                           AAR_DISPLAY_POINTER_ABS));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW | AAR_DISPCAP_SCALED | AAR_DISPCAP_RESIZE |
                      AAR_DISPCAP_POINTER_ABS));
    fill_surface(&c, 2);
    /* the user makes the window 128 x 96 inside: the 64 x 32 surface is 128 x 64 in it, with bars */
    hx_cur_window->Width = (WORD)(128 + 8);
    hx_cur_window->Height = (WORD)(96 + 11 + 10);
    hx_window_post(IDCMP_NEWSIZE, 0, 0, 0, 0);
    ticks(3);
    drain();
    e = last_event(AAR_EV_RESIZE);
    CHECK(e != NULL && e->dx == 128 && e->dy == 96);
    flip_once(21);
    CHECK(hx_pwa_calls >= 2); /* strips of 16 rows: four of them */
    for (y = 0; y < 64; y++)
        for (x = 0; x < 128; x++)
            if (canvas_px(4 + x, 11 + 16 + y) != (((x / 2u) << 8) | (y / 2u)))
                ok = 0;
    CHECK(ok);
    /* the bars above and below were cleared, the area outside the window is untouched */
    CHECK(canvas_px(4, 11) == 0 && canvas_px(4 + 127, 11 + 15) == 0);
    CHECK(canvas_px(4, 11 + 16 + 64) == 0 && canvas_px(4 + 127, 11 + 95) == 0);
    CHECK(canvas_px(4 + 128, 11) == 0xCDCDu);
    /* a pointer position is a position on the surface: the window's inner origin and the bars go */
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, (WORD)(4 + 20), (WORD)(11 + 16 + 10));
    ticks(2);
    drain();
    e = last_event(AAR_EV_MOUSE);
    CHECK(e && e->dx == 10 && e->dy == 5 && e->buttons == AAR_MOUSE_ABSOLUTE);
    /* the same size again: the frame is drawn again with no new clear */
    {
        unsigned long fills = hx_rectfills;

        flip_once(22);
        CHECK(hx_rectfills == fills);
    }
    /* a taller window: bars at the sides, and the next flip clears and draws again */
    hx_cur_window->Width = (WORD)(200 + 8);
    hx_cur_window->Height = (WORD)(32 + 11 + 10);
    hx_window_post(IDCMP_NEWSIZE, 0, 0, 0, 0);
    ticks(3);
    drain();
    e = last_event(AAR_EV_RESIZE);
    CHECK(e && e->dx == 200 && e->dy == 32);
    flip_once(23);
    /* 64 x 32 in 200 x 32: 64 x 32 at x = 68 */
    CHECK(canvas_px(4 + 68, 11) == 0x0000u && canvas_px(4 + 68 + 5, 11 + 3) == ((5u << 8) | 3u));
    CHECK(canvas_px(4 + 67, 11 + 3) == 0 && canvas_px(4 + 68 + 64, 11 + 3) == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 24, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

static void test_a_scaled_screen_has_a_surface_of_its_own(void)
{
    struct comp c;
    const aar_event *e;
    uint32_t x, y, ok = 1;

    setup("display_scale_screen");
    /* a Workbench screen the fake drawing area can hold */
    hx_pub_w = 240;
    hx_pub_h = 160;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_SCALE | AAR_DISPLAY_POINTER_ABS));
    CHECK(c.result == AAR_OK);
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_SCALED | AAR_DISPCAP_POINTER_ABS | AAR_DISP_REPLY_SCANOUT));
    /* the screen has the size of the Workbench screen, no screen buffers, an allocation for the surface */
    CHECK(hx_screens == 1 && hx_buffers_live == 0 && refplat.surface_live == 1);
    CHECK(hx_cur_screen && hx_cur_screen->Width == 240 && hx_cur_screen->Height == 160);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_PITCH] == ((DW * 2u + 15u) & ~15u) && c.out[AAR_R_DISPLAY_OPEN_BACK] == 0);
    CHECK(hx_pub_locks == 0);
    /* the front token is the surface's, not the screen's bitmap */
    CHECK(c.out[AAR_R_DISPLAY_OPEN_FRONT] >= REFPLAT_SURFACE_TOKEN_BASE && surface_mem(&c) != NULL);
    /* the spare word of the reply is the screen's own scanout, not a buffer of the app */
    CHECK(c.out[AAR_R_DISPLAY_OPEN_SPARE] != 0 && S->core.display.spare == 0);
    CHECK((const void *)(uintptr_t)refplat_ptr(c.out[AAR_R_DISPLAY_OPEN_SPARE]) ==
          (const void *)hx_cur_screen->bm.Planes[0]);
    fill_surface(&c, 2);
    flip_once(21);
    /* 64 x 32 in 240 x 160: 240 x 120, 20 rows of bars above and below, drawn through the window */
    for (y = 0; y < 120; y++)
        for (x = 0; x < 240; x++) {
            uint32_t sx = ((2u * x + 1u) * 64u) / (2u * 240u), sy = ((2u * y + 1u) * 32u) / (2u * 120u);

            if (canvas_px(x, 20 + y) != ((sx << 8) | sy))
                ok = 0;
        }
    CHECK(ok);
    CHECK(canvas_px(0, 0) == 0 && canvas_px(239, 19) == 0 && canvas_px(0, 140) == 0 && canvas_px(239, 159) == 0);
    /* the pointer: back through the rectangle */
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, 120, 20 + 60);
    ticks(2);
    drain();
    e = last_event(AAR_EV_MOUSE);
    CHECK(e && e->dx == 32 && e->dy == 16 && e->buttons == AAR_MOUSE_ABSOLUTE);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(hx_screens == 0 && refplat.surface_live == 0 && refplat.surface_frees == 1);
    /* the surface cannot be allocated: a screen of the surface's size, not scaled */
    refplat.surface_alloc_rc = AAR_E_NOMEM;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_SCALE));
    CHECK(c.result == AAR_OK && c.flags == CAPS_BASE);
    CHECK(hx_cur_screen && hx_cur_screen->Width == (WORD)DW && hx_cur_screen->Height >= (WORD)DH);
    finish();
}

static void test_a_window_is_resized_by_the_app(void)
{
    struct comp c;
    uint32_t arg[6], first_front, front2;

    setup("display_resize");
    hx_pub_w = 100;
    hx_pub_h = 60;
    hx_pub_scanout = 1;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE));
    first_front = c.out[AAR_R_DISPLAY_OPEN_FRONT];
    CHECK(c.result == AAR_OK && (c.flags & AAR_DISP_REPLY_SCANOUT) && c.out[AAR_R_DISPLAY_OPEN_SPARE] != 0);
    memset(arg, 0, sizeof(arg));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    arg[1] = 100u;
    arg[2] = 50u;
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 30, arg, 0, 0, &c));
    CHECK(c.result == AAR_OK);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_ID] == AAR_SERVE_DISPLAY_ID);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_PITCH] == ((100u * 2u + 15u) & ~15u));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE));
    front2 = c.out[AAR_R_DISPLAY_OPEN_FRONT];
    CHECK(front2 != 0 && front2 != first_front);
    /* the new surface was allocated before the old one was given back; the reply names no scanout */
    CHECK(refplat.surface_live == 1 && refplat.surface_allocs == 2 && refplat.surface_frees == 1);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_SPARE] == 0 && surface_rows(&c) >= 50);
    /* a flip shows the new surface (the inner area of the window did not change: 64 x 32 shown) */
    fill_surface(&c, 2);
    flip_once(31);
    CHECK(canvas_px(4 + 7, 11 + 9) == ((7u << 8) | 9u));
    /* the user resizes the window: the event carries the inner size */
    hx_cur_window->Width = (WORD)(120 + 8);
    hx_cur_window->Height = (WORD)(80 + 11 + 10);
    hx_window_post(IDCMP_NEWSIZE, 0, 0, 0, 0);
    ticks(3);
    drain();
    CHECK(last_event(AAR_EV_RESIZE) && last_event(AAR_EV_RESIZE)->dx == 120 &&
          last_event(AAR_EV_RESIZE)->dy == 80);
    /* a surface that cannot be allocated leaves the old one, which the flips go on drawing */
    refplat.surface_alloc_rc = AAR_E_NOMEM;
    arg[1] = 200u;
    arg[2] = 100u;
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 32, arg, 0, 0, &c));
    CHECK(c.result == AAR_E_NOMEM && refplat.surface_live == 1 && refplat.surface_frees == 1);
    refplat.surface_alloc_rc = AAR_OK;
    flip_once(33);
    CHECK(hx_pwa_last_mem == (const void *)(uintptr_t)refplat_ptr(front2));
    /* sizes outside 1..4096 */
    arg[1] = 0u;
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 34, arg, 0, 0, &c) && c.result == AAR_E_INVAL);
    arg[1] = 4097u;
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 35, arg, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 36, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* no resize cap: a window that was not asked to be resizable, and a screen */
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    arg[1] = 100u;
    arg[2] = 50u;
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 37, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 38, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_RESIZABLE));
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 39, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    /* and without a display it is invalid */
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 40, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 41, arg, 0, 0, &c) && c.result == AAR_E_INVAL);
    finish();
}

static void test_active_wheel_and_mouse_events(void)
{
    struct comp c;
    unsigned i, wheels = 0, keys = 0;

    setup("input_events");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    hx_window_post(IDCMP_ACTIVEWINDOW, 0, 0, 0, 0);
    hx_window_post(IDCMP_INACTIVEWINDOW, 0, 0, 0, 0);
    /* the NewMouse wheel: up, down, left, right, each with its release, and an ordinary key */
    hx_window_post(IDCMP_RAWKEY, 0x7A, 1, 0, 0);
    hx_window_post(IDCMP_RAWKEY, 0x7A | IECODE_UP_PREFIX, 1, 0, 0);
    hx_window_post(IDCMP_RAWKEY, 0x7B, 0, 0, 0);
    hx_window_post(IDCMP_RAWKEY, 0x7C, 0, 0, 0);
    hx_window_post(IDCMP_RAWKEY, 0x7D, 0, 0, 0);
    hx_window_post(IDCMP_RAWKEY, 0x45, 0, 0, 0);
    /* the buttons: left down, middle down, left up; then motion */
    hx_window_post(IDCMP_MOUSEBUTTONS, SELECTDOWN, 0, 0, 0);
    hx_window_post(IDCMP_MOUSEBUTTONS, MIDDLEDOWN, 0, 0, 0);
    hx_window_post(IDCMP_MOUSEBUTTONS, SELECTUP, 0, 0, 0);
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, 5, -3);
    ticks(4);
    drain();
    CHECK(evs[0].type == AAR_EV_ACTIVE && evs[0].code == 1);
    CHECK(evs[1].type == AAR_EV_ACTIVE && evs[1].code == 0);
    for (i = 0; i < nev; i++) {
        if (evs[i].type == AAR_EV_WHEEL)
            wheels++;
        if (evs[i].type == AAR_EV_KEY_DOWN || evs[i].type == AAR_EV_KEY_UP)
            keys++;
    }
    CHECK(wheels == 4 && keys == 1); /* the wheel keys are not sent as keys, up or down */
    CHECK(evs[2].type == AAR_EV_WHEEL && evs[2].dx == 0 && evs[2].dy == -1 && evs[2].buttons == 1);
    CHECK(evs[3].type == AAR_EV_WHEEL && evs[3].dx == 0 && evs[3].dy == 1);
    CHECK(evs[4].type == AAR_EV_WHEEL && evs[4].dx == -1 && evs[4].dy == 0);
    CHECK(evs[5].type == AAR_EV_WHEEL && evs[5].dx == 1 && evs[5].dy == 0);
    CHECK(evs[6].type == AAR_EV_KEY_DOWN && evs[6].code == 0x45);
    /* code: the button that changed, buttons: the state of all of them */
    CHECK(evs[7].type == AAR_EV_MOUSE && evs[7].code == 1 && evs[7].buttons == AAR_MOUSE_LEFT);
    CHECK(evs[8].type == AAR_EV_MOUSE && evs[8].code == 3 &&
          evs[8].buttons == (AAR_MOUSE_LEFT | AAR_MOUSE_MIDDLE));
    CHECK(evs[9].type == AAR_EV_MOUSE && evs[9].code == 1 && evs[9].buttons == AAR_MOUSE_MIDDLE);
    CHECK(evs[10].type == AAR_EV_MOUSE && evs[10].code == 0 && evs[10].dx == 5 && evs[10].dy == -3 &&
          evs[10].buttons == AAR_MOUSE_MIDDLE);
    CHECK(events_of(AAR_EV_TEXT) == 0 && events_of(AAR_EV_CLOSE_REQUEST) == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

static void test_text_input_maps_keys_with_the_keymap(void)
{
    struct comp c;
    APTR dead_prev = &c;
    unsigned i;

    setup("text_input");
    cfg.joy_ports = 0;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_TEXT_INPUT));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_TEXT_INPUT) && hx_keymap_open == 1);
    hx_window_post(IDCMP_RAWKEY, 0x10, 0, 0, 0);                    /* q */
    hx_window_post(IDCMP_RAWKEY, 0x10 | IECODE_UP_PREFIX, 0, 0, 0); /* its release: no text */
    hx_window_post(IDCMP_RAWKEY, 0x10, 1, 0, 0);                    /* shift q */
    hx_window_post(IDCMP_RAWKEY, 0x44, 0, 0, 0);                    /* return: a key, no text */
    hx_window_post(IDCMP_RAWKEY, 0x4C, 0, 0, 0);                    /* cursor up: a CSI, no text */
    hx_window_post(IDCMP_RAWKEY, 0x0D, 0, 0, 0);                    /* a Latin-1 letter */
    hx_window_post(IDCMP_RAWKEY, 0x10, IEQUALIFIER_LCOMMAND, 0, 0); /* a shortcut: no text */
    hx_window_post(IDCMP_RAWKEY, 0x00, 0, 0, 0);                    /* a dead key: no text */
    hx_post_iaddr = &dead_prev;                                     /* Intuition's previous keys */
    hx_window_post(IDCMP_RAWKEY, 0x12, 0, 0, 0);                    /* e after it: a grave e */
    hx_post_iaddr = NULL;
    hx_window_post(IDCMP_RAWKEY, 0x12, 0, 0, 0);                    /* e alone: an e */
    ticks(4);
    drain();
    CHECK(events_of(AAR_EV_TEXT) == 5);
    /* in order: the key first, its characters after it, the qualifiers with them */
    CHECK(evs[0].type == AAR_EV_KEY_DOWN && evs[0].code == 0x10);
    CHECK(evs[1].type == AAR_EV_TEXT && evs[1].code == 'q' && evs[1].buttons == 0);
    CHECK(evs[2].type == AAR_EV_KEY_UP && evs[2].code == 0x10);
    CHECK(evs[3].type == AAR_EV_KEY_DOWN && evs[4].type == AAR_EV_TEXT && evs[4].code == 'Q' &&
          evs[4].buttons == 1);
    for (i = 5; i < nev && evs[i].type != AAR_EV_TEXT; i++)
        ;
    CHECK(i < nev && evs[i].code == 0xE9u && evs[i - 1].type == AAR_EV_KEY_DOWN && evs[i - 1].code == 0x0D);
    for (i++; i < nev && evs[i].type != AAR_EV_TEXT; i++)
        ;
    CHECK(i < nev && evs[i].code == 0xE8u && evs[i - 1].code == 0x12);
    CHECK(hx_keymap_calls == 9); /* every key press, not the release */
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    aar_serve_destroy(S);
    S = NULL;
    CHECK(hx_keymap_open == 0);
    drain();
    /* no keymap.library: the cap is not granted and nothing is sent */
    hx_keymap_present = 0;
    nev = 0;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_TEXT_INPUT));
    CHECK(c.result == AAR_OK && c.flags == CAPS_BASE);
    hx_window_post(IDCMP_RAWKEY, 0x10, 0, 0, 0);
    ticks(3);
    drain();
    CHECK(events_of(AAR_EV_KEY_DOWN) == 1 && events_of(AAR_EV_TEXT) == 0);
    /* and text input that was not asked for sends none, with the keymap there */
    hx_keymap_present = 1;
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(c.flags == CAPS_BASE && hx_keymap_open == 0);
    finish();
}

static void test_absolute_pointer_positions(void)
{
    struct comp c;
    const aar_event *e;

    setup("pointer_abs");
    cfg.joy_ports = 0;
    make_serve();
    /* on a screen: positions from the surface's top-left, in the buttons too */
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_POINTER_ABS));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_POINTER_ABS));
    CHECK(!(hx_cur_window->hx_idcmp & IDCMP_DELTAMOVE));
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, 30, 40);
    hx_window_post(IDCMP_MOUSEBUTTONS, SELECTDOWN, 0, 31, 41);
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, 32, 42);
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, -5, 70); /* outside the surface, as with a button held */
    hx_window_post(IDCMP_MOUSEBUTTONS, SELECTUP, 0, -5, 70);
    ticks(4);
    drain();
    CHECK(nev == 5);
    CHECK(evs[0].type == AAR_EV_MOUSE && evs[0].code == 0 && evs[0].dx == 30 && evs[0].dy == 40 &&
          evs[0].buttons == AAR_MOUSE_ABSOLUTE);
    CHECK(evs[1].code == 1 && evs[1].dx == 31 && evs[1].dy == 41 &&
          evs[1].buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT));
    CHECK(evs[2].code == 0 && evs[2].dx == 32 && evs[2].buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT));
    CHECK(evs[3].dx == -5 && evs[3].dy == 70);
    CHECK(evs[4].code == 1 && evs[4].buttons == AAR_MOUSE_ABSOLUTE && evs[4].dx == -5);
    /* a motion to the same place is still an event: a position is not a delta */
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, 0, 0);
    ticks(2);
    drain();
    e = last_event(AAR_EV_MOUSE);
    CHECK(e && nev == 6 && e->dx == 0 && e->dy == 0 && e->buttons == AAR_MOUSE_ABSOLUTE);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* in a window: from the inner area, not the window's corner */
    nev = 0;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_POINTER_ABS));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW | AAR_DISPCAP_POINTER_ABS));
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, 4 + 10, 11 + 20);
    hx_window_post(IDCMP_MOUSEBUTTONS, MENUDOWN, 0, 4, 11);
    ticks(3);
    drain();
    CHECK(nev == 2 && evs[0].dx == 10 && evs[0].dy == 20 && evs[1].code == 2 && evs[1].dx == 0 &&
          evs[1].dy == 0 && evs[1].buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_RIGHT));
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* not asked for: deltas, and the position is not in the buttons */
    nev = 0;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW));
    CHECK(hx_cur_window->hx_idcmp & IDCMP_DELTAMOVE);
    hx_window_post(IDCMP_MOUSEMOVE, 0, 0, 3, -2);
    ticks(2);
    drain();
    /* the right button is still down from before: the state is in the buttons */
    CHECK(nev == 1 && evs[0].dx == 3 && evs[0].dy == -2 && evs[0].buttons == AAR_MOUSE_RIGHT);
    finish();
}

static void test_the_close_gadget_sends_a_close_request_when_asked(void)
{
    struct comp c;
    struct aar_serve_result res;

    setup("close_request");
    cfg.joy_ports = 0;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_CLOSE_REQUEST));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW | AAR_DISPCAP_CLOSE_REQUEST));
    hx_window_post(IDCMP_CLOSEWINDOW, 0, 0, 0, 0);
    ticks(3);
    drain();
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK(events_of(AAR_EV_CLOSE_REQUEST) == 1 && events_of(AAR_EV_STOP) == 0);
    CHECK(!(res.flags & AAR_SERVE_R_STOP_ASKED));
    /* Ctrl-C still stops it */
    hx_window_post(IDCMP_RAWKEY, 0x33, IEQUALIFIER_CONTROL, 0, 0);
    ticks(3);
    drain();
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_USER_STOP) && events_of(AAR_EV_STOP) >= 1);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c) || 1);
    aar_serve_destroy(S);
    S = NULL;
    drain();
    /* a window opened without the flag: the gadget stops the app */
    nev = 0;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW));
    hx_window_post(IDCMP_CLOSEWINDOW, 0, 0, 0, 0);
    ticks(3);
    drain();
    (void)aar_serve_result(S, &res);
    CHECK(events_of(AAR_EV_CLOSE_REQUEST) == 0 && (res.flags & AAR_SERVE_R_STOP_ASKED) &&
          !(res.flags & AAR_SERVE_R_USER_STOP));
    finish();
}

static void test_display_info_and_modes(void)
{
    struct comp c;
    uint32_t arg[6];
    uint32_t pal_mhz = aar_serve_refresh_mhz(525u, 113u, 3571429u);
    uint32_t pub_mhz = aar_serve_refresh_mhz(750u, 95u, 3571429u);
    volatile uint8_t *b;

    setup("display_info");
    make_serve();
    /* the modes need no display: the Picasso96 modes that are available, the one of the
     * Workbench screen's size preferred (640 x 480) */
    memset(arg, 0, sizeof(arg));
    CHECK(call6(AAR_OP_DISPLAY_MODES, 30, arg, 0, 24u * 8u, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_MODES_COUNT] == 3 && c.out[AAR_R_DISPLAY_MODES_TOTAL] == 3);
    b = buf(0);
    CHECK(get_be(b + 0) == 640 && get_be(b + 4) == 480 &&
          get_be(b + 8) == aar_serve_refresh_mhz(525u, 113u, 3571429u));
    CHECK(get_be(b + 12) == ((1u << AAR_FMT_RGB565) | (1u << AAR_FMT_CLUT8)));
    CHECK(get_be(b + 16) == AAR_MODEF_PREFERRED && get_be(b + 20) == 0);
    CHECK(get_be(b + 24) == 1280 && get_be(b + 28) == 720 && get_be(b + 40) == 0);
    CHECK(get_be(b + 36) == ((1u << AAR_FMT_RGB565) | (1u << AAR_FMT_BGRA8888) | (1u << AAR_FMT_CLUT8)));
    CHECK(get_be(b + 48) == 1920 && get_be(b + 52) == 1080 && get_be(b + 60) == (1u << AAR_FMT_BGRA8888));
    /* the room is two records: two written, three counted */
    CHECK(call6(AAR_OP_DISPLAY_MODES, 31, arg, 0, 24u * 2u, &c));
    CHECK(c.out[AAR_R_DISPLAY_MODES_COUNT] == 2 && c.out[AAR_R_DISPLAY_MODES_TOTAL] == 3);
    /* no Picasso96: no modes */
    aar_serve_destroy(S);
    S = NULL;
    drain();
    hx_p96_present = 0;
    make_serve();
    CHECK(call6(AAR_OP_DISPLAY_MODES, 32, arg, 0, 24u * 8u, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_MODES_COUNT] == 0 && c.out[AAR_R_DISPLAY_MODES_TOTAL] == 0);
    aar_serve_destroy(S);
    S = NULL;
    drain();
    hx_p96_present = 1;
    make_serve();
    /* a screen display: its mode's refresh rate, the screen is the surface's size */
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    CHECK(call6(AAR_OP_DISPLAY_INFO, 33, arg, 0, 0, &c));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_INFO_WIDTH] == DW && c.out[AAR_R_DISPLAY_INFO_HEIGHT] == DH);
    CHECK(c.out[AAR_R_DISPLAY_INFO_REFRESH_MHZ] == pal_mhz && pal_mhz != 0);
    CHECK(c.out[AAR_R_DISPLAY_INFO_SCREEN_W] == DW && c.out[AAR_R_DISPLAY_INFO_SCREEN_H] == DH);
    CHECK(c.out[AAR_R_DISPLAY_INFO_FLAGS] == AAR_DISPINFO_ACTIVE);
    hx_cur_window->Flags = 0; /* another window has the focus */
    CHECK(call6(AAR_OP_DISPLAY_INFO, 34, arg, 0, 0, &c));
    CHECK(c.out[AAR_R_DISPLAY_INFO_FLAGS] == 0);
    arg[0] = 5u;
    CHECK(call6(AAR_OP_DISPLAY_INFO, 35, arg, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 36, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* a window display: the public screen's size and mode, window and scaled flags */
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    CHECK(call6(AAR_OP_DISPLAY_INFO, 37, arg, 0, 0, &c));
    CHECK(c.out[AAR_R_DISPLAY_INFO_SCREEN_W] == 640 && c.out[AAR_R_DISPLAY_INFO_SCREEN_H] == 480);
    CHECK(c.out[AAR_R_DISPLAY_INFO_REFRESH_MHZ] == pub_mhz);
    CHECK(c.out[AAR_R_DISPLAY_INFO_FLAGS] ==
          (AAR_DISPINFO_WINDOW | AAR_DISPINFO_SCALED | AAR_DISPINFO_ACTIVE));
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 38, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

static void test_the_title_goes_to_the_window_or_the_screen(void)
{
    struct comp c;
    uint32_t arg[6];

    setup("display_title");
    cfg.screen_title = (CONST_STRPTR)"My Game";
    make_serve();
    memset(arg, 0, sizeof(arg));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    CHECK(strcmp(hx_cur_window->hx_title, "My Game") == 0);
    memcpy((void *)(uintptr_t)buf(0), "Zo\xC3\xAB \xE2\x82\xAC", 9);
    CHECK(call6(AAR_OP_DISPLAY_TITLE, 30, arg, 0, 9, &c) && c.result == AAR_OK);
    CHECK(strcmp(hx_cur_window->hx_title, "Zo\xEB ?") == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 31, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* on a screen: the screen's title, through the input window */
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(call6(AAR_OP_DISPLAY_TITLE, 32, arg, 0, 9, &c) && c.result == AAR_OK);
    CHECK(strcmp(hx_cur_window->hx_screen_title, "Zo\xEB ?") == 0);
    /* too long, and no NUL */
    memset((void *)(uintptr_t)buf(0), 'x', 300);
    CHECK(call6(AAR_OP_DISPLAY_TITLE, 33, arg, 0, 256, &c) && c.result == AAR_E_INVAL);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 34, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    aar_serve_destroy(S);
    S = NULL;
    drain();
    /* with no input window of ours it is the screen's own title */
    cfg.flags |= AAR_SERVEF_NO_INPUT_WINDOW;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    memcpy((void *)(uintptr_t)buf(0), "Zo\xC3\xAB \xE2\x82\xAC", 9);
    CHECK(call6(AAR_OP_DISPLAY_TITLE, 35, arg, 0, 9, &c) && c.result == AAR_OK);
    CHECK(hx_cur_screen && hx_cur_screen->Title && strcmp((const char *)hx_cur_screen->Title, "Zo\xEB ?") == 0);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 36, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* and a window display without an input window: no IDCMP, none of the caps that need input */
    CHECK(open_display(&c, AAR_FMT_RGB565, WINF_ALL));
    CHECK(c.flags == (CAPS_BASE | AAR_DISPCAP_WINDOW));
    CHECK(hx_cur_window->hx_idcmp == 0 && !hx_cur_window->hx_size_gadget && !hx_cur_window->hx_close_gadget);
    memcpy((void *)(uintptr_t)buf(0), "Zo\xC3\xAB \xE2\x82\xAC", 9);
    CHECK(call6(AAR_OP_DISPLAY_TITLE, 37, arg, 0, 9, &c) && c.result == AAR_OK);
    CHECK(strcmp(hx_cur_window->hx_title, "Zo\xEB ?") == 0);
    finish();
}

/* a word of the pointer bitmap's planes: the pixel value (0 to 3) at x, y */
static unsigned pointer_pixel(const struct hx_ptr *p, unsigned x, unsigned y)
{
    unsigned bit = 0x80u >> (x & 7u), off = y * p->bm->BytesPerRow + (x >> 3);

    return ((p->bm->Planes[0][off] & bit) ? 1u : 0u) | ((p->bm->Planes[1][off] & bit) ? 2u : 0u);
}

static void test_the_pointer_can_be_hidden_busy_or_an_image(void)
{
    struct comp c;
    uint32_t arg[6], x;
    const struct hx_ptr *p;

    setup("display_pointer");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW));
    memset(arg, 0, sizeof(arg));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    arg[1] = AAR_POINTER_BUSY;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 30, arg, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_ptr_kind == 1);
    /* hidden: an image with nothing in it */
    arg[1] = AAR_POINTER_HIDDEN;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 31, arg, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_ptr_kind == 2 && hx_pointers_live == 1 && hx_bitmaps_live == 1);
    p = hx_ptr_obj;
    CHECK(p && pointer_pixel(p, 0, 0) == 0 && pointer_pixel(p, 15, 0) == 0);
    /* a custom image of 5 x 3: pixels 0xAARRGGBB, quantised to the Preferences colours
     * (black, white, orange); the hot spot is the offset of the object */
    for (x = 0; x < 15; x++)
        put_be(buf(0) + x * 4u, 0u);
    put_be(buf(0) + 0, 0xFF000000u);          /* black */
    put_be(buf(0) + 4, 0xFFFFFFFFu);          /* white */
    put_be(buf(0) + 8, 0xFFFF8800u);          /* orange */
    put_be(buf(0) + 12, 0x40FFFFFFu);         /* mostly transparent */
    put_be(buf(0) + 16, 0xFFF0F0F0u);         /* nearly white */
    put_be(buf(0) + 20 + 8, 0xFF101010u);     /* row 1, pixel 2: nearly black */
    arg[1] = AAR_POINTER_CUSTOM;
    arg[2] = 5;
    arg[3] = 3;
    arg[4] = 2;
    arg[5] = 1;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 32, arg, 0, 5u * 3u * 4u, &c) && c.result == AAR_OK);
    CHECK(hx_ptr_kind == 2 && hx_pointers_live == 1 && hx_bitmaps_live == 1); /* the hidden one is gone */
    p = hx_ptr_obj;
    CHECK(p && p->xo == -2 && p->yo == -1);
    CHECK(pointer_pixel(p, 0, 0) == 1 && pointer_pixel(p, 1, 0) == 2 && pointer_pixel(p, 2, 0) == 3);
    CHECK(pointer_pixel(p, 3, 0) == 0 && pointer_pixel(p, 4, 0) == 2);
    CHECK(pointer_pixel(p, 2, 1) == 1 && pointer_pixel(p, 0, 2) == 0);
    CHECK(p->bm->BytesPerRow >= 2); /* a width of 16 for 5 pixels */
    /* too big, and bad hot spots */
    arg[2] = 65;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 33, arg, 0, 65u * 3u * 4u, &c) && c.result == AAR_E_INVAL);
    arg[2] = 5;
    arg[4] = 5;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 34, arg, 0, 5u * 3u * 4u, &c) && c.result == AAR_E_INVAL);
    arg[4] = 0;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 35, arg, 0, 5u * 3u * 4u - 4u, &c) && c.result == AAR_E_INVAL);
    arg[1] = 9;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 36, arg, 0, 0, &c) && c.result == AAR_E_INVAL);
    /* a bitmap that cannot be made is a failure, and the pointer stays */
    hx_fail_alloc = 1;
    arg[1] = AAR_POINTER_CUSTOM;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 37, arg, 0, 5u * 3u * 4u, &c) && c.result == AAR_E_NOMEM);
    hx_fail_alloc = 0;
    CHECK(hx_ptr_kind == 2 && hx_ptr_obj == p);
    /* busy takes the image off, default gives the system's back */
    arg[1] = AAR_POINTER_BUSY;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 38, arg, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_ptr_kind == 1 && hx_pointers_live == 0 && hx_bitmaps_live == 0);
    arg[1] = AAR_POINTER_CUSTOM;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 39, arg, 0, 5u * 3u * 4u, &c) && c.result == AAR_OK);
    arg[1] = AAR_POINTER_DEFAULT;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 40, arg, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_ptr_kind == 0 && hx_pointers_live == 0 && hx_bitmaps_live == 0);
    /* an image that stays when the display closes is let go of */
    arg[1] = AAR_POINTER_CUSTOM;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 41, arg, 0, 5u * 3u * 4u, &c) && c.result == AAR_OK);
    CHECK(hx_pointers_live == 1);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 42, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(hx_pointers_live == 0 && hx_bitmaps_live == 0);
    /* an Intuition before 39 has no sprite pointers */
    hx_intuition_version = 37;
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    arg[1] = AAR_POINTER_HIDDEN;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 43, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    arg[1] = AAR_POINTER_CUSTOM;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 44, arg, 0, 5u * 3u * 4u, &c) && c.result == AAR_E_NOSYS);
    /* and with no window of ours there is nothing to set it on */
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 45, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    hx_intuition_version = 47;
    aar_serve_destroy(S);
    S = NULL;
    drain();
    cfg.flags |= AAR_SERVEF_NO_INPUT_WINDOW;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    arg[1] = AAR_POINTER_BUSY;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 46, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    finish();
}

static void test_the_built_in_displays_cannot_see_a_vertical_blank(void)
{
    struct comp c;
    uint32_t arg[6];

    setup("display_vblank_nosys");
    make_serve();
    memset(arg, 0, sizeof(arg));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    arg[1] = 5000u;
    /* no display, and a zero timeout */
    CHECK(call6(AAR_OP_DISPLAY_VBLANK, 29, arg, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    arg[1] = 0u;
    CHECK(call6(AAR_OP_DISPLAY_VBLANK, 30, arg, 0, 0, &c) && c.result == AAR_E_INVAL);
    /* a Picasso96 screen, then a window with a scaled surface: the vertical blank that
     * timer.device counts is the chipset's, not the RTG display's, so none is promised */
    arg[1] = 5000u;
    CHECK(call6(AAR_OP_DISPLAY_VBLANK, 31, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    CHECK(hx_timer_open_count == 1); /* the heartbeat's timer only */
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 32, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE));
    CHECK(call6(AAR_OP_DISPLAY_VBLANK, 33, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    CHECK(hx_timer_open_count == 1);
    finish();
}

/* A replacement display's vertical blank: the program reports each one. */
static int xv_watch_on, xv_watch_off, xv_can = 1;

static int xv_vblank(void *ctx, int watch)
{
    (void)ctx;
    if (watch) {
        xv_watch_on++;
        return xv_can;
    }
    xv_watch_off++;
    return 1;
}

/* A request posted and served at once, without waiting for a wake of the loop. */
static int post_now(uint32_t op, uint32_t id, const uint32_t arg[6])
{
    if (post6(op, id, arg, 0, 0) != AAR_OK)
        return 0;
    aar_serve_step(S, 0);
    return 1;
}

static void test_a_replacement_display_waits_for_its_vblank_and_times_out(void)
{
    static struct aar_serve_display_ops table;
    struct comp c;
    uint32_t arg[6];
    uint64_t t0;

    setup("repl_display_vblank");
    memset(&rd, 0, sizeof(rd));
    table = rd_full;
    table.vblank = xv_vblank;
    xv_watch_on = xv_watch_off = 0;
    xv_can = 1;
    cfg.poll_us = 0; /* no heartbeat: the loop must wake for the timeout itself */
    use_hooks(&table, NULL);
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    memset(arg, 0, sizeof(arg));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    arg[1] = 400000u;
    /* parked until the program reports the blank, while the loop serves other requests */
    CHECK(post_now(AAR_OP_DISPLAY_VBLANK, 30, arg));
    CHECK(xv_watch_on == 1 && !next_comp(&c));
    CHECK(post_now(AAR_OP_DISPLAY_INFO, 31, arg));
    CHECK(next_comp(&c) && c.id == 31 && c.result == AAR_OK);
    CHECK(!next_comp(&c));
    aar_serve_vblank(S);
    CHECK(wait_comp(&c, 400) && c.id == 30 && c.result == AAR_OK && xv_watch_off == 1);
    /* no blank comes: the call ends at its timeout, without a heartbeat */
    arg[1] = 30000u;
    t0 = hx_now;
    CHECK(post_now(AAR_OP_DISPLAY_VBLANK, 32, arg));
    CHECK(wait_comp(&c, 400) && c.id == 32 && c.result == AAR_E_TIMEDOUT);
    CHECK(hx_now - t0 >= 29000u && hx_now - t0 <= 30000u + 1000u);
    /* the timeout is rounded up to a millisecond */
    arg[1] = 1u;
    t0 = hx_now;
    CHECK(post_now(AAR_OP_DISPLAY_VBLANK, 33, arg));
    CHECK(wait_comp(&c, 400) && c.id == 33 && c.result == AAR_E_TIMEDOUT);
    CHECK(hx_now - t0 >= 1000u && hx_now - t0 <= 2000u);
    CHECK(xv_watch_off == 3);
    /* a close while the call waits ends it with AAR_E_IO */
    arg[1] = 400000u;
    CHECK(post_now(AAR_OP_DISPLAY_VBLANK, 34, arg));
    CHECK(!next_comp(&c));
    {
        unsigned got34 = 0, got35 = 0;

        CHECK(post_now(AAR_OP_DISPLAY_CLOSE, 35, arg));
        while (next_comp(&c)) {
            if (c.id == 34) {
                got34 = 1;
                CHECK(c.result == AAR_E_IO);
            }
            if (c.id == 35) {
                got35 = 1;
                CHECK(c.result == AAR_OK);
            }
        }
        CHECK(got34 && got35);
    }
    /* and the destroy of the loop while a call waits */
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    CHECK(post_now(AAR_OP_DISPLAY_VBLANK, 36, arg));
    CHECK(!next_comp(&c));
    aar_serve_destroy(S);
    S = NULL;
    {
        unsigned got36 = 0;

        while (next_comp(&c))
            if (c.id == 36) {
                got36 = 1;
                CHECK(c.result == AAR_E_IO);
            }
        CHECK(got36 && xv_watch_off == 5);
    }
    /* a table that cannot see a vertical blank: not implemented, nothing parked */
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    xv_can = 0;
    arg[1] = 5000u;
    CHECK(post_now(AAR_OP_DISPLAY_VBLANK, 37, arg));
    CHECK(next_comp(&c) && c.id == 37 && c.result == AAR_E_NOSYS);
    CHECK(!next_comp(&c));
    finish();
}

static void test_cd32_ports_are_read_with_lowlevel(void)
{
    struct comp c;
    unsigned long reads;

    setup("joystick_cd32");
    /* lowlevel.library is needed, and only a port a bit names */
    cfg.cd32_ports = 4;
    CHECK(aar_serve_create(H, &cfg, &S) == AAR_E_INVAL && S == NULL);
    hx_ll_present = 0;
    cfg.cd32_ports = 2;
    CHECK(aar_serve_create(H, &cfg, &S) == AAR_E_NOSYS && S == NULL);
    hx_ll_present = 1;
    CHECK(hx_ll_open == 0);
    /* port 1 as a CD32 pad, port 0 as the default registers: the registers of a port that is a pad
     * are not read */
    hx_joydat[1] = 0x0200; /* left, if it were read */
    hx_jp[1] = JP_TYPE_GAMECTLR | JPF_BUTTON_RED | JPF_BUTTON_BLUE | JPF_JOY_RIGHT;
    make_serve();
    CHECK(hx_ll_open == 1);
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    ticks(5);
    drain();
    CHECK(events_of(AAR_EV_JOY) == 1);
    CHECK(evs[nev - 1].type == AAR_EV_JOY && evs[nev - 1].code == 1 && evs[nev - 1].dx == 1 &&
          evs[nev - 1].dy == 0 && evs[nev - 1].buttons == (AAR_JOY_BUTTON1 | AAR_JOY_BUTTON2));
    /* a change is an event; the pad is read about once a frame, not at every pass */
    reads = hx_jp_reads;
    hx_jp[1] = JP_TYPE_GAMECTLR | JPF_BUTTON_GREEN | JPF_BUTTON_YELLOW | JPF_BUTTON_PLAY |
               JPF_BUTTON_FORWARD | JPF_BUTTON_REVERSE | JPF_JOY_UP | JPF_JOY_LEFT;
    ticks(40);
    drain();
    CHECK(events_of(AAR_EV_JOY) == 2);
    CHECK(evs[nev - 1].dx == -1 && evs[nev - 1].dy == -1 &&
          evs[nev - 1].buttons == (AAR_JOY_BUTTON3 | AAR_JOY_BUTTON4 | AAR_JOY_PLAY | AAR_JOY_FORWARD |
                                   AAR_JOY_REVERSE));
    CHECK(hx_jp_reads - reads >= 2 && hx_jp_reads - reads <= 4);
    /* nothing changed: nothing sent. A joystick in the port is read the same way */
    hx_jp[1] = JP_TYPE_JOYSTK | JPF_BUTTON_RED | JPF_JOY_DOWN;
    ticks(40);
    drain();
    CHECK(events_of(AAR_EV_JOY) == 3 && evs[nev - 1].dy == 1 && evs[nev - 1].buttons == AAR_JOY_BUTTON1);
    /* a mouse, or nothing, in the port: released */
    hx_jp[1] = JP_TYPE_MOUSE | 0x0A05u;
    ticks(40);
    drain();
    CHECK(events_of(AAR_EV_JOY) == 4 && evs[nev - 1].dx == 0 && evs[nev - 1].dy == 0 &&
          evs[nev - 1].buttons == 0);
    hx_jp[1] = JP_TYPE_NOTAVAIL;
    ticks(40);
    drain();
    CHECK(events_of(AAR_EV_JOY) == 4);
    /* the window must have the focus, as for the registers */
    hx_jp[1] = JP_TYPE_GAMECTLR | JPF_BUTTON_RED;
    hx_cur_window->Flags = 0;
    ticks(40);
    drain();
    CHECK(events_of(AAR_EV_JOY) == 4);
    hx_cur_window->Flags = WFLG_WINDOWACTIVE;
    ticks(40);
    drain();
    CHECK(events_of(AAR_EV_JOY) == 5 && evs[nev - 1].buttons == AAR_JOY_BUTTON1);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 21, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    aar_serve_destroy(S);
    S = NULL;
    CHECK(hx_ll_open == 0);
    drain();
    /* port 0 as a pad is polled although joy_ports does not name it */
    nev = 0;
    cfg.cd32_ports = 1;
    hx_jp[0] = JP_TYPE_GAMECTLR | JPF_BUTTON_BLUE;
    hx_joydat[1] = 0;
    hx_ciaa_pra = 0xFF;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, 0));
    ticks(5);
    drain();
    CHECK(event_seen(AAR_EV_JOY, 0) && event_seen(AAR_EV_JOY, 1)); /* port 1 is the default register one */
    {
        unsigned i;

        for (i = 0; i < nev; i++)
            if (evs[i].type == AAR_EV_JOY && evs[i].code == 0)
                CHECK(evs[i].buttons == AAR_JOY_BUTTON2);
    }
    finish();
}

static int rv_watch_on, rv_watch_off, rv_can = 1;

static int rv_vblank(void *ctx, int watch)
{
    (void)ctx;
    if (watch) {
        rv_watch_on++;
        return rv_can;
    }
    rv_watch_off++;
    return 1;
}

static uint32_t canvas_px4(uint32_t x, uint32_t y)
{
    const uint8_t *p = hx_canvas + y * HX_CANVAS_W * 4u + x * 4u;

    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* The token is the address of the surface's memory, so the memory is never moved and never
 * locked: the platform allocates it once, the flips draw from that address, a resize allocates
 * the new surface before the old one goes, a resize that fails leaves the old one in place, and
 * the close (or the end of the instance) gives it back. No Picasso96 bitmap is made. */
static void test_a_surface_keeps_its_address_for_as_long_as_its_token_can_be_used(void)
{
    struct comp c;
    uint8_t *addr1, *addr2;
    uint32_t arg[6], first_token;

    setup("display_surface_stable");
    hx_pub_w = 240;
    hx_pub_h = 160;
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE));
    CHECK(c.result == AAR_OK && c.out[AAR_R_DISPLAY_OPEN_FRONT] >= REFPLAT_SURFACE_TOKEN_BASE);
    CHECK(refplat.surface_allocs == 1 && refplat.surface_live == 1);
    first_token = c.out[AAR_R_DISPLAY_OPEN_FRONT];
    addr1 = surface_mem(&c);
    CHECK(addr1 != NULL);
    fill_surface(&c, 2);
    flip_once(21);
    CHECK(hx_pwa_last_mem == (const void *)addr1);
    flip_once(22);
    CHECK(hx_pwa_calls == 2 && hx_pwa_last_mem == (const void *)addr1 && refplat.surface_allocs == 1);
    CHECK(canvas_px(4 + 10, 11 + 5) == ((10u << 8) | 5u));
    /* a resize: the new surface exists before the old one is given back, at another address */
    memset(arg, 0, sizeof(arg));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    arg[1] = 100u;
    arg[2] = 50u;
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 30, arg, 0, 0, &c) && c.result == AAR_OK);
    addr2 = surface_mem(&c);
    CHECK(addr2 != NULL && addr2 != addr1 && c.out[AAR_R_DISPLAY_OPEN_FRONT] != first_token);
    CHECK(refplat.surface_allocs == 2 && refplat.surface_frees == 1 && refplat.surface_live == 1);
    CHECK(addr1 && addr1[0] == 0xEE); /* the old block was given back */
    fill_surface(&c, 2);
    flip_once(31);
    CHECK(hx_pwa_last_mem == (const void *)addr2);
    /* a surface that cannot be allocated leaves the display as it was */
    refplat.surface_alloc_rc = AAR_E_NOMEM;
    arg[1] = 200u;
    arg[2] = 100u;
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 32, arg, 0, 0, &c) && c.result == AAR_E_NOMEM);
    refplat.surface_alloc_rc = AAR_OK;
    CHECK(refplat.surface_allocs == 2 && refplat.surface_frees == 1 && refplat.surface_live == 1);
    flip_once(33);
    CHECK(hx_pwa_last_mem == (const void *)addr2 && addr2 && addr2[2] == 1u);
    /* the close gives the allocation back */
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 34, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    CHECK(refplat.surface_live == 0 && refplat.surface_frees == 2 && addr2 && addr2[2] == 0xEE);
    /* a scaled display on a screen has an allocation of its own in the same way, in the app's
     * 32 bit format; the surface is no bitmap of the Workbench screen */
    CHECK(open_display(&c, AAR_FMT_BGRA8888, AAR_DISPLAY_SCALE));
    CHECK(c.result == AAR_OK && (c.flags & AAR_DISPCAP_SCALED));
    CHECK(refplat.surface_allocs == 3 && refplat.surface_live == 1);
    CHECK(c.out[AAR_R_DISPLAY_OPEN_PITCH] == ((DW * 4u + 15u) & ~15u));
    fill_surface(&c, 4);
    flip_once(35);
    CHECK(refplat.surface_live == 1 && hx_pub_locks == 0);
    /* the instance goes with the display open: the allocation goes with it */
    aar_serve_destroy(S);
    S = NULL;
    drain();
    CHECK(refplat.surface_live == 0 && refplat.surface_frees == 3 && refplat.surface_stale_frees == 0);
    finish();
}

/* The allocation is the platform's, not the claim's: a resize with a lost claim allocates nothing
 * and leaves the surface, and the instance gives it back although the claim was revoked. */
static void test_a_revoked_claim_gives_its_surface_back(void)
{
    struct comp c;
    struct aar_serve_req req;
    struct aar_serve_done done;
    uint32_t allocs;

    setup("display_surface_revoked");
    make_serve();
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE));
    CHECK(c.result == AAR_OK && refplat.surface_live == 1);
    allocs = refplat.surface_allocs;
    revoke_notice_cb(NULL);
    CHECK(!claim_check(S));
    memset(&req, 0, sizeof(req));
    req.op = AAR_OP_DISPLAY_RESIZE;
    req.arg[AAR_A_DISPLAY_OPEN_W] = 100u;
    req.arg[AAR_A_DISPLAY_OPEN_H] = 50u;
    req.arg[AAR_A_DISPLAY_OPEN_FORMAT] = AAR_FMT_RGB565;
    display_op_resize(S, &req, &done);
    CHECK(done.result == AAR_E_NOMEM && refplat.surface_allocs == allocs && refplat.surface_live == 1);
    aar_serve_destroy(S);
    S = NULL;
    drain();
    CHECK(refplat.surface_live == 0 && refplat.surface_frees == 1 && refplat.surface_stale_frees == 0);
    finish();
}

/* The BGRA8888 surface has no RECTFMT of cybergraphics.library ScalePixelArray (RGB, RGBA,
 * ARGB with alpha first, GREY8), so the loop scales every format itself and writes the strips
 * with p96WritePixelArray in the surface's own format. The fake fails an OpenLibrary of
 * cybergraphics.library. */
static void test_a_32_bit_surface_is_scaled_by_the_loop_in_its_own_byte_order(void)
{
    struct comp c;
    uint8_t *px;
    uint32_t pitch, rows;
    uint32_t x, y, ok = 1;

    setup("display_scale_bgra");
    make_serve();
    /* a window display of a BGRA8888 surface, the window made twice as wide and tall inside */
    CHECK(open_display(&c, AAR_FMT_BGRA8888, AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE |
                                              AAR_DISPLAY_RESIZABLE));
    CHECK(c.result == AAR_OK && (c.flags & AAR_DISPCAP_SCALED));
    fill_surface(&c, 4);
    /* the red and alpha bytes too, so that each byte of a pixel is told apart */
    px = surface_mem(&c);
    pitch = c.out[AAR_R_DISPLAY_OPEN_PITCH];
    rows = surface_rows(&c);
    CHECK(px != NULL);
    for (y = 0; px && y < rows; y++)
        for (x = 0; x < pitch / 4u && x < 256u; x++) {
            px[y * pitch + x * 4u + 2u] = 0x5A;
            px[y * pitch + x * 4u + 3u] = 0xA5;
        }
    hx_cur_window->Width = (WORD)(DW * 2u + 8u);
    hx_cur_window->Height = (WORD)(DH * 2u + 11u + 10u);
    hx_window_post(IDCMP_NEWSIZE, 0, 0, 0, 0);
    ticks(3);
    drain();
    /* four strips of 16 rows, each written as B8G8R8A8 */
    CHECK(hx_pwa_calls >= 4 && hx_pwa_last_rgb == RGBFB_B8G8R8A8 && S->gfx.strip != NULL);
    for (y = 0; y < DH * 2u; y++)
        for (x = 0; x < DW * 2u; x++)
            if (canvas_px4(4 + x, 11 + y) !=
                (((x / 2u) << 24) | ((y / 2u) << 16) | (0x5Au << 8) | 0xA5u))
                ok = 0;
    CHECK(ok);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 22, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    /* a 16 bit surface is scaled by the loop as well, in its own format */
    CHECK(open_display(&c, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE));
    fill_surface(&c, 2);
    hx_cur_window->Width = (WORD)(DW * 2u + 8u);
    hx_cur_window->Height = (WORD)(DH * 2u + 11u + 10u);
    {
        unsigned long pwa = hx_pwa_calls;

        flip_once(26);
        CHECK(hx_pwa_calls > pwa && hx_pwa_last_rgb == RGBFB_R5G6B5);
    }
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 27, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}


static void test_a_replacement_display_has_no_window_caps_and_serves_the_rest(void)
{
    static struct aar_serve_display_ops table;
    struct comp c;
    uint32_t arg[6];

    setup("repl_display_minor6");
    memset(&rd, 0, sizeof(rd));
    table = rd_full;
    table.vblank = rv_vblank;
    rv_watch_on = rv_watch_off = 0;
    rv_can = 1;
    use_hooks(&table, NULL);
    make_serve();
    /* the window flags are requests this table cannot meet: none is granted */
    CHECK(open_display(&c, AAR_FMT_RGB565, WINF_ALL | AAR_DISPLAY_SCALE | AAR_DISPLAY_VSYNC));
    CHECK(c.result == AAR_OK && c.flags == CAPS_BASE);
    memset(arg, 0, sizeof(arg));
    arg[0] = AAR_SERVE_DISPLAY_ID;
    CHECK(call6(AAR_OP_DISPLAY_INFO, 30, arg, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_DISPLAY_INFO_WIDTH] == DW && c.out[AAR_R_DISPLAY_INFO_SCREEN_W] == DW &&
          c.out[AAR_R_DISPLAY_INFO_REFRESH_MHZ] == 0 && c.out[AAR_R_DISPLAY_INFO_FLAGS] == 0);
    arg[1] = AAR_POINTER_BUSY;
    CHECK(call6(AAR_OP_DISPLAY_POINTER, 31, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    arg[1] = 50u;
    arg[2] = 50u;
    CHECK(call6(AAR_OP_DISPLAY_RESIZE, 32, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    CHECK(call6(AAR_OP_DISPLAY_MODES, 33, arg, 0, 240, &c) && c.result == AAR_OK &&
          c.out[AAR_R_DISPLAY_MODES_TOTAL] == 0);
    memcpy((void *)(uintptr_t)buf(0), "t", 2);
    CHECK(call6(AAR_OP_DISPLAY_TITLE, 34, arg, 0, 2, &c) && c.result == AAR_E_NOSYS);
    /* the table's vblank call is told to watch, and the program tells the loop about each blank */
    arg[1] = 100000u;
    CHECK(post6(AAR_OP_DISPLAY_VBLANK, 35, arg, 0, 0) == AAR_OK);
    ticks(3);
    CHECK(rv_watch_on == 1 && !next_comp(&c));
    aar_serve_vblank(S);
    CHECK(wait_comp(&c, 20) && c.id == 35 && c.result == AAR_OK && rv_watch_off == 1);
    aar_serve_vblank(S); /* none parked: ignored */
    ticks(2);
    CHECK(!next_comp(&c));
    rv_can = 0;
    CHECK(call6(AAR_OP_DISPLAY_VBLANK, 36, arg, 0, 0, &c) && c.result == AAR_E_NOSYS);
    CHECK(call(AAR_OP_DISPLAY_CLOSE, 37, AAR_SERVE_DISPLAY_ID, 0, 0, 0, 0, 0, &c));
    finish();
}

#include "serve_files_cases.h"

/* ---- Main ------------------------------------------------------------------------------------------------------------ */

#define T(fn) { #fn, fn }
static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {
    T(test_hello),
    T(test_console_long_write_is_chunked_in_order),
    T(test_launcher_text_comes_before_app_text),
    T(test_console_read_gets_a_line),
    T(test_revoke_with_a_console_read_pending_leaves_the_buffer_alone),
    T(test_output_failure_stops_the_app),
    T(test_files_open_read_seek_stat_close),
    T(test_files_write_creates_and_truncates),
    T(test_files_stay_under_the_root),
    T(test_files_absolute_paths_with_abs_through_two_directories),
    T(test_files_list_a_directory),
    T(test_file_delete_and_its_errors),
    T(test_file_delete_path_policy),
    T(test_file_delete_with_a_volume_path_needs_abs),
    T(test_a_write_protected_volume_refuses_every_change_with_acces),
    T(test_file_rename_and_its_errors),
    T(test_file_rename_replace_deletes_the_target_first),
    T(test_file_rename_stays_under_the_root_and_on_one_volume),
    T(test_dir_create_and_its_errors),
    T(test_set_date_and_stat_are_unix_seconds_utc),
    T(test_stat_of_a_file_uses_the_offset_too),
    T(test_set_date_errors),
    T(test_protection_bits_go_both_ways),
    T(test_file_comments_preserve_native_bytes),
    T(test_truncate_shortens_and_extends_with_zeros),
    T(test_truncate_needs_an_id_opened_for_writing),
    T(test_truncate_below_another_handles_position_is_busy),
    T(test_a_full_volume_stops_an_extension),
    T(test_volume_info_reports_blocks_and_protection),
    T(test_a_stop_cancels_the_new_file_requests_and_not_the_reads),
    T(test_the_platform_ending_the_app_cancels_the_new_file_requests),
    T(test_time_unix_is_utc_microseconds),
    T(test_locale_info_fills_the_record),
    T(test_locale_info_without_locale_library_is_zeros_and_ok),
    T(test_env_get_returns_the_stored_bytes_and_the_full_length),
    T(test_env_get_uses_the_full_length_getvar_reports),
    T(test_env_get_checks_the_name),
    T(test_a_config_flag_refuses_the_environment),
    T(test_clip_read_text_converts_ftxt_to_utf8),
    T(test_clip_read_units_and_types),
    T(test_a_chunked_clip_read_is_one_consistent_clip),
    T(test_a_chunked_text_write_replaces_the_clip_at_the_last_chunk),
    T(test_clip_write_sequence_errors_change_nothing),
    T(test_clip_write_empty_text_clears_the_text),
    T(test_clip_write_iff_passes_one_form_through),
    T(test_clip_without_the_system_libraries),
    T(test_a_config_flag_refuses_the_clipboard),
    T(test_ctrl_c_during_a_clipboard_write_aborts_it_and_keeps_the_clip),
    T(test_a_clipboard_device_that_ignores_the_abort_is_waited_for),
    T(test_a_clipboard_write_waits_for_the_other_program_and_completes),
    T(test_a_claim_revoked_during_a_clipboard_read_aborts_it),
    T(test_a_text_clip_of_the_limit_round_trips),
    T(test_clip_read_limits_are_on_the_text_and_on_the_form),
    T(test_the_config_flags_of_the_host_ops_are_known),
    T(test_the_host_libraries_are_closed_at_destroy),
    T(test_ctrl_c_during_a_dos_wait_stops_and_waits_for_the_reply),
    T(test_a_full_completion_queue_loses_no_completion),
    T(test_display_with_screen_buffers),
    T(test_display_falls_back_to_a_panned_tall_screen),
    T(test_display_falls_back_to_two_pages_then_one_buffer),
    T(test_display_strategy_starts_the_walk_later),
    T(test_display_palette_reaches_loadrgb32),
    T(test_display_that_cannot_open_stops_the_app),
    T(test_the_raw_key_ctrl_c_in_the_window_stops_and_the_key_goes_to_the_app),
    T(test_mouse_buttons_and_close_gadget),
    T(test_joystick_port_one_by_default_and_port_zero_by_config),
    T(test_ahi_audio_open_feed_underrun_close),
    T(test_audio_chunk_is_a_fiftieth_of_a_second),
    T(test_ahi_that_cannot_open_stops_the_app),
    T(test_a_group_the_config_leaves_out_answers_nosys),
    T(test_ctrl_c_stops_and_the_loop_ends_after_the_grace),
    T(test_ctrl_d_e_f_become_break_events),
    T(test_a_revoked_claim_ends_the_loop_as_lost),
    T(test_a_corrupt_request_queue_stops_the_app),
    T(test_logs_of_the_app_are_queued_as_lines),
    T(test_two_instances_in_one_task),
    T(test_hooks_replace_the_defaults),
    T(test_the_configuration_is_checked),
    T(test_the_workbench_console_opens_at_the_first_use),
    T(test_an_output_stream_alone_does_not_take_the_input),
    T(test_an_input_stream_alone_does_not_take_the_output),
    T(test_the_window_it_opened_is_read_as_well_as_written),
    T(test_a_requester_text_is_never_a_format),
    T(test_a_stop_cancels_the_requests_that_change_files),
    T(test_the_platform_ending_the_app_cancels_the_requests_that_change_files),
    T(test_nil_output_is_written_synchronously_and_the_app_is_not_stopped),
    T(test_the_output_error_note_goes_to_the_user_console),
    T(test_a_refused_audio_ring_closes_ahi),
    T(test_a_bitmap_token_outside_the_old_window_is_accepted),
    T(test_the_stop_grace_is_the_platforms),
    T(test_app_messages_are_drained_and_counted_by_default),
    T(test_the_message_hook_gets_the_messages),
    T(test_keep_messages_leaves_them_for_the_program),
    T(test_destroy_answers_every_held_request_once),
    T(test_the_signal_mask_includes_the_flip_and_ahi_ports),
    T(test_a_flip_completes_with_no_heartbeat),
    T(test_ahi_needs_a_heartbeat_of_at_most_a_fiftieth_of_a_second),
    T(test_no_input_window_when_the_program_has_its_own),
    T(test_buffers_with_another_pitch_fall_back_to_panning),
    T(test_close_shows_the_front_buffer_again),
    T(test_the_log_sink_is_gone_after_destroy),
    T(test_a_hook_table_from_an_older_header_has_only_the_hooks_it_has),
    T(test_destroy_waits_for_the_flip_that_is_out),
    T(test_a_tall_screen_that_opens_too_short_is_not_used),
    T(test_a_completion_of_a_held_request_waits_for_a_full_cq),
    T(test_the_heartbeat_runs_after_a_call_made_outside_the_loop),
    T(test_the_result_counts_the_logs_the_runtime_dropped),
    T(test_workbench_project_uses_dostrue_from_name_from_lock),
    T(test_ahi_that_runs_dry_is_counted_and_buffered_again),
    T(test_a_multi_packet_read_costs_one_timer_abort_per_packet),
    T(test_a_sixty_ms_loop_stall_does_not_starve_ahi),
    T(test_a_real_time_ring_gets_whole_requests_and_no_gap),
    T(test_a_replacement_display_is_recorded_and_completable),
    T(test_a_minimal_replacement_display_still_flips_and_closes),
    T(test_a_replacement_console_is_completed_through_the_public_calls),
    T(test_a_replacement_console_read_and_end_of_input),
    T(test_replacement_tables_are_checked_at_create),
    T(test_destroy_does_not_wait_for_a_replacement_console),
    T(test_a_stop_deadline_is_met_without_a_heartbeat),
    T(test_a_revoked_claim_is_noticed_without_a_heartbeat),
    T(test_a_nil_write_is_delivered_without_a_heartbeat),
    T(test_a_completion_backlog_is_flushed_without_a_heartbeat),
    T(test_the_timer_is_in_the_mask_without_a_heartbeat),
    T(test_a_replacement_display_that_pans),
    T(test_a_replacement_console_without_an_error_hook),
    T(test_a_break_the_program_owns_is_not_consumed_by_a_dos_wait),
    T(test_the_window_keys_follow_break_mask),
    T(test_a_window_display_shows_its_surface_in_a_window_on_the_public_screen),
    T(test_a_window_that_the_public_screen_cannot_show_falls_back_to_a_screen),
    T(test_the_window_surface_is_the_platforms_allocation_in_the_apps_format),
    T(test_a_scaled_window_keeps_the_aspect_and_follows_its_size),
    T(test_a_scaled_screen_has_a_surface_of_its_own),
    T(test_a_window_is_resized_by_the_app),
    T(test_active_wheel_and_mouse_events),
    T(test_text_input_maps_keys_with_the_keymap),
    T(test_absolute_pointer_positions),
    T(test_the_close_gadget_sends_a_close_request_when_asked),
    T(test_display_info_and_modes),
    T(test_the_title_goes_to_the_window_or_the_screen),
    T(test_the_pointer_can_be_hidden_busy_or_an_image),
    T(test_the_built_in_displays_cannot_see_a_vertical_blank),
    T(test_a_replacement_display_waits_for_its_vblank_and_times_out),
    T(test_cd32_ports_are_read_with_lowlevel),
    T(test_a_replacement_display_has_no_window_caps_and_serves_the_rest),
    T(test_a_32_bit_surface_is_scaled_by_the_loop_in_its_own_byte_order),
    T(test_a_surface_keeps_its_address_for_as_long_as_its_token_can_be_used),
    T(test_a_revoked_claim_gives_its_surface_back),
};

/* A test that loops without the simulation moving is a failure, not a hang. */
static void watchdog(int sig)
{
    (void)sig;
    printf("FAIL [%s] no progress for 120 s\n", cur_test);
    fflush(stdout);
#ifdef __linux__
    {
        void *frames[24];

        backtrace_symbols_fd(frames, backtrace(frames, 24), 1);
    }
#endif
    _exit(2);
}

/* serve_loop_test [name]: only the tests whose name contains the argument. */
int main(int argc, char **argv)
{
    unsigned i;

    signal(SIGALRM, watchdog);
    hx_trace = getenv("SERVE_TRACE") != NULL;
    alarm(getenv("SERVE_WATCHDOG") ? (unsigned)atoi(getenv("SERVE_WATCHDOG")) : 120u);
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        if (argc < 2 || strstr(tests[i].name, argv[1]))
            tests[i].fn();
    printf("serve_loop_test: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
