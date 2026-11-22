/*
 * A manual run of a payload: the runner opens the handle, loads and starts the app
 * itself and serves it with AARServiceNext and AARServiceReply, instead of aar_serve
 * (aar-conf.c always uses aar_serve). It is for the tests that must withhold,
 * delay or misaddress completions, run several apps at once or read what an app left
 * behind: C-QUEUE, C-AUDIO and C-MSG. Shared by r_queue.c and r_audio.c.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_R_MQ_H
#define AAR_CONF_R_MQ_H

#include "aar-conf.h"

#define MQ_STASH 8

struct mq_app {
    AARHandle *h;
    int started;
    UBYTE *text;               /* console text of the app, when mq_start was given room */
    ULONG text_len, text_max;
    char why[CONF_DETAIL_MAX]; /* why mq_start failed */
    /* requests taken while waiting for something else, in order (see mq_wait_request) */
    AARServiceReq stash[MQ_STASH];
    unsigned stash_n;
    int stash_console;         /* keep console requests too, unanswered */
    /* what AARServiceNext returned so far */
    ULONG seen_ok, seen_busy, seen_other;
    LONG other_rc;
};

/* Opens a handle for conf_<area>.elf of the profile, applies tags, loads it and starts
 * it with the arguments name, conf:peer=1 and conf:variant=<variant>. text_max is the
 * room for the app's console output (0: it is not kept). 0 on success; else a negative
 * AAR code, with a->why filled. The handle is closed by mq_finish even after a failure. */
LONG mq_start(struct mq_app *a, const char *area, const char *variant,
              const struct TagItem *tags, ULONG text_max);

/* Answers a request the way a plain client does: console writes are kept in a->text
 * and consumed, console reads end, every other op is AAR_E_NOSYS. */
void mq_answer(struct mq_app *a, AARServiceReq *req);

/* Takes one request without waiting: AAR_OK (req filled, not answered), AAR_E_BUSY or an
 * error. The size is set. */
LONG mq_next(struct mq_app *a, AARServiceReq *req);

/* Replies to req, retrying while the completion queue is full. */
LONG mq_reply(struct mq_app *a, const AARServiceReq *req);

/* Waits up to ms for a request that is not a console op (console ops are answered). 1: req
 * holds it, not answered. 0: none came, or the app ended. */
int mq_wait_request(struct mq_app *a, AARServiceReq *req, ULONG ms);

/* Waits up to ms for a message from the app, answering console requests meanwhile and
 * keeping the other requests unanswered. 1 with *m, 0 on time out. */
int mq_wait_message(struct mq_app *a, AARMessage *m, ULONG ms);

/* Serves for ms (console ops answered) without looking at messages. */
void mq_serve_for(struct mq_app *a, ULONG ms);

/* The app has ended (exit, stop or fault). */
int mq_ended(struct mq_app *a);

/* Ask the app to stop, wait up to grace_ms for it to end while answering its console, then
 * close the handle and free the text. Safe on a failed mq_start. */
void mq_finish(struct mq_app *a, ULONG grace_ms);

/* Delay in milliseconds, 20 ms steps. */
void mq_sleep_ms(ULONG ms);

/* 1/50 s ticks for a time in ms. */
ULONG mq_ticks(ULONG ms);

#endif
