/*
 * Runner side of C-AUDIO (conformance/TESTS.md): the AHI route and the other apps of
 * the stream-count test.
 *
 *   - C-AUDIO.10 and C-AUDIO.11: the runner serves the payload by hand with the route
 *     AHI (AAR_TAG_AUDIO_ROUTE): it answers the AUDIO_OPEN request, reads the PCM the app
 *     writes with AARAudioRead (the frames available with frames 0, then the frames) and
 *     compares it with the ramp of payloads/conf_audio.h. The loop does not use aar_serve:
 *     aar_serve's own AHI route would play the stream instead of handing it to the test.
 *   - C-AUDIO.9: the payload asks (keyword CONF-AUDIO-MORE) for other apps that open a
 *     stream and hold it, started by hand one after the other until one is refused.
 *   - C-AUDIO.12 (operator): the variant tone plays 440 Hz for two seconds, with
 *     INTERACTIVE.
 *
 * Assumptions for the card run: the platform grants the route AHI to a client that asks for
 * it with AAR_TAG_AUDIO_ROUTE; AARServiceReply of an AUDIO_OPEN completion that echoes the
 * request's buffer and frames is accepted; the ring is little-endian and AARAudioRead
 * returns the samples in 68k order.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "r_mq.h"
#include "../payloads/conf_audio.h"

#define MAX_HOLD 6

/* ---- C-AUDIO.9: other apps that hold a stream ------------------------------------------------ */

static void peer_more(struct conf_run *run, const AARMessage *msg)
{
    struct mq_app *apps[MAX_HOLD];
    ULONG opened = 0;
    LONG refusal = 0;
    unsigned n = 0;
    unsigned i;
    AARMessage am;

    (void)msg;
    while (n < MAX_HOLD) {
        struct mq_app *a = AllocVec(sizeof *a, MEMF_ANY | MEMF_CLEAR);
        AARMessage m;

        if (!a) {
            refusal = AAR_E_NOMEM;
            break;
        }
        apps[n++] = a;
        if (mq_start(a, "C-AUDIO", "hold", NULL, 0) != AAR_OK) {
            conf_emitf("# C-AUDIO.9: another app did not start: %.80s", a->why);
            refusal = AAR_E_IO;
            break;
        }
        if (!mq_wait_message(a, &m, 8000) || !ca_is_hold_marker(m.bytes)) {
            refusal = AAR_E_TIMEDOUT;
            break;
        }
        if ((LONG)cm_get32(m.bytes, 4) == AAR_OK) {
            opened++;
        } else {
            refusal = (LONG)cm_get32(m.bytes, 4);
            break;
        }
    }
    for (i = 0; i < n; i++) {
        mq_finish(apps[i], 4000);
        FreeVec(apps[i]);
    }
    cm_make_result(am.bytes, refusal == 0 || refusal == AAR_E_BUSY, opened, 0, CM_NONE,
                   (ULONG)refusal, 0);
    for (i = 0; i < 100; i++) {
        if (AARSendMessage(run->h, &am) != AAR_E_BUSY)
            break;
        Delay(1);
    }
}

static const struct conf_peer peers[] = {
    { CA_K_MORE, peer_more },
};

/* ---- C-AUDIO.10 and C-AUDIO.11: the route AHI ------------------------------------------------- */

static WORD pcm[4096];

/* Grants the app's AUDIO_OPEN: the rate asked for, two channels, the ring the app named. */
static LONG answer_open(struct mq_app *a, AARServiceReq *req, ULONG *frames_out)
{
    ULONG channels = 2UL;
    ULONG room = req->buf_len > AAR_AUDIO_RING_HDR_BYTES ?
                 (req->buf_len - AAR_AUDIO_RING_HDR_BYTES) / (channels * 2UL) : 0UL;
    ULONG frames = 1UL;

    while (frames * 2UL <= room)
        frames <<= 1;
    req->result = room ? AAR_OK : AAR_E_INVAL;
    req->out[AAR_R_AUDIO_OPEN_RATE] = req->arg[AAR_A_AUDIO_OPEN_RATE];
    req->out[AAR_R_AUDIO_OPEN_CHANNELS] = channels;
    req->out[AAR_R_AUDIO_OPEN_RING] = req->buf;
    req->out[AAR_R_AUDIO_OPEN_RING_BYTES] = req->buf_len;
    req->out[AAR_R_AUDIO_OPEN_FRAMES] = frames;
    *frames_out = frames;
    return mq_reply(a, req);
}

static void t_ahi(int idx)
{
    struct mq_app a;
    struct TagItem tags[2];
    AARServiceReq req;
    ULONG frames_cap = 0;
    ULONG collected = 0, mismatches = 0, first_bad = 0xFFFFFFFFUL;
    ULONG max_avail = 0;
    LONG query_rc = AAR_OK;
    int query_seen = 0;
    int opened = 0;
    LONG open_reply = AAR_OK;
    ULONG deadline;
    char d[CONF_DETAIL_MAX];
    int ok10, ok11;

    (void)idx;
    tags[0].ti_Tag = AAR_TAG_AUDIO_ROUTE;
    tags[0].ti_Data = AAR_AUDIO_ROUTE_AHI;
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0;
    if (mq_start(&a, "C-AUDIO", "ahi", tags, 0) != AAR_OK) {
        conf_detailf(d, "the route AHI app did not start: %.80s", a.why);
        conf_record("C-AUDIO.10", 0, d);
        conf_record("C-AUDIO.11", 0, d);
        mq_finish(&a, 3000);
        return;
    }
    deadline = conf_ticks() + mq_ticks(40000);
    while (!mq_ended(&a) && collected < CA_RAMP_FRAMES && (LONG)(conf_ticks() - deadline) < 0) {
        while (mq_next(&a, &req) == AAR_OK) {
            if (req.op == AAR_OP_AUDIO_OPEN) {
                open_reply = answer_open(&a, &req, &frames_cap);
                opened = open_reply == AAR_OK;
            } else if (req.op == AAR_OP_AUDIO_CLOSE) {
                req.result = AAR_OK;
                (void)mq_reply(&a, &req);
            } else {
                mq_answer(&a, &req);
            }
        }
        if (opened) {
            ULONG avail = 0xFFFFFFFFUL;
            LONG qrc = AARAudioRead(a.h, NULL, 0, &avail);

            if (!query_seen) {
                query_rc = qrc;
                query_seen = 1;
            }
            if (qrc == AAR_OK) {
                if (avail > max_avail)
                    max_avail = avail;
                if (avail > 0) {
                    ULONG n = avail < 2048UL ? avail : 2048UL;
                    ULONG got = 0;

                    if (AARAudioRead(a.h, pcm, n, &got) == AAR_OK) {
                        ULONG j;

                        for (j = 0; j < got; j++) {
                            ULONG f = collected + j;

                            if (pcm[2 * j] != (WORD)ca_ramp_left(f) ||
                                pcm[2 * j + 1] != (WORD)ca_ramp_right(f)) {
                                mismatches++;
                                if (first_bad == 0xFFFFFFFFUL)
                                    first_bad = f;
                            }
                        }
                        collected += got;
                    }
                }
            }
        }
        Delay(1);
    }
    mq_finish(&a, 5000);
    if (!opened) {
        conf_detailf(d, "the audio stream was not opened (the reply to AUDIO_OPEN returned %ld)",
                     (long)open_reply);
        conf_record("C-AUDIO.10", 0, d);
        conf_record("C-AUDIO.11", 0, d);
        return;
    }
    ok10 = query_seen && query_rc == AAR_OK && max_avail > 0 && max_avail <= frames_cap;
    conf_detailf(d, "AARAudioRead with 0 frames returned %ld, at most %lu of %lu frames available",
                 (long)query_rc, (unsigned long)max_avail, (unsigned long)frames_cap);
    conf_record("C-AUDIO.10", ok10, d);
    ok11 = collected == CA_RAMP_FRAMES && mismatches == 0;
    if (mismatches)
        conf_detailf(d, "%lu of %lu frames differ from the ramp, the first at frame %lu",
                     (unsigned long)mismatches, (unsigned long)collected, (unsigned long)first_bad);
    else
        conf_detailf(d, "%lu of %lu ramp frames read, all as written", (unsigned long)collected,
                     (unsigned long)CA_RAMP_FRAMES);
    conf_record("C-AUDIO.11", ok11, d);
}

/* ---- Variants ------------------------------------------------------------------------------------ */

static void prep_std(struct conf_run *run)
{
    run->timeout_s = 150;
}

static void prep_tone(struct conf_run *run)
{
    if (!conf_g.interactive)
        return; /* the payload ends at once without conf:show */
    run->extra_args[run->extra_count++] = "conf:show=1";
    conf_emitf("# C-AUDIO.12: a 440 Hz tone plays for two seconds: listen for it");
}

static const struct conf_operator operators[] = {
    { "C-AUDIO.12", "tone", NULL, NULL, 0 },
};

static const struct conf_variant variants[] = {
    { "std", prep_std },
    { "tone", prep_tone },
};

static const struct conf_r_test tests[] = {
    { "C-AUDIO.10", t_ahi },
    { "C-AUDIO.11", t_ahi },
};

const struct conf_area conf_area_audio = {
    .name = "C-AUDIO",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
    .operators = operators,
    .operator_count = CONF_COUNT(operators),
};
