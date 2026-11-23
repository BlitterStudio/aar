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

AAR_NOTE(0, 0, AAR_GROUP_DISPLAY, 0)

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
            conf_check(&t, (s.caps & ~7u) == 0, "caps holds a bit that is not defined");
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
        if (rc != AAR_E_INVAL && rc != AAR_E_NOSYS && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "format ");
            conf_buf_u64(&t.why, bad_formats[i]);
            conf_buf_str(&t.why, " returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_INVAL or AAR_E_NOSYS");
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
 * front (when there is one) holds the frame just presented [spec question 16]. */
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
         * ZZ9000 yields once), so a floor on the time is not a requirement [spec question 65];
         * the ceiling still catches a call that hangs. */
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
    return conf_finish("C-DISPLAY");
}
