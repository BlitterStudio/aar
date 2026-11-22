/*
 * What the payload conf_con.c (the app) and the runner side r_con.c (the
 * client) of C-CON agree on: the peer keywords, the message layout, the console
 * block the runner checks in its capture, the log records, the scripted input.
 * Both include this file; the payload includes conf.h first to get the app's
 * helpers.
 *
 * Every message is 32 bytes: bytes 0 to 15 are the keyword (zero padded), bytes
 * 16 to 31 four 32-bit words, little-endian, w0 to w3. The runner answers each
 * request with one CONF_CON_RES (w0 0 for passed, else a code; w1 detail).
 *
 * Variants (conf:variant=<name>, in every run of the runner):
 *   main      C-CON.1 and C-CON.7 to C-CON.9; the console is a plain capture and
 *             the log hook stores the records
 *   slowout   C-CON.2; every console write completes 500 ms after it started
 *   input     C-CON.3, C-CON.4 and C-CON.6; console_read gets CONF_CON_INPUT
 *             and then the end of input
 *   block     C-CON.5; console_read after the (empty) script waits for ever
 * A run without the argument (the host runner) is a run of main for the tests that
 * need no runner, the others print SKIP host.
 *
 * Requests (app to runner):
 *
 *   CONF_CON_VERIFY w0 length, w1 seed
 *     The app wrote, with one console_write, CONF_CON_BEGIN, the block of
 *     length bytes conf_con_block_byte(seed, i), then CONF_CON_END. The runner
 *     finds CONF_CON_BEGIN in its capture, checks the length bytes after it and
 *     the end marker, cuts the whole text out of the capture (so that it is not
 *     taken for result lines) and answers. w0 of the answer: 0, or 1 begin not
 *     found, 2 a byte differs (w1 the offset), 3 the end marker is missing or
 *     the capture overflowed.
 *   CONF_CON_FIND   bytes 16 to 31: a text of up to 15 bytes
 *     Answers w0 0 if the capture holds the text, 1 if not.
 *   CONF_CON_LOGCHK w0 mode (1 one record, 2 all), w1 record number k
 *     Mode 1: exactly one log record of number k arrived and it is a prefix of
 *     conf_con_log_fill(k) of at most CONF_LOG_MAX (1024) bytes; w0 of the
 *     answer 0 or 1 no record, 2 more than one, 3 not a prefix or too long
 *     (w1 the length). The text of the record is not in the capture. Mode 2:
 *     every record that arrived (of any number) is a prefix of the record of
 *     its number and at most 1024 bytes, no number twice, at least one arrived
 *     of the numbers 2 to 7; w0 of the answer 0 or the same codes, 4 for a
 *     record that is not ours.
 *   CONF_CON_STOPAT w0 milliseconds
 *     The runner stops the app (as a client does) after that time. The answer
 *     is sent at once.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_CON_H
#define AAR_CONF_CON_H

#include <stdint.h>

#define CONF_CON_VERIFY "CONF-CON-VERIFY"
#define CONF_CON_FIND   "CONF-CON-FIND"
#define CONF_CON_LOGCHK "CONF-CON-LOGCHK"
#define CONF_CON_STOPAT "CONF-CON-STOPAT"
#define CONF_CON_RES    "CONF-CON-RES"

#define CONF_CON_KEYWORD_BYTES 16u
#define CONF_CON_MSG_BYTES     32u
#define CONF_CON_RPC_WAIT_US   10000000u

#define CONF_CON_VARIANT_MAIN    "main"
#define CONF_CON_VARIANT_SLOWOUT "slowout"
#define CONF_CON_VARIANT_INPUT   "input"
#define CONF_CON_VARIANT_BLOCK   "block"

/* The block: lines of 64 bytes. A console write of the markers and the block. */
#define CONF_CON_BEGIN "@@CON-BEGIN@@\n"
#define CONF_CON_END   "@@CON-END@@\n"
#define CONF_CON_BIG_BYTES  (200u * 1024u)   /* C-CON.1 */
#define CONF_CON_SLOW_BYTES 4096u            /* C-CON.2 */
#define CONF_CON_SLOW_HOLD_TICKS 25u         /* 500 ms at 50 ticks a second */
#define CONF_CON_SLOW_MIN_US 450000u

/* Scripted input of the variant input: the first line is read with
 * aar_console_read, the second with read(0, ...). */
#define CONF_CON_INPUT_A "conf-input: the quick brown fox jumps over\n"
#define CONF_CON_INPUT_B "libc read(0) line, the second part\n"
#define CONF_CON_INPUT CONF_CON_INPUT_A CONF_CON_INPUT_B
#define CONF_CON_INPUT_HOLD_TICKS 15u        /* the first read waits 300 ms */
#define CONF_CON_INPUT_MIN_US 250000u

/* libc flavour markers of C-CON.6, written with write(1, ...) and write(2, ...). */
#define CONF_CON_FD1 "CONF-CON-FD1 ok\n"
#define CONF_CON_FD2 "CONF-CON-FD2 ok\n"

#define CONF_CON_STOP_DELAY_MS 300u

/* ---- Log records ------------------------------------------------------------------ */

#define CONF_LOG_MAX 1024u        /* AAR_LOG_MAX */
#define CONF_CON_LOG_KINDS 8u
#define CONF_CON_LOG_TAG_BYTES 12u  /* "CONF-LOG-kk:" */

/* The length of the record of number k that the app sends. k 0 is an ordinary
 * record (C-CON.7), 1 is 2000 bytes (C-CON.8), 2 to 7 go with C-CON.9. */
static inline uint32_t conf_con_log_len(uint32_t k)
{
    static const uint32_t len[CONF_CON_LOG_KINDS] = {
        40u, 2000u, 1023u, 1024u, 1025u, 1500u, 3000u, 5000u
    };

    return len[k % CONF_CON_LOG_KINDS];
}

/* Byte i of the record of number k: the tag "CONF-LOG-kk:" and then a printable
 * pattern. */
static inline uint8_t conf_con_log_byte(uint32_t k, uint32_t i)
{
    static const char tag[] = "CONF-LOG-";
    static const char hex[] = "0123456789abcdef";

    if (i < 9u)
        return (uint8_t)tag[i];
    if (i == 9u)
        return (uint8_t)hex[(k >> 4) & 15u];
    if (i == 10u)
        return (uint8_t)hex[k & 15u];
    if (i == 11u)
        return (uint8_t)':';
    return (uint8_t)(' ' + 1u + (i * 7u + k * 13u) % 90u);
}

static inline void conf_con_log_fill(uint32_t k, uint8_t *out, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
        out[i] = conf_con_log_byte(k, i);
}

/* ---- The console block --------------------------------------------------------------- */

/* Byte i of the block of a seed: lines of 64 bytes, "L", six hex digits of the
 * line number, a space, 55 characters of a 64-letter alphabet, a newline. Never
 * starts a line with the text of a result line. */
static inline uint8_t conf_con_block_byte(uint32_t seed, uint32_t i)
{
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char hex[] = "0123456789abcdef";
    uint32_t line = i >> 6;
    uint32_t j = i & 63u;

    if (j == 0)
        return (uint8_t)'L';
    if (j <= 6u)
        return (uint8_t)hex[(line >> (4u * (6u - j))) & 15u];
    if (j == 7u)
        return (uint8_t)' ';
    if (j == 63u)
        return (uint8_t)'\n';
    return (uint8_t)alpha[(line * 31u + j * 17u + seed * 5u) & 63u];
}

static inline void conf_con_block_fill(uint32_t seed, uint8_t *out, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
        out[i] = conf_con_block_byte(seed, i);
}

/* ---- Words ---------------------------------------------------------------------------- */

static inline uint32_t conf_con_get32(const uint8_t *m, uint32_t word)
{
    const uint8_t *p = m + CONF_CON_KEYWORD_BYTES + 4u * word;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void conf_con_build(uint8_t *m, const char *keyword, uint32_t w0,
                                  uint32_t w1, uint32_t w2, uint32_t w3)
{
    uint32_t w[4];
    uint32_t i;
    uint32_t k;

    w[0] = w0;
    w[1] = w1;
    w[2] = w2;
    w[3] = w3;
    for (i = 0; i < CONF_CON_MSG_BYTES; i++)
        m[i] = 0;
    for (i = 0; keyword[i] && i < CONF_CON_KEYWORD_BYTES; i++)
        m[i] = (uint8_t)keyword[i];
    for (k = 0; k < 4u; k++) {
        m[CONF_CON_KEYWORD_BYTES + 4u * k + 0] = (uint8_t)w[k];
        m[CONF_CON_KEYWORD_BYTES + 4u * k + 1] = (uint8_t)(w[k] >> 8);
        m[CONF_CON_KEYWORD_BYTES + 4u * k + 2] = (uint8_t)(w[k] >> 16);
        m[CONF_CON_KEYWORD_BYTES + 4u * k + 3] = (uint8_t)(w[k] >> 24);
    }
}

static inline int conf_con_is(const uint8_t *m, const char *keyword)
{
    uint32_t i;

    for (i = 0; keyword[i]; i++) {
        if (m[i] != (uint8_t)keyword[i])
            return 0;
    }
    return 1;
}

/* ---- The app's side: only when the payload protocol (conf.h) is included first ---------- */

#ifdef AAR_CONF_H

/* Sends a request and waits for the CONF_CON_RES. Returns AAR_OK with
 * out[0..3], AAR_E_TIMEDOUT, AAR_E_IO on a stop, or the failing call's code. */
static inline int conf_con_rpc(const char *keyword, uint32_t w0, uint32_t w1,
                               const char *text, uint32_t out[4])
{
    uint8_t m[CONF_CON_MSG_BYTES];
    uint64_t start = aar_time_us();
    uint32_t i;
    int rc;

    conf_con_build(m, keyword, w0, w1, 0, 0);
    if (text) {
        for (i = 0; text[i] && i < 15u; i++)
            m[CONF_CON_KEYWORD_BYTES + i] = (uint8_t)text[i];
    }
    do {
        rc = aar_msg_send(m);
        if (rc != AAR_E_BUSY)
            break;
        aar_sleep_us(1000);
    } while (aar_time_us() - start < 100000u);
    if (rc != AAR_OK)
        return rc;
    start = aar_time_us();
    for (;;) {
        if (aar_time_us() - start >= CONF_CON_RPC_WAIT_US)
            return AAR_E_TIMEDOUT;
        rc = aar_msg_wait(100000u);
        if (rc != AAR_OK && rc != AAR_E_TIMEDOUT)
            return rc;
        while ((rc = aar_msg_poll(m)) == 1) {
            if (conf_con_is(m, CONF_CON_RES)) {
                for (i = 0; i < 4u; i++)
                    out[i] = conf_con_get32(m, i);
                return AAR_OK;
            }
        }
        if (rc < 0)
            return rc;
    }
}

#endif /* AAR_CONF_H */

#endif /* AAR_CONF_CON_H */
