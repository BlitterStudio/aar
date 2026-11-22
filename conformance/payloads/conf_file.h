/*
 * What the payload conf_file.c (the app) and the runner side r_file.c (the
 * client) of C-FILE agree on: the fixture tree the runner prepares under ROOT,
 * the patterns, the peer keywords and the variants. Both include this file; the
 * payload includes conf.h first to get the app's helpers.
 *
 * The fixtures, under CONF_FILE_DIR (conf-fixtures) in the client's root:
 *   text.txt       CONF_FILE_TEXT
 *   pattern.bin    1 MiB, byte i is conf_file_pat(i)
 *   dated.txt      CONF_FILE_DATED_TEXT; its date is CONF_FILE_MTIME (Unix)
 *   dir5/          five files a.txt to e.txt, each holding its own name
 *   long1xxx/.../long7xxx/leafyyy...   a chain of seven directories of 30
 *                  characters; the file CONF_FILE_LONG255 (24 characters) and
 *                  the file one character longer, CONF_FILE_LONG256, so that
 *                  the path to each is exactly 255 and 256 bytes
 *   leakdir/       an empty directory
 *   big17.bin      16 MiB and one byte (only where the platform is ZZ9000)
 * The runner also removes every *.tmp of the fixture directory before each run:
 * the files the payload creates are called *.tmp.
 *
 * Every message is 32 bytes: bytes 0 to 15 the keyword (zero padded), bytes 16
 * to 31 four 32-bit words, little-endian.
 *
 * Variants (conf:variant=<name>):
 *   main    every test of the payload but the second half of C-FILE.5; the
 *           runner prepares the fixtures first. Without the argument (the host
 *           runner) the tests that need no fixture run in the current directory
 *           and the others print SKIP host
 *   abs     the runner serves with AAR_SERVEF_ABS_PATHS and passes the full path
 *           of text.txt in conf:abs=<path>: the second half of C-FILE.5
 *   stall   run by the runner's own serving loop (r_file.c), not by aar_serve:
 *           it withholds replies. Judges C-FILE.26 to C-FILE.28. See below
 *   leak    run by r_file.c: opens files and lists and ends without closing
 *           them (C-FILE.31)
 *
 * Requests of the payload in main:
 *   CONF_FILE_CHECK w0 1: the runner reads created.tmp and compares it with
 *                      CONF_FILE_CREATED
 *                   w0 2: the runner reads out1m.tmp and compares it with
 *                      conf_file_pat2 over 1 MiB
 *     Answer CONF_FILE_RES w0 0, or 1 the file is missing, 2 the bytes differ
 *     (w1 the offset or the length found).
 *
 * The stall variant, steps (the app sends CONF_FILE_STALL with w0 = step, w1 =
 * the call's result as an unsigned 32-bit value, w2 = the milliseconds the call
 * took, w3 = a count):
 *   1  aar_file_open(CONF_FILE_STALL_A): the runner withholds the reply. The
 *      app gets AAR_E_TIMEDOUT after 30 s and sends step 1. The runner then
 *      completes the late request and answers CONF_FILE_GO.
 *   2  after GO the app opens and closes CONF_FILE_STALL_B 20 times (the
 *      runner completes every request) and sends step 2: w1 the first failing
 *      result or 0, w3 the number of rounds done. The runner has checked that
 *      the file id of the timed-out open was never handed out again.
 *   3  the app sends step 3 and calls aar_file_stat(CONF_FILE_STALL_C); the
 *      runner withholds that reply, waits one second and stops the app. The
 *      app gets AAR_E_IO and sends step 4 (w1 the result, w2 the milliseconds).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_FILE_H
#define AAR_CONF_FILE_H

#include <stdint.h>

#define CONF_FILE_CHECK "CONF-FILE-CHECK"
#define CONF_FILE_RES   "CONF-FILE-RES"
#define CONF_FILE_STALL "CONF-FILE-STALL"
#define CONF_FILE_GO    "CONF-FILE-GO"

#define CONF_FILE_KEYWORD_BYTES 16u
#define CONF_FILE_MSG_BYTES     32u
#define CONF_FILE_RPC_WAIT_US   10000000u

#define CONF_FILE_VARIANT_MAIN  "main"
#define CONF_FILE_VARIANT_ABS   "abs"
#define CONF_FILE_VARIANT_STALL "stall"
#define CONF_FILE_VARIANT_LEAK  "leak"

/* ---- Fixture names --------------------------------------------------------------- */

#define CONF_FILE_DIR      "conf-fixtures"
#define CONF_FILE_TEXT_NAME    "text.txt"
#define CONF_FILE_PATTERN_NAME "pattern.bin"
#define CONF_FILE_DATED_NAME   "dated.txt"
#define CONF_FILE_DIR5_NAME    "dir5"
#define CONF_FILE_LEAKDIR_NAME "leakdir"
#define CONF_FILE_BIG_NAME     "big17.bin"

#define CONF_FILE_TEXT "AAR conformance fixture\nthe second line of the text file\n"
#define CONF_FILE_DATED_TEXT "dated fixture\n"
#define CONF_FILE_CREATED "created by the app\n"
#define CONF_FILE_LONG_TEXT "long path leaf\n"

#define CONF_FILE_PATTERN_BYTES (1024u * 1024u)
#define CONF_FILE_BIG_BYTES ((16u * 1024u * 1024u) + 1u)

/* Unix seconds 1000000000, and the AmigaDOS DateStamp that SetFileDate takes. */
#define CONF_FILE_MTIME 1000000000
#define CONF_FILE_DATE_DAYS  8652u
#define CONF_FILE_DATE_MINS  106u
#define CONF_FILE_DATE_TICKS 2000u

/* The five files of dir5. */
#define CONF_FILE_DIR5_COUNT 5u
#define CONF_FILE_DIR5_NAMES { "a.txt", "b.txt", "c.txt", "d.txt", "e.txt" }

/* The chain of directories (30 characters each) and the leaf names. */
#define CONF_FILE_LONG_DEPTH 7u
#define CONF_FILE_LD1 "long1xxxxxxxxxxxxxxxxxxxxxxxxx"
#define CONF_FILE_LD2 "long2xxxxxxxxxxxxxxxxxxxxxxxxx"
#define CONF_FILE_LD3 "long3xxxxxxxxxxxxxxxxxxxxxxxxx"
#define CONF_FILE_LD4 "long4xxxxxxxxxxxxxxxxxxxxxxxxx"
#define CONF_FILE_LD5 "long5xxxxxxxxxxxxxxxxxxxxxxxxx"
#define CONF_FILE_LD6 "long6xxxxxxxxxxxxxxxxxxxxxxxxx"
#define CONF_FILE_LD7 "long7xxxxxxxxxxxxxxxxxxxxxxxxx"
#define CONF_FILE_LEAF24 "leafyyyyyyyyyyyyyyyyyyyy"
#define CONF_FILE_LEAF25 "leafyyyyyyyyyyyyyyyyyyyyy"
#define CONF_FILE_LONG_DIRS CONF_FILE_LD1 "/" CONF_FILE_LD2 "/" CONF_FILE_LD3 "/" \
    CONF_FILE_LD4 "/" CONF_FILE_LD5 "/" CONF_FILE_LD6 "/" CONF_FILE_LD7 "/"
/* The paths from the client's root: 255 and 256 bytes. */
#define CONF_FILE_LONG255 CONF_FILE_DIR "/" CONF_FILE_LONG_DIRS CONF_FILE_LEAF24
#define CONF_FILE_LONG256 CONF_FILE_DIR "/" CONF_FILE_LONG_DIRS CONF_FILE_LEAF25

/* Names of the stall variant. */
#define CONF_FILE_STALL_A "conf-stall-a"
#define CONF_FILE_STALL_B "conf-stall-b"
#define CONF_FILE_STALL_C "conf-stall-c"
#define CONF_FILE_STALL_ROUNDS 20u
#define CONF_FILE_TIMEOUT_MIN_MS 29000u
#define CONF_FILE_TIMEOUT_MAX_MS 36000u

_Static_assert(sizeof(CONF_FILE_LONG255) - 1u == 255u, "the long path is 255 bytes");
_Static_assert(sizeof(CONF_FILE_LONG256) - 1u == 256u, "the long path is 256 bytes");

/* ---- Patterns ----------------------------------------------------------------------- */

/* The 1 MiB fixture. */
static inline uint8_t conf_file_pat(uint32_t i)
{
    return (uint8_t)((i * 167u) ^ (i >> 8) ^ (i >> 16) ^ 0x3Cu);
}

/* What the payload writes in its 1 MiB file (C-FILE.22). */
static inline uint8_t conf_file_pat2(uint32_t i)
{
    return (uint8_t)(conf_file_pat(i) ^ 0x5Au);
}

/* The small scratch files of 100 bytes. */
static inline uint8_t conf_file_small(uint32_t i)
{
    return (uint8_t)(i * 3u + 1u);
}

/* ---- Words ----------------------------------------------------------------------------- */

static inline uint32_t conf_file_get32(const uint8_t *m, uint32_t word)
{
    const uint8_t *p = m + CONF_FILE_KEYWORD_BYTES + 4u * word;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void conf_file_build(uint8_t *m, const char *keyword, uint32_t w0,
                                   uint32_t w1, uint32_t w2, uint32_t w3)
{
    uint32_t w[4];
    uint32_t i;
    uint32_t k;

    w[0] = w0;
    w[1] = w1;
    w[2] = w2;
    w[3] = w3;
    for (i = 0; i < CONF_FILE_MSG_BYTES; i++)
        m[i] = 0;
    for (i = 0; keyword[i] && i < CONF_FILE_KEYWORD_BYTES; i++)
        m[i] = (uint8_t)keyword[i];
    for (k = 0; k < 4u; k++) {
        m[CONF_FILE_KEYWORD_BYTES + 4u * k + 0] = (uint8_t)w[k];
        m[CONF_FILE_KEYWORD_BYTES + 4u * k + 1] = (uint8_t)(w[k] >> 8);
        m[CONF_FILE_KEYWORD_BYTES + 4u * k + 2] = (uint8_t)(w[k] >> 16);
        m[CONF_FILE_KEYWORD_BYTES + 4u * k + 3] = (uint8_t)(w[k] >> 24);
    }
}

static inline int conf_file_is(const uint8_t *m, const char *keyword)
{
    uint32_t i;

    for (i = 0; keyword[i]; i++) {
        if (m[i] != (uint8_t)keyword[i])
            return 0;
    }
    return 1;
}

/* ---- The app's side: only when the payload protocol (conf.h) is included first ----------- */

#ifdef AAR_CONF_H

/* Sends a message; a full queue is retried for up to 100 ms. */
static inline int conf_file_send(const char *keyword, uint32_t w0, uint32_t w1,
                                 uint32_t w2, uint32_t w3)
{
    uint8_t m[CONF_FILE_MSG_BYTES];
    uint64_t start = aar_time_us();
    int rc;

    conf_file_build(m, keyword, w0, w1, w2, w3);
    do {
        rc = aar_msg_send(m);
        if (rc != AAR_E_BUSY)
            break;
        aar_sleep_us(1000);
    } while (aar_time_us() - start < 100000u);
    return rc;
}

/* Waits for a message that starts with `keyword`; other messages are dropped.
 * Returns AAR_OK with out[0..3], AAR_E_TIMEDOUT after wait_us, AAR_E_IO on a
 * stop, or the failing call's code. */
static inline int conf_file_wait(const char *keyword, uint64_t wait_us, uint32_t out[4])
{
    uint64_t start = aar_time_us();

    for (;;) {
        uint8_t m[CONF_FILE_MSG_BYTES];
        int rc;

        if (aar_time_us() - start >= wait_us)
            return AAR_E_TIMEDOUT;
        rc = aar_msg_wait(100000u);
        if (rc != AAR_OK && rc != AAR_E_TIMEDOUT)
            return rc;
        while ((rc = aar_msg_poll(m)) == 1) {
            if (conf_file_is(m, keyword)) {
                uint32_t i;

                for (i = 0; i < 4u; i++)
                    out[i] = conf_file_get32(m, i);
                return AAR_OK;
            }
        }
        if (rc < 0)
            return rc;
    }
}

#endif /* AAR_CONF_H */

#endif /* AAR_CONF_FILE_H */
