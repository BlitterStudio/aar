/*
 * What the payload conf_life.c and the runner (conformance/runner/r_life.c, and
 * r_client.c, which uses the same app) agree on: the variants, the exit codes,
 * the peer messages.
 *
 * conf_life is not a payload that judges anything itself. Every C-LIFE and many
 * C-CLIENT tests are judged by the runner from outside, and this app is the
 * behaviour it observes: one source with a variant (conf:variant=<name>, set by
 * the runner) that says what the app does. It prints no AAR-CONF line.
 *
 * Messages are 32 bytes: a 16-byte keyword (zero padded), then four 32-bit words,
 * little-endian: w0 and w1 are the low and high half of a 64-bit value a, w2 and
 * w3 of b.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_LIFE_H
#define AAR_CONF_LIFE_H

#include <stdint.h>

/* ---- Variants (the value of conf:variant) ---------------------------------------- */

/* ret:<n> returns n from aar_main; exit:<n> calls aar_exit(n). */
#define CONF_LIFE_V_RET "ret:"
#define CONF_LIFE_V_EXIT "exit:"
/* Runs until a stop request, takes 200 ms to clean up, then returns
 * CONF_LIFE_STOP_OK when should_stop was 1 and an AAR_EV_STOP event was queued. */
#define CONF_LIFE_V_STOP "stop"
/* Ignores should_stop for ever (it calls aar_yield in a loop). */
#define CONF_LIFE_V_IGNORE "ignore-stop"
/* Report the registers, then fault: data abort, fetch from an unmapped address,
 * undefined instruction, misaligned exclusive access. */
#define CONF_LIFE_V_FAULT_DATA "fault-data"
#define CONF_LIFE_V_FAULT_FETCH "fault-fetch"
#define CONF_LIFE_V_FAULT_ILLEGAL "fault-illegal"
#define CONF_LIFE_V_FAULT_ALIGN "fault-align"
/* Sleeps 500 ms and then writes to the unmapped address, sending no message. */
#define CONF_LIFE_V_FAULT_QUIET "fault-quiet"
/* Waits in aar_msg_wait for a message, returns 0 when one arrives (5 on a stop). */
#define CONF_LIFE_V_WAIT_MSG "wait-msg"
/* Computes without any table call for conf:ms milliseconds (calibrated), then
 * sends CONF_LIFE_GAP with the silent time it measured and returns 0. */
#define CONF_LIFE_V_SPIN "spin"
/* aar_sleep_us for conf:ms milliseconds, then CONF_LIFE_GAP and 0. */
#define CONF_LIFE_V_SLEEP "sleep"
/* Six rounds of three 4 s silent computations, each followed by one table call:
 * yield, should_stop, time_us, msg_wait with a timeout, a file call, a console
 * call. Returns 0. Needs a serving client. */
#define CONF_LIFE_V_ACTIVITY "activity"
/* Allocates CONF_LIFE_SHARED_BYTES of shared memory, sends CONF_LIFE_TOKEN with the
 * token and the size, and waits for the stop request. Returns 0. */
#define CONF_LIFE_V_HOLD_SHARED "hold-shared"
/* Allocates blocks of CONF_LIFE_SHARED_BYTES until the allocation fails (at most
 * 64), fills them with conf_life_byte, then checks them for 4 s. Returns 0 when
 * every byte stayed, 1 when one changed. */
#define CONF_LIFE_V_FILL_SHARED "fill-shared"
/* Sleeps 500 ms, sends conf:ms (at least 1) CONF_LIFE_MSG messages, waits for a
 * message of the runner (10 s), writes "x" to the console (a service request that
 * the client has to serve) and returns 0 (9 without the runner's message). */
#define CONF_LIFE_V_BURST "burst"
/* Answers every message of the runner with CONF_LIFE_PONG until the stop request,
 * then returns 0. */
#define CONF_LIFE_V_PING "ping"
/* Sends CONF_LIFE_BEAT every 50 ms until the stop request, then returns 0. */
#define CONF_LIFE_V_BEAT "beat"
/* Checks its BSS and its arguments, prints one line on the console (the same
 * line in every run) and returns 0. */
#define CONF_LIFE_V_HELLO "hello"
/* Calls aar_display_open; returns 0 when it answered AAR_E_NOSYS, else 1 (and
 * closes a display that opened). */
#define CONF_LIFE_V_DISPLAY "display-open"
/* Files, directory list, console, log and message calls through aar_serve;
 * returns 0 when all succeeded, else the number of the first failing step. */
#define CONF_LIFE_V_SERVE_ALL "serve-all"

/* ---- Exit codes ------------------------------------------------------------------- */

#define CONF_LIFE_STOP_OK 42      /* variant stop: should_stop and AAR_EV_STOP seen */
#define CONF_LIFE_STOP_NO_FLAG 41 /* should_stop was 0 after the stop */
#define CONF_LIFE_STOP_NO_EVENT 43 /* no AAR_EV_STOP event */
#define CONF_LIFE_UNKNOWN_VARIANT 99
#define CONF_LIFE_STOP_CLEANUP_US 200000u

/* ---- Messages ------------------------------------------------------------------------ */

#define CONF_LIFE_FAULT_A "CONF-LIFE-FAULTA" /* a: the address touched, b: pc of the instruction */
#define CONF_LIFE_FAULT_B "CONF-LIFE-FAULTB" /* a: sp at the instruction, b: lr (CONF_LIFE_LR_MAGIC) */
#define CONF_LIFE_FAULT_C "CONF-LIFE-FAULTC" /* a: the address of aar_start (offset 0 of the image) */
#define CONF_LIFE_TOKEN "CONF-LIFE-TOKEN"    /* a: the token, b: the bytes */
#define CONF_LIFE_GAP "CONF-LIFE-GAP"        /* a: silent microseconds measured, b: asked */
#define CONF_LIFE_BEAT "CONF-LIFE-BEAT"      /* a: counter */
#define CONF_LIFE_MSG "CONF-LIFE-MSG"        /* a: index (burst) */
#define CONF_LIFE_GO "CONF-LIFE-GO"          /* runner to app: go on */
#define CONF_LIFE_PING "CONF-LIFE-PING"      /* runner to app: a: number */
#define CONF_LIFE_PONG "CONF-LIFE-PONG"      /* app to runner: the same a */

#define CONF_LIFE_LR_MAGIC 0x4C494645u /* "LIFE": the lr the fault variants set */
/* The address the fault variants touch (unmapped in any app's address space [spec
 * question 41]), as the low and high word. */
#define CONF_LIFE_FAULT_ADDR_LO32 0xDEAD0000u
#define CONF_LIFE_FAULT_ADDR_HI32 0x00000000u
#define CONF_LIFE_FAULT_ADDR_LO64 0x00000000u
#define CONF_LIFE_FAULT_ADDR_HI64 0x0000DEADu

#define CONF_LIFE_SHARED_BYTES 262144u

/* The byte i of block number n that fill-shared writes. */
static inline uint8_t conf_life_byte(uint32_t n, uint32_t i)
{
    return (uint8_t)(i * 7u + n * 13u + 1u);
}

/* ---- Words ---------------------------------------------------------------------------- */

#define CONF_LIFE_KEYWORD_BYTES 16u
#define CONF_LIFE_MSG_BYTES 32u

static inline void conf_life_build(uint8_t *m, const char *keyword, uint32_t a_lo,
                                   uint32_t a_hi, uint32_t b_lo, uint32_t b_hi)
{
    uint32_t w[4];
    uint32_t i;
    uint32_t k;

    w[0] = a_lo;
    w[1] = a_hi;
    w[2] = b_lo;
    w[3] = b_hi;
    for (i = 0; i < CONF_LIFE_MSG_BYTES; i++)
        m[i] = 0;
    for (i = 0; keyword[i] && i < CONF_LIFE_KEYWORD_BYTES; i++)
        m[i] = (uint8_t)keyword[i];
    for (k = 0; k < 4u; k++) {
        m[CONF_LIFE_KEYWORD_BYTES + 4u * k + 0] = (uint8_t)w[k];
        m[CONF_LIFE_KEYWORD_BYTES + 4u * k + 1] = (uint8_t)(w[k] >> 8);
        m[CONF_LIFE_KEYWORD_BYTES + 4u * k + 2] = (uint8_t)(w[k] >> 16);
        m[CONF_LIFE_KEYWORD_BYTES + 4u * k + 3] = (uint8_t)(w[k] >> 24);
    }
}

/* Word 0 to 3 of a message. */
static inline uint32_t conf_life_get(const uint8_t *m, uint32_t word)
{
    const uint8_t *p = m + CONF_LIFE_KEYWORD_BYTES + 4u * word;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline int conf_life_is(const uint8_t *m, const char *keyword)
{
    uint32_t i;

    for (i = 0; keyword[i]; i++) {
        if (m[i] != (uint8_t)keyword[i])
            return 0;
    }
    return 1;
}

#endif
