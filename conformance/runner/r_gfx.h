/*
 * The runner's own display: an aar_serve display table (struct aar_serve_display_ops)
 * that gives the app pixel memory owned by the runner. The runner names that memory to
 * the platform with AARSharedToken, so every frame the app presents lands in memory the
 * runner reads, with no screen involved. One buffer: the platform copies each presented
 * frame into it, which is the mode the serving loop calls "single buffer". The C-DISPLAY
 * and C-GL rows use it (r_display.c, r_gl.c).
 *
 * Pixel memory comes from AllocVec when the platform accepts its address as a token,
 * else from a Picasso96 bitmap (the memory of the RTG card, for a platform whose shared
 * aperture is that card's memory: the ZZ9000).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_R_GFX_H
#define AAR_CONF_R_GFX_H

#include "aar-conf.h"

struct gfx_display {
    AARHandle *h;            /* the handle of the run being served (r_display.c on_open) */
    ULONG pad;               /* bytes added to each row of AllocVec memory */
    int open;
    ULONG width, height, format, layout, pitch, bytes;
    UBYTE *mem;
    int source;              /* 0 none, 1 AllocVec, 2 Picasso96 bitmap */
    int last_source;         /* the source of the last open that worked */
    void *bitmap;
    ULONG token;
    ULONG opens, closes;
    ULONG palette[256];      /* 0x00RRGGBB of each entry */
    ULONG palette_entries;   /* entries received (each LoadRGB32 update counts its own) */
    int alloc_failed;
    char why[112];           /* why the last open failed */
};

extern struct gfx_display gfx;
extern const struct aar_serve_display_ops gfx_ops;

/* Frees anything still open and clears the state (between runs). */
void gfx_reset(void);

/* Makes the frame in the pixel memory current for the CPU. Call before reading it. */
void gfx_sync(void);

#endif
