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
SCALE_FLAGS = ["-DREFPLAT_CLAIMS=240u", "-DREFPLAT_LOAD_STRIDE=0x1000u", "-DREFPLAT_BUF_STRIDE=0x1000u"]

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
       ("owner_look(&memo, owner_state, user, &h->owner) == AAR_OWNER_ABSENT) {\n                gone = h;",
        "owner_look(&memo, owner_state, user, &h->owner) != AAR_OWNER_ALIVE) {\n                gone = h;"))
mutant("changed-owner-not-reaped", "reap", CORE,
       ("owner_look(&memo, owner_state, user, &h->owner) != AAR_OWNER_ALIVE) {\n                victim = h;",
        "owner_look(&memo, owner_state, user, &h->owner) == AAR_OWNER_ABSENT) {\n                victim = h;"))
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
mutant("profile-hz-not-written", "tags", CORE,
       ("    aar_put_be32(h->cb + AAR_CB_PROFILE_HZ, h->profile_hz);\n", ""))
mutant("profile-hz-unchecked", "tags", CORE,
       ("    case 7u:\n        if (data > AAR_PROFILE_HZ_MAX)\n            return AAR_E_INVAL;\n",
        "    case 7u:\n"))
# ---- core: stack snapshot and profile samples -------------------------------------------------
mutant("stack-size-unchecked", "stack", CORE,
       ("        } else if (have > AAR_CB_STACK_MAX) {\n            rc = AAR_E_IO;\n",
        "        } else if (0) {\n            rc = AAR_E_IO;\n"))
mutant("profile-loss-not-counted", "profile_lost", CORE,
       ("gone = behind - usable;", "gone = 0u;"))
mutant("profile-start-not-moved-on-loss", "profile_lost", CORE,
       ("        start = head - usable;\n        behind = usable;", "        behind = usable;"))
mutant("profile-torn-copy-kept", "profile_torn", CORE,
       ("    if (over > usable) {", "    if (0) {"))
mutant("profile-no-cache-acquire", "profile_torn", CORE,
       ("    if (!(mapf & (AAR_MAPF_UNCACHED | AAR_MAPF_COHERENT)) && o->cache_acquire)",
        "    if (0 && !(mapf & (AAR_MAPF_UNCACHED | AAR_MAPF_COHERENT)) && o->cache_acquire)"))
mutant("profile-head-going-back-ignored", "profile_corrupt", CORE,
       ("    if (behind > 0x80000000u)\n        return AAR_E_IO; /* the counter went back */\n", ""))
mutant("profile-newest-slot-trusted", "profile_partial", CORE,
       ("    usable = count - 1u;", "    usable = count;"))
mutant("profile-wrap-unhandled", "profile_wrap", CORE,
       ("    behind = head - start; /* modulo 2^32 */", "    behind = head < start ? 0u : head - start;"))
mutant("profile-ring-size-unchecked", "profile_corrupt", CORE,
       ("    if (bytes < AAR_PROFILE_RING_MIN_BYTES || (bytes & (bytes - 1u)) != 0u)\n        return AAR_E_IO;\n",
        ""))
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
       ("        h->armed = 1u;\n        h->scan = 1u;\n        mask = h->signal_mask;", "        h->scan = 1u;\n        mask = h->signal_mask;"))
mutant("signal-every-scan", "notify", CORE,
       ("(uint32_t)(ticks - h->last_signal_tick) >= AAR_CORE_NOTIFY_RESIGNAL_TICKS", "1"))
mutant("end-reported-every-scan", "notify", CORE,
       ("            if (carries_end && answer == AAR_CORE_NOTIFIED)\n                h->ended_notified = 1u;\n", ""))
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
mutant("revoke-keeps-poll-needed", "stale_platform", CORE,
       ("    h->kick_pending = 0u;\n    h->poll_needed = 0u;\n", "    h->kick_pending = 0u;\n"))
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
# ---- many handles: the table, the work list, the lease lists -----------------------------------
mutant("table-lookup-walks-the-list", "validation", CORE,
       ("    i = htab_slot(c->htab_shift, h);\n    while ((p = c->htab[i]) != NULL && p != h) {\n        i = (i + 1u) & (c->htab_cap - 1u);\n        steps++;\n    }\n    c->stat_lookup_steps += steps;\n    return p;",
        "    (void)i;\n    for (p = c->handles; p && p != h; p = p->next)\n        steps++;\n    c->stat_lookup_steps += steps;\n    return p;"))
mutant("table-delete-leaves-a-hole", "table", CORE,
       ("        c->htab[i] = c->htab[j];\n        i = j;\n",
        ""))
mutant("table-kept-when-empty", "table", CORE,
       ("    if (c->htab_count == 0u)\n        htab_drop(c);\n    env_unlock(c);",
        "    env_unlock(c);"))
mutant("table-entry-kept-after-close", "table", CORE,
       ("    htab_delete(c, h);\n    work_remove_locked(c, h);",
        "    work_remove_locked(c, h);"))
mutant("table-grow-keeps-old-entries-out", "table", CORE,
       ("    for (h = c->handles; h; h = h->next)\n        htab_put(table, cap, shift, h);",
        "    for (h = c->handles; h && h->next; h = h->next)\n        htab_put(table, cap, shift, h);"))
mutant("lease-all-on-one-list", "lease", CORE,
       ("        if (c->lease_count[i] < c->lease_count[b])",
        "        if (0)"))
mutant("lease-phase-stuck", "lease", CORE,
       ("    c->lease_phase = phase + 1u < AAR_CORE_LEASE_PERIOD_TICKS ? phase + 1u : 0u;",
        "    c->lease_phase = phase;"))
mutant("lease-unlink-keeps-count", "close_order", CORE,
       ("        c->lease_count[b]--;\n",
        ""))
mutant("lease-unlink-keeps-slot", "close_order", CORE,
       ("        h->lease_next = h->lease_prev = NULL;\n        h->lease_slot = 0u;",
        "        h->lease_next = h->lease_prev = NULL;"))
mutant("work-note-ignored", "pass", CORE,
       ("    irq_off(c);\n    if (work_wanted(h))\n        work_add_locked(c, h);\n    irq_on(c);\n}\n\n/* The pass has finished",
        "    (void)c;\n}\n\n/* The pass has finished"))
mutant("work-retire-never", "pass", CORE,
       ("    irq_off(c);\n    if (!work_wanted(h))\n        work_remove_locked(c, h);\n    irq_on(c);",
        "    (void)c;\n    (void)h;"))
mutant("work-retire-keeps-polling-handles", "pass", CORE,
       ("(h->kick_pending | h->mouse_pending | h->poll_needed | (h->armed & h->scan)) != 0u;",
        "(h->kick_pending | h->mouse_pending | (h->armed & h->scan)) != 0u;"))
mutant("work-retire-forgets-mouse", "pass", CORE,
       ("(h->kick_pending | h->mouse_pending | h->poll_needed | (h->armed & h->scan)) != 0u;",
        "(h->kick_pending | h->poll_needed | (h->armed & h->scan)) != 0u;"))
mutant("work-retire-forgets-kick", "pass", CORE,
       ("(h->kick_pending | h->mouse_pending | h->poll_needed | (h->armed & h->scan)) != 0u;",
        "(h->mouse_pending | h->poll_needed | (h->armed & h->scan)) != 0u;"))
mutant("work-retire-forgets-armed", "pass", CORE,
       ("(h->kick_pending | h->mouse_pending | h->poll_needed | (h->armed & h->scan)) != 0u;",
        "(h->kick_pending | h->mouse_pending | h->poll_needed) != 0u;"))
mutant("work-poll-answer-ignored", "pass", CORE,
       ("                h->poll_needed = c->plat->ops->poll(c->plat, &h->claim) != 0;",
        "                h->poll_needed = (c->plat->ops->poll(c->plat, &h->claim), 0);"))
mutant("work-kick-sets-no-poll", "pass", CORE,
       ("        if (o->poll) /* the request may still be in flight: the pass retires it */\n            h->poll_needed = 1u;",
        ""))
mutant("work-poll-set-without-platform-poll", "work_list", CORE,
       ("        if (o->poll) /* the request may still be in flight: the pass retires it */\n            h->poll_needed = 1u;",
        "        h->poll_needed = 1u;"))
mutant("work-close-leaves-entry", "pass", CORE,
       ("    htab_delete(c, h);\n    work_remove_locked(c, h);",
        "    htab_delete(c, h);"))
mutant("work-dead-handle-stays", "work_list", CORE,
       ("        if (h->state != AAR_H_LIVE) {\n            work_retire(c, h);\n            continue;\n        }",
        "        if (h->state != AAR_H_LIVE)\n            continue;"))
mutant("work-message-signal-not-added", "pass", CORE,
       ("        mask = h->signal_mask;\n        if (work_wanted(h))\n            work_add_locked(c, h);",
        "        mask = h->signal_mask;"))
mutant("work-open-armed-not-added", "work_list", CORE,
       ("    if (work_wanted(h))\n        work_add_locked(c, h); /* armed at open: a notify port was given */",
        ""))
mutant("work-full-sq-not-noted", "kick", CORE,
       ("        h->kick_pending |= AAR_PLAT_KICK_SQ;\n        work_note(h);",
        "        h->kick_pending |= AAR_PLAT_KICK_SQ;"))
mutant("work-mouse-busy-not-noted", "pass", CORE,
       ("    } else {\n        work_note(h); /* the CQ is full: the pass posts it when there is room */\n    }",
        "    }"))
mutant("work-kick-not-noted", "pass", CORE,
       ("    kick_send(h);\n    work_note(h); /* a bell that was busy, or still in flight, is the pass's */",
        "    kick_send(h);"))
mutant("work-pass-takes-idle-bg", "pass", CORE,
       ("        if ((h->kick_pending | h->mouse_pending | h->poll_needed) != 0u && bg_take(c, h)) {",
        "        if (bg_take(c, h)) {"))
# ---- event counts: armed handles that have nothing to do leave the work list ---------------------
mutant("events-idle-armed-stays-listed", "events", CORE,
       ("            h->scan = 0u;\n            if (h->armed && !h->revoked", "            if (h->armed && !h->revoked"))
mutant("events-pending-not-kept", "events", CORE,
       ("        if (events == AAR_PLAT_EVENTS_NONE || (left & SCAN_PENDING) ||", "        if (events == AAR_PLAT_EVENTS_NONE ||"))
mutant("events-undelivered-end-forgotten", "events", CORE,
       ("            ((left & SCAN_ENDED) && !h->ended_notified))\n            h->scan = 1u;", "            0)\n            h->scan = 1u;"))
mutant("events-announced-end-kept", "events", CORE,
       ("((left & SCAN_ENDED) && !h->ended_notified))\n            h->scan = 1u;", "(left & SCAN_ENDED))\n            h->scan = 1u;"))
mutant("events-news-never-scans", "events", CORE,
       ("        if (o->claim_news(c->plat, &h->claim))\n            scan_next(c, h);", "        (void)o->claim_news(c->plat, &h->claim);"))
mutant("events-news-scans-every-handle", "events", CORE,
       ("        if (o->claim_news(c->plat, &h->claim))\n            scan_next(c, h);", "        (void)o->claim_news(c->plat, &h->claim);\n        scan_next(c, h);"))
mutant("events-claim-not-asked-before-scan", "events", CORE,
       ("                c->stat_pass_news++;\n                (void)o->claim_news(c->plat, &h->claim);\n", ""))
mutant("events-quiet-sample-walks", "events", CORE,
       ("        if (events != AAR_PLAT_EVENTS_NONE && events != AAR_PLAT_EVENTS_QUIET)\n            events = AAR_PLAT_EVENTS_NEWS;",
        "        if (events != AAR_PLAT_EVENTS_NONE)\n            events = AAR_PLAT_EVENTS_NEWS;"))
mutant("events-lost-not-rearmed", "events", CORE,
       ("    if (events == AAR_PLAT_EVENTS_NONE && c->events != AAR_PLAT_EVENTS_NONE)\n        events_off(c);\n", ""))
mutant("events-signal-not-scanned", "events", CORE,
       ("        h->armed = 1u;\n        h->scan = 1u;", "        h->armed = 1u;"))
mutant("events-open-not-scanned", "events", CORE,
       ("    h->scan = 1u; /* opened or armed: scanned at least once */\n", ""))
mutant("events-claim-news-unchecked", "events", CORE,
       ("o->events_sample && o->claim_news) {", "o->events_sample) {"))
mutant("events-sampled-before-attach", "events", CORE,
       ("    if (c->attach_rc == AAR_OK && o->events_sample && o->claim_news) {", "    if (o->events_sample && o->claim_news) {"))
mutant("events-wrapped-count-is-no-change", "events", "client/src/refplat.c",
       ("if (rc->ev_asked && rc->ev_seen == now)", "if (rc->ev_asked && rc->ev_seen >= now)"))
mutant("events-unknown-group-ignores-global", "events", "client/src/refplat.c",
       ("refplat.ev_sample[1u + group] : refplat.ev_sample[0];", "refplat.ev_sample[1u + group] : 0u;"))
mutant("reap-asks-per-handle", "reap", CORE,
       ("    if (!m->have || m->stamp.id != o->id || m->stamp.name != o->name ||\n        m->stamp.number != o->number || m->stamp.kind != o->kind) {",
        "    if (1) {"))
mutant("reap-memo-ignores-owner", "reap", CORE,
       ("    if (!m->have || m->stamp.id != o->id || m->stamp.name != o->name ||\n        m->stamp.number != o->number || m->stamp.kind != o->kind) {",
        "    if (!m->have) {"))
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
       ("    if (!under_root(f, locked)) {\n        d->unlock(d->ctx, locked);\n        *err = AAR_E_ACCES;\n        return 0;\n    }\n    *out = locked;",
        "    if (0) {\n        d->unlock(d->ctx, locked);\n        *err = AAR_E_ACCES;\n        return 0;\n    }\n    *out = locked;"))
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
       ("        if (!open_step_allowed(d)) {\n            err = AAR_E_IO;\n            break;\n        }\n        memcpy(f->io, req->buf + put, chunk);",
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
       ("    if (d->flip_active) {\n        struct aar_serve_req finished = d->flip_req;\n\n        d->flip_active = 0;\n        flip_complete(d, &finished, AAR_E_IO);\n    }\n    if (d->flip_queued) {\n        struct aar_serve_req next = d->flip_next;\n\n        d->flip_queued = 0;\n        flip_complete(d, &next, AAR_E_IO);\n    }\n    vblank_finish(d, AAR_E_IO);\n}",
        "    vblank_finish(d, AAR_E_IO);\n}"))
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
       ("return request_current && !stopping;", "return request_current;"))
mutant("consumed-break-forgotten", "serve_policy", SH,
       ("return ctrl_c_pending != 0 || user_stop != 0;", "return ctrl_c_pending != 0;"))
mutant("safe-message-count-wraps", "serve_policy", SH,
       ("return pending ? pending - 1u : 0u;", "return pending - 1u;"))
# ---- serve: the loop (client/serve/aar_serve.c) and its backends --------------------------
mutant("lost-claim-not-noticed", "revoke_read_pending", SL,
       ("    return s->handle != NULL && !s->claim_lost;", "    return s->handle != NULL;"))
mutant("user-stop-not-recorded", "ctrl_c_stop", SL,
       ("        s->user_stop = 1;\n        want_stop(s, AAR_SERVE_WHY_BREAK);\n    }\n    /* A Shell", "        want_stop(s, AAR_SERVE_WHY_BREAK);\n    }\n    /* A Shell"))
# Also equivalent, so not run: the `breaks &` before ctrl_c_set(s) in aar_serve_step
# (ctrl_c_set(s) and the Wait mask already leave Ctrl-C out without break_mask).
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
       ("           cfg->platform == 0 &&\n           hooks_valid(cfg);", "           hooks_valid(cfg);"))
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
       ("    if (!s->closing && !s->in_step)\n        timer_arm_for(s, timed_wake_us(s));\n    if (check_live && !claim_live(s)) {", "    if (check_live && !claim_live(s)) {"))
mutant("destroy-lets-a-dos-wait-arm-the-timer", "heartbeat_after_locate", SL,
       ("    s->closing = 1; /* the DOS waits below must not arm the timer again */\n", ""))
mutant("runtime-log-drops-not-reported", "log_dropped", SL,
       ("    out->runtime_dropped_logs = st.log_dropped;\n", ""))
mutant("runtime-log-drops-forgotten-with-the-claim", "log_dropped", SL,
       ("    s->runtime_dropped_logs = st.log_dropped;\n", ""))
mutant("runtime-log-drops-not-from-the-status", "log_dropped", SL,
       ("    out->runtime_dropped_logs = st.log_dropped;\n", "    out->runtime_dropped_logs = 0;\n"))
mutant("ahi-underruns-not-reported", "ahi_gaps", SL,
       ("    out->ahi_underruns = s->ahi.underruns;", "    out->ahi_underruns = 0;"))
mutant("ahi-underruns-counted-twice", "ahi_gaps", "client/serve/aar_serve_ahi.c",
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
mutant("tooltype-aar-run-icon-beats-the-project", "run_policy", "client/run/aar_run_policy.h",
       ("    if (!value && chain->tool)", "    if (chain->tool)"))
mutant("document-root-matches-a-sibling-prefix", "run_policy", "client/run/aar_run_policy.h",
       ("            else if (path[rn] == '/')", "            else"))
mutant("unclosed-args-quote-accepted", "run_policy", "client/run/aar_run_policy.h",
       ("            result = AAR_RUN_ARGS_QUOTE;", "            result = AAR_RUN_ARGS_OK;"))
mutant("profile-files-in-the-wrong-order", "run_policy", "client/run/aar_run_policy.h",
       ('{".a64.elf", ".a32.elf"};', '{".a32.elf", ".a64.elf"};'))
mutant("a-drawer-can-be-the-app", "run_policy", "client/run/aar_run_policy.h",
       ("    return !is_directory && probe_rc != AAR_E_INVAL;", "    return probe_rc != AAR_E_INVAL;"))
mutant("default-root-keeps-the-slash", "run_policy", "client/run/aar_run_policy.h",
       ("        cut--; /* the slash is not part of the directory's name */", "        ;"))
mutant("drawer-icon-path-keeps-the-slash", "run_policy", "client/run/aar_run_policy.h",
       ("    while (n > 0 && dir[n - 1] == '/')\n        n--;\n    if (n == 0 || n + 1 > cap)",
        "    if (n == 0 || n + 1 > cap)"))
mutant("dos-wait-in-a-step-arms-the-timer", "timer_ops", "client/serve/aar_serve_dos.c",
       ("    if (!s->closing && !s->in_step)", "    if (!s->closing)"))
mutant("ahi-dry-not-counted", "ahi_gaps", "client/serve/aar_serve_ahi.c",
       ("            s->ahi.gaps++;", "            ;"))
mutant("ahi-end-of-a-sound-counted-as-dry", "ahi_audio", "client/serve/aar_serve_ahi.c",
       ("    if (!s->ahi.tail && ahi_idle(s) && completed &&", "    if (ahi_idle(s) && completed &&"))
mutant("ahi-gaps-not-reported", "ahi_gaps", SL,
       ("    out->ahi_gaps = s->ahi.gaps;", "    out->ahi_gaps = 0;"))
mutant("ahi-six-requests-in-flight", "ahi_audio", "client/serve/aar_serve_priv.h",
       ("#define AAR_SERVE_AHI_BUFFER_COUNT 2u   /* requests in flight: the guide's double buffering */",
        "#define AAR_SERVE_AHI_BUFFER_COUNT 6u   /* requests in flight: the guide's double buffering */"))
mutant("ahi-chunk-is-a-fiftieth", "serve_core", SC,
       ("    uint32_t tick = rate / 50u * 3u; /* 60 ms */", "    uint32_t tick = rate / 50u;"))
mutant("ahi-offset-left-from-the-last-use", "ahi_stall", "client/serve/aar_serve_ahi.c",
       ("    request->ahir_Std.io_Offset = 0;", "    if (!link)\n        request->ahir_Std.io_Offset = 0;"))
mutant("ahi-starts-without-prebuffer", "ahi_slow_ring", "client/serve/aar_serve_ahi.c",
       ("        if (s->ahi.rsv_fill < 2u * ahi_request_frames(s) && !stalled)", "        if (s->ahi.rsv_fill == 0u)"))
mutant("ahi-short-requests-without-a-stall", "ahi_gaps", "client/serve/aar_serve_ahi.c",
       ("    submitted = ahi_queue(s, stalled);", "    submitted = ahi_queue(s, 1);"))
mutant("ahi-no-last-request-when-the-app-stops", "ahi_audio", "client/serve/aar_serve_ahi.c",
       ("    stalled = s->ahi.rsv_fill != 0u && left == 0u &&", "    stalled = 0 && s->ahi.rsv_fill != 0u && left == 0u &&"))
mutant("ahi-close-drops-the-tail", "ahi_slow_ring", "client/serve/aar_serve_ahi.c",
       ("    if (s->ahi.rsv_fill != 0u || !ahi_idle(s)) {\n        s->ahi.draining = 1;\n        return;\n    }\n    ahi_close(s);", "    ahi_close(s);"))
mutant("ahi-min-frames-not-tracked", "ahi_gaps", "client/serve/aar_serve_ahi.c",
       ("    if (s->ahi.requests == 1u || frames < s->ahi.min_frames)", "    if (s->ahi.requests == 1u)"))
mutant("ahi-requests-not-counted", "ahi_audio", "client/serve/aar_serve_ahi.c",
       ("    s->ahi.requests++;\n", ""))
mutant("ahi-chains-to-the-wrong-request", "ahi_audio", "client/serve/aar_serve_ahi.c",
       ("    if (s->ahi.pending[s->ahi.last])\n        link = s->ahi.request[s->ahi.last]; /* the other request: still queued */",
        "    link = s->ahi.request[0];"))
# ---- serve: the embedder's contract -----------------------------------------------------
mutant("repl-display-not-recorded", "repl_display", SC,
       ("            if (done->result == AAR_OK && !s->display.open)\n                display_adopt(&s->display, req, done);", "            ;"))
mutant("repl-display-without-a-front-token-opens", "repl_display_min", SC,
       ("    if (front == 0u || pitch == 0u) {", "    if (0) {"))
mutant("repl-display-two-buffers-without-change-buffer", "repl_display_min", SC,
       ("    int two = d->ops.change_buffer != NULL && back != 0u;", "    int two = back != 0u;"))
mutant("repl-display-never-pans", "repl_display_pan", SC,
       ("    int pan = !two && d->ops.pan != NULL && back != 0u;", "    int pan = 0;"))
mutant("display-close-without-a-close-op", "repl_display_min", SC,
       ("    /* The close waits for the SafeMessages of flips still in flight. */\n    if (d->ops.close)\n        d->ops.close(d->ops.ctx);",
        "    /* The close waits for the SafeMessages of flips still in flight. */\n    d->ops.close(d->ops.ctx);"))
mutant("palette-without-load-rgb32", "repl_display_min", SC,
       ("    if (!d->ops.load_rgb32) {\n        done->result = AAR_E_NOSYS;\n        return;\n    }\n", ""))
mutant("flip-waits-for-a-missing-wait-tof", "repl_display_min", SC,
       ("    if ((d->flags & AAR_DISPLAY_VSYNC) && d->ops.wait_tof)", "    if (d->flags & AAR_DISPLAY_VSYNC)"))
mutant("write-done-without-a-write", "repl_console", SC,
       ("    if (!c->write_pending)\n        return; /* nothing was started: a completion without a write is ignored */\n", ""))
mutant("repl-console-failure-not-handled", "repl_console", SL,
       ("    want_stop(s, AAR_SERVE_WHY_CONSOLE);\n    s->console_output_failed = 1;\n    if (REPL_CON(s)->output_error)",
        "    if (REPL_CON(s)->output_error)"))
mutant("repl-console-error-hook-required", "repl_console_quiet", SL,
       ("    if (REPL_CON(s)->output_error)\n        REPL_CON(s)->output_error(REPL_CON(s)->ctx);", "    REPL_CON(s)->output_error(REPL_CON(s)->ctx);"))
mutant("repl-console-open-required", "repl_console", SL,
       ("    return REPL_CON(s)->open ? REPL_CON(s)->open(REPL_CON(s)->ctx) : 1;", "    return REPL_CON(s)->open(REPL_CON(s)->ctx);"))
mutant("repl-console-read-allowed-required", "repl_console_read", SL,
       ("    return REPL_CON(s)->read_allowed ? REPL_CON(s)->read_allowed(REPL_CON(s)->ctx) : 1;", "    return REPL_CON(s)->read_allowed(REPL_CON(s)->ctx);"))
mutant("destroy-waits-for-a-replacement-console", "repl_console_destroy", SL,
       ("    if (!AAR_SERVE_HOOK(s, console))\n        console_read_finish(s);", "    console_read_finish(s);"))
mutant("destroy-forgets-a-replacement-display", "repl_display", SL,
       ("    else if (s->core.display.open && s->core.display.ops.close)\n        s->core.display.ops.close(s->core.display.ops.ctx);", ""))
mutant("display-table-without-open-accepted", "hooks_tables", SL,
       ("        h->display && !h->display->open)", "        0)"))
mutant("console-table-without-write-accepted", "hooks_tables", SL,
       ("(!h->console->write_start || !h->console->read_start || !h->console->live))", "(!h->console->read_start || !h->console->live))"))
mutant("console-table-without-read-accepted", "hooks_tables", SL,
       ("(!h->console->write_start || !h->console->read_start || !h->console->live))", "(!h->console->write_start || !h->console->live))"))
mutant("console-table-without-live-accepted", "hooks_tables", SL,
       ("(!h->console->write_start || !h->console->read_start || !h->console->live))", "(!h->console->write_start || !h->console->read_start))"))
mutant("no-timed-wake-while-a-stop-is-pending", "poll0_stop", SL,
       ("        if ((s->stop_asked || s->events.stop_pending) && us > 10000UL)\n            us = 10000UL;\n", ""))
mutant("no-watchdog-without-a-heartbeat", "poll0_claim", SL,
       ("    us = AAR_SERVE_WATCHDOG_US;", "    us = 900000UL;"))
mutant("no-timed-wake-for-a-completion-backlog", "poll0_backlog", SL,
       ("        if (s->reply_n)\n            us = 1000UL;\n", ""))
mutant("step-arms-the-timer-only-with-a-heartbeat", "poll0_stop", SL,
       ("    timer_arm_for(s, timed_wake_us(s));\n    return AAR_SERVE_CONTINUE;", "    if (s->cfg.poll_us)\n        timer_arm(s);\n    return AAR_SERVE_CONTINUE;"))
mutant("sigmask-ignores-a-timed-wake", "poll0_mask", SL,
       ("    if (s->tm.port && (s->cfg.poll_us || s->tm.pending))", "    if (s->tm.port && s->cfg.poll_us)"))
mutant("a-dos-wait-takes-the-break-the-program-owns", "break_mask_dos", "client/serve/aar_serve_dos.c",
       ("    mask = (1UL << port->mp_SigBit) | (s->cfg.break_mask & SIGBREAKF_CTRL_C);", "    mask = (1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C;"))
mutant("ctrl-c-looked-at-without-break-mask", "break_mask_dos", SL,
       ("    return (s->cfg.break_mask & SIGBREAKF_CTRL_C) != 0 &&\n           (X_SET_SIGNAL(0, 0) & SIGBREAKF_CTRL_C) != 0;", "    return (X_SET_SIGNAL(0, 0) & SIGBREAKF_CTRL_C) != 0;"))
mutant("window-ctrl-c-stops-without-break-mask", "break_mask_keys", "client/serve/aar_serve_input.c",
       ("                (s->cfg.break_mask & SIGBREAKF_CTRL_C) &&\n", ""))
mutant("window-ctrl-d-break-without-break-mask", "break_mask_keys", "client/serve/aar_serve_input.c",
       ("            if (signal && !(s->cfg.break_mask & break_signal_bit(signal)))\n                signal = 0;\n", ""))
mutant("output-stream-takes-the-input", "con_defaults_out", "client/serve/aar_serve_con.c",
       ("    return s->con.owned && s->con.handle ? s->con.handle : X_INPUT();", "    return s->con.handle ? s->con.handle : X_INPUT();"))
mutant("opened-window-is-not-read", "con_defaults_spec", "client/serve/aar_serve_con.c",
       ("    return s->con.owned && s->con.handle ? s->con.handle : X_INPUT();", "    return X_INPUT();"))
mutant("input-stream-takes-the-output", "con_defaults_in", "client/serve/aar_serve_con.c",
       ("    return s->con.handle ? s->con.handle : X_OUTPUT();", "    return s->con.handle ? s->con.handle : s->con.in ? s->con.in : X_OUTPUT();"))
# ---- serve: confinement and stops --------------------------------------------------------
mutant("root-target-skipped-with-abs", "serve_core", SC,
       ("    if (!under_root(f, locked)) {\n        d->unlock(d->ctx, locked);\n        *err = AAR_E_ACCES;\n        return 0;\n    }\n    *out = locked;",
        "    if (!f->abs_paths && !under_root(f, locked)) {\n        d->unlock(d->ctx, locked);\n        *err = AAR_E_ACCES;\n        return 0;\n    }\n    *out = locked;"))
mutant("mutation-ignores-every-stop-but-a-break", "serve_core", SC,
       ("    return aar_serve_mutation_allowed(d->live(d->ctx), d->stopping(d->ctx));",
        "    return aar_serve_mutation_allowed(d->live(d->ctx), d->user_break(d->ctx));"))
mutant("write-runs-while-stopping", "serve_core", SC,
       ("        if (!open_step_allowed(d)) {\n            err = AAR_E_IO;\n            break;\n        }\n        memcpy(f->io, req->buf + put, chunk);",
        "        if (!d->live(d->ctx)) {\n            err = AAR_E_IO;\n            break;\n        }\n        memcpy(f->io, req->buf + put, chunk);"))
mutant("stopping-ignores-an-asked-stop", "serve_policy", SH,
       ("aar_serve_user_break(ctrl_c_pending, user_stop) || stop_asked != 0 ||", "aar_serve_user_break(ctrl_c_pending, user_stop) ||"))
mutant("stopping-ignores-the-platform-stopping", "serve_policy", SH,
       ("           app_state == AAR_STATE_STOPPING || app_state == AAR_STATE_FAULTED;", "           app_state == AAR_STATE_FAULTED;"))
mutant("stopping-ignores-the-platform-fault", "serve_policy", SH,
       ("           app_state == AAR_STATE_STOPPING || app_state == AAR_STATE_FAULTED;", "           app_state == AAR_STATE_STOPPING;"))
mutant("dos-stopping-ignores-the-asked-stop", "stop_files", "client/serve/aar_serve_dos.c",
       ("s->user_stop, s->stop_asked,\n                              s->app_state);", "s->user_stop, 0,\n                              s->app_state);"))
mutant("dos-stopping-uses-a-stale-state", "stop_platform_end", "client/serve/aar_serve_dos.c",
       ("    if (claim_state(s, &state, &end_reason) == AAR_OK)\n        s->app_state = state;\n", ""))
mutant("requester-text-is-the-format", "requester_format", SL,
       ("    es.es_TextFormat = (UBYTE *)\"%s\";", "    es.es_TextFormat = (UBYTE *)msg;"))
# the backends
mutant("nil-takes-no-packets", "nil_output", "client/serve/aar_serve_con.c",
       ("    if (!handle->fh_Type) {", "    if (0) {"))
mutant("output-note-into-the-failed-pipe", "output_error_note", "client/serve/aar_serve_con.c",
       ("        console_user_note(s, note);\n    }\n    s->console_output_failed = 1;", "        console_write_fh(console_output(s), note);\n    }\n    s->console_output_failed = 1;"))
mutant("display-buffers-forced", "display_strategy", "client/serve/aar_serve_gfx.c",
       ("    if (!scale_screen && s->cfg.display_strategy == AAR_SERVE_DISPLAY_BUFFERS) {", "    if (!scale_screen) {"))
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
mutant("ahi-ring-underrun-not-read", "ahi_gaps", "client/serve/aar_serve_ahi.c",
       ("        if (left == 0u) {", "        if (0) {"))
mutant("ahi-buffer-reused-while-pending", "audio_chunk", "client/serve/aar_serve_ahi.c",
       ("        slot = ahi_free_slot(s);\n        if (slot < 0)\n            break;", "        slot = 0;"))
mutant("ctrl-d-raw-key-is-e", "raw_key_ctrl_c", "client/serve/aar_serve_input.c",
       ("    case 0x22u: return AAR_BREAK_CTRL_D;", "    case 0x22u: return AAR_BREAK_CTRL_E;"))
mutant("right-button-is-left", "mouse_and_close", "client/serve/aar_serve_input.c",
       ("else if (code == MENUDOWN) { s->in.mouse_buttons |= 2u; id = 2; }", "else if (code == MENUDOWN) { s->in.mouse_buttons |= 1u; id = 2; }"))
mutant("joystick-port-mask-ignored", "joystick", "client/serve/aar_serve_input.c",
       ("        if (!(s->cfg.joy_ports & (1UL << port)))\n            continue;\n", ""))
mutant("close-gadget-ignored", "mouse_and_close", "client/serve/aar_serve_input.c",
       ("            want_stop(s, AAR_SERVE_WHY_WINDOW);", "            (void)0;"))
# ---- serve: display and input, API 0.6 ----------------------------------------------------
SI = "client/serve/aar_serve_input.c"
SG = "client/serve/aar_serve_gfx.c"
mutant("mouse-absolute-summed", "absolute mouse merge", CORE,
       ("                h->mouse.dx = ev->dx;\n                h->mouse.dy = ev->dy;\n",
        "                h->mouse.dx = sat16((int32_t)h->mouse.dx + ev->dx);\n                h->mouse.dy = sat16((int32_t)h->mouse.dy + ev->dy);\n"))
mutant("wheel-up-is-down", "serve_core", SC,
       ("    case 0x7Au: *dy = -1; return 1;", "    case 0x7Au: *dy = 1; return 1;"))
mutant("text-keeps-control-characters", "serve_core", SC,
       ("        if (c < 0x20u || (c >= 0x7Fu && c <= 0x9Fu))\n            continue;\n", ""))
mutant("text-ignores-the-command-qualifier", "serve_core", SC,
       ("    if (qualifiers & command_mask)\n        return 0;\n", ""))
mutant("latin1-passes-wide-characters", "serve_core", SC,
       ("        out[o++] = cp <= 0xFFu ? (char)cp : '?';", "        out[o++] = (char)cp;"))
mutant("resize-without-the-cap", "serve_core", SC,
       ("    if (!(d->caps & AAR_DISPCAP_RESIZE) || !d->ops.resize) {", "    if (!d->ops.resize) {"))
mutant("resize-with-a-flip-pending", "serve_core", SC,
       ("    if (d->flip_active || d->flip_queued) {\n        done->result = AAR_E_BUSY;\n        return;\n    }\n", ""))
mutant("vblank-timeout-ignored", "serve_core", SC,
       ("    if (d->vb_active && (int32_t)(now_ms - d->vb_deadline_ms) >= 0)", "    if (0)"))
mutant("vblank-timeout-rounded-down", "serve_core", SC,
       ("    ms = us / 1000u + ((us % 1000u) ? 1u : 0u);", "    ms = us / 1000u;"))
mutant("vblank-survives-the-close", "serve_core", SC,
       ("    vblank_finish(d, AAR_E_IO);\n    aar_serve_display_reset(d);\n    done->result = AAR_OK;",
        "    aar_serve_display_reset(d);\n    done->result = AAR_OK;"))
mutant("window-cap-without-the-flag", "serve_core", SC,
       ("    if ((open_flags & AAR_DISPLAY_WINDOW) && (features & AAR_DISPCAP_WINDOW)) {",
        "    if (features & AAR_DISPCAP_WINDOW) {"))
mutant("fit-rectangle-not-centred", "serve_core", SC,
       ("    *x = (dw - fw) / 2u;", "    *x = 0;"))
mutant("surface-position-truncates", "serve_core", SC,
       ("    if (a % b < 0)\n        q--;", "    (void)0;"))
mutant("scaling-takes-the-left-edge", "serve_core", SC,
       ("        uint32_t sx = 0, ax = sw;", "        uint32_t sx = 0, ax = 0;"))
mutant("pointer-half-alpha-is-opaque", "serve_core", SC,
       ("    if ((argb >> 24) < 0x80u)\n        return 0;", "    if ((argb >> 24) == 0)\n        return 0;"))
mutant("cd32-forward-is-reverse", "serve_core", SC,
       ("    if (jp & AAR_SERVE_JPF_FORWARD)\n        b |= AAR_JOY_FORWARD;",
        "    if (jp & AAR_SERVE_JPF_FORWARD)\n        b |= AAR_JOY_REVERSE;"))
mutant("cd32-mouse-is-a-pad", "serve_core", SC,
       ("    return type == AAR_SERVE_JP_TYPE_GAMECTLR || type == AAR_SERVE_JP_TYPE_JOYSTK;",
        "    return type != 0;"))
mutant("refresh-rate-unbounded", "serve_core", SC,
       ("    return (mhz < 20000u || mhz > 300000u) ? 0u : mhz;", "    return mhz;"))
mutant("modes-written-is-the-total", "serve_core", SC,
       ("    done->out[AAR_R_DISPLAY_MODES_COUNT] = n;", "    done->out[AAR_R_DISPLAY_MODES_COUNT] = total;"))
mutant("title-nul-not-required", "serve_core", SC,
       ("    if (n >= req->buf_len || n >= 256u) {", "    if (n >= 256u) {"))
mutant("pointer-hot-spot-unchecked", "serve_core", SC,
       ("         hot_x < 0 || hot_y < 0 || hot_x >= (int32_t)w || hot_y >= (int32_t)h ||\n", ""))
mutant("info-flags-from-the-table", "serve_core", SC,
       ("    info.flags &= ~(uint32_t)(AAR_DISPINFO_WINDOW | AAR_DISPINFO_SCALED);\n", ""))
mutant("every-mode-is-preferred", "serve_core", SC,
       ("            modes[i].flags |= AAR_MODEF_PREFERRED;\n            found = 1;",
        "            modes[i].flags |= AAR_MODEF_PREFERRED;"))
mutant("wheel-is-also-a-key", "input_events", SI,
       ("                    push_event(s, AAR_EV_WHEEL, 0, wheel_dx, wheel_dy, qual);\n                continue;",
        "                    push_event(s, AAR_EV_WHEEL, 0, wheel_dx, wheel_dy, qual);"))
mutant("active-event-dropped", "input_events", SI,
       ("            push_event(s, AAR_EV_ACTIVE, 1, 0, 0, 0);", "            (void)0;"))
mutant("close-request-stops-anyway", "close_request", SI,
       ("            if (s->gfx.caps & AAR_DISPCAP_CLOSE_REQUEST)\n", "            if (0)\n"))
mutant("text-for-a-key-release", "text_input", SI,
       ("            if (type == AAR_EV_KEY_DOWN && (s->gfx.caps & AAR_DISPCAP_TEXT_INPUT))",
        "            if (s->gfx.caps & AAR_DISPCAP_TEXT_INPUT)"))
mutant("dead-key-address-not-passed", "text_input", SI,
       ("            dead = *(APTR *)im->IAddress;", "            dead = NULL;"))
mutant("absolute-flag-ignored", "pointer_abs", SI,
       ("    int absolute = (s->gfx.caps & AAR_DISPCAP_POINTER_ABS) != 0;", "    int absolute = 0;"))
mutant("cd32-read-at-every-pass", "joystick_cd32", SI,
       ("    if (s->in.joy_valid[port] && now - s->in.cd32_ms[port] < 16u)\n        return;\n", ""))
mutant("cd32-port-also-read-from-the-registers", "joystick_cd32", SI,
       ("            poll_cd32(s, port);\n            continue;", "            poll_cd32(s, port);"))
mutant("window-position-from-the-corner", "pointer_abs", SG,
       ("        px -= win->BorderLeft;\n", ""))
mutant("scaled-position-not-mapped", "display_scale_window", SG,
       ("                          s->gfx.scaled ? s->gfx.fit_w : 0u,\n", "                          0u,\n"))
mutant("window-on-an-8-bit-public-screen", "display_window_fallback", SG,
       ("X_P96_BITMAP_ATTR(s, pub->RastPort.BitMap, P96BMA_BYTESPERPIXEL) < 2) {",
        "X_P96_BITMAP_ATTR(s, pub->RastPort.BitMap, P96BMA_BYTESPERPIXEL) < 0) {"))
mutant("clut8-in-a-window", "display_window_fallback", SG,
       ("    if (format == AAR_FMT_CLUT8 || !s->gfx.p96", "    if (!s->gfx.p96"))
mutant("surface-token-not-checked", "display_window_fallback", SG,
       ("    if ((ULONG)X_P96_BITMAP_ATTR(s, bm, P96BMA_RGBFORMAT) != rgb ||\n        !bitmap_token(s, bm, token, pitch)) {",
        "    if ((ULONG)X_P96_BITMAP_ATTR(s, bm, P96BMA_RGBFORMAT) != rgb) {\n        (void)bitmap_token(s, bm, token, pitch);"))
mutant("scaled-flip-draws-unscaled", "display_scale_window", SG,
       ("    if (s->gfx.scaled)\n        write_scaled(s, &ri, rp, ox + x, oy + y, w, h);",
        "    if (0)\n        write_scaled(s, &ri, rp, ox + x, oy + y, w, h);"))
mutant("scaled-area-not-cleared", "display_scale_window", SG,
       ("            update_fit(s);\n            clear_inner(s);\n", "            update_fit(s);\n"))
mutant("resize-keeps-the-old-bitmap", "display_resize", SG,
       ("    X_P96_FREE_BITMAP(s, s->gfx.surface);\n    adopt_surface(s, bm, w, h, format);", "    adopt_surface(s, bm, w, h, format);"))
mutant("new-size-not-reported", "display_scale_window", SG,
       ("    if (s->gfx.caps & AAR_DISPCAP_RESIZE)\n        push_event(s, AAR_EV_RESIZE, 0, (int16_t)iw, (int16_t)ih, 0);",
        "    if (0)\n        push_event(s, AAR_EV_RESIZE, 0, (int16_t)iw, (int16_t)ih, 0);"))
mutant("pointer-freed-while-shown", "display_pointer", SG,
       ("    X_SET_WINDOW_POINTER(win, WA_Pointer, (ULONG)obj, TAG_DONE);\n    pointer_release(s);\n",
        "    pointer_release(s);\n    X_SET_WINDOW_POINTER(win, WA_Pointer, (ULONG)obj, TAG_DONE);\n"))
mutant("keymap-opened-without-text-input", "text_input", SG,
       ("        if ((flags & AAR_DISPLAY_TEXT_INPUT) && !s->in.keymap)", "        if (!s->in.keymap)"))
mutant("modes-list-non-picasso96-modes", "display_info", SG,
       ("        if (!X_P96_MODE_ATTR(s, id, P96IDA_ISP96))\n            continue;\n", ""))
mutant("modes-list-unavailable-modes", "display_info", SG,
       ("            disp.NotAvailable ||\n", ""))
mutant("vblank-reply-not-served", "display_vblank", SG,
       ("    s->gfx.vb_pending = 0;\n    aar_serve_display_vblank_seen(&s->core.display);\n}",
        "    s->gfx.vb_pending = 0;\n}"))
mutant("no-timed-wake-for-a-vblank-timeout", "display_vblank_timeout", SL,
       ("    if (vb < us)\n        us = vb;\n", ""))
mutant("cd32-without-lowlevel-is-served", "joystick_cd32", SL,
       ("        if (!s->in.ll) {", "        if (0) {"))
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
    shutil.copytree(os.path.join(ROOT, "loader"), os.path.join(work, "loader"),
                    ignore=shutil.ignore_patterns("__pycache__", "tests"))
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
        sources += ["client/src/aar_probe.c", "loader/aar_image.c", "client/tests/shell_test.c"]
        tests = ["client/tests/shell_test.c"]
    else:
        sources += ["client/tests/core_test.c"]
    # A core mutant is run against both test programs; the named test fails in one of them.
    programs = []
    if is_leak:
        programs.append(("aarleak_test", ["client/tests/aarleak_test.c"], False, []))
    elif is_shell:
        programs.append(("shell_test", sources, True, []))
    else:
        programs.append(("core_test", sources, False, []))
        programs.append(("shell_test", ["client/src/aar_core.c", "client/src/refplat.c",
                                        "client/src/aar_probe.c", "loader/aar_image.c",
                                        "client/tests/shell_test.c"], True, []))
        # The scale test has 240 claims and no staging areas (see run_host.sh).
        programs.append(("scale_test", ["client/src/aar_core.c", "client/src/refplat.c",
                                        "client/tests/scale_test.c"], False, SCALE_FLAGS))
    flags = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Wno-unused-function", "-pthread", "-O1", "-g", "-no-pie", "-fno-pie",
             "-Iclient/tests", "-Iclient/tests/stubs", "-Iclient/include",
             "-I" + os.path.join(ROOT, "include")]
    good = os.path.join(tmp, "good.elf")
    stray = []  # programs that failed in tests other than the one named
    for prog, srcs, wants_image, extra_flags in programs:
        exe = os.path.join(work, prog)
        build = subprocess.run([CC] + flags + extra_flags + srcs + ["-o", exe], cwd=work,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if build.returncode != 0:
            return "BAD", "does not build:\n" + build.stdout[-800:]
        cmd = [exe] + ([good] if wants_image else [])
        try:
            run = subprocess.run(cmd, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=120)
            out, code = run.stdout, run.returncode
        except subprocess.TimeoutExpired as hung:
            # A hang is a failure too: the test that was running is the one that caught it.
            out = hung.stdout.decode(errors="replace") if isinstance(hung.stdout, bytes) else (hung.stdout or "")
            code = 124
        if code == 0:
            continue
        failed = set(re.findall(r"^FAIL \[([^\]]+)\]", out, re.M))
        last = re.findall(r"^== (\S+)", out, re.M)
        if code not in (0, 1) and last:
            failed.add(last[-1])  # a crash: the test that was running
        if test in failed:
            extra = sorted(failed - {test})
            return "KILLED", prog + (" (also: %s)" % ", ".join(extra) if extra else "")
        stray.append("%s failed in %s" % (prog, sorted(failed)))
    if stray:
        return "BAD", "%s, not in %s" % ("; ".join(stray), test)
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
