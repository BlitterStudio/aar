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
 * The runner always shows the surface in a window, so AAR_DISPLAY_WINDOW is
 * granted. AAR_DISPLAY_RESIZABLE makes the SDL window resizable: a resize
 * becomes AAR_EV_RESIZE (input.c) and aar_display_resize reallocates the
 * buffers at the size the app asks for. AAR_DISPLAY_SCALE shows the frame
 * through SDL_RenderSetLogicalSize, which keeps the aspect ratio; a GL window
 * is not scaled. The title, pointer and vertical blank estimate are in the
 * calls below. The other flags (pointer position, text input, close request)
 * are honoured by input.c, which reads the granted caps from here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"
#include "aar/detail/present_feedback.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_DISPLAY_FLAGS \
    (AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST | AAR_DISPLAY_WINDOW | \
     AAR_DISPLAY_RESIZABLE | AAR_DISPLAY_POINTER_ABS | \
     AAR_DISPLAY_TEXT_INPUT | AAR_DISPLAY_CLOSE_REQUEST | AAR_DISPLAY_SCALE)
#define HOST_TITLE_MAX 255u
#define HOST_MODE_FORMATS \
    ((1u << AAR_FMT_RGB565) | (1u << AAR_FMT_BGRA8888) | (1u << AAR_FMT_CLUT8))

struct host_display {
    int open;
    uint32_t id;
    uint32_t w, h, pitch, format, layout, flags, caps;
    uint8_t *buf[3];
    int nbuf;
    int showing;
    int drawing;
    int pending;
    uint64_t pending_id;
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
    uint32_t tex_w, tex_h;
    int tex_valid;       /* the texture holds a frame */
    int win_x, win_y;
    int gl;              /* the window is the GL form */
    uint32_t window_id;
    int active;          /* the window has input focus */
    uint32_t reported_w; /* the surface size of the last AAR_EV_RESIZE */
    uint32_t reported_h;
    uint64_t opened_us;
    uint64_t presented_us;
    char title[HOST_TITLE_MAX + 1u];
};

static struct host_display disp;
/* Frame IDs and unread results belong to the app, not the open surface. */
static aar_present_queue presents;
static SDL_Cursor *cursor_custom;
static SDL_Cursor *cursor_busy;

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

/* Window pixels per surface pixel. A scaled (logical size) window and a GL
 * window show the surface 1:1 in the coordinates that SDL reports. */
static int window_scale(void)
{
    return disp.gl ? 1 : host_options()->scale;
}

static int logical_size_in_use(void)
{
    return !disp.gl && (disp.caps & AAR_DISPCAP_SCALED) != 0u;
}

static void finish_tracked(uint64_t id, uint32_t outcome, uint32_t reason)
{
    if (id)
        aar_present_finish(&presents, id, outcome, reason, host_time_us());
}

void host_display_present_complete(uint64_t id, uint32_t reason)
{
    const struct host_options *opt;
    uint64_t now;

    if (reason != AAR_PRESENT_REASON_NONE) {
        finish_tracked(id, AAR_PRESENT_UNCONFIRMED, reason);
        return;
    }
    now = host_time_us();
    if (id)
        aar_present_finish(&presents, id, AAR_PRESENT_ACKNOWLEDGED,
                           AAR_PRESENT_REASON_NONE, now);
    opt = host_options();

    disp.flips++;
    disp.presented_us = now ? now : 1u;
    if (opt->frames_limit && disp.flips >= opt->frames_limit)
        host_request_stop();
}

/* shown is the last presented frame, for --dump-front. It has the size of the
 * surface that was shown, which is not the surface's size between a resize
 * and the next frame. */
static void ensure_shown(void)
{
    uint32_t bytes = disp.pitch * disp.h;

    if (disp.shown && disp.shown_bytes == bytes)
        return;
    free(disp.shown);
    disp.shown = (uint8_t *)calloc(1, bytes);
    disp.shown_bytes = disp.shown ? bytes : 0u;
}

static void ensure_texture(void)
{
    if (disp.tex && disp.tex_w == disp.w && disp.tex_h == disp.h)
        return;
    if (disp.tex)
        SDL_DestroyTexture(disp.tex);
    disp.tex = SDL_CreateTexture(disp.ren, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, (int)disp.w,
                                 (int)disp.h);
    disp.tex_w = disp.tex ? disp.w : 0u;
    disp.tex_h = disp.tex ? disp.h : 0u;
    disp.tex_valid = 0;
}

static int draw_texture(void)
{
    SDL_Rect dst;
    int scale = logical_size_in_use() ? 1 : host_options()->scale;

    dst.x = 0;
    dst.y = 0;
    dst.w = (int)disp.tex_w * scale;
    dst.h = (int)disp.tex_h * scale;
    if (SDL_RenderClear(disp.ren) != 0 ||
        SDL_RenderCopy(disp.ren, disp.tex, NULL, &dst) != 0)
        return -1;
    /* SDL2 has no return value here, but some drivers set an error. */
    SDL_ClearError();
    SDL_RenderPresent(disp.ren);
    return *SDL_GetError() ? -1 : 0;
}

/* The window shows the last frame again, after an expose or a size change. */
static void redraw(void)
{
    if (disp.open && !disp.gl && disp.ren && disp.tex && disp.tex_valid)
        (void)draw_texture();
}

static void remember_buffer(int index)
{
    ensure_shown();
    if (disp.shown)
        memcpy(disp.shown, disp.buf[index], disp.shown_bytes);
}

static int present_buffer(int index)
{
    uint32_t y;
    uint8_t *src = disp.buf[index];

    if (!src || !disp.win || disp.gl)
        return -1;
    /* A headless software window has no renderer: the runner is the sink. */
    if (host_options()->headless)
        return 0;
    if (!disp.convert || !disp.ren)
        return -1;
    ensure_texture();
    if (!disp.tex)
        return -1;
    for (y = 0; y < disp.h; y++) {
        convert_row(src + y * disp.pitch, disp.format, disp.layout, disp.w,
                    disp.palette, disp.convert + y * disp.w * 4u);
    }
    disp.tex_valid = 0;
    if (SDL_UpdateTexture(disp.tex, NULL, disp.convert, (int)(disp.w * 4u)) != 0 ||
        draw_texture() != 0)
        return -1;
    disp.tex_valid = 1;
    return 0;
}

static int present_pixels(int index, uint64_t id, int check_stop)
{
    int shown = present_buffer(index) == 0;
    int stopped = check_stop && host_stop_requested();

    host_display_present_complete(id, shown ? AAR_PRESENT_REASON_NONE
                                            : AAR_PRESENT_REASON_BACKEND);
    if (shown)
        remember_buffer(index);
    return stopped ? AAR_E_IO : AAR_OK;
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

static uint64_t take_pending(void)
{
    uint64_t id = disp.pending_id;

    disp.pending = -1;
    disp.pending_id = 0u;
    return id;
}

static void cancel_pending(void)
{
    finish_tracked(take_pending(), AAR_PRESENT_UNCONFIRMED,
                   AAR_PRESENT_REASON_CANCELLED);
}

void host_display_cancel_tracked(void)
{
    if (disp.pending_id)
        cancel_pending();
}

static void complete_pending(void)
{
    int was = disp.pending;
    uint64_t id = take_pending();

    (void)present_pixels(was, id, 0);
    disp.showing = was;
    if (!disp.latest)
        disp.drawing = disp.drawing == was ? (was ^ 1) : disp.drawing;
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
    uint32_t flags = (opt->headless ? SDL_WINDOW_HIDDEN : 0) |
                     (gl ? SDL_WINDOW_OPENGL : 0) |
                     ((disp.caps & AAR_DISPCAP_RESIZE) ? SDL_WINDOW_RESIZABLE : 0);

    disp.win = SDL_CreateWindow(disp.title, disp.win_x, disp.win_y,
                                (int)disp.w * scale, (int)disp.h * scale,
                                flags);
    if (!disp.win)
        return -1;
    disp.window_id = SDL_GetWindowID(disp.win);
    return 0;
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
    if (logical_size_in_use())
        SDL_RenderSetLogicalSize(disp.ren, (int)disp.w, (int)disp.h);
    ensure_texture();
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
    disp.tex_w = 0u;
    disp.tex_h = 0u;
    disp.tex_valid = 0;
    disp.ren = 0;
    disp.win = 0;
    disp.window_id = 0u;
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
    if ((flags & ~(uint32_t)HOST_DISPLAY_FLAGS) != 0u)
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
    /* The host shows the surface in a window, so WINDOW is always granted
     * when asked for. RESIZABLE needs it. */
    disp.caps = AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8 |
                AAR_DISPCAP_PRESENT_FEEDBACK |
                (disp.latest ? AAR_DISPCAP_LATEST : 0u);
    if (flags & AAR_DISPLAY_WINDOW) {
        disp.caps |= AAR_DISPCAP_WINDOW;
        if (flags & AAR_DISPLAY_RESIZABLE)
            disp.caps |= AAR_DISPCAP_RESIZE;
    }
    if (flags & AAR_DISPLAY_POINTER_ABS)
        disp.caps |= AAR_DISPCAP_POINTER_ABS;
    if (flags & AAR_DISPLAY_TEXT_INPUT)
        disp.caps |= AAR_DISPCAP_TEXT_INPUT;
    if (flags & AAR_DISPLAY_CLOSE_REQUEST)
        disp.caps |= AAR_DISPCAP_CLOSE_REQUEST;
    if (flags & AAR_DISPLAY_SCALE)
        disp.caps |= AAR_DISPCAP_SCALED;
    disp.nbuf = disp.latest ? 3 : 2;
    disp.showing = 0;
    disp.drawing = 1;
    disp.id = 1u;
    disp.shown_bytes = (uint32_t)bytes;
    disp.reported_w = w;
    disp.reported_h = h;
    memcpy(disp.title, "aar-host", 9);
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
    disp.active = (SDL_GetWindowFlags(disp.win) & SDL_WINDOW_INPUT_FOCUS) != 0u;
    if (disp.caps & AAR_DISPCAP_TEXT_INPUT)
        SDL_StartTextInput();
    else
        SDL_StopTextInput();
    disp.opened_us = host_time_us();
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

static uint64_t reserve_frame(uint64_t *out)
{
    uint64_t id = aar_present_reserve(&presents);

    if (id)
        *out = id;
    return id;
}

static int flip_pixels(int async, uint64_t *out)
{
    int old = disp.pending;
    uint64_t id = 0u;
    int rc;

    if (old >= 0 && !disp.latest)
        return AAR_E_BUSY;
    if (out && !(id = reserve_frame(out)))
        return AAR_E_BUSY;
    if (old >= 0) {
        disp.dropped++;
        finish_tracked(take_pending(), AAR_PRESENT_REPLACED,
                       AAR_PRESENT_REASON_NONE);
    }
    if (async) {
        disp.pending = disp.drawing;
        disp.pending_id = id;
        if (disp.latest) {
            disp.drawing = spare_buffer();
            if (disp.drawing == disp.pending)
                disp.drawing = old >= 0 ? old : (disp.showing ^ 1);
        }
        return AAR_OK;
    }
    rc = present_pixels(disp.drawing, id, !out);
    if (rc != AAR_OK)
        return rc;
    disp.showing = disp.drawing;
    /*
     * Pick the next drawing buffer from the live rotation (showing /
     * pending / free). A fixed `drawing ^= 1` assumes two buffers and
     * produces index 3 after a latest-mode async flip left drawing at 2.
     */
    disp.drawing = spare_buffer();
    if (disp.drawing == disp.showing)
        disp.drawing = (disp.showing + 1) % disp.nbuf;
    return AAR_OK;
}

static int api_display_flip(uint32_t surface_id)
{
    if (host_stop_requested())
        return AAR_E_IO;
    if (!valid_id(surface_id))
        return AAR_E_INVAL;
    return flip_pixels(0, NULL);
}

static int api_display_flip_async(uint32_t surface_id)
{
    if (!valid_id(surface_id))
        return AAR_E_INVAL;
    return flip_pixels(1, NULL);
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

/* Back to the default pointer, and the SDL cursors made for the app are freed. */
static void reset_pointer(void)
{
    SDL_Cursor *def = SDL_GetDefaultCursor();

    if (def)
        SDL_SetCursor(def);
    SDL_ShowCursor(SDL_ENABLE);
    if (cursor_custom) {
        SDL_FreeCursor(cursor_custom);
        cursor_custom = 0;
    }
    if (cursor_busy) {
        SDL_FreeCursor(cursor_busy);
        cursor_busy = 0;
    }
}

static int api_display_close(uint32_t surface_id)
{
    int i;

    if (!valid_id(surface_id))
        return AAR_E_INVAL;
    cancel_pending();
    host_gl_release();
    reset_pointer();
    destroy_window();
    for (i = 0; i < 3; i++) {
        free(disp.buf[i]);
        disp.buf[i] = 0;
    }
    free(disp.convert);
    disp.convert = 0;
    disp.open = 0;
    disp.gl = 0;
    disp.caps = 0u;
    return AAR_OK;
}

/* ---- API 0.6 ------------------------------------------------------------ */

static int display_index(void)
{
    int idx = disp.win ? SDL_GetWindowDisplayIndex(disp.win) : 0;

    return idx >= 0 ? idx : 0;
}

/* SDL reports whole hertz. */
static uint32_t refresh_mhz(void)
{
    SDL_DisplayMode dm;

    if (SDL_GetCurrentDisplayMode(display_index(), &dm) != 0 ||
        dm.refresh_rate <= 0)
        return 0u;
    return (uint32_t)dm.refresh_rate * 1000u;
}

static int api_display_info(uint32_t id, aar_display_info *out)
{
    aar_display_info di;
    SDL_DisplayMode dm;
    uint32_t size;

    if (!out)
        return AAR_E_INVAL;
    size = out->size;
    if (size < (uint32_t)sizeof out->size)
        return AAR_E_INVAL;
    if (!valid_id(id))
        return AAR_E_INVAL;
    memset(&di, 0, sizeof di);
    di.size = size;
    di.width = disp.w;
    di.height = disp.h;
    di.refresh_mhz = refresh_mhz();
    di.screen_width = disp.w;
    di.screen_height = disp.h;
    if (SDL_GetDesktopDisplayMode(display_index(), &dm) == 0 && dm.w > 0 &&
        dm.h > 0) {
        di.screen_width = (uint32_t)dm.w;
        di.screen_height = (uint32_t)dm.h;
    }
    di.flags = AAR_DISPINFO_WINDOW;
    if (!disp.gl && (logical_size_in_use() || host_options()->scale > 1))
        di.flags |= AAR_DISPINFO_SCALED;
    if (disp.active)
        di.flags |= AAR_DISPINFO_ACTIVE;
    memcpy(out, &di, size < sizeof di ? size : sizeof di);
    if (size > sizeof di)
        memset((uint8_t *)out + sizeof di, 0, size - sizeof di);
    return AAR_OK;
}

static int mode_known(const aar_display_mode *list, uint32_t n, uint32_t w,
                      uint32_t h, uint32_t mhz)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (list[i].width == w && list[i].height == h &&
            list[i].refresh_mhz == mhz)
            return (int)i;
    }
    return -1;
}

static int api_display_modes(aar_display_mode *out, uint32_t max,
                             uint32_t *out_n)
{
    aar_display_mode *all;
    SDL_DisplayMode dm;
    uint32_t count = 0;
    uint32_t copy;
    int idx;
    int total;
    int i;
    int pref = 0;

    if (!out_n || (max && !out))
        return AAR_E_INVAL;
    if (!host_sdl_video_ok())
        return AAR_E_NOSYS;
    idx = display_index();
    total = SDL_GetNumDisplayModes(idx);
    if (total < 0)
        total = 0;
    all = (aar_display_mode *)calloc((size_t)total + 1u, sizeof *all);
    if (!all)
        return AAR_E_NOMEM;
    for (i = 0; i < total; i++) {
        uint32_t mhz;

        if (SDL_GetDisplayMode(idx, i, &dm) != 0 || dm.w <= 0 || dm.h <= 0)
            continue;
        mhz = dm.refresh_rate > 0 ? (uint32_t)dm.refresh_rate * 1000u : 0u;
        if (mode_known(all, count, (uint32_t)dm.w, (uint32_t)dm.h, mhz) >= 0)
            continue;
        all[count].width = (uint32_t)dm.w;
        all[count].height = (uint32_t)dm.h;
        all[count].refresh_mhz = mhz;
        all[count].formats = HOST_MODE_FORMATS;
        count++;
    }
    /* The preferred mode is the desktop mode, the native size. */
    if (SDL_GetDesktopDisplayMode(idx, &dm) == 0 && dm.w > 0 && dm.h > 0) {
        uint32_t mhz = dm.refresh_rate > 0 ? (uint32_t)dm.refresh_rate * 1000u
                                           : 0u;

        pref = mode_known(all, count, (uint32_t)dm.w, (uint32_t)dm.h, mhz);
        if (pref < 0) {
            pref = (int)count;
            all[count].width = (uint32_t)dm.w;
            all[count].height = (uint32_t)dm.h;
            all[count].refresh_mhz = mhz;
            all[count].formats = HOST_MODE_FORMATS;
            count++;
        }
    }
    if (count)
        all[pref].flags |= AAR_MODEF_PREFERRED;
    copy = count < max ? count : max;
    if (copy)
        memcpy(out, all, (size_t)copy * sizeof *out);
    free(all);
    *out_n = count;
    return count > max ? AAR_E_NOSPC : AAR_OK;
}

static int api_display_resize(uint32_t id, uint32_t w, uint32_t h,
                              aar_surface *out)
{
    uint32_t bpp;
    uint32_t pitch;
    uint64_t bytes;
    uint8_t *buf[3] = {0, 0, 0};
    uint8_t *convert;
    int scale;
    int i;

    if (host_stop_requested())
        return AAR_E_IO;
    if (!valid_id(id) || !out)
        return AAR_E_INVAL;
    if (!(disp.caps & AAR_DISPCAP_RESIZE))
        return AAR_E_NOSYS;
    if (!w || !h || w > AAR_DISPLAY_MAX_DIM || h > AAR_DISPLAY_MAX_DIM)
        return AAR_E_INVAL;
    if (!disp.latest && disp.pending >= 0)
        return AAR_E_BUSY;
    bpp = bpp_of(disp.format);
    pitch = w * bpp;
    bytes = (uint64_t)pitch * h;
    if (bytes > (64u << 20))
        return AAR_E_NOMEM;
    for (i = 0; i < disp.nbuf; i++) {
        buf[i] = (uint8_t *)calloc(1, (size_t)bytes);
        if (!buf[i])
            break;
    }
    convert = i == disp.nbuf ? (uint8_t *)malloc((size_t)w * h * 4u) : 0;
    if (!convert) {
        for (i = 0; i < 3; i++)
            free(buf[i]);
        return AAR_E_NOMEM;
    }
    if (disp.gl) {
        /* A GL window of another size is a new window: some drivers (SDL's
         * offscreen one) keep the framebuffer at the size they made it with.
         * The context moves to it, and keeps its state. A window that already
         * has the size, as after the user resized it, stays. */
        SDL_Window *old = disp.win;
        uint32_t old_id = disp.window_id;
        uint32_t old_w = disp.w;
        uint32_t old_h = disp.h;
        int cur_w = 0;
        int cur_h = 0;

        SDL_GetWindowSize(old, &cur_w, &cur_h);
        if (cur_w != (int)w || cur_h != (int)h) {
            if (!host_options()->headless)
                SDL_GetWindowPosition(old, &disp.win_x, &disp.win_y);
            disp.w = w;
            disp.h = h;
            if (create_window(1) != 0 ||
                host_gl_set_window(disp.win, (int32_t)w, (int32_t)h) != 0) {
                if (disp.win)
                    SDL_DestroyWindow(disp.win);
                (void)host_gl_set_window(old, (int32_t)old_w, (int32_t)old_h);
                disp.win = old;
                disp.window_id = old_id;
                disp.w = old_w;
                disp.h = old_h;
                for (i = 0; i < 3; i++)
                    free(buf[i]);
                free(convert);
                return AAR_E_IO;
            }
            SDL_DestroyWindow(old);
        } else if (host_gl_set_window(old, (int32_t)w, (int32_t)h) != 0) {
            for (i = 0; i < 3; i++)
                free(buf[i]);
            free(convert);
            return AAR_E_IO;
        }
    }
    cancel_pending(); /* a latest-mode frame of the old size cannot finish */
    for (i = 0; i < 3; i++) {
        free(disp.buf[i]);
        disp.buf[i] = buf[i];
    }
    free(disp.convert);
    disp.convert = convert;
    disp.w = w;
    disp.h = h;
    disp.pitch = pitch;
    disp.showing = 0;
    disp.drawing = 1;
    disp.reported_w = w;
    disp.reported_h = h;
    /* The window follows the surface. The frame on screen stays what it was
     * (the old texture and the old copy for --dump-front) until the next
     * flip. */
    scale = window_scale();
    if (disp.win && !disp.gl)
        SDL_SetWindowSize(disp.win, (int)w * scale, (int)h * scale);
    if (disp.ren && logical_size_in_use())
        SDL_RenderSetLogicalSize(disp.ren, (int)w, (int)h);
    if (!disp.gl)
        redraw();
    fill_surface(out);
    return AAR_OK;
}

static int api_display_set_title(uint32_t id, const char *title)
{
    if (!valid_id(id) || !title)
        return AAR_E_INVAL;
    if (strlen(title) > HOST_TITLE_MAX)
        return AAR_E_INVAL;
    memcpy(disp.title, title, strlen(title) + 1u);
    if (disp.win)
        SDL_SetWindowTitle(disp.win, disp.title);
    return AAR_OK;
}

static int api_display_pointer(uint32_t id, uint32_t mode,
                               const aar_pointer *image)
{
    if (!valid_id(id))
        return AAR_E_INVAL;
    if (mode == AAR_POINTER_DEFAULT) {
        reset_pointer();
    } else if (mode == AAR_POINTER_HIDDEN) {
        SDL_ShowCursor(SDL_DISABLE);
    } else if (mode == AAR_POINTER_BUSY) {
        if (!cursor_busy)
            cursor_busy = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAITARROW);
        /* A video driver without system cursors (the dummy one) has nothing
         * to show: the call still succeeds. */
        if (cursor_busy)
            SDL_SetCursor(cursor_busy);
        SDL_ShowCursor(SDL_ENABLE);
    } else if (mode == AAR_POINTER_CUSTOM) {
        SDL_Surface *surf;
        SDL_Cursor *cur;

        if (!image || !image->argb || image->width < 1u ||
            image->width > AAR_POINTER_MAX_DIM || image->height < 1u ||
            image->height > AAR_POINTER_MAX_DIM || image->hot_x < 0 ||
            image->hot_y < 0 || (uint32_t)image->hot_x >= image->width ||
            (uint32_t)image->hot_y >= image->height)
            return AAR_E_INVAL;
        surf = SDL_CreateRGBSurfaceWithFormatFrom(
            (void *)image->argb, (int)image->width, (int)image->height, 32,
            (int)(image->width * 4u), SDL_PIXELFORMAT_ARGB8888);
        if (!surf)
            return AAR_E_NOMEM;
        /* SDL copies the pixels, so the app's array is not kept. */
        cur = SDL_CreateColorCursor(surf, image->hot_x, image->hot_y);
        SDL_FreeSurface(surf);
        if (!cur)
            return AAR_E_IO;
        SDL_SetCursor(cur);
        if (cursor_custom)
            SDL_FreeCursor(cursor_custom);
        cursor_custom = cur;
        SDL_ShowCursor(SDL_ENABLE);
    } else {
        return AAR_E_INVAL;
    }
    return AAR_OK;
}

/*
 * The next refresh boundary is estimated: SDL gives the display's refresh
 * rate, and the boundaries are the multiples of one period after the last
 * presented frame (or after the open, before the first). A call returns at the
 * next boundary, or after timeout_us with AAR_E_TIMEDOUT when that is sooner.
 * A rate of 0 (a driver that does not know it) is AAR_E_NOSYS.
 */
static int api_display_wait_vblank(uint32_t timeout_us)
{
    uint64_t now;
    uint64_t base;
    uint64_t next;
    uint64_t k;
    uint32_t mhz;
    int timed_out;

    if (host_stop_requested())
        return AAR_E_IO;
    if (!disp.open || timeout_us == 0u)
        return AAR_E_INVAL;
    mhz = refresh_mhz();
    if (mhz == 0u)
        return AAR_E_NOSYS;
    now = host_time_us();
    base = disp.presented_us ? disp.presented_us : disp.opened_us;
    k = now > base ? (now - base) * mhz / 1000000000ull + 1u : 1u;
    /* Rounded up, so that a call that wakes at a boundary sees the next one. */
    next = base + (k * 1000000000ull + mhz - 1u) / mhz;
    timed_out = next - now > timeout_us;
    host_sleep_until(timed_out ? now + timeout_us : next);
    if (host_stop_requested())
        return AAR_E_IO;
    /* A flip that waited for the blank is shown by now. */
    host_display_pump();
    return timed_out ? AAR_E_TIMEDOUT : AAR_OK;
}

static uint64_t api_display_last_present_us(void)
{
    return disp.presented_us;
}

static int api_display_present(uint32_t id, uint32_t flags, uint64_t *out)
{
    uint64_t frame_id;

    if (!out)
        return AAR_E_INVAL;
    *out = 0u;
    if (!valid_id(id) || (flags != 0u && flags != AAR_PRESENT_ASYNC &&
                          flags != AAR_PRESENT_GL))
        return AAR_E_INVAL;
    if (flags == AAR_PRESENT_GL) {
        if (!disp.gl)
            return host_gl_available() ? AAR_E_INVAL : AAR_E_NOSYS;
    } else if (disp.gl) {
        return AAR_E_NOSYS;
    }
    if (host_stop_requested())
        return AAR_E_IO;
    if (flags != AAR_PRESENT_GL)
        return flip_pixels(flags == AAR_PRESENT_ASYNC, out);
    frame_id = reserve_frame(out);
    if (!frame_id)
        return AAR_E_BUSY;
    host_gl_swap(frame_id);
    return AAR_OK;
}

static int api_display_present_poll(aar_present_result *out)
{
    if (!out)
        return AAR_E_INVAL;
    if (disp.pending_id && host_stop_event_pending())
        cancel_pending();
    return aar_present_poll(&presents, out);
}

/* ---- Window events, for input.c ----------------------------------------- */

uint32_t host_display_caps(void)
{
    return disp.open ? disp.caps : 0u;
}

uint32_t host_display_window_id(void)
{
    return disp.open ? disp.window_id : 0u;
}

int host_display_focus(uint32_t window_id, int gained)
{
    if (!disp.open || window_id != disp.window_id)
        return 0;
    gained = gained != 0;
    if (disp.active == gained)
        return 0;
    disp.active = gained;
    return 1;
}

/* A window of a new size. Returns 1 with the size the surface would have when
 * the app should hear of it: an app that asked for AAR_DISPLAY_RESIZABLE and a
 * size that is not the one it was last told or has set. */
int host_display_window_resized(uint32_t window_id, int w, int h,
                                uint32_t *sw, uint32_t *sh)
{
    int scale = window_scale();
    uint32_t nw;
    uint32_t nh;

    if (!disp.open || window_id != disp.window_id ||
        !(disp.caps & AAR_DISPCAP_RESIZE) || w <= 0 || h <= 0)
        return 0;
    nw = (uint32_t)(w / scale);
    nh = (uint32_t)(h / scale);
    if (nw < 1u)
        nw = 1u;
    if (nh < 1u)
        nh = 1u;
    if (nw > AAR_DISPLAY_MAX_DIM)
        nw = AAR_DISPLAY_MAX_DIM;
    if (nh > AAR_DISPLAY_MAX_DIM)
        nh = AAR_DISPLAY_MAX_DIM;
    redraw();
    if (nw == disp.reported_w && nh == disp.reported_h)
        return 0;
    disp.reported_w = nw;
    disp.reported_h = nh;
    *sw = nw;
    *sh = nh;
    return 1;
}

void host_display_expose(uint32_t window_id)
{
    if (disp.open && window_id == disp.window_id)
        redraw();
}

/* A window position in surface pixels (floor division, so the part of the
 * window left of or above the surface is negative). A window that SDL scales
 * for a logical size already reports surface pixels. */
void host_display_to_surface(int x, int y, int *sx, int *sy)
{
    int scale = logical_size_in_use() ? 1 : window_scale();

    if (scale < 1)
        scale = 1;
    *sx = x >= 0 ? x / scale : -((-x + scale - 1) / scale);
    *sy = y >= 0 ? y / scale : -((-y + scale - 1) / scale);
}

/*
 * The GL side (hostgl.c). gl_open swaps the window of the open surface for one
 * with an OpenGL pixel format and returns it with its size. gl_close puts the
 * software window back and shows the surface again. gl_store copies a frame
 * that GL drew (RGBA, bottom row first) into the surface, in its format, so that
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
    disp.gl = 1;
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
    disp.gl = 0;
    if (create_software() != 0) {
        fprintf(stderr, "aar-host: cannot restore the window: %s\n",
                SDL_GetError());
        return;
    }
    if (present_buffer(disp.showing) == 0)
        remember_buffer(disp.showing);
}

void host_display_gl_store(const uint8_t *rgba, uint32_t w, uint32_t h)
{
    uint8_t *dst = disp.open ? disp.buf[disp.showing] : 0;
    uint32_t x, y;

    if (!dst || w != disp.w || h != disp.h || disp.format == AAR_FMT_CLUT8)
        return;
    ensure_shown();
    if (!disp.shown)
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
    if (!disp.open || disp.pending < 0)
        return;
    if (disp.pending_id && host_stop_event_pending())
        cancel_pending();
    else
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
    memset(&presents, 0, sizeof presents);
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
    api->display_info = api_display_info;
    api->display_modes = api_display_modes;
    api->display_resize = api_display_resize;
    api->display_set_title = api_display_set_title;
    api->display_pointer = api_display_pointer;
    api->display_wait_vblank = api_display_wait_vblank;
    api->display_last_present_us = api_display_last_present_us;
    api->display_present = api_display_present;
    api->display_present_poll = api_display_present_poll;
}
