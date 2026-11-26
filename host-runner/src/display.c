/*
 * One display surface. Layouts match the ZZ9000 defaults: RGB565 is
 * big-endian in memory, BGRA8888 is byte order B,G,R,A, CLUT8 is indices.
 * Pitch is width times bytes per pixel; a platform may pad, so apps must use
 * surface.pitch. The surface grants AAR_DISPCAP_ASYNC and AAR_DISPCAP_CLUT8,
 * and AAR_DISPCAP_LATEST when the open asked for AAR_DISPLAY_LATEST.
 *
 * A synchronous flip presents the back buffer and swaps. flip_async in plain
 * mode stays pending until yield or vsync, so flip_idle can return 0, and a
 * flip or refresh while it is pending is AAR_E_BUSY. Latest mode keeps three
 * buffers: a flip before the pending frame is shown replaces that frame,
 * counts it as dropped, and returns a buffer to drawing at once, so flip_idle
 * stays 1.
 *
 * --frames N sets stop after N presented frames. --scale N sizes the window
 * only; the surface stays at the app's resolution.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct host_display {
    int open;
    uint32_t id;
    uint32_t w, h, pitch, format, layout, flags, caps;
    uint8_t *buf[3];
    int nbuf;
    int showing;
    int drawing;
    int pending;
    uint32_t dropped;
    int latest;
    uint32_t palette[256];
    uint8_t *convert;
    uint8_t *shown;
    uint32_t shown_bytes;
    uint32_t flips;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int win_x, win_y;
};

static struct host_display disp;

static uint32_t bpp_of(uint32_t format)
{
    if (format == AAR_FMT_RGB565)
        return 2u;
    if (format == AAR_FMT_BGRA8888)
        return 4u;
    if (format == AAR_FMT_CLUT8)
        return 1u;
    return 0u;
}

static uint32_t layout_of(uint32_t format)
{
    if (format == AAR_FMT_RGB565)
        return AAR_LAYOUT_RGB565_BE;
    if (format == AAR_FMT_BGRA8888)
        return AAR_LAYOUT_BGRA8888;
    if (format == AAR_FMT_CLUT8)
        return AAR_LAYOUT_CLUT8;
    return 0u;
}

static void expand565(uint16_t px, int bgr, uint8_t bgra[4])
{
    uint8_t r5 = (uint8_t)((px >> 11) & 31u);
    uint8_t g6 = (uint8_t)((px >> 5) & 63u);
    uint8_t b5 = (uint8_t)(px & 31u);
    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));

    if (bgr) {
        uint8_t t = r;
        r = b;
        b = t;
    }
    bgra[0] = b;
    bgra[1] = g;
    bgra[2] = r;
    bgra[3] = 255u;
}

static uint16_t load565(const uint8_t *p, int le)
{
    if (le)
        return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* dst is bytes B,G,R,A, the SDL ARGB8888 memory order on a little-endian host
 * and the order we upload on every host. */
static void convert_row(const uint8_t *src, uint32_t format, uint32_t layout,
                        uint32_t width, const uint32_t *palette, uint8_t *dst)
{
    uint32_t x;

    for (x = 0; x < width; x++) {
        uint8_t *px = dst + x * 4u;
        if (format == AAR_FMT_RGB565) {
            int le = layout == AAR_LAYOUT_RGB565_LE ||
                     layout == AAR_LAYOUT_BGR565_LE;
            int bgr = layout == AAR_LAYOUT_BGR565_BE ||
                      layout == AAR_LAYOUT_BGR565_LE;
            expand565(load565(src + x * 2u, le), bgr, px);
        } else if (format == AAR_FMT_BGRA8888 && layout == AAR_LAYOUT_RGBA8888) {
            px[0] = src[x * 4u + 2u];
            px[1] = src[x * 4u + 1u];
            px[2] = src[x * 4u];
            px[3] = src[x * 4u + 3u];
        } else if (format == AAR_FMT_BGRA8888) {
            px[0] = src[x * 4u];
            px[1] = src[x * 4u + 1u];
            px[2] = src[x * 4u + 2u];
            px[3] = src[x * 4u + 3u];
        } else {
            uint32_t pal = palette ? palette[src[x]] : 0u;
            px[0] = (uint8_t)pal;
            px[1] = (uint8_t)(pal >> 8);
            px[2] = (uint8_t)(pal >> 16);
            px[3] = 255u;
        }
    }
}

static int expect_bgra(const uint8_t *px, uint8_t b, uint8_t g, uint8_t r)
{
    return px[0] == b && px[1] == g && px[2] == r && px[3] == 255u;
}

int host_display_selftest(void)
{
    uint8_t dst[4];
    uint8_t be[2] = {0xf8u, 0x00u};
    uint8_t le[2] = {0x00u, 0xf8u};
    uint8_t bgr[2] = {0x00u, 0x1fu};
    uint8_t bgra[4] = {0x00u, 0x00u, 0xffu, 0xffu};
    uint8_t index = 7u;
    uint32_t pal[256];

    convert_row(be, AAR_FMT_RGB565, AAR_LAYOUT_RGB565_BE, 1u, 0, dst);
    if (!expect_bgra(dst, 0, 0, 255))
        return 1;
    convert_row(le, AAR_FMT_RGB565, AAR_LAYOUT_RGB565_LE, 1u, 0, dst);
    if (!expect_bgra(dst, 0, 0, 255))
        return 2;
    convert_row(bgr, AAR_FMT_RGB565, AAR_LAYOUT_BGR565_BE, 1u, 0, dst);
    if (!expect_bgra(dst, 0, 0, 255))
        return 3;
    convert_row(bgra, AAR_FMT_BGRA8888, AAR_LAYOUT_BGRA8888, 1u, 0, dst);
    if (!expect_bgra(dst, 0, 0, 255))
        return 4;
    memset(pal, 0, sizeof pal);
    pal[7] = 0x0000ff00u;
    convert_row(&index, AAR_FMT_CLUT8, AAR_LAYOUT_CLUT8, 1u, pal, dst);
    if (!expect_bgra(dst, 0, 255, 0))
        return 5;
    return 0;
}

static void count_flip(void)
{
    const struct host_options *opt = host_options();

    disp.flips++;
    if (opt->frames_limit && disp.flips >= opt->frames_limit)
        host_request_stop();
}

static void present_buffer(int index)
{
    const struct host_options *opt = host_options();
    uint32_t y;
    uint8_t *src = disp.buf[index];

    if (!src)
        return;
    if (disp.shown && disp.shown_bytes)
        memcpy(disp.shown, src, disp.shown_bytes);
    if (!disp.convert || !disp.tex)
        return;
    for (y = 0; y < disp.h; y++) {
        convert_row(src + y * disp.pitch, disp.format, disp.layout, disp.w,
                    disp.palette, disp.convert + y * disp.w * 4u);
    }
    SDL_UpdateTexture(disp.tex, NULL, disp.convert, (int)(disp.w * 4u));
    SDL_RenderClear(disp.ren);
    {
        SDL_Rect dst;
        dst.x = 0;
        dst.y = 0;
        dst.w = (int)disp.w * opt->scale;
        dst.h = (int)disp.h * opt->scale;
        SDL_RenderCopy(disp.ren, disp.tex, NULL, &dst);
    }
    SDL_RenderPresent(disp.ren);
}

static int spare_buffer(void)
{
    int i;

    for (i = 0; i < disp.nbuf; i++) {
        if (i != disp.showing && i != disp.drawing && i != disp.pending)
            return i;
    }
    return disp.drawing;
}

static void complete_pending(void)
{
    int was;

    if (!disp.open || disp.pending < 0)
        return;
    was = disp.pending;
    present_buffer(was);
    disp.showing = was;
    disp.pending = -1;
    if (!disp.latest)
        disp.drawing = disp.drawing == was ? (was ^ 1) : disp.drawing;
    count_flip();
}

static void fill_surface(aar_surface *out)
{
    memset(out, 0, sizeof *out);
    out->id = disp.id;
    out->width = disp.w;
    out->height = disp.h;
    out->pitch = disp.pitch;
    out->format = disp.format;
    out->layout = disp.layout;
    out->caps = disp.caps;
    out->front = disp.buf[disp.showing];
    out->back = disp.buf[disp.drawing];
}

/*
 * The window of the surface, in one of two forms. The software form shows the
 * converted surface through a renderer and a texture; a headless run has only
 * a hidden window, because nothing would see the picture. The GL form has an
 * OpenGL pixel format for the context of hostgl.c, and nothing else: GL draws
 * into the window itself, at the size of the surface (--scale does not apply).
 * A window cannot have both, so a bind replaces one with the other.
 */
static int create_window(int gl)
{
    const struct host_options *opt = host_options();
    int scale = gl ? 1 : opt->scale;

    disp.win = SDL_CreateWindow("aar-host", disp.win_x, disp.win_y,
                                (int)disp.w * scale, (int)disp.h * scale,
                                (opt->headless ? SDL_WINDOW_HIDDEN : 0) |
                                    (gl ? SDL_WINDOW_OPENGL : 0));
    return disp.win ? 0 : -1;
}

static int create_software(void)
{
    const struct host_options *opt = host_options();

    if (create_window(0) != 0)
        return -1;
    if (opt->headless)
        return 0;
    disp.ren = SDL_CreateRenderer(
        disp.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!disp.ren)
        disp.ren = SDL_CreateRenderer(disp.win, -1, SDL_RENDERER_SOFTWARE);
    if (!disp.ren)
        return -1;
    disp.tex = SDL_CreateTexture(disp.ren, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, (int)disp.w,
                                 (int)disp.h);
    return disp.tex ? 0 : -1;
}

/* Remembers where the window was, so that a replacement opens there. */
static void destroy_window(void)
{
    if (disp.win && !host_options()->headless)
        SDL_GetWindowPosition(disp.win, &disp.win_x, &disp.win_y);
    if (disp.tex)
        SDL_DestroyTexture(disp.tex);
    if (disp.ren)
        SDL_DestroyRenderer(disp.ren);
    if (disp.win)
        SDL_DestroyWindow(disp.win);
    disp.tex = 0;
    disp.ren = 0;
    disp.win = 0;
}

static int api_display_open(uint32_t w, uint32_t h, uint32_t format,
                            uint32_t flags, aar_surface *out)
{
    uint32_t bpp;
    uint32_t pitch;
    uint64_t bytes;
    int i;
    if (host_stop_requested())
        return AAR_E_IO;

    if (!out)
        return AAR_E_INVAL;
    if (disp.open)
        return AAR_E_BUSY;
    bpp = bpp_of(format);
    if (!w || !h || !bpp || w > AAR_DISPLAY_MAX_DIM || h > AAR_DISPLAY_MAX_DIM)
        return AAR_E_INVAL;
    if ((flags & ~(AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST)) != 0u)
        return AAR_E_INVAL;
    pitch = w * bpp;
    bytes = (uint64_t)pitch * h;
    if (bytes == 0u || bytes > (64u << 20))
        return AAR_E_NOMEM;
    if (!host_sdl_video_ok())
        return AAR_E_IO;
    free(disp.shown);
    free(disp.convert);
    memset(&disp, 0, sizeof disp);
    disp.pending = -1;
    disp.w = w;
    disp.h = h;
    disp.pitch = pitch;
    disp.format = format;
    disp.layout = layout_of(format);
    disp.flags = flags;
    disp.latest = (flags & AAR_DISPLAY_LATEST) != 0u;
    disp.caps = AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8 |
                (disp.latest ? AAR_DISPCAP_LATEST : 0u);
    disp.nbuf = disp.latest ? 3 : 2;
    disp.showing = 0;
    disp.drawing = 1;
    disp.id = 1u;
    disp.shown_bytes = (uint32_t)bytes;
    for (i = 0; i < 256; i++)
        disp.palette[i] = (uint32_t)i << 16 | (uint32_t)i << 8 | (uint32_t)i;
    for (i = 0; i < disp.nbuf; i++) {
        disp.buf[i] = (uint8_t *)calloc(1, (size_t)bytes);
        if (!disp.buf[i])
            goto fail;
    }
    disp.shown = (uint8_t *)calloc(1, (size_t)bytes);
    disp.convert = (uint8_t *)malloc((size_t)w * h * 4u);
    if (!disp.shown || !disp.convert)
        goto fail;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    disp.win_x = SDL_WINDOWPOS_UNDEFINED;
    disp.win_y = SDL_WINDOWPOS_UNDEFINED;
    if (create_software() != 0)
        goto fail;
    disp.open = 1;
    fill_surface(out);
    return AAR_OK;
fail:
    for (i = 0; i < 3; i++)
        free(disp.buf[i]);
    free(disp.shown);
    free(disp.convert);
    destroy_window();
    memset(&disp, 0, sizeof disp);
    disp.pending = -1;
    return AAR_E_IO;
}

static int valid_id(uint32_t id)
{
    return disp.open && id == disp.id;
}

static int api_display_flip(uint32_t surface_id)
{
    if (host_stop_requested())
        return AAR_E_IO;
    if (!valid_id(surface_id))
        return AAR_E_INVAL;
    if (disp.pending >= 0) {
        if (!disp.latest)
            return AAR_E_BUSY;
        /* Latest mode: this frame replaces the one still pending. */
        disp.dropped++;
        disp.pending = -1;
    }
    present_buffer(disp.drawing);
    if (host_stop_requested())
        return AAR_E_IO;
    disp.showing = disp.drawing;
    /*
     * Pick the next drawing buffer from the live rotation (showing /
     * pending / free). A fixed `drawing ^= 1` assumes two buffers and
     * produces index 3 after a latest-mode async flip left drawing at 2.
     */
    disp.drawing = spare_buffer();
    if (disp.drawing == disp.showing)
        disp.drawing = (disp.showing + 1) % disp.nbuf;
    count_flip();
    return AAR_OK;
}

static int api_display_flip_async(uint32_t surface_id)
{
    int old;

    if (!valid_id(surface_id))
        return AAR_E_INVAL;
    if (!disp.latest) {
        if (disp.pending >= 0)
            return AAR_E_BUSY;
        disp.pending = disp.drawing;
        return AAR_OK;
    }
    if (disp.pending >= 0)
        disp.dropped++;
    old = disp.pending;
    disp.pending = disp.drawing;
    disp.drawing = spare_buffer();
    if (disp.drawing == disp.pending)
        disp.drawing = old >= 0 ? old : (disp.showing ^ 1);
    return AAR_OK;
}

static int api_display_flip_idle(void)
{
    if (!disp.open)
        return AAR_E_INVAL;
    if (disp.latest)
        return 1;
    return disp.pending < 0;
}

static int api_display_refresh(aar_surface *surface)
{
    if (!surface || !disp.open || surface->id != disp.id)
        return AAR_E_INVAL;
    if (!disp.latest && disp.pending >= 0)
        return AAR_E_BUSY;
    fill_surface(surface);
    return AAR_OK;
}

static int api_display_set_palette(uint32_t surface_id, uint32_t first,
                                   uint32_t count, const uint32_t *rgb)
{
    uint32_t i;

    if (!valid_id(surface_id))
        return AAR_E_INVAL;
    if (disp.format != AAR_FMT_CLUT8)
        return AAR_E_INVAL;
    if (!rgb || count == 0u || first > 255u || count > 256u - first)
        return AAR_E_INVAL;
    for (i = 0; i < count; i++)
        disp.palette[first + i] = rgb[i];
    return AAR_OK;
}

static uint32_t api_display_latest_dropped(void)
{
    if (!disp.open || !disp.latest)
        return 0u;
    return disp.dropped;
}

static int api_display_vsync(void)
{
    if (!disp.open)
        return AAR_E_INVAL;
    host_display_pump();
    return AAR_OK;
}

static int api_display_close(uint32_t surface_id)
{
    int i;

    if (!valid_id(surface_id))
        return AAR_E_INVAL;
    host_gl_release();
    destroy_window();
    for (i = 0; i < 3; i++) {
        free(disp.buf[i]);
        disp.buf[i] = 0;
    }
    free(disp.convert);
    disp.convert = 0;
    disp.open = 0;
    disp.pending = -1;
    return AAR_OK;
}

/*
 * The GL side (hostgl.c). gl_open swaps the window of the open surface for one
 * with an OpenGL pixel format and returns it with its size. gl_close puts the
 * software window back and shows the surface again. gl_swapped counts a
 * presented frame, as a flip does. gl_store copies a frame that GL drew (RGBA,
 * bottom row first) into the surface, in the format of the surface, so that
 * --dump-front and a read of the front buffer see it. A CLUT8 surface has no
 * colour to store.
 */
int host_display_gl_open(uint32_t surface_id, void **window, int32_t *w,
                         int32_t *h)
{
    if (!valid_id(surface_id))
        return -1;
    destroy_window();
    if (create_window(1) != 0) {
        create_software();
        return -1;
    }
    *window = disp.win;
    *w = (int32_t)disp.w;
    *h = (int32_t)disp.h;
    return 0;
}

void host_display_gl_close(void)
{
    if (!disp.open)
        return;
    destroy_window();
    if (create_software() != 0) {
        fprintf(stderr, "aar-host: cannot restore the window: %s\n",
                SDL_GetError());
        return;
    }
    present_buffer(disp.showing);
}

void host_display_gl_swapped(void)
{
    count_flip();
}

void host_display_gl_store(const uint8_t *rgba, uint32_t w, uint32_t h)
{
    uint8_t *dst = disp.open ? disp.buf[disp.showing] : 0;
    uint32_t x, y;

    if (!dst || !disp.shown || w != disp.w || h != disp.h ||
        disp.format == AAR_FMT_CLUT8)
        return;
    for (y = 0; y < h; y++) {
        const uint8_t *src = rgba + (size_t)(h - 1u - y) * w * 4u;
        uint8_t *row = dst + (size_t)y * disp.pitch;

        for (x = 0; x < w; x++) {
            if (disp.format == AAR_FMT_RGB565) {
                row[x * 2u] = (uint8_t)((src[x * 4u] & 0xf8u) |
                                        (src[x * 4u + 1u] >> 5));
                row[x * 2u + 1u] = (uint8_t)(((src[x * 4u + 1u] << 3) & 0xe0u) |
                                             (src[x * 4u + 2u] >> 3));
            } else {
                row[x * 4u] = src[x * 4u + 2u];
                row[x * 4u + 1u] = src[x * 4u + 1u];
                row[x * 4u + 2u] = src[x * 4u];
                row[x * 4u + 3u] = 255u;
            }
        }
    }
    memcpy(disp.shown, dst, disp.shown_bytes);
}

void host_display_pump(void)
{
    if (!disp.open)
        return;
    if (disp.pending >= 0)
        complete_pending();
}

void host_display_dump(const char *path)
{
    FILE *fp;
    const uint8_t *src;
    uint32_t n;

    if (!path)
        return;
    src = disp.shown;
    n = disp.shown_bytes;
    if ((!src || n == 0u) && disp.buf[disp.showing]) {
        src = disp.buf[disp.showing];
        n = disp.pitch * disp.h;
    }
    if (!src || n == 0u)
        return;
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "aar-host: cannot write %s\n", path);
        return;
    }
    if (fwrite(src, 1, n, fp) != n)
        fprintf(stderr, "aar-host: short write %s\n", path);
    fclose(fp);
}

uint32_t host_display_flips(void)
{
    return disp.flips;
}

int host_display_init(void)
{
    disp.pending = -1;
    return 0;
}

void host_display_shutdown(void)
{
    if (disp.open)
        api_display_close(disp.id);
    free(disp.shown);
    disp.shown = 0;
    free(disp.convert);
    disp.convert = 0;
}

void host_display_bind(aar_api *api)
{
    api->display_open = api_display_open;
    api->display_flip = api_display_flip;
    api->display_flip_async = api_display_flip_async;
    api->display_flip_idle = api_display_flip_idle;
    api->display_refresh = api_display_refresh;
    api->display_vsync = api_display_vsync;
    api->display_set_palette = api_display_set_palette;
    api->display_close = api_display_close;
    api->display_latest_dropped = api_display_latest_dropped;
}
