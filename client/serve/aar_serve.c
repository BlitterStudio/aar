/*
 * aar_serve: the serving loop of an aar.library client. It answers the requests of
 * an app on a platform behind aar.library (files, console, display, audio) and
 * posts the input events, for the program that opened the AARHandle. The public
 * calls are declared in libraries/aar_serve.h.
 *
 * This file is the one translation unit of the loop: it includes the OS-free
 * handlers' header, the Amiga backends (one file per service) and defines the
 * loop, the instance's create and destroy and the public calls. The OS-free
 * handlers themselves are compiled alone (aar_serve_core.c) and linked with it.
 *
 * The layout is aar_serve_priv.h. Every operating system call is an X_* binding
 * there.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

#include "aar_serve_priv.h"

/* ---- The services every backend uses ----------------------------------------------------- */

/* True while the claim is ours: it was opened and no call has reported it
 * revoked. A flag, no card access: every library call below reports
 * AAR_E_STALE through claim_note(). */
static int claim_live(struct aar_serve *s)
{
    return s->handle != NULL && !s->claim_lost;
}

static LONG claim_note(struct aar_serve *s, LONG rc)
{
    if (rc == AAR_E_STALE)
        s->claim_lost = 1;
    return rc;
}

/* Is the claim still ours? A status read of the smallest size: the state
 * word and the handle's generation. The library renews the lease itself
 * (from a vertical-blank interrupt), so this is not a renewal. */
static int claim_check(struct aar_serve *s)
{
    AARStatusInfo st;

    if (!claim_live(s))
        return 0;
    memset(&st, 0, sizeof(st));
    st.size = offsetof(AARStatusInfo, generation);
    return claim_note(s, X_AAR_STATUS(s->handle, &st)) == AAR_OK;
}

/* The state and the end reason: size 16 reads three control block words. */
static LONG claim_state(struct aar_serve *s, uint32_t *state,
                        uint32_t *end_reason)
{
    AARStatusInfo st;
    LONG rc;

    if (!claim_live(s))
        return AAR_E_STALE;
    memset(&st, 0, sizeof(st));
    st.size = offsetof(AARStatusInfo, exit_code);
    rc = claim_note(s, X_AAR_STATUS(s->handle, &st));
    if (rc == AAR_OK) {
        *state = st.state;
        *end_reason = st.end_reason;
    }
    return rc;
}

static uint32_t now_ms(struct aar_serve *s)
{
    struct timeval tv;
    X_GET_SYS_TIME(s, &tv);
    return (uint32_t)tv.tv_secs * 1000u + (uint32_t)(tv.tv_micro / 1000);
}

static int ctrl_c_set(void)
{
    return (X_SET_SIGNAL(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

static void push_event(struct aar_serve *s, uint16_t type, uint16_t code,
                       int16_t dx, int16_t dy, uint16_t buttons)
{
    if (!claim_live(s))
        return;
    (void)aar_serve_events_push(&s->events, type, code, dx, dy, buttons, now_ms(s));
}

static void stop_owned(struct aar_serve *s)
{
    if (claim_live(s))
        (void)claim_note(s, X_AAR_STOP(s->handle));
}

/* A stop: the stop event first, which aar_serve keeps asking for until the
 * queue takes it, then AARStop (the stop request word, the stop event and
 * the platform's stop). */
static void request_stop(struct aar_serve *s)
{
    if (s->stop_asked)
        return;
    s->stop_asked = 1;
    s->stop_ms = now_ms(s);
    if (!s->stop_event_sent) {
        push_event(s, AAR_EV_STOP, 0, 0, 0, 0);
        s->stop_event_sent = 1;
    }
    stop_owned(s);
}

static void want_stop(struct aar_serve *s, ULONG why)
{
    if (AAR_SERVE_HOOK(s, stop_wanted))
        s->cfg.hooks->stop_wanted(s->cfg.user, why);
    else
        request_stop(s);
}

static void launcher_status(struct aar_serve *s, const char *msg)
{
    if (AAR_SERVE_HOOK(s, status))
        s->cfg.hooks->status(s->cfg.user, msg);
    else
        aar_serve_console_launcher_status(&s->core.console, msg);
}

static void launcher_say(struct aar_serve *s, const char *text)
{
    char line[256];

    snprintf(line, sizeof(line), "%s: %s", s->title, text);
    launcher_status(s, line);
}

/* A requester for a Workbench start, where the console text may never be seen. */
static void serve_requester(struct aar_serve *s, const char *msg)
{
    struct EasyStruct es;

    if (!s->con.spec || !IntuitionBase)
        return;
    memset(&es, 0, sizeof(es));
    es.es_StructSize = sizeof(es);
    es.es_Title = (UBYTE *)s->title;
    es.es_TextFormat = (UBYTE *)msg;
    es.es_GadgetFormat = (UBYTE *)"OK";
    X_EASY_REQUEST(NULL, &es, NULL, NULL);
}

/* The heartbeat: poll_us, or the default while the loop has none (a DOS packet
 * wait still needs a tick to see a lost claim). */
static void timer_arm(struct aar_serve *s)
{
    if (!s->tm.io || s->tm.pending)
        return;
    s->tm.io->tr_node.io_Command = TR_ADDREQUEST;
    s->tm.io->tr_time.tv_secs = 0;
    s->tm.io->tr_time.tv_micro = s->cfg.poll_us ? s->cfg.poll_us :
                                                  AAR_SERVE_DEFAULT_POLL_US;
    X_SEND_IO((struct IORequest *)s->tm.io);
    s->tm.pending = 1;
}

static void timer_reap(struct aar_serve *s)
{
    if (!s->tm.io || !s->tm.pending)
        return;
    if (!X_CHECK_IO((struct IORequest *)s->tm.io)) {
        X_ABORT_IO((struct IORequest *)s->tm.io);
        X_WAIT_IO((struct IORequest *)s->tm.io);
        /* The abort's reply set the port's signal, and nothing is waiting for
         * it: left set, the next Wait would return at once, and a DOS packet
         * wait would abort and re-arm the timer in a loop. */
        X_SET_SIGNAL(0, 1UL << s->tm.port->mp_SigBit);
    } else {
        X_WAIT_IO((struct IORequest *)s->tm.io);
    }
    s->tm.pending = 0;
}

/* ---- The backends -------------------------------------------------------------------------- */

#include "aar_serve_dos.c"
#include "aar_serve_con.c"
#include "aar_serve_ahi.c"
#include "aar_serve_gfx.c"
#include "aar_serve_input.c"

/* ---- Serving the app's requests ----------------------------------------- */

static int post_reply(struct aar_serve *s, const struct aar_serve_req *q,
                      const struct aar_serve_done *d)
{
    AARServiceReq r;
    unsigned i;
    int rc;

    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.op = q->op;
    r.id = q->id;
    r.result = d->result;
    r.reply_flags = d->flags;
    for (i = 0; i < AAR_ENTRY_OUTS; i++)
        r.out[i] = d->out[i];
    rc = (int)claim_note(s, X_AAR_SERVICE_REPLY(s->handle, &r));
    if (q->op == AAR_OP_AUDIO_OPEN && d->result == AAR_OK && rc != AAR_OK &&
        rc != AAR_E_BUSY) {
        /* The library turned the completion into a refusal (the ring does not
         * check out): the app is told the open failed, so AHI must not stay
         * open on a ring nobody attached. */
        aar_serve_audio_refused(&s->core.audio);
    }
    return rc;
}

/* Completions that found the CQ full wait here until the app has drained it. */
static void reply_flush(struct aar_serve *s)
{
    unsigned i = 0;

    while (i < s->reply_n) {
        if (post_reply(s, &s->reply_backlog[i].req, &s->reply_backlog[i].done) ==
            AAR_E_BUSY)
            break;
        i++;
    }
    if (i) {
        memmove(s->reply_backlog, s->reply_backlog + i,
                (s->reply_n - i) * sizeof(s->reply_backlog[0]));
        s->reply_n -= i;
    }
}

static void reply_request(struct aar_serve *s, const struct aar_serve_req *q,
                          const struct aar_serve_done *d)
{
    if (s->reply_n == 0 && post_reply(s, q, d) != AAR_E_BUSY)
        return;
    if (s->reply_n < AAR_SERVE_REPLY_BACKLOG) {
        s->reply_backlog[s->reply_n].req = *q;
        s->reply_backlog[s->reply_n].done = *d;
        s->reply_n++;
        return;
    }
    launcher_say(s, "completion queue full; a completion was lost.");
}

static void sink_complete(void *ctx, const struct aar_serve_req *req,
                          const struct aar_serve_done *done)
{
    struct aar_serve *s = ctx;

    reply_request(s, req, done);
}

/* config.groups: the display and audio groups can be left unserved; the ops of
 * the other groups (files, console) are always answered. */
static int group_served(const struct aar_serve *s, uint16_t op)
{
    ULONG group = 0;

    if (op >= AAR_OP_DISPLAY_OPEN && op <= AAR_OP_DISPLAY_CLOSE)
        group = AAR_GROUP_DISPLAY;
    else if (op == AAR_OP_AUDIO_OPEN || op == AAR_OP_AUDIO_CLOSE)
        group = AAR_GROUP_AUDIO;
    return group == 0 || s->cfg.groups == 0 || (s->cfg.groups & group) != 0;
}

/*
 * The app's messages. The library keeps 64 unread ones, and a message that the
 * full inbox cannot take blocks every request behind it in the SQ, so a client
 * that does not read them hangs the app. Unless the program reads them itself
 * (AAR_SERVEF_KEEP_MESSAGES), they are taken here: given to the hook, or
 * dropped and counted.
 */
static void service_messages(struct aar_serve *s)
{
    AARMessage m;
    unsigned n;

    if (s->cfg.flags & AAR_SERVEF_KEEP_MESSAGES)
        return;
    for (n = 0; n < 64u; n++) { /* the library's inbox */
        memset(&m, 0, sizeof(m));
        if (claim_note(s, X_AAR_GET_MESSAGE(s->handle, &m)) != AAR_OK)
            break;
        if (AAR_SERVE_HOOK(s, message))
            s->cfg.hooks->message(s->cfg.user, &m);
        else
            s->dropped_messages++;
    }
}

static void service_requests(struct aar_serve *s)
{
    unsigned served = 0;
    AARServiceReq r;
    LONG rc = AAR_OK;

    reply_flush(s);
    for (;;) {
        struct aar_serve_req q;
        struct aar_serve_done d;
        unsigned i;

        if (served >= 16u || !aar_serve_reply_may_take_request(s->reply_n))
            break;
        memset(&r, 0, sizeof(r));
        r.size = sizeof(r);
        rc = claim_note(s, X_AAR_SERVICE_NEXT(s->handle, &r));
        if (rc != AAR_OK)
            break;
        served++;
        memset(&q, 0, sizeof(q));
        q.op = (uint16_t)r.op;
        q.flags = (uint16_t)r.flags;
        q.id = r.id;
        for (i = 0; i < AAR_ENTRY_ARGS; i++)
            q.arg[i] = r.arg[i];
        q.buf_token = r.buf;
        q.buf_off = r.buf_off;
        q.buf_len = r.buf_len;
        q.buf = (uint8_t *)r.ptr;
        memset(&d, 0, sizeof(d));
        if (!group_served(s, q.op))
            d.result = AAR_E_NOSYS;
        else if (aar_serve_core_request(&s->core, &q, &d) != AAR_SERVE_REQ_DONE)
            continue;
        reply_request(s, &q, &d);
    }
    if (rc == AAR_E_IO && !s->queue_corrupt) {
        s->queue_corrupt = 1;
        launcher_say(s, "the request queue is corrupt; stopping the app.");
        want_stop(s, AAR_SERVE_WHY_QUEUE);
    }
}

static int events_op_post(void *ctx, const aar_event *ev)
{
    struct aar_serve *s = ctx;

    return (int)claim_note(s, X_AAR_POST_EVENT(s->handle, ev));
}

/* The event table of the loop; the instance copies it and sets ctx. */
static const struct aar_serve_events_ops aar_serve_events_backend = {
    NULL, events_op_post
};

/* ---- The log sink ------------------------------------------------------------------------ */

/* One app log record (AAR_OP_LOG): the hook, or a diagnostic line in the console
 * queue. */
static void serve_log_record(struct aar_serve *s, const void *data, ULONG len)
{
    if (AAR_SERVE_HOOK(s, log))
        s->cfg.hooks->log(s->cfg.user, data, len);
    else
        aar_serve_console_launcher_log(&s->core.console,
                                       (const uint8_t *)data, len);
}

static void log_sink(APTR user, const void *data, ULONG len)
{
    serve_log_record((struct aar_serve *)user, data, len);
}

/*
 * The one place where the app's log records reach the loop: the sink is
 * installed at create and removed at destroy (the handle outlives this
 * instance, and the sink's user pointer is the instance).
 */
static void log_sink_install(struct aar_serve *s)
{
    (void)X_AAR_SET_LOG_SINK(s->handle, log_sink, (APTR)s);
}

static void log_sink_remove(struct aar_serve *s)
{
    if (claim_live(s))
        (void)claim_note(s, X_AAR_SET_LOG_SINK(s->handle, NULL, NULL));
}

/* ---- Create, destroy ----------------------------------------------------------------------- */

void aar_serve_config_default(aar_serve_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->size = sizeof(*cfg);
    cfg->audio_route = AAR_AUDIO_ROUTE_PLATFORM;
    cfg->joy_ports = AAR_SERVE_DEFAULT_JOY_PORTS;
    cfg->poll_us = AAR_SERVE_DEFAULT_POLL_US;
    cfg->break_mask = AAR_SERVE_BREAK_ALL;
    cfg->display_strategy = AAR_SERVE_DISPLAY_BUFFERS;
}

static int config_valid(const aar_serve_config *cfg)
{
    const ULONG known = AAR_SERVEF_ABS_PATHS | AAR_SERVEF_KEEP_MESSAGES |
                        AAR_SERVEF_NO_INPUT_WINDOW | AAR_SERVEF_USE_SIGNAL;

    return cfg->size >= sizeof(aar_serve_config) &&
           (cfg->flags & ~known) == 0 &&
           (cfg->audio_route == AAR_AUDIO_ROUTE_PLATFORM ||
            cfg->audio_route == AAR_AUDIO_ROUTE_AHI) &&
           (cfg->groups & ~AAR_GROUPS_DEFINED) == 0 &&
           (cfg->joy_ports & ~3UL) == 0 &&
           (cfg->break_mask & ~AAR_SERVE_BREAK_ALL) == 0 &&
           cfg->display_strategy <= AAR_SERVE_DISPLAY_SINGLE &&
           cfg->platform == 0;
}

/* Everything create opened, in the reverse order. Used by destroy and by a
 * create that fails half way. */
static void serve_free(struct aar_serve *s)
{
    if (s->root)
        X_UNLOCK(s->root);
    if (s->tm.io) {
        if (s->tm.io->tr_node.io_Device)
            X_CLOSE_DEVICE((struct IORequest *)s->tm.io);
        X_DELETE_IOREQ((struct IORequest *)s->tm.io);
    }
    if (s->tm.port)
        X_DELETE_PORT(s->tm.port);
    if (s->gfx.p96)
        X_CLOSE_LIB(s->gfx.p96);
    X_FREE_VEC(s);
}

LONG aar_serve_create(AARHandle *h, const aar_serve_config *cfg,
                      aar_serve **out)
{
    struct aar_serve_sink sink;
    AARStatusInfo st;
    struct aar_serve *s;
    LONG rc;

    if (!out)
        return AAR_E_INVAL;
    *out = NULL;
    if (!h || !cfg || !config_valid(cfg))
        return AAR_E_INVAL;
    /* The AHI feeder moves at most a fiftieth of a second per pass: it needs a
     * heartbeat at least that fast. */
    if (cfg->audio_route == AAR_AUDIO_ROUTE_AHI &&
        (cfg->poll_us == 0 || cfg->poll_us > 20000UL))
        return AAR_E_NOSYS;
    s = X_ALLOC_VEC(sizeof(*s), MEMF_PUBLIC | MEMF_CLEAR);
    if (!s)
        return AAR_E_NOMEM;
    s->handle = h;
    s->cfg = *cfg;
    s->cfg.size = sizeof(s->cfg);
    s->title = cfg->screen_title ? (const char *)cfg->screen_title : "aar-serve";
    /* The handle's owner and generation are checked by the first call. */
    memset(&st, 0, sizeof(st));
    st.size = offsetof(AARStatusInfo, generation);
    rc = claim_note(s, X_AAR_STATUS(h, &st));
    if (rc != AAR_OK) {
        serve_free(s);
        return rc;
    }
    {
        /* How long an app gets to honour a stop: the platform says. */
        AARInfo info;

        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (X_AAR_QUERY(&info) == AAR_OK)
            s->grace_ms = info.stop_grace_ms;
    }
    if (cfg->root) {
        s->root = X_DUP_LOCK(cfg->root);
    } else {
        BPTR cur = X_CURRENT_DIR(0);

        X_CURRENT_DIR(cur);
        s->root = X_DUP_LOCK(cur);
    }
    if (!s->root) {
        serve_free(s);
        return AAR_E_NOMEM;
    }
    s->gfx.p96 = X_OPEN_LIB((CONST_STRPTR)"Picasso96API.library", 2);
    s->tm.port = X_CREATE_PORT();
    if (s->tm.port)
        s->tm.io = (struct timerequest *)X_CREATE_IOREQ(
            s->tm.port, sizeof(struct timerequest));
    if (!s->tm.io || X_OPEN_DEVICE((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                                   (struct IORequest *)s->tm.io, 0)) {
        if (s->tm.io)
            s->tm.io->tr_node.io_Device = NULL;
        serve_free(s);
        return AAR_E_NOSYS;
    }
    s->tm.base = s->tm.io->tr_node.io_Device;
    s->ahi.device_open = -1;
    s->con.in = cfg->console_in;
    s->con.handle = cfg->console_out;
    s->con.spec = cfg->console_spec;

    /* Wires the handlers to the DOS, console, display and audio backends. */
    sink.ctx = s;
    sink.complete = sink_complete;
    aar_serve_core_init(&s->core, &sink);
    s->dos.ops = aar_serve_dos_backend;
    s->dos.ops.ctx = s;
    aar_serve_files_init(&s->core.files, &s->dos.ops, (uintptr_t)s->root,
                         (cfg->flags & AAR_SERVEF_ABS_PATHS) != 0);
    {
        struct aar_serve_console_ops con_ops = aar_serve_con_backend;
        struct aar_serve_display_ops gfx_ops = aar_serve_gfx_backend;
        struct aar_serve_audio_ops ahi_ops = aar_serve_ahi_backend;
        struct aar_serve_events_ops ev_ops = aar_serve_events_backend;

        con_ops.ctx = s;
        gfx_ops.ctx = s;
        ahi_ops.ctx = s;
        ev_ops.ctx = s;
        aar_serve_console_init(&s->core.console,
                               AAR_SERVE_HOOK(s, console) ?
                                   s->cfg.hooks->console : &con_ops,
                               &s->core.sink);
        aar_serve_display_init(&s->core.display,
                               AAR_SERVE_HOOK(s, display) ?
                                   s->cfg.hooks->display : &gfx_ops,
                               &s->core.sink);
        aar_serve_audio_init(&s->core.audio, &ahi_ops);
        aar_serve_events_init(&s->events, &ev_ops);
    }
    log_sink_install(s);
    if (cfg->poll_us)
        timer_arm(s);
    *out = s;
    return AAR_OK;
}

void aar_serve_destroy(aar_serve *s)
{
    if (!s)
        return;
    timer_reap(s);
    s->cfg.poll_us = 0; /* the DOS waits below must not arm the timer again */
    if (!s->stop_asked && claim_live(s)) {
        uint32_t leaving, leaving_end;

        if (claim_state(s, &leaving, &leaving_end) == AAR_OK &&
            leaving != AAR_STATE_IDLE)
            stop_owned(s);
    }
    log_sink_remove(s);
    ahi_close(s);
    /* A pending console read cannot be cancelled. Close the app screen before
     * that wait so the prompt is not hidden behind it. */
    close_display(s);
    console_read_finish(s);
    /* What a handler still holds is answered, once, with AAR_E_IO. */
    aar_serve_core_abort(&s->core);
    reply_flush(s);
    if (s->con.owned && s->con.handle)
        X_CLOSE(s->con.handle);
    if (s->core.files.dos)
        aar_serve_files_close_all(&s->core.files);
    serve_free(s);
}

/* ---- The loop -------------------------------------------------------------------------------- */

ULONG aar_serve_sigmask(const aar_serve *cs)
{
    struct aar_serve *s = (struct aar_serve *)cs;
    ULONG mask = s->cfg.break_mask;

    if (s->tm.port && s->cfg.poll_us)
        mask |= 1UL << s->tm.port->mp_SigBit;
    if (s->con.port)
        mask |= 1UL << s->con.port->mp_SigBit;
    if (s->gfx.input_win && s->gfx.input_win->UserPort)
        mask |= 1UL << s->gfx.input_win->UserPort->mp_SigBit;
    /* The SafeMessage of a flip and the completion of an AHI buffer: with no
     * heartbeat they are what wakes the program to finish the flip and refill
     * AHI. */
    if (s->gfx.safe_port)
        mask |= 1UL << s->gfx.safe_port->mp_SigBit;
    if (s->ahi.port)
        mask |= 1UL << s->ahi.port->mp_SigBit;
    if (s->cfg.flags & AAR_SERVEF_USE_SIGNAL)
        mask |= X_AAR_MESSAGE_SIGNAL(s->handle);
    return mask;
}

LONG aar_serve_step(aar_serve *s, ULONG signals)
{
    ULONG breaks = s->cfg.break_mask;
    struct aar_serve_exit_in leave;
    uint32_t state, end_reason;

    if (s->tm.port && (signals & (1UL << s->tm.port->mp_SigBit)))
        timer_reap(s);
    /* The library renews the lease from its own interrupt. A claim whose
     * handle went stale is not served any more. */
    if (!claim_live(s)) {
        s->ownership_lost = 1;
        timer_reap(s);
        return AAR_SERVE_LOST;
    }
    if ((breaks & SIGBREAKF_CTRL_C) &&
        ((signals & SIGBREAKF_CTRL_C) || ctrl_c_set())) {
        X_SET_SIGNAL(0, SIGBREAKF_CTRL_C);
        s->user_stop = 1;
        want_stop(s, AAR_SERVE_WHY_BREAK);
    }
    /* A Shell delivers Ctrl-D/E/F as task signals, not as keys in the app's
     * display window, so map them here as well. */
    if ((breaks & signals & SIGBREAKF_CTRL_D))
        push_event(s, AAR_EV_BREAK, AAR_BREAK_CTRL_D, 0, 0, 0);
    if ((breaks & signals & SIGBREAKF_CTRL_E))
        push_event(s, AAR_EV_BREAK, AAR_BREAK_CTRL_E, 0, 0, 0);
    if ((breaks & signals & SIGBREAKF_CTRL_F))
        push_event(s, AAR_EV_BREAK, AAR_BREAK_CTRL_F, 0, 0, 0);
    drain_window(s, s->gfx.input_win);
    poll_joysticks(s);
    aar_serve_events_flush(&s->events);
    /* The library flushes a pending coalesced mouse event itself. */
    service_requests(s);
    service_messages(s);
    service_console(s);
    service_display(s);
    service_ahi_audio(s);
    reply_flush(s);
    /* The claim may have been revoked while a request was served. One small
     * status read gives the state and the end reason; any failure of it means
     * the claim is gone. */
    if (claim_state(s, &state, &end_reason) != AAR_OK) {
        s->ownership_lost = 1;
        timer_reap(s);
        return AAR_SERVE_LOST;
    }
    s->app_state = state;
    if (state == AAR_STATE_FAULTED)
        s->saw_fault = 1;
    memset(&leave, 0, sizeof(leave));
    leave.state = state;
    leave.end_reason = end_reason;
    leave.elapsed_ms = s->stop_asked ? now_ms(s) - s->stop_ms : 0u;
    leave.entered = 1;
    leave.stop_asked = s->stop_asked;
    leave.user_stop = s->user_stop;
    leave.output_pending = aar_serve_console_output_pending(&s->core.console);
    leave.output_failed = s->console_output_failed;
    leave.grace_ms = s->grace_ms;
    if (aar_serve_run_should_exit(&leave)) {
        timer_reap(s);
        return AAR_SERVE_ENDED;
    }
    if (s->cfg.poll_us)
        timer_arm(s);
    return AAR_SERVE_CONTINUE;
}

LONG aar_serve_run(aar_serve *s)
{
    LONG rc;

    do {
        rc = aar_serve_step(s, X_WAIT(aar_serve_sigmask(s)));
    } while (rc == AAR_SERVE_CONTINUE);
    return rc;
}

/* ---- The rest of the calls ------------------------------------------------------------------- */

LONG aar_serve_post_event(aar_serve *s, const aar_event *ev)
{
    return aar_serve_events_push(&s->events, ev->type, ev->code, ev->dx,
                                 ev->dy, ev->buttons, ev->time_ms);
}

void aar_serve_flip_shown(aar_serve *s)
{
    aar_serve_display_flip_safe(&s->core.display);
}

void aar_serve_stop(aar_serve *s)
{
    request_stop(s);
}

void aar_serve_status(aar_serve *s, const char *text)
{
    launcher_status(s, text);
}

LONG aar_serve_locate(aar_serve *s, BPTR dir, const char *name, char *full,
                      ULONG size)
{
    int transport_ok = 0;
    BPTR lock = lock_find_async(s, dir, name, &transport_ok);

    if (!transport_ok || !lock)
        return AAR_E_NOENT;
    if (!X_NAME_FROM_LOCK(lock, (STRPTR)full, (LONG)size)) {
        X_UNLOCK(lock);
        return AAR_E_IO;
    }
    X_UNLOCK(lock);
    return AAR_OK;
}

LONG aar_serve_result(aar_serve *s, struct aar_serve_result *out)
{
    AARStatusInfo st;
    LONG rc = AAR_E_STALE;

    out->flags = (s->saw_fault ? AAR_SERVE_R_FAULTED : 0UL) |
                 (s->user_stop ? AAR_SERVE_R_USER_STOP : 0UL) |
                 (s->stop_asked ? AAR_SERVE_R_STOP_ASKED : 0UL) |
                 (s->ownership_lost ? AAR_SERVE_R_CLAIM_LOST : 0UL) |
                 (s->display_failed ? AAR_SERVE_R_DISPLAY_FAILED : 0UL) |
                 (s->console_output_failed ? AAR_SERVE_R_CONSOLE_FAILED : 0UL) |
                 (s->audio_open_failed ? AAR_SERVE_R_AUDIO_FAILED : 0UL) |
                 (s->queue_corrupt ? AAR_SERVE_R_QUEUE_CORRUPT : 0UL);
    out->dropped_logs = s->core.console.launcher_dropped;
    out->dropped_messages = s->dropped_messages;
    if (!claim_live(s))
        return rc;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    rc = claim_note(s, X_AAR_STATUS(s->handle, &st));
    if (rc != AAR_OK)
        return rc;
    out->status = st;
    out->state = st.state;
    out->end_reason = st.end_reason;
    out->exit_code = st.exit_code;
    if (st.fault_class)
        out->flags |= AAR_SERVE_R_FAULTED;
    return AAR_OK;
}
