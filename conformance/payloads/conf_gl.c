/*
 * Conformance payload for C-GL (spec 8.5): the GL group from the app. It is
 * templates/mgl_conformance.c adapted to print result ids: the texture environment
 * matrix of the template is C-GL.6 (the matrix file is shared, so the expected values are
 * the template's), and the table, the bind rules, the swap, the double-precision matrix
 * calls and the isolated draw are the other rows. The runner supplies the display (its
 * own table, runner/r_gfx.c) and reads the frame that aar_gl_swap presents (C-GL.5).
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_gl.h"
#include "conf_msgio.h"
#include "../../templates/mgl_conformance_matrix.h"

AAR_NOTE(0, 0, AAR_GROUP_DISPLAY | AAR_GROUP_GL, 0)

#define GL_DEPTH_BUFFER_BIT 0x00000100u
#define GL_COLOR_BUFFER_BIT 0x00004000u
#define GL_TRIANGLES 0x0004u
#define GL_QUADS 0x0007u
#define GL_INVALID_VALUE 0x0501u
#define GL_DEPTH_TEST 0x0B71u
#define GL_LESS 0x0201u
#define GL_MODELVIEW 0x1700u
#define GL_PROJECTION 0x1701u
#define GL_TEXTURE_2D 0x0de1u
#define GL_TEXTURE_ENV 0x2300u
#define GL_TEXTURE_ENV_MODE 0x2200u
#define GL_REPLACE 0x1e01u
#define GL_MODULATE 0x2100u
#define GL_DECAL 0x2101u
#define GL_BLEND 0x0be2u
#define GL_ADD 0x0104u
#define GL_RGBA 0x1908u
#define GL_RGB 0x1907u
#define GL_LUMINANCE 0x1909u
#define GL_ALPHA 0x1906u
#define GL_LUMINANCE_ALPHA 0x190au
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_FLOAT 0x1406u
#define GL_UNSIGNED_SHORT 0x1403u
#define GL_NEAREST 0x2600u
#define GL_TEXTURE_MIN_FILTER 0x2801u
#define GL_TEXTURE_MAG_FILTER 0x2800u
#define GL_VERTEX_ARRAY 0x8074u
#define GL_TEXTURE_COORD_ARRAY 0x8078u
#define GL_COLOR_ARRAY 0x8076u
#define GL_ALPHA_TEST 0x0bc0u
#define GL_GREATER 0x0204u
#define GL_SRC_ALPHA 0x0302u
#define GL_ONE_MINUS_SRC_ALPHA 0x0303u

#define WIDTH 32
#define HEIGHT 32

static int have_peer;
static aar_surface surf;
static const aar_gl *gl;

static int bind_display(struct conf_test *t)
{
    int rc = aar_display_open(WIDTH, HEIGHT, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, &surf);

    if (rc != AAR_OK) {
        if (conf_fail_first(t))
            cmio_why_rc(t, "display_open", rc);
        return 0;
    }
    gl = aar_gl_bind(surf.id);
    if (!gl) {
        if (conf_fail_first(t))
            conf_buf_str(&t->why, "aar_gl_bind of the open surface returned NULL");
        (void)aar_display_close(surf.id);
        return 0;
    }
    return 1;
}

static void unbind_display(void)
{
    aar_gl_unbind();
    gl = 0;
    (void)aar_display_close(surf.id);
}

/* ---- C-GL.1 to C-GL.4: the table and the bind rules -------------------------------------------- */

static void test_table(void)
{
    struct conf_test t1;
    struct conf_test t2;
    uint32_t slots;
    uint32_t i;
    uint32_t missing = 0;
    static const uint32_t array_slots[] = {
        (uint32_t)offsetof(aar_gl, VertexPointer), (uint32_t)offsetof(aar_gl, TexCoordPointer),
        (uint32_t)offsetof(aar_gl, ColorPointer), (uint32_t)offsetof(aar_gl, EnableClientState),
        (uint32_t)offsetof(aar_gl, DisableClientState),
        (uint32_t)offsetof(aar_gl, ClientActiveTextureARB), (uint32_t)offsetof(aar_gl, DrawArrays),
        (uint32_t)offsetof(aar_gl, DrawElements), (uint32_t)offsetof(aar_gl, LockArrays),
        (uint32_t)offsetof(aar_gl, UnlockArrays)
    };

    conf_begin(&t1, "C-GL.1");
    conf_begin(&t2, "C-GL.2");
    if (!bind_display(&t1)) {
        conf_fail_first(&t2);
        conf_buf_str(&t2.why, "no table: the bind failed");
        conf_end(&t1);
        conf_end(&t2);
        return;
    }
    conf_check(&t1, gl->size > 8u, "the table size is not set");
    conf_check(&t1, gl->version == AAR_GL_VERSION_V0 || gl->version == AAR_GL_VERSION_ARRAYS,
               "the table version is neither AAR_GL_VERSION_V0 nor AAR_GL_VERSION_ARRAYS");
    conf_buf_str(&t1.note, "size ");
    conf_buf_u64(&t1.note, gl->size);
    conf_buf_str(&t1.note, ", version ");
    conf_buf_u64(&t1.note, gl->version);
    conf_end(&t1);

    conf_check(&t2, AAR_GL_COVERS(gl, Build2DMipmaps), "the table size does not cover Build2DMipmaps");
    slots = (uint32_t)((offsetof(aar_gl, Build2DMipmaps) - offsetof(aar_gl, Begin)) / sizeof(void *)) + 1u;
    for (i = 0; i < slots; i++) {
        const void *const *p = (const void *const *)((const uint8_t *)gl + offsetof(aar_gl, Begin));

        if (p[i] == 0)
            missing++;
    }
    if (missing && conf_fail_first(&t2)) {
        conf_buf_u64(&t2.why, missing);
        conf_buf_str(&t2.why, " of the ");
        conf_buf_u64(&t2.why, slots);
        conf_buf_str(&t2.why, " slots through Build2DMipmaps are NULL");
    }
    if (gl->version == AAR_GL_VERSION_ARRAYS) {
        for (i = 0; i < sizeof array_slots / sizeof array_slots[0]; i++) {
            const void *const *p = (const void *const *)((const uint8_t *)gl + array_slots[i]);

            if (gl->size < array_slots[i] + sizeof(void *) && conf_fail_first(&t2)) {
                conf_buf_str(&t2.why, "AAR_GL_VERSION_ARRAYS but the size does not cover array slot ");
                conf_buf_u64(&t2.why, i);
            } else if (gl->size >= array_slots[i] + sizeof(void *) && *p == 0 &&
                       conf_fail_first(&t2)) {
                conf_buf_str(&t2.why, "array slot ");
                conf_buf_u64(&t2.why, i);
                conf_buf_str(&t2.why, " is NULL");
            }
        }
    }
    conf_buf_u64(&t2.note, slots);
    conf_buf_str(&t2.note, " slots filled");
    conf_end(&t2);
    unbind_display();
}

static void test_bind_rules(void)
{
    struct conf_test t3;
    struct conf_test t4;
    uint32_t other[3];
    uint32_t i;

    conf_begin(&t3, "C-GL.3");
    conf_begin(&t4, "C-GL.4");
    if (aar_display_open(WIDTH, HEIGHT, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, &surf) != AAR_OK) {
        conf_check(&t3, 0, "display_open failed");
        conf_check(&t4, 0, "display_open failed");
        conf_end(&t3);
        conf_end(&t4);
        return;
    }
    other[0] = surf.id + 1u;
    other[1] = 0u;
    other[2] = 0xFFFFFFFFu;
    for (i = 0; i < 3u; i++) {
        if (aar_gl_bind(other[i]) != 0) {
            if (conf_fail_first(&t3)) {
                conf_buf_str(&t3.why, "aar_gl_bind of id ");
                conf_buf_u64(&t3.why, other[i]);
                conf_buf_str(&t3.why, " returned a table");
            }
            aar_gl_unbind(); /* so that the next call and C-GL.4 start with no context */
        }
    }
    conf_buf_str(&t3.note, "ids that are not an open surface return NULL");
    conf_end(&t3);

    gl = aar_gl_bind(surf.id);
    conf_check(&t4, gl != 0, "the first bind failed");
    aar_gl_unbind();
    gl = aar_gl_bind(surf.id);
    conf_check(&t4, gl != 0, "a new bind after aar_gl_unbind failed");
    conf_buf_str(&t4.note, "bind, unbind, bind");
    conf_end(&t4);
    unbind_display();
}

/* ---- C-GL.5: the swap presents the frame -------------------------------------------------------- */

static void test_swap(void)
{
    struct conf_test t;
    static const float colours[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
                                         { 0.0f, 0.0f, 1.0f } };
    uint32_t i;

    if (!have_peer) {
        conf_skip("C-GL.5", "host: no runner to read the frame the swap presented");
        return;
    }
    conf_begin(&t, "C-GL.5");
    if (bind_display(&t)) {
        uint32_t late = 0;

        for (i = 0; i < 3u; i++) {
            uint32_t ok = 0, rgb = 0;
            uint32_t want = ((uint32_t)(colours[i][0] * 255.0f) << 16) |
                            ((uint32_t)(colours[i][1] * 255.0f) << 8) |
                            (uint32_t)(colours[i][2] * 255.0f);
            uint32_t swaps = 1;
            int rc;

            glClearColor(colours[i][0], colours[i][1], colours[i][2], 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            aar_gl_swap();
            /* A platform may present the frame of a swap while a later swap runs (the copy
             * and flip are queued): at most two swapped frames are in flight, so the same frame
             * is swapped again, up to 2 more times, until the client has it (spec 8.5). */
            for (;;) {
                (void)cmio_kw(CG_K_PIXEL, WIDTH / 2, HEIGHT / 2, 0);
                rc = cmio_result(10000000u, &ok, &rgb, 0, 0, 0, 0);
                if (rc == 1 && ok && rgb == want)
                    break;
                if (rc != 1 || swaps > 2u) {
                    if (conf_fail_first(&t)) {
                        conf_buf_str(&t.why, "after ");
                        conf_buf_u64(&t.why, swaps);
                        conf_buf_str(&t.why, " swaps of colour ");
                        conf_buf_u64(&t.why, i);
                        conf_buf_str(&t.why, " the client's pixel is ");
                        conf_buf_hex(&t.why, rgb);
                        conf_buf_str(&t.why, ", expected ");
                        conf_buf_hex(&t.why, want);
                        if (rc != 1)
                            conf_buf_str(&t.why, " (no answer from the runner)");
                    }
                    break;
                }
                glClear(GL_COLOR_BUFFER_BIT);
                aar_gl_swap();
                aar_sleep_us(50000u);
                swaps++;
            }
            if (swaps > 1u)
                late++;
        }
        conf_buf_str(&t.note, late ? "red, green and blue clears read by the client, each after "
                                     "a further swap (the swap presents the frame queued before it)"
                                   : "red, green and blue clears read by the client after the swap");
        unbind_display();
    }
    conf_end(&t);
}

/* ---- C-GL.6: the texture environment matrix of templates/mgl_conformance.c ------------------------ */

static const float vertices[4][3] = {
    { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f },
    { 1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f }
};
static const float coords[4][2] = {
    { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }
};
static const uint16_t indexes[4] = { 0u, 1u, 2u, 3u };

static uint32_t gl_env(enum mgl_probe_env mode)
{
    static const uint32_t values[] = { GL_REPLACE, GL_MODULATE, GL_DECAL, GL_BLEND, GL_ADD };

    return values[mode];
}

static uint32_t gl_format(enum mgl_probe_format format)
{
    static const uint32_t values[] = { GL_RGBA, GL_RGB, GL_LUMINANCE, GL_ALPHA, GL_LUMINANCE_ALPHA };

    return values[format];
}

static void upload_texture(enum mgl_probe_format format, mgl_probe_rgba value)
{
    uint8_t pixels[4];
    uint8_t luma = mgl_probe_luminance(value);
    uint32_t source_format = gl_format(format);

    pixels[0] = value.r;
    pixels[1] = value.g;
    pixels[2] = value.b;
    pixels[3] = value.a;
    if (format == MGL_PROBE_LUMINANCE)
        pixels[0] = luma;
    if (format == MGL_PROBE_ALPHA)
        pixels[0] = value.a;
    if (format == MGL_PROBE_LUMINANCE_ALPHA) {
        pixels[0] = luma;
        pixels[1] = value.a;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, (int32_t)source_format, 1, 1, 0, source_format,
                 GL_UNSIGNED_BYTE, pixels);
}

static void draw_quad(enum mgl_probe_draw draw, mgl_probe_rgba colour)
{
    float colours[4][4];
    unsigned int vertex;

    for (vertex = 0u; vertex < 4u; vertex++) {
        colours[vertex][0] = colour.r / 255.0f;
        colours[vertex][1] = colour.g / 255.0f;
        colours[vertex][2] = colour.b / 255.0f;
        colours[vertex][3] = colour.a / 255.0f;
    }
    if (draw == MGL_PROBE_IMMEDIATE) {
        glBegin(GL_QUADS);
        for (vertex = 0u; vertex < 4u; vertex++) {
            glColor4fv(colours[vertex]);
            glTexCoord2fv(coords[vertex]);
            glVertex3fv(vertices[vertex]);
        }
        glEnd();
        return;
    }
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coords);
    glColorPointer(4, GL_FLOAT, 0, colours);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    if (draw == MGL_PROBE_ARRAYS)
        glDrawArrays(GL_QUADS, 0, 4);
    else
        glDrawElements(GL_QUADS, 4, GL_UNSIGNED_SHORT, indexes);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static uint32_t pack(mgl_probe_rgba c)
{
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}

static void test_matrix(void)
{
    struct conf_test t;
    uint32_t texture;
    uint32_t cases = 0;
    uint32_t failures = 0;
    unsigned int env, format, texel, colour, draw, alpha_test, blend;

    conf_begin(&t, "C-GL.6");
    if (!bind_display(&t)) {
        conf_end(&t);
        return;
    }
    glViewport(0, 0, WIDTH, HEIGHT);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    for (env = 0u; env < sizeof(mgl_probe_envs) / sizeof(mgl_probe_envs[0]); env++)
    for (format = 0u; format < sizeof(mgl_probe_formats) / sizeof(mgl_probe_formats[0]); format++)
    for (draw = 0u; draw < sizeof(mgl_probe_draws) / sizeof(mgl_probe_draws[0]); draw++)
    for (texel = 0u; texel < sizeof(mgl_probe_texels) / sizeof(mgl_probe_texels[0]); texel++)
    for (colour = 0u; colour < sizeof(mgl_probe_colours) / sizeof(mgl_probe_colours[0]); colour++)
    for (alpha_test = 0u; alpha_test < 2u; alpha_test++)
    for (blend = 0u; blend < 2u; blend++) {
        mgl_probe_rgba actual;
        mgl_probe_rgba expected;

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        upload_texture(mgl_probe_formats[format], mgl_probe_texels[texel].value);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (int32_t)gl_env(mgl_probe_envs[env]));
        if (alpha_test) {
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.5f);
        } else {
            glDisable(GL_ALPHA_TEST);
        }
        if (blend) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_BLEND);
        }
        draw_quad(mgl_probe_draws[draw], mgl_probe_colours[colour].value);
        glFinish();
        glReadPixels(WIDTH / 2, HEIGHT / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &actual);
        expected = mgl_probe_expected(mgl_probe_envs[env], mgl_probe_formats[format],
                                      mgl_probe_texels[texel].value, mgl_probe_colours[colour].value,
                                      (int)alpha_test, (int)blend);
        /* The target is RGB565: a framebuffer without alpha reads A back as 1.0 (the
         * template's rule). */
        expected.a = 255u;
        cases++;
        if (!mgl_probe_match(actual, expected, 8u)) {
            failures++;
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "case ");
                conf_buf_u64(&t.why, cases);
                conf_buf_str(&t.why, " env=");
                conf_buf_str(&t.why, mgl_probe_env_names[env]);
                conf_buf_str(&t.why, " fmt=");
                conf_buf_str(&t.why, mgl_probe_format_names[format]);
                conf_buf_str(&t.why, " draw=");
                conf_buf_str(&t.why, mgl_probe_draw_names[draw]);
                conf_buf_str(&t.why, " texel=");
                conf_buf_str(&t.why, mgl_probe_texels[texel].name);
                conf_buf_str(&t.why, " colour=");
                conf_buf_str(&t.why, mgl_probe_colours[colour].name);
                conf_buf_str(&t.why, " at=");
                conf_buf_u64(&t.why, alpha_test);
                conf_buf_str(&t.why, " bl=");
                conf_buf_u64(&t.why, blend);
                conf_buf_str(&t.why, " actual=");
                conf_buf_hex(&t.why, pack(actual));
                conf_buf_str(&t.why, " expected=");
                conf_buf_hex(&t.why, pack(expected));
            }
        }
    }
    glDeleteTextures(1, &texture);
    if (failures && t.failed) {
        conf_buf_str(&t.why, " (");
        conf_buf_u64(&t.why, failures);
        conf_buf_str(&t.why, " of ");
        conf_buf_u64(&t.why, cases);
        conf_buf_str(&t.why, " cases failed)");
    }
    conf_buf_u64(&t.note, cases);
    conf_buf_str(&t.note, " cases (5 environments, 5 formats, 3 draw paths), none failed");
    unbind_display();
    conf_end(&t);
}

/* ---- C-GL.7: double-precision calls give the expected pixels ---------------------------------------- */

static void rect(float x0, float y0, float x1, float y1, float z, float r, float g, float b)
{
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    glVertex3f(x0, y0, z);
    glVertex3f(x1, y0, z);
    glVertex3f(x1, y1, z);
    glVertex3f(x0, y1, z);
    glEnd();
}

static mgl_probe_rgba pixel_at(int32_t x, int32_t y)
{
    mgl_probe_rgba p = { 0u, 0u, 0u, 0u };

    glFinish();
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &p);
    return p;
}

static void reset_view(void)
{
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static int is_colour(mgl_probe_rgba p, uint8_t r, uint8_t g, uint8_t b)
{
    mgl_probe_rgba want = { 0u, 0u, 0u, 0u };

    want.r = r;
    want.g = g;
    want.b = b;
    want.a = p.a;
    return mgl_probe_match(p, want, 8u);
}

static void test_doubles(void)
{
    struct conf_test t;
    mgl_probe_rgba p;

    conf_begin(&t, "C-GL.7");
    if (bind_display(&t)) {
        glViewport(0, 0, WIDTH, HEIGHT);
        /* Ortho: the left half of the window is red. */
        reset_view();
        glMatrixMode(GL_PROJECTION);
        glOrtho(0.0, 32.0, 0.0, 32.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        rect(0.0f, 0.0f, 16.0f, 32.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        conf_check(&t, is_colour(pixel_at(8, 16), 255, 0, 0), "glOrtho: the left half is not red");
        conf_check(&t, is_colour(pixel_at(24, 16), 0, 0, 0), "glOrtho: the right half is not clear");
        /* Frustum: at z = -5 the view is 5 wide each way, so a square of 2.5 covers the middle half. */
        reset_view();
        glMatrixMode(GL_PROJECTION);
        glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
        glMatrixMode(GL_MODELVIEW);
        rect(-2.5f, -2.5f, 2.5f, 2.5f, -5.0f, 0.0f, 1.0f, 0.0f);
        p = pixel_at(16, 16);
        conf_check(&t, is_colour(p, 0, 255, 0), "glFrustum: the middle is not green");
        conf_check(&t, is_colour(pixel_at(12, 16), 0, 255, 0), "glFrustum: the square is too small");
        conf_check(&t, is_colour(pixel_at(2, 16), 0, 0, 0), "glFrustum: the square is too big");
        /* gluPerspective with a field of view of 90 degrees is the same frustum. */
        reset_view();
        glMatrixMode(GL_PROJECTION);
        gluPerspective(90.0, 1.0, 1.0, 10.0);
        glMatrixMode(GL_MODELVIEW);
        rect(-2.5f, -2.5f, 2.5f, 2.5f, -5.0f, 0.0f, 0.0f, 1.0f);
        conf_check(&t, is_colour(pixel_at(16, 16), 0, 0, 255), "gluPerspective: the middle is not blue");
        conf_check(&t, is_colour(pixel_at(12, 16), 0, 0, 255), "gluPerspective: the square is too small");
        conf_check(&t, is_colour(pixel_at(2, 16), 0, 0, 0), "gluPerspective: the square is too big");
        /* DepthRange: the nearer square wins whatever the order. */
        reset_view();
        glMatrixMode(GL_PROJECTION);
        glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glDepthRange(0.0, 1.0);
        glDepthFunc(GL_LESS);
        glEnable(GL_DEPTH_TEST);
        rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 1.0f, 0.0f);
        rect(-1.0f, -1.0f, 1.0f, 1.0f, -0.5f, 1.0f, 0.0f, 0.0f);
        conf_check(&t, is_colour(pixel_at(16, 16), 0, 255, 0),
                   "glDepthRange: the nearer square was overdrawn");
        glDisable(GL_DEPTH_TEST);
        conf_buf_str(&t.note, "Ortho, Frustum, gluPerspective and DepthRange scenes drew the expected pixels");
        unbind_display();
    }
    conf_end(&t);
}

/* ---- C-GL.8: a bad client-array pointer on an isolated runtime ------------------------------------------ */

/* One draw from a vertex array at `p` (3 vertices of 3 floats). Returns the GL error. */
static uint32_t draw_from(const void *p)
{
    uint32_t err;
    uint32_t guard = 0;

    while (glGetError() != 0 && guard++ < 16u)
        ;
    glVertexPointer(3, GL_FLOAT, 0, p);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    err = glGetError();
    glDisableClientState(GL_VERTEX_ARRAY);
    return err;
}

static void test_isolated_draw(void)
{
    struct conf_test t;
    aar_info info;
    void *heap = 0;
    uint64_t heap_bytes = 0;
    uint32_t err;

    conf_begin(&t, "C-GL.8");
    info.size = sizeof info;
    if (aar_get_info(&info) != AAR_OK || !(info.flags & AAR_INFOF_ISOLATED)) {
        conf_buf_str(&t.note, "not isolated");
        conf_end(&t);
        return;
    }
    if (bind_display(&t)) {
        /* A pointer in the unmapped first page, and a range that starts in the last bytes of
         * the heap and runs past its end: the first is a bad pointer, the second a bad range
         * (draw-time validation, spec 8.5). */
        err = draw_from((const void *)(uintptr_t)0x10u);
        if (err != GL_INVALID_VALUE && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "a draw from the bad pointer 0x10 left glGetError at ");
            conf_buf_hex(&t.why, err);
            conf_buf_str(&t.why, ", not GL_INVALID_VALUE");
        }
        aar_heap_range(&heap, &heap_bytes);
        if (heap && heap_bytes >= 64u) {
            err = draw_from((const uint8_t *)heap + heap_bytes - 4u);
            if (err != GL_INVALID_VALUE && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "a draw from a range that leaves the heap left glGetError at ");
                conf_buf_hex(&t.why, err);
                conf_buf_str(&t.why, ", not GL_INVALID_VALUE");
            }
        }
        conf_buf_str(&t.note, "isolated: the bad draws set GL_INVALID_VALUE and the app went on");
        unbind_display();
    }
    conf_end(&t);
}

int aar_main(int argc, char **argv)
{
    have_peer = conf_have_peer(argc, argv);
    test_table();
    test_bind_rules();
    test_swap();
    test_matrix();
    test_doubles();
    test_isolated_draw();
    return conf_finish("C-GL");
}
