/*
 * The runner's own display table (r_gfx.h).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <exec/execbase.h>
#include <graphics/gfx.h>
#include <libraries/Picasso96.h>
#include <proto/Picasso96.h>

#include <stdio.h>
#include <string.h>

#include "r_gfx.h"

struct Library *P96Base;

struct gfx_display gfx;

static struct Library *p96_lib;

static ULONG bytes_per_pixel(ULONG format)
{
    return format == AAR_FMT_BGRA8888 ? 4UL : (format == AAR_FMT_CLUT8 ? 1UL : 2UL);
}

static void why_more(struct gfx_display *g, const char *more)
{
    char tmp[sizeof g->why];

    snprintf(tmp, sizeof tmp, "%.60s; %s", g->why, more);
    snprintf(g->why, sizeof g->why, "%s", tmp);
}

/* Pixel memory the platform accepts as a token. Fills mem, pitch, bytes, token, source. */
static int alloc_pixels(struct gfx_display *g, ULONG w, ULONG h, ULONG format)
{
    ULONG bpp = bytes_per_pixel(format);
    ULONG pitch = w * bpp + g->pad;
    ULONG bytes = pitch * h;
    UBYTE *mem;
    ULONG token = 0;
    LONG rc;

    g->why[0] = '\0';
    mem = AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (mem) {
        rc = AARSharedToken(g->h, mem, bytes, &token);
        if (rc == AAR_OK) {
            g->mem = mem;
            g->pitch = pitch;
            g->bytes = bytes;
            g->token = token;
            g->source = 1;
            return 1;
        }
        snprintf(g->why, sizeof g->why, "AARSharedToken refused Amiga memory (%ld)", (long)rc);
        FreeVec(mem);
    } else {
        snprintf(g->why, sizeof g->why, "no memory for %lu bytes of pixels", (unsigned long)bytes);
    }
    /* The platform's aperture is not Amiga memory: a bitmap of the RTG card is. */
    if (!p96_lib)
        p96_lib = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    if (p96_lib) {
        struct BitMap *bm;
        ULONG rgb = format == AAR_FMT_BGRA8888 ? RGBFB_B8G8R8A8 :
                    (format == AAR_FMT_CLUT8 ? RGBFB_CLUT : RGBFB_R5G6B5);

        P96Base = p96_lib;
        bm = p96AllocBitMap(w, h, bpp * 8UL, BMF_CLEAR, NULL, (RGBFTYPE)rgb);
        if (bm) {
            ULONG addr = p96GetBitMapAttr(bm, P96BMA_MEMORY);
            ULONG row = p96GetBitMapAttr(bm, P96BMA_BYTESPERROW);

            if (addr && row >= w * bpp &&
                AARSharedToken(g->h, (APTR)addr, row * h, &token) == AAR_OK) {
                g->mem = (UBYTE *)addr;
                g->pitch = row;
                g->bytes = row * h;
                g->token = token;
                g->bitmap = bm;
                g->source = 2;
                return 1;
            }
            p96FreeBitMap(bm);
            why_more(g, "the Picasso96 bitmap was refused too");
        } else {
            why_more(g, "no Picasso96 bitmap");
        }
    } else {
        why_more(g, "no Picasso96API.library");
    }
    return 0;
}

static void free_pixels(struct gfx_display *g)
{
    if (g->source == 1 && g->mem)
        FreeVec(g->mem);
    else if (g->source == 2 && g->bitmap && p96_lib) {
        P96Base = p96_lib;
        p96FreeBitMap((struct BitMap *)g->bitmap);
    }
    g->mem = NULL;
    g->bitmap = NULL;
    g->source = 0;
}

static void gfx_do_open(void *ctx, const struct aar_serve_req *req, struct aar_serve_done *done)
{
    struct gfx_display *g = (struct gfx_display *)ctx;
    ULONG w = req->arg[AAR_A_DISPLAY_OPEN_W];
    ULONG h = req->arg[AAR_A_DISPLAY_OPEN_H];
    ULONG format = req->arg[AAR_A_DISPLAY_OPEN_FORMAT];

    memset(done, 0, sizeof *done);
    if (g->open) {
        done->result = AAR_E_BUSY;
        return;
    }
    if (!g->h || !alloc_pixels(g, w, h, format)) {
        g->alloc_failed = 1;
        if (!g->h)
            snprintf(g->why, sizeof g->why, "the runner has no handle for AARSharedToken");
        done->result = AAR_E_NOMEM;
        return;
    }
    g->open = 1;
    g->opens++;
    g->last_source = g->source;
    g->width = w;
    g->height = h;
    g->format = format;
    /* The same layouts as the serving loop reports for each format. */
    g->layout = format == AAR_FMT_BGRA8888 ? AAR_LAYOUT_BGRA8888 :
                (format == AAR_FMT_CLUT8 ? AAR_LAYOUT_CLUT8 : AAR_LAYOUT_RGB565_BE);
    done->result = AAR_OK;
    done->out[AAR_R_DISPLAY_OPEN_PITCH] = g->pitch;
    done->out[AAR_R_DISPLAY_OPEN_FRONT] = g->token;
}

static void gfx_do_close(void *ctx)
{
    struct gfx_display *g = (struct gfx_display *)ctx;

    if (!g->open)
        return;
    free_pixels(g);
    g->open = 0;
    g->closes++;
}

/* (count << 16) | first, then three words per colour (left-justified in 32 bits), then 0. */
static void gfx_do_palette(void *ctx, const uint32_t *table)
{
    struct gfx_display *g = (struct gfx_display *)ctx;
    ULONG first = table[0] & 0xFFFFUL;
    ULONG count = table[0] >> 16;
    ULONG i;

    for (i = 0; i < count && first + i < 256UL; i++) {
        g->palette[first + i] = ((table[1 + i * 3] >> 24) << 16) |
                                ((table[2 + i * 3] >> 24) << 8) | (table[3 + i * 3] >> 24);
        g->palette_entries++;
    }
}

const struct aar_serve_display_ops gfx_ops = {
    .ctx = &gfx,
    .open = gfx_do_open,
    .load_rgb32 = gfx_do_palette,
    .close = gfx_do_close,
};

void gfx_sync(void)
{
    if (gfx.mem && gfx.bytes)
        CacheClearE(gfx.mem, gfx.bytes, CACRF_ClearD);
}

void gfx_reset(void)
{
    AARHandle *keep = gfx.h;

    gfx_do_close(&gfx);
    memset(&gfx, 0, sizeof gfx);
    gfx.h = keep;
    if (p96_lib) {
        CloseLibrary(p96_lib);
        p96_lib = NULL;
    }
}
