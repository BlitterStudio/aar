/*
 * What the C-FREE payload (conf_free.c) and the runner side (runner/r_free.c)
 * agree on. Plain macros and static inline functions, no includes, so the m68k
 * runner and the Arm payload can both use it.
 *
 * Variants (the runner passes conf:variant=<name>):
 *   main       C-FREE.1 and C-FREE.2: the freestanding build, judged from inside
 *   hog        takes the largest private heap it can get and exits without
 *              freeing it (C-FREE.4)
 *   sharedhog  uses the console and a file, then takes every shared block it can
 *              get and exits without freeing (C-FREE.5)
 *   services   opens four image and four audio decode sessions and exits (C-FREE.8)
 *   display    opens the display and exits (C-FREE.9)
 *   audio      opens the audio stream and exits (C-FREE.10)
 *   quick      returns 0 at once, with no console use: a claim that does nothing
 *              (C-FREE.6, and the claims the corpus run (C-FREE.7) counts)
 * The runner serves the variants other than quick and reads one message from
 * each: the keyword (at most 15 bytes, so that a NUL follows it) in the first
 * bytes, then little-endian 32-bit words from byte 16.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_FREE_H
#define AAR_CONF_FREE_H

#define CONF_MSG_FREE_HEAP    "CONF-FREE-HEAP"    /* w0: MiB of heap granted */
#define CONF_MSG_FREE_SHARED  "CONF-FREE-SHRD"   /* w0: KiB of shared blocks granted */
#define CONF_MSG_FREE_SVC     "CONF-FREE-SVC"     /* w0 image sessions opened, w1 1 if no image
                                                   * codec answered, w2 audio sessions opened,
                                                   * w3 1 if no audio codec answered */
#define CONF_MSG_FREE_DISPLAY "CONF-FREE-DISP"   /* w0: the result of aar_display_open */
#define CONF_MSG_FREE_AUDIO   "CONF-FREE-AUDIO"   /* w0: the result of aar_audio_open */

#define CONF_FREE_WORDS_OFFSET 16u
#define CONF_FREE_SESSIONS 4u /* AAR_MIN_DECODE_SESSIONS: opened per kind */

/* Little-endian 32-bit word number n of a message (from byte 16). */
static inline unsigned long conf_free_word(const unsigned char *b, unsigned n)
{
    unsigned off = CONF_FREE_WORDS_OFFSET + 4u * n;

    return (unsigned long)b[off] | ((unsigned long)b[off + 1] << 8) |
           ((unsigned long)b[off + 2] << 16) | ((unsigned long)b[off + 3] << 24);
}

#endif
