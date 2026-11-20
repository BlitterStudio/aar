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
       ("    if (h->load_begun || h->started)\n        return AAR_E_BUSY;\n    if (t->set & (1u << 1))",
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
mutant("busy-port-counts-as-posted", "notify_terminal", SHELL,
       ("    if (queued)\n        return AAR_CORE_NOTIFY_BUSY;", "    if (queued)\n        return AAR_CORE_NOTIFIED;"))
# ---- the records of a task that is gone are reclaimed whatever their state ----------------
mutant("obsolete-opener-hidden", "opener_states", SHELL,
       ("        if (prior)\n            prior->next = o->next;\n        else\n            b->openers = o->next;\n        obsolete = o;\n        o = NULL;",
        "        o->closing = 1u;\n        o = NULL;"))
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
# ---- the leak probe waits for the processes that started -------------------------------------
mutant("hold-waits-for-asked-ready", "hold", LEAK,
       ("    while (child_ready < started)\n", "    while (child_ready < n)\n"))
mutant("hold-waits-for-asked-exit", "hold", LEAK,
       ("    while (child_exited < started)\n", "    while (child_exited < n)\n"))
mutant("hold-reports-success", "hold", LEAK,
       ("    return started == n;", "    return 1;"))
mutant("children-report-success", "children", LEAK,
       ("            printf(\"CreateNewProc failed after %lu children\\n\", (unsigned long)i);\n            return 0;",
        "            printf(\"CreateNewProc failed after %lu children\\n\", (unsigned long)i);\n            return 1;"))
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

NAMES = {m[0]: m for m in M}


def build_and_run(work, test_name):
    out = os.path.join(work, "t.bin")
    if test_name in ("shell",):
        srcs = ["client/tests/shell_test.c"]
    else:
        srcs = ["client/tests/core_test.c"]
    return srcs, out


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
    flags = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Wno-unused-function", "-O1", "-g", "-no-pie", "-fno-pie",
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
