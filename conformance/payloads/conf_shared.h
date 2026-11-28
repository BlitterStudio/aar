/*
 * The peer protocol of C-SHARED between the payload conf_shared.c (the app) and
 * the runner (conformance/runner/r_shared.c, the client). Both include this file.
 * It holds the keywords, the message layout, the shared pattern and the
 * helpers that build and wait for messages, so that the two sides cannot
 * disagree.
 *
 * Every message is 32 bytes. Bytes 0 to 15 are the keyword, padded with zero
 * bytes; the runner dispatches by keyword (the CONF-SHARED- prefix is the
 * runner's). Bytes 16 to 31 are four 32-bit words, little-endian, w0 to w3. A
 * message the app sends to the runner is a request. The runner answers each
 * with one CONF_SHARED_ACK, except BEGIN_SEQ (answered after END_SEQ) and
 * PROGRESS (answered after observing each checkpoint, as described below).
 *
 * The app runs the tests in one run of the payload (variant "default"), apart
 * from C-SHARED.8 that is judged in the variant "caps" (the runner sets
 * AAR_TAG_SHARED_MIB to CONF_SHARED_CAPS_MIB MiB before the load). The runner
 * passes conf:variant=<name> in every run.
 *
 * Requests (app to runner):
 *
 *   CONF_SHARED_MAP   w0 haddr, w1 bytes, w2 seed        C-SHARED.9
 *     The app has written conf_shared_fill(block, 0, bytes, seed), called
 *     aar_publish on it and sent this. The runner maps (haddr, bytes) with
 *     AARSharedMap, acquires, and checks every byte; ACK w0 is 0 or an error
 *     (below), w1 the first bad offset. While it handles this message the runner
 *     also judges C-SHARED.15 to C-SHARED.20 on the live block, so the app MUST
 *     keep the block allocated until the ACK has arrived, and MUST NOT write
 *     to it meanwhile. bytes is at least 2 and at most 64 granules.
 *
 *   CONF_SHARED_FILL  w0 haddr, w1 bytes, w2 seed        C-SHARED.10
 *     The runner maps the block, writes conf_shared_fill(map, 0, bytes, seed)
 *     through the map, publishes it and sends the ACK (w0 0 or an error). The
 *     app has to stay away from the block until then; after the ACK it
 *     acquires and checks the pattern. Use a seed that differs from the one the
 *     block held before.
 *
 *   CONF_SHARED_BEGIN_SEQ  w0 haddr, w1 bytes            C-SHARED.11
 *   CONF_SHARED_END_SEQ    w0 last head value            (see the layout below)
 *     The sequence test. The block holds, from offset 0, a big-endian 32-bit
 *     head word, 4 bytes of padding, and the body from CONF_SHARED_SEQ_BODY to
 *     bytes. The app sends BEGIN_SEQ, then runs rounds n = 1, 2, ... :
 *         put_be32(block, 2n - 1);  aar_publish(block, granule);   (odd: writing)
 *         conf_shared_fill(block, CONF_SHARED_SEQ_BODY, bytes, n);
 *         aar_publish(block, bytes);
 *         put_be32(block, 2n);      aar_publish(block, granule);   (even: complete)
 *     (the first head is written before BEGIN_SEQ is sent: put_be32(block, 0)
 *     and a publish; head 0 means nothing complete yet). After the last round it
 *     sends END_SEQ with w0 = 2 * rounds. Between rounds the app sleeps about
 *     2 ms (aar_sleep_us(2000)); CONF_SHARED_SEQ_ROUNDS rounds. While the app
 *     runs, the runner reads the block on every pass of its serving loop with
 *     AARSharedAcquire before each pass, as a seqlock reader: it takes the head,
 *     reads the body, takes the head again, and counts a sample only when both
 *     heads are the same even non-zero value; the body must then be
 *     conf_shared_check(body, seed = head / 2). A mismatch is a torn read, a
 *     head that goes back is a stale read. On END_SEQ it acquires once more and
 *     the head has to be w0 (a stale read otherwise). ACK: w0 0 or CONF_SHARED_BAD,
 *     w1 stale reads, w2 torn reads, w3 samples (the runner requires at least 5).
 *     The block must be live until the ACK, and bytes at least 2 granules.
 *
 *   CONF_SHARED_GRAN  w0 haddr, w1 bytes, w2 granule, w3 round   C-SHARED.14
 *     Granule ownership. The block is 8 granules of `granule` bytes
 *     (bytes = 8 * granule). Even granules (0, 2, 4, 6) belong to the app, odd
 *     granules to the client. Before round 1 the app fills the whole block:
 *     even granules with conf_shared_fill(.., seed 0), odd granules with seed 1,
 *     and publishes all of it. Round r (r = 1 to CONF_SHARED_GRAN_ROUNDS):
 *         the app fills each even granule with seed 2r, publishes the even
 *         granules (each on its own), and sends GRAN with w3 = r;
 *         the runner maps, acquires, checks that the even granules hold seed 2r
 *         and the odd granules the seed it wrote last (2r - 1; 1 in round 1),
 *         fills each odd granule with seed 2r + 1, publishes each on its own,
 *         and sends the ACK (w0 0, CONF_SHARED_BAD with w1 the first bad
 *         offset, or an error);
 *         the app, after the ACK, acquires the block and checks that the even
 *         granules still hold seed 2r and the odd ones seed 2r + 1.
 *     Neither side may write the granules of the other.
 *
 *   CONF_SHARED_PROGRESS  w0 haddr, w1 bytes, w2 granule          C-SHARED.28
 *     The first granule contains only an aligned, big-endian 32-bit count.
 *     Each later granule is one immutable payload record; slot n starts at
 *     n * granule and holds the pattern for seed n. The app publishes count
 *     zero, sends PROGRESS once to register the map, and waits for ACK(0).
 *     For each slot it writes and publishes the payload, does one release
 *     store of the new count, and publishes that word. It waits for an ACK
 *     after every batch of slots before starting the next batch.
 *     The client polls the word (acquire its range, load it with acquire
 *     ordering), then acquires and checks only payload newly announced by
 *     that word. Intermediate counts may be skipped, but the count may not
 *     go backwards or pass the current checkpoint. No app message announces
 *     an advance. ACK w1 is the count read, w2 is the map flags on success or
 *     the first bad byte (UINT32_MAX if none) on failure. The client unmaps
 *     before the final or failed ACK; the app keeps the block live until then.
 *
 *   CONF_SHARED_RELEASE_OPEN  w0 haddr, w1 bytes, w2 serial    C-SHARED.29
 *     The app fills a new block with seed 2 * serial and publishes it. The
 *     runner maps it twice, acquires and checks each view, then writes and
 *     publishes seed 2 * serial + 1 through the second view. ACK w2 is the
 *     serial and w3 is CONF_SHARED_RELEASE_READY; on success both views stay mapped.
 *   CONF_SHARED_RELEASE_DROP  w0 serial
 *     After acquiring and checking the reply, the app requests retirement.
 *     The runner retires this serial, has no more users of either view, and
 *     unmaps both. Only then does it send ACK w2 = serial, w3 =
 *     CONF_SHARED_RELEASE_DONE, and the app frees the block. A replacement
 *     uses the next serial even if its token is identical. If the exchange
 *     fails before retirement is acknowledged, the app leaves the block
 *     allocated and waits for a stop before exiting.
 *
 * The answer (runner to app):
 *
 *   CONF_SHARED_ACK  w0 result, w1, w2, w3 as the request says
 *     w0: 0 passed; a negative AAR_E_* the runner's library call returned
 *     (AARSharedMap for instance); CONF_SHARED_BAD (1) the data was wrong.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_SHARED_H
#define AAR_CONF_SHARED_H

#include <stdint.h>

#include "aar/transport.h"

/* ---- Keywords (16 bytes at most, no one is the prefix of another) -------------- */

#define CONF_SHARED_MAP       "CONF-SHARED-MAP"
#define CONF_SHARED_FILL      "CONF-SHARED-FILL"
#define CONF_SHARED_BEGIN_SEQ "CONF-SHARED-SEQ"
#define CONF_SHARED_END_SEQ   "CONF-SHARED-END"
#define CONF_SHARED_GRAN      "CONF-SHARED-GRAN"
#define CONF_SHARED_ACK       "CONF-SHARED-ACK"
#define CONF_SHARED_PROGRESS  "CONF-SHARED-PROG"
#define CONF_SHARED_RELEASE_OPEN "CONF-SHARED-OPEN"
#define CONF_SHARED_RELEASE_DROP "CONF-SHARED-DROP"

#define CONF_SHARED_KEYWORD_BYTES 16u
#define CONF_SHARED_MSG_BYTES     32u

/* ---- Variants and constants -------------------------------------------------------- */

#define CONF_SHARED_VARIANT_DEFAULT "default"
#define CONF_SHARED_VARIANT_CAPS    "caps"
#define CONF_SHARED_CAPS_MIB        8u   /* AAR_TAG_SHARED_MIB of the variant caps */

#define CONF_SHARED_BAD            1     /* ACK w0: the data was wrong */
#define CONF_SHARED_SEQ_BODY       8u    /* start of the body of the sequence block */
#define CONF_SHARED_SEQ_ROUNDS     300u
#define CONF_SHARED_SEQ_MIN_SAMPLES 5u
#define CONF_SHARED_GRAN_ROUNDS    16u
#define CONF_SHARED_GRAN_COUNT     8u    /* granules of the ownership block */
#define CONF_SHARED_PROGRESS_BATCH   3u
#define CONF_SHARED_PROGRESS_BATCHES 4u
#define CONF_SHARED_PROGRESS_COUNT   (CONF_SHARED_PROGRESS_BATCH * CONF_SHARED_PROGRESS_BATCHES)
#define CONF_SHARED_RELEASE_ROUNDS 2u
#define CONF_SHARED_RELEASE_READY  1u
#define CONF_SHARED_RELEASE_DONE   2u
#define CONF_SHARED_ACK_WAIT_US    10000000u

/* ---- Words and the pattern ------------------------------------------------------------ */

static inline uint32_t conf_shared_get32(const uint8_t *m, uint32_t word)
{
    const uint8_t *p = m + CONF_SHARED_KEYWORD_BYTES + 4u * word;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Builds a message: the keyword, zero padding, four words. */
static inline void conf_shared_build(uint8_t *m, const char *keyword, uint32_t w0,
                                     uint32_t w1, uint32_t w2, uint32_t w3)
{
    uint32_t w[4];
    uint32_t i;
    uint32_t k;

    w[0] = w0;
    w[1] = w1;
    w[2] = w2;
    w[3] = w3;
    for (i = 0; i < CONF_SHARED_MSG_BYTES; i++)
        m[i] = 0;
    for (i = 0; keyword[i] && i < CONF_SHARED_KEYWORD_BYTES; i++)
        m[i] = (uint8_t)keyword[i];
    for (k = 0; k < 4u; k++) {
        m[CONF_SHARED_KEYWORD_BYTES + 4u * k + 0] = (uint8_t)w[k];
        m[CONF_SHARED_KEYWORD_BYTES + 4u * k + 1] = (uint8_t)(w[k] >> 8);
        m[CONF_SHARED_KEYWORD_BYTES + 4u * k + 2] = (uint8_t)(w[k] >> 16);
        m[CONF_SHARED_KEYWORD_BYTES + 4u * k + 3] = (uint8_t)(w[k] >> 24);
    }
}

/* The message starts with the keyword. */
static inline int conf_shared_is(const uint8_t *m, const char *keyword)
{
    uint32_t i;

    for (i = 0; keyword[i]; i++) {
        if (m[i] != (uint8_t)keyword[i])
            return 0;
    }
    return 1;
}

/* Byte i of the pattern of a seed. Two seeds that differ by less than 256 differ
 * at every byte. */
static inline uint8_t conf_shared_pattern(uint32_t seed, uint32_t i)
{
    return (uint8_t)((i * 167u) ^ (i >> 8) ^ (seed * 73u + 11u));
}

/* Writes the pattern of seed over [from, to) of the block. */
static inline void conf_shared_fill(volatile uint8_t *p, uint32_t from, uint32_t to,
                                    uint32_t seed)
{
    uint32_t i;

    for (i = from; i < to; i++)
        p[i] = conf_shared_pattern(seed, i);
}

/* The offset of the first byte of [from, to) that is not the pattern of seed, or
 * 0xFFFFFFFF when all are. */
static inline uint32_t conf_shared_check(const volatile uint8_t *p, uint32_t from,
                                         uint32_t to, uint32_t seed)
{
    uint32_t i;

    for (i = from; i < to; i++) {
        if (p[i] != conf_shared_pattern(seed, i))
            return i;
    }
    return 0xFFFFFFFFu;
}

static inline uint32_t conf_shared_get_be32(const volatile uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static inline void conf_shared_put_be32(volatile uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* One aligned load, not the bytewise sequence-test accessor above. */
static inline uint32_t conf_shared_read_progress(const volatile void *p)
{
#if defined(__m68k__)
    uint32_t count = aar_get_be32(p);

    /* The 68k does not reorder reads; prevent compiler motion across the load. */
    __asm__ __volatile__("" ::: "memory");
    return count;
#else
    uint32_t count = __atomic_load_n((const volatile uint32_t *)p, __ATOMIC_ACQUIRE);

    return AAR_NATIVE_BIG_ENDIAN ? count : aar_bswap32(count);
#endif
}

/* ---- The app's side: only when the payload protocol (conf.h) is included first ------- */

#ifdef AAR_CONF_H

/* The caller has already published the newly completed slot, if any. */
static inline int conf_shared_publish_progress(void *p, uint32_t count)
{
    __atomic_store_n((volatile uint32_t *)p,
                     AAR_NATIVE_BIG_ENDIAN ? count : aar_bswap32(count), __ATOMIC_RELEASE);
    return aar_publish(p, sizeof count);
}

/* Sends a request to the runner. A full queue is retried for up to 100 ms.
 * Returns the message call's result. */
static inline int conf_shared_send(const char *keyword, uint32_t w0, uint32_t w1,
                                   uint32_t w2, uint32_t w3)
{
    uint8_t m[CONF_SHARED_MSG_BYTES];
    uint64_t start = aar_time_us();
    int rc;

    conf_shared_build(m, keyword, w0, w1, w2, w3);
    do {
        rc = aar_msg_send(m);
        if (rc != AAR_E_BUSY)
            break;
        aar_sleep_us(1000);
    } while (aar_time_us() - start < 100000u);
    return rc;
}

/* Waits for the ACK of the runner: AAR_OK with out[0..3] = w0..w3, AAR_E_TIMEDOUT
 * after CONF_SHARED_ACK_WAIT_US, AAR_E_IO when a stop request ended the wait, or
 * the negative code of a failing call. Other messages are ignored. */
static inline int conf_shared_wait_ack(uint32_t out[4])
{
    uint64_t start = aar_time_us();

    for (;;) {
        uint8_t m[CONF_SHARED_MSG_BYTES];
        uint64_t used = aar_time_us() - start;
        int rc;

        if (used >= CONF_SHARED_ACK_WAIT_US)
            return AAR_E_TIMEDOUT;
        rc = aar_msg_wait(100000u);
        if (rc != AAR_OK && rc != AAR_E_TIMEDOUT)
            return rc;
        while ((rc = aar_msg_poll(m)) == 1) {
            if (conf_shared_is(m, CONF_SHARED_ACK)) {
                out[0] = conf_shared_get32(m, 0);
                out[1] = conf_shared_get32(m, 1);
                out[2] = conf_shared_get32(m, 2);
                out[3] = conf_shared_get32(m, 3);
                return AAR_OK;
            }
        }
        if (rc < 0)
            return rc;
    }
}

#endif /* AAR_CONF_H */

#endif /* AAR_CONF_SHARED_H */
