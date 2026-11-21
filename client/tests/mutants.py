#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove that the core and shell tests fail on deliberately broken code.

  mutants.py [--list] [NAME ...]

Each mutant is a small, exact change to client/src/aar_core.c or aar_shell.c
(a check removed, a counter not counted, two steps swapped). The runner copies
client/ to a temporary directory, applies the change, builds core_test or
shell_test and runs it. A mutant is KILLED when the build succeeds, the run
fails, and the failing test is the one the mutant names (a crash counts as the
last test that announced itself). It is a failure of this script when a mutant
survives, fails in another test, no longer applies, or does not build.

Needs a C compiler and python3. The tests are built without PIE (the reference
platform's arena must be below 4 GiB).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CC = os.environ.get("CC", "gcc")

CORE = "client/src/aar_core.c"
SHELL = "client/src/aar_shell.c"
LEAK = "client/tests/amiga/aarleak.c"

# (name, test, file, [(old, new, count)])  count None: exactly one occurrence
M = []


def mutant(name, test, path, *changes):
    fixed = []
    for change in changes:
        old, new = change[0], change[1]
        fixed.append((old, new, change[2] if len(change) > 2 else 1))
    M.append((name, test, path, fixed))


# ---- core: ownership, registry, lease ------------------------------------------
mutant("no-owner-check", "owner_stale", CORE,
       ("} else if (h->owner.id != me.id) {", "} else if (0) {"))
mutant("no-registry-lookup", "owner_stale", CORE,
       ("if (!registry_find(c, h) || h->state != AAR_H_LIVE) {", "if (h->state != AAR_H_LIVE) {"))
mutant("close-owner-closes-everyone", "open_close_many", CORE,
       ("if (h->owner.id == me.id &&\n                (h->state == AAR_H_LIVE",
        "if (1 &&\n                (h->state == AAR_H_LIVE"))
mutant("release-before-unlink", "close_order", CORE,
       ("    h->renew = 0u;\n    lease_unlink(h);\n    if (h->claim.handle != 0u) {",
        "    if (h->claim.handle != 0u) {"),
       ("        h->claim.handle = 0u;\n    }\n    drop_pointers(h);\n    h->revoked = 1u;\n}",
        "        h->claim.handle = 0u;\n    }\n    lease_unlink(h);\n    drop_pointers(h);\n    h->revoked = 1u;\n}"))
mutant("lock-held-across-platform", "close_order", CORE,
       ("    if (!take_for_close(c, h))\n        return AAR_E_STALE;\n    finish_close(c, h);",
        "    if (!take_for_close(c, h))\n        return AAR_E_STALE;\n    irq_off(c);\n    finish_close(c, h);\n    irq_on(c);"))
mutant("no-lease-gate", "lease", CORE,
       ("    if ((uint32_t)(t - c->disp_alive) > AAR_CORE_STALL_TICKS)\n        return;\n", ""))
mutant("lease-may-be-zero", "lease", CORE,
       ("    if (v == 0u)\n        v = 1u;\n    h->lease = v;", "    h->lease = v;"))
mutant("lease-all-in-one-tick", "lease", CORE,
       ("h->countdown = (uint8_t)(1u + c->opens % AAR_CORE_LEASE_PERIOD_TICKS);", "h->countdown = 1u;"))
mutant("lease-pause-ignored", "lease", CORE,
       ("h->pause_until = t ? t : 1u;", "h->pause_until = 0u;"))
mutant("renew-needs-owner-idle", "lease", CORE,
       ("        if (!h->renew || !cb)\n            continue;", "        if (!h->renew || !cb || h->busy)\n            continue;"))
mutant("revoked-claim-keeps-lease", "revoke", CORE,
       ("    h->renew = 0u;\n    lease_unlink(h);\n    drop_pointers(h);\n    h->revoked = 1u;\n}",
        "    drop_pointers(h);\n    h->revoked = 1u;\n}"))
mutant("revoke-not-noticed", "revoke", CORE,
       ("    if (rc == AAR_E_STALE)\n        revoke(h);", "    if (rc == AAR_E_STALE)\n        (void)0;"))
mutant("revoked-claim-not-released", "revoke", CORE,
       ("    if (h->claim.handle != 0u) {\n        if (h->started", "    if (h->claim.handle != 0u && !h->revoked) {\n        if (h->started"))
# ---- core: reap ------------------------------------------------------------------
mutant("tombstone-freed-too-early", "reap", CORE,
       ("owner_state(user, &h->owner) == AAR_OWNER_ABSENT) {\n                gone = h;",
        "owner_state(user, &h->owner) != AAR_OWNER_ALIVE) {\n                gone = h;"))
mutant("changed-owner-not-reaped", "reap", CORE,
       ("owner_state(user, &h->owner) != AAR_OWNER_ALIVE) {\n                victim = h;",
        "owner_state(user, &h->owner) == AAR_OWNER_ABSENT) {\n                victim = h;"))
mutant("reap-skips-stop", "reap", CORE,
       ("if (h->started && !h->revoked && o->stop)", "if (0 && h->started && !h->revoked && o->stop)"))
# ---- core: tags, load, start ------------------------------------------------------------
mutant("weight-limit-off-by-one", "tags", CORE,
       ("if (data > pi->weight_max)", "if (data > pi->weight_max + 1u)"))
mutant("private-mib-unchecked", "tags", CORE,
       ("    case 1u:\n        if (data > TAG_LIMIT_MIB)\n            return AAR_E_INVAL;",
        "    case 1u:"))
mutant("tags-after-load", "tags", CORE,
       ("    if (h->load_state != AAR_LOAD_NONE || h->started)\n        return AAR_E_BUSY;\n    if (t->set & (1u << 1))",
        "    if (t->set & (1u << 1))"))
mutant("image-limit-unchecked", "load_start", CORE,
       ("if (image_bytes == 0u || (pi->max_image_bytes != 0u && image_bytes > pi->max_image_bytes))",
        "if (image_bytes == 0u)"))
mutant("arg-length-unchecked", "load_start", CORE,
       ("if (pi->max_arg_bytes != 0u && n + 1u > pi->max_arg_bytes)", "if (0)"))
mutant("limits-not-written", "tags", CORE,
       ("    aar_put_be32(h->cb + AAR_CB_WEIGHT, h->weight);\n", ""))
# ---- core: status, messages, events -------------------------------------------------------
mutant("status-ignores-size", "status", CORE,
       ("    memcpy(st, &local, n);\n    st->size = size;", "    memcpy(st, &local, sizeof(local));\n    st->size = size;"))
mutant("generation-read-from-card", "status", CORE,
       ("local.generation = aar_handle_generation(h->claim.handle);", "local.generation = 0u;"))
mutant("no-message-credit", "messages", CORE,
       ("rc = aar_cb_post_msg(&h->cq, h->cb, &h->msg_posted, message->bytes,\n                             ops_of(h)->barrier_acquire, ops_of(h)->barrier_release);",
        "rc = aar_queue_post_msg(&h->cq, message->bytes,\n                             ops_of(h)->barrier_acquire, ops_of(h)->barrier_release);"))
mutant("inbox-overruns", "messages", CORE,
       ("if (h->inbox_tail - h->inbox_head >= AAR_CORE_INBOX) {", "if (h->inbox_tail - h->inbox_head >= AAR_CORE_INBOX + 40u) {"))
mutant("mouse-not-summed", "events", CORE,
       ("h->mouse.dx = sat16((int32_t)h->mouse.dx + ev->dx);", "h->mouse.dx = ev->dx;"))
mutant("mouse-wraps", "events", CORE,
       ("return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);", "return (int16_t)v;"))
mutant("drop-not-counted", "events", CORE,
       ("    h->stats.events_dropped++;\n    cb_add(h, AAR_CB_EVENTS_DROPPED, 1u);", "    cb_add(h, AAR_CB_EVENTS_DROPPED, 1u);"))
mutant("button-change-folded", "events", CORE,
       ("(h->mouse.buttons != ev->buttons || h->mouse.code != ev->code)", "(0)"))
mutant("kick-not-coalesced", "kick", CORE,
       ("    if (h->kick_pending)\n        h->stats.kicks_coalesced++;\n", ""))
mutant("sq-space-not-rung", "kick", CORE,
       ("        h->kick_pending |= AAR_PLAT_KICK_SQ;", "        h->kick_pending |= 0u;"))
mutant("dispatcher-no-kick-retry", "kick", CORE,
       ("            if (claim_live(h) == AAR_OK)\n                kick_send(h);\n", ""))
mutant("dispatcher-no-mouse-flush", "events", CORE,
       ("                (void)mouse_post(h);\n", ""))
mutant("dispatcher-ignores-busy-owner", "kick", CORE,
       ("if (h->state == AAR_H_LIVE && !h->busy) {", "if (h->state == AAR_H_LIVE) {"))
# ---- core: service requests ----------------------------------------------------------------
mutant("reply-without-request", "service", CORE,
       ("    if (!out) {\n        leave(h);\n        return AAR_E_INVAL;\n    }\n    if (local.op == AAR_OP_AUDIO_OPEN",
        "    if (!out) {\n        static aar_outstanding dummy;\n\n        out = &dummy;\n    }\n    if (local.op == AAR_OP_AUDIO_OPEN"))
mutant("request-buffer-not-lent", "service", CORE,
       ("            req->ptr = APTR_OF(p);\n", ""))
mutant("bad-buffer-answered-ok", "service", CORE,
       ("                comp.result = AAR_E_INVAL;\n                if (aar_queue_post_completion",
        "                comp.result = AAR_OK;\n                if (aar_queue_post_completion"))
mutant("outstanding-unlimited", "service", CORE,
       ("        out = out_free(h);\n        if (!out)\n            return 0;",
        "        out = out_free(h);\n        if (!out) {\n            static aar_outstanding spare;\n\n            out = &spare;\n        }"))
mutant("request-flags-lost", "service", CORE,
       ("        req->flags = r.flags;\n", ""))
mutant("reply-flags-lost", "service", CORE,
       ("    comp.flags = (uint16_t)local.reply_flags;", "    comp.flags = 0u;"))
mutant("logs-not-delivered", "logs_and_stats", CORE,
       ("    h->log_sink(h->log_user, (const void *)(uintptr_t)p, n);", "    (void)p;"))
mutant("null-sink-called", "logs_and_stats", CORE,
       ("    if (!h->log_sink || n == 0u) {", "    if (n == 0u) {"))
# ---- core: AARSetLogSink -------------------------------------------------------------------
SINK_SET = "    h->log_sink = sink;\n    h->log_user = user;\n    leave(h);"
mutant("log-sink-no-handle-check", "owner_stale", CORE,
       ("AARLogSink sink, APTR user)\n{\n    int rc = require_live(c, h);\n",
        "AARLogSink sink, APTR user)\n{\n    int rc = AAR_OK;\n"))
mutant("log-sink-set-before-check", "log_sink", CORE,
       ("    int rc = require_live(c, h);\n\n    if (rc != AAR_OK)\n        return rc;\n    h->log_sink = sink;",
        "    int rc;\n\n    h->log_sink = sink;\n    h->log_user = user;\n    rc = require_live(c, h);\n    if (rc != AAR_OK)\n        return rc;\n    h->log_sink = sink;"))
mutant("log-sink-remove-ignored", "log_sink", CORE,
       (SINK_SET, "    if (sink) {\n        h->log_sink = sink;\n        h->log_user = user;\n    }\n    leave(h);"))
mutant("log-sink-user-not-stored", "log_sink", CORE,
       (SINK_SET, "    h->log_sink = sink;\n    leave(h);"))
mutant("log-sink-kept-after-teardown", "log_sink", CORE,
       ("    h->log_sink = NULL; /* nothing reaches the caller's sink or its user again */\n", ""))
mutant("log-user-kept-after-teardown", "log_sink", CORE,
       ("    h->log_user = 0;\n", ""))
mutant("log-sink-latched", "log_sink", CORE,
       ("    h->log_sink(h->log_user, (const void *)(uintptr_t)p, n);",
        "    {\n        static AARLogSink latched;\n\n        if (!latched)\n            latched = h->log_sink;\n        latched(h->log_user, (const void *)(uintptr_t)p, n);\n    }"))
mutant("log-record-delivered-twice", "log_sink", CORE,
       ("    h->log_sink(h->log_user, (const void *)(uintptr_t)p, n);",
        "    h->log_sink(h->log_user, (const void *)(uintptr_t)p, n);\n    h->log_sink(h->log_user, (const void *)(uintptr_t)p, n);"))
mutant("log-sink-nested-call-drops-busy", "log_sink", CORE,
       ("    if (h->busy)\n        h->busy--;", "    h->busy = 0u;"))
# ---- core: audio ---------------------------------------------------------------------------
mutant("audio-query-returns-zero", "audio", CORE,
       ("        *got = avail;\n        leave(h);\n        return AAR_OK;", "        *got = 0u;\n        leave(h);\n        return AAR_OK;"))
mutant("audio-query-reads", "audio", CORE,
       ("    if (frames == 0u) {\n        /* The available query", "    if (frames == 0xFFFFFFFFu) {\n        /* The available query"))
mutant("audio-query-counts-underrun", "audio", CORE,
       ("        *got = avail;\n        leave(h);\n        return AAR_OK;\n    }\n    n = avail < frames ? avail : frames;",
        "        *got = avail;\n        put_le32(ring + AAR_AUDIO_RING_UNDERRUNS,\n                 le32(ring + AAR_AUDIO_RING_UNDERRUNS) + 1u);\n        leave(h);\n        return AAR_OK;\n    }\n    n = avail < frames ? avail : frames;"))
mutant("audio-underrun-always", "audio", CORE,
       ("    if (n < frames)\n        put_le32(ring + AAR_AUDIO_RING_UNDERRUNS,", "    if (n <= frames)\n        put_le32(ring + AAR_AUDIO_RING_UNDERRUNS,"))
mutant("audio-not-attached", "audio", CORE,
       ("if (local.op == AAR_OP_AUDIO_OPEN && local.result == AAR_OK) {", "if (0) {"))
mutant("audio-not-detached", "audio", CORE,
       ("    if (local.op == AAR_OP_AUDIO_CLOSE)\n        audio_detach(h);\n", ""))
# ---- core: shared maps -----------------------------------------------------------------------
mutant("map-cache-off", "shared_map", CORE,
       ("            if (h->maps[i].bytes >= need) {", "            if (0) {"))
mutant("map-flags-ignored", "shared_map", CORE,
       ("bytes == 0u || flags != 0u || haddr == 0u", "bytes == 0u || haddr == 0u"))
mutant("map-generation-unchecked", "shared_map", CORE,
       ("    if (m->reserved[0] != h->claim.handle)\n        return AAR_E_STALE;", ""))
mutant("map-range-unchecked", "shared_map", CORE,
       ("    if (off > m->bytes || len > m->bytes - off)\n        return AAR_E_INVAL;", ""))
mutant("publish-always", "shared_map", CORE,
       ("    if (map->flags & (AAR_MAPF_UNCACHED | AAR_MAPF_COHERENT))\n        return AAR_OK;", ""))
# ---- core: notification scan -------------------------------------------------------------------
mutant("scan-ignores-armed", "notify", CORE,
       ("if (!h->armed || !cb || h->revoked)", "if (!cb || h->revoked)"))
mutant("signal-does-not-arm", "notify", CORE,
       ("        h->armed = 1u;\n        mask = h->signal_mask;", "        mask = h->signal_mask;"))
mutant("signal-every-scan", "notify", CORE,
       ("(uint32_t)(ticks - h->last_signal_tick) >= AAR_CORE_NOTIFY_RESIGNAL_TICKS", "1"))
mutant("end-reported-every-scan", "notify", CORE,
       ("        if (carries_end && answer == AAR_CORE_NOTIFIED)\n            h->ended_notified = 1u;\n", ""))
mutant("fault-reported-as-complete", "notify", CORE,
       ("end_reason == AAR_END_STUCK_CALL || end_reason == AAR_END_LEASE) ?", "end_reason == AAR_END_STUCK_CALL) ?"))
mutant("stale-after-open", "revoked_codes", SHELL,
       ("    rc = aar_core_check(c, h);\n    if (rc != AAR_OK)\n        return rc;\n    if (!path)\n        return AAR_E_INVAL;\n    f = X_FILE_OPEN(path);\n    if (!f)\n        return AAR_E_NOENT;",
        "    if (!path)\n        return AAR_E_INVAL;\n    f = X_FILE_OPEN(path);\n    if (!f)\n        return AAR_E_NOENT;\n    rc = aar_core_check(c, h);\n    if (rc != AAR_OK)\n        return rc;"))
# ---- a notification is done only when it was delivered -----------------------------------
mutant("end-marked-before-delivery", "notify_answers", CORE,
       ("        if (carries_end && answer == AAR_CORE_NOTIFIED)\n", "        if (carries_end)\n"))
mutant("work-marked-before-delivery", "notify_answers", CORE,
       ("        if (answer == AAR_CORE_NOTIFIED || answer == AAR_CORE_NOTIFY_BUSY) {", "        if (1) {"))
mutant("log-sink-vector-does-nothing", "log_sink", SHELL,
       ("    return aar_core_set_log_sink(&g_base->core, h, sink, user);",
        "    (void)h;\n    (void)sink;\n    (void)user;\n    return AAR_OK;"))
mutant("busy-port-counts-as-posted", "notify_terminal", SHELL,
       ("    if (queued)\n        return AAR_CORE_NOTIFY_BUSY;", "    if (queued)\n        return AAR_CORE_NOTIFIED;"))
# ---- the records of a task that is gone are reclaimed whatever their state ----------------
mutant("obsolete-opener-hidden", "opener_states", SHELL,
       ("                if (prior)\n                    prior->next = o->next;\n                else\n                    b->openers = o->next;\n                obsolete = o;",
        "                o->closing = 1u;"))
mutant("open-detaches-before-alloc", "opener_alloc", SHELL,
       ("        X_SEM_RELEASE(&b->lock);\n        fresh = (struct AAROpener *)X_ALLOC(sizeof(*fresh));",
        "        if (o && !prior)\n            b->openers = o->next;\n        X_SEM_RELEASE(&b->lock);\n        fresh = (struct AAROpener *)X_ALLOC(sizeof(*fresh));"))
mutant("reap-skips-closing-openers", "opener_states", SHELL,
       ("        if (shell_owner_state(b, &o->stamp) != AAR_OWNER_ALIVE) {", "        if (!o->closing && shell_owner_state(b, &o->stamp) != AAR_OWNER_ALIVE) {"))
mutant("reap-skips-closing-handles", "opener_states", CORE,
       ("            if ((h->state == AAR_H_LIVE || h->state == AAR_H_CLOSING) &&", "            if ((h->state == AAR_H_LIVE) &&"))
# ---- the dispatcher process is gone before the library is -----------------------------------
mutant("stop-wait-not-retested", "dispatcher", SHELL,
       ("        while (b->disp_process)\n            (void)X_WAIT(X_SINGLE);", "        if (b->disp_process)\n            (void)X_WAIT(X_SINGLE);"))
mutant("start-wait-not-retested", "dispatcher", SHELL,
       ("        while (!b->start_done)\n", "        if (!b->start_done)\n"))
mutant("end-signals-no-stopper", "dispatcher", SHELL,
       ("    shell_wake(b->stop_task); /* none when a break from outside ended the process */",
        "    X_SIGNAL(b->stop_task, X_SINGLE);"))
mutant("end-without-forbid", "lifecycle", SHELL,
       ("    X_FORBID();\n    b->disp_process = NULL;\n    shell_wake(b->stop_task);", "    b->disp_process = NULL;\n    shell_wake(b->stop_task);"))
mutant("expunge-ignores-interrupt", "dispatcher", SHELL,
       ("    return b->library.lib_OpenCnt == 0 && !b->disp_process && !b->dispatcher_up &&\n           !b->vblank_installed;", "    return b->library.lib_OpenCnt == 0;"))
mutant("expunge-does-not-stop", "dispatcher", SHELL,
       ("static int shell_expunge_ready(struct AARBase *b)\n{\n    shell_final_close(b);\n", "static int shell_expunge_ready(struct AARBase *b)\n{\n"))
mutant("wait-not-withdrawn", "single_signal", SHELL,
       ("    *slot = NULL;\n    X_SINGLE_CLEAR();\n    X_PERMIT();", "    *slot = NULL;\n    X_PERMIT();"))
mutant("wake-without-waiter-check", "dispatcher", SHELL,
       ("    if (waiter)\n        X_SIGNAL(waiter, X_SINGLE);", "    X_SIGNAL(waiter, X_SINGLE);"))
mutant("wake-after-waiter-left", "single_signal", SHELL,
       ("    shell_wait_done(&b->stop_task);\n", "    b->stop_task = NULL;\n"))
# ---- a stale answer of the platform revokes the claim and stops its use ---------------------
mutant("kick-stale-ignored", "stale_platform", CORE,
       ("        (void)plat_rc(h, rc);\n    }\n}", "        (void)rc;\n    }\n}"))
mutant("pump-ignores-revoke", "stale_platform", CORE,
       ("        /* A call below may have revoked the claim (the stale rule). */\n        if (claim_live(h) != AAR_OK)\n            return AAR_E_STALE;\n", ""))
mutant("take-log-ignores-revoke", "stale_platform", CORE,
       ("            if (claim_live(h) != AAR_OK) /* its map found the claim gone */\n                return AAR_E_STALE;\n", ""))
mutant("request-map-ignores-revoke", "stale_platform", CORE,
       ("                if (claim_live(h) != AAR_OK) /* the claim is gone: nobody to refuse it to */\n                    return AAR_E_STALE;\n", ""))
mutant("audio-map-ignores-revoke", "stale_platform", CORE,
       ("        if (claim_live(h) != AAR_OK) { /* its map found the claim gone */\n            leave(h);\n            return AAR_E_STALE;\n        }\n", ""))
mutant("poll-after-revoke", "stale_platform", CORE,
       ("            if (claim_live(h) == AAR_OK && c->plat->ops->poll)", "            if (c->plat->ops->poll)"))
mutant("stop-after-revoke", "stale_platform", CORE,
       ("        rc = claim_live(h);\n        if (rc == AAR_OK)\n            rc = plat_rc(h, ops_of(h)->stop(plat_of(h), &h->claim));", "        rc = plat_rc(h, ops_of(h)->stop(plat_of(h), &h->claim));"))
# ---- admission is serialised with the final close and expunge ----------------------------
mutant("admission-skips-ctl", "admission", SHELL,
       ("    X_SEM_OBTAIN(&b->ctl);\n    if (!b->dispatcher_up)\n        rc = shell_dispatcher_start(b);\n    X_SEM_RELEASE(&b->ctl);",
        "    if (!b->dispatcher_up)\n        rc = shell_dispatcher_start(b);"))
mutant("expunge-ignores-reopen", "admission", SHELL,
       ("    return b->library.lib_OpenCnt == 0 && !b->disp_process", "    return !b->disp_process"))
# ---- the leak probe: every failure is recorded, every process joined -----------------------------
mutant("leak-hold-waits-for-asked-ready", "table", LEAK,
       ("wait_count(&child_ready, started,",
        "wait_count(&child_ready, n,"))
mutant("leak-hold-waits-for-asked-exit", "table", LEAK,
       ("wait_count(&child_exited, started,",
        "wait_count(&child_exited, n,"))
mutant("leak-hold-no-release-after-failure", "table", LEAK,
       ("    child_go = 1;\n    if (!wait_count(&child_exited",
        "    if (ok)\n        child_go = 1;\n    if (!wait_count(&child_exited"))
mutant("leak-hold-spawn-failure-not-recorded", "table", LEAK,
       ("(unsigned long)n);\n            fails++;\n            ok = 0;\n            break;",
        "(unsigned long)n);\n            break;"))
mutant("leak-children-spawn-failure-not-recorded", "table", LEAK,
       ("(unsigned long)i);\n            fails++;\n            ok = 0;\n            break;",
        "(unsigned long)i);\n            break;"))
mutant("leak-hold-reports-success", "table", LEAK,
       ("    child_hold = 0;\n    return children_ok() && ok;",
        "    child_hold = 0;\n    return 1;"))
mutant("leak-children-report-success", "table", LEAK,
       ("    return children_ok() && ok;\n}\n\n/*\n * All children",
        "    return 1;\n}\n\n/*\n * All children"))
mutant("leak-wait-never-times-out", "table", LEAK,
       ("if (waited >= AARLEAK_WAIT_TICKS) {",
        "if (0) {"))
mutant("leak-exit-status-ignores-failures", "table", LEAK,
       ("    return fails ? 10 : 0;",
        "    return 0;"))
mutant("leak-child-openlibrary-unchecked", "table", LEAK,
       ("        if (!base)\n            child_fail(CF_OPENLIBRARY);\n        else if (AAROpen(&h, NULL, &reason) != AAR_OK) {",
        "        if (AAROpen(&h, NULL, &reason) != AAR_OK) {"))
mutant("leak-child-aaropen-unchecked", "table", LEAK,
       ("            child_fail(CF_AAROPEN);\n",
        ""))
mutant("leak-child-signal-unchecked", "table", LEAK,
       ("        } else if (child_hold && AARMessageSignal(h) == 0UL)\n            child_fail(CF_SIGNAL);",
        "        } else if (child_hold)\n            (void)AARMessageSignal(h);"))
mutant("leak-child-aarclose-unchecked", "table", LEAK,
       ("    if (h && AARClose(h) != AAR_OK)\n        child_fail(CF_AARCLOSE);",
        "    if (h)\n        (void)AARClose(h);"))
mutant("leak-pair-aaropen-unchecked", "table", LEAK,
       ("        fail(\"AAROpen\", (long)rc);\n        return 0;",
        "        return 0;"))
mutant("leak-pair-aarclose-unchecked", "table", LEAK,
       ("        fail(\"AARClose\", (long)rc);\n        return 0;",
        "        return 0;"))
mutant("leak-createmsgport-unchecked", "table", LEAK,
       ("            if (!port)\n                fail(\"CreateMsgPort\", 0);\n",
        ""))
mutant("leak-ext-unchecked", "table", LEAK,
       ("        if (!ext)\n            fail(\"AARExt\", 0);\n",
        ""))
mutant("leak-list-open-unchecked", "table", LEAK,
       ("                hs[i] = NULL;\n                fail(\"AAROpen\", (long)rc);",
        "                hs[i] = NULL;"))
mutant("leak-list-error-unchecked", "table", LEAK,
       ("            if (lr < 0)\n                fail(\"the claim list\", (long)lr);\n",
        ""))
mutant("leak-list-short-unchecked", "table", LEAK,
       ("        if (ext && (ULONG)steps < opened)\n            fail(\"the claim list is shorter than the handles held\", (long)steps);\n",
        ""))
mutant("leak-list-close-unchecked", "table", LEAK,
       ("                if (rc != AAR_OK)\n                    fail(\"AARClose\", (long)rc);",
        "                (void)rc;"))
mutant("leak-probe-unchecked", "table", LEAK,
       ("    if (rc != AAR_OK)\n        fail(\"AARProbeFile\", (long)rc);",
        "    (void)rc;"))
mutant("leak-dos-open-unchecked", "table", LEAK,
       ("    if (!f) {\n        fail(\"Open\", 0);\n        return;",
        "    if (!f) {\n        return;"))
mutant("leak-dos-seek-end-unchecked", "table", LEAK,
       ("    if (Seek(f, 0L, OFFSET_END) < 0L)\n        fail(\"Seek to the end\", 0);",
        "    (void)Seek(f, 0L, OFFSET_END);"))
mutant("leak-dos-seek-start-unchecked", "table", LEAK,
       ("    if (Seek(f, 0L, OFFSET_BEGINNING) < 0L)\n        fail(\"Seek to the start\", 0);",
        "    (void)Seek(f, 0L, OFFSET_BEGINNING);"))
mutant("leak-dos-read-unchecked", "table", LEAK,
       ("    if (Read(f, buf, sizeof(buf)) < 0L)\n        fail(\"Read\", 0);",
        "    (void)Read(f, buf, sizeof(buf));"))
mutant("leak-dos-close-unchecked", "table", LEAK,
       ("    if (!Close(f))\n        fail(\"Close\", 0);",
        "    Close(f);"))
mutant("leak-file-left-open", "table", LEAK,
       ("    if (!Close(f))\n        fail(\"Close\", 0);",
        "    (void)f;"))
# ---- start needs a load that completed ---------------------------------------------------------
mutant("start-accepts-unfinished-load", "load_failure", CORE,
       ("(h->load_state != AAR_LOAD_LOADED || h->started))\n        rc = AAR_E_INVAL;",
        "(h->load_state == AAR_LOAD_NONE || h->started))\n        rc = AAR_E_INVAL;"))
mutant("load-end-failure-counts-as-loaded", "load_failure", CORE,
       ("        h->load_state = rc == AAR_OK ? AAR_LOAD_LOADED : AAR_LOAD_FAILED;", "        h->load_state = AAR_LOAD_LOADED;"))
mutant("load-end-success-stays-loading", "load_failure", CORE,
       ("        h->load_state = rc == AAR_OK ? AAR_LOAD_LOADED : AAR_LOAD_FAILED;", "        h->load_state = rc == AAR_OK ? AAR_LOAD_LOADING : AAR_LOAD_FAILED;"))
mutant("chunk-failure-not-recorded", "load_failure", CORE,
       ("    if (rc != AAR_OK && h->load_state == AAR_LOAD_LOADING) /* not after a revoke */\n        h->load_state = AAR_LOAD_FAILED;\n", ""))
mutant("start-failure-not-recorded", "load_failure", CORE,
       ("            if (h->load_state == AAR_LOAD_LOADED)\n                h->load_state = AAR_LOAD_FAILED;\n", ""))
mutant("small-staging-not-recorded", "load_failure", CORE,
       ("        h->load_state = AAR_LOAD_FAILED;\n        return AAR_E_IO;", "        return AAR_E_IO;"))
mutant("refused-begin-closes-load", "load_failure", CORE,
       ("        if (rc == AAR_E_NORESOURCE)\n            h->reason = why;\n        return plat_rc(h, rc);\n    }\n    if (!area",
        "        if (rc == AAR_E_NORESOURCE)\n            h->reason = why;\n        h->load_state = AAR_LOAD_FAILED;\n        return plat_rc(h, rc);\n    }\n    if (!area"))
mutant("audio-ring-kept-undelivered", "audio_reply_busy", CORE,
       ("            if (local.op == AAR_OP_AUDIO_OPEN)\n                audio_detach(h);\n            if (prc == AAR_E_BUSY) {", "            if (prc == AAR_E_BUSY) {"))
# ---- size and offset arithmetic ---------------------------------------------------------
mutant("map-length-wraps", "size_arithmetic", CORE,
       ("    if (len > UINT32_MAX - off)\n        return 0;\n", ""))
mutant("map-round-up-wraps", "size_arithmetic", CORE,
       ("    if (end > UINT32_MAX - (AAR_CORE_MAP_STEP - 1u))\n        return 0;\n", ""))
mutant("align-up-wraps", "size_arithmetic", CORE,
       ("    if (v > UINT32_MAX - (g - 1u))\n        return 0;\n", ""))
mutant("handle-size-wraps", "size_arithmetic", CORE,
       ("    if (!align_up(o->claim_priv_bytes, 8u, &bytes) ||\n        bytes > UINT32_MAX - ROUND8((uint32_t)sizeof(*h)))",
        "    if (!align_up(o->claim_priv_bytes, 8u, &bytes))"))
# ---- allocator balance (the card's AvailMem check) -------------------------------------
mutant("handle-memory-not-released", "alloc_balance", CORE,
       ("    c->env.release(c->env.user, h, bytes);\n}", "    (void)bytes;\n}"))
mutant("tombstone-memory-not-released", "alloc_balance", CORE,
       ("        registry_remove(c, gone);\n        handle_free(gone);\n    }\n    return n;",
        "        registry_remove(c, gone);\n    }\n    return n;"))
mutant("opener-record-not-freed", "alloc_balance", SHELL,
       ("    X_SEM_RELEASE(&b->lock);\n    X_FREE(o, sizeof(*o));\n    return 0;\n}",
        "    X_SEM_RELEASE(&b->lock);\n    return 0;\n}"))
# ---- shell ----------------------------------------------------------------------------------------
mutant("signal-bit-per-handle", "lifecycle", SHELL,
       ("    if (o->sigbit < 0) {\n        /* One bit per owner task", "    if (1) {\n        /* One bit per owner task"))
mutant("last-close-keeps-handles", "last_close", SHELL,
       ("    (void)aar_core_close_owner(&b->core);\n    sigbit = o->sigbit;", "    sigbit = o->sigbit;"))
mutant("dispatcher-never-stopped", "lifecycle", SHELL,
       ("        shell_dispatcher_stop_locked(b);\n        if (b->library.lib_OpenCnt == 0)", "        if (b->library.lib_OpenCnt == 0)"))
mutant("signal-bit-leaked", "lifecycle", SHELL,
       ("    if (sigbit >= 0)\n        X_FREE_SIGNAL(sigbit);", ""))
mutant("reap-every-tick", "reap", SHELL,
       ("#define AAR_REAP_PERIOD_TICKS 50u", "#define AAR_REAP_PERIOD_TICKS 1u"))
mutant("dead-openers-kept", "reap", SHELL,
       ("        if (shell_owner_state(b, &o->stamp) != AAR_OWNER_ALIVE) {", "        if (0) {"))
mutant("signals-dead-owner", "dead_owner_no_signal", SHELL,
       ("    alive = X_TASK_LOOKUP_LOCKED(h->owner.id, &now) && stamp_equal(&h->owner, &now);\n    if (alive && h->signal_mask",
        "    alive = 1;\n    if (alive && h->signal_mask"))
mutant("event-left-on-port", "notify", SHELL,
       ("        X_MSG_REMOVE(&h->event.message);", "        (void)0;"))
mutant("event-posted-twice", "notify", SHELL,
       ("    if (queued)\n        return AAR_CORE_NOTIFY_BUSY;\n", ""))
mutant("no-pass", "lease_gate", SHELL,
       ("    aar_core_dispatch(&b->core, shell_notify, b);\n", ""))
mutant("isr-does-nothing", "lease_gate", SHELL,
       ("    aar_core_vblank(&b->core);\n}", "    (void)b;\n}"))
mutant("ctrl-c-ignored", "load_file", SHELL,
       ("        if (X_CTRL_C()) {", "        if (0) {"))
mutant("read-chunk-uncapped", "load_file", SHELL,
       ("        if (n > AAR_LOAD_CHUNK_BYTES)\n            n = AAR_LOAD_CHUNK_BYTES;\n", ""))
mutant("file-left-open", "load_file", SHELL,
       ("    X_FILE_CLOSE(f);\n    if (rc == AAR_OK)\n        rc = aar_core_load_end(c, h);",
        "    if (rc == AAR_OK)\n        rc = aar_core_load_end(c, h);"))
mutant("file-before-owner", "load_file", SHELL,
       ("    rc = aar_core_check(c, h);\n    if (rc != AAR_OK)\n        return rc;\n    if (!path)\n        return AAR_E_INVAL;\n    f = X_FILE_OPEN(path);",
        "    if (!path)\n        return AAR_E_INVAL;\n    f = X_FILE_OPEN(path);"))
mutant("tags-applied-as-walked", "tag_list", SHELL,
       ("        rc = aar_core_tags_add(&g_base->core, &t, item->ti_Tag, item->ti_Data);\n        if (rc != AAR_OK)\n            return rc;",
        "        rc = aar_core_tags_add(&g_base->core, &t, item->ti_Tag, item->ti_Data);\n        if (rc != AAR_OK) {\n            (void)aar_core_set_tags(&g_base->core, h, &t);\n            return rc;\n        }"))
mutant("bad-tag-before-bad-handle", "owner_stale", SHELL,
       ("    rc = aar_core_check(&g_base->core, h);\n    if (rc != AAR_OK)\n        return rc;\n    aar_core_tags_init(&t);", "    aar_core_tags_init(&t);"))
mutant("probe-file-left-open", "probe", SHELL,
       ("    rc = aar_probe_source(file_read, &r, (uint32_t)size, info, NULL);\n    X_FILE_CLOSE(r.file);",
        "    rc = aar_probe_source(file_read, &r, (uint32_t)size, info, NULL);"))
mutant("ext-without-platform", "ext", SHELL,
       ("    if (ensure_dispatcher(g_base) != AAR_OK)\n        return (APTR)0;\n", ""))
mutant("tombstone-kept-after-last-close", "rename", SHELL,
       ("        X_SEM_RELEASE(&b->lock);\n        (void)aar_core_close_owner(&b->core);\n        return 0;",
        "        X_SEM_RELEASE(&b->lock);\n        return 0;"))

# ---- serve: the OS-free handlers (client/serve/aar_serve_core.c) --------------------------
SC = "client/serve/aar_serve_core.c"
SH = "client/serve/aar_serve_core.h"
SL = "client/serve/aar_serve.c"
mutant("root-leaf-unchecked", "serve_core", SC,
       ("    if (!under_root(f, locked)) {\n        d->unlock(d->ctx, locked);\n        *err = AAR_E_ACCES;\n        return 0;\n    }\n    d->unlock(d->ctx, locked);\n    return 1;",
        "    d->unlock(d->ctx, locked);\n    return 1;"))
# Not mutated, because the mutant is equivalent: root-child (the leaf is checked
# again), seek SET above 2 GiB (DOS refuses it), Ctrl-D with break_mask (Wait
# never returns an unmasked signal), taking a request with a full backlog (the
# core takes none while the CQ is full; the rule is tested on its own).
mutant("root-parent-unchecked", "serve_core", SC,
       ("    if (!parent || !under_root(f, parent)) {", "    if (!parent) {"))
mutant("root-target-unchecked", "serve_core", SC,
       ("    if (!f->abs_paths && !under_root(f, locked)) {", "    if (0) {"))
mutant("abs-open-without-flag", "serve_core", SC,
       ("        if (!f->abs_paths)\n            return AAR_E_ACCES;\n        absolute = 1;", "        absolute = 1;"))
mutant("abs-stat-without-flag", "serve_core", SC,
       ("        if (!f->abs_paths) {\n            *err = AAR_E_ACCES;\n            return 0;\n        }\n        *locked = d->lock_abs",
        "        *locked = d->lock_abs"))
mutant("open-without-live-check", "serve_core", SC,
       ("    if (!d->live(d->ctx))\n        return AAR_E_IO;\n    if (!absolute) {", "    if (!absolute) {"))
mutant("open-keeps-handle-after-loss", "serve_core", SC,
       ("    if (!d->live(d->ctx)) {\n        err = AAR_E_IO;\n        goto done;\n    }\n    f->fh[index] = fh;",
        "    f->fh[index] = fh;"))
mutant("open-over-a-used-slot", "serve_core", SC,
       ("    if (f->fh[index])\n        return AAR_E_BUSY;\n", ""))
mutant("open-flags-unchecked", "serve_core", SC,
       ("    if (plan == AAR_SERVE_OPEN_INVALID)\n        return AAR_E_INVAL;\n", ""))
mutant("read-after-loss", "serve_core", SC,
       ("        if (!d->live(d->ctx)) {\n            err = AAR_E_IO;\n            break;\n        }\n        if (d->user_break(d->ctx))",
        "        if (d->user_break(d->ctx))"))
mutant("read-ignores-break", "serve_core", SC,
       ("        if (d->user_break(d->ctx))\n            break;\n", ""))
mutant("transfer-length-unchecked", "serve_core", SC,
       ("    if (left > req->buf_len || (left && !req->buf))\n        return AAR_E_INVAL;",
        "    if (0)\n        return AAR_E_INVAL;", 2))
mutant("write-after-loss", "serve_core", SC,
       ("        if (!d->live(d->ctx)) {\n            err = AAR_E_IO;\n            break;\n        }\n        memcpy(f->io, req->buf + put, chunk);",
        "        memcpy(f->io, req->buf + put, chunk);"))
mutant("write-of-nothing-is-ok", "serve_core", SC,
       ("        if (n == 0) {\n            err = AAR_E_NOSPC;\n            break;\n        }\n", ""))
mutant("short-read-goes-on", "serve_core", SC,
       ("        if ((uint32_t)n < chunk)\n            break;\n    }\n    done->out[AAR_R_FILE_COUNT] = got;",
        "    }\n    done->out[AAR_R_FILE_COUNT] = got;"))
mutant("seek-whence-unchecked", "serve_core", SC,
       ("    } else {\n        return AAR_E_INVAL;\n    }\n    if (target > 2147483647LL", "    } else {\n        mode = 0;\n    }\n    if (target > 2147483647LL"))
mutant("close-forgets-an-undelivered-end", "serve_core", SC,
       ("    if (delivered)\n        f->fh[index] = 0;", "    f->fh[index] = 0;"))
mutant("descriptor-range-unchecked", "serve_core", SC,
       ("if (id < AAR_FD_BASE || id >= AAR_FD_BASE + (int32_t)AAR_SERVE_FD_COUNT) {", "if (id < AAR_FD_BASE) {"))
mutant("exclusive-create-without-probe", "serve_core", SC,
       ("    case AAR_SERVE_OPEN_CREATE_EXCLUSIVE:\n        if (exists) {", "    case AAR_SERVE_OPEN_CREATE_EXCLUSIVE:\n        if (0) {"))
mutant("update-of-a-missing-file", "serve_core", SC,
       ("        if (!exists) {\n            *err = AAR_E_NOENT;\n            goto out;\n        }\n", ""))
mutant("truncate-after-cancel", "serve_core", SC,
       ("            if (!open_step_allowed(d)) {\n                d->fh_end_or_close(d->ctx, fh);\n                *err = AAR_E_IO;\n                goto out;\n            }\n", ""))
mutant("truncate-failure-ignored", "serve_core", SC,
       ("            if (!size_ok) {\n                d->fh_end_or_close(d->ctx, fh);\n                *err = AAR_E_IO;\n                goto out;\n            }\n", ""))
mutant("create-after-cancel", "serve_core", SC,
       ("    if (mode != AAR_SERVE_MODE_OLD && !open_step_allowed(d)) {", "    if (0) {"))
mutant("disk-full-is-io", "serve_core", SC,
       ("    case AAR_SERVE_IOERR_DISK_FULL:\n        return AAR_E_NOSPC;", "    case AAR_SERVE_IOERR_DISK_FULL:\n        return AAR_E_IO;"))
mutant("in-use-is-io", "serve_core", SC,
       ("    case AAR_SERVE_IOERR_OBJECT_IN_USE:\n        return AAR_E_BUSY;", "    case AAR_SERVE_IOERR_OBJECT_IN_USE:\n        return AAR_E_IO;"))
mutant("unix-epoch-missing", "serve_core", SC,
       ("return (uint32_t)AAR_SERVE_UNIX_DELTA + days * 86400u", "return days * 86400u"))
mutant("console-write-while-one-is-out", "serve_core", SC,
       ("    if (c->app_write_active) {\n        done->result = AAR_E_BUSY;\n        return;\n    }", ""))
mutant("console-write-after-failure", "serve_core", SC,
       ("    if (c->failed) {\n        done->result = AAR_E_IO;\n        return;\n    }\n    if (req->buf_len == 0) {", "    if (req->buf_len == 0) {"))
mutant("console-buffer-used-after-loss", "serve_core", SC,
       ("    if (!c->ops.live(c->ops.ctx)) {\n        /* The claim was lost:", "    if (0) {\n        /* The claim was lost:"))
mutant("short-console-write-is-complete", "serve_core", SC,
       ("c->app_write_done >= c->app_write.buf_len)", "c->app_write_done > 0)"))
mutant("console-read-copied-after-loss", "serve_core", SC,
       ("    if (!c->ops.live(c->ops.ctx)) {\n        read_finish(c, AAR_E_IO, 0);\n        return;\n    }", ""))
mutant("console-eof-not-kept", "serve_core", SC,
       ("    if (got <= 0) {\n        c->eof = 1;", "    if (got <= 0) {\n        c->eof = 0;"))
mutant("console-read-longer-than-the-room", "serve_core", SC,
       ("    count = (uint32_t)got < c->read_req.buf_len ? (uint32_t)got :\n                                                  c->read_req.buf_len;",
        "    count = (uint32_t)got;"))
mutant("console-opened-late", "wb_console", SC,
       ("if (app_left != 0u && !c->ops.open(c->ops.ctx)) {", "if (0) {"))
mutant("third-flip-queued", "serve_core", SC,
       ("            if (d->flip_queued) {\n                done->result = AAR_E_BUSY;\n                return;\n            }\n", ""))
mutant("palette-range-unchecked", "serve_core", SC,
       ("count > 256u - first", "count > 256u"))
mutant("palette-first-unchecked", "serve_core", SC,
       ("first >= 256u ||", ""))
mutant("palette-on-a-truecolor-screen", "serve_core", SC,
       ("d->format != AAR_FMT_CLUT8 ||", ""))
mutant("palette-buffer-unchecked", "serve_core", SC,
       (" || req->buf_len < count * 4u", ""))
mutant("display-opened-twice", "serve_core", SC,
       ("        else if (s->display.open)\n            done->result = AAR_E_BUSY;\n", ""))
mutant("display-size-unchecked", "serve_core", SC,
       ("if (w == 0 || h == 0 || w > AAR_SERVE_SCREEN_MAX || h > AAR_SERVE_SCREEN_MAX)", "if (w == 0 || h == 0)"))
mutant("display-format-unchecked", "serve_core", SC,
       ("    if (format != AAR_FMT_RGB565 && format != AAR_FMT_BGRA8888 &&\n        format != AAR_FMT_CLUT8)\n        return AAR_E_INVAL;\n    return AAR_OK;",
        "    return AAR_OK;"))
mutant("flip-of-another-display", "serve_core", SC,
       ("if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id) {\n        done->result = AAR_E_INVAL;\n        return;\n    }\n    if (d->have_two) {",
        "if (!d->open) {\n        done->result = AAR_E_INVAL;\n        return;\n    }\n    if (d->have_two) {"))
mutant("audio-platform-route-served", "serve_core", SC,
       ("if (route != AAR_AUDIO_ROUTE_AHI || rate == 0 || channels != 2u ||", "if (rate == 0 || channels != 2u ||"))
mutant("audio-channels-unchecked", "serve_core", SC,
       ("rate == 0 || channels != 2u ||", "rate == 0 ||"))
mutant("audio-ring-offset-unchecked", "serve_core", SC,
       ("req->buf_token == 0 || req->buf_off != 0 ||", "req->buf_token == 0 ||"))
mutant("audio-reopen-keeps-ahi", "serve_core", SC,
       ("    if (a->open) {\n        a->ops.close(a->ops.ctx);\n        a->open = 0;\n    }\n    if (!a->ops.open(", "    if (!a->ops.open("))
mutant("audio-open-failure-is-ok", "serve_core", SC,
       ("        a->open_failed = 1;\n        done->result = AAR_E_IO;\n        return;", "        a->open_failed = 1;\n        done->result = AAR_OK;\n        return;"))
mutant("audio-chunk-over-a-fiftieth", "serve_core", SC,
       ("    if (tick < frames)\n        frames = tick;\n", ""))
mutant("audio-chunk-over-the-buffer", "serve_core", SC,
       ("    if (fit < frames)\n        frames = fit;\n", ""))
mutant("refused-ring-keeps-ahi-state", "refused_ring", SC,
       ("void aar_serve_audio_refused(struct aar_serve_audio *a)\n{\n    if (a->open) {\n        a->ops.close(a->ops.ctx);\n        a->open = 0;\n    }\n}",
        "void aar_serve_audio_refused(struct aar_serve_audio *a)\n{\n    (void)a;\n}"))
mutant("stop-event-not-retried", "serve_core", SC,
       ("        if (rc == AAR_E_BUSY) {\n            e->stop_pending = 1;", "        if (0) {\n            e->stop_pending = 1;"))
mutant("stop-event-never-flushed", "serve_core", SC,
       ("if (e->stop_pending && e->ops.post(e->ops.ctx, &e->stop) != AAR_E_BUSY)", "if (0 && e->stop_pending && e->ops.post(e->ops.ctx, &e->stop) != AAR_E_BUSY)"))
mutant("unserved-op-is-ok", "serve_core", SC,
       ("        done_result(done, AAR_E_NOSYS);\n        break;\n    }\n    return deferred", "        done_result(done, AAR_OK);\n        break;\n    }\n    return deferred"))
mutant("abort-forgets-a-console-read", "destroy_held", SC,
       ("    if (c->read_active)\n        read_finish(c, AAR_E_IO, 0);\n    if (d->flip_active) {", "    if (d->flip_active) {"))
mutant("abort-forgets-a-flip", "destroy_held", SC,
       ("    if (d->flip_active) {\n        struct aar_serve_req finished = d->flip_req;\n\n        d->flip_active = 0;\n        flip_complete(d, &finished, AAR_E_IO);\n    }\n    if (d->flip_queued) {\n        struct aar_serve_req next = d->flip_next;\n\n        d->flip_queued = 0;\n        flip_complete(d, &next, AAR_E_IO);\n    }\n}",
        "}"))
# ---- serve: the decisions (client/serve/aar_serve_core.h) ----------------------------------
mutant("reply-reserve-off-by-one", "serve_policy", SH,
       ("return backlog_used + AAR_SERVE_REPLY_RESERVE < AAR_SERVE_REPLY_BACKLOG;", "return backlog_used + AAR_SERVE_REPLY_RESERVE <= AAR_SERVE_REPLY_BACKLOG;"))
mutant("launcher-reserve-ignored", "serve_policy", SH,
       ("    uint32_t limit = diagnostic ? AAR_SERVE_LAUNCHER_OUTPUT_BYTES -\n                                  AAR_SERVE_LAUNCHER_STATUS_RESERVE_BYTES :\n                                  AAR_SERVE_LAUNCHER_OUTPUT_BYTES;",
        "    uint32_t limit = AAR_SERVE_LAUNCHER_OUTPUT_BYTES;"))
mutant("launcher-text-after-app-text", "serve_policy", SH,
       ("    if (launcher_bytes)\n        return AAR_SERVE_OUTPUT_LAUNCHER;\n    return app_bytes ? AAR_SERVE_OUTPUT_APP : AAR_SERVE_OUTPUT_NONE;",
        "    if (app_bytes)\n        return AAR_SERVE_OUTPUT_APP;\n    return launcher_bytes ? AAR_SERVE_OUTPUT_LAUNCHER : AAR_SERVE_OUTPUT_NONE;"))
mutant("user-stop-waits-one-grace-only", "serve_policy", SH,
       ("    if (in->user_stop && in->elapsed_ms >= grace * 2u)", "    if (in->user_stop && in->elapsed_ms >= grace * 20u)"))
mutant("lost-claim-keeps-serving", "serve_policy", SH,
       ("    if (in->lost)\n        return 1;", ""))
mutant("stop-ends-while-output-is-pending", "serve_policy", SH,
       ("    if (in->stop_asked && in->elapsed_ms >= grace * 2u &&\n        !blocked)", "    if (in->stop_asked && in->elapsed_ms >= grace * 2u)"))
mutant("console-read-without-a-running-app", "serve_policy", SH,
       ("return state == AAR_STATE_RUNNING && !stop_asked;", "return !stop_asked;"))
mutant("packet-for-a-lost-operation-is-sent", "serve_policy", SH,
       ("return release || (ownership_ok && !ctrl_c_pending);", "return 1;"))
mutant("lost-claim-does-not-abandon-a-packet", "serve_policy", SH,
       ("return ctrl_c || !claim_live;", "return ctrl_c;"))
mutant("any-key-with-control-stops", "serve_policy", SH,
       ("return (qualifiers & control_qualifier) != 0 && code == 0x33u;", "return (qualifiers & control_qualifier) != 0;"))
mutant("step-ignores-a-user-break", "serve_policy", SH,
       ("return request_current && !aar_serve_user_break(ctrl_c_pending, user_stop);", "return request_current;"))
mutant("consumed-break-forgotten", "serve_policy", SH,
       ("return ctrl_c_pending != 0 || user_stop != 0;", "return ctrl_c_pending != 0;"))
mutant("safe-message-count-wraps", "serve_policy", SH,
       ("return pending ? pending - 1u : 0u;", "return pending - 1u;"))
# ---- serve: the loop (client/serve/aar_serve.c) and its backends --------------------------
mutant("lost-claim-not-noticed", "revoke_read_pending", SL,
       ("    return s->handle != NULL && !s->claim_lost;", "    return s->handle != NULL;"))
mutant("user-stop-not-recorded", "ctrl_c_stop", SL,
       ("        s->user_stop = 1;\n        want_stop(s, AAR_SERVE_WHY_BREAK);\n    }\n    /* A Shell", "        want_stop(s, AAR_SERVE_WHY_BREAK);\n    }\n    /* A Shell"))
mutant("ctrl-c-while-the-program-owns-signals", "ctrl_def", SL,
       ("    if ((breaks & SIGBREAKF_CTRL_C) &&\n        ((signals & SIGBREAKF_CTRL_C) || ctrl_c_set())) {", "    if (((signals & SIGBREAKF_CTRL_C) || ctrl_c_set())) {"))
mutant("unserved-group-is-served", "groups", SL,
       ("    return group == 0 || s->cfg.groups == 0 || (s->cfg.groups & group) != 0;", "    return 1;"))
mutant("full-cq-completion-dropped", "full_cq_held", SL,
       ("            AAR_E_BUSY)\n            break;", "            AAR_E_BUSY)\n            i++;"))
mutant("corrupt-queue-not-noticed", "queue_corrupt", SL,
       ("    if (rc == AAR_E_IO && !s->queue_corrupt) {", "    if (0) {"))
mutant("messages-not-drained", "messages_drained", SL,
       ("    if (s->cfg.flags & AAR_SERVEF_KEEP_MESSAGES)\n        return;", "    return;"))
mutant("messages-drained-despite-keep", "keep_messages", SL,
       ("    if (s->cfg.flags & AAR_SERVEF_KEEP_MESSAGES)\n        return;", ""))
mutant("dropped-messages-not-counted", "messages_drained", SL,
       ("            s->dropped_messages++;", "            ;"))
mutant("message-hook-ignored", "message_hook", SL,
       ("        if (AAR_SERVE_HOOK(s, message))\n            s->cfg.hooks->message(s->cfg.user, &m);\n        else\n            s->dropped_messages++;",
        "        s->dropped_messages++;"))
mutant("stop-grace-not-read", "stop_grace", SL,
       ("            s->grace_ms = info.stop_grace_ms;", "            s->grace_ms = 0;"))
mutant("destroy-answers-nothing", "destroy_held", SL,
       ("    aar_serve_core_abort(&s->core);\n", ""))
mutant("destroy-does-not-stop-the-app", "destroy_held", SL,
       ("            leaving != AAR_STATE_IDLE)\n            stop_owned(s);", "            leaving != AAR_STATE_IDLE)\n            (void)0;"))
mutant("log-sink-left-installed", "log_sink_removed", SL,
       ("        (void)claim_note(s, X_AAR_SET_LOG_SINK(s->handle, NULL, NULL));", "        (void)0;"))
mutant("flip-port-not-waited-on", "sigmask", SL,
       ("    if (s->gfx.safe_port)\n        mask |= 1UL << s->gfx.safe_port->mp_SigBit;", ""))
mutant("ahi-port-not-waited-on", "sigmask", SL,
       ("    if (s->ahi.port)\n        mask |= 1UL << s->ahi.port->mp_SigBit;", ""))
mutant("ahi-without-a-heartbeat", "ahi_heartbeat", SL,
       ("        (cfg->poll_us == 0 || cfg->poll_us > 20000UL))", "        (0))"))
mutant("refused-ring-leaves-ahi-open", "refused_ring", SL,
       ("        aar_serve_audio_refused(&s->core.audio);", "        (void)0;"))
mutant("timer-signal-left-set", "files", SL,
       ("        X_SET_SIGNAL(0, 1UL << s->tm.port->mp_SigBit);", "        (void)0;"))
mutant("flags-unchecked", "config", SL,
       ("           (cfg->flags & ~known) == 0 &&\n", ""))
mutant("audio-route-unchecked", "config", SL,
       ("           (cfg->audio_route == AAR_AUDIO_ROUTE_PLATFORM ||\n            cfg->audio_route == AAR_AUDIO_ROUTE_AHI) &&\n", ""))
mutant("joy-ports-unchecked", "config", SL,
       ("           (cfg->joy_ports & ~3UL) == 0 &&\n", ""))
mutant("display-strategy-unchecked", "config", SL,
       ("           cfg->display_strategy <= AAR_SERVE_DISPLAY_SINGLE &&\n", ""))
mutant("reserved-word-unchecked", "config", SL,
       ("           cfg->platform == 0;", "           1;"))
mutant("stale-claim-served", "config", SL,
       ("    rc = claim_note(s, X_AAR_STATUS(h, &st));\n    if (rc != AAR_OK) {\n        serve_free(s);\n        return rc;\n    }",
        "    rc = claim_note(s, X_AAR_STATUS(h, &st));\n    (void)rc;"))
mutant("result-forgets-the-user-stop", "ctrl_c_stop", SL,
       ("(s->user_stop ? AAR_SERVE_R_USER_STOP : 0UL)", "0UL"))
mutant("stop-hook-ignored", "hooks", SL,
       ("    if (AAR_SERVE_HOOK(s, stop_wanted))\n        s->cfg.hooks->stop_wanted", "    if (0)\n        s->cfg.hooks->stop_wanted"))
mutant("status-hook-ignored", "hooks", SL,
       ("    if (AAR_SERVE_HOOK(s, status))\n        s->cfg.hooks->status", "    if (0)\n        s->cfg.hooks->status"))
mutant("log-hook-ignored", "hooks", SL,
       ("    if (AAR_SERVE_HOOK(s, log))\n        s->cfg.hooks->log", "    if (0)\n        s->cfg.hooks->log"))
mutant("hook-table-size-unchecked", "hooks_size", "client/serve/aar_serve_priv.h",
       ("     (s)->cfg.hooks->size >= offsetof(aar_serve_hooks, field) +               \\\n                             sizeof((s)->cfg.hooks->field) &&                 \\\n", ""))
mutant("dos-wait-leaves-the-heartbeat-unarmed", "heartbeat_after_locate", "client/serve/aar_serve_dos.c",
       ("    if (s->cfg.poll_us && !s->in_step)\n        timer_arm(s);\n    if (check_live && !claim_live(s)) {", "    if (check_live && !claim_live(s)) {"))
mutant("destroy-lets-a-dos-wait-arm-the-timer", "heartbeat_after_locate", SL,
       ("    s->cfg.poll_us = 0; /* the DOS waits below must not arm the timer again */\n", ""))
mutant("runtime-log-drops-not-reported", "log_dropped", SL,
       ("    out->runtime_dropped_logs = st.log_dropped;\n", ""))
mutant("runtime-log-drops-forgotten-with-the-claim", "log_dropped", SL,
       ("    s->runtime_dropped_logs = st.log_dropped;\n", ""))
mutant("runtime-log-drops-not-from-the-status", "log_dropped", SL,
       ("    out->runtime_dropped_logs = st.log_dropped;\n", "    out->runtime_dropped_logs = 0;\n"))
mutant("ahi-underruns-not-reported", "ahi_audio", SL,
       ("    out->ahi_underruns = s->ahi.underruns;", "    out->ahi_underruns = 0;"))
mutant("ahi-underruns-counted-twice", "ahi_audio", "client/serve/aar_serve_ahi.c",
       ("    s->ahi.ring_underruns = count;", "    s->ahi.ring_underruns = 0;"))
mutant("workbench-rejects-dostrue", "workbench_project", "client/run/aar_run_workbench.h",
       ("                     (LONG)sizeof(directory)) != 0) {",
        "                     (LONG)sizeof(directory)) > 0) {"))
mutant("a-seen-break-is-forgotten", "run_policy", "client/run/aar_run_policy.h",
       ("    return (latched != 0 || ctrl_c_pending != 0 || user_stop != 0) ? 1 : 0;",
        "    return (ctrl_c_pending != 0 || user_stop != 0) ? 1 : 0;"))
mutant("stack-ignores-the-cli-default", "run_policy", "client/run/aar_run_policy.h",
       ("        if (cli_bytes < bytes)\n            bytes = cli_bytes;", "        (void)cli_bytes;"))
mutant("cli-stack-longs-wrap", "run_policy", "client/run/aar_run_policy.h",
       ("cli_default_longs > UINT32_MAX / 4u ?\n                                 UINT32_MAX : cli_default_longs * 4u;",
        "cli_default_longs * 4u;"))
mutant("dos-wait-in-a-step-arms-the-timer", "timer_ops", "client/serve/aar_serve_dos.c",
       ("    if (s->cfg.poll_us && !s->in_step)", "    if (s->cfg.poll_us)"))
mutant("ahi-late-refill-not-counted", "ahi_gaps", "client/serve/aar_serve_ahi.c",
       ("        if (!submitted && completed && ahi_idle(s))\n            s->ahi.gaps++;", "        if (!submitted && completed && ahi_idle(s))\n            ;"))
mutant("ahi-first-buffer-counted-late", "ahi_gaps", "client/serve/aar_serve_ahi.c",
       ("        if (!submitted && completed && ahi_idle(s))\n            s->ahi.gaps++;", "        if (!submitted && ahi_idle(s))\n            s->ahi.gaps++;"))
mutant("ahi-gaps-not-reported", "ahi_gaps", SL,
       ("    out->ahi_gaps = s->ahi.gaps;", "    out->ahi_gaps = 0;"))
mutant("ahi-queue-is-two-requests-deep", "ahi_stall", "client/serve/aar_serve_priv.h",
       ("#define AAR_SERVE_AHI_BUFFER_COUNT 6u   /* requests in flight: 6 x 20 ms = 120 ms of queued audio */",
        "#define AAR_SERVE_AHI_BUFFER_COUNT 2u   /* requests in flight: 6 x 20 ms = 120 ms of queued audio */"))
mutant("ahi-refills-one-slot-per-pass", "ahi_stall", "client/serve/aar_serve_ahi.c",
       ("        submitted++;\n        available = got < available ? available - got : 0u;\n    }", "        return;\n    }"))
mutant("ahi-chains-to-the-wrong-request", "ahi_audio", "client/serve/aar_serve_ahi.c",
       ("    if (s->ahi.pending[s->ahi.last])\n        link = s->ahi.request[s->ahi.last]; /* the chain's tail */",
        "    link = s->ahi.request[0];"))
# the backends
mutant("nil-takes-no-packets", "nil_output", "client/serve/aar_serve_con.c",
       ("    if (!handle->fh_Type) {", "    if (0) {"))
mutant("output-note-into-the-failed-pipe", "output_error_note", "client/serve/aar_serve_con.c",
       ("        console_user_note(s, note);\n    }\n    s->console_output_failed = 1;", "        console_write_fh(console_output(s), note);\n    }\n    s->console_output_failed = 1;"))
mutant("display-buffers-forced", "display_strategy", "client/serve/aar_serve_gfx.c",
       ("    if (s->cfg.display_strategy == AAR_SERVE_DISPLAY_BUFFERS) {", "    if (1) {"))
mutant("pan3-forced", "display_strategy", "client/serve/aar_serve_gfx.c",
       ("        if (s->cfg.display_strategy <= AAR_SERVE_DISPLAY_PAN3 &&\n            h <= AAR_SERVE_SCREEN_MAX / 3u)", "        if (h <= AAR_SERVE_SCREEN_MAX / 3u)"))
mutant("pan2-forced", "display_strategy", "client/serve/aar_serve_gfx.c",
       ("            if (s->cfg.display_strategy <= AAR_SERVE_DISPLAY_PAN2 &&\n                h <= AAR_SERVE_SCREEN_MAX / 2u)", "            if (h <= AAR_SERVE_SCREEN_MAX / 2u)"))
mutant("input-window-despite-the-flag", "no_input_window", "client/serve/aar_serve_gfx.c",
       ("    if (!(s->cfg.flags & AAR_SERVEF_NO_INPUT_WINDOW)) {", "    if (1) {"))
mutant("buffer-pitch-unchecked", "buffer_pitch", "client/serve/aar_serve_gfx.c",
       ("            } else if (back_pitch != pitch) {", "            } else if (0) {"))
mutant("front-buffer-not-restored", "close_restores", "client/serve/aar_serve_gfx.c",
       ("        if (s->core.display.showing && X_CHANGE_SCREEN_BUFFER(s->gfx.screen, s->gfx.buf_front)) {", "        if (0) {"))
mutant("public-screen-not-restored", "display_buffers", "client/serve/aar_serve_gfx.c",
       ("            X_SCREEN_TO_FRONT(pub);", "            (void)pub;"))
mutant("safe-messages-not-awaited", "destroy_flip_out", "client/serve/aar_serve_gfx.c",
       ("        flip_wait_safe_messages(s);\n        s->gfx.flip_pending = 0;", "        s->gfx.flip_pending = 0;"))
mutant("tall-screen-height-unchecked", "display_pan3_short", "client/serve/aar_serve_gfx.c",
       ("        if (tall && tall->Height >= (WORD)(h * 3u) &&", "        if (tall &&"))
mutant("underrun-counted-every-pass", "ahi_audio", "client/serve/aar_serve_ahi.c",
       ("            if (!submitted && completed && available == 0u && ahi_idle(s)) {", "            if (!submitted && available == 0u && ahi_idle(s)) {"))
mutant("ahi-buffer-reused-while-pending", "audio_chunk", "client/serve/aar_serve_ahi.c",
       ("        slot = ahi_free_slot(s);\n        if (slot < 0)\n            return;", "        slot = 0;"))
mutant("ctrl-d-raw-key-is-e", "raw_key_ctrl_c", "client/serve/aar_serve_input.c",
       ("    case 0x22u: return AAR_BREAK_CTRL_D;", "    case 0x22u: return AAR_BREAK_CTRL_E;"))
mutant("right-button-is-left", "mouse_and_close", "client/serve/aar_serve_input.c",
       ("else if (code == MENUDOWN) { s->in.mouse_buttons |= 2u; id = 2; }", "else if (code == MENUDOWN) { s->in.mouse_buttons |= 1u; id = 2; }"))
mutant("joystick-port-mask-ignored", "joystick", "client/serve/aar_serve_input.c",
       ("        if (!(s->cfg.joy_ports & (1UL << port)))\n            continue;\n", ""))
mutant("close-gadget-ignored", "mouse_and_close", "client/serve/aar_serve_input.c",
       ("            want_stop(s, AAR_SERVE_WHY_WINDOW);", "            (void)0;"))
NAMES = {m[0]: m for m in M}


def build_and_run(work, test_name):
    out = os.path.join(work, "t.bin")
    if test_name in ("shell",):
        srcs = ["client/tests/shell_test.c"]
    else:
        srcs = ["client/tests/core_test.c"]
    return srcs, out


def run_serve(work, test):
    """A serve mutant is built into the three programs that test client/serve:
    serve_core_test and serve_policy_test (assert: a failure is the program's
    name, "serve_core" or "serve_policy") and serve_loop_test (FAIL [name], or the
    test that was running when it crashed). It is KILLED when the test it names
    is among the failures."""
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter",
             "-O1", "-g", "-no-pie", "-fno-pie", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
             "-Iclient/tests", "-Iclient/tests/stubs", "-Iclient/include", "-Iclient/serve",
             "-I" + os.path.join(ROOT, "include")]
    core = "client/serve/aar_serve_core.c"
    programs = [
        ("serve_core", ["client/tests/serve_core_test.c", core]),
        ("serve_policy", ["client/tests/serve_policy_test.c", core]),
        ("serve_loop", ["client/tests/serve_loop_test.c", core, "client/src/aar_core.c",
                        "client/src/refplat.c"]),
    ]
    if test == "run_policy":
        flags.append("-Iclient/run")
        programs = [("run_policy", ["client/tests/run_policy_test.c"])]
    programs += [
    ]
    failed = set()
    for prog, srcs in programs:
        exe = os.path.join(work, prog)
        build = subprocess.run([CC] + flags + srcs + ["-o", exe], cwd=work,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if build.returncode != 0:
            return "BAD", "does not build (%s):\n%s" % (prog, build.stdout[-800:])
        try:
            run = subprocess.run([exe], cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=120, env=dict(os.environ, SERVE_WATCHDOG="20"))
        except subprocess.TimeoutExpired:
            failed.add("(hang in %s)" % prog)
            continue
        if run.returncode == 0:
            continue
        if prog != "serve_loop":
            failed.add(prog)
            continue
        out = run.stdout
        failed |= set(re.findall(r"^FAIL \[([^\]]+)\]", out, re.M))
        last = re.findall(r"^== (\S+)", out, re.M)
        if run.returncode not in (0, 1) and last:
            failed.add(last[-1])  # a crash: the test that was running
    if not failed:
        return "SURVIVED", "every test passed"
    if test in failed:
        extra = sorted(failed - {test})
        return "KILLED", ("(also: %s)" % ", ".join(extra)) if extra else ""
    return "BAD", "failed in %s, not in %s" % (sorted(failed), test)


def run_one(name, test, path, changes, tmp):
    work = os.path.join(tmp, name)
    shutil.copytree(os.path.join(ROOT, "client"), os.path.join(work, "client"),
                    ignore=shutil.ignore_patterns("__pycache__"))
    target = os.path.join(work, path)
    with open(target, "r", newline="") as fh:
        text = fh.read()
    for old, new, count in changes:
        found = text.count(old)
        if found != count:
            return "BAD", "pattern found %d times, expected %d: %r" % (found, count, old[:60])
        text = text.replace(old, new)
    with open(target, "w", newline="") as fh:
        fh.write(text)

    if path.startswith("client/serve/") or path == "client/run/aar_run_workbench.h" or test == "run_policy":
        return run_serve(work, test)

    is_shell = path == SHELL
    is_leak = path == LEAK
    sources = ["client/src/aar_core.c", "client/src/refplat.c"]
    if is_shell:
        sources += ["client/src/aar_probe.c", "client/tests/shell_test.c"]
        tests = ["client/tests/shell_test.c"]
    else:
        sources += ["client/tests/core_test.c"]
    # A core mutant is run against both test programs; the named test fails in one of them.
    programs = []
    if is_leak:
        programs.append(("aarleak_test", ["client/tests/aarleak_test.c"], False))
    elif is_shell:
        programs.append(("shell_test", sources, True))
    else:
        programs.append(("core_test", sources, False))
        programs.append(("shell_test", ["client/src/aar_core.c", "client/src/refplat.c",
                                        "client/src/aar_probe.c", "client/tests/shell_test.c"], True))
    flags = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Wno-unused-function", "-pthread", "-O1", "-g", "-no-pie", "-fno-pie",
             "-Iclient/tests", "-Iclient/tests/stubs", "-Iclient/include",
             "-I" + os.path.join(ROOT, "include")]
    good = os.path.join(tmp, "good.elf")
    for prog, srcs, wants_image in programs:
        exe = os.path.join(work, prog)
        build = subprocess.run([CC] + flags + srcs + ["-o", exe], cwd=work,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if build.returncode != 0:
            return "BAD", "does not build:\n" + build.stdout[-800:]
        cmd = [exe] + ([good] if wants_image else [])
        run = subprocess.run(cmd, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=120)
        out = run.stdout
        if run.returncode == 0:
            continue
        failed = set(re.findall(r"^FAIL \[([^\]]+)\]", out, re.M))
        last = re.findall(r"^== (\S+)", out, re.M)
        if run.returncode not in (0, 1) and last:
            failed.add(last[-1])  # a crash: the test that was running
        if test in failed:
            extra = sorted(failed - {test})
            return "KILLED", prog + (" (also: %s)" % ", ".join(extra) if extra else "")
        return "BAD", "%s failed in %s, not in %s" % (prog, sorted(failed), test)
    return "SURVIVED", "every test passed"


def main(argv):
    if "--list" in argv:
        for m in M:
            print("%-34s %-22s %s" % (m[0], m[1], m[2]))
        return 0
    names = [a for a in argv[1:] if not a.startswith("-")] or [m[0] for m in M]
    for n in names:
        if n not in NAMES:
            print("unknown mutant", n)
            return 2
    bad = 0
    with tempfile.TemporaryDirectory(prefix="aar-mutants.") as tmp:
        img = subprocess.run([sys.executable, os.path.join(ROOT, "client/tests/make_images.py"),
                              os.path.join(tmp, "good.elf")])
        if img.returncode != 0:
            return 2
        for n in names:
            name, test, path, changes = NAMES[n]
            status, why = run_one(name, test, path, changes, tmp)
            print("%-9s %-34s [%s] %s" % (status, name, test, why if status != "KILLED" else why))
            sys.stdout.flush()
            if status != "KILLED":
                bad += 1
    print("mutants: %d run, %d not killed" % (len(names), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
