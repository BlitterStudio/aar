/*
 * Runner side of C-QUEUE (conformance/TESTS.md): the queue pair as a client sees it,
 * and the many-apps rows (admission, release, weights, parked apps).
 *
 * Three kinds of work live here:
 *   - the standard run of the payload (variant std, served by aar_serve) with the
 *     keywords of payloads/conf_queue.h: hold the loop, report the log counters, serve
 *     only on a slow timer, check the console text (C-QUEUE.9 and C-QUEUE.10);
 *   - manual runs (r_mq.c): the runner opens, loads and starts the payload itself and
 *     serves it with AARServiceNext and AARServiceReply, so that it can withhold,
 *     delay and misaddress completions (C-QUEUE.2 to C-QUEUE.8, C-QUEUE.11, and the
 *     late completion of C-QUEUE.6 and C-QUEUE.7, whose lines it relays);
 *   - tests that open claims or run several apps (C-QUEUE.12 to C-QUEUE.16).
 *
 * Assumptions for the card run: a console write need not park the app (if it does the
 * rule of C-QUEUE.5 holds trivially and the detail says so); AARPostEvent accepts a key
 * event while the app is parked in a file call; the file wait of an app is 30 s (the
 * runner waits up to 45 s for the app's report).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "r_mq.h"
#include "../payloads/conf_queue.h"

/* ---- The standard run: keywords --------------------------------------------------------- */

struct q_state {
    ULONG slow_ticks;
    int check_pending;
    ULONG check_n, check_deadline;
    ULONG scan_pos, next_line, errors;
};

static struct q_state qs;

static void send_result(struct conf_run *run, ULONG ok, ULONG count, ULONG errors)
{
    AARMessage am;
    int tries;

    cm_make_result(am.bytes, ok, count, errors, CM_NONE, 0, 0);
    for (tries = 0; tries < 100; tries++) {
        if (AARSendMessage(run->h, &am) != AAR_E_BUSY)
            return;
        Delay(1);
    }
}

static void peer_hold(struct conf_run *run, const AARMessage *msg)
{
    (void)run;
    Delay((LONG)mq_ticks(cm_get32(msg->bytes, 20)));
}

static void peer_logstat(struct conf_run *run, const AARMessage *msg)
{
    AARStatusInfo st;

    (void)msg;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    (void)AARStatus(run->h, &st);
    send_result(run, 1, run->logs, st.log_dropped);
}

static void peer_slow(struct conf_run *run, const AARMessage *msg)
{
    ULONG ms = cm_get32(msg->bytes, 20);

    (void)run;
    qs.slow_ticks = ms ? mq_ticks(ms) : 0UL;
}

static void peer_check(struct conf_run *run, const AARMessage *msg)
{
    (void)run;
    qs.check_n = cm_get32(msg->bytes, 20);
    qs.check_pending = 1;
    qs.check_deadline = conf_ticks() + 20UL * CONF_TICKS_PER_SECOND;
}

/* Reads the lines the console has delivered since the last pass: "Q10 nnnnn". */
static void scan_console(struct conf_run *run, ULONG now)
{
    const char *buf = (const char *)run->cap.buf;

    while (qs.scan_pos < run->cap.len) {
        const char *line = buf + qs.scan_pos;
        const char *nl = memchr(line, '\n', run->cap.len - qs.scan_pos);
        ULONG n = 0;
        int i;

        if (!nl)
            break;
        qs.scan_pos += (ULONG)(nl - line) + 1UL;
        if (strncmp(line, "Q10 ", 4) != 0)
            continue;
        for (i = 4; i < 9 && line[i] >= '0' && line[i] <= '9'; i++)
            n = n * 10UL + (ULONG)(line[i] - '0');
        if (i != 9 || n != qs.next_line)
            qs.errors++;
        else
            qs.next_line++;
    }
    if (qs.next_line >= qs.check_n || (LONG)(now - qs.check_deadline) > 0) {
        send_result(run, qs.next_line == qs.check_n && qs.errors == 0, qs.next_line,
                    qs.errors);
        qs.check_pending = 0;
    }
}

static void q_poll(struct conf_run *run, ULONG now)
{
    if (qs.check_pending)
        scan_console(run, now);
    if (qs.slow_ticks)
        Delay((LONG)qs.slow_ticks); /* serving once per period, whatever wakes the loop */
}

static void prep_std(struct conf_run *run)
{
    memset(&qs, 0, sizeof qs);
    run->poll = q_poll;
    run->timeout_s = 150;
}

static const struct conf_peer peers[] = {
    { CQ_K_HOLD, peer_hold },
    { CQ_K_LOGSTAT, peer_logstat },
    { CQ_K_SLOW, peer_slow },
    { CQ_K_CHECK, peer_check },
};

static const struct conf_variant variants[] = {
    { "std", prep_std },
};

/* ---- Manual runs ------------------------------------------------------------------------------ */

enum { R2, R3, R4, R5, R8, R11, NRES };

struct qres {
    int ok;
    char d[CONF_DETAIL_MAX];
};

static struct qres res[NRES];

static void res_set(int which, int ok, const char *detail)
{
    res[which].ok = ok;
    snprintf(res[which].d, sizeof res[which].d, "%s", detail);
}

static void res_all(const char *detail)
{
    int i;

    for (i = 0; i < NRES; i++)
        res_set(i, 0, detail);
}

static int is_marker(const AARMessage *m, char stage)
{
    return cq_is_marker(m->bytes, stage);
}

/* The app's report after a stage: wait for the marker; other messages are skipped. */
static int wait_marker(struct mq_app *a, char stage, ULONG ms, LONG *value)
{
    ULONG deadline = conf_ticks() + mq_ticks(ms);

    for (;;) {
        AARMessage m;
        ULONG left = (ULONG)((LONG)(deadline - conf_ticks()) > 0 ? deadline - conf_ticks() : 0UL);

        if (!mq_wait_message(a, &m, left * 20UL + 20UL))
            return 0;
        if (is_marker(&m, stage)) {
            if (value)
                *value = (LONG)cm_get32(m.bytes, 4);
            return 1;
        }
        if ((LONG)(conf_ticks() - deadline) >= 0)
            return 0;
    }
}

/* No marker of the stage in ms: the app is still parked. */
static int stays_parked(struct mq_app *a, char stage, ULONG ms)
{
    return !wait_marker(a, stage, ms, NULL);
}

static void stage_park(void)
{
    struct mq_app a;
    AARServiceReq r1, r2, rcon, r3, r4, bad, good;
    AARMessage m;
    aar_event ev;
    LONG value = 0;
    LONG rc_id, rc_op, rc_dup, rc_good;
    int resumed_id, resumed_op, resumed_dup, parked_console;
    char d[CONF_DETAIL_MAX];

    if (mq_start(&a, "C-QUEUE", "park", NULL, 4096) != AAR_OK) {
        conf_detailf(d, "the app did not start: %.80s", a.why);
        res_all(d);
        mq_finish(&a, 3000);
        return;
    }
    if (!wait_marker(&a, '1', 8000, NULL) || !mq_wait_request(&a, &r1, 5000) ||
        r1.op != AAR_OP_FILE_STAT) {
        res_all("the app sent no request at stage 1");
        goto out;
    }
    /* C-QUEUE.3: a reply with a wrong id leaves the app parked. */
    bad = r1;
    bad.id = r1.id + 1000UL;
    bad.result = AAR_E_NOENT;
    rc_id = AARServiceReply(a.h, &bad);
    resumed_id = !stays_parked(&a, '2', 300);
    /* C-QUEUE.4: so does a reply with a wrong op. */
    bad = r1;
    bad.op = AAR_OP_FILE_OPEN;
    bad.result = AAR_E_NOENT;
    rc_op = AARServiceReply(a.h, &bad);
    resumed_op = !stays_parked(&a, '2', 300);
    good = r1;
    good.result = AAR_E_NOENT;
    rc_good = mq_reply(&a, &good);
    if (!wait_marker(&a, '2', 4000, &value) || value != AAR_E_NOENT) {
        conf_detailf(d, "the right reply (%ld) did not resume the app with its result", (long)rc_good);
        res_set(R3, 0, d);
        res_set(R4, 0, d);
        res_set(R2, 0, d);
        goto out;
    }
    conf_detailf(d, "wrong id: reply returned %ld, app %s; right reply resumed it",
                 (long)rc_id, resumed_id ? "RESUMED" : "stayed parked");
    res_set(R3, !resumed_id, d);

    /* C-QUEUE.4, second half: a duplicate of a completed request, while the app is parked
     * on the next one. */
    if (!wait_marker(&a, '3', 4000, NULL) || !mq_wait_request(&a, &r2, 5000) ||
        r2.op != AAR_OP_FILE_STAT) {
        res_set(R4, 0, "the app sent no request at stage 2");
        goto out;
    }
    good = r1;
    good.result = AAR_E_NOENT;
    rc_dup = AARServiceReply(a.h, &good);
    resumed_dup = !stays_parked(&a, '4', 300);
    a.stash_console = 1; /* stage 3 wants the console write unanswered */
    good = r2;
    good.result = AAR_E_NOENT;
    (void)mq_reply(&a, &good);
    if (!wait_marker(&a, '4', 4000, &value) || value != AAR_E_NOENT) {
        res_set(R4, 0, "the right reply did not resume the app after a duplicate");
        goto out;
    }
    conf_detailf(d, "wrong op: reply returned %ld, app %s; duplicate: reply returned %ld, app %s",
                 (long)rc_op, resumed_op ? "RESUMED" : "stayed parked", (long)rc_dup,
                 resumed_dup ? "RESUMED" : "stayed parked");
    res_set(R4, !resumed_op && !resumed_dup, d);

    /* C-QUEUE.5: a reply to a request that did not park the app (a console write) must not
     * resume the app, which is parked on the request behind it. */
    if (!mq_wait_request(&a, &rcon, 4000) || rcon.op != AAR_OP_CONSOLE_WRITE) {
        res_set(R5, 0, "the app sent no console write at stage 3");
        goto out;
    }
    parked_console = stays_parked(&a, '5', 700); /* no marker: the console write parked it */
    mq_answer(&a, &rcon);
    a.stash_console = 0;
    if (parked_console) {
        if (!wait_marker(&a, '5', 4000, NULL) || !mq_wait_request(&a, &r3, 5000)) {
            res_set(R5, 0, "the app did not go on after the console write");
            goto out;
        }
        res_set(R5, 1, "the console write parks the app on this platform: a reply to it "
                       "resumes it, as it must; nothing to misdirect");
    } else {
        /* The app went past the console write and parked on the stat behind it. */
        if (!mq_wait_request(&a, &r3, 5000) || r3.op != AAR_OP_FILE_STAT) {
            res_set(R5, 0, "the app sent no request behind the console write");
            goto out;
        }
        /* The console write was answered above (mq_answer): the app must still wait. */
        if (!stays_parked(&a, '6', 400)) {
            res_set(R5, 0, "the reply to the console write resumed the app parked on another request");
            goto out;
        }
        res_set(R5, 1, "the reply to a console write left the app parked on its stat");
    }
    good = r3;
    good.result = AAR_E_NOENT;
    (void)mq_reply(&a, &good);
    if (!wait_marker(&a, '6', 4000, NULL)) {
        res_set(R5, 0, "the right reply did not resume the app at stage 3");
        goto out;
    }

    /* C-QUEUE.11: a log record and an event while a request is pending. */
    if (!wait_marker(&a, '7', 4000, NULL) || !mq_wait_request(&a, &r4, 5000) ||
        r4.op != AAR_OP_FILE_STAT) {
        res_set(R11, 0, "the app sent no request at stage 4");
        goto out;
    }
    memset(&ev, 0, sizeof ev);
    ev.type = AAR_EV_KEY_DOWN;
    ev.code = 0x45;
    (void)AARPostEvent(a.h, &ev);
    (void)AARPostEvent(a.h, &ev);
    good = r4;
    good.result = AAR_E_NOENT;
    (void)mq_reply(&a, &good);
    if (wait_marker(&a, '8', 4000, &value) && value == AAR_E_NOENT)
        res_set(R11, 1, "a log record before the request and events during it left it intact");
    else
        res_set(R11, 0, "an event or a log record disturbed the pending request");
out:
    /* C-QUEUE.2: AARServiceNext only ever said AAR_OK or AAR_E_BUSY. */
    conf_detailf(d, "AARServiceNext: %lu requests, %lu times AAR_E_BUSY, %lu other (last %ld)",
                 (unsigned long)a.seen_ok, (unsigned long)a.seen_busy,
                 (unsigned long)a.seen_other, (long)a.other_rc);
    res_set(R2, a.seen_ok >= 4 && a.seen_busy >= 1 && a.seen_other == 0, d);
    (void)m;
    mq_finish(&a, 5000);
}

/* C-QUEUE.8: nobody serves, the app floods messages until its send is refused. */
static void stage_sqfull(void)
{
    struct mq_app a;
    AARStatusInfo st;
    AARMessage m;
    ULONG deadline;
    char d[CONF_DETAIL_MAX];

    if (mq_start(&a, "C-QUEUE", "sqfull", NULL, 0) != AAR_OK) {
        conf_detailf(d, "the app did not start: %.80s", a.why);
        res_set(R8, 0, d);
        mq_finish(&a, 3000);
        return;
    }
    memset(&st, 0, sizeof st);
    deadline = conf_ticks() + mq_ticks(6000);
    do {
        mq_sleep_ms(100);
        st.size = sizeof st;
        (void)AARStatus(a.h, &st);
    } while (st.sq_full == 0 && (LONG)(conf_ticks() - deadline) < 0);
    conf_detailf(d, "sq_full is %lu after the app's send was refused", (unsigned long)st.sq_full);
    res_set(R8, st.sq_full > 0, d);
    while (AARGetMessage(a.h, &m) == AAR_OK)
        ;
    mq_finish(&a, 5000);
}

/* C-QUEUE.6 and C-QUEUE.7: the client withholds a reply past the app's file timeout, then
 * sends it late. The payload prints the ids; the lines are relayed from the console text. */
static void stage_late(void)
{
    struct mq_app a;
    struct conf_run run;
    struct conf_parsed pr;
    AARServiceReq r1, r3, good;
    AARMessage m;
    AARMessage go;
    ULONG deadline;
    int phase = 0;

    if (mq_start(&a, "C-QUEUE", "late", NULL, 16384) != AAR_OK) {
        conf_emitf("# C-QUEUE late completion: the app did not start: %.80s", a.why);
        mq_finish(&a, 3000);
        return;
    }
    if (!mq_wait_request(&a, &r1, 10000) || r1.op != AAR_OP_FILE_STAT) {
        conf_emitf("# C-QUEUE late completion: the app sent no request");
        goto out;
    }
    phase = 1;
    /* Withheld: the app's own 30 s timeout runs out; it reports when its next call was refused. */
    if (!wait_marker(&a, 'L', 45000, NULL)) {
        conf_emitf("# C-QUEUE late completion: the app did not report in 45 s");
        goto out;
    }
    phase = 2;
    good = r1;
    good.result = AAR_E_NOENT;
    (void)mq_reply(&a, &good); /* the late completion */
    memset(&go, 0, sizeof go);
    go.bytes[0] = 'G';
    go.bytes[1] = 'O';
    (void)AARSendMessage(a.h, &go);
    if (!mq_wait_request(&a, &r3, 8000) || r3.op != AAR_OP_FILE_STAT) {
        conf_emitf("# C-QUEUE late completion: the app sent no request after the late completion");
        goto out;
    }
    phase = 3;
    good = r3;
    good.result = AAR_E_NOENT;
    (void)mq_reply(&a, &good);
    deadline = conf_ticks() + mq_ticks(8000);
    while (!mq_ended(&a) && (LONG)(conf_ticks() - deadline) < 0)
        mq_serve_for(&a, 100);
out:
    (void)m;
    mq_serve_for(&a, 200);
    conf_run_init(&run, "C-QUEUE");
    run.cap.buf = a.text;
    run.cap.len = a.text_len;
    conf_parse_capture(&run, &pr);
    run.cap.buf = NULL;
    if (!pr.have_end)
        conf_emitf("# C-QUEUE late completion: the app printed no end line (reached phase %d)",
                   phase);
    mq_finish(&a, 5000);
}

static void t_manual(int idx)
{
    int i;

    (void)idx;
    res_all("the manual run was not done");
    stage_park();
    stage_sqfull();
    stage_late();
    conf_record("C-QUEUE.2", res[R2].ok, res[R2].d);
    conf_record("C-QUEUE.3", res[R3].ok, res[R3].d);
    conf_record("C-QUEUE.4", res[R4].ok, res[R4].d);
    conf_record("C-QUEUE.5", res[R5].ok, res[R5].d);
    conf_record("C-QUEUE.8", res[R8].ok, res[R8].d);
    conf_record("C-QUEUE.11", res[R11].ok, res[R11].d);
    for (i = 0; i < NRES; i++)
        res[i].ok = 0;
}

/* ---- Admission and release (C-QUEUE.12 to C-QUEUE.14) -------------------------------------- */

#define ADM_CAP 256UL

/* Opens claims until one is refused or limit are open. Returns the count; *rc and *reason
 * are the refusal's (AAR_OK when none came). */
static ULONG fill_claims(AARHandle **hs, ULONG limit, LONG *rc, ULONG *reason)
{
    ULONG n;

    *rc = AAR_OK;
    *reason = 0;
    for (n = 0; n < limit; n++) {
        *rc = AAROpen(&hs[n], NULL, reason);
        if (*rc != AAR_OK) {
            hs[n] = NULL;
            break;
        }
    }
    return n;
}

static void close_claims(AARHandle **hs, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++) {
        if (hs[i])
            (void)AARClose(hs[i]);
        hs[i] = NULL;
    }
}

static void t_admission(int idx)
{
    AARHandle **hs;
    ULONG limit = ADM_CAP;
    ULONG n1, n2, n3, n4, k, extra = 0;
    ULONG reason = 0;
    LONG rc;
    int capped;
    int ceiling;
    int ok12, ok13, ok14;
    char d12[CONF_DETAIL_MAX], d13[CONF_DETAIL_MAX], d14[CONF_DETAIL_MAX];

    (void)idx;
    hs = AllocVec((ADM_CAP + 120UL) * sizeof(AARHandle *), MEMF_ANY | MEMF_CLEAR);
    if (!hs) {
        conf_record("C-QUEUE.12", 0, "no memory for the claim table");
        conf_record("C-QUEUE.13", 0, "no memory for the claim table");
        conf_record("C-QUEUE.14", 0, "no memory for the claim table");
        return;
    }
    ceiling = conf_g.info.max_apps != 0 && conf_g.info.max_apps < ADM_CAP;
    if (ceiling)
        limit = conf_g.info.max_apps + 1UL;

    /* Run A: open to the limit, close all, open again. */
    n1 = fill_claims(hs, limit, &rc, &reason);
    capped = rc == AAR_OK;
    if (ceiling) {
        ok12 = !capped && rc == AAR_E_NORESOURCE && reason >= AAR_REASON_MEMORY &&
               reason <= AAR_REASON_LIMIT && n1 <= conf_g.info.max_apps;
        conf_detailf(d12, "%lu opened, then %ld reason %lu; platform ceiling %lu",
                     (unsigned long)n1, (long)rc, (unsigned long)reason,
                     (unsigned long)conf_g.info.max_apps);
    } else if (capped) {
        ok12 = 1;
        conf_detailf(d12, "no refusal in %lu opens (the test stops there); max_apps is 0",
                     (unsigned long)n1);
    } else {
        ok12 = rc == AAR_E_NORESOURCE && reason >= AAR_REASON_MEMORY && reason <= AAR_REASON_LIMIT;
        conf_detailf(d12, "%lu opened, then %ld reason %lu", (unsigned long)n1, (long)rc,
                     (unsigned long)reason);
    }
    close_claims(hs, n1);
    n2 = fill_claims(hs, limit, &rc, &reason);
    close_claims(hs, n2);
    ok14 = n1 == n2;
    conf_detailf(d14, "%lu claims, all closed, then %lu again%s", (unsigned long)n1,
                 (unsigned long)n2, capped ? " (test limit)" : "");

    /* Run B: open to the limit, 100 more refused opens, close all, open again. */
    n3 = fill_claims(hs, limit, &rc, &reason);
    ok13 = 1;
    if (rc != AAR_OK) {
        for (k = 0; k < 100UL; k++) {
            AARHandle *h = NULL;
            ULONG r2 = 0;
            LONG rc2 = AAROpen(&h, NULL, &r2);

            if (rc2 == AAR_OK) {
                /* Something was freed meanwhile: keep it and count it. */
                if (n3 + extra < ADM_CAP + 120UL)
                    hs[n3 + extra++] = h;
                else
                    (void)AARClose(h);
            } else if (rc2 != AAR_E_NORESOURCE) {
                ok13 = 0;
            }
        }
    }
    close_claims(hs, n3 + extra);
    n4 = fill_claims(hs, limit, &rc, &reason);
    close_claims(hs, n4);
    if (n3 + extra != n4)
        ok13 = 0;
    conf_detailf(d13, "%lu claims, 100 refused opens (%lu got through), all closed, then %lu again",
                 (unsigned long)n3, (unsigned long)extra, (unsigned long)n4);
    FreeVec(hs);
    conf_record("C-QUEUE.12", ok12, d12);
    conf_record("C-QUEUE.13", ok13, d13);
    conf_record("C-QUEUE.14", ok14, d14);
}

/* ---- Sharing the CPU (C-QUEUE.15, C-QUEUE.16) ---------------------------------------------------- */

/* Stops a spin app and reads what it reports: loops per second times 10 (so that a slow app
 * still has digits). 0 when it reported nothing. */
static ULONG spin_rate(struct mq_app *a)
{
    AARMessage m;
    int k;

    (void)AARStop(a->h);
    for (k = 0; k < 3; k++) {
        if (!mq_wait_message(a, &m, 10000))
            return 0;
        if (cq_is_spin(m.bytes)) {
            ULONG n = cm_get32(m.bytes, 4);
            ULONG ms = cm_get32(m.bytes, 8);

            if (ms == 0)
                return 0;
            return (ULONG)((unsigned long long)n * 10000ULL / ms);
        }
    }
    return 0;
}

static void t_weights(int idx)
{
    struct mq_app a, b;
    struct TagItem t1[2], tw[2];
    ULONG w = 3;
    ULONG ra, rb;
    LONG rc;
    char d[CONF_DETAIL_MAX];
    ULONG ratio;

    if (conf_g.info.weight_max == 0) {
        conf_set_result(idx, CONF_R_SKIP, "weight_max is 0: the platform has no weighting");
        return;
    }
    if (conf_g.info.weight_max < w)
        w = conf_g.info.weight_max;
    if (w < 2) {
        conf_set_result(idx, CONF_R_SKIP, "weight_max is 1: there is nothing to weigh");
        return;
    }
    t1[0].ti_Tag = AAR_TAG_WEIGHT;
    t1[0].ti_Data = 1;
    t1[1].ti_Tag = TAG_DONE;
    t1[1].ti_Data = 0;
    tw[0].ti_Tag = AAR_TAG_WEIGHT;
    tw[0].ti_Data = w;
    tw[1].ti_Tag = TAG_DONE;
    tw[1].ti_Data = 0;
    rc = mq_start(&a, "C-QUEUE", "spin", t1, 0);
    if (rc != AAR_OK) {
        if (rc == AAR_E_NOSYS || rc == AAR_E_INVAL)
            conf_detailf(d, "AAR_TAG_WEIGHT is not accepted: %ld", (long)rc);
        else
            conf_detailf(d, "the app did not start: %.80s", a.why);
        conf_set_result(idx, rc == AAR_E_NOSYS || rc == AAR_E_INVAL ? CONF_R_SKIP : CONF_R_FAIL, d);
        mq_finish(&a, 3000);
        return;
    }
    rc = mq_start(&b, "C-QUEUE", "spin", tw, 0);
    if (rc != AAR_OK) {
        conf_detailf(d, "the second app did not start: %.80s", b.why);
        conf_set_result(idx, CONF_R_FAIL, d);
        mq_finish(&b, 3000);
        mq_finish(&a, 3000);
        return;
    }
    mq_serve_for(&a, 4000);
    ra = spin_rate(&a);
    rb = spin_rate(&b);
    mq_finish(&b, 5000);
    mq_finish(&a, 5000);
    if (ra == 0 || rb == 0) {
        conf_set_result(idx, CONF_R_FAIL, "a spin app did not report its work");
        return;
    }
    ratio = (ULONG)((unsigned long long)rb * 100ULL / ra);
    conf_detailf(d, "weights 1 and %lu: work ratio %lu.%02lu (within 30 percent of %lu)",
                 (unsigned long)w, (unsigned long)(ratio / 100UL),
                 (unsigned long)(ratio % 100UL), (unsigned long)w);
    conf_set_result(idx, ratio >= w * 70UL && ratio <= w * 130UL ? CONF_R_PASS : CONF_R_FAIL, d);
}

static void t_parked(int idx)
{
    struct mq_app a, p, b;
    ULONG alone, beside;
    char d[CONF_DETAIL_MAX];

    if (mq_start(&a, "C-QUEUE", "spin", NULL, 0) != AAR_OK) {
        conf_detailf(d, "the app did not start: %.80s", a.why);
        conf_set_result(idx, CONF_R_FAIL, d);
        mq_finish(&a, 3000);
        return;
    }
    mq_serve_for(&a, 3000);
    alone = spin_rate(&a);
    mq_finish(&a, 5000);
    if (mq_start(&p, "C-QUEUE", "wait", NULL, 0) != AAR_OK) {
        conf_detailf(d, "the parked app did not start: %.80s", p.why);
        conf_set_result(idx, CONF_R_FAIL, d);
        mq_finish(&p, 3000);
        return;
    }
    mq_serve_for(&p, 500); /* it is parked by now */
    if (mq_start(&b, "C-QUEUE", "spin", NULL, 0) != AAR_OK) {
        conf_detailf(d, "the app did not start beside the parked one: %.80s", b.why);
        conf_set_result(idx, CONF_R_FAIL, d);
        mq_finish(&b, 3000);
        mq_finish(&p, 3000);
        return;
    }
    mq_serve_for(&b, 3000);
    beside = spin_rate(&b);
    mq_finish(&b, 5000);
    mq_finish(&p, 5000);
    if (alone == 0 || beside == 0) {
        conf_set_result(idx, CONF_R_FAIL, "a spin app did not report its work");
        return;
    }
    conf_detailf(d, "compute-bound app: %lu alone, %lu beside a parked app (%lu percent)",
                 (unsigned long)alone, (unsigned long)beside,
                 (unsigned long)((unsigned long long)beside * 100ULL / alone));
    conf_set_result(idx, (unsigned long long)beside * 100ULL >= (unsigned long long)alone * 90ULL
                             ? CONF_R_PASS : CONF_R_FAIL, d);
}

/* C-QUEUE.1 */
static void t_depth(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG depth = conf_g.info.queue_depth;
    int ok = depth >= 64UL && depth <= 4096UL && (depth & (depth - 1UL)) == 0;

    conf_detailf(d, "queue depth %lu", (unsigned long)depth);
    conf_set_result(idx, ok ? CONF_R_PASS : CONF_R_FAIL, d);
}

static const struct conf_r_test tests[] = {
    { "C-QUEUE.1", t_depth },
    { "C-QUEUE.2", t_manual },
    { "C-QUEUE.3", t_manual },
    { "C-QUEUE.4", t_manual },
    { "C-QUEUE.5", t_manual },
    { "C-QUEUE.8", t_manual },
    { "C-QUEUE.11", t_manual },
    { "C-QUEUE.12", t_admission },
    { "C-QUEUE.13", t_admission },
    { "C-QUEUE.14", t_admission },
    { "C-QUEUE.15", t_weights },
    { "C-QUEUE.16", t_parked },
};

const struct conf_area conf_area_queue = {
    .name = "C-QUEUE",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
};
