/*
 * GL group of the host runner: bind, a known picture read back with
 * glReadPixels, swap, unbind, and the failures. "absent" checks a run without
 * the group instead (--no-gl). Exit status is the failing step, 77 when the
 * group is absent in the default mode (the driver offers no GL).
 *
 * argv[1]: "gl" (default), "front" (gl, and also read the surface after a
 * swap, which needs --dump-front), "pump" (a loop that only swaps, polls
 * events and checks should_stop must see a key and a quit that SDL queued), or
 * "absent".
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <stdio.h>
#include <string.h>

AAR_NOTE(0, 0, AAR_GROUP_DISPLAY, 0)

#define GL_COLOR_BUFFER_BIT 0x00004000u
#define GL_DEPTH_BUFFER_BIT 0x00000100u
#define GL_TRIANGLES 0x0004u
#define GL_QUADS 0x0007u
#define GL_MODELVIEW 0x1700u
#define GL_PROJECTION 0x1701u
#define GL_DEPTH_TEST 0x0b71u
#define GL_DEPTH_BITS 0x0d56u
#define GL_TEXTURE_2D 0x0de1u
#define GL_TEXTURE_ENV 0x2300u
#define GL_TEXTURE_ENV_MODE 0x2200u
#define GL_REPLACE 0x1e01u
#define GL_NEAREST 0x2600u
#define GL_NEAREST_MIPMAP_NEAREST 0x2700u
#define GL_TEXTURE_MIN_FILTER 0x2801u
#define GL_TEXTURE_MAG_FILTER 0x2800u
#define GL_RGBA 0x1908u
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_FLOAT 0x1406u
#define GL_VERSION 0x1f02u
#define GL_VERTEX_ARRAY 0x8074u
#define GL_NO_ERROR 0u

#define WIDTH 64
#define HEIGHT 48

static int fail(int code, const char *msg)
{
    fprintf(stderr, "gltest: step %d: %s\n", code, msg);
    return code;
}

static int near_rgb(const uint8_t *p, int r, int g, int b)
{
    int dr = p[0] - r, dg = p[1] - g, db = p[2] - b;

    return dr >= -2 && dr <= 2 && dg >= -2 && dg <= 2 && db >= -2 && db <= 2;
}

/* The pixel at (x, y), counted from the lower left, as GL reads it. */
static int pixel_is(int x, int y, int r, int g, int b)
{
    uint8_t px[4] = {0, 0, 0, 0};

    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return near_rgb(px, r, g, b);
}

static void ortho_2d(void)
{
    glViewport(0, 0, WIDTH, HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, WIDTH, 0.0, HEIGHT, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/* Red clear colour, and a green triangle on the lower left half. */
static void draw_scene(void)
{
    ortho_2d();
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0.0f, 1.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glVertex2f(0.0f, 0.0f);
    glVertex2f((float)WIDTH, 0.0f);
    glVertex2f(0.0f, (float)HEIGHT);
    glEnd();
}

static int scene_ok(void)
{
    /* Inside the triangle, outside it, and the cleared corner. */
    return pixel_is(8, 8, 0, 255, 0) && pixel_is(60, 44, 255, 0, 0) &&
           pixel_is(3, 40, 0, 255, 0) && pixel_is(50, 5, 0, 255, 0);
}

static int test_absent(void)
{
    aar_info info;
    aar_surface surface;

    memset(&info, 0, sizeof info);
    info.size = (uint32_t)sizeof info;
    if (aar_get_info(&info) != AAR_OK || (info.groups & AAR_GROUP_GL) != 0u)
        return fail(1, "the GL group is set");
    if (aar_api_ptr->groups & AAR_GROUP_GL)
        return fail(2, "the table claims the GL group");
    if (aar_api_ptr->gl_bind || aar_api_ptr->gl_swap || aar_api_ptr->gl_unbind)
        return fail(3, "a GL slot is set");
    if (aar_display_open(WIDTH, HEIGHT, AAR_FMT_RGB565, 0u, &surface) != AAR_OK)
        return fail(4, "display_open");
    if (aar_gl_bind(surface.id) != 0)
        return fail(5, "aar_gl_bind returned a table");
    glClear(GL_COLOR_BUFFER_BIT);
    aar_gl_swap();
    aar_gl_unbind();
    if (aar_display_close(surface.id) != AAR_OK)
        return fail(6, "display_close");
    return 0;
}

static int check_table(const aar_gl *gl)
{
    if (gl->size != sizeof *gl || gl->version != AAR_GL_VERSION_ARRAYS)
        return fail(20, "table size or version");
    if (!gl->Begin || !gl->End || !gl->Vertex3fv || !gl->Color4ubv ||
        !gl->DepthRange || !gl->Ortho || !gl->Frustum || !gl->TexImage2D ||
        !gl->GetString || !gl->ReadPixels || !gl->Perspective ||
        !gl->Build2DMipmaps || !gl->VertexPointer || !gl->DrawArrays ||
        !gl->DrawElements || !gl->EnableClientState)
        return fail(21, "a slot is NULL");
    return 0;
}

static int test_mipmaps(void)
{
    uint8_t texels[3 * 5 * 4];
    uint32_t tex = 0;
    unsigned i;
    float v[4][3] = {{0, 0, 0}, {64, 0, 0}, {64, 48, 0}, {0, 48, 0}};
    float c[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    for (i = 0; i < 15u; i++) {
        texels[i * 4u] = 200u;
        texels[i * 4u + 1u] = 100u;
        texels[i * 4u + 2u] = 50u;
        texels[i * 4u + 3u] = 255u;
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* A float type is outside what the builder takes. */
    if (gluBuild2DMipmaps(GL_TEXTURE_2D, 4, 3, 5, GL_RGBA, GL_FLOAT, texels) ==
        0)
        return fail(60, "mipmaps accepted GL_FLOAT");
    if (gluBuild2DMipmaps(GL_TEXTURE_2D, 4, 0, 5, GL_RGBA, GL_UNSIGNED_BYTE,
                          texels) == 0)
        return fail(61, "mipmaps accepted a zero width");
    (void)glGetError();
    /* 3 x 5 scales to 4 x 4, then 2 x 2, then 1 x 1. */
    if (gluBuild2DMipmaps(GL_TEXTURE_2D, 4, 3, 5, GL_RGBA, GL_UNSIGNED_BYTE,
                          texels) != 0)
        return fail(62, "gluBuild2DMipmaps");
    if (glGetError() != GL_NO_ERROR)
        return fail(63, "GL error after gluBuild2DMipmaps");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    (int32_t)GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (int32_t)GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (int32_t)GL_REPLACE);
    glEnable(GL_TEXTURE_2D);
    ortho_2d();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    for (i = 0; i < 4u; i++) {
        glTexCoord2fv(c[i]);
        glVertex3fv(v[i]);
    }
    glEnd();
    if (!pixel_is(32, 24, 200, 100, 50))
        return fail(64, "textured quad from the mipmap levels");
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &tex);
    return 0;
}

static int test_perspective(void)
{
    /* A quad at z = -1, a quarter of the view high, in front of a far one. */
    glViewport(0, 0, WIDTH, HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(90.0, (double)WIDTH / (double)HEIGHT, 0.1, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthRange(0.0, 1.0);
    glColor3f(0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS); /* near, blue */
    glVertex3f(-0.25f, -0.25f, -1.0f);
    glVertex3f(0.25f, -0.25f, -1.0f);
    glVertex3f(0.25f, 0.25f, -1.0f);
    glVertex3f(-0.25f, 0.25f, -1.0f);
    glEnd();
    glColor3f(1.0f, 1.0f, 0.0f);
    glBegin(GL_QUADS); /* far, yellow, drawn after, covering the view */
    glVertex3f(-8.0f, -8.0f, -4.0f);
    glVertex3f(8.0f, -8.0f, -4.0f);
    glVertex3f(8.0f, 8.0f, -4.0f);
    glVertex3f(-8.0f, 8.0f, -4.0f);
    glEnd();
    glDisable(GL_DEPTH_TEST);
    if (!pixel_is(32, 24, 0, 0, 255))
        return fail(70, "the depth test let a far quad over a near one");
    if (!pixel_is(32, 3, 255, 255, 0))
        return fail(71, "the far quad is missing");
    return 0;
}

static int test_arrays(void)
{
    static const float tri[3][2] = {{0.0f, 0.0f}, {64.0f, 0.0f}, {0.0f, 48.0f}};

    ortho_2d();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1.0f, 1.0f, 1.0f);
    glVertexPointer(2, GL_FLOAT, 0, tri);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    if (!pixel_is(8, 8, 255, 255, 255) || !pixel_is(60, 44, 0, 0, 0))
        return fail(80, "DrawArrays");
    return 0;
}

static int test_gl(int front)
{
    aar_surface surface;
    const aar_gl *gl;
    const uint8_t *version;
    int32_t depth = 0;
    int rc;
    uint32_t id;

    if (!(aar_api_ptr->groups & AAR_GROUP_GL) || !aar_api_ptr->gl_bind)
        return 77;
    if (aar_display_open(WIDTH, HEIGHT, AAR_FMT_RGB565, 0u, &surface) != AAR_OK)
        return fail(10, "display_open");
    id = surface.id;

    /* A surface that is not open. */
    if (aar_gl_bind(0u) != 0 || aar_gl_bind(id + 1u) != 0 ||
        aar_gl_bind(0xffffffffu) != 0)
        return fail(11, "bind of an id that is not an open surface");
    aar_gl_swap(); /* unbound: nothing happens */
    aar_gl_unbind();

    gl = aar_gl_bind(id);
    if (!gl)
        return fail(12, "aar_gl_bind of the open surface");
    rc = check_table(gl);
    if (rc)
        return rc;
    /* Straight through the table: aar_gl_bind would record the NULL and the
     * gl* wrappers would then do nothing. */
    if (aar_api_ptr->gl_bind(id) != 0)
        return fail(13, "a second bind before unbind");
    glGetIntegerv(GL_DEPTH_BITS, &depth);
    if (depth < 16) {
        fprintf(stderr, "gltest: GL_DEPTH_BITS = %d\n", (int)depth);
        return fail(14, "depth buffer under 16 bits");
    }
    version = glGetString(GL_VERSION);
    if (!version || !version[0])
        return fail(15, "glGetString(GL_VERSION)");

    draw_scene();
    glFinish();
    if (!scene_ok())
        return fail(30, "the triangle and clear colour read back wrong");
    if (glGetError() != GL_NO_ERROR)
        return fail(31, "GL error after the scene");

    rc = test_mipmaps();
    if (rc)
        return rc;
    rc = test_perspective();
    if (rc)
        return rc;
    rc = test_arrays();
    if (rc)
        return rc;

    draw_scene();
    aar_gl_swap();
    if (front) {
        /* The swap put the frame into the surface (--dump-front): RGB565 big
         * endian, top row first. Green at the lower left, red at the upper
         * right. */
        const uint8_t *p;

        if (aar_display_refresh(&surface) != AAR_OK)
            return fail(32, "display_refresh");
        p = (const uint8_t *)surface.front;
        if (p[(HEIGHT - 1 - 8) * surface.pitch + 8 * 2] != 0x07u ||
            p[(HEIGHT - 1 - 8) * surface.pitch + 8 * 2 + 1] != 0xe0u ||
            p[(HEIGHT - 1 - 44) * surface.pitch + 60 * 2] != 0xf8u ||
            p[(HEIGHT - 1 - 44) * surface.pitch + 60 * 2 + 1] != 0x00u)
            return fail(33, "the swapped frame is not in the surface");
    }

    /* Unbind returns the window to the software surface. */
    aar_gl_unbind();
    glClear(GL_COLOR_BUFFER_BIT); /* no table: nothing happens */
    if (aar_display_flip(id) != AAR_OK)
        return fail(40, "display_flip after unbind");

    /* A new bind works, and the new context draws. */
    gl = aar_gl_bind(id);
    if (!gl)
        return fail(41, "bind after unbind");
    draw_scene();
    if (!scene_ok())
        return fail(42, "the second context draws wrong");
    aar_gl_unbind();

    /* Closing the display with the context bound releases it. */
    if (!aar_gl_bind(id))
        return fail(50, "third bind");
    if (aar_display_close(id) != AAR_OK)
        return fail(51, "display_close with GL bound");
    aar_gl_swap();
    if (aar_gl_bind(id) != 0)
        return fail(52, "bind of a closed surface");
    if (aar_display_open(WIDTH, HEIGHT, AAR_FMT_RGB565, 0u, &surface) !=
        AAR_OK)
        return fail(53, "display_open again");
    if (!aar_gl_bind(surface.id))
        return fail(54, "bind of the reopened surface");

    /* The picture that the runner dumps, left bound at exit. */
    draw_scene();
    aar_gl_swap();
    return 0;
}

/* A render loop with no yield and no sleep: aar_gl_swap parks like a flip, so
 * it has to service the host's events. */
static int test_pump(void)
{
    aar_surface surface;
    SDL_Event ev;
    aar_event got;
    uint32_t n = 0;
    int i, seen = 0;

    if (!(aar_api_ptr->groups & AAR_GROUP_GL) || !aar_api_ptr->gl_bind)
        return 77;
    if (aar_display_open(WIDTH, HEIGHT, AAR_FMT_RGB565, 0u, &surface) != AAR_OK)
        return fail(90, "display_open");
    if (!aar_gl_bind(surface.id))
        return fail(91, "aar_gl_bind");

    memset(&ev, 0, sizeof ev);
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.scancode = SDL_SCANCODE_A;
    if (SDL_PushEvent(&ev) != 1)
        return fail(92, "SDL_PushEvent of a key");
    for (i = 0; i < 50 && !seen; i++) {
        glClear(GL_COLOR_BUFFER_BIT);
        aar_gl_swap();
        if (aar_event_poll(&got, 1u, &n) == AAR_OK && n == 1u &&
            got.type == AAR_EV_KEY_DOWN)
            seen = 1;
    }
    if (!seen)
        return fail(93, "a key did not arrive through swap and event_poll");

    memset(&ev, 0, sizeof ev);
    ev.type = SDL_QUIT;
    if (SDL_PushEvent(&ev) != 1)
        return fail(94, "SDL_PushEvent of a quit");
    for (i = 0; i < 50 && !aar_should_stop(); i++) {
        glClear(GL_COLOR_BUFFER_BIT);
        aar_gl_swap();
    }
    if (!aar_should_stop())
        return fail(95, "a quit did not reach should_stop through swap");
    aar_gl_unbind();
    return 0;
}

int aar_main(int argc, char **argv)
{
    int mode_absent = argc > 1 && strcmp(argv[1], "absent") == 0;
    int front = argc > 1 && strcmp(argv[1], "front") == 0;

    if (argc > 1 && strcmp(argv[1], "pump") == 0)
        return test_pump();
    return mode_absent ? test_absent() : test_gl(front);
}
