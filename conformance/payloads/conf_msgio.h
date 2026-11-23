/*
 * Payload-side helpers for talking to the runner in messages (conf_msg.h has the
 * layouts): sending with a retry on a full queue, waiting for one message, and
 * waiting for the runner's result message. Used by the C-MSG, C-QUEUE, C-INPUT and
 * C-AUDIO payloads.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_MSGIO_H
#define AAR_CONF_MSGIO_H

#include "aar/app.h"

#include "conf.h"
#include "conf_msg.h"

static inline int cmio_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Sends m, retrying while the queue is full, for up to timeout_us. Returns the
 * last result of aar_msg_send. */
static inline int cmio_send(const uint8_t *m, uint32_t timeout_us)
{
    uint64_t start = aar_time_us();
    int rc;

    do {
        rc = aar_msg_send(m);
        if (rc != AAR_E_BUSY)
            break;
        aar_sleep_us(500);
    } while (aar_time_us() - start < timeout_us);
    return rc;
}

static inline int cmio_kw(const char *kw, uint32_t a, uint32_t b, uint32_t c)
{
    uint8_t m[AAR_MSG_BYTES];

    cm_keyword(m, kw, a, b, c);
    return cmio_send(m, 5000000u);
}

/* Waits up to timeout_us for one message and copies it into m. Returns 1 for a
 * message, 0 for none in time, a negative code for an error (a stop included). */
static inline int cmio_recv(uint8_t *m, uint32_t timeout_us)
{
    uint64_t start = aar_time_us();

    for (;;) {
        uint64_t used = aar_time_us() - start;
        int rc = aar_msg_poll(m);

        if (rc != 0)
            return rc;
        if (used >= timeout_us)
            return 0;
        rc = aar_msg_wait((uint32_t)(timeout_us - used));
        if (rc != AAR_OK && rc != AAR_E_TIMEDOUT)
            return rc;
    }
}

/* Waits for the runner's result message. Returns 1 and the words, 0 when none
 * came in time, a negative code on an error. */
static inline int cmio_result(uint32_t timeout_us, uint32_t *ok, uint32_t *count,
                              uint32_t *errors, uint32_t *first_bad, uint32_t *x,
                              uint32_t *y)
{
    uint8_t m[AAR_MSG_BYTES];
    int rc = cmio_recv(m, timeout_us);

    if (rc != 1)
        return rc;
    if (!cm_is_result(m))
        return -1000;
    if (ok)
        *ok = cm_get32(m, 4);
    if (count)
        *count = cm_get32(m, 8);
    if (errors)
        *errors = cm_get32(m, 12);
    if (first_bad)
        *first_bad = cm_get32(m, 16);
    if (x)
        *x = cm_get32(m, 20);
    if (y)
        *y = cm_get32(m, 24);
    return 1;
}

/* Takes every message that is queued for the app, waiting a moment for late
 * ones. Returns how many. */
static inline uint32_t cmio_drain(void)
{
    uint8_t m[AAR_MSG_BYTES];
    uint32_t n = 0;
    uint32_t quiet = 0;

    while (quiet < 3u) {
        if (aar_msg_poll(m) == 1) {
            n++;
            quiet = 0;
        } else {
            quiet++;
            aar_sleep_us(2000);
        }
    }
    return n;
}

/* Appends "name rc" to a failure text. */
static inline void cmio_why_rc(struct conf_test *t, const char *what, int rc)
{
    conf_buf_str(&t->why, what);
    conf_buf_str(&t->why, ": ");
    conf_buf_i64(&t->why, rc);
}

#endif
