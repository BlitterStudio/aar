/*
 * Conformance payload for C-QUEUE (spec 9.6, 10.4, 10.6, 5.6): the queue pair as the
 * app sees it. Most rows are judged by the runner (runner/r_queue.c); this payload
 * judges the ones that need the app's view: a request whose completion is withheld
 * (C-QUEUE.6, C-QUEUE.7), a log flood (C-QUEUE.9) and requests served on a timer
 * (C-QUEUE.10), and it is the app of the runner's manual runs. See conf_queue.h for
 * the variants. Without a runner (the host runner) the tests print SKIP host.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_queue.h"
#include "conf_msgio.h"

AAR_NOTE(0, 0, 0, 0)

static int have_peer;
static const char *variant;

#define WANT(name) (variant == 0 || cmio_streq(variant, name))

static int skip_no_peer(const char *id)
{
    if (have_peer)
        return 0;
    conf_skip(id, "host: no runner to serve or withhold completions");
    return 1;
}

static void mark(char stage, int32_t value)
{
    uint8_t m[AAR_MSG_BYTES];

    cq_marker(m, stage, value);
    (void)cmio_send(m, 5000000u);
}

/* A parking request: file_stat is served by the client. */
static int stat_call(const char *path)
{
    aar_stat st;

    return aar_file_stat(path, &st);
}

/* ---- C-QUEUE.9: a log flood while the client does not serve ------------------------- */

#define LOG_FLOOD 5000u

static void test_log_flood(void)
{
    struct conf_test t;
    char rec[28] = "conf-queue log flood 00000";
    uint32_t i;
    uint64_t worst = 0;
    uint32_t ok = 0, received = 0, dropped = 0;
    int rc;

    if (skip_no_peer("C-QUEUE.9"))
        return;
    conf_begin(&t, "C-QUEUE.9");
    cmio_kw(CQ_K_HOLD, 600, 0, 0);
    for (i = 0; i < LOG_FLOOD; i++) {
        uint64_t t0;
        uint64_t dt;
        uint32_t v = i;
        int k;

        for (k = 25; k >= 21; k--) {
            rec[k] = (char)('0' + v % 10u);
            v /= 10u;
        }
        t0 = aar_time_us();
        aar_log(rec, 26);
        dt = aar_time_us() - t0;
        if (dt > worst)
            worst = dt;
    }
    if (worst >= 100000u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "aar_log blocked for ");
        conf_buf_u64(&t.why, worst);
        conf_buf_str(&t.why, " us with the client not serving");
    }
    cmio_kw(CQ_K_LOGSTAT, 0, 0, 0);
    rc = cmio_result(15000000u, &ok, &received, &dropped, 0, 0, 0);
    if (rc != 1) {
        if (conf_fail_first(&t))
            cmio_why_rc(&t, "no answer from the runner", rc);
    } else {
        if (received > LOG_FLOOD && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "the client received more records than were written: ");
            conf_buf_u64(&t.why, received);
        }
        if (received < LOG_FLOOD && dropped == 0 && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "the client received ");
            conf_buf_u64(&t.why, received);
            conf_buf_str(&t.why, " of ");
            conf_buf_u64(&t.why, LOG_FLOOD);
            conf_buf_str(&t.why, " records and log_dropped is 0");
        }
        conf_buf_u64(&t.note, LOG_FLOOD);
        conf_buf_str(&t.note, " records written, ");
        conf_buf_u64(&t.note, received);
        conf_buf_str(&t.note, " received, log_dropped ");
        conf_buf_u64(&t.note, dropped);
        conf_buf_str(&t.note, ", slowest call ");
        conf_buf_u64(&t.note, worst);
        conf_buf_str(&t.note, " us");
    }
    conf_end(&t);
}

/* ---- C-QUEUE.10: requests served only on a slow timer ------------------------------------ */

static void test_slow_serving(void)
{
    struct conf_test t;
    char line[11] = "Q10 00000\n";
    uint32_t i;
    uint32_t ok = 0, count = 0, errors = 0;
    int rc;

    if (skip_no_peer("C-QUEUE.10"))
        return;
    conf_begin(&t, "C-QUEUE.10");
    cmio_kw(CQ_K_SLOW, 50, 0, 0);
    for (i = 0; i < CQ_LINES; i++) {
        uint32_t v = i;
        uint32_t wrote = 0;
        int k;

        for (k = 8; k >= 4; k--) {
            line[k] = (char)('0' + v % 10u);
            v /= 10u;
        }
        rc = aar_console_write(line, 10, &wrote);
        if ((rc != AAR_OK || wrote != 10u) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "console write ");
            conf_buf_u64(&t.why, i);
            conf_buf_str(&t.why, " returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, " with ");
            conf_buf_u64(&t.why, wrote);
            conf_buf_str(&t.why, " bytes");
            break;
        }
    }
    cmio_kw(CQ_K_CHECK, CQ_LINES, 0, 0);
    rc = cmio_result(60000000u, &ok, &count, &errors, 0, 0, 0);
    (void)cmio_kw(CQ_K_SLOW, 0, 0, 0);
    if (rc != 1) {
        if (conf_fail_first(&t))
            cmio_why_rc(&t, "no answer from the runner", rc);
    } else if (!ok && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the client got ");
        conf_buf_u64(&t.why, count);
        conf_buf_str(&t.why, " of ");
        conf_buf_u64(&t.why, CQ_LINES);
        conf_buf_str(&t.why, " lines in order, ");
        conf_buf_u64(&t.why, errors);
        conf_buf_str(&t.why, " wrong");
    }
    conf_buf_u64(&t.note, CQ_LINES);
    conf_buf_str(&t.note, " console requests served every 50 ms, every byte present");
    conf_end(&t);
}

/* ---- C-QUEUE.6 and C-QUEUE.7: a completion that comes late --------------------------------- */

static void test_late_completion(void)
{
    struct conf_test t6;
    struct conf_test t7;
    uint8_t m[AAR_MSG_BYTES];
    int rc1;
    int rc2;
    int rc3;
    int got;

    if (!have_peer) {
        conf_skip("C-QUEUE.6", "host: no runner to withhold a completion");
        conf_skip("C-QUEUE.7", "host: no runner to withhold a completion");
        return;
    }
    conf_begin(&t6, "C-QUEUE.6");
    rc1 = stat_call("conf-late-1"); /* the client withholds the reply: 30 s */
    rc2 = stat_call("conf-late-2"); /* the storage is still borrowed */
    conf_check(&t6, rc1 == AAR_E_TIMEDOUT, "the withheld request did not return AAR_E_TIMEDOUT");
    conf_check(&t6, rc2 == AAR_E_BUSY,
               "a call that needs the storage of a timed-out request did not return AAR_E_BUSY");
    conf_buf_str(&t6.note, "timeout ");
    conf_buf_i64(&t6.note, rc1);
    conf_buf_str(&t6.note, ", next call ");
    conf_buf_i64(&t6.note, rc2);
    conf_end(&t6);

    mark('L', 0);                  /* the runner now sends the late completion */
    got = cmio_recv(m, 15000000u); /* and says so */
    conf_begin(&t7, "C-QUEUE.7");
    conf_check(&t7, got == 1, "the runner did not say that it had sent the late completion");
    rc3 = stat_call("conf-late-3");
    conf_check(&t7, rc3 == AAR_E_NOENT,
               "after the late completion the storage could not be used again");
    conf_buf_str(&t7.note, "the next call returned ");
    conf_buf_i64(&t7.note, rc3);
    conf_end(&t7);
}

/* ---- The app of the runner's manual runs ----------------------------------------------------- */

static void variant_park(void)
{
    static const char park_log[] = "conf-queue: a log record before a parked call";
    uint32_t n = 0;
    int rc;

    mark('1', 0);
    rc = stat_call("conf-q-1");
    mark('2', rc);
    mark('3', 0);
    rc = stat_call("conf-q-2");
    mark('4', rc);
    (void)aar_console_write("q\n", 2, &n); /* a request that need not park the app */
    mark('5', 0);
    rc = stat_call("conf-q-3");
    mark('6', rc);
    aar_log(park_log, (uint32_t)sizeof park_log - 1u);
    mark('7', 0);
    rc = stat_call("conf-q-4");
    mark('8', rc);
}

static void variant_sqfull(void)
{
    aar_info info;
    uint8_t m[AAR_MSG_BYTES];
    uint32_t cap = 63u;
    uint32_t i;

    info.size = sizeof info;
    if (aar_get_info(&info) == AAR_OK && info.msg_capacity)
        cap = info.msg_capacity;
    for (i = 0; i < 4u * cap + 8u; i++) {
        cm_fill(m, CM_MODE_SEQ, CM_DIR_TO_CLIENT, i);
        if (aar_msg_send(m) != AAR_OK)
            break;
    }
    /* A message is refused one entry early (the reserve for a request). A log record may take
     * the last entry, and the next post finds the queue full: that is the SQ-full event the
     * counter counts [spec question 55]. */
    for (i = 0; i < 4u; i++)
        aar_log("conf-queue sq full", 18u);
    while (!aar_should_stop())
        (void)aar_msg_wait(1000000u);
}

static void variant_spin(void)
{
    volatile uint32_t sink = 0;
    uint32_t acc = 1;
    uint32_t n = 0;
    uint64_t t0 = aar_time_us();
    uint8_t m[AAR_MSG_BYTES];
    uint32_t i;

    while (!aar_should_stop()) {
        for (i = 0; i < 2000u; i++)
            acc = acc * 1664525u + 1013904223u;
        n++;
    }
    sink = acc;
    (void)sink;
    for (i = 0; i < 32u; i++)
        m[i] = 0;
    m[0] = 'S';
    m[1] = 'P';
    m[2] = 'I';
    m[3] = 'N';
    cm_put32(m, 4, n);
    cm_put32(m, 8, (uint32_t)((aar_time_us() - t0) / 1000u));
    (void)cmio_send(m, 2000000u);
}

static void variant_wait(void)
{
    while (!aar_should_stop())
        (void)aar_msg_wait(5000000u);
}

int aar_main(int argc, char **argv)
{
    have_peer = conf_have_peer(argc, argv);
    variant = conf_arg(argc, argv, "conf:variant");

    if (variant && cmio_streq(variant, "park")) {
        variant_park();
    } else if (variant && cmio_streq(variant, "sqfull")) {
        variant_sqfull();
    } else if (variant && cmio_streq(variant, "spin")) {
        variant_spin();
    } else if (variant && cmio_streq(variant, "wait")) {
        variant_wait();
    } else {
        if (WANT("std")) {
            test_log_flood();
            test_slow_serving();
        }
        if (WANT("late"))
            test_late_completion();
    }
    return conf_finish("C-QUEUE");
}
