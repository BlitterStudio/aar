/*
 * The manual run of a payload (r_mq.h): open, load, start, and serve with
 * AARServiceNext and AARServiceReply. See conformance/CONTRIBUTING.md ("A manual
 * service loop").
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "r_mq.h"

ULONG mq_ticks(ULONG ms)
{
    ULONG t = (ms * CONF_TICKS_PER_SECOND + 999UL) / 1000UL;

    return t ? t : 1UL;
}

void mq_sleep_ms(ULONG ms)
{
    Delay((LONG)mq_ticks(ms));
}

LONG mq_start(struct mq_app *a, const char *area, const char *variant,
              const struct TagItem *tags, ULONG text_max)
{
    char file[40];
    char path[256];
    char variant_arg[48];
    const char *argv[3];
    ULONG reason = 0;
    LONG rc;
    size_t i;

    memset(a, 0, sizeof *a);
    if (text_max) {
        a->text = AllocVec(text_max + 1UL, MEMF_ANY | MEMF_CLEAR);
        if (!a->text) {
            snprintf(a->why, sizeof a->why, "no memory for the console text");
            return AAR_E_NOMEM;
        }
        a->text_max = text_max;
    }
    snprintf(file, sizeof file, "conf_%s.elf", area + 2);
    for (i = 0; file[i]; i++) {
        if (file[i] >= 'A' && file[i] <= 'Z')
            file[i] = (char)(file[i] - 'A' + 'a');
    }
    snprintf(path, sizeof path, "%s", conf_g.dir);
    if (!AddPart((STRPTR)path, (CONST_STRPTR)conf_g.profile_name, sizeof path) ||
        !AddPart((STRPTR)path, (CONST_STRPTR)file, sizeof path)) {
        snprintf(a->why, sizeof a->why, "payload path too long");
        return AAR_E_INVAL;
    }
    rc = AAROpen(&a->h, NULL, &reason);
    if (rc != AAR_OK) {
        a->h = NULL;
        snprintf(a->why, sizeof a->why, "AAROpen returned %ld (reason %lu)", (long)rc,
                 (unsigned long)reason);
        return rc;
    }
    if (tags) {
        rc = AARSetTagList(a->h, (struct TagItem *)tags);
        if (rc != AAR_OK) {
            snprintf(a->why, sizeof a->why, "AARSetTagList returned %ld", (long)rc);
            return rc;
        }
    }
    rc = AARLoadFile(a->h, path);
    if (rc != AAR_OK) {
        snprintf(a->why, sizeof a->why, "load of %.40s refused: %ld", file, (long)rc);
        return rc;
    }
    snprintf(variant_arg, sizeof variant_arg, "conf:variant=%s", variant);
    argv[0] = file;
    argv[1] = "conf:peer=1";
    argv[2] = variant_arg;
    rc = AARStart(a->h, argv, 3);
    if (rc != AAR_OK) {
        snprintf(a->why, sizeof a->why, "start of %.40s refused: %ld", file, (long)rc);
        return rc;
    }
    a->started = 1;
    return AAR_OK;
}

LONG mq_reply(struct mq_app *a, const AARServiceReq *req)
{
    LONG rc = AAR_E_BUSY;
    int tries;

    for (tries = 0; tries < 100 && rc == AAR_E_BUSY; tries++) {
        rc = AARServiceReply(a->h, req);
        if (rc == AAR_E_BUSY)
            Delay(1);
    }
    return rc;
}

void mq_answer(struct mq_app *a, AARServiceReq *req)
{
    switch (req->op) {
    case AAR_OP_CONSOLE_WRITE:
        if (a->text && req->ptr) {
            ULONG room = a->text_max - a->text_len;
            ULONG take = req->buf_len < room ? req->buf_len : room;

            memcpy(a->text + a->text_len, req->ptr, take);
            a->text_len += take;
            a->text[a->text_len] = '\0';
        }
        req->result = AAR_OK;
        req->out[AAR_R_CONSOLE_COUNT] = req->buf_len;
        break;
    case AAR_OP_CONSOLE_READ:
        req->result = AAR_OK;
        req->out[AAR_R_CONSOLE_COUNT] = 0;
        break;
    default:
        req->result = AAR_E_NOSYS;
        break;
    }
    (void)mq_reply(a, req);
}

LONG mq_next(struct mq_app *a, AARServiceReq *req)
{
    LONG rc;

    memset(req, 0, sizeof *req);
    req->size = sizeof *req;
    rc = AARServiceNext(a->h, req);
    if (rc == AAR_OK)
        a->seen_ok++;
    else if (rc == AAR_E_BUSY)
        a->seen_busy++;
    else {
        a->seen_other++;
        a->other_rc = rc;
    }
    return rc;
}

/* Takes every waiting request. Console ops are answered (unless stash_console is
 * set), the others are kept for mq_wait_request. */
static void pump(struct mq_app *a)
{
    unsigned guard;

    for (guard = 0; guard < 64; guard++) {
        AARServiceReq req;
        LONG rc = mq_next(a, &req);

        if (rc != AAR_OK)
            break;
        if (!a->stash_console &&
            (req.op == AAR_OP_CONSOLE_WRITE || req.op == AAR_OP_CONSOLE_READ)) {
            mq_answer(a, &req);
        } else if (a->stash_n < MQ_STASH) {
            a->stash[a->stash_n++] = req;
        } else {
            req.result = AAR_E_NOSYS;
            (void)mq_reply(a, &req);
        }
    }
}

int mq_ended(struct mq_app *a)
{
    AARStatusInfo st;

    memset(&st, 0, sizeof st);
    st.size = 16;
    if (!a->h || AARStatus(a->h, &st) != AAR_OK)
        return 1;
    return (st.state == AAR_STATE_IDLE || st.state == AAR_STATE_FAULTED) &&
           st.end_reason != AAR_END_NONE;
}

int mq_wait_request(struct mq_app *a, AARServiceReq *req, ULONG ms)
{
    ULONG deadline = conf_ticks() + mq_ticks(ms);

    for (;;) {
        pump(a);
        if (a->stash_n) {
            *req = a->stash[0];
            a->stash_n--;
            memmove(&a->stash[0], &a->stash[1], a->stash_n * sizeof a->stash[0]);
            return 1;
        }
        if (mq_ended(a) || (LONG)(conf_ticks() - deadline) >= 0)
            return 0;
        Delay(1);
    }
}

int mq_wait_message(struct mq_app *a, AARMessage *m, ULONG ms)
{
    ULONG deadline = conf_ticks() + mq_ticks(ms);

    for (;;) {
        pump(a);
        if (AARGetMessage(a->h, m) == AAR_OK)
            return 1;
        if ((LONG)(conf_ticks() - deadline) >= 0)
            return 0;
        if (mq_ended(a)) {
            /* what the app sent before it ended is still readable */
            return AARGetMessage(a->h, m) == AAR_OK;
        }
        Delay(1);
    }
}

void mq_serve_for(struct mq_app *a, ULONG ms)
{
    ULONG deadline = conf_ticks() + mq_ticks(ms);

    while ((LONG)(conf_ticks() - deadline) < 0) {
        pump(a);
        Delay(1);
    }
}

void mq_finish(struct mq_app *a, ULONG grace_ms)
{
    if (a->h) {
        if (a->started) {
            ULONG deadline = conf_ticks() + mq_ticks(grace_ms);

            (void)AARStop(a->h);
            a->stash_console = 0;
            while (!mq_ended(a) && (LONG)(conf_ticks() - deadline) < 0) {
                pump(a);
                Delay(1);
            }
        }
        (void)AARClose(a->h);
        a->h = NULL;
    }
    if (a->text)
        FreeVec(a->text);
    a->text = NULL;
}
