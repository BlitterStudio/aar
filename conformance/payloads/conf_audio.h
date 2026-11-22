/*
 * What the C-AUDIO payload (conf_audio.c) and runner/r_audio.c share. conf_msg.h has the
 * message helpers.
 *
 * Variants (conf:variant=<name>):
 *   std    C-AUDIO.1 to C-AUDIO.9, the platform route (the serving loop of aar_serve)
 *   ahi    the runner judges C-AUDIO.10 and C-AUDIO.11 (AAR_TAG_AUDIO_ROUTE AHI): the app
 *          writes a ramp and the runner reads it back with AARAudioRead, served by hand
 *   hold   an app that opens a stream and keeps it, for C-AUDIO.9 (a runner peer starts
 *          several of them)
 *   tone   the operator row C-AUDIO.12 (only with INTERACTIVE): a 440 Hz tone for two seconds
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_AUDIO_H
#define AAR_CONF_AUDIO_H

#include "conf_msg.h"

/* The runner starts apps that open an audio stream and hold it, one after the other, until
 * one of them is refused or six are open, and answers with a result message: count = how
 * many opened, ok = 1 when the first refusal was AAR_E_BUSY or there was none, x = the
 * refusal's code (0 when none). */
#define CA_K_MORE "CONF-AUDIO-MORE"

/* The ramp of the ahi variant: frame f is left (f & 0x7FFF), right ((~f) & 0x7FFF). */
#define CA_RAMP_FRAMES 48000u

static inline int16_t ca_ramp_left(uint32_t f)
{
    return (int16_t)(f & 0x7FFFu);
}

static inline int16_t ca_ramp_right(uint32_t f)
{
    return (int16_t)((~f) & 0x7FFFu);
}

/* The hold app reports "AHLD" and the result of its audio_open. */
static inline void ca_hold_marker(uint8_t *m, int32_t rc)
{
    uint32_t i;

    for (i = 0; i < 32u; i++)
        m[i] = 0;
    m[0] = 'A';
    m[1] = 'H';
    m[2] = 'L';
    m[3] = 'D';
    cm_put32(m, 4, (uint32_t)rc);
}

static inline int ca_is_hold_marker(const uint8_t *m)
{
    return m[0] == 'A' && m[1] == 'H' && m[2] == 'L' && m[3] == 'D';
}

#endif
