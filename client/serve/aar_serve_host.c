/*
 * aar_serve: the AmigaOS backend of the locale, time, environment and clipboard
 * handlers (aar_serve_core.c decides what each request means). locale.library
 * and iffparse.library are opened at their first use and closed at destroy; a
 * library that is not there makes the locale answer with zeros and the
 * clipboard with AAR_E_NOSYS. The clipboard is clipboard.device, opened
 * through iffparse.library's OpenClipboard, and read and written as the IFF
 * stream it holds.
 *
 * This file is part of the translation unit of aar_serve.c.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

#define HOST_ENV_START_BYTES 1024u
#define HOST_ENV_MAX_BYTES (1024u * 1024u)
#define HOST_CLIP_CHUNK 4096u
#define HOST_ENV_TRIES 4u

static int host_live(void *ctx)
{
    return claim_live(ctx);
}

static void *host_mem_alloc(void *ctx, uint32_t bytes)
{
    (void)ctx;
    return X_ALLOC_VEC(bytes, MEMF_PUBLIC);
}

static void host_mem_free(void *ctx, void *mem)
{
    (void)ctx;
    X_FREE_VEC(mem);
}

static void host_copy_text(char *dst, size_t cap, const char *src)
{
    size_t n = 0;

    if (src)
        while (n + 1 < cap && src[n]) {
            dst[n] = src[n];
            n++;
        }
    dst[n] = 0;
}

/* The user's locale: what the Locale of OpenLocale(NULL) holds. */
static int host_locale(void *ctx, struct aar_serve_locale *out)
{
    struct aar_serve *s = ctx;
    struct Locale *locale;

    memset(out, 0, sizeof(*out));
    if (!s->host.locale_tried) {
        s->host.locale_tried = 1;
        s->host.locale = X_OPEN_LIB((CONST_STRPTR)"locale.library", 38);
    }
    if (!s->host.locale)
        return 0;
    locale = X_OPEN_LOCALE(s, NULL);
    if (!locale)
        return 0;
    out->gmt_west_min = (int32_t)locale->loc_GMTOffset;
    host_copy_text(out->language, sizeof(out->language), (const char *)locale->loc_LanguageName);
    out->country = (uint32_t)locale->loc_CountryCode;
    host_copy_text(out->decimal_point, sizeof(out->decimal_point),
                   (const char *)locale->loc_DecimalPoint);
    host_copy_text(out->group_separator, sizeof(out->group_separator),
                   (const char *)locale->loc_GroupSeparator);
    X_CLOSE_LOCALE(s, locale);
    return 1;
}

static void host_datestamp(void *ctx, uint32_t *days, uint32_t *minutes, uint32_t *ticks)
{
    struct DateStamp stamp;

    (void)ctx;
    X_DATE_STAMP(&stamp);
    *days = (uint32_t)stamp.ds_Days;
    *minutes = (uint32_t)stamp.ds_Minute;
    *ticks = (uint32_t)stamp.ds_Tick;
}

/* GetVar() of a global, binary variable, by its documented result (dos.library
 * V37 or later): the value is copied cut to the buffer less one byte for the
 * NUL; the return is the number of bytes copied, NUL not counted, and IoErr()
 * the full size of the variable whatever the buffer. A value that was cut is
 * seen by IoErr() above the copied length, not by an error code. Only what the
 * request can take is fetched: a value longer than room is counted from the
 * first call, with no buffer that grows to it; one that the request can hold
 * but the buffer could not is fetched again with a buffer of its size. A value
 * that changes in between is fetched again a few times, then it is AAR_E_IO. */
static int32_t host_env_get(void *ctx, const char *name, uint8_t *dst, uint32_t room,
                            uint32_t *full)
{
    uint32_t size = HOST_ENV_START_BYTES;
    unsigned tries;

    (void)ctx;
    for (tries = 0; tries < HOST_ENV_TRIES; tries++) {
        uint8_t *value = X_ALLOC_VEC(size, MEMF_PUBLIC);
        uint32_t whole;
        uint32_t want;
        LONG len;
        LONG err;

        if (!value)
            return AAR_E_NOMEM;
        X_SET_IO_ERR(0);
        len = X_GET_VAR((STRPTR)name, (STRPTR)value, (LONG)size,
                        GVF_GLOBAL_ONLY | GVF_BINARY_VAR);
        err = X_IO_ERR();
        if (len < 0) {
            X_FREE_VEC(value);
            return err == ERROR_OBJECT_NOT_FOUND ? AAR_E_NOENT : AAR_E_IO;
        }
        /* A full size below the bytes copied is a dos.library that does not
         * report it as documented. */
        if (err < len) {
            X_FREE_VEC(value);
            return AAR_E_IO;
        }
        whole = (uint32_t)err;
        want = whole < room ? whole : room;
        if ((uint32_t)len >= want) {
            memcpy(dst, value, want);
            *full = whole;
            X_FREE_VEC(value);
            return AAR_OK;
        }
        X_FREE_VEC(value);
        if (want >= HOST_ENV_MAX_BYTES)
            return AAR_E_NOMEM;
        size = want + 1u;
    }
    return AAR_E_IO;
}

/* The clipboard unit, opened through iffparse.library. NULL with *err. */
static struct ClipboardHandle *host_clip_open(struct aar_serve *s, uint32_t unit,
                                              int32_t *err)
{
    struct ClipboardHandle *cb = NULL;

    if (!s->host.iffparse_tried) {
        s->host.iffparse_tried = 1;
        s->host.iffparse = X_OPEN_LIB((CONST_STRPTR)"iffparse.library", 37);
    }
    if (s->host.iffparse)
        cb = X_OPEN_CLIPBOARD(s, (LONG)unit);
    if (!cb)
        *err = AAR_E_NOSYS;
    return cb;
}

/* The signals the wait of a clipboard command takes: the reply port of the
 * request, the heartbeat timer and Ctrl-C (when the program leaves it to the
 * loop). */
static ULONG host_clip_mask(struct aar_serve *s, const struct MsgPort *port)
{
    ULONG mask = (1UL << port->mp_SigBit) | (s->cfg.break_mask & SIGBREAKF_CTRL_C);

    if (s->tm.port)
        mask |= 1UL << s->tm.port->mp_SigBit;
    return mask;
}

/* A Ctrl-C the wait saw: consumed, recorded as a user stop, and passed on as a
 * stop request, as the DOS packet wait does. */
static void host_clip_break(struct aar_serve *s)
{
    X_SET_SIGNAL(0, SIGBREAKF_CTRL_C);
    s->user_stop = 1;
    if (aar_serve_ctrl_c_must_request_stop(1))
        want_stop(s, AAR_SERVE_WHY_BREAK);
}

/*
 * One clipboard command (the request of cb is set up) as an asynchronous I/O
 * request. clipboard.device holds another program's read or write sequence
 * against this one until that program ends it, so the request can wait for as
 * long as that program wants. The wait is the DOS packet wait's: on the
 * request's reply port, the heartbeat timer and Ctrl-C. A Ctrl-C (it also asks
 * the app to stop) or a claim the heartbeat finds revoked abandons the
 * command: it is aborted (AbortIO, once) when abortable, and its result is
 * discarded, AAR_E_IO. Only the reply ends the wait: the request and its
 * buffers belong to the device until it replies, and the handle may not be
 * closed before. A device that does not grant AbortIO therefore keeps this
 * wait until it replies; nothing is reported as done that was not.
 * A command that was abandoned before it was sent is not sent.
 *
 * Not abortable (CMD_UPDATE): it does not wait for another program, it
 * commits what this sequence wrote, so it is waited out and its own result
 * stands; aborting it would leave the clip unknown.
 *
 * AAR_OK, or AAR_E_IO for a device error, an abandoned command, or an abort.
 */
static int32_t host_clip_io(struct aar_serve *s, struct ClipboardHandle *cb, int abortable)
{
    struct IOClipReq *req = &cb->cbh_Req;
    struct IORequest *io = (struct IORequest *)req;
    struct MsgPort *port = req->io_Message.mn_ReplyPort;
    ULONG mask = host_clip_mask(s, port);
    int abandoned = 0;
    int aborted = 0;
    int ctrl_c = ctrl_c_set(s);

    if (!aar_serve_packet_may_send(0, claim_live(s), ctrl_c)) {
        if (ctrl_c)
            host_clip_break(s);
        return AAR_E_IO;
    }
    X_SEND_IO(io);
    timer_reap(s);
    timer_arm(s);
    while (!X_CHECK_IO(io)) {
        ULONG signals = X_WAIT(mask);

        if ((signals & SIGBREAKF_CTRL_C) || ctrl_c_set(s)) {
            host_clip_break(s);
            abandoned = 1;
        }
        if (s->tm.port && (signals & (1UL << s->tm.port->mp_SigBit))) {
            timer_reap(s);
            if (aar_serve_packet_abandoned(0, claim_check(s)))
                abandoned = 1;
            else if (!abandoned)
                timer_arm(s);
        }
        if (abandoned && abortable && !aborted && !X_CHECK_IO(io)) {
            X_ABORT_IO(io);
            aborted = 1;
        }
    }
    (void)X_WAIT_IO(io);
    /* The reply's signal is not wanted by anyone any more. */
    X_SET_SIGNAL(0, 1UL << port->mp_SigBit);
    timer_reap(s);
    /* The heartbeat is re-armed for the loop, as after a DOS packet wait. */
    if (!s->closing && !s->in_step)
        timer_arm_for(s, timed_wake_us(s));
    if (abortable && (abandoned || !claim_live(s)))
        return AAR_E_IO;
    return req->io_Error ? AAR_E_IO : AAR_OK;
}

/* The whole stream of the clip: CMD_READ until the device gives no more. */
static int32_t host_clip_read(void *ctx, uint32_t unit, uint8_t **data, uint32_t *bytes)
{
    struct aar_serve *s = ctx;
    struct ClipboardHandle *cb;
    struct IOClipReq *req;
    uint8_t *buf = NULL;
    uint32_t cap = 0;
    uint32_t len = 0;
    uint32_t total = 0;
    int overflow = 0;
    int32_t rc = AAR_OK;

    cb = host_clip_open(s, unit, &rc);
    if (!cb)
        return rc;
    req = &cb->cbh_Req;
    req->io_ClipID = 0;
    for (;;) {
        if (cap - len < HOST_CLIP_CHUNK) {
            uint32_t grown = cap ? cap * 2u : 2u * HOST_CLIP_CHUNK;
            const uint32_t room = AAR_SERVE_CLIP_RAW_MAX + HOST_CLIP_CHUNK;
            uint8_t *bigger;

            /* Keep room for one final read or discard, not a second full buffer. */
            if (grown > room)
                grown = room;
            bigger = X_ALLOC_VEC(grown, MEMF_PUBLIC);

            if (!bigger) {
                rc = AAR_E_NOMEM;
                break;
            }
            if (buf) {
                memcpy(bigger, buf, len);
                X_FREE_VEC(buf);
            }
            buf = bigger;
            cap = grown;
        }
        req->io_Command = CMD_READ;
        req->io_Data = (STRPTR)(buf + len);
        req->io_Length = HOST_CLIP_CHUNK;
        req->io_Offset = total;
        rc = host_clip_io(s, cb, 1);
        if (rc != AAR_OK)
            break;
        if (req->io_Actual == 0)
            break;
        total += req->io_Actual;
        /* A clip that is too long is read to its end all the same: the
         * device wants the read finished. The bound is the longest text with
         * its wrapper; the core checks the text and the IFF limits proper. */
        if (overflow || len + req->io_Actual > AAR_SERVE_CLIP_RAW_MAX)
            overflow = 1;
        else
            len += req->io_Actual;
    }
    X_CLOSE_CLIPBOARD(s, cb);
    if (rc == AAR_OK && overflow)
        rc = AAR_E_NOMEM;
    if (rc == AAR_OK && len == 0)
        rc = AAR_E_NOENT;
    if (rc != AAR_OK) {
        if (buf)
            X_FREE_VEC(buf);
        return rc;
    }
    *data = buf;
    *bytes = len;
    return AAR_OK;
}

/* CMD_WRITE of the stream, then CMD_UPDATE: the clip is replaced. The clip is
 * the device's only when CMD_UPDATE is done: a write that was abandoned or
 * failed is dropped by the close and the old clip stays. */
static int32_t host_clip_write(void *ctx, uint32_t unit, const uint8_t *data, uint32_t bytes)
{
    struct aar_serve *s = ctx;
    struct ClipboardHandle *cb;
    struct IOClipReq *req;
    int32_t rc = AAR_OK;

    cb = host_clip_open(s, unit, &rc);
    if (!cb)
        return rc;
    req = &cb->cbh_Req;
    req->io_ClipID = 0;
    req->io_Offset = 0;
    req->io_Command = CMD_WRITE;
    req->io_Data = (STRPTR)data;
    req->io_Length = bytes;
    rc = host_clip_io(s, cb, 1);
    if (rc == AAR_OK) {
        req->io_Command = CMD_UPDATE;
        rc = host_clip_io(s, cb, 0);
    }
    X_CLOSE_CLIPBOARD(s, cb);
    return rc;
}

/* The host table; the instance copies it and sets ctx. */
static const struct aar_serve_host_ops aar_serve_host_backend = {
    .ctx = NULL,
    .live = host_live,
    .locale = host_locale,
    .datestamp = host_datestamp,
    .env_get = host_env_get,
    .mem_alloc = host_mem_alloc,
    .mem_free = host_mem_free,
    .clip_read = host_clip_read,
    .clip_write = host_clip_write,
};

/* The libraries this backend opened. */
static void host_close_libs(struct aar_serve *s)
{
    if (s->host.iffparse)
        X_CLOSE_LIB(s->host.iffparse);
    if (s->host.locale)
        X_CLOSE_LIB(s->host.locale);
    s->host.iffparse = NULL;
    s->host.locale = NULL;
}
