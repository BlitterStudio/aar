/*
 * What the C-QUEUE payload (conf_queue.c) and runner/r_queue.c share: the keywords of
 * the standard run (the runner serves with aar_serve and acts on these messages) and
 * the marker messages of the runs the runner serves by hand (AARServiceNext and
 * AARServiceReply, runner/r_mq.c). The helpers of conf_msg.h build and read both.
 *
 * Variants (conf:variant=<name>):
 *   std     C-QUEUE.9, C-QUEUE.10, served by aar_serve; the keywords below
 *   late    C-QUEUE.6, C-QUEUE.7, served by hand: the runner withholds a completion
 *   park    the runner judges C-QUEUE.2 to C-QUEUE.5 and C-QUEUE.11, served by hand
 *   sqfull  the runner judges C-QUEUE.8: the app floods messages, nobody drains
 *   spin    a compute-bound app that reports its work when it is stopped (C-QUEUE.15,16)
 *   wait    an app parked in a wait for ever (C-QUEUE.16)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_QUEUE_H
#define AAR_CONF_QUEUE_H

#include "conf_msg.h"

/* a = ms: the runner does not serve for that long (it sits in the handler). */
#define CQ_K_HOLD    "CONF-QUEUE-HOLD"
/* The runner answers with a result message: count = log records it received so far,
 * errors = AARStatusInfo.log_dropped. */
#define CQ_K_LOGSTAT "CONF-QUEUE-LOGSTAT"
/* a = ms: from now on the runner serves only once per a ms (it sleeps between
 * passes, whatever wakes it); 0 ends that. */
#define CQ_K_SLOW    "CONF-QUEUE-SLOW"
/* a = n: the runner checks that the console text holds the lines "Q10 00000" to
 * "Q10 <n-1>" in order, once the last has arrived (or 20 s have passed), and answers
 * with a result message. */
#define CQ_K_CHECK   "CONF-QUEUE-CHECK"

#define CQ_LINES 5000u

/* Markers of the manual runs: "QMK" and a stage character, then the little-endian result
 * code of the call the app made at that stage. */
static inline void cq_marker(uint8_t *m, char stage, int32_t value)
{
    uint32_t i;

    for (i = 0; i < 32u; i++)
        m[i] = 0;
    m[0] = 'Q';
    m[1] = 'M';
    m[2] = 'K';
    m[3] = (uint8_t)stage;
    cm_put32(m, 4, (uint32_t)value);
}

static inline int cq_is_marker(const uint8_t *m, char stage)
{
    return m[0] == 'Q' && m[1] == 'M' && m[2] == 'K' && m[3] == (uint8_t)stage;
}

/* The report of a spin app: "SPIN", loops, elapsed ms. */
static inline int cq_is_spin(const uint8_t *m)
{
    return m[0] == 'S' && m[1] == 'P' && m[2] == 'I' && m[3] == 'N';
}

_Static_assert(sizeof CQ_K_HOLD <= 20 && sizeof CQ_K_LOGSTAT <= 20 && sizeof CQ_K_SLOW <= 20 &&
                   sizeof CQ_K_CHECK <= 20,
               "a queue keyword is longer than 19 characters");

#endif
