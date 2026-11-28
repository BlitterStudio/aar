/*
 * Runner side of C-SHARED (conformance/TESTS.md): shared buffers, as the client
 * sees them. This file belongs to the C-SHARED runner author; the payload
 * conf_shared.c belongs to the C-SHARED payload author, and the two agree through
 * payloads/conf_shared.h (read its header comment first).
 *
 * The runner is the peer of C-SHARED.9, .10, .11, .14 and .28: the app sends a
 * token (or a request) in a message and the runner maps it with AARSharedMap,
 * reads or writes through the map, publishes or acquires, and sends ACKs.
 * The payload judges those rows from its side. While the runner handles the first
 * message (CONF-SHARED-MAP) it also judges C-SHARED.15 to C-SHARED.20 on the live
 * block, which the app keeps allocated until the answer arrives. C-SHARED.16 and
 * C-SHARED.27 need no app.
 *
 * The deliberately cacheable run in C-SHARED.12 is not tested: the flags
 * argument of AARSharedMap is reserved. C-SHARED.28 reports the map flags
 * actually received, without assuming that cache maintenance was needed.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "conf_util.h"
#include "../payloads/conf_shared.h"

#define NONE 0xFFFFFFFFUL

static int is_pow2(ULONG v)
{
    return v != 0 && (v & (v - 1UL)) == 0;
}

/* ---- Map and answer -------------------------------------------------------------------- */

static LONG map_block(AARHandle *h, ULONG haddr, ULONG bytes, struct AARSharedMap *map)
{
    memset(map, 0, sizeof *map);
    map->size = sizeof *map;
    return AARSharedMap(h, haddr, bytes, 0UL, map);
}

static void ack(struct conf_run *run, LONG w0, ULONG w1, ULONG w2, ULONG w3)
{
    AARMessage m;

    memset(&m, 0, sizeof m);
    conf_shared_build(m.bytes, CONF_SHARED_ACK, (ULONG)w0, w1, w2, w3);
    (void)AARSendMessage(run->h, &m);
}

/* ---- CONF-SHARED-MAP: C-SHARED.9, and the rules of C-SHARED.15 to C-SHARED.20 ------------- */

/* C-SHARED.15 and C-SHARED.18: what AARSharedMap refuses. */
static void judge_refusals(AARHandle *h, ULONG haddr, ULONG bytes)
{
    struct AARSharedMap m;
    char d[CONF_DETAIL_MAX];
    LONG unknown_zero = map_block(h, 0UL, 64UL, &m);
    LONG unknown_far = map_block(h, 0xFFFFFFF0UL, 64UL, &m);
    LONG huge = map_block(h, haddr, 0x80000000UL, &m);
    LONG wrap = map_block(h, haddr, 0xFFFFFFF0UL, &m);
    LONG f1;
    LONG f2;
    LONG f3;

    if (unknown_zero == AAR_E_INVAL && unknown_far == AAR_E_INVAL && huge == AAR_E_INVAL &&
        wrap == AAR_E_INVAL) {
        conf_record("C-SHARED.15", 1, "unknown tokens and ranges outside the aperture: AAR_E_INVAL");
    } else {
        conf_detailf(d, "token 0: %ld, token 0xFFFFFFF0: %ld, 2 GiB: %ld, wrap: %ld "
                        "(AAR_E_INVAL is due) [spec question 43]", (long)unknown_zero,
                     (long)unknown_far, (long)huge, (long)wrap);
        conf_record("C-SHARED.15", 0, d);
    }
    memset(&m, 0, sizeof m);
    m.size = sizeof m;
    f1 = AARSharedMap(h, haddr, bytes, AAR_MAPF_UNCACHED, &m);
    f2 = AARSharedMap(h, haddr, bytes, AAR_MAPF_COHERENT, &m);
    f3 = AARSharedMap(h, haddr, bytes, 0x80000000UL, &m);
    if (f1 == AAR_E_INVAL && f2 == AAR_E_INVAL && f3 == AAR_E_INVAL) {
        conf_record("C-SHARED.18", 1, "a non-zero flags argument: AAR_E_INVAL");
    } else {
        conf_detailf(d, "flags 1: %ld, flags 2: %ld, flags 0x80000000: %ld (AAR_E_INVAL is due)",
                     (long)f1, (long)f2, (long)f3);
        conf_record("C-SHARED.18", 0, d);
    }
}

/* C-SHARED.17: the report of the map. */
static void judge_map(const struct AARSharedMap *map, ULONG haddr, ULONG bytes)
{
    char d[CONF_DETAIL_MAX];
    int ok = (map->flags & ~(ULONG)(AAR_MAPF_UNCACHED | AAR_MAPF_COHERENT)) == 0 &&
             is_pow2(map->granule) && map->base != NULL && map->bytes == bytes &&
             map->haddr == haddr;

    conf_detailf(d, "flags 0x%lx, granule %lu, base %s, bytes %lu, token 0x%lx",
                 (unsigned long)map->flags, (unsigned long)map->granule,
                 map->base ? "set" : "NULL", (unsigned long)map->bytes,
                 (unsigned long)map->haddr);
    conf_record("C-SHARED.17", ok, d);
}

/* C-SHARED.20: publish and acquire over valid ranges, rounding to the granule, and
 * no change to the data on an uncached or coherent map. */
static void judge_publish(AARHandle *h, struct AARSharedMap *map, ULONG bytes)
{
    UBYTE before[64];
    volatile UBYTE *p = (volatile UBYTE *)map->base;
    char d[CONF_DETAIL_MAX];
    LONG r[6];
    ULONG n = bytes < sizeof before ? bytes : (ULONG)sizeof before;
    ULONG i;
    int same = 1;

    for (i = 0; i < n; i++)
        before[i] = p[i];
    r[0] = AARSharedPublish(h, map, 1UL, 1UL);
    r[1] = AARSharedAcquire(h, map, 1UL, 1UL);
    r[2] = AARSharedPublish(h, map, bytes - 1UL, 1UL);
    r[3] = AARSharedAcquire(h, map, bytes - 1UL, 1UL);
    r[4] = AARSharedPublish(h, map, 0UL, bytes);
    r[5] = AARSharedAcquire(h, map, 0UL, bytes);
    for (i = 0; i < n; i++)
        same &= before[i] == p[i];
    for (i = 0; i < 6; i++) {
        if (r[i] != AAR_OK) {
            conf_detailf(d, "call %lu on a valid range returned %ld", (unsigned long)i,
                         (long)r[i]);
            conf_record("C-SHARED.20", 0, d);
            return;
        }
    }
    if (!same) {
        conf_record("C-SHARED.20", 0, "publish and acquire changed the data of the map");
        return;
    }
    conf_record("C-SHARED.20", 1, "publish and acquire of odd ranges: AAR_OK, data unchanged");
}

static void peer_map(struct conf_run *run, const AARMessage *msg)
{
    ULONG haddr = conf_shared_get32(msg->bytes, 0);
    ULONG bytes = conf_shared_get32(msg->bytes, 1);
    ULONG seed = conf_shared_get32(msg->bytes, 2);
    struct AARSharedMap map;
    char d[CONF_DETAIL_MAX];
    LONG rc = map_block(run->h, haddr, bytes, &map);
    ULONG bad;
    LONG un;

    if (rc != AAR_OK) {
        conf_detailf(d, "AARSharedMap of the app's block returned %ld", (long)rc);
        conf_record("C-SHARED.17", 0, d);
        conf_record("C-SHARED.19", 0, d);
        conf_record("C-SHARED.20", 0, d);
        judge_refusals(run->h, haddr, bytes);
        ack(run, rc, 0, 0, 0);
        return;
    }
    rc = AARSharedAcquire(run->h, &map, 0UL, bytes);
    bad = rc == AAR_OK ? conf_shared_check((const volatile uint8_t *)map.base, 0, bytes, seed)
                       : 0UL;
    judge_map(&map, haddr, bytes);
    judge_refusals(run->h, haddr, bytes);
    judge_publish(run->h, &map, bytes);
    /* The map is valid until it is unmapped: the bytes must still be there after the
     * calls above (a mismatch is reported at offset 0 when the first read was clean). */
    if (conf_shared_check((const volatile uint8_t *)map.base, 0, bytes, seed) != NONE &&
        bad == NONE)
        bad = 0UL;
    un = AARSharedUnmap(run->h, &map);
    if (un == AAR_OK && rc == AAR_OK) {
        conf_record("C-SHARED.19", 1, "AARSharedUnmap of a live map: AAR_OK");
    } else {
        conf_detailf(d, "acquire returned %ld, AARSharedUnmap returned %ld", (long)rc, (long)un);
        conf_record("C-SHARED.19", 0, d);
    }
    ack(run, rc != AAR_OK ? rc : (bad == NONE ? 0 : CONF_SHARED_BAD), bad, 0, 0);
}

/* ---- CONF-SHARED-FILL: C-SHARED.10 ----------------------------------------------------------- */

static void peer_fill(struct conf_run *run, const AARMessage *msg)
{
    ULONG haddr = conf_shared_get32(msg->bytes, 0);
    ULONG bytes = conf_shared_get32(msg->bytes, 1);
    ULONG seed = conf_shared_get32(msg->bytes, 2);
    struct AARSharedMap map;
    LONG rc = map_block(run->h, haddr, bytes, &map);

    if (rc != AAR_OK) {
        ack(run, rc, 0, 0, 0);
        return;
    }
    conf_shared_fill((volatile uint8_t *)map.base, 0, bytes, seed);
    rc = AARSharedPublish(run->h, &map, 0UL, bytes);
    (void)AARSharedUnmap(run->h, &map);
    ack(run, rc, 0, 0, 0);
}

/* ---- CONF-SHARED-SEQ and -END: C-SHARED.11 ----------------------------------------------------- */

/* The reader of the sequence test. It lives across passes of the serving loop: the
 * poll callback takes one sample per pass. */
static struct {
    int active;
    AARHandle *h;
    struct AARSharedMap map;
    ULONG bytes;
    ULONG max_head;
    ULONG samples, stale, torn;
} seq;

static void peer_seq_begin(struct conf_run *run, const AARMessage *msg)
{
    ULONG haddr = conf_shared_get32(msg->bytes, 0);
    ULONG bytes = conf_shared_get32(msg->bytes, 1);
    LONG rc;

    memset(&seq, 0, sizeof seq);
    rc = map_block(run->h, haddr, bytes, &seq.map);
    if (rc != AAR_OK || bytes < CONF_SHARED_SEQ_BODY + 16UL) {
        ack(run, rc != AAR_OK ? rc : AAR_E_INVAL, 0, 0, 0);
        return;
    }
    seq.h = run->h;
    seq.bytes = bytes;
    seq.active = 1;
}

/* One sample: a seqlock read with an acquire before each pass over the block. */
static void seq_sample(void)
{
    const volatile uint8_t *p = (const volatile uint8_t *)seq.map.base;
    ULONG h1;
    ULONG h2;
    ULONG bad;

    (void)AARSharedAcquire(seq.h, &seq.map, 0UL, seq.bytes);
    h1 = conf_shared_get_be32(p);
    if (h1 == 0UL || (h1 & 1UL))
        return;
    bad = conf_shared_check(p, CONF_SHARED_SEQ_BODY, seq.bytes, h1 / 2UL);
    (void)AARSharedAcquire(seq.h, &seq.map, 0UL, CONF_SHARED_SEQ_BODY);
    h2 = conf_shared_get_be32(p);
    if (h2 != h1)
        return; /* the writer moved on: not a sample */
    if (h1 < seq.max_head)
        seq.stale++;
    else
        seq.max_head = h1;
    if (bad != NONE)
        seq.torn++;
    else
        seq.samples++;
}

static void poll_seq(struct conf_run *run, ULONG now)
{
    (void)run;
    (void)now;
    if (seq.active)
        seq_sample();
}

static void peer_seq_end(struct conf_run *run, const AARMessage *msg)
{
    ULONG last = conf_shared_get32(msg->bytes, 0);
    const volatile uint8_t *p;
    ULONG head;
    ULONG bad;
    int ok;

    if (!seq.active) {
        ack(run, AAR_E_INVAL, 0, 0, 0);
        return;
    }
    seq.active = 0;
    p = (const volatile uint8_t *)seq.map.base;
    (void)AARSharedAcquire(seq.h, &seq.map, 0UL, seq.bytes);
    head = conf_shared_get_be32(p);
    bad = conf_shared_check(p, CONF_SHARED_SEQ_BODY, seq.bytes, last / 2UL);
    if (head != last)
        seq.stale++; /* the last head the app published is not there */
    else if (bad != NONE)
        seq.torn++;
    ok = seq.stale == 0UL && seq.torn == 0UL && seq.samples >= CONF_SHARED_SEQ_MIN_SAMPLES;
    (void)AARSharedUnmap(seq.h, &seq.map);
    ack(run, ok ? 0 : CONF_SHARED_BAD, seq.stale, seq.torn, seq.samples);
}

/* ---- CONF-SHARED-PROG: C-SHARED.28 ----------------------------------------------------------- */

static struct {
    int active;
    struct AARSharedMap map;
    ULONG last;    /* all records through this count have been checked */
    ULONG next;    /* producer waits at this checkpoint (initially zero) */
} progress;

static void peer_progress(struct conf_run *run, const AARMessage *msg)
{
    ULONG haddr = conf_shared_get32(msg->bytes, 0);
    ULONG bytes = conf_shared_get32(msg->bytes, 1);
    ULONG g = conf_shared_get32(msg->bytes, 2);
    LONG rc;

    if (!is_pow2(g) || g < 64UL || g > NONE / (1UL + CONF_SHARED_PROGRESS_COUNT) ||
        bytes != (1UL + CONF_SHARED_PROGRESS_COUNT) * g) {
        ack(run, AAR_E_INVAL, 0, NONE, 0);
        return;
    }
    rc = map_block(run->h, haddr, bytes, &progress.map);
    if (rc != AAR_OK) {
        ack(run, rc, 0, NONE, 0);
        return;
    }
    if (!progress.map.base || (uintptr_t)progress.map.base % sizeof(uint32_t) != 0u ||
        progress.map.granule != g || progress.map.bytes != bytes) {
        (void)AARSharedUnmap(run->h, &progress.map);
        ack(run, AAR_E_INVAL, 0, NONE, 0);
        return;
    }
    progress.active = 1;
}

static void poll_progress(struct conf_run *run)
{
    const volatile uint8_t *p = (const volatile uint8_t *)progress.map.base;
    ULONG g = progress.map.granule;
    ULONG flags = progress.map.flags;
    ULONG count = progress.last;
    ULONG bad = NONE;
    LONG rc;

    rc = AARSharedAcquire(run->h, &progress.map, 0UL, sizeof(uint32_t));
    if (rc != AAR_OK)
        goto end;
    count = conf_shared_read_progress(p);
    if (count < progress.last || count > progress.next) {
        rc = CONF_SHARED_BAD;
        goto end;
    }
    if (count > progress.last) {
        ULONG n;

        rc = AARSharedAcquire(run->h, &progress.map, (progress.last + 1UL) * g,
                              (count - progress.last) * g);
        if (rc != AAR_OK)
            goto end;
        for (n = progress.last + 1UL; n <= count; n++) {
            bad = conf_shared_check(p, n * g, (n + 1UL) * g, n);
            if (bad != NONE) {
                rc = CONF_SHARED_BAD;
                goto end;
            }
        }
        progress.last = count;
    }
    if (count != progress.next)
        return;
    if (count != CONF_SHARED_PROGRESS_COUNT) {
        progress.next += CONF_SHARED_PROGRESS_BATCH;
        ack(run, AAR_OK, count, flags, 0);
        return;
    }

end:
    progress.active = 0;
    {
        LONG unmap_rc = AARSharedUnmap(run->h, &progress.map);

        if (rc == AAR_OK)
            rc = unmap_rc;
    }
    ack(run, rc, count, rc == AAR_OK ? flags : bad, 0);
}

static void poll_shared(struct conf_run *run, ULONG now)
{
    poll_seq(run, now);
    if (progress.active)
        poll_progress(run);
}

/* ---- CONF-SHARED-GRAN: C-SHARED.14 ------------------------------------------------------------- */

static void peer_gran(struct conf_run *run, const AARMessage *msg)
{
    ULONG haddr = conf_shared_get32(msg->bytes, 0);
    ULONG bytes = conf_shared_get32(msg->bytes, 1);
    ULONG g = conf_shared_get32(msg->bytes, 2);
    ULONG r = conf_shared_get32(msg->bytes, 3);
    struct AARSharedMap map;
    volatile uint8_t *p;
    ULONG k;
    ULONG bad = NONE;
    LONG rc;

    if (g == 0UL || bytes != CONF_SHARED_GRAN_COUNT * g) {
        ack(run, AAR_E_INVAL, 0, 0, 0);
        return;
    }
    rc = map_block(run->h, haddr, bytes, &map);
    if (rc != AAR_OK) {
        ack(run, rc, 0, 0, 0);
        return;
    }
    p = (volatile uint8_t *)map.base;
    rc = AARSharedAcquire(run->h, &map, 0UL, bytes);
    for (k = 0; rc == AAR_OK && k < CONF_SHARED_GRAN_COUNT && bad == NONE; k++) {
        /* The app's granules hold its seed of this round, ours the seed we wrote last
         * (seed 1 before the first round). */
        ULONG seed = (k & 1UL) ? 2UL * r - 1UL : 2UL * r;

        bad = conf_shared_check(p, k * g, (k + 1UL) * g, seed);
    }
    if (rc == AAR_OK && bad == NONE) {
        for (k = 1; k < CONF_SHARED_GRAN_COUNT; k += 2UL) {
            conf_shared_fill(p, k * g, (k + 1UL) * g, 2UL * r + 1UL);
            if (rc == AAR_OK)
                rc = AARSharedPublish(run->h, &map, k * g, g);
        }
    }
    (void)AARSharedUnmap(run->h, &map);
    ack(run, rc != AAR_OK ? rc : (bad == NONE ? 0 : CONF_SHARED_BAD), bad, 0, 0);
}

static const struct conf_peer peers[] = {
    { CONF_SHARED_MAP, peer_map },
    { CONF_SHARED_FILL, peer_fill },
    { CONF_SHARED_BEGIN_SEQ, peer_seq_begin },
    { CONF_SHARED_END_SEQ, peer_seq_end },
    { CONF_SHARED_GRAN, peer_gran },
    { CONF_SHARED_PROGRESS, peer_progress },
};

/* ---- The variants of the payload --------------------------------------------------------------- */

static void prepare_default(struct conf_run *run)
{
    memset(&seq, 0, sizeof seq);
    memset(&progress, 0, sizeof progress);
    run->poll = poll_shared;
}

static const struct TagItem caps_tags[] = {
    { AAR_TAG_SHARED_MIB, CONF_SHARED_CAPS_MIB },
    { TAG_DONE, 0 },
};

static void prepare_caps(struct conf_run *run)
{
    prepare_default(run);
    run->tags = caps_tags;
}

static const struct conf_variant variants[] = {
    { CONF_SHARED_VARIANT_DEFAULT, prepare_default },
    { CONF_SHARED_VARIANT_CAPS, prepare_caps },
};

/* ---- C-SHARED.16 and C-SHARED.27 ------------------------------------------------------------- */

/* C-SHARED.16 (SHOULD): a map for a claim that is gone gives AAR_E_STALE. */
static void t_map_stale(int idx)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    struct AARSharedMap map;
    char d[CONF_DETAIL_MAX];
    LONG rc = AAROpen(&h, NULL, &reason);

    if (rc != AAR_OK) {
        conf_detailf(d, "AAROpen returned %ld", (long)rc);
        conf_set_result(idx, CONF_R_FAIL, d);
        return;
    }
    (void)AARClose(h);
    rc = map_block(h, 0x1000UL, 64UL, &map);
    if (rc == AAR_E_STALE) {
        conf_set_result(idx, CONF_R_PASS, "AARSharedMap with a closed handle: AAR_E_STALE");
    } else if (rc == AAR_E_INVAL || rc == AAR_E_NOSYS) {
        conf_detailf(d, "the platform cannot tell: AARSharedMap with a closed handle returned %ld",
                     (long)rc);
        conf_set_result(idx, CONF_R_SKIP, d);
    } else {
        conf_detailf(d, "AARSharedMap with a closed handle returned %ld", (long)rc);
        conf_set_result(idx, CONF_R_FAIL, d);
    }
}

/* C-SHARED.27: a limit the platform cannot grant when the load starts fails the load (spec 4.2). */
static void t_limit_too_big(int idx)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    ULONG max = conf_g.info.shared_max_mib;
    char path[256];
    char d[CONF_DETAIL_MAX];
    struct TagItem tags[2];
    LONG rc;
    LONG tag_rc;

    if (max >= 4095UL) {
        conf_detailf(d, "shared_max_mib is %lu: every limit up to 4095 MiB can be granted",
                     (unsigned long)max);
        conf_set_result(idx, CONF_R_PASS, d);
        return;
    }
    if (!conf_util_image_path(path, sizeof path, conf_g.profile_name, "ok-stack0")) {
        conf_set_result(idx, CONF_R_FAIL, "image path too long");
        return;
    }
    rc = AAROpen(&h, NULL, &reason);
    if (rc != AAR_OK) {
        conf_detailf(d, "AAROpen returned %ld", (long)rc);
        conf_set_result(idx, CONF_R_FAIL, d);
        return;
    }
    tags[0].ti_Tag = AAR_TAG_SHARED_MIB;
    tags[0].ti_Data = max + 1UL;
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0;
    tag_rc = AARSetTagList(h, tags);
    if (tag_rc == AAR_E_INVAL || tag_rc == AAR_E_NOSYS) {
        conf_detailf(d, "AARSetTagList refused %lu MiB (shared_max_mib %lu): %ld",
                     (unsigned long)(max + 1UL), (unsigned long)max, (long)tag_rc);
        conf_set_result(idx, CONF_R_PASS, d);
    } else if (tag_rc != AAR_OK) {
        conf_detailf(d, "AARSetTagList returned %ld", (long)tag_rc);
        conf_set_result(idx, CONF_R_FAIL, d);
    } else {
        rc = AARLoadFile(h, path);
        if (rc == AAR_E_NOMEM || rc == AAR_E_NORESOURCE) {
            conf_detailf(d, "the load refused %lu MiB (shared_max_mib %lu) with %ld",
                         (unsigned long)(max + 1UL), (unsigned long)max, (long)rc);
            conf_set_result(idx, CONF_R_PASS, d);
        } else {
            conf_detailf(d, "a shared limit of %lu MiB, above shared_max_mib %lu: the load "
                            "returned %ld, AAR_E_NOMEM or AAR_E_NORESOURCE is due (spec 4.2)",
                         (unsigned long)(max + 1UL), (unsigned long)max, (long)rc);
            conf_set_result(idx, CONF_R_FAIL, d);
        }
    }
    (void)AARClose(h);
}

static const struct conf_r_test tests[] = {
    { "C-SHARED.16", t_map_stale },
    { "C-SHARED.27", t_limit_too_big },
};

/* Judged by peer_map while the app's CONF-SHARED-MAP message is served (variant default,
 * which also carries payload ids). */
static const struct conf_needs needs[] = {
    { "C-SHARED.15", CONF_SHARED_VARIANT_DEFAULT, 0 },
    { "C-SHARED.17", CONF_SHARED_VARIANT_DEFAULT, 0 },
    { "C-SHARED.18", CONF_SHARED_VARIANT_DEFAULT, 0 },
    { "C-SHARED.19", CONF_SHARED_VARIANT_DEFAULT, 0 },
    { "C-SHARED.20", CONF_SHARED_VARIANT_DEFAULT, 0 },
};

const struct conf_area conf_area_shared = {
    .name = "C-SHARED",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
    .needs = needs,
    .need_count = CONF_COUNT(needs),
};
