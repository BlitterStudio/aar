/*
 * aar_serve: the AHI audio backend (the AARAudioRead feed).
 *
 * This file is part of aar_serve.c, which includes it after the services of the
 * loop and before the public calls; it is not compiled alone. State lives in the
 * instance (struct aar_serve in aar_serve_priv.h).
 *
 * The feed: PCM is copied from the app's ring into a client-side reserve (a FIFO
 * of frames), AHI is started once two requests' worth is buffered, and from then
 * on every request is the same size, taken from the reserve (the AHI developer
 * guide's double buffering: two requests, each linked to the other). A ring that
 * the app fills just ahead of playback is thus never cut into short requests,
 * which some drivers play with a gap between them.
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
    for (i = 1u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        if (s->ahi.request[i]) {
            X_FREE_VEC(s->ahi.request[i]);
            s->ahi.request[i] = NULL;
        }
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
    if (s->ahi.reserve) {
        X_FREE_VEC(s->ahi.reserve);
        s->ahi.reserve = NULL;
    }
    if (s->ahi.port) {
        X_DELETE_PORT(s->ahi.port);
        s->ahi.port = NULL;
    }
    s->ahi.rate = 0u;
    s->ahi.channels = 0u;
    s->ahi.rsv_head = 0u;
    s->ahi.rsv_fill = 0u;
    s->ahi.started = 0;
    s->ahi.dry = 0;
    s->ahi.draining = 0;
}

static int ahi_open(struct aar_serve *s, uint32_t rate, uint32_t channels,
                    uint32_t ring_frames)
{
    unsigned i;

    if (channels != 2u || rate == 0u || ring_frames == 0u)
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
    for (i = 1u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        s->ahi.request[i] = X_ALLOC_VEC(sizeof(*s->ahi.request[i]), MEMF_PUBLIC);
        if (!s->ahi.request[i])
            goto failed;
        X_COPY_MEM(s->ahi.request[0], s->ahi.request[i], sizeof(*s->ahi.request[i]));
    }
    for (i = 0u; i < AAR_SERVE_AHI_BUFFER_COUNT; i++) {
        s->ahi.buffer[i] = X_ALLOC_VEC(AAR_SERVE_AHI_BUFFER_BYTES,
                                 MEMF_PUBLIC | MEMF_CLEAR);
        if (!s->ahi.buffer[i])
            goto failed;
    }
    s->ahi.reserve = X_ALLOC_VEC(AAR_SERVE_AHI_RESERVE_BYTES, MEMF_PUBLIC);
    if (!s->ahi.reserve)
        goto failed;
    s->ahi.reserve_frames = AAR_SERVE_AHI_RESERVE_BYTES / (channels * 2u);
    s->ahi.ring_frames = ring_frames;
    s->ahi.last_data_ms = now_ms(s);
    s->ahi.tail = 0;
    s->ahi.rate = rate;
    s->ahi.channels = channels;
    s->ahi.rsv_head = 0u;
    s->ahi.rsv_fill = 0u;
    s->ahi.started = 0;
    s->ahi.dry = 0;
    s->ahi.draining = 0;
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
    uint32_t frames = bytes / (s->ahi.channels * 2u);

    if (s->ahi.pending[s->ahi.last])
        link = s->ahi.request[s->ahi.last]; /* the other request: still queued */
    request->ahir_Std.io_Command = CMD_WRITE;
    request->ahir_Std.io_Length = bytes;
    request->ahir_Std.io_Data = s->ahi.buffer[slot];
    /* io_Offset must be 0 and the device trashes it, and ahir_Link, when a
     * request finishes: every field is set again for every write. */
    request->ahir_Std.io_Offset = 0;
    request->ahir_Type = AHIST_S16S;
    request->ahir_Frequency = s->ahi.rate;
    request->ahir_Volume = 0x10000L;
    request->ahir_Position = 0x8000L;
    request->ahir_Link = link;
    s->ahi.pending_bytes[slot] = bytes;
    s->ahi.pending[slot] = 1u;
    s->ahi.last = slot;
    s->ahi.requests++;
    s->ahi.total_frames += frames;
    if (s->ahi.requests == 1u || frames < s->ahi.min_frames)
        s->ahi.min_frames = frames;
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

/* One request: up to 3/50 s (60 ms), and never more than the ring holds. */
static uint32_t ahi_request_frames(struct aar_serve *s)
{
    uint32_t frames = aar_serve_audio_chunk_frames(0xffffffffu, s->ahi.rate,
                                                   s->ahi.channels,
                                                   AAR_SERVE_AHI_BUFFER_BYTES);

    return frames < s->ahi.ring_frames ? frames : s->ahi.ring_frames;
}

/* Ring to reserve: at most 20 ms per pass (a read from the card is slow, and the
 * loop has to be on time), up to two requests' worth in the reserve, so that the
 * latency stays bounded when the app writes far ahead. Returns the frames the
 * ring still holds. */
static uint32_t ahi_fill_reserve(struct aar_serve *s, uint32_t available)
{
    uint32_t frame_bytes = s->ahi.channels * 2u;
    uint32_t cap = s->ahi.reserve_frames;
    uint32_t target = 2u * ahi_request_frames(s);
    uint32_t budget = s->ahi.rate / 50u;

    if (target > cap)
        target = cap;
    while (budget != 0u && available != 0u && s->ahi.rsv_fill < target) {
        uint32_t tail = (s->ahi.rsv_head + s->ahi.rsv_fill) % cap;
        uint32_t n = target - s->ahi.rsv_fill;
        ULONG got = 0;

        if (n > available)
            n = available;
        if (n > cap - tail)
            n = cap - tail;
        if (n > budget)
            n = budget;
        if (claim_note(s, X_AAR_AUDIO_READ(s->handle,
                                           s->ahi.reserve + tail * frame_bytes,
                                           n, &got)) != AAR_OK || got == 0u)
            break;
        s->ahi.rsv_fill += got;
        available = got < available ? available - got : 0u;
        budget = got < budget ? budget - got : 0u;
        s->ahi.last_data_ms = now_ms(s);
        s->ahi.tail = 0;
    }
    return available;
}

/* Reserve to a request buffer. */
static void ahi_take(struct aar_serve *s, uint8_t *dst, uint32_t frames)
{
    uint32_t frame_bytes = s->ahi.channels * 2u;
    uint32_t cap = s->ahi.reserve_frames;
    uint32_t first = cap - s->ahi.rsv_head;

    if (first > frames)
        first = frames;
    memcpy(dst, s->ahi.reserve + s->ahi.rsv_head * frame_bytes, first * frame_bytes);
    if (first < frames)
        memcpy(dst + first * frame_bytes, s->ahi.reserve,
               (frames - first) * frame_bytes);
    s->ahi.rsv_head = (s->ahi.rsv_head + frames) % cap;
    s->ahi.rsv_fill -= frames;
}

/* Queues the reserve into the free requests: whole requests, and, when
 * `flush` says nothing more is coming, a last short one. */
static unsigned ahi_queue(struct aar_serve *s, int flush)
{
    uint32_t request = ahi_request_frames(s);
    unsigned submitted = 0u;

    for (;;) {
        uint32_t frames = s->ahi.rsv_fill < request ? s->ahi.rsv_fill : request;
        int slot;

        if (frames == 0u || (frames < request && !flush))
            break;
        slot = ahi_free_slot(s);
        if (slot < 0)
            break;
        ahi_take(s, s->ahi.buffer[slot], frames);
        ahi_submit(s, (unsigned)slot, frames * s->ahi.channels * 2u);
        submitted++;
    }
    return submitted;
}

/* Moves PCM from the app's audio ring to AHI. AHI is not started before two
 * requests' worth is in the reserve (the prebuffer, about 120 ms); after that
 * every request is a whole one. If the reserve cannot keep AHI fed, AHI runs dry:
 * that is counted (ahi_gaps), a read of the empty ring is counted in the ring
 * header too (a ring underrun), and the stream is buffered again before it goes
 * on; short requests are not used to hide it. When the app stops writing (no new
 * frames for 30 ms) what is left in the reserve is played as a last, short
 * request: the end of a sound. AARAudioRead copies the ring's little-endian
 * words into samples in CPU order. */
static void service_ahi_audio(struct aar_serve *s)
{
    uint32_t available, left;
    int completed;
    int stalled;
    unsigned submitted;
    ULONG got = 0;

    if ((!s->core.audio.open && !s->ahi.draining) || !s->ahi.request[0])
        return;
    completed = ahi_reap(s);
    if (s->ahi.draining) {
        /* The app closed the audio: the ring is gone, what is queued plays out. */
        (void)ahi_queue(s, 1);
        if (s->ahi.rsv_fill == 0u && ahi_idle(s))
            ahi_close(s);
        return;
    }
    available = audio_available(s);
    /* The ring's underrun word is read now and then, not every pass: it costs a
     * status read, and the loop has to be on time. */
    if ((++s->ahi.passes & 63u) == 0u)
        ahi_count_underruns(s);
    left = ahi_fill_reserve(s, available);
    stalled = s->ahi.rsv_fill != 0u && left == 0u &&
              (uint32_t)(now_ms(s) - s->ahi.last_data_ms) >= 30u;
    if (!s->ahi.started) {
        if (s->ahi.rsv_fill < 2u * ahi_request_frames(s) && !stalled)
            return; /* prebuffering */
        s->ahi.started = 1;
        s->ahi.dry = 0;
    }
    submitted = ahi_queue(s, stalled);
    if (stalled && submitted)
        s->ahi.tail = 1; /* the end of a sound: not a gap when it has played */
    if (!s->ahi.tail && ahi_idle(s) && completed &&
        s->ahi.rsv_fill < ahi_request_frames(s)) {
        /* AHI has nothing left to play and the reserve is empty: it ran dry.
         * A read that finds less than asked counts it in the ring header. */
        if (!s->ahi.dry) {
            s->ahi.dry = 1;
            s->ahi.gaps++;
        }
        if (left == 0u) {
            (void)claim_note(s, X_AAR_AUDIO_READ(s->handle, s->ahi.buffer[0],
                                                 1u, &got));
            ahi_count_underruns(s);
        }
        s->ahi.started = 0; /* prebuffer again */
    }
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
    if (s->ahi.port)
        ahi_close(s); /* a stream that was still playing out its tail */
    if (!ahi_open(s, rate, channels, frames)) {
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

/* The ring is detached with the close: what the reserve and AHI still hold plays
 * out, and the device is closed when it has. */
static void audio_op_close(void *ctx)
{
    struct aar_serve *s = ctx;

    ahi_count_underruns(s);
    s->ahi.ring = NULL;
    if (s->ahi.rsv_fill != 0u || !ahi_idle(s)) {
        s->ahi.draining = 1;
        return;
    }
    ahi_close(s);
}

/* The audio table of the built-in backend; the instance copies it and sets ctx. */
static const struct aar_serve_audio_ops aar_serve_ahi_backend = {
    NULL, audio_op_open, audio_op_close
};
