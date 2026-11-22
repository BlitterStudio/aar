/*
 * Runner side of C-CON (conformance/TESTS.md): the client of the payload conf_con.
 * It shapes the console of each run (variants: plain, slow output, scripted
 * input, input that never comes), stores the log records the app sends, and
 * answers the peer requests of conf_con.h by looking into its console capture
 * and its log store. The tests themselves are judged by the payload. This file
 * belongs to the C-CON author.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "aar-conf.h"
#include "../payloads/conf_con.h"

/* ---- Answers ------------------------------------------------------------------- */

static void reply(struct conf_run *run, ULONG w0, ULONG w1)
{
    AARMessage m;

    conf_con_build(m.bytes, CONF_CON_RES, (uint32_t)w0, (uint32_t)w1, 0, 0);
    (void)AARSendMessage(run->h, &m);
}

static const UBYTE *find_bytes(const UBYTE *hay, ULONG hay_len, const UBYTE *needle,
                               ULONG needle_len)
{
    ULONG i;

    if (needle_len == 0 || hay_len < needle_len)
        return NULL;
    for (i = 0; i + needle_len <= hay_len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    }
    return NULL;
}

/* ---- The log store (variant main) ----------------------------------------------------- */

#define LOG_SLOTS 40UL

static struct {
    ULONG count;
    ULONG lost;                     /* records that did not fit the store */
    ULONG len[LOG_SLOTS];           /* the length the hook was given */
    UBYTE data[LOG_SLOTS][CONF_LOG_MAX];
} logs;

static void on_log(struct conf_run *run, const void *data, ULONG len)
{
    ULONG keep = len < CONF_LOG_MAX ? len : CONF_LOG_MAX;

    (void)run;
    if (logs.count >= LOG_SLOTS) {
        logs.lost++;
        return;
    }
    logs.len[logs.count] = len;
    memcpy(logs.data[logs.count], data, keep);
    logs.count++;
}

/* The record number of a stored record from its tag, or -1. */
static LONG log_kind(ULONG slot)
{
    const UBYTE *d = logs.data[slot];
    LONG k = 0;
    int i;

    if (logs.len[slot] < CONF_CON_LOG_TAG_BYTES || memcmp(d, "CONF-LOG-", 9) != 0 || d[11] != ':')
        return -1;
    for (i = 9; i < 11; i++) {
        UBYTE c = d[i];

        if (c >= '0' && c <= '9')
            k = k * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f')
            k = k * 16 + (c - 'a' + 10);
        else
            return -1;
    }
    return k;
}

/* 0: the record is a prefix of the record of its number and at most 1024 bytes. */
static int log_valid(ULONG slot, ULONG k)
{
    ULONG len = logs.len[slot];
    ULONG i;

    if (len > CONF_LOG_MAX || len > conf_con_log_len((uint32_t)k))
        return 3;
    for (i = 0; i < len; i++) {
        if (logs.data[slot][i] != conf_con_log_byte((uint32_t)k, (uint32_t)i))
            return 3;
    }
    return 0;
}

/* CONF_CON_LOGCHK */
static void h_logchk(struct conf_run *run, const AARMessage *msg)
{
    ULONG mode = conf_con_get32(msg->bytes, 0);
    ULONG k = conf_con_get32(msg->bytes, 1);
    ULONG slot;

    if (mode == 1) {
        ULONG found = 0;
        ULONG at = 0;
        UBYTE tag[CONF_CON_LOG_TAG_BYTES];

        for (slot = 0; slot < logs.count; slot++) {
            if (log_kind(slot) == (LONG)k) {
                found++;
                at = slot;
            }
        }
        if (found == 0) {
            reply(run, 1, logs.count);
            return;
        }
        if (found > 1) {
            reply(run, 2, found);
            return;
        }
        if (log_valid(at, k) != 0) {
            reply(run, 3, logs.len[at]);
            return;
        }
        conf_con_log_fill((uint32_t)k, tag, CONF_CON_LOG_TAG_BYTES);
        if (find_bytes(run->cap.buf, run->cap.len, tag, CONF_CON_LOG_TAG_BYTES)) {
            reply(run, 5, 0);
            return;
        }
        reply(run, 0, logs.len[at]);
        return;
    }
    {
        ULONG seen[CONF_CON_LOG_KINDS];
        ULONG big = 0;

        memset(seen, 0, sizeof seen);
        for (slot = 0; slot < logs.count; slot++) {
            LONG kind = log_kind(slot);

            if (kind < 0 || kind >= (LONG)CONF_CON_LOG_KINDS) {
                reply(run, 4, slot);
                return;
            }
            if (log_valid(slot, (ULONG)kind) != 0) {
                reply(run, 3, logs.len[slot]);
                return;
            }
            if (++seen[kind] > 1) {
                reply(run, 2, (ULONG)kind);
                return;
            }
        }
        for (k = 2; k < CONF_CON_LOG_KINDS; k++)
            big += seen[k];
        if (big == 0) {
            reply(run, 1, logs.count);
            return;
        }
        reply(run, 0, logs.count);
    }
}

/* ---- The console capture ---------------------------------------------------------------- */

/* CONF_CON_VERIFY: the block between the markers is the pattern; cut it out. */
static void h_verify(struct conf_run *run, const AARMessage *msg)
{
    ULONG length = conf_con_get32(msg->bytes, 0);
    ULONG seed = conf_con_get32(msg->bytes, 1);
    const ULONG bl = (ULONG)strlen(CONF_CON_BEGIN);
    const ULONG el = (ULONG)strlen(CONF_CON_END);
    const UBYTE *hit;
    ULONG start;
    ULONG data;
    ULONG i;

    hit = find_bytes(run->cap.buf, run->cap.len, (const UBYTE *)CONF_CON_BEGIN, bl);
    if (!hit) {
        reply(run, 1, 0);
        return;
    }
    start = (ULONG)(hit - run->cap.buf);
    data = start + bl;
    if (data + length + el > run->cap.len || run->cap.over) {
        reply(run, 3, run->cap.len - data);
        return;
    }
    for (i = 0; i < length; i++) {
        if (run->cap.buf[data + i] != conf_con_block_byte((uint32_t)seed, (uint32_t)i)) {
            reply(run, 2, i);
            return;
        }
    }
    if (memcmp(run->cap.buf + data + length, CONF_CON_END, el) != 0) {
        reply(run, 3, 0);
        return;
    }
    memmove(run->cap.buf + start, run->cap.buf + data + length + el,
            run->cap.len - (data + length + el));
    run->cap.len -= bl + length + el;
    run->cap.buf[run->cap.len] = '\0';
    reply(run, 0, length);
}

/* CONF_CON_FIND */
static void h_find(struct conf_run *run, const AARMessage *msg)
{
    UBYTE text[16];

    memcpy(text, msg->bytes + CONF_CON_KEYWORD_BYTES, 15);
    text[15] = '\0';
    if (find_bytes(run->cap.buf, run->cap.len, text, (ULONG)strlen((const char *)text)))
        reply(run, 0, 0);
    else
        reply(run, 1, 0);
}

/* ---- The stop (variant block) -------------------------------------------------------------- */

static struct {
    int pending;
    ULONG at;
} stopper;

/* CONF_CON_STOPAT */
static void h_stopat(struct conf_run *run, const AARMessage *msg)
{
    ULONG ms = conf_con_get32(msg->bytes, 0);

    stopper.at = conf_ticks() + (ms + 19UL) / 20UL;
    stopper.pending = 1;
    reply(run, 0, 0);
}

static void poll_stop(struct conf_run *run, ULONG now_ticks)
{
    if (stopper.pending && (LONG)(now_ticks - stopper.at) >= 0) {
        stopper.pending = 0;
        conf_run_stop(run);
    }
}

static const struct conf_peer peers[] = {
    { CONF_CON_VERIFY, h_verify },
    { CONF_CON_FIND, h_find },
    { CONF_CON_LOGCHK, h_logchk },
    { CONF_CON_STOPAT, h_stopat },
};

/* ---- Variants ---------------------------------------------------------------------------------- */

static void prepare_main(struct conf_run *run)
{
    memset(&logs, 0, sizeof logs);
    run->on_log = on_log;
}

static void prepare_slow(struct conf_run *run)
{
    run->con.write_hold_ticks = CONF_CON_SLOW_HOLD_TICKS;
}

static void prepare_input(struct conf_run *run)
{
    run->con.input = (const UBYTE *)CONF_CON_INPUT;
    run->con.input_len = (ULONG)(sizeof CONF_CON_INPUT - 1);
    run->con.input_hold_ticks = CONF_CON_INPUT_HOLD_TICKS;
}

static void prepare_block(struct conf_run *run)
{
    memset(&stopper, 0, sizeof stopper);
    run->con.input_block = 1;
    run->poll = poll_stop;
}

static const struct conf_variant variants[] = {
    { CONF_CON_VARIANT_MAIN, prepare_main },
    { CONF_CON_VARIANT_SLOWOUT, prepare_slow },
    { CONF_CON_VARIANT_INPUT, prepare_input },
    { CONF_CON_VARIANT_BLOCK, prepare_block },
};

const struct conf_area conf_area_con = {
    .name = "C-CON",
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
};
