/*
 * Runner side of C-LIFE (conformance/TESTS.md): lifecycle. This file belongs to
 * the C-LIFE author.
 *
 * C-LIFE.1 to C-LIFE.3 are judged on every payload run of every area, through the
 * observers below: the claim state after AAROpen, the end state, and the exit
 * code a payload returned.
 *
 * The other tests start the app conf_life (payloads/conf_life.c, one source with
 * variants, see conf_life.h) and observe it from outside: the end state, exit
 * code and fault report, the timing of a stop, a long computation, a long sleep,
 * a client that blocks, a client task that dies or closes the library without
 * AARClose. A test either runs the app through conf_run_payload (a serving
 * client, with a poll callback that acts at a time) or starts it by hand
 * (AAROpen, AARLoadFile, AARStart, no serving) when it needs to control the claim.
 * Tests that share one run (the fault report, the stop) run it once and record
 * every id they judge.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libraries/aar_zz9000.h>

#include "conf_util.h"
#include "../payloads/conf_life.h"

/* The first FAIL of a test stays, so every run is judged and one bad run shows. */
static void life_note(const char *id, int ok, const char *detail)
{
    conf_record(id, ok, detail);
}

/* C-LIFE.1 */
static void on_open(const struct conf_run *run, LONG rc, const AARStatusInfo *st)
{
    char d[CONF_DETAIL_MAX];

    (void)run;
    if (rc == AAR_OK && st->state == AAR_STATE_CLAIMED && st->generation != 0) {
        conf_detailf(d, "claimed, generation %lu", (unsigned long)st->generation);
        life_note("C-LIFE.1", 1, d);
    } else {
        conf_detailf(d, "after open: status %ld, state %lu, generation %lu", (long)rc,
                     (unsigned long)st->state, (unsigned long)st->generation);
        life_note("C-LIFE.1", 0, d);
    }
}

/* C-LIFE.2 and C-LIFE.3 */
static void on_end(const struct conf_run *run, const struct conf_outcome *out,
                   const struct conf_parsed *pr)
{
    char d[CONF_DETAIL_MAX];
    const char *name = run->area;

    if (!out->ran) {
        conf_detailf(d, "%s did not run: %.80s", name, out->why);
        life_note("C-LIFE.2", 0, d);
        life_note("C-LIFE.3", 0, d);
        return;
    }
    if (out->status_rc == AAR_OK && out->state == AAR_STATE_IDLE &&
        out->end_reason == AAR_END_EXIT && out->fault_class == AAR_FAULT_NONE) {
        conf_detailf(d, "%s ended: idle, exit, no fault", name);
        life_note("C-LIFE.2", 1, d);
    } else {
        conf_detailf(d, "%s ended: status %ld, state %lu, end reason %lu, fault %lu",
                     name, (long)out->status_rc, (unsigned long)out->state,
                     (unsigned long)out->end_reason, (unsigned long)out->fault_class);
        life_note("C-LIFE.2", 0, d);
    }
    if (!pr->have_end) {
        conf_detailf(d, "%s printed no end line", name);
        life_note("C-LIFE.3", 0, d);
    } else if (out->exit_code == (pr->fail ? 10L : 0L)) {
        conf_detailf(d, "%s returned %ld", name, (long)out->exit_code);
        life_note("C-LIFE.3", 1, d);
    } else {
        conf_detailf(d, "%s returned %ld but reported %lu failures", name,
                     (long)out->exit_code, (unsigned long)pr->fail);
        life_note("C-LIFE.3", 0, d);
    }
}

/* ---- What the app reported ---------------------------------------------------------------- */

static struct report {
    int have_a, have_b, have_c, have_gap;
    ULONG addr_lo, addr_hi, pc_lo, pc_hi;
    ULONG sp_lo, sp_hi, lr_lo, lr_hi;
    ULONG start_lo, start_hi;
    ULONG gap_lo, gap_hi, want_lo, want_hi;
} rep;

static void absorb(const UBYTE *m)
{
    if (conf_life_is(m, CONF_LIFE_FAULT_A)) {
        rep.addr_lo = conf_life_get(m, 0);
        rep.addr_hi = conf_life_get(m, 1);
        rep.pc_lo = conf_life_get(m, 2);
        rep.pc_hi = conf_life_get(m, 3);
        rep.have_a = 1;
    } else if (conf_life_is(m, CONF_LIFE_FAULT_B)) {
        rep.sp_lo = conf_life_get(m, 0);
        rep.sp_hi = conf_life_get(m, 1);
        rep.lr_lo = conf_life_get(m, 2);
        rep.lr_hi = conf_life_get(m, 3);
        rep.have_b = 1;
    } else if (conf_life_is(m, CONF_LIFE_FAULT_C)) {
        rep.start_lo = conf_life_get(m, 0);
        rep.start_hi = conf_life_get(m, 1);
        rep.have_c = 1;
    } else if (conf_life_is(m, CONF_LIFE_GAP)) {
        rep.gap_lo = conf_life_get(m, 0);
        rep.gap_hi = conf_life_get(m, 1);
        rep.want_lo = conf_life_get(m, 2);
        rep.want_hi = conf_life_get(m, 3);
        rep.have_gap = 1;
    }
}

/* The peer keywords of the runs through conf_run_payload. */
static void peer_report(struct conf_run *run, const AARMessage *msg)
{
    (void)run;
    absorb(msg->bytes);
}

static const struct conf_peer peers[] = {
    { CONF_LIFE_FAULT_A, peer_report },
    { CONF_LIFE_FAULT_B, peer_report },
    { CONF_LIFE_FAULT_C, peer_report },
    { CONF_LIFE_GAP, peer_report },
};

static unsigned long long u64(AARU64 v)
{
    return ((unsigned long long)v.hi << 32) | (unsigned long long)v.lo;
}

static unsigned long long pair(ULONG hi, ULONG lo)
{
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
}

/* ---- Running the app ------------------------------------------------------------------------- */

static struct conf_run run;
static char ms_arg[32];

/* The state a poll callback keeps. */
static struct pstate {
    ULONG t0;
    ULONG stop_after;       /* ticks after the first poll at which to stop */
    ULONG block_ticks;      /* Delay this long first (the client blocks) */
    ULONG stop_tick, ended_tick;
    int stopped, saw_stopping;
    ULONG state_before, state_after_block;
    int phase;
    LONG b_rc, send_rc;
    struct conf_util_end b_end;
    ULONG a_state;
} ps;

typedef void (*poll_fn)(struct conf_run *r, ULONG now);

/* One run of conf_life through a serving client. norelay: the app prints no
 * result lines. */
static void life_run(const char *variant, ULONG ms, ULONG timeout_s, poll_fn poll,
                     struct conf_outcome *out)
{
    memset(&rep, 0, sizeof rep);
    conf_run_init(&run, "C-LIFE");
    run.variant = variant;
    run.norelay = 1;
    run.timeout_s = timeout_s;
    run.poll = poll;
    if (ms) {
        snprintf(ms_arg, sizeof ms_arg, "conf:ms=%lu", (unsigned long)ms);
        run.extra_args[run.extra_count++] = ms_arg;
    }
    conf_run_payload(&run, out);
}

static void life_free(void)
{
    conf_run_free(&run);
}

/* Not ran: why. */
static void not_ran(const char *id, const struct conf_outcome *out)
{
    char d[CONF_DETAIL_MAX];

    conf_detailf(d, "conf_life did not run: %.80s", out->why[0] ? out->why : "no end");
    life_note(id, 0, d);
}

/* ---- C-LIFE.4: exit codes ------------------------------------------------------------------ */

static void g_exit_codes(void)
{
    static const ULONG codes[] = { 0, 5, 10, 20, 255 };
    int ok = 1;
    char d[CONF_DETAIL_MAX] = "";
    unsigned i;

    for (i = 0; i < CONF_COUNT(codes) && ok; i++) {
        unsigned how;

        for (how = 0; how < 2 && ok; how++) {
            char variant[24];
            struct conf_outcome out;

            snprintf(variant, sizeof variant, "%s%lu", how ? CONF_LIFE_V_EXIT : CONF_LIFE_V_RET,
                     (unsigned long)codes[i]);
            life_run(variant, 0, 60, NULL, &out);
            life_free();
            if (!out.ran) {
                conf_detailf(d, "%s did not run: %.60s", variant, out.why);
                ok = 0;
            } else if (out.end_reason != AAR_END_EXIT || out.exit_code != (LONG)codes[i]) {
                conf_detailf(d, "%s: end reason %lu, exit code %ld", variant,
                             (unsigned long)out.end_reason, (long)out.exit_code);
                ok = 0;
            }
        }
    }
    if (ok)
        conf_detailf(d, "0, 5, 10, 20 and 255 by return and by aar_exit");
    life_note("C-LIFE.4", ok, d);
}

/* ---- C-LIFE.5, .6, .7, .19: a stop ---------------------------------------------------------- */

static void poll_stop(struct conf_run *r, ULONG now)
{
    AARStatusInfo st;

    if (!ps.t0)
        ps.t0 = now;
    memset(&st, 0, sizeof st);
    st.size = 16;
    if (AARStatus(r->h, &st) == AAR_OK) {
        if (st.state == AAR_STATE_STOPPING)
            ps.saw_stopping = 1;
        if (st.end_reason != AAR_END_NONE && !ps.ended_tick)
            ps.ended_tick = now;
        if (!ps.stopped)
            ps.state_before = st.state;
    }
    if (!ps.stopped && now - ps.t0 >= ps.stop_after) {
        if (ps.block_ticks) {
            /* The client does not call the library for a long time. */
            Delay(ps.block_ticks);
            memset(&st, 0, sizeof st);
            st.size = 16;
            (void)AARStatus(r->h, &st);
            ps.state_after_block = st.state;
            ps.state_before = st.state;
        }
        conf_run_stop(r);
        ps.stopped = 1;
        ps.stop_tick = conf_ticks();
    }
}

static ULONG ms_of(ULONG ticks)
{
    return ticks * 1000UL / CONF_TICKS_PER_SECOND;
}

static ULONG end_tick(void)
{
    return ps.ended_tick ? ps.ended_tick : conf_ticks();
}

static void g_stop(void)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];
    ULONG took;

    memset(&ps, 0, sizeof ps);
    ps.stop_after = CONF_TICKS_PER_SECOND;
    life_run(CONF_LIFE_V_STOP, 0, 60, poll_stop, &out);
    life_free();
    took = ms_of(end_tick() - ps.stop_tick);
    if (!out.ran) {
        not_ran("C-LIFE.5", &out);
        not_ran("C-LIFE.6", &out);
        return;
    }
    /* C-LIFE.5: RUNNING, then STOPPING, then IDLE. */
    if (ps.state_before == AAR_STATE_RUNNING && ps.saw_stopping &&
        out.state == AAR_STATE_IDLE) {
        conf_detailf(d, "running, stopping, idle, %lu ms to end", (unsigned long)took);
        life_note("C-LIFE.5", 1, d);
    } else {
        conf_detailf(d, "state before the stop %lu, stopping %s, final %lu",
                     (unsigned long)ps.state_before, ps.saw_stopping ? "seen" : "not seen",
                     (unsigned long)out.state);
        life_note("C-LIFE.5", 0, d);
    }
    /* C-LIFE.6: should_stop and the event, a clean exit inside the grace period. */
    if (out.end_reason == AAR_END_EXIT && out.exit_code == CONF_LIFE_STOP_OK) {
        conf_detailf(d, "should_stop and AAR_EV_STOP seen, exit code %ld after %lu ms",
                     (long)out.exit_code, (unsigned long)took);
        life_note("C-LIFE.6", 1, d);
    } else if (out.exit_code == CONF_LIFE_STOP_NO_FLAG) {
        life_note("C-LIFE.6", 0, "aar_should_stop was 0 after the stop request");
    } else if (out.exit_code == CONF_LIFE_STOP_NO_EVENT) {
        life_note("C-LIFE.6", 0, "no AAR_EV_STOP event was queued");
    } else {
        conf_detailf(d, "end reason %lu, exit code %ld after %lu ms (grace %lu ms)",
                     (unsigned long)out.end_reason, (long)out.exit_code,
                     (unsigned long)took, (unsigned long)conf_g.info.stop_grace_ms);
        life_note("C-LIFE.6", 0, d);
    }
}

static void g_forced(void)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];
    ULONG took;
    ULONG grace = conf_g.info.stop_grace_ms;

    memset(&ps, 0, sizeof ps);
    ps.stop_after = CONF_TICKS_PER_SECOND;
    life_run(CONF_LIFE_V_IGNORE, 0, 60, poll_stop, &out);
    life_free();
    if (!out.ran) {
        not_ran("C-LIFE.7", &out);
        return;
    }
    took = ms_of(end_tick() - ps.stop_tick);
    /* The clock has a 20 ms step and the stop call returns after it started. */
    if (out.end_reason != AAR_END_FORCED) {
        conf_detailf(d, "end reason %lu after %lu ms, want AAR_END_FORCED",
                     (unsigned long)out.end_reason, (unsigned long)took);
        life_note("C-LIFE.7", 0, d);
    } else if (took + 60UL < grace || took > grace + 2000UL) {
        conf_detailf(d, "forced after %lu ms, the grace period is %lu ms (up to +2000)",
                     (unsigned long)took, (unsigned long)grace);
        life_note("C-LIFE.7", 0, d);
    } else {
        conf_detailf(d, "forced after %lu ms, grace %lu ms", (unsigned long)took,
                     (unsigned long)grace);
        life_note("C-LIFE.7", 1, d);
    }
}

static void g_blocked(void)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];

    memset(&ps, 0, sizeof ps);
    ps.stop_after = CONF_TICKS_PER_SECOND;
    ps.block_ticks = 15UL * CONF_TICKS_PER_SECOND;
    life_run(CONF_LIFE_V_STOP, 0, 120, poll_stop, &out);
    life_free();
    if (!out.ran) {
        not_ran("C-LIFE.19", &out);
        return;
    }
    if (ps.state_after_block == AAR_STATE_RUNNING && out.end_reason == AAR_END_EXIT) {
        life_note("C-LIFE.19", 1, "still running after the client blocked for 15 s");
    } else {
        conf_detailf(d, "after 15 s without a call: state %lu, then end reason %lu",
                     (unsigned long)ps.state_after_block, (unsigned long)out.end_reason);
        life_note("C-LIFE.19", 0, d);
    }
}

/* ---- C-LIFE.8: a fault ends one app only ------------------------------------------------------ */

static void poll_two(struct conf_run *r, ULONG now)
{
    AARStatusInfo st;
    AARMessage go;
    AARHandle *hb = NULL;

    if (!ps.t0)
        ps.t0 = now;
    if (ps.phase != 0 || now - ps.t0 < CONF_TICKS_PER_SECOND)
        return;
    ps.phase = 1;
    /* The second app: a data abort. */
    ps.b_rc = conf_util_start_payload(&hb, "conf_life.elf", CONF_LIFE_V_FAULT_DATA, 0);
    if (ps.b_rc == AAR_OK) {
        conf_util_wait_end(hb, 10UL * CONF_TICKS_PER_SECOND, &ps.b_end);
        (void)AARClose(hb);
    }
    /* The first is still waiting in msg_wait. */
    memset(&st, 0, sizeof st);
    st.size = 16;
    (void)AARStatus(r->h, &st);
    ps.a_state = st.state;
    memset(&go, 0, sizeof go);
    conf_life_build(go.bytes, CONF_LIFE_GO, 0, 0, 0, 0);
    ps.send_rc = AARSendMessage(r->h, &go);
}

static void g_two(void)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];

    memset(&ps, 0, sizeof ps);
    life_run(CONF_LIFE_V_WAIT_MSG, 0, 90, poll_two, &out);
    life_free();
    if (!out.ran) {
        not_ran("C-LIFE.8", &out);
        return;
    }
    if (ps.b_rc != AAR_OK) {
        conf_detailf(d, "the faulting app did not start: %ld", (long)ps.b_rc);
        life_note("C-LIFE.8", 0, d);
    } else if (ps.b_end.end_reason != AAR_END_USER_FAULT) {
        conf_detailf(d, "the faulting app ended with reason %lu",
                     (unsigned long)ps.b_end.end_reason);
        life_note("C-LIFE.8", 0, d);
    } else if (ps.a_state != AAR_STATE_RUNNING) {
        conf_detailf(d, "the waiting app was in state %lu after the other faulted",
                     (unsigned long)ps.a_state);
        life_note("C-LIFE.8", 0, d);
    } else if (ps.send_rc != AAR_OK || out.end_reason != AAR_END_EXIT || out.exit_code != 0) {
        conf_detailf(d, "the waiting app: send %ld, end reason %lu, exit code %ld",
                     (long)ps.send_rc, (unsigned long)out.end_reason, (long)out.exit_code);
        life_note("C-LIFE.8", 0, d);
    } else {
        life_note("C-LIFE.8", 1, "one app faulted, the other kept waiting and exited normally");
    }
}

/* ---- C-LIFE.9 to C-LIFE.14: the fault report -------------------------------------------------- */

static int user_mode(ULONG pstate)
{
    /* A32: CPSR mode bits 0x10 (user). A64: PSTATE M[4:0] 0 (EL0t, AArch64). */
    return conf_g.profile_value == AAR_PROFILE_A32 ? (pstate & 0x1FUL) == 0x10UL
                                                   : (pstate & 0x1FUL) == 0UL;
}

/* Runs a fault variant and checks the part every kind shares: the app ended with
 * AAR_END_USER_FAULT and the class. Returns 1 to go on. */
static int fault_run(const char *variant, const char *id, ULONG fault_class,
                     struct conf_outcome *out)
{
    char d[CONF_DETAIL_MAX];

    life_run(variant, 0, 60, NULL, out);
    life_free();
    if (!out->ran) {
        not_ran(id, out);
        return 0;
    }
    if (out->end_reason != AAR_END_USER_FAULT ||
        (out->state != AAR_STATE_IDLE && out->state != AAR_STATE_FAULTED)) {
        conf_detailf(d, "%s: end reason %lu, state %lu (want USER_FAULT, IDLE or FAULTED)",
                     variant, (unsigned long)out->end_reason, (unsigned long)out->state);
        life_note(id, 0, d);
        return 0;
    }
    if (out->fault_class != fault_class) {
        conf_detailf(d, "%s: fault class %lu, want %lu", variant,
                     (unsigned long)out->fault_class, (unsigned long)fault_class);
        life_note(id, 0, d);
        return 0;
    }
    if (!rep.have_a) {
        life_note(id, 0, "the app's fault report did not arrive");
        return 0;
    }
    return 1;
}

static void g_fault_data(void)
{
    struct conf_outcome out;
    const AARStatusInfo *st;
    char d[CONF_DETAIL_MAX];
    unsigned long long off_report;
    unsigned long long off_status;

    if (!fault_run(CONF_LIFE_V_FAULT_DATA, "C-LIFE.9", AAR_FAULT_DATA, &out)) {
        life_note("C-LIFE.13", 0, "no fault report (see C-LIFE.9)");
        life_note("C-LIFE.14", 0, "no fault report (see C-LIFE.9)");
        return;
    }
    st = &out.status;
    /* C-LIFE.9: the address and the instruction. */
    if (u64(st->fault_addr) != pair(rep.addr_hi, rep.addr_lo)) {
        conf_detailf(d, "fault_addr %lx:%lx, the app touched %lx:%lx",
                     (unsigned long)st->fault_addr.hi, (unsigned long)st->fault_addr.lo,
                     (unsigned long)rep.addr_hi, (unsigned long)rep.addr_lo);
        life_note("C-LIFE.9", 0, d);
    } else if (u64(st->fault_pc) != pair(rep.pc_hi, rep.pc_lo)) {
        conf_detailf(d, "fault_pc %lx:%lx, the instruction is at %lx:%lx",
                     (unsigned long)st->fault_pc.hi, (unsigned long)st->fault_pc.lo,
                     (unsigned long)rep.pc_hi, (unsigned long)rep.pc_lo);
        life_note("C-LIFE.9", 0, d);
    } else {
        conf_detailf(d, "fault_addr %lx, fault_pc %lx", (unsigned long)st->fault_addr.lo,
                     (unsigned long)st->fault_pc.lo);
        life_note("C-LIFE.9", 1, d);
    }
    /* C-LIFE.13: sp, lr, and the pc relative to the load base. */
    off_report = pair(rep.pc_hi, rep.pc_lo) - pair(rep.start_hi, rep.start_lo);
    off_status = u64(st->fault_pc) - u64(st->load_base);
    if (!rep.have_b || !rep.have_c) {
        life_note("C-LIFE.13", 0, "the app's register report did not arrive");
    } else if (u64(st->fault_sp) != pair(rep.sp_hi, rep.sp_lo)) {
        conf_detailf(d, "fault_sp %lx, the app had sp %lx", (unsigned long)st->fault_sp.lo,
                     (unsigned long)rep.sp_lo);
        life_note("C-LIFE.13", 0, d);
    } else if (u64(st->fault_lr) != pair(rep.lr_hi, rep.lr_lo)) {
        conf_detailf(d, "fault_lr %lx, the app had lr %lx", (unsigned long)st->fault_lr.lo,
                     (unsigned long)rep.lr_lo);
        life_note("C-LIFE.13", 0, d);
    } else if (u64(st->load_base) == 0 || off_status != off_report) {
        conf_detailf(d, "fault_pc - load_base is %lx, the image offset is %lx (base %lx)",
                     (unsigned long)off_status, (unsigned long)off_report,
                     (unsigned long)st->load_base.lo);
        life_note("C-LIFE.13", 0, d);
    } else {
        conf_detailf(d, "sp %lx, lr %lx, image offset %lx", (unsigned long)rep.sp_lo,
                     (unsigned long)rep.lr_lo, (unsigned long)off_report);
        life_note("C-LIFE.13", 1, d);
    }
    /* C-LIFE.14: the syndrome, the mode, the profile. */
    if (st->fault_syndrome == 0) {
        life_note("C-LIFE.14", 0, "fault_syndrome is 0 for a data abort");
    } else if (!user_mode(st->fault_pstate)) {
        conf_detailf(d, "fault_pstate %lx is not user mode", (unsigned long)st->fault_pstate);
        life_note("C-LIFE.14", 0, d);
    } else if (st->profile != conf_g.profile_value) {
        conf_detailf(d, "status profile %lu, the image is %lu", (unsigned long)st->profile,
                     (unsigned long)conf_g.profile_value);
        life_note("C-LIFE.14", 0, d);
    } else {
        conf_detailf(d, "syndrome %lx, pstate %lx", (unsigned long)st->fault_syndrome,
                     (unsigned long)st->fault_pstate);
        life_note("C-LIFE.14", 1, d);
    }
}

/* Fetch from an unmapped address and an undefined instruction: the pc. */
static void g_fault_pc(const char *variant, const char *id, ULONG fault_class)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];

    if (!fault_run(variant, id, fault_class, &out))
        return;
    if (u64(out.status.fault_pc) != pair(rep.pc_hi, rep.pc_lo)) {
        conf_detailf(d, "fault_pc %lx:%lx, want %lx:%lx",
                     (unsigned long)out.status.fault_pc.hi,
                     (unsigned long)out.status.fault_pc.lo, (unsigned long)rep.pc_hi,
                     (unsigned long)rep.pc_lo);
        life_note(id, 0, d);
    } else {
        conf_detailf(d, "class %lu at pc %lx", (unsigned long)fault_class,
                     (unsigned long)rep.pc_lo);
        life_note(id, 1, d);
    }
}

static void g_fault_fetch(void)
{
    g_fault_pc(CONF_LIFE_V_FAULT_FETCH, "C-LIFE.10", AAR_FAULT_FETCH);
}

static void g_fault_illegal(void)
{
    g_fault_pc(CONF_LIFE_V_FAULT_ILLEGAL, "C-LIFE.11", AAR_FAULT_ILLEGAL);
}

static void g_fault_align(void)
{
    struct conf_outcome out;

    if (fault_run(CONF_LIFE_V_FAULT_ALIGN, "C-LIFE.12", AAR_FAULT_ALIGN, &out))
        life_note("C-LIFE.12", 1, "misaligned exclusive access: AAR_FAULT_ALIGN");
}

/* ---- C-LIFE.15 to C-LIFE.18: the watchdog ------------------------------------------------------ */

/* An app that computes (spin) or sleeps for ms and reports the silent time it
 * measured: it has to be alive at the end and have been silent for at least
 * least_ms. The calibration of a computing app is not exact (the share of the CPU
 * it gets changes with what else runs): when it was alive but measured too little,
 * a second run asks for the time that makes up the difference. */
static void gap_run(const char *variant, ULONG ms, ULONG least_ms, ULONG timeout_s,
                    const char *id, const char *what)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];
    unsigned long long gap = 0;
    ULONG ask = ms;
    int attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        life_run(variant, ask, timeout_s, NULL, &out);
        life_free();
        if (!out.ran) {
            not_ran(id, &out);
            return;
        }
        gap = (unsigned long long)(rep.gap_lo / 1000UL); /* below 4294 s: the high word is 0 */
        if (out.end_reason != AAR_END_EXIT || out.exit_code != 0 || !rep.have_gap)
            break;
        if (gap >= least_ms || gap < 1000ULL || attempt == 1)
            break;
        ask = (ULONG)((unsigned long long)ask * (unsigned long long)ms / gap);
    }
    if (out.end_reason != AAR_END_EXIT || out.exit_code != 0) {
        conf_detailf(d, "%s: ended with reason %lu (exit %ld) after %lu ms of %lu",
                     what, (unsigned long)out.end_reason, (long)out.exit_code,
                     (unsigned long)gap, (unsigned long)ask);
        life_note(id, 0, d);
    } else if (!rep.have_gap || gap < least_ms) {
        conf_detailf(d, "%s: the app measured only %lu ms of %lu", what, (unsigned long)gap,
                     (unsigned long)ask);
        life_note(id, 0, d);
    } else {
        conf_detailf(d, "%s for %lu ms, stall limit %lu ms: not stopped", what,
                     (unsigned long)gap, (unsigned long)conf_g.info.stall_limit_ms);
        life_note(id, 1, d);
    }
}

static void g_spin10(void)
{
    gap_run(CONF_LIFE_V_SPIN, 9800UL, 9500UL, 90, "C-LIFE.15", "computed without a call");
}

static void g_sleep(void)
{
    gap_run(CONF_LIFE_V_SLEEP, 12000UL, 11500UL, 90, "C-LIFE.16", "parked in sleep_us");
}

static void g_spin90(void)
{
    ULONG limit = conf_g.info.stall_limit_ms;
    ULONG ms = limit / 10UL * 9UL;

    if (ms > 60000UL)
        ms = 60000UL;
    gap_run(CONF_LIFE_V_SPIN, ms, ms / 100UL * 85UL, ms / 1000UL + 90UL, "C-LIFE.18",
            "computed without a call");
}

static void g_activity(void)
{
    struct conf_outcome out;
    char d[CONF_DETAIL_MAX];

    life_run(CONF_LIFE_V_ACTIVITY, 0, 240, NULL, &out);
    life_free();
    if (!out.ran) {
        not_ran("C-LIFE.17", &out);
    } else if (out.end_reason != AAR_END_EXIT || out.exit_code != 0) {
        conf_detailf(d, "the app ended with reason %lu, exit code %ld",
                     (unsigned long)out.end_reason, (long)out.exit_code);
        life_note("C-LIFE.17", 0, d);
    } else {
        life_note("C-LIFE.17", 1, "six kinds of call every 4 s for 12 s each: alive");
    }
}

/* ---- C-LIFE.20 and C-LIFE.21: a client that does not close --------------------------------- */

/* The child process starts an app and leaves. What it did, read by the parent. */
static struct child {
    volatile LONG done;
    LONG rc;
    AARHandle *h;
    char path[256];
} child;

static const char *const child_argv[] = { "conf_life.elf", "conf:variant=stop" };

static void child_start(void)
{
    ULONG t0 = conf_ticks();

    child.rc = conf_util_start_file(&child.h, child.path, child_argv, 2);
    /* Let it reach RUNNING before the child goes away. */
    while (child.rc == AAR_OK && conf_ticks() - t0 < 3UL * CONF_TICKS_PER_SECOND) {
        AARStatusInfo st;

        memset(&st, 0, sizeof st);
        st.size = 16;
        if (AARStatus(child.h, &st) == AAR_OK && st.state == AAR_STATE_RUNNING)
            break;
        Delay(1);
    }
}

/* C-LIFE.20: the task ends and never calls AARClose. */
static void child_dies(void)
{
    child_start();
    child.done = 1;
}

/* C-LIFE.21: the task closes its last aar.library base and never calls AARClose. */
static void child_closes_library(void)
{
    struct Library *own = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, AAR_LIB_VERSION);
    struct Library *saved = AARBase;

    if (!own) {
        child.rc = AAR_E_NOSYS;
        child.done = 1;
        return;
    }
    AARBase = own; /* the parent is waiting and calls nothing meanwhile */
    child_start();
    AARBase = saved;
    CloseLibrary(own);
    child.done = 1;
}

/* After the child is gone: the parent's call on the child's handle. AAR_E_OWNER while
 * the claim is live and the child's, AAR_E_STALE once the library released it. */
static LONG probe_child_handle(void)
{
    AARStatusInfo st;

    memset(&st, 0, sizeof st);
    st.size = 16;
    return AARStatus(child.h, &st);
}

/* Claims that must be available again: AAROpen once, and max_apps of them when the
 * platform has a ceiling. */
static int claims_available(char *d)
{
    AARHandle *hs[64];
    ULONG want = conf_g.info.max_apps ? conf_g.info.max_apps : 1UL;
    ULONG n = 0;
    ULONG reason = 0;
    LONG rc = AAR_OK;

    if (want > 64UL)
        want = 1UL;
    while (n < want && (rc = AAROpen(&hs[n], NULL, &reason)) == AAR_OK)
        n++;
    while (n > 0)
        (void)AARClose(hs[--n]);
    if (rc != AAR_OK) {
        conf_detailf(d, "a new claim failed: AAROpen returned %ld (reason %lu)", (long)rc,
                     (unsigned long)reason);
        return 0;
    }
    return 1;
}

static void dead_owner(const char *id, void (*fn)(void), const char *what, ULONG wait_s)
{
    char d[CONF_DETAIL_MAX];
    ULONG t0;
    LONG rc = AAR_E_OWNER;

    memset(&child, 0, sizeof child);
    if (!conf_util_payload_path(child.path, sizeof child.path, "conf_life.elf")) {
        life_note(id, 0, "payload path too long");
        return;
    }
    if (!conf_util_spawn("aar-conf-child", fn)) {
        life_note(id, 0, "cannot create the child process");
        return;
    }
    if (!conf_util_wait_flag(&child.done, 15)) {
        life_note(id, 0, "the child process did not finish");
        return;
    }
    if (child.rc != AAR_OK) {
        conf_detailf(d, "the child could not start the app: %ld", (long)child.rc);
        life_note(id, 0, d);
        return;
    }
    t0 = conf_ticks();
    for (;;) {
        rc = probe_child_handle();
        if (rc != AAR_E_OWNER || conf_ticks() - t0 > wait_s * CONF_TICKS_PER_SECOND)
            break;
        Delay(10);
    }
    if (rc != AAR_E_STALE) {
        conf_detailf(d, "%s: after %lu s a call on its handle returns %ld, not AAR_E_STALE",
                     what, (unsigned long)((conf_ticks() - t0) / CONF_TICKS_PER_SECOND),
                     (long)rc);
        life_note(id, 0, d);
        return;
    }
    if (!claims_available(d)) {
        life_note(id, 0, d);
        return;
    }
    conf_detailf(d, "%s: handle released after %lu ms, a new claim works", what,
                 (unsigned long)ms_of(conf_ticks() - t0));
    life_note(id, 1, d);
}

static void g_dead_owner(void)
{
    dead_owner("C-LIFE.20", child_dies, "the owner task ended", 10UL);
}

static void g_close_library(void)
{
    dead_owner("C-LIFE.21", child_closes_library, "the owner closed its last base", 10UL);
}

/* ---- C-LIFE.22 and C-LIFE.23: a revoked claim (ZZ9000) ---------------------------------------- */

static const aar_client_ext_zz9000 *zz_ext(char *d)
{
    const aar_client_ext_zz9000 *x =
        (const aar_client_ext_zz9000 *)AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);

    if (!x || !AAR_CLIENT_EXT_ZZ9000_COVERS(x, release_handle)) {
        conf_detailf(d, "AARExt(ZZ9000, CLIENT) gives no release_handle");
        return NULL;
    }
    return x;
}

#define ZZ_SNAP 64

/* The handle words of the claims on the card. */
static ULONG snapshot(const aar_client_ext_zz9000 *x, ULONG *words)
{
    AARZZ9000List li;
    ULONG cursor = 0;
    ULONG n = 0;

    while (n < ZZ_SNAP) {
        memset(&li, 0, sizeof li);
        li.size = sizeof li;
        if (x->list(cursor, &li) != 1)
            break;
        words[n++] = li.handle;
        cursor = li.index + 1UL;
    }
    return n;
}

/* The claim that is not in before[]: the card's word for the app just started. */
static ULONG new_claim(const aar_client_ext_zz9000 *x, const ULONG *before, ULONG nbefore,
                       AARZZ9000List *found)
{
    AARZZ9000List li;
    ULONG cursor = 0;

    for (;;) {
        ULONG i;
        int known = 0;

        memset(&li, 0, sizeof li);
        li.size = sizeof li;
        if (x->list(cursor, &li) != 1)
            return 0;
        for (i = 0; i < nbefore; i++)
            known |= before[i] == li.handle;
        if (!known) {
            *found = li;
            return li.handle;
        }
        cursor = li.index + 1UL;
    }
}

static void g_revoke(void)
{
    static const char *const argv[] = { "conf-life-rel", "conf:variant=stop" };
    const aar_client_ext_zz9000 *x;
    ULONG before[ZZ_SNAP];
    ULONG nbefore;
    ULONG word;
    AARZZ9000List li;
    AARHandle *h = NULL;
    AARStatusInfo st;
    AARMessage msg;
    char path[256];
    char d[CONF_DETAIL_MAX];
    LONG rc;
    LONG status_before;

    x = zz_ext(d);
    if (!x) {
        life_note("C-LIFE.22", 0, d);
        return;
    }
    if (!conf_util_payload_path(path, sizeof path, "conf_life.elf")) {
        life_note("C-LIFE.22", 0, "payload path too long");
        return;
    }
    nbefore = snapshot(x, before);
    rc = conf_util_start_file(&h, path, argv, 2);
    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        life_note("C-LIFE.22", 0, d);
        return;
    }
    Delay(25);
    word = new_claim(x, before, nbefore, &li);
    if (!word) {
        (void)AARClose(h);
        life_note("C-LIFE.22", 0, "the card does not list the new claim");
        return;
    }
    rc = x->release_handle(word);
    if (rc != AAR_OK) {
        (void)AARClose(h);
        conf_detailf(d, "release_handle returned %ld", (long)rc);
        life_note("C-LIFE.22", 0, d);
        return;
    }
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    memset(&msg, 0, sizeof msg);
    /* The first call that reaches the platform (AARStop) learns that the claim is gone;
     * a read of the control block (AARStatus) cannot [spec question 55]. What the
     * status says before that is noted, not judged. */
    status_before = AARStatus(h, &st);
    if (AARStop(h) != AAR_E_STALE) {
        life_note("C-LIFE.22", 0, "AARStop on a revoked handle is not AAR_E_STALE");
    } else if (AARStatus(h, &st) != AAR_E_STALE) {
        life_note("C-LIFE.22", 0, "AARStatus after AARStop on a revoked handle is not AAR_E_STALE");
    } else if (AARSendMessage(h, &msg) != AAR_E_STALE) {
        life_note("C-LIFE.22", 0, "AARSendMessage on a revoked handle is not AAR_E_STALE");
    } else if (AARGetMessage(h, &msg) != AAR_E_STALE) {
        life_note("C-LIFE.22", 0, "AARGetMessage on a revoked handle is not AAR_E_STALE");
    } else if (AARClose(h) != AAR_OK) {
        life_note("C-LIFE.22", 0, "AARClose of a revoked handle is not AAR_OK");
    } else {
        conf_detailf(d, "released through the card: AAR_E_STALE for every call, close AAR_OK "
                        "(AARStatus before the first platform call: %ld)", (long)status_before);
        life_note("C-LIFE.22", 1, d);
        return;
    }
    (void)AARClose(h);
}

#define LATE_WAIT_S 60UL

static void g_late_writes(void)
{
    static const char *const argv1[] = { "conf-life-h1", "conf:variant=hold-shared" };
    static const char *const argv2[] = { "conf-life-f2", "conf:variant=fill-shared" };
    const aar_client_ext_zz9000 *x;
    ULONG before[ZZ_SNAP];
    ULONG nbefore;
    AARZZ9000List li;
    AARHandle *h1 = NULL;
    AARHandle *h2 = NULL;
    struct AARSharedMap map;
    AARMessage msg;
    struct conf_util_end e;
    char path[256];
    char d[CONF_DETAIL_MAX];
    ULONG t0;
    ULONG word;
    LONG rc;
    int got = 0;

    x = zz_ext(d);
    if (!x || !AAR_CLIENT_EXT_ZZ9000_COVERS(x, lease_pause)) {
        life_note("C-LIFE.23", 0, x ? "the library has no lease_pause hook" : d);
        return;
    }
    if (!conf_util_payload_path(path, sizeof path, "conf_life.elf")) {
        life_note("C-LIFE.23", 0, "payload path too long");
        return;
    }
    nbefore = snapshot(x, before);
    rc = conf_util_start_file(&h1, path, argv1, 2);
    if (rc != AAR_OK) {
        conf_detailf(d, "the first app did not start: %ld", (long)rc);
        life_note("C-LIFE.23", 0, d);
        return;
    }
    /* Its token. */
    t0 = conf_ticks();
    while (!got && conf_ticks() - t0 < 10UL * CONF_TICKS_PER_SECOND) {
        if (AARGetMessage(h1, &msg) == AAR_OK && conf_life_is(msg.bytes, CONF_LIFE_TOKEN))
            got = 1;
        else
            Delay(2);
    }
    memset(&map, 0, sizeof map);
    map.size = sizeof map;
    if (!got) {
        (void)AARClose(h1);
        life_note("C-LIFE.23", 0, "the first app sent no token");
        return;
    }
    rc = AARSharedMap(h1, conf_life_get(msg.bytes, 0), conf_life_get(msg.bytes, 2), 0, &map);
    if (rc != AAR_OK) {
        (void)AARClose(h1);
        conf_detailf(d, "AARSharedMap returned %ld", (long)rc);
        life_note("C-LIFE.23", 0, d);
        return;
    }
    word = new_claim(x, before, nbefore, &li);
    /* The client stops renewing: the platform revokes the claim. No call is made with
     * h1 until the end, a stale request would release the quarantine. */
    rc = x->lease_pause(h1, 120000UL);
    if (rc != AAR_OK || !word) {
        (void)AARClose(h1);
        conf_detailf(d, "lease_pause %ld, claim %s", (long)rc, word ? "listed" : "not listed");
        life_note("C-LIFE.23", 0, d);
        return;
    }
    t0 = conf_ticks();
    for (;;) {
        ULONG cursor = 0;
        int gone = 1;

        for (;;) {
            memset(&li, 0, sizeof li);
            li.size = sizeof li;
            if (x->list(cursor, &li) != 1)
                break;
            if (li.handle == word && li.end_reason == AAR_END_NONE)
                gone = 0;
            cursor = li.index + 1UL;
        }
        if (gone || conf_ticks() - t0 > LATE_WAIT_S * CONF_TICKS_PER_SECOND)
            break;
        Delay(25);
    }
    /* The second app allocates; the old client writes garbage through its map. */
    rc = conf_util_start_file(&h2, path, argv2, 2);
    if (rc != AAR_OK) {
        (void)x->lease_pause(h1, 0);
        (void)AARClose(h1);
        conf_detailf(d, "the second app did not start: %ld", (long)rc);
        life_note("C-LIFE.23", 0, d);
        return;
    }
    t0 = conf_ticks();
    for (;;) {
        volatile UBYTE *p = (volatile UBYTE *)map.base;
        AARStatusInfo s2;
        ULONG i;

        for (i = 0; i < map.bytes; i++)
            p[i] = 0xEE;
        memset(&s2, 0, sizeof s2);
        s2.size = 16;
        if (AARStatus(h2, &s2) != AAR_OK || s2.end_reason != AAR_END_NONE ||
            conf_ticks() - t0 > 30UL * CONF_TICKS_PER_SECOND)
            break;
    }
    conf_util_wait_end(h2, 10UL * CONF_TICKS_PER_SECOND, &e);
    (void)AARClose(h2);
    /* Now the old client's stale request. */
    (void)AARClose(h1);
    if (!e.ended || e.exit_code != 0) {
        conf_detailf(d, "the second app's pattern changed or it never ended: end reason %lu, "
                        "exit code %ld", (unsigned long)e.end_reason, (long)e.exit_code);
        life_note("C-LIFE.23", 0, d);
    } else {
        life_note("C-LIFE.23", 1, "the second app's blocks stayed intact under the old client's writes");
    }
}

/* ---- C-LIFE.25: relaunch -------------------------------------------------------------------------- */

static void g_relaunch(void)
{
    UBYTE *first = NULL;
    ULONG first_len = 0;
    char d[CONF_DETAIL_MAX] = "";
    int ok = 1;
    unsigned i;

    for (i = 0; i < 20 && ok; i++) {
        struct conf_outcome out;

        life_run(CONF_LIFE_V_HELLO, 0, 60, NULL, &out);
        if (!out.ran) {
            conf_detailf(d, "run %u did not run: %.70s", i + 1, out.why);
            ok = 0;
        } else if (out.end_reason != AAR_END_EXIT || out.exit_code != 0 ||
                   out.state != AAR_STATE_IDLE) {
            conf_detailf(d, "run %u: end reason %lu, exit code %ld", i + 1,
                         (unsigned long)out.end_reason, (long)out.exit_code);
            ok = 0;
        } else if (i == 0) {
            first = AllocVec(run.cap.len + 1UL, MEMF_ANY | MEMF_CLEAR);
            if (!first || run.cap.len == 0) {
                conf_detailf(d, "run 1 printed %lu bytes", (unsigned long)run.cap.len);
                ok = 0;
            } else {
                memcpy(first, run.cap.buf, run.cap.len);
                first_len = run.cap.len;
            }
        } else if (run.cap.len != first_len || memcmp(first, run.cap.buf, first_len) != 0) {
            conf_detailf(d, "run %u printed something else than run 1", i + 1);
            ok = 0;
        }
        life_free();
    }
    if (first)
        FreeVec(first);
    if (ok)
        conf_detailf(d, "20 runs: the same output (%lu bytes), exit code 0 each time",
                     (unsigned long)first_len);
    life_note("C-LIFE.25", ok, d);
}

/* ---- C-LIFE.26: AARClose of a running app -------------------------------------------------------- */

static void g_close_running(void)
{
    AARHandle *h = NULL;
    AARStatusInfo st;
    AARMessage msg;
    char d[CONF_DETAIL_MAX];
    ULONG beats = 0;
    ULONG t0;
    LONG rc;

    rc = conf_util_start_payload(&h, "conf_life.elf", CONF_LIFE_V_BEAT, 0);
    if (rc != AAR_OK) {
        conf_detailf(d, "the app did not start: %ld", (long)rc);
        life_note("C-LIFE.26", 0, d);
        return;
    }
    /* The heartbeat: messages every 50 ms. */
    t0 = conf_ticks();
    while (beats < 3 && conf_ticks() - t0 < 5UL * CONF_TICKS_PER_SECOND) {
        if (AARGetMessage(h, &msg) == AAR_OK && conf_life_is(msg.bytes, CONF_LIFE_BEAT))
            beats++;
        else
            Delay(1);
    }
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    (void)AARStatus(h, &st);
    if (beats < 3 || st.state != AAR_STATE_RUNNING) {
        (void)AARClose(h);
        conf_detailf(d, "the app was not running before the close: %lu beats, state %lu",
                     (unsigned long)beats, (unsigned long)st.state);
        life_note("C-LIFE.26", 0, d);
        return;
    }
    rc = AARClose(h);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARClose of a running app returned %ld", (long)rc);
        life_note("C-LIFE.26", 0, d);
    } else if (AARStatus(h, &st) != AAR_E_STALE) {
        life_note("C-LIFE.26", 0, "the handle is still usable after AARClose");
    } else if (!claims_available(d)) {
        life_note("C-LIFE.26", 0, d);
    } else {
        life_note("C-LIFE.26", 1, "closed while running: AAR_OK, handle stale, a new claim works");
    }
}

/* ---- The table ---------------------------------------------------------------------------------- */

#define GROUP(name)                                  \
    static int name##_done;                          \
    static void t_##name(int idx)                    \
    {                                                \
        (void)idx;                                   \
        if (!name##_done) {                          \
            name##_done = 1;                         \
            g_##name();                              \
        }                                            \
    }

GROUP(exit_codes)
GROUP(stop)
GROUP(forced)
GROUP(blocked)
GROUP(two)
GROUP(fault_data)
GROUP(fault_fetch)
GROUP(fault_illegal)
GROUP(fault_align)
GROUP(spin10)
GROUP(sleep)
GROUP(activity)
GROUP(spin90)
GROUP(dead_owner)
GROUP(close_library)
GROUP(revoke)
GROUP(late_writes)
GROUP(relaunch)
GROUP(close_running)

static const struct conf_r_test tests[] = {
    { "C-LIFE.4", t_exit_codes },
    { "C-LIFE.5", t_stop },
    { "C-LIFE.6", t_stop },
    { "C-LIFE.7", t_forced },
    { "C-LIFE.8", t_two },
    { "C-LIFE.9", t_fault_data },
    { "C-LIFE.10", t_fault_fetch },
    { "C-LIFE.11", t_fault_illegal },
    { "C-LIFE.12", t_fault_align },
    { "C-LIFE.13", t_fault_data },
    { "C-LIFE.14", t_fault_data },
    { "C-LIFE.15", t_spin10 },
    { "C-LIFE.16", t_sleep },
    { "C-LIFE.17", t_activity },
    { "C-LIFE.18", t_spin90 },
    { "C-LIFE.19", t_blocked },
    { "C-LIFE.20", t_dead_owner },
    { "C-LIFE.21", t_close_library },
    { "C-LIFE.22", t_revoke },
    { "C-LIFE.23", t_late_writes },
    { "C-LIFE.25", t_relaunch },
    { "C-LIFE.26", t_close_running },
};

const struct conf_area conf_area_life = {
    .name = "C-LIFE",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .on_open = on_open,
    .on_end = on_end,
};
