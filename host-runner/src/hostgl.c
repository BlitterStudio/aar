/*
 * The GL group on the host: an SDL2 OpenGL compatibility context for the open
 * surface, and an aar_gl table over the host's GL 1.1 entry points.
 *
 * Startup (host_gl_init) creates a hidden window and a context once, to find a
 * pixel format with a depth buffer of at least 16 bits and to resolve every
 * entry point. The group bit is set only when that works, as spec 8.5 asks.
 * gl_bind then swaps the window of the surface for one with an OpenGL pixel
 * format (display.c), creates the context, and fills the table. gl_unbind
 * deletes the context and gives the surface its software window back.
 *
 * The table slots point at wrappers, not at the host functions: a host GL
 * entry can use another calling convention (stdcall on 32-bit Windows), and a
 * slot that the host lacks stays NULL. The list below is the one place that
 * names a slot. A slot that a later version of aar/gl.h appends is one more
 * line in HOSTGL_REQUIRED or HOSTGL_OPTIONAL.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"

#include <SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef APIENTRY
#ifdef _WIN32
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

/* The GL constants the runner itself needs. */
#define HGL_TEXTURE_2D 0x0de1u
#define HGL_UNSIGNED_BYTE 0x1401u
#define HGL_ALPHA 0x1906u
#define HGL_RGB 0x1907u
#define HGL_RGBA 0x1908u
#define HGL_LUMINANCE 0x1909u
#define HGL_LUMINANCE_ALPHA 0x190au
#define HGL_BACK 0x0405u
#define HGL_READ_BUFFER 0x0c02u
#define HGL_MAX_TEXTURE_SIZE 0x0d33u
#define HGL_PACK_ROW_LENGTH 0x0d02u
#define HGL_PACK_SKIP_ROWS 0x0d03u
#define HGL_PACK_SKIP_PIXELS 0x0d04u
#define HGL_PACK_ALIGNMENT 0x0d05u
#define HGL_UNPACK_ROW_LENGTH 0x0cf2u
#define HGL_UNPACK_SKIP_ROWS 0x0cf3u
#define HGL_UNPACK_SKIP_PIXELS 0x0cf4u
#define HGL_UNPACK_ALIGNMENT 0x0cf5u

/* GLU error codes returned by the mipmap builder. */
#define GLU_INVALID_ENUM 100900
#define GLU_INVALID_VALUE 100901
#define GLU_OUT_OF_MEMORY 100902

/*
 * Slots with a one-to-one host entry point. V is a function that returns
 * nothing: V(slot, host symbol, (parameters), (arguments)). R returns a value:
 * R(type, slot, host symbol, (parameters), (arguments)). The parameter types
 * are the ones of aar/gl.h. Required: the group is absent when the host lacks
 * one. Perspective and Build2DMipmaps are written below, because GLU is not
 * linked.
 */
#define HOSTGL_REQUIRED(V, R) \
    V(Begin, glBegin, (uint32_t mode), (mode)) \
    V(End, glEnd, (void), ()) \
    V(Vertex2f, glVertex2f, (float x, float y), (x, y)) \
    V(Vertex3f, glVertex3f, (float x, float y, float z), (x, y, z)) \
    V(Vertex3fv, glVertex3fv, (const float *v), (v)) \
    V(TexCoord2f, glTexCoord2f, (float s, float t), (s, t)) \
    V(TexCoord2fv, glTexCoord2fv, (const float *v), (v)) \
    V(Color3f, glColor3f, (float r, float g, float b), (r, g, b)) \
    V(Color3fv, glColor3fv, (const float *v), (v)) \
    V(Color3ubv, glColor3ubv, (const uint8_t *v), (v)) \
    V(Color4f, glColor4f, (float r, float g, float b, float a), (r, g, b, a)) \
    V(Color4fv, glColor4fv, (const float *v), (v)) \
    V(Color4ub, glColor4ub, (uint8_t r, uint8_t g, uint8_t b, uint8_t a), \
      (r, g, b, a)) \
    V(Color4ubv, glColor4ubv, (const uint8_t *v), (v)) \
    V(Enable, glEnable, (uint32_t cap), (cap)) \
    V(Disable, glDisable, (uint32_t cap), (cap)) \
    R(uint8_t, IsEnabled, glIsEnabled, (uint32_t cap), (cap)) \
    V(AlphaFunc, glAlphaFunc, (uint32_t func, float ref), (func, ref)) \
    V(BlendFunc, glBlendFunc, (uint32_t sfactor, uint32_t dfactor), \
      (sfactor, dfactor)) \
    V(DepthFunc, glDepthFunc, (uint32_t func), (func)) \
    V(DepthMask, glDepthMask, (uint8_t flag), (flag)) \
    V(DepthRange, glDepthRange, (double n, double f), (n, f)) \
    V(CullFace, glCullFace, (uint32_t mode), (mode)) \
    V(FrontFace, glFrontFace, (uint32_t mode), (mode)) \
    V(ShadeModel, glShadeModel, (uint32_t mode), (mode)) \
    V(PolygonMode, glPolygonMode, (uint32_t face, uint32_t mode), \
      (face, mode)) \
    V(Clear, glClear, (uint32_t mask), (mask)) \
    V(ClearColor, glClearColor, (float r, float g, float b, float a), \
      (r, g, b, a)) \
    V(Viewport, glViewport, (int32_t x, int32_t y, int32_t width, \
                             int32_t height), (x, y, width, height)) \
    V(Scissor, glScissor, (int32_t x, int32_t y, int32_t width, \
                           int32_t height), (x, y, width, height)) \
    V(MatrixMode, glMatrixMode, (uint32_t mode), (mode)) \
    V(LoadIdentity, glLoadIdentity, (void), ()) \
    V(LoadMatrixf, glLoadMatrixf, (const float *m), (m)) \
    V(PushMatrix, glPushMatrix, (void), ()) \
    V(PopMatrix, glPopMatrix, (void), ()) \
    V(Rotatef, glRotatef, (float angle, float x, float y, float z), \
      (angle, x, y, z)) \
    V(Translatef, glTranslatef, (float x, float y, float z), (x, y, z)) \
    V(Scalef, glScalef, (float x, float y, float z), (x, y, z)) \
    V(Ortho, glOrtho, (double left, double right, double bottom, double top, \
                       double zNear, double zFar), \
      (left, right, bottom, top, zNear, zFar)) \
    V(Frustum, glFrustum, (double left, double right, double bottom, \
                           double top, double zNear, double zFar), \
      (left, right, bottom, top, zNear, zFar)) \
    V(TexImage2D, glTexImage2D, \
      (uint32_t target, int32_t level, int32_t internalformat, int32_t width, \
       int32_t height, int32_t border, uint32_t format, uint32_t type, \
       const void *pixels), \
      (target, level, internalformat, width, height, border, format, type, \
       pixels)) \
    V(TexSubImage2D, glTexSubImage2D, \
      (uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, \
       int32_t width, int32_t height, uint32_t format, uint32_t type, \
       const void *pixels), \
      (target, level, xoffset, yoffset, width, height, format, type, pixels)) \
    V(TexParameteri, glTexParameteri, \
      (uint32_t target, uint32_t pname, int32_t param), (target, pname, param)) \
    V(TexParameterf, glTexParameterf, \
      (uint32_t target, uint32_t pname, float param), (target, pname, param)) \
    V(TexEnvi, glTexEnvi, (uint32_t target, uint32_t pname, int32_t param), \
      (target, pname, param)) \
    V(TexEnvf, glTexEnvf, (uint32_t target, uint32_t pname, float param), \
      (target, pname, param)) \
    V(GenTextures, glGenTextures, (int32_t n, uint32_t *textures), \
      (n, textures)) \
    V(DeleteTextures, glDeleteTextures, (int32_t n, const uint32_t *textures), \
      (n, textures)) \
    V(BindTexture, glBindTexture, (uint32_t target, uint32_t texture), \
      (target, texture)) \
    V(Finish, glFinish, (void), ()) \
    V(Flush, glFlush, (void), ()) \
    V(DrawBuffer, glDrawBuffer, (uint32_t mode), (mode)) \
    V(ReadBuffer, glReadBuffer, (uint32_t mode), (mode)) \
    V(ReadPixels, glReadPixels, \
      (int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, \
       uint32_t type, void *pixels), (x, y, width, height, format, type, pixels)) \
    R(const uint8_t *, GetString, glGetString, (uint32_t name), (name)) \
    V(GetIntegerv, glGetIntegerv, (uint32_t pname, int32_t *params), \
      (pname, params)) \
    V(GetFloatv, glGetFloatv, (uint32_t pname, float *params), (pname, params)) \
    R(uint32_t, GetError, glGetError, (void), ()) \
    V(Hint, glHint, (uint32_t target, uint32_t mode), (target, mode)) \
    V(PixelStorei, glPixelStorei, (uint32_t pname, int32_t param), \
      (pname, param)) \
    V(VertexPointer, glVertexPointer, \
      (int32_t size, uint32_t type, int32_t stride, const void *pointer), \
      (size, type, stride, pointer)) \
    V(TexCoordPointer, glTexCoordPointer, \
      (int32_t size, uint32_t type, int32_t stride, const void *pointer), \
      (size, type, stride, pointer)) \
    V(ColorPointer, glColorPointer, \
      (int32_t size, uint32_t type, int32_t stride, const void *pointer), \
      (size, type, stride, pointer)) \
    V(EnableClientState, glEnableClientState, (uint32_t cap), (cap)) \
    V(DisableClientState, glDisableClientState, (uint32_t cap), (cap)) \
    V(DrawArrays, glDrawArrays, (uint32_t mode, int32_t first, int32_t count), \
      (mode, first, count)) \
    V(DrawElements, glDrawElements, \
      (uint32_t mode, int32_t count, uint32_t type, const void *indices), \
      (mode, count, type, indices))

/* Slots that an extension or a later GL version provides. The slot stays NULL
 * when the host lacks the entry point, and the gl* wrappers of libaar do
 * nothing for it. */
#define HOSTGL_OPTIONAL(V, R) \
    V(ClientActiveTextureARB, glClientActiveTextureARB, (uint32_t texture), \
      (texture)) \
    V(LockArrays, glLockArraysEXT, (uint32_t first, int32_t count), \
      (first, count)) \
    V(UnlockArrays, glUnlockArraysEXT, (void), ())

#define HOSTGL_ALL(V, R) HOSTGL_REQUIRED(V, R) HOSTGL_OPTIONAL(V, R)

/* Written by hand: Perspective and Build2DMipmaps. */
#define HOSTGL_LOCAL_SLOTS 2u

#define HOSTGL_COUNT_V(slot, sym, params, args) +1u
#define HOSTGL_COUNT_R(ret, slot, sym, params, args) +1u
_Static_assert(0u HOSTGL_ALL(HOSTGL_COUNT_V, HOSTGL_COUNT_R) +
               HOSTGL_LOCAL_SLOTS == AAR_GL_SLOT_COUNT,
               "aar_gl has a slot that hostgl.c does not fill");

/* The host entry points, resolved for the current context. */
#define HOSTGL_PTR_V(slot, sym, params, args) \
    static void (APIENTRY *host_##slot) params;
#define HOSTGL_PTR_R(ret, slot, sym, params, args) \
    static ret (APIENTRY *host_##slot) params;
HOSTGL_ALL(HOSTGL_PTR_V, HOSTGL_PTR_R)

/* The wrappers that the table points at. */
#define HOSTGL_WRAP_V(slot, sym, params, args) \
    static void wrap_##slot params { host_##slot args; }
#define HOSTGL_WRAP_R(ret, slot, sym, params, args) \
    static ret wrap_##slot params { return host_##slot args; }
HOSTGL_ALL(HOSTGL_WRAP_V, HOSTGL_WRAP_R)

static struct {
    int available;
    int depth;
    int stencil;
    int bound;
    SDL_Window *win;
    SDL_GLContext ctx;
    int32_t w, h;
    uint8_t *readback;
    size_t readback_bytes;
    aar_gl table;
} gl;

static void *load_symbol(const char *name)
{
    return (void *)SDL_GL_GetProcAddress(name);
}

#define HOSTGL_LOAD_V(slot, sym, params, args) \
    host_##slot = (void (APIENTRY *) params)load_symbol(#sym);
#define HOSTGL_LOAD_R(ret, slot, sym, params, args) \
    host_##slot = (ret (APIENTRY *) params)load_symbol(#sym);
#define HOSTGL_NEED_V(slot, sym, params, args) \
    if (!host_##slot && !missing) \
        missing = #sym;
#define HOSTGL_NEED_R(ret, slot, sym, params, args) \
    if (!host_##slot && !missing) \
        missing = #sym;

/* Resolves every entry point for the current context. Returns the name of the
 * first required entry point that is absent, or NULL. */
static const char *resolve_all(void)
{
    const char *missing = 0;

    HOSTGL_ALL(HOSTGL_LOAD_V, HOSTGL_LOAD_R)
    if (!host_ClientActiveTextureARB)
        host_ClientActiveTextureARB = (void (APIENTRY *)(uint32_t))
            load_symbol("glClientActiveTexture");
    HOSTGL_REQUIRED(HOSTGL_NEED_V, HOSTGL_NEED_R)
    return missing;
}

/* ---- gluPerspective and gluBuild2DMipmaps, without GLU ------------------ */

static void wrap_Perspective(double fovy, double aspect, double zNear,
                             double zFar)
{
    double ymax = zNear * tan(fovy * 3.14159265358979323846 / 360.0);
    double xmax = ymax * aspect;

    host_Frustum(-xmax, xmax, -ymax, ymax, zNear, zFar);
}

static int components_of(uint32_t format)
{
    switch (format) {
    case HGL_RGBA:
        return 4;
    case HGL_RGB:
        return 3;
    case HGL_LUMINANCE_ALPHA:
        return 2;
    case HGL_LUMINANCE:
    case HGL_ALPHA:
        return 1;
    default:
        return 0;
    }
}

/* The power of two that GLU picks for a size: a value of 3 * 2^k or more
 * rounds up, anything else down. */
static int32_t nearest_power(int32_t value)
{
    int32_t p = 1;

    for (;;) {
        if (value == 1)
            return p;
        if (value == 3)
            return p * 4;
        value >>= 1;
        p *= 2;
    }
}

/* Area average of src (sw x sh, row stride srow bytes) into a tight dst
 * (dw x dh). A 2:1 step is the exact 2x2 box. */
static void scale_box(const uint8_t *src, int32_t sw, int32_t sh, size_t srow,
                      uint8_t *dst, int32_t dw, int32_t dh, int comps)
{
    int32_t dy;

    for (dy = 0; dy < dh; dy++) {
        int32_t y0 = (int32_t)((int64_t)dy * sh / dh);
        int32_t y1 = (int32_t)(((int64_t)(dy + 1) * sh + dh - 1) / dh);
        int32_t dx;

        if (y1 <= y0)
            y1 = y0 + 1;
        for (dx = 0; dx < dw; dx++) {
            int32_t x0 = (int32_t)((int64_t)dx * sw / dw);
            int32_t x1 = (int32_t)(((int64_t)(dx + 1) * sw + dw - 1) / dw);
            int c;

            if (x1 <= x0)
                x1 = x0 + 1;
            for (c = 0; c < comps; c++) {
                uint64_t sum = 0;
                uint64_t n = (uint64_t)(y1 - y0) * (uint64_t)(x1 - x0);
                int32_t y;

                for (y = y0; y < y1; y++) {
                    const uint8_t *row = src + (size_t)y * srow;
                    int32_t x;

                    for (x = x0; x < x1; x++)
                        sum += row[(size_t)x * (size_t)comps + (size_t)c];
                }
                dst[((size_t)dy * (size_t)dw + (size_t)dx) * (size_t)comps +
                    (size_t)c] = (uint8_t)((sum + n / 2u) / n);
            }
        }
    }
}

static int32_t pixel_store(uint32_t pname)
{
    int32_t v = 0;

    host_GetIntegerv(pname, &v);
    return v;
}

static int32_t wrap_Build2DMipmaps(uint32_t target, int32_t internalFormat,
                                   int32_t width, int32_t height,
                                   uint32_t format, uint32_t type,
                                   const void *data)
{
    int comps = components_of(format);
    int32_t max_size = 0;
    int32_t w, h, level;
    int32_t align, rowlen;
    int32_t keep_align, keep_rowlen, keep_rows, keep_pixels;
    size_t stride;
    uint8_t *cur, *next;

    if (target != HGL_TEXTURE_2D || type != HGL_UNSIGNED_BYTE || comps == 0)
        return GLU_INVALID_ENUM;
    if (width < 1 || height < 1 || width > 32768 || height > 32768 || !data)
        return GLU_INVALID_VALUE;
    host_GetIntegerv(HGL_MAX_TEXTURE_SIZE, &max_size);
    if (max_size < 1)
        max_size = 1024;

    w = nearest_power(width);
    h = nearest_power(height);
    while (w > max_size)
        w /= 2;
    while (h > max_size)
        h /= 2;

    keep_align = pixel_store(HGL_UNPACK_ALIGNMENT);
    keep_rowlen = pixel_store(HGL_UNPACK_ROW_LENGTH);
    keep_rows = pixel_store(HGL_UNPACK_SKIP_ROWS);
    keep_pixels = pixel_store(HGL_UNPACK_SKIP_PIXELS);
    align = keep_align < 1 ? 1 : keep_align;
    rowlen = keep_rowlen < 1 ? width : keep_rowlen;
    stride = ((size_t)rowlen * (size_t)comps + (size_t)align - 1u) /
             (size_t)align * (size_t)align;

    cur = (uint8_t *)malloc((size_t)w * (size_t)h * (size_t)comps);
    next = (uint8_t *)malloc((size_t)w * (size_t)h * (size_t)comps);
    if (!cur || !next) {
        free(cur);
        free(next);
        return GLU_OUT_OF_MEMORY;
    }
    scale_box((const uint8_t *)data + (size_t)keep_rows * stride +
                  (size_t)keep_pixels * (size_t)comps,
              width, height, stride, cur, w, h, comps);

    /* The levels are tight, so upload them with the default unpacking. */
    host_PixelStorei(HGL_UNPACK_ALIGNMENT, 1);
    host_PixelStorei(HGL_UNPACK_ROW_LENGTH, 0);
    host_PixelStorei(HGL_UNPACK_SKIP_ROWS, 0);
    host_PixelStorei(HGL_UNPACK_SKIP_PIXELS, 0);
    for (level = 0;; level++) {
        int32_t nw, nh;

        host_TexImage2D(target, level, internalFormat, w, h, 0, format, type,
                        cur);
        if (w == 1 && h == 1)
            break;
        nw = w > 1 ? w / 2 : 1;
        nh = h > 1 ? h / 2 : 1;
        scale_box(cur, w, h, (size_t)w * (size_t)comps, next, nw, nh, comps);
        {
            uint8_t *t = cur;
            cur = next;
            next = t;
        }
        w = nw;
        h = nh;
    }
    host_PixelStorei(HGL_UNPACK_ALIGNMENT, keep_align);
    host_PixelStorei(HGL_UNPACK_ROW_LENGTH, keep_rowlen);
    host_PixelStorei(HGL_UNPACK_SKIP_ROWS, keep_rows);
    host_PixelStorei(HGL_UNPACK_SKIP_PIXELS, keep_pixels);
    free(cur);
    free(next);
    return 0;
}

/* ---- the table ---------------------------------------------------------- */

#define HOSTGL_FILL_V(slot, sym, params, args) \
    t->slot = host_##slot ? wrap_##slot : 0;
#define HOSTGL_FILL_R(ret, slot, sym, params, args) \
    t->slot = host_##slot ? wrap_##slot : 0;

static void fill_table(aar_gl *t)
{
    memset(t, 0, sizeof *t);
    t->size = (uint32_t)sizeof *t;
    t->version = AAR_GL_VERSION_ARRAYS;
    HOSTGL_ALL(HOSTGL_FILL_V, HOSTGL_FILL_R)
    t->Perspective = wrap_Perspective;
    t->Build2DMipmaps = wrap_Build2DMipmaps;
}

/* ---- context and window ------------------------------------------------- */

static void set_attributes(int depth, int stencil)
{
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencil);
}

/* One try of the probe: a hidden window and a context with this much depth
 * and stencil. Returns NULL when it works, else the reason. */
static const char *probe_once(int depth, int stencil)
{
    SDL_Window *win;
    SDL_GLContext ctx;
    const char *missing;
    const char *why = 0;
    int got = 0;

    set_attributes(depth, stencil);
    win = SDL_CreateWindow("aar-host", SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, 64, 64,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win)
        return SDL_GetError();
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        why = SDL_GetError();
    } else {
        if (SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &got) != 0 || got < 16) {
            why = "the context has no 16-bit depth buffer";
        } else {
            missing = resolve_all();
            if (missing)
                why = missing;
        }
        SDL_GL_MakeCurrent(win, NULL);
        SDL_GL_DeleteContext(ctx);
    }
    SDL_DestroyWindow(win);
    return why;
}

int host_gl_init(void)
{
    static const int tries[][2] = {{24, 8}, {24, 0}, {16, 0}};
    const char *driver = SDL_GetCurrentVideoDriver();
    const char *why = "no video";
    size_t i;

    gl.available = 0;
    if (!driver)
        return 0;
    for (i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        why = probe_once(tries[i][0], tries[i][1]);
        if (!why) {
            gl.depth = tries[i][0];
            gl.stencil = tries[i][1];
            gl.available = 1;
            return 1;
        }
    }
    /* The dummy driver cannot do GL; saying so on every run would be noise. */
    if (strcmp(driver, "dummy") != 0)
        fprintf(stderr, "aar-host: no GL with the %s video driver (%s)\n",
                driver, why);
    return 0;
}

int host_gl_available(void)
{
    return gl.available;
}

void host_gl_release(void)
{
    if (!gl.bound)
        return;
    SDL_GL_MakeCurrent(gl.win, NULL);
    SDL_GL_DeleteContext(gl.ctx);
    gl.ctx = 0;
    gl.win = 0;
    gl.bound = 0;
    free(gl.readback);
    gl.readback = 0;
    gl.readback_bytes = 0;
}

void host_gl_shutdown(void)
{
    host_gl_release();
}

static const aar_gl *api_gl_bind(uint32_t surface_id)
{
    void *win = 0;

    if (!gl.available || gl.bound)
        return 0;
    set_attributes(gl.depth, gl.stencil);
    if (host_display_gl_open(surface_id, &win, &gl.w, &gl.h) != 0)
        return 0;
    gl.win = (SDL_Window *)win;
    gl.ctx = SDL_GL_CreateContext(gl.win);
    if (!gl.ctx || resolve_all() != 0) {
        if (gl.ctx)
            SDL_GL_DeleteContext(gl.ctx);
        gl.ctx = 0;
        gl.win = 0;
        host_display_gl_close();
        return 0;
    }
    /* A hidden offscreen surface has no refresh to wait for. */
    SDL_GL_SetSwapInterval(host_options()->headless ? 0 : 1);
    fill_table(&gl.table);
    gl.bound = 1;
    return &gl.table;
}

/* The back buffer into the surface, for --dump-front. */
static void store_frame(void)
{
    size_t bytes = (size_t)gl.w * (size_t)gl.h * 4u;
    int32_t read_buffer = 0;
    int32_t pack[4];

    if (gl.readback_bytes < bytes) {
        free(gl.readback);
        gl.readback = (uint8_t *)malloc(bytes);
        gl.readback_bytes = gl.readback ? bytes : 0;
    }
    if (!gl.readback)
        return;
    host_GetIntegerv(HGL_READ_BUFFER, &read_buffer);
    pack[0] = pixel_store(HGL_PACK_ALIGNMENT);
    pack[1] = pixel_store(HGL_PACK_ROW_LENGTH);
    pack[2] = pixel_store(HGL_PACK_SKIP_ROWS);
    pack[3] = pixel_store(HGL_PACK_SKIP_PIXELS);
    host_ReadBuffer(HGL_BACK);
    host_PixelStorei(HGL_PACK_ALIGNMENT, 1);
    host_PixelStorei(HGL_PACK_ROW_LENGTH, 0);
    host_PixelStorei(HGL_PACK_SKIP_ROWS, 0);
    host_PixelStorei(HGL_PACK_SKIP_PIXELS, 0);
    host_ReadPixels(0, 0, gl.w, gl.h, HGL_RGBA, HGL_UNSIGNED_BYTE,
                    gl.readback);
    host_ReadBuffer((uint32_t)read_buffer);
    host_PixelStorei(HGL_PACK_ALIGNMENT, pack[0]);
    host_PixelStorei(HGL_PACK_ROW_LENGTH, pack[1]);
    host_PixelStorei(HGL_PACK_SKIP_ROWS, pack[2]);
    host_PixelStorei(HGL_PACK_SKIP_PIXELS, pack[3]);
    host_display_gl_store(gl.readback, (uint32_t)gl.w, (uint32_t)gl.h);
}

/* A swap parks like a flip, so it also runs the host pump: an app whose loop
 * only swaps, polls events and checks should_stop still gets input and a
 * close request. After a stop the frame is not presented, as with a flip. */
static void api_gl_swap(void)
{
    if (!gl.bound)
        return;
    if (!host_stop_requested()) {
        if (host_options()->dump_front)
            store_frame();
        SDL_GL_SwapWindow(gl.win);
        host_display_gl_swapped();
    }
    host_pump();
}

static void api_gl_unbind(void)
{
    if (!gl.bound)
        return;
    host_gl_release();
    host_display_gl_close();
}

void host_gl_bind(aar_api *api)
{
    if (!gl.available)
        return;
    api->gl_bind = api_gl_bind;
    api->gl_swap = api_gl_swap;
    api->gl_unbind = api_gl_unbind;
}
