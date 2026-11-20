/*
 * aar_serve: the console backend (asynchronous reads and writes, the Workbench
 * console, the notes to the user's console).
 *
 * This file is part of aar_serve.c, which includes it after the services of the
 * loop and before the public calls; it is not compiled alone. State lives in the
 * instance (struct aar_serve in aar_serve_priv.h).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

static BPTR console_output(struct aar_serve *s)
{
    return s->con.handle ? s->con.handle : X_OUTPUT();
}

static BPTR console_input(struct aar_serve *s)
{
    if (s->con.in)
        return s->con.in;
    return s->con.handle ? s->con.handle : X_INPUT();
}

static int console_open_if_needed(struct aar_serve *s)
{
    if (!s->con.spec || s->con.handle)
        return 1;
    s->con.handle = X_OPEN(s->con.spec, MODE_OLDFILE);
    if (!s->con.handle) {
        if (!s->con.open_failed) {
            char note[160];

            snprintf(note, sizeof(note),
                     "%s: cannot open the requested Workbench console.",
                     s->title);
            launcher_status(s, note);
            serve_requester(s, note);
            s->con.open_failed = 1;
        }
        return 0;
    }
    s->con.owned = 1;
    return 1;
}

static void console_reap_packets(struct aar_serve *s);
static int console_write_start(struct aar_serve *s, BPTR fh,
                               const uint8_t *data, uint32_t bytes);
static void console_write_fh(BPTR fh, const char *msg);

static void console_output_error(struct aar_serve *s)
{
    char note[160];

    want_stop(s, AAR_SERVE_WHY_CONSOLE);
    if (!s->console_output_failed) {
        snprintf(note, sizeof(note), "%s: cannot write app console output.\n",
                 s->title);
        console_write_fh(console_output(s), note);
    }
    s->console_output_failed = 1;
}

/*
 * Input stays asynchronous for every handle. A keyboard read uses the
 * console's normal line mode, so the Shell keeps echo, line editing and
 * Ctrl-C/D/E/F. A pipe or file uses the same ACTION_READ: a blocking Read
 * would stop the main loop, display and audio while the writer stays open.
 * A pending read returns a line on Return and 0 bytes on Ctrl-\. It starts
 * only for an outstanding CONSOLE_READ request, while the app is RUNNING;
 * the request is the app's demand for input. Synchronous DOS calls keep
 * using pr_MsgPort, so their replies never meet this packet.
 */

static int console_read_start(struct aar_serve *s, BPTR fh, uint32_t max)
{
    struct FileHandle *handle = (struct FileHandle *)BADDR(fh);

    if (!handle || !handle->fh_Type || max == 0 ||
        max > AAR_SERVE_CONSOLE_READ_BYTES)
        return 0;
    if (!s->con.port)
        s->con.port = X_CREATE_PORT();
    if (!s->con.packet)
        s->con.packet = X_ALLOC_MEM(sizeof(*s->con.packet), MEMF_PUBLIC | MEMF_CLEAR);
    if (!s->con.buffer)
        s->con.buffer = X_ALLOC_MEM(AAR_SERVE_CONSOLE_READ_BYTES, MEMF_PUBLIC);
    if (!s->con.port || !s->con.packet || !s->con.buffer)
        return 0;
    memset(s->con.packet, 0, sizeof(*s->con.packet));
    s->con.packet->sp_Msg.mn_Node.ln_Name = (char *)&s->con.packet->sp_Pkt;
    s->con.packet->sp_Pkt.dp_Link = &s->con.packet->sp_Msg;
    s->con.packet->sp_Pkt.dp_Port = s->con.port;
    s->con.packet->sp_Pkt.dp_Type = ACTION_READ;
    s->con.packet->sp_Pkt.dp_Arg1 = handle->fh_Arg1;
    s->con.packet->sp_Pkt.dp_Arg2 = (LONG)s->con.buffer;
    s->con.packet->sp_Pkt.dp_Arg3 = (LONG)max;
    s->con.fh = fh;
    s->con.interactive = X_IS_INTERACTIVE(fh);
    s->con.pending = 1;
    X_PUT_MSG(handle->fh_Type, &s->con.packet->sp_Msg);
    return 1;
}

static void console_write_fh(BPTR fh, const char *msg)
{
    if (!fh || !msg || !msg[0])
        return;
    (void)X_WRITE(fh, (APTR)msg, (LONG)strlen(msg));
}

/* The user's console, never redirected stdout and never the pipe. */
static void console_user_note(struct aar_serve *s, const char *msg)
{
    BPTR fh;

    if (s->con.handle) {
        console_write_fh(s->con.handle, msg);
        return;
    }
    fh = X_OPEN((CONST_STRPTR)"CONSOLE:", MODE_OLDFILE);
    if (fh) {
        console_write_fh(fh, msg);
        X_CLOSE(fh);
        return;
    }
    serve_requester(s, msg);
}

static int console_write_start(struct aar_serve *s, BPTR fh,
                               const uint8_t *data, uint32_t bytes)
{
    struct FileHandle *handle = (struct FileHandle *)BADDR(fh);

    if (!handle || !handle->fh_Type || !data || bytes == 0 ||
        bytes > AAR_SERVE_CONSOLE_WRITE_BYTES)
        return 0;
    if (!s->con.port)
        s->con.port = X_CREATE_PORT();
    if (!s->con.wpacket)
        s->con.wpacket = X_ALLOC_MEM(sizeof(*s->con.wpacket),
                                    MEMF_PUBLIC | MEMF_CLEAR);
    if (!s->con.wbuffer)
        s->con.wbuffer = X_ALLOC_MEM(AAR_SERVE_CONSOLE_WRITE_BYTES, MEMF_PUBLIC);
    if (!s->con.port || !s->con.wpacket || !s->con.wbuffer)
        return 0;
    memcpy(s->con.wbuffer, data, bytes);
    memset(s->con.wpacket, 0, sizeof(*s->con.wpacket));
    s->con.wpacket->sp_Msg.mn_Node.ln_Name =
        (char *)&s->con.wpacket->sp_Pkt;
    s->con.wpacket->sp_Pkt.dp_Link = &s->con.wpacket->sp_Msg;
    s->con.wpacket->sp_Pkt.dp_Port = s->con.port;
    s->con.wpacket->sp_Pkt.dp_Type = ACTION_WRITE;
    s->con.wpacket->sp_Pkt.dp_Arg1 = handle->fh_Arg1;
    s->con.wpacket->sp_Pkt.dp_Arg2 = (LONG)s->con.wbuffer;
    s->con.wpacket->sp_Pkt.dp_Arg3 = (LONG)bytes;
    s->con.winteractive = X_IS_INTERACTIVE(fh);
    s->con.wpending = 1;
    X_PUT_MSG(handle->fh_Type, &s->con.wpacket->sp_Msg);
    return 1;
}

static void console_write_complete(struct aar_serve *s)
{
    s->con.wpending = 0;
    aar_serve_console_write_done(&s->core.console,
                                 (int32_t)s->con.wpacket->sp_Pkt.dp_Res1);
}

static void console_read_complete(struct aar_serve *s)
{
    LONG got = s->con.packet->sp_Pkt.dp_Res1;

    s->con.pending = 0;
    aar_serve_console_read_done(&s->core.console, (int32_t)got, s->con.buffer);
}

static void console_reap_packets(struct aar_serve *s)
{
    struct Message *msg;

    if (!s->con.port)
        return;
    while ((msg = X_GET_MSG(s->con.port)) != NULL) {
        if (s->con.pending && s->con.packet && msg == &s->con.packet->sp_Msg)
            console_read_complete(s);
        else if (s->con.wpending && s->con.wpacket &&
                 msg == &s->con.wpacket->sp_Msg)
            console_write_complete(s);
    }
}

static void console_read_finish(struct aar_serve *s)
{
    char note[160];
    int input_noted = 0;
    int output_noted = 0;

    while (s->con.pending || s->con.wpending ||
           (!s->core.console.failed && s->core.console.launcher_used)) {
        console_reap_packets(s);
        aar_serve_console_pump(&s->core.console);
        if (!s->con.pending && !s->con.wpending &&
            (s->core.console.failed || !s->core.console.launcher_used))
            break;
        if (s->con.pending && !input_noted) {
            int kind = aar_serve_finish_prompt(1, s->con.interactive);

            if (kind == AAR_SERVE_FINISH_RETURN_ON_READ_HANDLE) {
                snprintf(note, sizeof(note), "%s: press Return to finish.\n",
                         s->title);
                console_write_fh(s->con.fh ? s->con.fh : console_input(s),
                                 note);
            } else {
                snprintf(note, sizeof(note),
                         "%s: waiting for pending input to finish.\n",
                         s->title);
                console_user_note(s, note);
            }
            input_noted = 1;
        }
        if (s->con.wpending && !s->con.winteractive && !output_noted) {
            snprintf(note, sizeof(note),
                     "%s: waiting for pending output to finish.\n", s->title);
            console_user_note(s, note);
            output_noted = 1;
        }
        if (s->con.port)
            X_WAIT_PORT(s->con.port);
    }
    if (s->con.wbuffer)
        X_FREE_MEM(s->con.wbuffer, AAR_SERVE_CONSOLE_WRITE_BYTES);
    if (s->con.wpacket)
        X_FREE_MEM(s->con.wpacket, sizeof(*s->con.wpacket));
    if (s->con.buffer)
        X_FREE_MEM(s->con.buffer, AAR_SERVE_CONSOLE_READ_BYTES);
    if (s->con.packet)
        X_FREE_MEM(s->con.packet, sizeof(*s->con.packet));
    if (s->con.port)
        X_DELETE_PORT(s->con.port);
    s->con.wbuffer = NULL;
    s->con.wpacket = NULL;
    s->con.wpending = 0;
    s->con.winteractive = 0;
    s->con.buffer = NULL;
    s->con.packet = NULL;
    s->con.port = NULL;
    s->con.fh = 0;
    s->con.interactive = 0;
}

static void service_console(struct aar_serve *s)
{
    console_reap_packets(s);
    aar_serve_console_pump(&s->core.console);
}

static int con_op_open(void *ctx)
{
    struct aar_serve *s = ctx;

    return console_open_if_needed(s);
}

static int con_op_write_start(void *ctx, const uint8_t *data, uint32_t bytes)
{
    struct aar_serve *s = ctx;

    return console_write_start(s, console_output(s), data, bytes);
}

static int con_op_read_start(void *ctx, uint32_t max)
{
    struct aar_serve *s = ctx;

    return console_read_start(s, console_input(s), max);
}

static int con_op_read_allowed(void *ctx)
{
    struct aar_serve *s = ctx;

    return aar_serve_console_read_allowed(s->app_state, s->stop_asked);
}

static void con_op_output_error(void *ctx)
{
    struct aar_serve *s = ctx;

    console_output_error(s);
}

static int con_op_live(void *ctx)
{
    struct aar_serve *s = ctx;

    return claim_live(s);
}

/* The console table of the built-in backend; the instance copies it and sets ctx. */
static const struct aar_serve_console_ops aar_serve_con_backend = {
    NULL, con_op_open, con_op_write_start, con_op_read_start,
    con_op_read_allowed, con_op_live, con_op_output_error
};
