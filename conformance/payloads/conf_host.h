/*
 * What the C-HOST payload (conf_host.c, the app) and the runner side (runner/r_host.c,
 * the client) of C-HOST agree on: the environment variables and clipboard units that
 * the runner prepares, the peer keywords and the patterns. Both include this file.
 *
 * Fixtures of the variant main (the runner prepares them before the load and removes
 * them after the run; every environment name starts with AAR_ so that the qemu loader
 * of libaar, which serves host variables with that prefix, can serve them too, and
 * conformance/tests/qemu_run.sh sets them):
 *   AAR_CONF_HELLO   global variable, CH_HELLO_VALUE (11 bytes)
 *   AAR_CONF_LONG    global variable of 300 bytes, byte i is ch_long_byte(i)
 *   AAR_CONF_LOCAL   a variable local to the runner's process, which is not in ENV:
 *                    and so must not be found by the app
 *   AAR_CONF_UNSET   never set
 *   clipboard unit 201   an IFF FTXT with one CHRS chunk, CH_RUNNER_LATIN1
 *
 * Keywords (a message is a keyword and three words, conf_msg.h). The runner answers
 * each with a result message (ok, count, errors; conf_msg.h): ok 1 matches, 0 differs
 * (count what the client has, errors what the app sent), 2 the runner had nothing to
 * compare with.
 *   CONF-HOST-LOCALE  a = utc_offset_min (as a 32-bit word), b = ch_hash(language),
 *                     c = flags
 *   CONF-HOST-LOC2    a = ch_hash(decimal_point), b = ch_hash(group_separator)
 *   CONF-HOST-CLIP    a = a clipboard unit: the runner reads it and compares the text
 *                     with CH_APP_LATIN1
 *
 * Without the argument conf:peer=1 (the host runner, a run by hand) the tests that
 * need the runner print SKIP host. The argument conf:fixtures=1 says that the
 * variables above exist although there is no runner (the qemu run).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_HOST_H
#define AAR_CONF_HOST_H

#include "conf_msg.h"

#define CH_VARIANT_MAIN "main"

#define CH_K_LOCALE "CONF-HOST-LOCALE"
#define CH_K_LOC2   "CONF-HOST-LOC2"
#define CH_K_CLIP   "CONF-HOST-CLIP"

#define CH_ENV_HELLO "AAR_CONF_HELLO"
#define CH_ENV_LONG  "AAR_CONF_LONG"
#define CH_ENV_LOCAL "AAR_CONF_LOCAL"
#define CH_ENV_UNSET "AAR_CONF_UNSET"

#define CH_HELLO_VALUE "hello world"
#define CH_HELLO_BYTES 11u
#define CH_LONG_BYTES  300u

/* The clipboard: the unit the runner fills, and the unit the app fills for the runner. */
#define CH_RUNNER_UNIT 201u
#define CH_APP_UNIT    202u
/* UTF-8 as the app reads it, and ISO-8859-1 as it is stored on the clipboard. */
#define CH_RUNNER_UTF8   "Clip from the runner: caf\xC3\xA9"
#define CH_RUNNER_LATIN1 "Clip from the runner: caf\xE9"
/* What the app writes (the last character, U+4E2D, is in no Amiga character set) and what the
 * clipboard then holds. */
#define CH_APP_UTF8   "App clip: caf\xC3\xA9 \xE4\xB8\xAD"
#define CH_APP_LATIN1 "App clip: caf\xE9 ?"
/* The scratch unit of the app's own round trips. */
#define CH_SCRATCH_UNIT 200u

/* The clip of the large test: bytes of ch_big_byte(i), 70000 of them, more than one staging
 * buffer of the runtime. */
#define CH_BIG_BYTES 70000u

static inline uint8_t ch_long_byte(uint32_t i)
{
    return (uint8_t)('A' + i % 26u);
}

static inline uint8_t ch_big_byte(uint32_t i)
{
    return (uint8_t)('a' + (i * 7u + (i >> 8)) % 26u);
}

/* FNV-1a over the bytes of a string (without its NUL). */
static inline uint32_t ch_hash(const char *s)
{
    uint32_t h = 2166136261u;

    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

_Static_assert(sizeof CH_K_LOCALE <= 20 && sizeof CH_K_LOC2 <= 20 && sizeof CH_K_CLIP <= 20,
               "a message keyword is longer than 19 characters");

#endif
