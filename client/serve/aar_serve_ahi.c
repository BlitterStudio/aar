/*
 * aar_serve: the AHI audio backend (the AARAudioRead feed).
 *
 * This file is part of aar_serve.c, which includes it after the services of the
 * loop and before the public calls; it is not compiled alone. State lives in the
 * instance (struct aar_serve in aar_serve_priv.h).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

/* Abort in-flight writes and drop their byte counts. Callers that are
 * retiring a live stream use ahi_reap instead, which publishes the tail. */
static void ahi_abort_pending(struct aar_serve *s)
{
    unsigned i;

    for (i = 0u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        if (s->ahi.request[i] && s->ahi.pending[i]) {
            if (!X_CHECK_IO((struct IORequest *)s->ahi.request[i]))
                X_ABORT_IO((struct IORequest *)s->ahi.request[i]);
            X_WAIT_IO((struct IORequest *)s->ahi.request[i]);
        }
        s->ahi.pending[i] = 0u;
        s->ahi.pending_bytes[i] = 0u;
    }
    if (s->ahi.port) {
        while (X_GET_MSG(s->ahi.port))
            ;
    }
}

/* A ring belongs to this stream only while the claim still runs. Cache its
 * count before close/end; never dereference its mapping after an ended status. */
static void ahi_count_underruns(struct aar_serve *s)
{
    AARStatusInfo st;
    const volatile uint8_t *p;
    uint32_t count;

    if (!s->ahi.ring || !claim_live(s))
        return;
    memset(&st, 0, sizeof(st));
    st.size = offsetof(AARStatusInfo, end_reason);
    if (claim_note(s, X_AAR_STATUS(s->handle, &st)) != AAR_OK ||
        (st.state != AAR_STATE_RUNNING && st.state != AAR_STATE_STOPPING))
        return;
    p = s->ahi.ring + AAR_AUDIO_RING_UNDERRUNS;
    count = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    s->ahi.underruns += count - s->ahi.ring_underruns;
    s->ahi.ring_underruns = count;
}

static void ahi_close(struct aar_serve *s)
{
    unsigned i;

    ahi_count_underruns(s);
    s->ahi.ring = NULL;
    ahi_abort_pending(s);
    if (s->ahi.device_open == 0 && s->ahi.request[0])
        X_CLOSE_DEVICE((struct IORequest *)s->ahi.request[0]);
    s->ahi.device_open = -1;
    if (s->ahi.request[1]) {
        X_FREE_VEC(s->ahi.request[1]);
        s->ahi.request[1] = NULL;
    }
    if (s->ahi.request[0]) {
        X_DELETE_IOREQ((struct IORequest *)s->ahi.request[0]);
        s->ahi.request[0] = NULL;
    }
    for (i = 0u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        if (s->ahi.buffer[i]) {
            X_FREE_VEC(s->ahi.buffer[i]);
            s->ahi.buffer[i] = NULL;
        }
    }
    if (s->ahi.port) {
        X_DELETE_PORT(s->ahi.port);
        s->ahi.port = NULL;
    }
    s->ahi.rate = 0u;
    s->ahi.channels = 0u;
}

static int ahi_open(struct aar_serve *s, uint32_t rate, uint32_t channels)
{
    unsigned i;

    if (channels != 2u || rate == 0u)
        return 0;
    s->ahi.port = X_CREATE_PORT();
    if (!s->ahi.port)
        return 0;
    s->ahi.request[0] = (struct AHIRequest *)X_CREATE_IOREQ(
        s->ahi.port, sizeof(*s->ahi.request[0]));
    if (!s->ahi.request[0])
        goto failed;
    s->ahi.request[0]->ahir_Version = 4;
    /* CMD_WRITE needs a real unit; AHI_NO_UNIT only opens the library
     * interface and has no unit to queue the request on. */
    s->ahi.device_open = X_OPEN_DEVICE((CONST_STRPTR)AHINAME, AHI_DEFAULT_UNIT,
                                 (struct IORequest *)s->ahi.request[0], 0);
    if (s->ahi.device_open)
        goto failed;
    s->ahi.request[1] = X_ALLOC_VEC(sizeof(*s->ahi.request[1]), MEMF_PUBLIC);
    if (!s->ahi.request[1])
        goto failed;
    X_COPY_MEM(s->ahi.request[0], s->ahi.request[1], sizeof(*s->ahi.request[1]));
    for (i = 0u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        s->ahi.buffer[i] = X_ALLOC_VEC(AAR_SERVE_AHI_BUFFER_BYTES,
                                 MEMF_PUBLIC | MEMF_CLEAR);
        if (!s->ahi.buffer[i])
            goto failed;
    }
    s->ahi.rate = rate;
    s->ahi.channels = channels;
    return 1;
failed:
    ahi_close(s);
    return 0;
}

static int ahi_free_slot(struct aar_serve *s)
{
    unsigned i;

    for (i = 0u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        if (!s->ahi.pending[i])
            return (int)i;
    }
    return -1;
}

static int ahi_reap(struct aar_serve *s)
{
    struct Message *message;
    unsigned i;
    int completed = 0;

    while ((message = X_GET_MSG(s->ahi.port)) != NULL) {
        for (i = 0u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
            if (s->ahi.request[i] &&
                message == &s->ahi.request[i]->ahir_Std.io_Message) {
                if (s->ahi.pending[i]) {
                    s->ahi.pending_bytes[i] = 0u;
                    s->ahi.pending[i] = 0u;
                    completed = 1;
                }
                break;
            }
        }
    }
    return completed;
}

static void ahi_submit(struct aar_serve *s, unsigned slot, uint32_t bytes)
{
    struct AHIRequest *request = s->ahi.request[slot];
    struct AHIRequest *link = NULL;
    unsigned i;

    for (i = 0u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        if (s->ahi.pending[i]) {
            link = s->ahi.request[i];
            break;
        }
    }
    request->ahir_Std.io_Command = CMD_WRITE;
    request->ahir_Std.io_Length = bytes;
    request->ahir_Std.io_Data = s->ahi.buffer[slot];
    if (!link)
        request->ahir_Std.io_Offset = 0;
    request->ahir_Type = AHIST_S16S;
    request->ahir_Frequency = s->ahi.rate;
    request->ahir_Volume = 0x10000L;
    request->ahir_Position = 0x8000L;
    request->ahir_Link = link;
    s->ahi.pending_bytes[slot] = bytes;
    s->ahi.pending[slot] = 1u;
    X_SEND_IO((struct IORequest *)request);
}

/* The frames AARAudioRead could hand over now: a read with no buffer and no
 * frames returns the readable count and does not count an underrun. */
static uint32_t audio_available(struct aar_serve *s)
{
    ULONG got = 0;

    if (!claim_live(s) ||
        claim_note(s, X_AAR_AUDIO_READ(s->handle, NULL, 0, &got)) != AAR_OK)
        return 0u;
    return got;
}

static int ahi_idle(struct aar_serve *s)
{
    unsigned i;

    for (i = 0u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        if (s->ahi.pending[i])
            return 0;
    }
    return 1;
}

/* Moves PCM from the app's audio ring to AHI: one buffer of at most 1/50 s
 * per pass, the way the earlier launcher did. AARAudioRead copies the
 * ring's little-endian words into samples in CPU order. */
static void service_ahi_audio(struct aar_serve *s)
{
    uint32_t available, frames;
    ULONG got = 0;
    uint32_t bytes;
    int completed;
    int slot;

    if (!s->core.audio.open || !s->ahi.request[0])
        return;
    completed = ahi_reap(s);
    available = audio_available(s);
    ahi_count_underruns(s);
    frames = aar_serve_audio_chunk_frames(available, s->ahi.rate, s->ahi.channels,
                                          AAR_SERVE_AHI_BUFFER_BYTES);
    if (frames == 0u) {
        /* An underrun: a buffer just finished and AHI has nothing queued.
         * A read that finds less than asked counts it in the ring header. */
        if (completed && available == 0u && ahi_idle(s)) {
            (void)claim_note(s, X_AAR_AUDIO_READ(s->handle, s->ahi.buffer[0], 1u,
                                          &got));
            ahi_count_underruns(s);
        }
        return;
    }
    slot = ahi_free_slot(s);
    if (slot < 0)
        return;
    if (claim_note(s, X_AAR_AUDIO_READ(s->handle, s->ahi.buffer[slot], frames,
                                &got)) != AAR_OK || got == 0u)
        return;
    bytes = got * s->ahi.channels * 2u;
    ahi_submit(s, (unsigned)slot, bytes);
}

/* The audio ring the app writes: the library attaches it when aar_serve completes
 * the AUDIO_OPEN request with AAR_OK and detaches it at AUDIO_CLOSE, so aar_serve
 * only opens and closes AHI here. */
static int audio_op_open(void *ctx, uint32_t rate, uint32_t channels,
                         uint32_t token, uint32_t bytes, uint32_t frames)
{
    struct aar_serve *s = ctx;

    (void)token;
    (void)bytes;
    (void)frames;
    if (!ahi_open(s, rate, channels)) {
        ahi_close(s);
        /* An AHI open failure on the AUDIO=AHI route ends the run with an
         * error, as before. */
        launcher_say(s, "cannot open AHI for AUDIO=AHI.");
        s->audio_open_failed = 1;
        want_stop(s, AAR_SERVE_WHY_AUDIO);
        return 0;
    }
    return 1;
}

static void audio_op_close(void *ctx)
{
    struct aar_serve *s = ctx;

    ahi_close(s);
}

/* The audio table of the built-in backend; the instance copies it and sets ctx. */
static const struct aar_serve_audio_ops aar_serve_ahi_backend = {
    NULL, audio_op_open, audio_op_close
};
