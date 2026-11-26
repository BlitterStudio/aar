/*
 * What the C-DISPLAY payload (conf_display.c), the C-GL payload (conf_gl.c) and the
 * runner side (runner/r_display.c, r_gl.c, r_gfx.c) share: the peer keywords, the test
 * patterns and the pixel codecs. The runner supplies its own display table to aar_serve
 * (pixel memory that it names with AARSharedToken), so it reads every frame the app
 * presents. conf_msg.h has the message helpers.
 *
 * Variants (conf:variant=<name>): std (the rows that the runner's display table can judge),
 * window (the display calls of API 0.6, against the client's own display), picture, pointer and
 * resize (the operator rows C-DISPLAY.17, C-DISPLAY.30 and C-DISPLAY.31, only with INTERACTIVE and
 * the built-in screen). The variant resize reports each resize it answered to the runner as a
 * message that starts with "RSZ" (a = width, b = height), "RSZBAD" for a refused resize and "NOCP"
 * when the display did not grant AAR_DISPCAP_RESIZE.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_DISPLAY_H
#define AAR_CONF_DISPLAY_H

#include "conf_msg.h"

/* a = bytes the runner adds to each row of the pixel memory it gives the next display. */
#define CD_K_PAD     "CONF-DISPLAY-PAD"
/* a = pattern (CD_PAT_*), b = parameter, c = layout the app was told. The runner compares
 * the frame in its pixel memory and answers with a result message (ok, count = bytes or
 * pixels that differ). */
#define CD_K_FRAME   "CONF-DISPLAY-FRAME"
/* The runner compares the palette it received with cd_pal and answers with a result message
 * (ok, count = entries it has, errors = entries that differ). */
#define CD_K_PALETTE "CONF-DISPLAY-PAL"

#define CD_PAT_BYTES 1u /* row bytes (cd_byte(b, i, y)); the padding is not looked at */
#define CD_PAT_QUADS 2u /* four colours: red, green / blue, white, by the layout in c */
#define CD_PAT_ROWS  3u /* every byte of row y is cd_row(y) */

static inline uint8_t cd_byte(uint32_t k, uint32_t i, uint32_t y)
{
    return (uint8_t)(i * 7u + y * 13u + k);
}

static inline uint8_t cd_row(uint32_t y)
{
    return (uint8_t)(y + 1u);
}

/* The palette the CLUT8 test sets: 0x00RRGGBB of entry i. */
static inline uint32_t cd_pal(uint32_t i)
{
    return ((i & 0xFFu) << 16) | (((255u - i) & 0xFFu) << 8) | ((i * 3u) & 0xFFu);
}

/* Layouts and formats as in aar/app.h (repeated so that the runner needs no more than
 * the numbers). */
#define CD_LAYOUT_RGB565_BE 1u
#define CD_LAYOUT_RGB565_LE 2u
#define CD_LAYOUT_BGR565_BE 3u
#define CD_LAYOUT_BGR565_LE 4u
#define CD_LAYOUT_BGRA8888  5u
#define CD_LAYOUT_RGBA8888  6u

static inline uint32_t cd_layout_bytes(uint32_t layout)
{
    return layout >= CD_LAYOUT_BGRA8888 && layout <= CD_LAYOUT_RGBA8888 ? 4u :
           (layout >= CD_LAYOUT_RGB565_BE && layout <= CD_LAYOUT_BGR565_LE ? 2u : 1u);
}

/* Stores the colour (8 bits each) as the layout says; for 565 layouts the low bits are
 * dropped. Returns the bytes stored (0 for a layout that is not a colour layout). */
static inline uint32_t cd_put_pixel(uint8_t *p, uint32_t layout, uint32_t r, uint32_t g,
                                    uint32_t b)
{
    uint32_t v;

    switch (layout) {
    case CD_LAYOUT_RGB565_BE:
    case CD_LAYOUT_RGB565_LE:
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        break;
    case CD_LAYOUT_BGR565_BE:
    case CD_LAYOUT_BGR565_LE:
        v = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
        break;
    case CD_LAYOUT_BGRA8888:
        p[0] = (uint8_t)b;
        p[1] = (uint8_t)g;
        p[2] = (uint8_t)r;
        p[3] = 0xFFu;
        return 4;
    case CD_LAYOUT_RGBA8888:
        p[0] = (uint8_t)r;
        p[1] = (uint8_t)g;
        p[2] = (uint8_t)b;
        p[3] = 0xFFu;
        return 4;
    default:
        return 0;
    }
    if (layout == CD_LAYOUT_RGB565_BE || layout == CD_LAYOUT_BGR565_BE) {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
    return 2;
}

/* Reads a pixel back as 8-bit components. Returns 0 for a layout that is not a colour layout. */
static inline int cd_get_pixel(const uint8_t *p, uint32_t layout, uint32_t *r, uint32_t *g,
                               uint32_t *b)
{
    uint32_t v;
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;

    if (layout == CD_LAYOUT_BGRA8888) {
        *b = p[0];
        *g = p[1];
        *r = p[2];
        return 1;
    }
    if (layout == CD_LAYOUT_RGBA8888) {
        *r = p[0];
        *g = p[1];
        *b = p[2];
        return 1;
    }
    if (layout < CD_LAYOUT_RGB565_BE || layout > CD_LAYOUT_BGR565_LE)
        return 0;
    v = (layout == CD_LAYOUT_RGB565_BE || layout == CD_LAYOUT_BGR565_BE) ?
            (((uint32_t)p[0] << 8) | p[1]) : (((uint32_t)p[1] << 8) | p[0]);
    c0 = (v >> 11) & 0x1Fu;
    c1 = (v >> 5) & 0x3Fu;
    c2 = v & 0x1Fu;
    c0 = (c0 << 3) | (c0 >> 2);
    c1 = (c1 << 2) | (c1 >> 4);
    c2 = (c2 << 3) | (c2 >> 2);
    if (layout == CD_LAYOUT_RGB565_BE || layout == CD_LAYOUT_RGB565_LE) {
        *r = c0;
        *g = c1;
        *b = c2;
    } else {
        *b = c0;
        *g = c1;
        *r = c2;
    }
    return 1;
}

/* The colour of quadrant q (0 top left, 1 top right, 2 bottom left, 3 bottom right). */
static inline void cd_quad_colour(uint32_t q, uint32_t *r, uint32_t *g, uint32_t *b)
{
    *r = q == 0 || q == 3 ? 255u : 0u;
    *g = q == 1 || q == 3 ? 255u : 0u;
    *b = q == 2 || q == 3 ? 255u : 0u;
}

_Static_assert(sizeof CD_K_PAD <= 20 && sizeof CD_K_FRAME <= 20 && sizeof CD_K_PALETTE <= 20,
               "a display keyword is longer than 19 characters");

#endif
