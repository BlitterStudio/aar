/*
 * Runner side of C-DISPLAY (conformance/TESTS.md): the client's half of the display
 * group. The runner supplies its own display table to aar_serve (r_gfx.c): pixel memory
 * that it owns and names with AARSharedToken, and a palette it records. The payload
 * presents frames and asks, with the keywords of payloads/conf_display.h, whether what
 * arrived in that memory is what it drew (C-DISPLAY.12 to C-DISPLAY.15). The picture
 * (C-DISPLAY.17) is judged by the operator, with the built-in screen of aar_serve
 * (variant picture, INTERACTIVE only).
 *
 * This file also records the handle of every run for r_gfx.c (AARSharedToken needs it),
 * so the C-GL run, which uses the same display table, finds it.
 *
 * Assumptions for the card run: a presented frame is in the client's pixel memory when the
 * flip request completes; the platform copies the frame with the pitch that the client's
 * open reply gave; the platform grants a display backed by memory the runner supplies
 * (AARSharedToken accepts it).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "r_gfx.h"
#include "../payloads/conf_display.h"

static void send_result(struct conf_run *run, ULONG ok, ULONG count, ULONG errors)
{
    AARMessage am;
    int tries;

    cm_make_result(am.bytes, ok, count, errors, CM_NONE, 0, 0);
    for (tries = 0; tries < 100; tries++) {
        if (AARSendMessage(run->h, &am) != AAR_E_BUSY)
            return;
        Delay(1);
    }
}

static ULONG row_bytes(void)
{
    return gfx.width * (gfx.format == AAR_FMT_BGRA8888 ? 4UL : (gfx.format == AAR_FMT_CLUT8 ? 1UL : 2UL));
}

/* Bytes of the frame that are not cd_byte(k, i, y). */
static ULONG diff_bytes(ULONG k)
{
    ULONG rowbytes = row_bytes();
    ULONG y, i, bad = 0;

    for (y = 0; y < gfx.height; y++) {
        for (i = 0; i < rowbytes; i++) {
            if (gfx.mem[y * gfx.pitch + i] != cd_byte(k, i, y))
                bad++;
        }
    }
    return bad;
}

/* Bytes that are not cd_row(y). */
static ULONG diff_rows(void)
{
    ULONG rowbytes = row_bytes();
    ULONG y, i, bad = 0;

    for (y = 0; y < gfx.height; y++) {
        for (i = 0; i < rowbytes; i++) {
            if (gfx.mem[y * gfx.pitch + i] != cd_row(y))
                bad++;
        }
    }
    return bad;
}

/* Pixels that do not decode to the colour of their quadrant. The layout the app was told
 * must be the one the client's open reply carried. */
static ULONG diff_quads(ULONG layout)
{
    ULONG bpp = row_bytes() / gfx.width;
    ULONG x, y, bad = 0;

    if (layout != gfx.layout)
        return gfx.width * gfx.height;
    for (y = 0; y < gfx.height; y++) {
        for (x = 0; x < gfx.width; x++) {
            ULONG r, g, b, er, eg, eb;

            cd_quad_colour((y >= gfx.height / 2UL ? 2UL : 0UL) + (x >= gfx.width / 2UL ? 1UL : 0UL),
                           &er, &eg, &eb);
            if (!cd_get_pixel(gfx.mem + y * gfx.pitch + x * bpp, layout, &r, &g, &b) ||
                r != er || g != eg || b != eb)
                bad++;
        }
    }
    return bad;
}

static void peer_pad(struct conf_run *run, const AARMessage *msg)
{
    (void)run;
    gfx.pad = cm_get32(msg->bytes, 20);
}

static void peer_frame(struct conf_run *run, const AARMessage *msg)
{
    ULONG pattern = cm_get32(msg->bytes, 20);
    ULONG b = cm_get32(msg->bytes, 24);
    ULONG layout = cm_get32(msg->bytes, 28);
    ULONG bad;

    if (!gfx.open || !gfx.mem) {
        send_result(run, 0, 1, 0);
        return;
    }
    gfx_sync();
    switch (pattern) {
    case CD_PAT_BYTES:
        bad = diff_bytes(b);
        break;
    case CD_PAT_QUADS:
        bad = diff_quads(layout);
        break;
    case CD_PAT_ROWS:
        bad = diff_rows();
        break;
    default:
        bad = 1;
        break;
    }
    send_result(run, bad == 0, bad, 0);
}

static void peer_palette(struct conf_run *run, const AARMessage *msg)
{
    ULONG i, bad = 0;

    (void)msg;
    for (i = 0; i < 256UL; i++) {
        if (gfx.palette[i] != cd_pal(i))
            bad++;
    }
    send_result(run, bad == 0 && gfx.palette_entries >= 256UL, gfx.palette_entries, bad);
}

static const struct conf_peer peers[] = {
    { CD_K_PAD, peer_pad },
    { CD_K_FRAME, peer_frame },
    { CD_K_PALETTE, peer_palette },
};

/* Every run: the display table needs the handle of the run it serves. */
static void on_open(const struct conf_run *run, LONG rc, const AARStatusInfo *st)
{
    (void)rc;
    (void)st;
    gfx.h = run->h;
}

/* The runner's table serves CLUT8 surfaces and palettes, and the platform grants CLUT8 only to a
 * client that says so (AAR_TAG_CLIENT_CAPS): without it C-DISPLAY.15 would pass without a palette. */
static const struct TagItem client_caps[] = {
    { AAR_TAG_CLIENT_CAPS, AAR_CCAP_DISPLAY_CLUT8 },
    { TAG_DONE, 0 }
};

static void prep_std(struct conf_run *run)
{
    gfx_reset();
    run->display = &gfx_ops;
    run->tags = client_caps;
    run->timeout_s = 120;
}

static void prep_picture(struct conf_run *run)
{
    gfx_reset();
    if (!conf_g.interactive)
        return; /* the payload ends at once without conf:show */
    run->extra_args[run->extra_count++] = "conf:show=1";
    conf_emitf("# C-DISPLAY.17: for five seconds the screen shows four colours: red top left, green "
               "top right, blue bottom left, white bottom right");
}

static const struct conf_operator operators[] = {
    { "C-DISPLAY.17", "picture", NULL, NULL },
};

static const struct conf_variant variants[] = {
    { "std", prep_std },
    { "picture", prep_picture },
};

static void on_end(const struct conf_run *run, const struct conf_outcome *out,
                   const struct conf_parsed *pr)
{
    (void)out;
    (void)pr;
    if (strcmp(run->area, "C-DISPLAY") != 0 || !run->variant || strcmp(run->variant, "std") != 0)
        return;
    if (gfx.alloc_failed)
        conf_emitf("# C-DISPLAY: the runner's display could not get pixel memory the platform "
                   "accepts: %.100s", gfx.why);
    conf_emitf("# C-DISPLAY: the runner's display was opened %lu times, pixels from %s",
               (unsigned long)gfx.opens, gfx.last_source == 2 ? "a Picasso96 bitmap" : "Amiga memory");
    gfx_reset();
}

const struct conf_area conf_area_display = {
    .name = "C-DISPLAY",
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
    .operators = operators,
    .operator_count = CONF_COUNT(operators),
    .on_open = on_open,
    .on_end = on_end,
};
