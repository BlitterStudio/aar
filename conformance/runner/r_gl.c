/*
 * Runner side of C-GL (conformance/TESTS.md): the GL payload runs with the runner's own
 * display table (r_gfx.c), so the runner can read the frame that aar_gl_swap presents: the
 * peer keyword CONF-GL-PIXEL returns a pixel of the client's copy (C-GL.5). Everything else
 * is judged by the payload (templates/mgl_conformance.c adapted). The handle for the display
 * table is recorded by r_display.c for every run.
 *
 * Assumptions for the card run: the swap completes after the platform has copied the frame
 * into the client's pixel memory (as the flip does for C-DISPLAY.12); the long texture
 * matrix (8400 cases) finishes within the run's timeout (10 minutes).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "r_gfx.h"
#include "../payloads/conf_display.h"
#include "../payloads/conf_gl.h"

static void send_result(struct conf_run *run, ULONG ok, ULONG count)
{
    AARMessage am;
    int tries;

    cm_make_result(am.bytes, ok, count, 0, CM_NONE, 0, 0);
    for (tries = 0; tries < 100; tries++) {
        if (AARSendMessage(run->h, &am) != AAR_E_BUSY)
            return;
        Delay(1);
    }
}

static void peer_pixel(struct conf_run *run, const AARMessage *msg)
{
    ULONG x = cm_get32(msg->bytes, 20);
    ULONG y = cm_get32(msg->bytes, 24);
    ULONG r = 0, g = 0, b = 0;
    ULONG bpp;

    if (!gfx.open || !gfx.mem || x >= gfx.width || y >= gfx.height) {
        send_result(run, 0, 0);
        return;
    }
    bpp = gfx.format == AAR_FMT_BGRA8888 ? 4UL : (gfx.format == AAR_FMT_CLUT8 ? 1UL : 2UL);
    gfx_sync();
    if (!cd_get_pixel(gfx.mem + y * gfx.pitch + x * bpp, gfx.layout, &r, &g, &b)) {
        send_result(run, 0, 0);
        return;
    }
    send_result(run, 1, (r << 16) | (g << 8) | b);
}

static const struct conf_peer peers[] = {
    { CG_K_PIXEL, peer_pixel },
};

static void prep_std(struct conf_run *run)
{
    gfx_reset();
    run->display = &gfx_ops;
    run->timeout_s = 600;
}

static const struct conf_variant variants[] = {
    { "std", prep_std },
};

static void on_end(const struct conf_run *run, const struct conf_outcome *out,
                   const struct conf_parsed *pr)
{
    (void)out;
    (void)pr;
    if (strcmp(run->area, "C-GL") != 0)
        return;
    if (gfx.alloc_failed)
        conf_emitf("# C-GL: the runner's display could not get pixel memory the platform "
                   "accepts: %.100s", gfx.why);
    gfx_reset();
}

const struct conf_area conf_area_gl = {
    .name = "C-GL",
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
    .on_end = on_end,
};
