/*
 * Conformance payload for C-DISPLAY (spec 8.2, 9.6): the display group from the app. The
 * runner supplies its own display table to aar_serve, so it owns the pixel memory and
 * can read every frame the app presents; the tests that look at a frame ask it with the
 * keywords of conf_display.h (runner/r_display.c). The picture itself is judged by the
 * operator (variant picture, C-DISPLAY.17, only with INTERACTIVE).
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_display.h"
#include "conf_msgio.h"

AAR_NOTE(0, 0, AAR_GROUP_DISPLAY, AAR_GROUP_INPUT)

static int have_peer;

static uint32_t bpp_of(uint32_t format)
{
    return format == AAR_FMT_BGRA8888 ? 4u : (format == AAR_FMT_CLUT8 ? 1u : 2u);
}

static int skip_no_peer(const char *id)
{
    if (have_peer)
        return 0;
    conf_skip(id, "host: no runner to read the frame the app presented");
    return 1;
}

/* Opens a surface; a failure is recorded in the test. Returns 1 when it is open. */
static int open_s(struct conf_test *t, aar_surface *s, uint32_t w, uint32_t h, uint32_t format,
                  uint32_t flags)
{
    int rc = aar_display_open(w, h, format, flags, s);

    if (rc != AAR_OK) {
        if (conf_fail_first(t)) {
            conf_buf_str(&t->why, "display_open ");
            conf_buf_u64(&t->why, w);
            conf_buf_str(&t->why, "x");
            conf_buf_u64(&t->why, h);
            conf_buf_str(&t->why, " format ");
            conf_buf_u64(&t->why, format);
            conf_buf_str(&t->why, " returned ");
            conf_buf_i64(&t->why, rc);
        }
        return 0;
    }
    return 1;
}

static void close_s(struct conf_test *t, const aar_surface *s)
{
    int rc = aar_display_close(s->id);

    if (rc != AAR_OK && conf_fail_first(t))
        cmio_why_rc(t, "display_close", rc);
}

/* Stores the byte pattern k in the rows of the draw buffer, by pitch. */
static void fill_bytes(const aar_surface *s, uint32_t k)
{
    uint32_t rowbytes = s->width * bpp_of(s->format);
    uint8_t *base = (uint8_t *)s->back;
    uint32_t y;
    uint32_t i;

    for (y = 0; y < s->height; y++) {
        for (i = 0; i < rowbytes; i++)
            base[y * s->pitch + i] = cd_byte(k, i, y);
    }
}

/* Asks the runner to compare the frame it received. Returns the number of differences
 * (0xFFFFFFFF when it did not answer). */
static uint32_t ask_frame(uint32_t pattern, uint32_t b, uint32_t layout)
{
    uint32_t ok = 0, count = 0;
    int rc;

    (void)cmio_kw(CD_K_FRAME, pattern, b, layout);
    rc = cmio_result(10000000u, &ok, &count, 0, 0, 0, 0);
    if (rc != 1)
        return 0xFFFFFFFFu;
    return ok ? 0u : (count ? count : 1u);
}

static const uint32_t dims[][2] = { { 320, 200 }, { 640, 480 }, { 4096, 1 } };
static const uint32_t formats[] = { AAR_FMT_RGB565, AAR_FMT_BGRA8888 };

/* C-DISPLAY.1 */
static void test_open(void)
{
    struct conf_test t;
    uint32_t d;
    uint32_t f;

    conf_begin(&t, "C-DISPLAY.1");
    for (d = 0; d < 3u; d++) {
        for (f = 0; f < 2u; f++) {
            aar_surface s;
            uint32_t w = dims[d][0];
            uint32_t h = dims[d][1];

            if (!open_s(&t, &s, w, h, formats[f], AAR_DISPLAY_VSYNC))
                continue;
            if ((s.width != w || s.height != h || s.format != formats[f]) && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "the grant for ");
                conf_buf_u64(&t.why, w);
                conf_buf_str(&t.why, "x");
                conf_buf_u64(&t.why, h);
                conf_buf_str(&t.why, " says ");
                conf_buf_u64(&t.why, s.width);
                conf_buf_str(&t.why, "x");
                conf_buf_u64(&t.why, s.height);
                conf_buf_str(&t.why, " format ");
                conf_buf_u64(&t.why, s.format);
            }
            if (s.pitch < w * bpp_of(formats[f]) && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "pitch ");
                conf_buf_u64(&t.why, s.pitch);
                conf_buf_str(&t.why, " is below the row bytes ");
                conf_buf_u64(&t.why, w * bpp_of(formats[f]));
            }
            conf_check(&t, s.reserved == 0, "the reserved word of the surface is not 0");
            conf_check(&t, (s.caps & ~(7u | AAR_DISPCAP_PRESENT_FEEDBACK)) == 0,
                       "caps holds a bit that is not defined");
            close_s(&t, &s);
        }
    }
    conf_buf_str(&t.note, "320x200, 640x480 and 4096x1 in RGB565 and BGRA8888");
    conf_end(&t);
}

/* A buffer is writable: the first and the last byte keep what is stored. */
static int writable(void *base, uint32_t bytes)
{
    volatile uint8_t *p = (volatile uint8_t *)base;
    uint8_t keep0 = p[0];
    uint8_t keep1 = p[bytes - 1u];
    int ok;

    p[0] = 0xA5u;
    p[bytes - 1u] = 0x5Au;
    ok = p[0] == 0xA5u && p[bytes - 1u] == 0x5Au;
    p[0] = keep0;
    p[bytes - 1u] = keep1;
    return ok;
}

/* C-DISPLAY.2 */
static void test_layout(void)
{
    struct conf_test t;
    uint32_t f;

    conf_begin(&t, "C-DISPLAY.2");
    for (f = 0; f < 2u; f++) {
        aar_surface s;
        int fits;

        if (!open_s(&t, &s, 64, 48, formats[f], AAR_DISPLAY_VSYNC))
            continue;
        fits = formats[f] == AAR_FMT_RGB565 ? (s.layout >= AAR_LAYOUT_RGB565_BE &&
                                               s.layout <= AAR_LAYOUT_BGR565_LE)
                                            : (s.layout == AAR_LAYOUT_BGRA8888 ||
                                               s.layout == AAR_LAYOUT_RGBA8888);
        if (!fits && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "layout ");
            conf_buf_u64(&t.why, s.layout);
            conf_buf_str(&t.why, " does not fit format ");
            conf_buf_u64(&t.why, formats[f]);
        }
        if (!s.back) {
            conf_check(&t, 0, "back is NULL");
        } else {
            conf_check(&t, writable(s.back, s.pitch * s.height), "back is not writable");
        }
        if (s.front)
            conf_check(&t, writable(s.front, s.pitch * s.height), "front is not writable");
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* C-DISPLAY.3 */
static void test_refusals(void)
{
    struct conf_test t;
    aar_surface s;
    static const uint32_t bad_formats[] = { 0u, 4u, 99u };
    int rc;
    uint32_t i;

    conf_begin(&t, "C-DISPLAY.3");
    rc = aar_display_open(0, 100, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, &s);
    conf_check(&t, rc < 0, "a width of 0 was accepted");
    if (rc == AAR_OK)
        (void)aar_display_close(s.id);
    rc = aar_display_open(100, 0, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, &s);
    conf_check(&t, rc < 0, "a height of 0 was accepted");
    if (rc == AAR_OK)
        (void)aar_display_close(s.id);
    for (i = 0; i < 3u; i++) {
        rc = aar_display_open(64, 64, bad_formats[i], AAR_DISPLAY_VSYNC, &s);
        if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "format ");
            conf_buf_u64(&t.why, bad_formats[i]);
            conf_buf_str(&t.why, " returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_INVAL (an unknown format is a bad argument, 8.2)");
        }
        if (rc == AAR_OK)
            (void)aar_display_close(s.id);
    }
    conf_end(&t);
}

/* C-DISPLAY.4 */
static void test_one_display(void)
{
    struct conf_test t;
    aar_surface a;
    aar_surface b;
    int rc;

    conf_begin(&t, "C-DISPLAY.4");
    if (open_s(&t, &a, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        rc = aar_display_open(64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, &b);
        if (rc >= 0 && conf_fail_first(&t))
            conf_buf_str(&t.why, "a second display_open succeeded while one is open");
        if (rc == AAR_OK)
            (void)aar_display_close(b.id);
        rc = aar_display_flip(a.id);
        conf_check(&t, rc == AAR_OK, "the first display stopped working after the refused open");
        close_s(&t, &a);
    }
    conf_end(&t);
}

/* C-DISPLAY.5 */
static void test_close(void)
{
    struct conf_test t;
    aar_surface s;
    int rc;

    conf_begin(&t, "C-DISPLAY.5");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        rc = aar_display_close(s.id);
        conf_check(&t, rc == AAR_OK, "display_close did not return AAR_OK");
        rc = aar_display_close(s.id);
        conf_check(&t, rc < 0, "a second display_close was accepted");
        rc = aar_display_flip(s.id);
        conf_check(&t, rc < 0, "a flip after the close was accepted");
        if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC))
            close_s(&t, &s);
    }
    conf_end(&t);
}

/* C-DISPLAY.6 */
static void test_flip_ids(void)
{
    struct conf_test t;
    aar_surface s;
    uint32_t other[3];
    uint32_t i;

    conf_begin(&t, "C-DISPLAY.6");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        conf_check(&t, aar_display_flip(s.id) == AAR_OK, "flip of the open id did not return AAR_OK");
        other[0] = s.id + 1u;
        other[1] = 0u;
        other[2] = 0xFFFFFFFFu;
        for (i = 0; i < 3u; i++) {
            int rc = aar_display_flip(other[i]);

            if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "flip of id ");
                conf_buf_u64(&t.why, other[i]);
                conf_buf_str(&t.why, " returned ");
                conf_buf_i64(&t.why, rc);
                conf_buf_str(&t.why, ", not AAR_E_INVAL");
            }
        }
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* C-DISPLAY.7: write back, flip, refresh. After the refresh back can be written again and
 * front (when there is one) holds the frame just presented (spec 8.2; the test opens with
 * AAR_DISPLAY_VSYNC only, a plain open, so the rule of LATEST does not apply). */
static void test_refresh(void)
{
    struct conf_test t;
    aar_surface s;

    conf_begin(&t, "C-DISPLAY.7");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        if (!s.back) {
            conf_check(&t, 0, "back is NULL");
        } else {
            fill_bytes(&s, 0x11u);
            conf_check(&t, aar_display_flip(s.id) == AAR_OK, "flip did not return AAR_OK");
            conf_check(&t, aar_display_refresh(&s) == AAR_OK, "refresh did not return AAR_OK");
            if (s.front) {
                const uint8_t *f = (const uint8_t *)s.front;
                uint32_t y;
                uint32_t i;
                uint32_t bad = 0;

                for (y = 0; y < 8u; y++) {
                    for (i = 0; i < s.width * 2u; i++) {
                        if (f[y * s.pitch + i] != cd_byte(0x11u, i, y))
                            bad++;
                    }
                }
                if (bad && conf_fail_first(&t)) {
                    conf_buf_u64(&t.why, bad);
                    conf_buf_str(&t.why, " bytes of front differ from the frame just presented");
                }
            }
            if (!s.back) {
                conf_check(&t, 0, "back is NULL after the refresh");
            } else {
                fill_bytes(&s, 0x22u);
                conf_check(&t, ((uint8_t *)s.back)[3] == cd_byte(0x22u, 3, 0),
                           "back could not be written after the refresh");
            }
        }
        conf_buf_str(&t.note, s.front ? "front shows the presented frame" : "no front buffer");
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* Waits up to 1 s for flip_idle to reach 1. Returns the last result. */
static int wait_idle(void)
{
    uint64_t start = aar_time_us();
    int idle;

    do {
        idle = aar_display_flip_idle();
        if (idle != 0)
            return idle;
        aar_yield();
        aar_sleep_us(1000u);
    } while (aar_time_us() - start < 1000000u);
    return idle;
}

/* C-DISPLAY.8 */
static void test_async(void)
{
    struct conf_test t;
    aar_surface s;
    int rc;

    conf_begin(&t, "C-DISPLAY.8");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        if (s.caps & AAR_DISPCAP_ASYNC) {
            rc = aar_display_flip_async(s.id);
            conf_check(&t, rc == AAR_OK, "flip_async did not return AAR_OK");
            rc = wait_idle();
            conf_check(&t, rc == 1, "flip_idle did not reach 1 within 1 s");
            rc = aar_display_flip_async(s.id);
            if (rc == AAR_OK) {
                rc = aar_display_flip_async(s.id);
                conf_check(&t, rc == AAR_OK || rc == AAR_E_BUSY,
                           "a second flip_async while one is pending was neither accepted nor AAR_E_BUSY");
                conf_check(&t, wait_idle() == 1, "flip_idle did not reach 1 after two flips");
            } else {
                conf_check(&t, 0, "the second flip_async was refused");
            }
            conf_buf_str(&t.note, "async granted");
        } else {
            rc = aar_display_flip_async(s.id);
            conf_check(&t, rc < 0, "flip_async worked without AAR_DISPCAP_ASYNC");
            conf_buf_str(&t.note, "async not granted, flip_async refused");
        }
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* C-DISPLAY.9 */
static void test_latest_open(void)
{
    struct conf_test t;
    aar_surface s;

    conf_begin(&t, "C-DISPLAY.9");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST)) {
        if (!(s.caps & AAR_DISPCAP_LATEST)) {
            conf_check(&t, aar_display_latest_dropped() == 0,
                       "display_latest_dropped is not 0 without AAR_DISPCAP_LATEST");
            conf_buf_str(&t.note, "opened without the LATEST cap, dropped 0");
        } else {
            conf_buf_str(&t.note, "opened with the LATEST cap");
        }
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* C-DISPLAY.10 */
static void test_latest_flips(void)
{
    struct conf_test t;
    aar_surface s;
    uint32_t prev = 0;
    uint32_t i;

    conf_begin(&t, "C-DISPLAY.10");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST)) {
        if (!(s.caps & AAR_DISPCAP_LATEST)) {
            conf_buf_str(&t.note, "the LATEST cap is not granted: nothing to count");
        } else {
            for (i = 0; i < 100u; i++) {
                uint32_t d;
                int rc;

                if (s.back)
                    ((uint8_t *)s.back)[0] = (uint8_t)i;
                rc = aar_display_flip(s.id);
                if (rc != AAR_OK && rc != AAR_E_BUSY && conf_fail_first(&t))
                    cmio_why_rc(&t, "flip", rc);
                d = aar_display_latest_dropped();
                if (d < prev && conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "display_latest_dropped went down from ");
                    conf_buf_u64(&t.why, prev);
                    conf_buf_str(&t.why, " to ");
                    conf_buf_u64(&t.why, d);
                }
                prev = d;
            }
            if (prev > 99u && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "display_latest_dropped is ");
                conf_buf_u64(&t.why, prev);
                conf_buf_str(&t.why, " after 100 flips");
            }
            conf_buf_str(&t.note, "100 flips, dropped ");
            conf_buf_u64(&t.note, prev);
        }
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* C-DISPLAY.11 */
static void test_vsync(void)
{
    struct conf_test t;
    aar_surface s;
    uint64_t t0;
    uint64_t dt;
    uint32_t i;

    conf_begin(&t, "C-DISPLAY.11");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        t0 = aar_time_us();
        for (i = 0; i < 10u; i++) {
            int rc = aar_display_vsync();

            if (rc != AAR_OK && conf_fail_first(&t))
                cmio_why_rc(&t, "vsync", rc);
        }
        dt = aar_time_us() - t0;
        /* "Requests synchronisation with the refresh" does not say that the call waits for it (the
         * ZZ9000 yields once), so a floor on the time is not a requirement (spec 8.2, a pacing
         * hint); the ceiling still catches a call that hangs. */
        if (dt > 2000000u && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "ten vsyncs took ");
            conf_buf_u64(&t.why, dt);
            conf_buf_str(&t.why, " us");
        }
        conf_buf_str(&t.note, "ten vsyncs in ");
        conf_buf_u64(&t.note, dt / 1000u);
        conf_buf_str(&t.note, " ms");
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* C-DISPLAY.12: the flip alone makes the frame visible to the client. */
static void test_flip_publishes(void)
{
    struct conf_test t;
    aar_surface s;
    uint32_t round;

    if (skip_no_peer("C-DISPLAY.12"))
        return;
    conf_begin(&t, "C-DISPLAY.12");
    if (open_s(&t, &s, 128, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        for (round = 0; round < 2u && s.back; round++) {
            uint32_t k = round ? 0xC3u : 0x5Au;
            uint32_t diff;

            fill_bytes(&s, k);
            if (aar_display_flip(s.id) != AAR_OK) {
                conf_check(&t, 0, "flip did not return AAR_OK");
                break;
            }
            diff = ask_frame(CD_PAT_BYTES, k, s.layout);
            if (diff && conf_fail_first(&t)) {
                if (diff == 0xFFFFFFFFu) {
                    conf_buf_str(&t.why, "no answer from the runner");
                } else {
                    conf_buf_str(&t.why, "frame ");
                    conf_buf_u64(&t.why, round);
                    conf_buf_str(&t.why, ": the client's copy differs in ");
                    conf_buf_u64(&t.why, diff);
                    conf_buf_str(&t.why, " bytes");
                }
            }
        }
        conf_check(&t, s.back != 0, "back is NULL");
        conf_buf_str(&t.note, "two frames read byte for byte by the client");
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* C-DISPLAY.13: four colours stored by the layout decode to four colours. */
static void test_pixel_layout(void)
{
    struct conf_test t;
    uint32_t f;

    if (skip_no_peer("C-DISPLAY.13"))
        return;
    conf_begin(&t, "C-DISPLAY.13");
    for (f = 0; f < 2u; f++) {
        aar_surface s;

        if (!open_s(&t, &s, 64, 64, formats[f], AAR_DISPLAY_VSYNC))
            continue;
        if (s.back && cd_layout_bytes(s.layout) == bpp_of(formats[f])) {
            uint32_t x;
            uint32_t y;
            uint32_t diff;

            for (y = 0; y < 64u; y++) {
                for (x = 0; x < 64u; x++) {
                    uint32_t r, g, b;

                    cd_quad_colour((y >= 32u ? 2u : 0u) + (x >= 32u ? 1u : 0u), &r, &g, &b);
                    (void)cd_put_pixel((uint8_t *)s.back + y * s.pitch + x * bpp_of(formats[f]),
                                       s.layout, r, g, b);
                }
            }
            (void)aar_display_flip(s.id);
            diff = ask_frame(CD_PAT_QUADS, 0, s.layout);
            if (diff && conf_fail_first(&t)) {
                if (diff == 0xFFFFFFFFu) {
                    conf_buf_str(&t.why, "no answer from the runner");
                } else {
                    conf_buf_str(&t.why, "format ");
                    conf_buf_u64(&t.why, formats[f]);
                    conf_buf_str(&t.why, " layout ");
                    conf_buf_u64(&t.why, s.layout);
                    conf_buf_str(&t.why, ": ");
                    conf_buf_u64(&t.why, diff);
                    conf_buf_str(&t.why, " pixels decode to the wrong colour");
                }
            }
        } else if (conf_fail_first(&t)) {
            conf_buf_str(&t.why, "no draw buffer or a layout that does not fit the format");
        }
        close_s(&t, &s);
    }
    conf_buf_str(&t.note, "RGB565 and BGRA8888 decode to red, green, blue and white");
    conf_end(&t);
}

/* C-DISPLAY.14: rows are stored by pitch. */
static void test_pitch(void)
{
    struct conf_test t;
    aar_surface s;

    if (skip_no_peer("C-DISPLAY.14"))
        return;
    conf_begin(&t, "C-DISPLAY.14");
    (void)cmio_kw(CD_K_PAD, 64, 0, 0);
    if (open_s(&t, &s, 100, 40, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        uint32_t rowbytes = 200u;
        uint32_t y;
        uint32_t i;
        uint32_t diff;

        conf_check(&t, s.pitch >= rowbytes, "pitch is below the row bytes");
        if (s.back && s.pitch >= rowbytes) {
            uint8_t *base = (uint8_t *)s.back;

            for (y = 0; y < s.height; y++) {
                for (i = 0; i < s.pitch; i++)
                    base[y * s.pitch + i] = i < rowbytes ? cd_row(y) : 0xEEu;
            }
            (void)aar_display_flip(s.id);
            diff = ask_frame(CD_PAT_ROWS, 0, s.layout);
            if (diff && conf_fail_first(&t)) {
                if (diff == 0xFFFFFFFFu) {
                    conf_buf_str(&t.why, "no answer from the runner");
                } else {
                    conf_buf_u64(&t.why, diff);
                    conf_buf_str(&t.why, " bytes of the client's copy are in the wrong row (pitch ");
                    conf_buf_u64(&t.why, s.pitch);
                    conf_buf_str(&t.why, ")");
                }
            }
        }
        conf_buf_str(&t.note, "pitch ");
        conf_buf_u64(&t.note, s.pitch);
        conf_buf_str(&t.note, " for rows of 200 bytes");
        close_s(&t, &s);
    }
    (void)cmio_kw(CD_K_PAD, 0, 0, 0);
    conf_end(&t);
}

/* C-DISPLAY.15 */
static void test_palette(void)
{
    struct conf_test t;
    aar_surface s;
    uint32_t pal[256];
    uint32_t i;
    uint32_t ok = 0, count = 0, errors = 0;
    int rc;

    if (skip_no_peer("C-DISPLAY.15"))
        return;
    conf_begin(&t, "C-DISPLAY.15");
    rc = aar_display_open(64, 64, AAR_FMT_CLUT8, AAR_DISPLAY_VSYNC, &s);
    if (rc != AAR_OK) {
        if (rc != AAR_E_NOSYS && rc != AAR_E_INVAL && conf_fail_first(&t))
            cmio_why_rc(&t, "display_open CLUT8", rc);
        conf_buf_str(&t.note, "CLUT8 is not offered (refused with ");
        conf_buf_i64(&t.note, rc);
        conf_buf_str(&t.note, ")");
        conf_end(&t);
        return;
    }
    if (!(s.caps & AAR_DISPCAP_CLUT8)) {
        conf_buf_str(&t.note, "a CLUT8 surface without AAR_DISPCAP_CLUT8: no palette promised");
        close_s(&t, &s);
        conf_end(&t);
        return;
    }
    for (i = 0; i < 256u; i++)
        pal[i] = cd_pal(i);
    rc = aar_display_set_palette(s.id, 0, 256, pal);
    conf_check(&t, rc == AAR_OK, "set_palette of 256 colours did not return AAR_OK");
    rc = aar_display_set_palette(s.id, 200, 100, pal);
    conf_check(&t, rc == AAR_E_INVAL, "set_palette with first + count above 256 was not AAR_E_INVAL");
    aar_sleep_us(200000u); /* the palette request does not park the app: give the client time */
    (void)cmio_kw(CD_K_PALETTE, 0, 0, 0);
    rc = cmio_result(10000000u, &ok, &count, &errors, 0, 0, 0);
    if (rc != 1) {
        conf_check(&t, 0, "no answer from the runner");
    } else if (!ok && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the client got ");
        conf_buf_u64(&t.why, count);
        conf_buf_str(&t.why, " palette entries, ");
        conf_buf_u64(&t.why, errors);
        conf_buf_str(&t.why, " wrong");
    }
    conf_buf_str(&t.note, "256 colours reached the client");
    close_s(&t, &s);
    conf_end(&t);
}

/* C-DISPLAY.16 */
static void test_palette_refused(void)
{
    struct conf_test t;
    aar_surface s;
    uint32_t colour = 0x00112233u;

    conf_begin(&t, "C-DISPLAY.16");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        int rc = aar_display_set_palette(s.id, 0, 1, &colour);

        if (rc >= 0 && conf_fail_first(&t))
            conf_buf_str(&t.why, "set_palette on an RGB565 surface was accepted");
        close_s(&t, &s);
    }
    conf_end(&t);
}

/* ---- C-DISPLAY.21 to C-DISPLAY.29: the display calls of API 0.6 ---------------------------------------------- */

/* The caps bits of the flags of API 0.6. */
#define NEW_CAPS (AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE | AAR_DISPCAP_POINTER_ABS | AAR_DISPCAP_TEXT_INPUT | \
                  AAR_DISPCAP_CLOSE_REQUEST | AAR_DISPCAP_SCALED)
#define ALL_CAPS (AAR_DISPCAP_ASYNC | AAR_DISPCAP_LATEST | AAR_DISPCAP_CLUT8 | \
                  AAR_DISPCAP_PRESENT_FEEDBACK | NEW_CAPS)

/* A client that does not serve a call answers AAR_E_NOSYS (spec 8.2). The row then passes and says so. */
static int not_served(struct conf_test *t, int rc, const char *call)
{
    if (rc != AAR_E_NOSYS)
        return 0;
    conf_buf_str(&t->note, call);
    conf_buf_str(&t->note, ": AAR_E_NOSYS, the client does not serve it");
    return 1;
}

static int expect_rc(struct conf_test *t, int rc, int want, const char *what)
{
    if (rc == want)
        return 1;
    if (conf_fail_first(t))
        cmio_why_rc(t, what, rc);
    return 0;
}

static void fill_bytes_zero(void *p, uint8_t v, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    uint32_t i;

    for (i = 0; i < n; i++)
        b[i] = v;
}

/* The flags and caps of the new requests: each alone, and all at once. */
static const uint32_t new_flags[] = {
    AAR_DISPLAY_WINDOW, AAR_DISPLAY_RESIZABLE, AAR_DISPLAY_POINTER_ABS, AAR_DISPLAY_TEXT_INPUT,
    AAR_DISPLAY_CLOSE_REQUEST, AAR_DISPLAY_SCALE,
};
static const uint32_t new_flag_caps[] = {
    AAR_DISPCAP_WINDOW, AAR_DISPCAP_RESIZE, AAR_DISPCAP_POINTER_ABS, AAR_DISPCAP_TEXT_INPUT,
    AAR_DISPCAP_CLOSE_REQUEST, AAR_DISPCAP_SCALED,
};
#define NEW_FLAG_COUNT 6u
#define ALL_NEW_FLAGS (AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE | AAR_DISPLAY_POINTER_ABS | \
                       AAR_DISPLAY_TEXT_INPUT | AAR_DISPLAY_CLOSE_REQUEST | AAR_DISPLAY_SCALE)

/* C-DISPLAY.28: the requests of API 0.6 are granted or dropped, never refused. */
static void test_new_flags(void)
{
    struct conf_test t;
    uint32_t i;

    conf_begin(&t, "C-DISPLAY.28");
    for (i = 0; i <= NEW_FLAG_COUNT; i++) {
        aar_surface s;
        uint32_t flags = i < NEW_FLAG_COUNT ? new_flags[i] : ALL_NEW_FLAGS;
        uint32_t allowed = i < NEW_FLAG_COUNT ? new_flag_caps[i] : NEW_CAPS;
        int rc = aar_display_open(160, 100, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC | flags, &s);

        if (rc != AAR_OK) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "display_open with the request bits ");
                conf_buf_hex(&t.why, flags);
                conf_buf_str(&t.why, " returned ");
                conf_buf_i64(&t.why, rc);
            }
            continue;
        }
        if ((s.caps & NEW_CAPS & ~allowed) != 0u && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "the request bits ");
            conf_buf_hex(&t.why, flags);
            conf_buf_str(&t.why, " were granted caps ");
            conf_buf_hex(&t.why, s.caps);
        }
        if ((s.caps & ~(uint32_t)ALL_CAPS) != 0u && conf_fail_first(&t))
            conf_buf_str(&t.why, "caps has an undefined bit");
        /* RESIZABLE needs WINDOW. */
        if ((s.caps & AAR_DISPCAP_RESIZE) != 0u && (s.caps & AAR_DISPCAP_WINDOW) == 0u &&
            conf_fail_first(&t))
            conf_buf_str(&t.why, "AAR_DISPCAP_RESIZE was granted without AAR_DISPCAP_WINDOW");
        if (flags == AAR_DISPLAY_RESIZABLE && (s.caps & AAR_DISPCAP_RESIZE) != 0u && conf_fail_first(&t))
            conf_buf_str(&t.why, "RESIZABLE without WINDOW was granted AAR_DISPCAP_RESIZE");
        if (flags == ALL_NEW_FLAGS) {
            conf_buf_str(&t.note, "all requests: caps ");
            conf_buf_hex(&t.note, s.caps);
        }
        (void)aar_display_close(s.id);
    }
    conf_end(&t);
}

/* C-DISPLAY.21 */
static void test_info(void)
{
    struct conf_test t;
    aar_surface s;
    struct {
        aar_display_info i;
        uint8_t tail[16];
    } big;
    uint8_t raw[sizeof(aar_display_info) + 8];
    uint32_t k;
    int rc;

    conf_begin(&t, "C-DISPLAY.21");
    if (!open_s(&t, &s, 160, 100, AAR_FMT_RGB565,
                AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE)) {
        conf_end(&t);
        return;
    }
    fill_bytes_zero(&big, 0xAAu, sizeof big);
    big.i.size = (uint32_t)sizeof big;
    rc = aar_display_get_info(s.id, &big.i);
    if (not_served(&t, rc, "display_info")) {
        close_s(&t, &s);
        conf_end(&t);
        return;
    }
    if (expect_rc(&t, rc, AAR_OK, "display_info of the open display")) {
        conf_check(&t, big.i.width == s.width && big.i.height == s.height, "width and height differ from the surface");
        conf_check(&t, (big.i.flags & ~(uint32_t)(AAR_DISPINFO_WINDOW | AAR_DISPINFO_SCALED | AAR_DISPINFO_ACTIVE)) == 0u,
                   "flags has an undefined bit");
        conf_check(&t, ((big.i.flags & AAR_DISPINFO_WINDOW) != 0u) == ((s.caps & AAR_DISPCAP_WINDOW) != 0u),
                   "AAR_DISPINFO_WINDOW and AAR_DISPCAP_WINDOW disagree");
        conf_check(&t, ((big.i.flags & AAR_DISPINFO_SCALED) != 0u) == ((s.caps & AAR_DISPCAP_SCALED) != 0u),
                   "AAR_DISPINFO_SCALED and AAR_DISPCAP_SCALED disagree");
        conf_check(&t, big.i.screen_width > 0u && big.i.screen_height > 0u, "the screen size is 0");
        conf_check(&t, big.i.reserved[0] == 0u && big.i.reserved[1] == 0u && big.i.reserved[2] == 0u &&
                           big.i.reserved[3] == 0u && big.i.reserved[4] == 0u, "reserved is not 0");
        for (k = 0; k < sizeof big.tail; k++) {
            if (big.tail[k] != 0u) {
                conf_check(&t, 0, "the bytes past the structure, inside size, were not zeroed");
                break;
            }
        }
        conf_buf_u64(&t.note, big.i.width);
        conf_buf_str(&t.note, "x");
        conf_buf_u64(&t.note, big.i.height);
        conf_buf_str(&t.note, " on a screen of ");
        conf_buf_u64(&t.note, big.i.screen_width);
        conf_buf_str(&t.note, "x");
        conf_buf_u64(&t.note, big.i.screen_height);
        conf_buf_str(&t.note, ", ");
        conf_buf_u64(&t.note, big.i.refresh_mhz);
        conf_buf_str(&t.note, " mHz");
    }
    fill_bytes_zero(raw, 0xAAu, sizeof raw);
    ((aar_display_info *)(void *)raw)->size = 8u;
    if (expect_rc(&t, aar_display_get_info(s.id, (aar_display_info *)(void *)raw), AAR_OK,
                  "display_info with size 8")) {
        for (k = 8; k < sizeof raw; k++) {
            if (raw[k] != 0xAAu) {
                conf_check(&t, 0, "display_info wrote past the size the caller set");
                break;
            }
        }
    }
    big.i.size = (uint32_t)sizeof big.i;
    expect_rc(&t, aar_display_get_info(s.id + 1u, &big.i), AAR_E_INVAL, "display_info of an id that is not open");
    close_s(&t, &s);
    big.i.size = (uint32_t)sizeof big.i;
    expect_rc(&t, aar_display_get_info(s.id, &big.i), AAR_E_INVAL, "display_info after display_close");
    conf_end(&t);
}

/* C-DISPLAY.22 */
static void test_modes(void)
{
    struct conf_test t;
    aar_display_mode m[32];
    uint32_t n = 0, n0 = 0, shown, i, preferred = 0;
    int rc;

    conf_begin(&t, "C-DISPLAY.22");
    fill_bytes_zero(m, 0xAAu, sizeof m);
    rc = aar_display_modes(m, 32u, &n);
    if (not_served(&t, rc, "display_modes")) {
        conf_end(&t);
        return;
    }
    if (rc != AAR_OK && rc != AAR_E_NOSPC) {
        expect_rc(&t, rc, AAR_OK, "display_modes");
        conf_end(&t);
        return;
    }
    conf_check(&t, (rc == AAR_E_NOSPC) == (n > 32u), "NOSPC and the count of modes disagree");
    shown = n < 32u ? n : 32u;
    for (i = 0; i < shown; i++) {
        if (m[i].width < 1u || m[i].height < 1u)
            conf_check(&t, 0, "a mode has a width or height of 0");
        if (m[i].formats == 0u || (m[i].formats & ~(uint32_t)((1u << AAR_FMT_RGB565) | (1u << AAR_FMT_BGRA8888) |
                                                             (1u << AAR_FMT_CLUT8))) != 0u)
            conf_check(&t, 0, "a mode has no format, or a bit that is not a format");
        if ((m[i].flags & ~(uint32_t)AAR_MODEF_PREFERRED) != 0u || m[i].reserved != 0u)
            conf_check(&t, 0, "a mode has an undefined flag or reserved is not 0");
        if (m[i].flags & AAR_MODEF_PREFERRED)
            preferred++;
    }
    conf_check(&t, preferred <= 1u, "more than one mode is preferred");
    /* With no room the call counts and writes nothing. */
    rc = aar_display_modes(m, 0u, &n0);
    expect_rc(&t, rc, n > 0u ? AAR_E_NOSPC : AAR_OK, "display_modes with max 0");
    conf_check(&t, n0 == n, "the count with max 0 differs from the count with room");
    conf_buf_u64(&t.note, n);
    conf_buf_str(&t.note, " modes");
    conf_end(&t);
}

/* C-DISPLAY.23 */
static void test_title(void)
{
    struct conf_test t;
    aar_surface s;
    char title[260];
    uint32_t i;
    int rc;

    conf_begin(&t, "C-DISPLAY.23");
    if (!open_s(&t, &s, 160, 100, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW)) {
        conf_end(&t);
        return;
    }
    rc = aar_display_set_title(s.id, "AAR conformance");
    if (not_served(&t, rc, "display_set_title")) {
        close_s(&t, &s);
        conf_end(&t);
        return;
    }
    expect_rc(&t, rc, AAR_OK, "display_set_title");
    expect_rc(&t, aar_display_set_title(s.id, "caf\xC3\xA9 \xE4\xB8\xAD"), AAR_OK, "display_set_title with UTF-8");
    for (i = 0; i < 255u; i++)
        title[i] = 'T';
    title[255] = '\0';
    expect_rc(&t, aar_display_set_title(s.id, title), AAR_OK, "display_set_title of 255 bytes");
    title[255] = 'T';
    title[256] = '\0';
    expect_rc(&t, aar_display_set_title(s.id, title), AAR_E_INVAL, "display_set_title of 256 bytes");
    close_s(&t, &s);
    conf_buf_str(&t.note, "255 bytes are taken, 256 are AAR_E_INVAL");
    conf_end(&t);
}

/* C-DISPLAY.24 */
static void test_pointer(void)
{
    struct conf_test t;
    aar_surface s;
    static uint32_t px[64 * 64];
    aar_pointer p;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-DISPLAY.24");
    if (!open_s(&t, &s, 160, 100, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW)) {
        conf_end(&t);
        return;
    }
    for (i = 0; i < 64u * 64u; i++)
        px[i] = (i & 1u) ? 0xFFFF0000u : 0x00000000u;
    rc = aar_display_pointer(s.id, AAR_POINTER_HIDDEN, 0);
    if (not_served(&t, rc, "display_pointer")) {
        close_s(&t, &s);
        conf_end(&t);
        return;
    }
    expect_rc(&t, rc, AAR_OK, "display_pointer HIDDEN");
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_BUSY, 0), AAR_OK, "display_pointer BUSY");
    p.width = 16;
    p.height = 16;
    p.hot_x = 8;
    p.hot_y = 8;
    p.reserved[0] = 0;
    p.reserved[1] = 0;
    p.argb = px;
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &p), AAR_OK, "display_pointer CUSTOM 16x16");
    p.width = AAR_POINTER_MAX_DIM;
    p.height = AAR_POINTER_MAX_DIM;
    p.hot_x = 63;
    p.hot_y = 63;
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &p), AAR_OK, "display_pointer CUSTOM 64x64, hot spot in the corner");
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_CUSTOM, 0), AAR_E_INVAL, "display_pointer CUSTOM with no image");
    p.width = 0;
    p.height = 16;
    p.hot_x = 0;
    p.hot_y = 0;
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &p), AAR_E_INVAL, "display_pointer CUSTOM of width 0");
    p.width = 65;
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &p), AAR_E_INVAL, "display_pointer CUSTOM of width 65");
    p.width = 16;
    p.height = 65;
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &p), AAR_E_INVAL, "display_pointer CUSTOM of height 65");
    p.height = 16;
    p.hot_x = 16;
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &p), AAR_E_INVAL, "display_pointer CUSTOM with hot_x outside the image");
    p.hot_x = 0;
    p.hot_y = -1;
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &p), AAR_E_INVAL, "display_pointer CUSTOM with hot_y -1");
    expect_rc(&t, aar_display_pointer(s.id, 4u, 0), AAR_E_INVAL, "display_pointer of mode 4");
    expect_rc(&t, aar_display_pointer(s.id, 0xFFFFFFFFu, 0), AAR_E_INVAL, "display_pointer of mode 2^32-1");
    expect_rc(&t, aar_display_pointer(s.id, AAR_POINTER_DEFAULT, 0), AAR_OK, "display_pointer DEFAULT");
    close_s(&t, &s);
    conf_buf_str(&t.note, "the four modes, bad images and modes are AAR_E_INVAL");
    conf_end(&t);
}

/* C-DISPLAY.25 */
static void test_resize(void)
{
    struct conf_test t;
    aar_surface s, r;
    aar_display_info info;
    int rc;

    conf_begin(&t, "C-DISPLAY.25");
    /* Without the cap the call is AAR_E_NOSYS. */
    if (open_s(&t, &s, 160, 100, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        rc = aar_display_resize(s.id, 80, 50, &r);
        expect_rc(&t, rc, AAR_E_NOSYS, "display_resize without AAR_DISPCAP_RESIZE");
        close_s(&t, &s);
    }
    if (!open_s(&t, &s, 160, 100, AAR_FMT_RGB565,
                AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE)) {
        conf_end(&t);
        return;
    }
    if ((s.caps & AAR_DISPCAP_RESIZE) == 0u) {
        conf_buf_str(&t.note, "AAR_DISPCAP_RESIZE was not granted, so display_resize is AAR_E_NOSYS only");
        close_s(&t, &s);
        conf_end(&t);
        return;
    }
    expect_rc(&t, aar_display_resize(s.id, 0, 100, &r), AAR_E_INVAL, "display_resize to width 0");
    expect_rc(&t, aar_display_resize(s.id, 100, 0, &r), AAR_E_INVAL, "display_resize to height 0");
    expect_rc(&t, aar_display_resize(s.id, 0xFFFFFFFFu, 100, &r), AAR_E_INVAL, "display_resize to width 2^32-1");
    expect_rc(&t, aar_display_resize(s.id, 100, 0xFFFFFFFFu, &r), AAR_E_INVAL, "display_resize to height 2^32-1");
    rc = aar_display_resize(s.id, 200, 120, &r);
    if (expect_rc(&t, rc, AAR_OK, "display_resize to 200x120")) {
        conf_check(&t, r.width == 200u && r.height == 120u && r.id == s.id && r.format == s.format,
                   "the refreshed surface has another size, id or format");
        conf_check(&t, r.back != 0 && writable(r.back, r.pitch * r.height), "the new back buffer is not writable");
        conf_check(&t, aar_display_flip(r.id) == AAR_OK, "flip after the resize");
        info.size = (uint32_t)sizeof info;
        if (aar_display_get_info(r.id, &info) == AAR_OK)
            conf_check(&t, info.width == 200u && info.height == 120u, "display_info does not report the new size");
        s = r;
    }
    close_s(&t, &s);
    conf_buf_str(&t.note, "AAR_E_NOSYS without the cap; with it a bad size is AAR_E_INVAL and 200x120 is taken");
    conf_end(&t);
}

/* C-DISPLAY.26 */
static void test_vblank(void)
{
    struct conf_test t;
    aar_surface s;
    uint64_t t0;
    int rc;

    conf_begin(&t, "C-DISPLAY.26");
    rc = aar_display_wait_vblank(100000u);
    if (rc != AAR_E_INVAL && rc != AAR_E_NOSYS && conf_fail_first(&t))
        cmio_why_rc(&t, "display_wait_vblank with no display open (AAR_E_INVAL or AAR_E_NOSYS expected)", rc);
    if (!open_s(&t, &s, 160, 100, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        conf_end(&t);
        return;
    }
    rc = aar_display_wait_vblank(0u);
    if (rc != AAR_E_INVAL && rc != AAR_E_NOSYS && conf_fail_first(&t))
        cmio_why_rc(&t, "display_wait_vblank with a timeout of 0 (AAR_E_INVAL or AAR_E_NOSYS expected)", rc);
    t0 = aar_time_us();
    rc = aar_display_wait_vblank(500000u);
    if (rc == AAR_E_NOSYS) {
        conf_buf_str(&t.note, "AAR_E_NOSYS, the client cannot see a vertical blank");
    } else if (expect_rc(&t, rc, AAR_OK, "display_wait_vblank with a timeout of 500 ms")) {
        uint64_t dt = aar_time_us() - t0;

        conf_check(&t, dt < 600000u, "the wait lasted longer than its timeout");
        conf_buf_str(&t.note, "a vertical blank after ");
        conf_buf_u64(&t.note, dt / 1000u);
        conf_buf_str(&t.note, " ms");
    }
    t0 = aar_time_us();
    rc = aar_display_wait_vblank(1u);
    if (rc != AAR_OK && rc != AAR_E_TIMEDOUT && rc != AAR_E_NOSYS && conf_fail_first(&t))
        cmio_why_rc(&t, "display_wait_vblank with a timeout of 1 us", rc);
    conf_check(&t, aar_time_us() - t0 < 200000u, "a wait of 1 us lasted over 200 ms");
    close_s(&t, &s);
    conf_end(&t);
}

/* C-DISPLAY.29 */
static void test_new_served(void)
{
    struct conf_test t;
    aar_surface s;
    aar_display_info info;
    aar_display_mode m;
    uint32_t n = 0;
    uint64_t t0;
    int rc;

    conf_begin(&t, "C-DISPLAY.29");
    if (!open_s(&t, &s, 160, 100, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW)) {
        conf_end(&t);
        return;
    }
    info.size = (uint32_t)sizeof info;
    conf_check(&t, aar_display_get_info(s.id, &info) != AAR_E_NOSYS, "display_info is not served");
    conf_check(&t, aar_display_modes(&m, 1u, &n) != AAR_E_NOSYS, "display_modes is not served");
    conf_check(&t, aar_display_set_title(s.id, "AAR") != AAR_E_NOSYS, "display_set_title is not served");
    conf_check(&t, aar_display_pointer(s.id, AAR_POINTER_DEFAULT, 0) != AAR_E_NOSYS, "display_pointer is not served");
    t0 = aar_time_us();
    rc = aar_display_wait_vblank(500000u);
    if (rc != AAR_OK && rc != AAR_E_TIMEDOUT && conf_fail_first(&t))
        cmio_why_rc(&t, "display_wait_vblank", rc);
    conf_buf_str(&t.note, s.caps & AAR_DISPCAP_WINDOW ? "window: " : "screen fallback: ");
    if (rc == AAR_OK) {
        conf_buf_str(&t.note, "vertical blank after ");
        conf_buf_u64(&t.note, (aar_time_us() - t0) / 1000u);
        conf_buf_str(&t.note, " ms");
    } else if (rc == AAR_E_TIMEDOUT) {
        conf_buf_str(&t.note, "vertical blank wait timed out");
    }
    close_s(&t, &s);
    conf_end(&t);
}

/* C-DISPLAY.27: the time of the last present. Runtime only, so it is judged with the runner's own display. */
static uint64_t present_at_start;

static void test_last_present(void)
{
    struct conf_test t;
    aar_surface s;
    uint64_t t0, t1, p1, p2;
    uint32_t i;

    conf_begin(&t, "C-DISPLAY.27");
    conf_check(&t, present_at_start == 0u, "display_last_present_us is not 0 before the first flip");
    if (open_s(&t, &s, 64, 64, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC)) {
        for (i = 0; s.back && i < s.pitch * s.height; i++)
            ((uint8_t *)s.back)[i] = 0x55u;
        t0 = aar_time_us();
        if (aar_display_flip(s.id) != AAR_OK) {
            conf_check(&t, 0, "the flip failed");
        } else {
            t1 = aar_time_us();
            p1 = aar_display_last_present_us();
            conf_check(&t, p1 >= t0 && p1 <= t1, "after a flip the time is not between the start and the end of the flip");
            (void)aar_display_refresh(&s);
            aar_sleep_us(5000u);
            if (aar_display_flip(s.id) == AAR_OK) {
                p2 = aar_display_last_present_us();
                conf_check(&t, p2 >= p1 && p2 <= aar_time_us(), "the time of the second flip went back or is in the future");
                conf_buf_str(&t.note, "present time ");
                conf_buf_u64(&t.note, p1);
                conf_buf_str(&t.note, " then ");
                conf_buf_u64(&t.note, p2);
            }
        }
        close_s(&t, &s);
    }
    conf_end(&t);
}

static void test_present_feedback(void)
{
    struct conf_test t;
    aar_surface s;
    aar_present_result result;
    uint64_t previous = 0;
    uint64_t id = 1;
    unsigned i, count;

    conf_begin(&t, "C-DISPLAY.34");
    if (!open_s(&t, &s, 160, 100, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW)) {
        conf_end(&t);
        return;
    }
    if (!(s.caps & AAR_DISPCAP_PRESENT_FEEDBACK)) {
        conf_check(&t, aar_display_present(s.id, 0, &id) == AAR_E_NOSYS && !id,
                   "present without feedback support was not refused");
        conf_buf_str(&t.note, "feedback not granted");
        goto done;
    }
    conf_check(&t, aar_display_present_poll(&result) == 0, "result before any tracked frame");
    conf_check(&t, aar_display_present(s.id, AAR_PRESENT_GL | AAR_PRESENT_ASYNC, &id) == AAR_E_INVAL && !id,
               "conflicting present flags were not refused");
    count = (s.caps & AAR_DISPCAP_ASYNC) ? 2u : 1u;
    for (i = 0; i < count; ++i) {
        uint64_t started;
        int rc;

        if (!s.back || aar_display_refresh(&s) != AAR_OK || !s.back) {
            conf_check(&t, 0, "cannot refresh the drawing buffer");
            break;
        }
        fill_bytes_zero(s.back, i ? 0xaau : 0x55u, s.pitch * s.height);
        started = aar_time_us();
        rc = aar_display_present(s.id, i ? AAR_PRESENT_ASYNC : 0u, &id);
        if (!expect_rc(&t, rc, AAR_OK, "tracked buffer present"))
            break;
        conf_check(&t, id != 0u && id != previous, "invalid or duplicate frame ID");
        previous = id;
        do {
            rc = aar_display_present_poll(&result);
            if (rc != 0)
                break;
            aar_sleep_us(1000u);
        } while (aar_time_us() - started < 1000000u && !aar_should_stop());
        if (!expect_rc(&t, rc, 1, "collect the tracked frame"))
            break;
        conf_check(&t, result.frame_id == id &&
                      result.outcome == AAR_PRESENT_ACKNOWLEDGED &&
                      result.reason == AAR_PRESENT_REASON_NONE,
                   "normal flip did not return its acknowledgement");
        conf_check(&t, result.completed_us >= started &&
                      result.completed_us <= aar_time_us(),
                   "result time is outside the submission-to-collection interval");
    }
done:
    close_s(&t, &s);
    if (s.caps & AAR_DISPCAP_PRESENT_FEEDBACK)
        conf_check(&t, aar_display_present_poll(&result) == 0, "frame result returned more than once");
    conf_end(&t);
}

static void test_latest_feedback(void)
{
    struct conf_test t;
    aar_surface s;
    uint64_t id[4], started[4], last_started = 0;
    unsigned accepted = 0, seen = 0, drained = 0, replaced = 0;
    uint32_t dropped, flags;

    conf_begin(&t, "C-DISPLAY.35");
    if (!open_s(&t, &s, 320, 200, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST))
        goto done;
    if ((s.caps & (AAR_DISPCAP_LATEST | AAR_DISPCAP_PRESENT_FEEDBACK)) !=
                  (AAR_DISPCAP_LATEST | AAR_DISPCAP_PRESENT_FEEDBACK)) {
        conf_buf_str(&t.note, "LATEST or feedback not granted");
        goto close;
    }
    flags = (s.caps & AAR_DISPCAP_ASYNC) ? AAR_PRESENT_ASYNC : 0u;
    dropped = aar_display_latest_dropped();
    while (accepted < sizeof(id) / sizeof(id[0])) {
        int rc = aar_display_refresh(&s);

        if (!conf_check(&t, rc == AAR_OK && s.back, "could not refresh the LATEST buffer"))
            goto close;
        fill_bytes_zero(s.back, 0x20u * accepted, s.pitch * s.height);
        last_started = started[accepted] = aar_time_us();
        if (!expect_rc(&t, aar_display_present(s.id, flags, &id[accepted]), AAR_OK,
                       "tracked LATEST flip"))
            goto close;
        ++accepted;
    }
    while (drained < accepted) {
        aar_present_result result;
        unsigned i;
        int rc = aar_display_present_poll(&result);

        if (rc == 0 && aar_time_us() - last_started < 1000000u && !aar_should_stop()) {
            aar_sleep_us(1000);
            continue;
        }
        if (!expect_rc(&t, rc, 1, "collect the LATEST result"))
            goto close;
        for (i = 0; i < accepted && id[i] != result.frame_id; ++i)
            ;
        if (!conf_check(&t, result.frame_id && i < accepted && !(seen & (1u << i)),
                        "unexpected or duplicate LATEST frame ID"))
            goto close;
        seen |= 1u << i;
        ++drained;
        replaced += result.outcome == AAR_PRESENT_REPLACED;
        conf_check(&t, result.reason == AAR_PRESENT_REASON_NONE &&
                      (result.outcome == AAR_PRESENT_ACKNOWLEDGED ||
                       (result.outcome == AAR_PRESENT_REPLACED && i + 1u < accepted)),
                   "LATEST frame failed, or final frame was not acknowledged");
        conf_check(&t, result.completed_us >= started[i] && result.completed_us <= aar_time_us(),
                   "LATEST result time is outside the submission-to-collection interval");
    }
    conf_check(&t, replaced == aar_display_latest_dropped() - dropped,
               "replacement count differs from tracked results");
    conf_buf_str(&t.note, "frames replaced: ");
    conf_buf_u64(&t.note, replaced);
close:
    close_s(&t, &s);
done:
    conf_end(&t);
}

/* Variant window: the rows above, with the client's own display (a window on a public screen where the client
 * can show one), because they ask the client. The variant std has the runner's display table, which
 * serves none of the display calls of API 0.6. */
static void variant_window(void)
{
    test_new_flags();
    test_info();
    test_modes();
    test_title();
    test_pointer();
    test_resize();
    test_vblank();
    test_new_served();
    test_present_feedback();
}

/* Variants pointer and resize (the operator rows C-DISPLAY.30 and C-DISPLAY.31, only with INTERACTIVE). */
static void paint(const aar_surface *s, uint32_t k)
{
    uint32_t x, y;

    for (y = 0; y < s->height; y++) {
        for (x = 0; x < s->width; x++) {
            uint32_t r = (x * 255u) / (s->width ? s->width : 1u);
            uint32_t g = (y * 255u) / (s->height ? s->height : 1u);

            (void)cd_put_pixel((uint8_t *)s->back + y * s->pitch + x * 2u, s->layout, r, g, k & 255u);
        }
    }
}

static void variant_pointer(int argc, char **argv)
{
    static uint32_t px[16 * 16];
    aar_surface s;
    aar_pointer p;
    uint32_t i;
    uint32_t step;

    if (!conf_arg(argc, argv, "conf:show"))
        return;
    if (aar_display_open(320, 240, AAR_FMT_RGB565,
                         AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE, &s) != AAR_OK || !s.back)
        return;
    (void)aar_display_set_title(s.id, "AAR conformance: title and pointer");
    paint(&s, 128u);
    (void)aar_display_flip(s.id);
    for (i = 0; i < 256u; i++)
        px[i] = ((i % 16u) < 2u || (i / 16u) < 2u || (i % 16u) > 13u || (i / 16u) > 13u) ? 0xFF000000u : 0xFFFF0000u;
    p.width = 16;
    p.height = 16;
    p.hot_x = 8;
    p.hot_y = 8;
    p.reserved[0] = 0;
    p.reserved[1] = 0;
    p.argb = px;
    for (step = 0; step < 4u && !aar_should_stop(); step++) {
        if (step == 0u)
            (void)aar_display_pointer(s.id, AAR_POINTER_DEFAULT, 0);
        else if (step == 1u)
            (void)aar_display_pointer(s.id, AAR_POINTER_HIDDEN, 0);
        else if (step == 2u)
            (void)aar_display_pointer(s.id, AAR_POINTER_BUSY, 0);
        else
            (void)aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &p);
        for (i = 0; i < 40u && !aar_should_stop(); i++)
            aar_sleep_us(100000u);
    }
    (void)aar_display_pointer(s.id, AAR_POINTER_DEFAULT, 0);
    (void)aar_display_close(s.id);
}

/* The runner counts the messages "RSZ" (one per resize the app answered) and "NOCP" (the cap was not granted). */
static void send_tag(const char *tag, uint32_t a, uint32_t b)
{
    uint8_t m[AAR_MSG_BYTES];
    uint32_t i;

    for (i = 0; i < AAR_MSG_BYTES; i++)
        m[i] = 0;
    for (i = 0; tag[i] && i < 8u; i++)
        m[i] = (uint8_t)tag[i];
    cm_put32(m, 8, a);
    cm_put32(m, 12, b);
    (void)cmio_send(m, 500000u);
}

static void variant_resize(int argc, char **argv)
{
    aar_surface s;
    uint64_t start;

    if (!conf_arg(argc, argv, "conf:show"))
        return;
    if (aar_display_open(320, 240, AAR_FMT_RGB565,
                         AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, &s) != AAR_OK || !s.back)
        return;
    if ((s.caps & AAR_DISPCAP_RESIZE) == 0u) {
        send_tag("NOCP", s.caps, 0);
        (void)aar_display_close(s.id);
        return;
    }
    (void)aar_display_set_title(s.id, "AAR conformance: resize me");
    paint(&s, 64u);
    (void)aar_display_flip(s.id);
    start = aar_time_us();
    while (aar_time_us() - start < 25000000u && !aar_should_stop()) {
        aar_event ev[4];
        uint32_t got = 0;
        uint32_t i;

        if (aar_event_poll(ev, 4, &got) != AAR_OK)
            break;
        for (i = 0; i < got; i++) {
            aar_surface r;

            if (ev[i].type != AAR_EV_RESIZE || ev[i].dx < 1 || ev[i].dy < 1)
                continue;
            if (aar_display_resize(s.id, (uint32_t)ev[i].dx, (uint32_t)ev[i].dy, &r) != AAR_OK || !r.back) {
                send_tag("RSZBAD", (uint32_t)ev[i].dx, (uint32_t)ev[i].dy);
                continue;
            }
            s = r;
            paint(&s, 64u + 17u * i);
            (void)aar_display_flip(s.id);
            send_tag("RSZ", s.width, s.height);
        }
        if (!got)
            aar_sleep_us(5000u);
    }
    (void)aar_display_close(s.id);
}

/* Variant picture (C-DISPLAY.17, INTERACTIVE only): four colours on the screen. */
static void variant_picture(int argc, char **argv)
{
    aar_surface s;
    uint32_t x;
    uint32_t y;
    uint32_t n;

    if (!conf_arg(argc, argv, "conf:show"))
        return;
    if (aar_display_open(320, 240, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, &s) != AAR_OK || !s.back)
        return;
    for (y = 0; y < 240u; y++) {
        for (x = 0; x < 320u; x++) {
            uint32_t r, g, b;

            cd_quad_colour((y >= 120u ? 2u : 0u) + (x >= 160u ? 1u : 0u), &r, &g, &b);
            (void)cd_put_pixel((uint8_t *)s.back + y * s.pitch + x * 2u, s.layout, r, g, b);
        }
    }
    (void)aar_display_flip(s.id);
    for (n = 0; n < 50u && !aar_should_stop(); n++)
        aar_sleep_us(100000u);
    (void)aar_display_close(s.id);
}

int aar_main(int argc, char **argv)
{
    const char *variant = conf_arg(argc, argv, "conf:variant");

    have_peer = conf_have_peer(argc, argv);
    present_at_start = aar_display_last_present_us();
    if (variant && cmio_streq(variant, "window")) {
        variant_window();
        return conf_finish("C-DISPLAY");
    }
    if (variant && cmio_streq(variant, "present-latest")) {
        test_latest_feedback();
        return conf_finish("C-DISPLAY");
    }
    if (variant && cmio_streq(variant, "pointer")) {
        variant_pointer(argc, argv);
        return conf_finish("C-DISPLAY");
    }
    if (variant && cmio_streq(variant, "resize")) {
        variant_resize(argc, argv);
        return conf_finish("C-DISPLAY");
    }
    if (variant && cmio_streq(variant, "picture")) {
        variant_picture(argc, argv);
        return conf_finish("C-DISPLAY");
    }
    test_open();
    test_layout();
    test_refusals();
    test_one_display();
    test_close();
    test_flip_ids();
    test_refresh();
    test_async();
    test_latest_open();
    test_latest_flips();
    test_vsync();
    test_flip_publishes();
    test_pixel_layout();
    test_pitch();
    test_palette();
    test_palette_refused();
    test_last_present();
    if (!have_peer) {
        test_present_feedback();
        test_latest_feedback();
    }
    return conf_finish("C-DISPLAY");
}
