/*
 * Conformance payload for C-MSG (spec 6, 9.4, 9.6, 10.4): messages and waiting,
 * judged from the app. The runner is the peer: it echoes, numbers, counts and holds
 * back messages as the keywords of conf_msg.h ask (runner/r_msg.c). Without a
 * runner (the host runner) the tests that need one print SKIP host.
 *
 * The runner starts the payload once per variant (conf:variant=<name>) and the
 * payload runs only the tests of that variant. Without a variant (the host runner)
 * it runs every test. Variants:
 *   std       C-MSG.1 to C-MSG.3, C-MSG.6, C-MSG.8 to C-MSG.10, C-MSG.17
 *   hold      C-MSG.4, C-MSG.16 (the runner stops draining for a while)
 *   stopwait  C-MSG.11   (the app is stopped while it waits)
 *   stopmsg   C-MSG.12   (a message and then a stop)
 *   park      C-MSG.13   (a 12 second wait)
 *   inbox     the runner judges C-MSG.5 and C-MSG.7; the app only cooperates
 *   exitmsgs  the runner judges C-MSG.14; the app only sends and exits
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_msg.h"
#include "conf_msgio.h"

AAR_NOTE(0, 0, 0, 0)

static uint32_t cap = 63u;
static int have_peer;
static const char *variant;

#define WANT(name) (variant == 0 || cmio_streq(variant, name))

static int skip_no_peer(const char *id)
{
    if (have_peer)
        return 0;
    conf_skip(id, "host: no runner to exchange messages with");
    return 1;
}

/* The runner's verdict on what it received since the last BEGIN. */
static void expect_verdict(struct conf_test *t, uint32_t count, const char *what)
{
    uint32_t ok = 0, got = 0, errors = 0, first = 0;
    int rc;

    cmio_kw(CM_K_END, count, 0, 0);
    rc = cmio_result(10000000u, &ok, &got, &errors, &first, 0, 0);
    if (rc != 1) {
        if (conf_fail_first(t)) {
            conf_buf_str(&t->why, "no result message from the runner for ");
            conf_buf_str(&t->why, what);
            conf_buf_str(&t->why, ": ");
            conf_buf_i64(&t->why, rc);
        }
        return;
    }
    if (!ok && conf_fail_first(t)) {
        conf_buf_str(&t->why, what);
        conf_buf_str(&t->why, ": the runner got ");
        conf_buf_u64(&t->why, got);
        conf_buf_str(&t->why, " of ");
        conf_buf_u64(&t->why, count);
        conf_buf_str(&t->why, " messages, ");
        conf_buf_u64(&t->why, errors);
        conf_buf_str(&t->why, " wrong, first bad index ");
        conf_buf_i64(&t->why, first == CM_NONE ? -1 : (int64_t)first);
    }
}

/* Takes n data messages of a mode that the runner sends. */
static void expect_stream(struct conf_test *t, uint32_t mode, uint32_t n,
                          const char *what)
{
    uint8_t m[AAR_MSG_BYTES];
    uint32_t i;

    for (i = 0; i < n; i++) {
        int rc = cmio_recv(m, 3000000u);

        if (rc != 1) {
            if (conf_fail_first(t)) {
                conf_buf_str(&t->why, what);
                conf_buf_str(&t->why, ": message ");
                conf_buf_u64(&t->why, i);
                conf_buf_str(&t->why, " of ");
                conf_buf_u64(&t->why, n);
                conf_buf_str(&t->why, " did not arrive (");
                conf_buf_i64(&t->why, rc);
                conf_buf_str(&t->why, ")");
            }
            return;
        }
        if (!cm_check(m, mode, CM_DIR_TO_APP, i) && conf_fail_first(t)) {
            conf_buf_str(&t->why, what);
            conf_buf_str(&t->why, ": message ");
            conf_buf_u64(&t->why, i);
            conf_buf_str(&t->why, " arrived changed or out of order");
            return;
        }
    }
}

/* ---- C-MSG.1 to C-MSG.3 ---------------------------------------------------------- */

static void test_roundtrip(void)
{
    struct conf_test t;
    uint8_t m[AAR_MSG_BYTES];
    uint32_t i;

    if (skip_no_peer("C-MSG.1"))
        return;
    conf_begin(&t, "C-MSG.1");
    cmio_kw(CM_K_BEGIN, CM_MODE_BYTES, 0, 0);
    for (i = 0; i < 8u; i++) {
        int rc;

        cm_fill(m, CM_MODE_BYTES, CM_DIR_TO_CLIENT, i);
        rc = cmio_send(m, 2000000u);
        if (rc != AAR_OK) {
            if (conf_fail_first(&t))
                cmio_why_rc(&t, "msg_send", rc);
            break;
        }
    }
    expect_verdict(&t, 8, "app to client");
    cmio_kw(CM_K_SEND, CM_MODE_BYTES, 8, 0);
    expect_stream(&t, CM_MODE_BYTES, 8, "client to app");
    conf_buf_str(&t.note, "256 byte values each way");
    conf_end(&t);
}

static void test_app_to_client(void)
{
    struct conf_test t;
    uint8_t m[AAR_MSG_BYTES];
    uint32_t i;

    if (skip_no_peer("C-MSG.2"))
        return;
    conf_begin(&t, "C-MSG.2");
    cmio_kw(CM_K_BEGIN, CM_MODE_SEQ, 0, 0);
    for (i = 0; i < cap; i++) {
        int rc;

        cm_fill(m, CM_MODE_SEQ, CM_DIR_TO_CLIENT, i);
        rc = cmio_send(m, 2000000u);
        if (rc != AAR_OK) {
            if (conf_fail_first(&t))
                cmio_why_rc(&t, "msg_send", rc);
            break;
        }
    }
    expect_verdict(&t, cap, "app to client");
    conf_buf_u64(&t.note, cap);
    conf_buf_str(&t.note, " messages in order");
    conf_end(&t);
}

static void test_client_to_app(void)
{
    struct conf_test t;
    uint8_t m[AAR_MSG_BYTES];

    if (skip_no_peer("C-MSG.3"))
        return;
    conf_begin(&t, "C-MSG.3");
    cmio_kw(CM_K_SEND, CM_MODE_SEQ, cap, 0);
    expect_stream(&t, CM_MODE_SEQ, cap, "client to app");
    conf_check(&t, aar_msg_poll(m) == 0, "a message is left after the last one");
    conf_buf_u64(&t.note, cap);
    conf_buf_str(&t.note, " messages in order");
    conf_end(&t);
}

/* ---- C-MSG.4 and C-MSG.16: the client does not drain --------------------------------- */

/* Sends numbered messages from number `first` until one is refused. Returns how many were
 * accepted; a call that took 100 ms or more, or an unexpected code, fails the test. */
static uint32_t flood_phase(struct conf_test *t, uint32_t first)
{
    uint8_t m[AAR_MSG_BYTES];
    uint32_t n = 0;
    uint32_t limit = 2u * cap + 8u;

    for (;;) {
        uint64_t t0;
        uint64_t dt;
        int rc;

        if (n >= limit) {
            conf_check(t, 0, "msg_send was never refused with AAR_E_BUSY");
            break;
        }
        cm_fill(m, CM_MODE_SEQ, CM_DIR_TO_CLIENT, first + n);
        t0 = aar_time_us();
        rc = aar_msg_send(m);
        dt = aar_time_us() - t0;
        if (dt >= 100000u && conf_fail_first(t)) {
            conf_buf_str(&t->why, "msg_send blocked for ");
            conf_buf_u64(&t->why, dt);
            conf_buf_str(&t->why, " us");
        }
        if (rc == AAR_OK) {
            n++;
            continue;
        }
        if (rc != AAR_E_BUSY && conf_fail_first(t))
            cmio_why_rc(t, "msg_send", rc);
        break;
    }
    return n;
}

/* Starts the hold, then floods until the client's side is full: a client that serves
 * requests moves what is in the queue into its inbox (64 messages), so one flood ends at
 * the queue's own limit and the next, after a pause, at the inbox's. Stops when a phase
 * gets nothing in. Returns every message accepted. The first is sent only after the
 * runner has taken the BEGIN message, so it does not take an entry of the queue. */
static uint32_t hold_and_flood(struct conf_test *t, uint32_t hold_ms)
{
    uint32_t total = 0;
    uint32_t phase;

    cmio_kw(CM_K_BEGIN, CM_MODE_SEQ, hold_ms, 0);
    aar_sleep_us(40000u);
    for (phase = 0; phase < 4u; phase++) {
        uint32_t n = flood_phase(t, total);

        total += n;
        if (n == 0)
            break;
        aar_sleep_us(40000u);
    }
    return total;
}

static void test_full_inbox(void)
{
    struct conf_test t;
    uint32_t accepted;

    if (skip_no_peer("C-MSG.4"))
        return;
    conf_begin(&t, "C-MSG.4");
    accepted = hold_and_flood(&t, 600);
    if (accepted < cap && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "msg_send was refused after ");
        conf_buf_u64(&t.why, accepted);
        conf_buf_str(&t.why, " messages, below msg_capacity ");
        conf_buf_u64(&t.why, cap);
    }
    expect_verdict(&t, accepted, "after the client drained");
    conf_buf_u64(&t.note, accepted);
    conf_buf_str(&t.note, " accepted, then AAR_E_BUSY; all delivered in order");
    conf_end(&t);
}

/* A request that parks the app (file_stat) behind messages the full client inbox holds back
 * waits until the client drains. A console write would not show it: it returns when its
 * bytes are in the runtime's ring, not when the client has answered. */
static void test_holdback(void)
{
    struct conf_test t;
    aar_stat st;
    uint32_t accepted;
    uint64_t t0;
    uint64_t dt;
    int rc;

    if (skip_no_peer("C-MSG.16"))
        return;
    conf_begin(&t, "C-MSG.16");
    accepted = hold_and_flood(&t, 900);
    t0 = aar_time_us();
    rc = aar_file_stat("c-msg-16-probe", &st);
    dt = aar_time_us() - t0;
    if ((rc == AAR_E_TIMEDOUT || rc == AAR_E_IO) && conf_fail_first(&t))
        cmio_why_rc(&t, "the file request returned", rc);
    if (dt < 200000u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the file request completed after ");
        conf_buf_u64(&t.why, dt);
        conf_buf_str(&t.why, " us, before the client resumed draining");
    }
    expect_verdict(&t, accepted, "after the client drained");
    conf_buf_str(&t.note, "the request waited ");
    conf_buf_u64(&t.note, dt / 1000u);
    conf_buf_str(&t.note, " ms behind ");
    conf_buf_u64(&t.note, accepted);
    conf_buf_str(&t.note, " held messages");
    conf_end(&t);
}

/* ---- C-MSG.6, 8, 9, 10: poll and wait ------------------------------------------------- */

/* Gets one message into the app's inbox: the runner echoes it (the host runner
 * loops a sent message back). */
static int inject(uint32_t tag)
{
    uint8_t m[AAR_MSG_BYTES];

    cm_keyword(m, CM_K_ECHO, tag, ~tag, 0x12345678u);
    return cmio_send(m, 1000000u);
}

static int is_echo(const uint8_t *m, uint32_t tag)
{
    uint8_t want[AAR_MSG_BYTES];
    uint32_t i;

    cm_keyword(want, CM_K_ECHO, tag, ~tag, 0x12345678u);
    for (i = 0; i < AAR_MSG_BYTES; i++) {
        if (m[i] != want[i])
            return 0;
    }
    return 1;
}

static void test_poll(void)
{
    struct conf_test t;
    uint8_t m[AAR_MSG_BYTES];
    int rc;

    conf_begin(&t, "C-MSG.6");
    (void)cmio_drain();
    conf_check(&t, aar_msg_poll(m) == 0, "msg_poll on an empty inbox did not return 0");
    rc = inject(0x66u);
    conf_check(&t, rc == AAR_OK, "the message could not be sent");
    rc = aar_msg_wait(2000000u);
    conf_check(&t, rc == AAR_OK, "msg_wait did not report the queued message");
    {
        uint32_t k;

        for (k = 0; k < AAR_MSG_BYTES; k++)
            m[k] = 0;
    }
    rc = aar_msg_poll(m);
    conf_check(&t, rc == 1, "msg_poll with a queued message did not return 1");
    conf_check(&t, is_echo(m, 0x66u), "the polled message is not the one sent");
    conf_check(&t, aar_msg_poll(m) == 0, "msg_poll after the last message did not return 0");
    conf_end(&t);
}

static void test_wait_keeps(void)
{
    struct conf_test t;
    uint8_t m[AAR_MSG_BYTES];
    int rc;

    conf_begin(&t, "C-MSG.8");
    (void)cmio_drain();
    conf_check(&t, inject(0x88u) == AAR_OK, "the message could not be sent");
    rc = aar_msg_wait(2000000u);
    conf_check(&t, rc == AAR_OK, "msg_wait did not return AAR_OK for a queued message");
    rc = aar_msg_wait(2000000u);
    conf_check(&t, rc == AAR_OK, "a second msg_wait did not return AAR_OK: the first took the message");
    rc = aar_msg_poll(m);
    conf_check(&t, rc == 1, "the message was not still queued after msg_wait");
    conf_check(&t, rc != 1 || is_echo(m, 0x88u), "the polled message is not the one sent");
    conf_end(&t);
}

static void test_wait_timeout(void)
{
    struct conf_test t;
    static const uint32_t waits[] = { 100000u, 250000u };
    uint32_t i;

    conf_begin(&t, "C-MSG.9");
    (void)cmio_drain();
    for (i = 0; i < 2u; i++) {
        uint64_t t0 = aar_time_us();
        int rc = aar_msg_wait(waits[i]);
        uint64_t dt = aar_time_us() - t0;

        if (rc != AAR_E_TIMEDOUT && conf_fail_first(&t))
            cmio_why_rc(&t, "msg_wait on an empty inbox returned", rc);
        if (dt < waits[i] && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "msg_wait of ");
            conf_buf_u64(&t.why, waits[i]);
            conf_buf_str(&t.why, " us returned after ");
            conf_buf_u64(&t.why, dt);
            conf_buf_str(&t.why, " us");
        }
    }
    conf_buf_str(&t.note, "100 ms and 250 ms waits ran their time");
    conf_end(&t);
}

static void test_wait_zero(void)
{
    struct conf_test t;
    uint8_t m[AAR_MSG_BYTES];
    uint64_t t0;
    uint64_t dt;
    int rc;

    conf_begin(&t, "C-MSG.10");
    (void)cmio_drain();
    t0 = aar_time_us();
    rc = aar_msg_wait(0);
    dt = aar_time_us() - t0;
    conf_check(&t, rc == AAR_E_TIMEDOUT, "msg_wait(0) on an empty inbox did not return AAR_E_TIMEDOUT");
    conf_check(&t, dt < 50000u, "msg_wait(0) did not return at once");
    conf_check(&t, inject(0x99u) == AAR_OK, "the message could not be sent");
    rc = aar_msg_wait(2000000u);
    conf_check(&t, rc == AAR_OK, "the message did not arrive");
    t0 = aar_time_us();
    rc = aar_msg_wait(0);
    dt = aar_time_us() - t0;
    conf_check(&t, rc == AAR_OK, "msg_wait(0) with a queued message did not return AAR_OK");
    conf_check(&t, dt < 50000u, "msg_wait(0) with a message did not return at once");
    (void)aar_msg_poll(m);
    conf_end(&t);
}

/* ---- C-MSG.11 and C-MSG.12: a stop ends the wait ------------------------------------------ */

static void test_stop_wait(void)
{
    struct conf_test t;
    uint64_t t0;
    uint64_t dt;
    int rc;

    if (skip_no_peer("C-MSG.11"))
        return;
    conf_begin(&t, "C-MSG.11");
    (void)cmio_kw(CM_K_STOPWAIT, 300, 0, 0);
    t0 = aar_time_us();
    rc = aar_msg_wait(10000000u);
    dt = aar_time_us() - t0;
    conf_check(&t, rc == AAR_E_IO, "msg_wait was not ended with AAR_E_IO by the stop");
    conf_check(&t, dt < 8000000u, "msg_wait ran to its timeout");
    conf_check(&t, aar_should_stop() == 1, "should_stop is 0 after the stop");
    conf_buf_str(&t.note, "the wait ended after ");
    conf_buf_u64(&t.note, dt / 1000u);
    conf_buf_str(&t.note, " ms with ");
    conf_buf_i64(&t.note, rc);
    conf_end(&t);
}

static void test_stop_message_first(void)
{
    struct conf_test t;
    uint8_t m[AAR_MSG_BYTES];
    int rc1;
    int rc2;
    int polled;

    if (skip_no_peer("C-MSG.12"))
        return;
    conf_begin(&t, "C-MSG.12");
    (void)cmio_kw(CM_K_STOPMSG, 0, 0, 0);
    aar_sleep_us(1000000u); /* the stop ends it early; the message is queued by then */
    rc1 = aar_msg_wait(3000000u);
    polled = aar_msg_poll(m);
    rc2 = aar_msg_wait(3000000u);
    conf_check(&t, rc1 == AAR_OK, "msg_wait did not report the message queued before the stop");
    conf_check(&t, polled == 1, "the message queued before the stop was lost");
    conf_check(&t, rc2 == AAR_E_IO, "the next msg_wait was not ended with AAR_E_IO by the stop");
    conf_buf_str(&t.note, "AAR_OK for the message, then AAR_E_IO");
    conf_end(&t);
}

/* ---- C-MSG.13: a long park ----------------------------------------------------------------- */

static void test_long_park(void)
{
    struct conf_test t;
    uint64_t t0;
    uint64_t dt;
    int rc;

    conf_begin(&t, "C-MSG.13");
    (void)cmio_drain();
    t0 = aar_time_us();
    rc = aar_msg_wait(12000000u);
    dt = aar_time_us() - t0;
    conf_check(&t, rc == AAR_E_TIMEDOUT, "the 12 s wait did not end with AAR_E_TIMEDOUT");
    conf_check(&t, dt >= 12000000u, "the 12 s wait ended early");
    conf_buf_str(&t.note, "parked ");
    conf_buf_u64(&t.note, dt / 1000u);
    conf_buf_str(&t.note, " ms and still running");
    conf_end(&t);
}

/* ---- C-MSG.17: a burst in both directions with a slow receiver ---------------------------- */

#define FLOOD_N 2000u

static void test_burst(void)
{
    struct conf_test t;
    uint8_t m[AAR_MSG_BYTES];
    uint32_t sent = 0;
    uint32_t got = 0;
    uint32_t bad = 0;
    uint64_t deadline;

    if (skip_no_peer("C-MSG.17"))
        return;
    conf_begin(&t, "C-MSG.17");
    cmio_kw(CM_K_BEGIN, CM_MODE_SEQ, 0, 3);
    cmio_kw(CM_K_SEND, CM_MODE_SEQ, FLOOD_N, 3);
    deadline = aar_time_us() + 60000000u;
    while ((sent < FLOOD_N || got < FLOOD_N) && aar_time_us() < deadline) {
        int progressed = 0;
        int rc;

        if (sent < FLOOD_N) {
            cm_fill(m, CM_MODE_SEQ, CM_DIR_TO_CLIENT, sent);
            rc = aar_msg_send(m);
            if (rc == AAR_OK) {
                sent++;
                progressed = 1;
            } else if (rc != AAR_E_BUSY) {
                if (conf_fail_first(&t))
                    cmio_why_rc(&t, "msg_send", rc);
                break;
            }
        }
        rc = aar_msg_poll(m);
        if (rc == 1) {
            if (!cm_check(m, CM_MODE_SEQ, CM_DIR_TO_APP, got))
                bad++;
            got++;
            progressed = 1;
            if ((got & 3u) == 0)
                aar_sleep_us(200); /* the app is the slow receiver */
        } else if (rc < 0) {
            if (conf_fail_first(&t))
                cmio_why_rc(&t, "msg_poll", rc);
            break;
        }
        if (!progressed)
            aar_sleep_us(500);
    }
    conf_check(&t, sent == FLOOD_N, "not every message could be sent in 60 s");
    conf_check(&t, got == FLOOD_N, "not every message from the client arrived in 60 s");
    if (bad && conf_fail_first(&t)) {
        conf_buf_u64(&t.why, bad);
        conf_buf_str(&t.why, " messages from the client were lost, repeated or out of order");
    }
    expect_verdict(&t, FLOOD_N, "app to client");
    conf_buf_u64(&t.note, FLOOD_N);
    conf_buf_str(&t.note, " messages each way, none lost, none reordered");
    conf_end(&t);
}

/* ---- Variants the runner judges ---------------------------------------------------------- */

/* inbox: the runner fills the inbox while the app does not poll (C-MSG.5); the app
 * then polls it all (what the runner checks) and reports. */
static void variant_inbox(void)
{
    uint8_t m[AAR_MSG_BYTES];
    uint32_t n = 0;
    uint32_t ok = 1;
    uint32_t idle = 0;

    (void)cmio_kw(CM_K_FILL, 0, 0, 0);
    aar_sleep_us(2000000u); /* not polling */
    while (idle < 20u) {
        if (aar_msg_poll(m) == 1) {
            if (!cm_check(m, CM_MODE_SEQ, CM_DIR_TO_APP, n))
                ok = 0;
            n++;
            idle = 0;
        } else {
            idle++;
            aar_sleep_us(5000);
        }
    }
    (void)cmio_kw(CM_K_GOT, n, ok, 0);
}

/* exitmsgs: the app sends msg_capacity messages and exits; the client reads them
 * after the exit (C-MSG.14). */
static void variant_exitmsgs(void)
{
    uint8_t m[AAR_MSG_BYTES];
    uint32_t i;

    for (i = 0; i < cap; i++) {
        cm_fill(m, CM_MODE_SEQ, CM_DIR_TO_CLIENT, i);
        if (cmio_send(m, 2000000u) != AAR_OK)
            break;
    }
}

int aar_main(int argc, char **argv)
{
    aar_info info;

    info.size = sizeof info;
    if (aar_get_info(&info) == AAR_OK && info.msg_capacity != 0)
        cap = info.msg_capacity;
    have_peer = conf_have_peer(argc, argv);
    variant = conf_arg(argc, argv, "conf:variant");

    if (variant && cmio_streq(variant, "inbox")) {
        variant_inbox();
        return conf_finish("C-MSG");
    }
    if (variant && cmio_streq(variant, "exitmsgs")) {
        variant_exitmsgs();
        return conf_finish("C-MSG");
    }
    if (WANT("std")) {
        test_roundtrip();
        test_app_to_client();
        test_client_to_app();
        test_poll();
        test_wait_keeps();
        test_wait_timeout();
        test_wait_zero();
        test_burst();
    }
    if (WANT("hold")) {
        test_full_inbox();
        test_holdback();
    }
    if (WANT("park"))
        test_long_park();
    if (WANT("stopmsg"))
        test_stop_message_first();
    if (WANT("stopwait"))
        test_stop_wait();
    return conf_finish("C-MSG");
}
