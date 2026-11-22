/*
 * Runner side of C-MSG (conformance/TESTS.md): the peer of the conf_msg payload and
 * the runner-judged rows C-MSG.5, C-MSG.7, C-MSG.14 and C-MSG.15.
 *
 * The messages are drained by this file, not by aar_serve (AAR_SERVEF_KEEP_MESSAGES):
 * the tests that need a client that does not drain (C-MSG.4, C-MSG.16, C-MSG.14) hold
 * the drain back, and the tests that need a slow receiver (C-MSG.17) limit it. The
 * payload talks to this file with the keywords of payloads/conf_msg.h; the keyword
 * dispatch is done here in the poll callback, so the core's own peer hook does not
 * see these runs (CONF-STOP-NOW is not used by them).
 *
 * Assumptions for the card run: aar_serve_step serves requests (the payload's console
 * lines) while this file does not call AARGetMessage; the library's inbox holds
 * msg_capacity messages and a request behind a message it cannot take waits.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "../payloads/conf_msg.h"

#define OUTQ 16u

static ULONG ms_ticks(ULONG ms)
{
    return (ms * CONF_TICKS_PER_SECOND + 999UL) / 1000UL;
}

struct msg_state {
    char variant[16];
    ULONG cap;
    /* from the app: counted since the last BEGIN */
    ULONG rx_mode, rx_count, rx_errors, rx_first, rx_limit;
    ULONG hold_until;
    int hold_forever;
    /* to the app */
    int tx_active;
    ULONG tx_mode, tx_total, tx_next, tx_limit;
    UBYTE outq[OUTQ][AAR_MSG_BYTES];
    unsigned out_count;
    /* a stop asked for later */
    int stop_pending;
    ULONG stop_at;
    /* inbox variant (C-MSG.5, C-MSG.7) */
    int fill_done;
    ULONG fill_accepted;
    LONG empty_rc;
    int got_seen;
    ULONG got_count, got_ok;
    /* exitmsgs variant (C-MSG.14) */
    int exit_done;
    ULONG exit_count, exit_bad;
    ULONG now;
};

static struct msg_state st;

/* ---- Sending to the app -------------------------------------------------------- */

static void send_msg(struct conf_run *run, const UBYTE *m)
{
    AARMessage am;

    memcpy(am.bytes, m, AAR_MSG_BYTES);
    if (st.out_count == 0 && AARSendMessage(run->h, &am) == AAR_OK)
        return;
    if (st.out_count < OUTQ) {
        memcpy(st.outq[st.out_count], m, AAR_MSG_BYTES);
        st.out_count++;
    } else {
        conf_emitf("# C-MSG: the reply queue is full, a reply was lost");
        conf_g.suite_notes++;
    }
}

static void flush_replies(struct conf_run *run)
{
    while (st.out_count) {
        AARMessage am;

        memcpy(am.bytes, st.outq[0], AAR_MSG_BYTES);
        if (AARSendMessage(run->h, &am) != AAR_OK)
            break;
        st.out_count--;
        memmove(st.outq[0], st.outq[1], st.out_count * AAR_MSG_BYTES);
    }
}

static void pump_tx(struct conf_run *run)
{
    ULONG sent = 0;

    while (st.tx_active && sent < st.tx_limit) {
        AARMessage am;

        cm_fill(am.bytes, st.tx_mode, CM_DIR_TO_APP, st.tx_next);
        if (AARSendMessage(run->h, &am) != AAR_OK)
            break;
        st.tx_next++;
        sent++;
        if (st.tx_next >= st.tx_total)
            st.tx_active = 0;
    }
}

/* ---- Keywords and data ------------------------------------------------------------ */

static int is_kw(const UBYTE *m, const char *kw)
{
    return memcmp(m, kw, strlen(kw) + 1) == 0;
}

static void on_data(const UBYTE *m)
{
    if (!cm_check(m, st.rx_mode, CM_DIR_TO_CLIENT, st.rx_count)) {
        st.rx_errors++;
        if (st.rx_first == CM_NONE)
            st.rx_first = st.rx_count;
    }
    st.rx_count++;
}

static void dispatch(struct conf_run *run, const UBYTE *m)
{
    ULONG a = cm_get32(m, 20);
    ULONG b = cm_get32(m, 24);
    ULONG c = cm_get32(m, 28);
    UBYTE r[AAR_MSG_BYTES];

    if (is_kw(m, CM_K_BEGIN)) {
        st.rx_mode = a;
        st.rx_count = 0;
        st.rx_errors = 0;
        st.rx_first = CM_NONE;
        st.rx_limit = c;
        st.hold_until = b ? st.now + ms_ticks(b) : st.now;
    } else if (is_kw(m, CM_K_END)) {
        cm_make_result(r, st.rx_count == a && st.rx_errors == 0, st.rx_count,
                       st.rx_errors, st.rx_first, 0, 0);
        send_msg(run, r);
        st.rx_limit = 0;
    } else if (is_kw(m, CM_K_SEND)) {
        st.tx_active = b != 0;
        st.tx_mode = a;
        st.tx_total = b;
        st.tx_next = 0;
        st.tx_limit = c ? c : 0x7FFFFFFFUL;
    } else if (is_kw(m, CM_K_ECHO)) {
        send_msg(run, m);
    } else if (is_kw(m, CM_K_STOPWAIT)) {
        st.stop_pending = 1;
        st.stop_at = st.now + ms_ticks(a);
    } else if (is_kw(m, CM_K_STOPMSG)) {
        cm_fill(r, CM_MODE_SEQ, CM_DIR_TO_APP, 0);
        send_msg(run, r);
        conf_run_stop(run);
    } else if (is_kw(m, CM_K_FILL)) {
        AARMessage probe;
        ULONG i;

        /* The app sleeps and has sent nothing: no message is queued (C-MSG.7). */
        st.empty_rc = AARGetMessage(run->h, &probe);
        st.fill_accepted = 0;
        for (i = 0; i < 4UL * st.cap + 8UL; i++) {
            AARMessage am;

            cm_fill(am.bytes, CM_MODE_SEQ, CM_DIR_TO_APP, i);
            if (AARSendMessage(run->h, &am) != AAR_OK)
                break;
            st.fill_accepted++;
        }
        st.fill_done = 1;
    } else if (is_kw(m, CM_K_GOT)) {
        st.got_seen = 1;
        st.got_count = a;
        st.got_ok = b;
    } else if (m[0] != 'C') {
        on_data(m);
    } else {
        conf_emitf("# C-MSG: unknown keyword message %.19s", (const char *)m);
    }
}

static void poll(struct conf_run *run, ULONG now)
{
    ULONG n = 0;

    st.now = now;
    if (st.stop_pending && (LONG)(now - st.stop_at) >= 0) {
        st.stop_pending = 0;
        conf_run_stop(run);
    }
    flush_replies(run);
    pump_tx(run);
    if (st.hold_forever)
        return;
    while ((LONG)(now - st.hold_until) >= 0 && (st.rx_limit == 0 || n < st.rx_limit)) {
        AARMessage am;

        if (AARGetMessage(run->h, &am) != AAR_OK)
            break;
        n++;
        dispatch(run, am.bytes);
    }
}

/* C-MSG.14: after the app exited, its messages are still readable. */
static void finished(struct conf_run *run)
{
    AARMessage am;
    ULONG n = 0;
    ULONG bad = 0;

    if (strcmp(st.variant, "exitmsgs") != 0)
        return;
    while (n < 4UL * st.cap + 8UL && AARGetMessage(run->h, &am) == AAR_OK) {
        if (!cm_check(am.bytes, CM_MODE_SEQ, CM_DIR_TO_CLIENT, n))
            bad++;
        n++;
    }
    st.exit_count = n;
    st.exit_bad = bad;
    st.exit_done = 1;
}

/* ---- Variants ------------------------------------------------------------------------ */

static void reset_state(struct conf_run *run, const char *name)
{
    memset(&st, 0, sizeof st);
    snprintf(st.variant, sizeof st.variant, "%s", name);
    st.cap = conf_g.info.msg_capacity ? conf_g.info.msg_capacity : AAR_MSG_CAPACITY;
    st.rx_first = CM_NONE;
    st.hold_until = conf_ticks();
    st.empty_rc = AAR_OK;
    run->serve_flags |= AAR_SERVEF_KEEP_MESSAGES;
    run->poll = poll;
    run->on_finished = finished;
}

static void prep_std(struct conf_run *run)
{
    reset_state(run, "std");
    run->timeout_s = 120;
}

static void prep_hold(struct conf_run *run)
{
    reset_state(run, "hold");
}

static void prep_stopwait(struct conf_run *run)
{
    reset_state(run, "stopwait");
}

static void prep_stopmsg(struct conf_run *run)
{
    reset_state(run, "stopmsg");
}

static void prep_park(struct conf_run *run)
{
    reset_state(run, "park");
    run->timeout_s = 60;
}

static void prep_inbox(struct conf_run *run)
{
    reset_state(run, "inbox");
}

static void prep_exitmsgs(struct conf_run *run)
{
    reset_state(run, "exitmsgs");
    st.hold_forever = 1; /* the client reads nothing until the app has exited */
}

static const struct conf_variant variants[] = {
    { "std", prep_std },
    { "hold", prep_hold },
    { "stopwait", prep_stopwait },
    { "stopmsg", prep_stopmsg },
    { "park", prep_park },
    { "inbox", prep_inbox },
    { "exitmsgs", prep_exitmsgs },
};

/* ---- Rows the runner judges ------------------------------------------------------------- */

static void on_end(const struct conf_run *run, const struct conf_outcome *out,
                   const struct conf_parsed *pr)
{
    char d[CONF_DETAIL_MAX];
    int ok;

    (void)pr;
    if (strcmp(run->area, "C-MSG") != 0 || !run->variant)
        return;
    if (strcmp(run->variant, "inbox") == 0) {
        /* C-MSG.7: nothing from the app was queued, AARGetMessage said BUSY. */
        ok = st.fill_done && st.empty_rc == AAR_E_BUSY;
        conf_detailf(d, "AARGetMessage with nothing queued returned %ld",
                     (long)st.empty_rc);
        if (!st.fill_done)
            conf_detailf(d, "the app never asked for the inbox test (%s)",
                         out->ran ? "no keyword" : out->why);
        conf_record("C-MSG.7", ok, d);
        /* C-MSG.5: BUSY only after msg_capacity messages, all of them polled intact. */
        ok = st.fill_done && st.fill_accepted >= st.cap && st.got_seen &&
             st.got_count == st.fill_accepted && st.got_ok;
        if (!st.fill_done || !st.got_seen)
            conf_detailf(d, "the inbox test did not complete (%s)",
                         st.fill_done ? "the app did not report" : "no request");
        else
            conf_detailf(d, "%lu accepted (capacity %lu), the app polled %lu, intact %lu",
                         (unsigned long)st.fill_accepted, (unsigned long)st.cap,
                         (unsigned long)st.got_count, (unsigned long)st.got_ok);
        conf_record("C-MSG.5", ok, d);
    } else if (strcmp(run->variant, "exitmsgs") == 0) {
        ok = st.exit_done && st.exit_count == st.cap && st.exit_bad == 0;
        if (st.exit_done)
            conf_detailf(d, "%lu of %lu messages readable after the exit, %lu wrong",
                         (unsigned long)st.exit_count, (unsigned long)st.cap,
                         (unsigned long)st.exit_bad);
        else
            conf_detailf(d, "the app did not run to its end (%s)", out->why);
        conf_record("C-MSG.14", ok, d);
    }
}

/* C-MSG.15 */
static void t_capacity(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG depth = conf_g.info.queue_depth;
    ULONG capacity = conf_g.info.msg_capacity;
    int ok = depth >= 2UL && capacity == depth - AAR_MSG_RESERVE &&
             (depth != AAR_QUEUE_DEPTH_DEFAULT || capacity == AAR_MSG_CAPACITY);

    conf_detailf(d, "queue depth %lu, msg capacity %lu", (unsigned long)depth,
                 (unsigned long)capacity);
    conf_set_result(idx, ok ? CONF_R_PASS : CONF_R_FAIL, d);
}

static const struct conf_r_test tests[] = {
    { "C-MSG.15", t_capacity },
};

const struct conf_area conf_area_msg = {
    .name = "C-MSG",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
    .on_end = on_end,
};
