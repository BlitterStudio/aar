/*
 * What the C-MSG payload (conf_msg.c) and the runner side (runner/r_msg.c) share:
 * the peer keywords, the layout of the messages they exchange and the helpers
 * that build and check them. The C-QUEUE, C-INPUT and C-AUDIO payloads use the
 * same helpers for their runner conversations.
 *
 * A keyword message is 32 bytes: the keyword (an ASCII string, at most 19
 * characters) and its NUL first, then three little-endian words a, b and c at
 * offsets 20, 24 and 28. A data message (the traffic a test sends and checks) is
 * built by cm_fill; its first byte is never 'C', so it cannot be taken for a
 * keyword. The runner's answer to a request for a verdict is a result message:
 * "RSLT", then little-endian ok, count, errors and first-bad-index (0xFFFFFFFF
 * when none), then two extra words.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_MSG_H
#define AAR_CONF_MSG_H

#include <stdint.h>

/* ---- Keywords (the core owns CONF-STOP-NOW) -------------------------------------- */

/* a = mode (CM_MODE_*), b = ms to stop draining after this message, c = most
 * messages drained per pass (0: all). The runner starts counting what follows. */
#define CM_K_BEGIN    "CONF-MSG-BEGIN"
/* a = how many data messages should have arrived. The runner answers with a
 * result message (ok, count, errors, first bad index). */
#define CM_K_END      "CONF-MSG-END"
/* a = mode, b = count, c = most per pass (0: all). The runner sends b data
 * messages, in direction client to app, then nothing more. */
#define CM_K_SEND     "CONF-MSG-SEND"
/* The runner sends the same 32 bytes back. */
#define CM_K_ECHO     "CONF-MSG-ECHO"
/* a = ms: the runner stops the app after that time (the app is by then parked). */
#define CM_K_STOPWAIT "CONF-MSG-STOPWAIT"
/* The runner queues one message for the app and asks it to stop at once. */
#define CM_K_STOPMSG  "CONF-MSG-STOPMSG"
/* The runner sends data messages until its send is refused and says no more.
 * (Variant inbox.) */
#define CM_K_FILL     "CONF-MSG-FILL"
/* a = messages the app polled in order, b = 1 when all were intact. (Variant
 * inbox.) */
#define CM_K_GOT      "CONF-MSG-GOT"

#define CM_MODE_SEQ   0u  /* "DATA", index, ~index, filler: checks order and loss */
#define CM_MODE_BYTES 1u  /* every byte value once over 8 messages */

#define CM_DIR_TO_CLIENT 0u
#define CM_DIR_TO_APP    1u

#define CM_NONE 0xFFFFFFFFu

static inline void cm_put32(uint8_t *m, uint32_t off, uint32_t v)
{
    m[off] = (uint8_t)v;
    m[off + 1u] = (uint8_t)(v >> 8);
    m[off + 2u] = (uint8_t)(v >> 16);
    m[off + 3u] = (uint8_t)(v >> 24);
}

static inline uint32_t cm_get32(const uint8_t *m, uint32_t off)
{
    return (uint32_t)m[off] | ((uint32_t)m[off + 1u] << 8) |
           ((uint32_t)m[off + 2u] << 16) | ((uint32_t)m[off + 3u] << 24);
}

/* The keyword message `kw` with its three words. */
static inline void cm_keyword(uint8_t *m, const char *kw, uint32_t a, uint32_t b,
                              uint32_t c)
{
    uint32_t i;

    for (i = 0; i < 32u; i++)
        m[i] = 0;
    for (i = 0; kw[i] && i < 19u; i++)
        m[i] = (uint8_t)kw[i];
    cm_put32(m, 20, a);
    cm_put32(m, 24, b);
    cm_put32(m, 28, c);
}

/* Data message number i of a mode, for a direction. */
static inline void cm_fill(uint8_t *m, uint32_t mode, uint32_t dir, uint32_t i)
{
    uint32_t j;

    if (mode == CM_MODE_BYTES) {
        for (j = 0; j < 32u; j++)
            m[j] = (uint8_t)(((i & 7u) * 32u + j) ^ (dir == CM_DIR_TO_APP ? 0xA5u : 0u));
        return;
    }
    m[0] = 'D';
    m[1] = 'A';
    m[2] = 'T';
    m[3] = 'A';
    cm_put32(m, 4, i);
    cm_put32(m, 8, ~i);
    for (j = 12; j < 32u; j++)
        m[j] = (uint8_t)(i * 7u + j + (dir == CM_DIR_TO_APP ? 1u : 0u));
}

/* 1 when m is data message number i of the mode and direction. */
static inline int cm_check(const uint8_t *m, uint32_t mode, uint32_t dir, uint32_t i)
{
    uint8_t want[32];
    uint32_t j;

    cm_fill(want, mode, dir, i);
    for (j = 0; j < 32u; j++) {
        if (m[j] != want[j])
            return 0;
    }
    return 1;
}

/* The result message the runner sends. */
static inline int cm_is_result(const uint8_t *m)
{
    return m[0] == 'R' && m[1] == 'S' && m[2] == 'L' && m[3] == 'T';
}

static inline void cm_make_result(uint8_t *m, uint32_t ok, uint32_t count,
                                  uint32_t errors, uint32_t first_bad,
                                  uint32_t x, uint32_t y)
{
    uint32_t i;

    for (i = 0; i < 32u; i++)
        m[i] = 0;
    m[0] = 'R';
    m[1] = 'S';
    m[2] = 'L';
    m[3] = 'T';
    cm_put32(m, 4, ok);
    cm_put32(m, 8, count);
    cm_put32(m, 12, errors);
    cm_put32(m, 16, first_bad);
    cm_put32(m, 20, x);
    cm_put32(m, 24, y);
}

_Static_assert(sizeof CM_K_BEGIN <= 20 && sizeof CM_K_END <= 20 && sizeof CM_K_SEND <= 20 &&
                   sizeof CM_K_ECHO <= 20 && sizeof CM_K_STOPWAIT <= 20 &&
                   sizeof CM_K_STOPMSG <= 20 && sizeof CM_K_FILL <= 20 && sizeof CM_K_GOT <= 20,
               "a message keyword is longer than 19 characters");

#endif
