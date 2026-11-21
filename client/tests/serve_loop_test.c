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
    CHECK(hx_live_blocks == 0);
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
