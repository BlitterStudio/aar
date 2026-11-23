/*
 * Conformance payload for C-AUDIO (spec 8.4, 9.6): the audio group from the app: the
 * grant, the ring (its place, its indices, the underrun count), the stats call and the
 * close. Whether the runtime consumes the ring at the granted rate is measured here too
 * (C-AUDIO.5). The route AHI, where the runner reads the PCM with AARAudioRead
 * (C-AUDIO.10, C-AUDIO.11), and the sound itself (C-AUDIO.12, an operator row) are
 * variants; see conf_audio.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"
#include "aar/transport.h"

#include "conf.h"
#include "conf_audio.h"
#include "conf_msgio.h"

AAR_NOTE(0, 0, AAR_GROUP_AUDIO, 0)

static int have_peer;

static int rate_ok(uint32_t rate)
{
    return rate == 22050u || rate == 44100u || rate == 48000u;
}

/* A word of the ring header (little-endian, in the app's own byte order), by byte offset. */
static uint32_t ring_word(const aar_audio *a, uint32_t off)
{
    return *(const volatile uint32_t *)((const uint8_t *)a->write_frame + off);
}

/* Opens a stream and records the checks of a grant in the test. Returns 1 when open. */
static int open_stream(struct conf_test *t, aar_audio *a, uint32_t rate)
{
    int rc;
    uint32_t i;

    for (i = 0; i < sizeof *a; i++)
        ((uint8_t *)a)[i] = 0;
    rc = aar_audio_open(rate, 2, a);
    if (rc != AAR_OK) {
        if (conf_fail_first(t)) {
            conf_buf_str(&t->why, "audio_open of ");
            conf_buf_u64(&t->why, rate);
            conf_buf_str(&t->why, " Hz stereo returned ");
            conf_buf_i64(&t->why, rc);
        }
        return 0;
    }
    return 1;
}

static void close_stream(struct conf_test *t)
{
    int rc = aar_audio_close();

    if (rc != AAR_OK && conf_fail_first(t))
        cmio_why_rc(t, "audio_close", rc);
}

/* ---- C-AUDIO.1 to C-AUDIO.4: the grant ------------------------------------------------------- */

static void test_grant(void)
{
    struct conf_test t;
    aar_audio a;

    conf_begin(&t, "C-AUDIO.1");
    if (open_stream(&t, &a, 22050u)) {
        conf_check(&t, a.channels == 2u, "the grant does not have 2 channels");
        conf_check(&t, rate_ok(a.rate), "the granted rate is not 22050, 44100 or 48000");
        conf_check(&t, a.frames != 0 && (a.frames & (a.frames - 1u)) == 0,
                   "frames is not a power of two");
        conf_check(&t, a.flags == 0, "flags is not 0");
        conf_check(&t, a.samples != 0 && a.read_frame != 0 && a.write_frame != 0,
                   "samples, read_frame or write_frame is NULL");
        conf_buf_u64(&t.note, a.rate);
        conf_buf_str(&t.note, " Hz, ");
        conf_buf_u64(&t.note, a.frames);
        conf_buf_str(&t.note, " frames");
        close_stream(&t);
    }
    conf_end(&t);
}

static void test_ring_header(void)
{
    struct conf_test t;
    aar_audio a;

    conf_begin(&t, "C-AUDIO.2");
    if (open_stream(&t, &a, 22050u)) {
        if (a.samples && a.read_frame && a.write_frame) {
            const uint8_t *wf = (const uint8_t *)a.write_frame;

            conf_check(&t, (const uint8_t *)a.read_frame == wf + AAR_AUDIO_RING_READ_FRAME,
                       "read_frame is not write_frame plus 0x40 bytes");
            conf_check(&t, (const uint8_t *)a.samples == wf + AAR_AUDIO_RING_HDR_BYTES,
                       "samples is not write_frame plus 0x80 bytes");
            conf_check(&t, ring_word(&a, AAR_AUDIO_RING_RATE) == a.rate,
                       "the ring header rate differs from the grant");
            conf_check(&t, ring_word(&a, AAR_AUDIO_RING_CHANNELS) == a.channels,
                       "the ring header channels differ from the grant");
            conf_check(&t, ring_word(&a, AAR_AUDIO_RING_FORMAT) == AAR_AUDIO_FORMAT_S16LE,
                       "the ring header format is not S16LE");
        } else {
            conf_check(&t, 0, "the grant has a NULL pointer");
        }
        close_stream(&t);
    }
    conf_end(&t);
}

static void test_other_rates(void)
{
    struct conf_test t;
    static const uint32_t rates[] = { 44100u, 48000u };
    uint32_t i;

    conf_begin(&t, "C-AUDIO.3");
    for (i = 0; i < 2u; i++) {
        aar_audio a;
        int rc;
        uint32_t k;

        for (k = 0; k < sizeof a; k++)
            ((uint8_t *)&a)[k] = 0;
        rc = aar_audio_open(rates[i], 2, &a);
        if (rc == AAR_OK) {
            if ((!rate_ok(a.rate) || a.channels != 2u) && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "the grant for ");
                conf_buf_u64(&t.why, rates[i]);
                conf_buf_str(&t.why, " says ");
                conf_buf_u64(&t.why, a.rate);
                conf_buf_str(&t.why, " Hz, ");
                conf_buf_u64(&t.why, a.channels);
                conf_buf_str(&t.why, " channels");
            }
            (void)aar_audio_close();
            conf_buf_u64(&t.note, rates[i]);
            conf_buf_str(&t.note, " granted as ");
            conf_buf_u64(&t.note, a.rate);
            conf_buf_str(&t.note, "; ");
        } else if (rc == AAR_E_INVAL) {
            conf_buf_u64(&t.note, rates[i]);
            conf_buf_str(&t.note, " refused; ");
        } else if (conf_fail_first(&t)) {
            conf_buf_u64(&t.why, rates[i]);
            conf_buf_str(&t.why, " Hz stereo returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_OK or AAR_E_INVAL");
        }
    }
    conf_end(&t);
}

static void test_bad_requests(void)
{
    struct conf_test t;
    static const uint32_t rate[] = { 8000u, 96000u, 22050u, 22050u, 22050u };
    static const uint32_t channels[] = { 2u, 2u, 1u, 0u, 6u };
    uint32_t i;

    conf_begin(&t, "C-AUDIO.4");
    for (i = 0; i < 5u; i++) {
        aar_audio a;
        uint32_t k;
        int rc;

        for (k = 0; k < sizeof a; k++)
            ((uint8_t *)&a)[k] = 0;
        rc = aar_audio_open(rate[i], channels[i], &a);
        if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "audio_open(");
            conf_buf_u64(&t.why, rate[i]);
            conf_buf_str(&t.why, ", ");
            conf_buf_u64(&t.why, channels[i]);
            conf_buf_str(&t.why, ") returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_INVAL");
        }
        if (rc == AAR_OK)
            (void)aar_audio_close();
    }
    conf_end(&t);
}

/* ---- C-AUDIO.5 and C-AUDIO.6: the runtime consumes the ring ------------------------------------------- */

/* Tops the ring up with silence to half full (the app never goes more than a ring ahead). */
static void feed_silence(const aar_audio *a, uint32_t *w)
{
    uint32_t mask = a->frames - 1u;
    uint32_t ahead = *w - *a->read_frame;
    uint32_t want = a->frames / 2u;
    uint32_t n;
    uint32_t j;

    if (ahead >= want)
        return;
    n = want - ahead;
    if (n > 1024u)
        n = 1024u;
    for (j = 0; j < n; j++) {
        uint32_t idx = (*w + j) & mask;

        a->samples[2u * idx] = 0;
        a->samples[2u * idx + 1u] = 0;
    }
    (void)aar_publish(a->samples, (uint64_t)a->frames * 4u);
    *w += n;
    *a->write_frame = *w;
    (void)aar_publish((const void *)a->write_frame, 4);
}

static void test_consumption(void)
{
    struct conf_test t5;
    struct conf_test t6;
    aar_audio a;
    aar_audio_stats st;
    uint32_t w;
    uint32_t r1 = 0, r2 = 0;
    uint64_t start;
    uint64_t t1 = 0, t2 = 0;
    uint32_t u0, u1, u2;
    uint32_t k;
    int rc;

    conf_begin(&t5, "C-AUDIO.5");
    conf_begin(&t6, "C-AUDIO.6");
    if (!open_stream(&t5, &a, 22050u)) {
        conf_fail_first(&t6);
        conf_buf_str(&t6.why, "the stream did not open");
        conf_end(&t5);
        conf_end(&t6);
        return;
    }
    w = *a.write_frame;
    start = aar_time_us();
    for (;;) {
        uint64_t now = aar_time_us();
        uint32_t ahead = w - *a.read_frame;

        if ((int32_t)ahead < 0 && conf_fail_first(&t5))
            conf_buf_str(&t5.why, "read_frame passed write_frame");
        if (ahead > a.frames && (int32_t)ahead >= 0 && conf_fail_first(&t5))
            conf_buf_str(&t5.why, "the app got more than `frames` ahead of read_frame");
        if (t1 == 0 && now - start >= 300000u) {
            t1 = now;
            r1 = *a.read_frame;
        }
        if (t1 != 0 && now - t1 >= 1000000u) {
            t2 = now;
            r2 = *a.read_frame;
            break;
        }
        if (now - start > 6000000u)
            break;
        feed_silence(&a, &w);
        aar_sleep_us(1000u);
    }
    if (t2 == 0) {
        conf_check(&t5, 0, "the measurement did not finish in 6 s");
    } else {
        uint64_t got = (uint64_t)(r2 - r1) * 100u * 1000000u;
        uint64_t want = (uint64_t)a.rate * (t2 - t1);

        if ((got < want * 75u / 100u || got > want * 125u / 100u) && conf_fail_first(&t5)) {
            conf_buf_str(&t5.why, "read_frame advanced ");
            conf_buf_u64(&t5.why, r2 - r1);
            conf_buf_str(&t5.why, " frames in ");
            conf_buf_u64(&t5.why, t2 - t1);
            conf_buf_str(&t5.why, " us at ");
            conf_buf_u64(&t5.why, a.rate);
            conf_buf_str(&t5.why, " Hz");
        }
        conf_buf_str(&t5.note, "read_frame advanced ");
        conf_buf_u64(&t5.note, r2 - r1);
        conf_buf_str(&t5.note, " frames in ");
        conf_buf_u64(&t5.note, (t2 - t1) / 1000u);
        conf_buf_str(&t5.note, " ms at ");
        conf_buf_u64(&t5.note, a.rate);
        conf_buf_str(&t5.note, " Hz");
    }

    /* C-AUDIO.6: stop writing, and the runtime counts underruns. */
    u0 = ring_word(&a, AAR_AUDIO_RING_UNDERRUNS);
    aar_sleep_us(1000000u);
    u1 = ring_word(&a, AAR_AUDIO_RING_UNDERRUNS);
    for (k = 0; k < sizeof st; k++)
        ((uint8_t *)&st)[k] = 0;
    st.size = sizeof st;
    rc = aar_audio_get_stats(&st);
    u2 = ring_word(&a, AAR_AUDIO_RING_UNDERRUNS);
    conf_check(&t6, u1 > u0, "the underrun count in the ring did not rise while the app wrote nothing");
    if (rc == AAR_OK) {
        if ((st.underruns < u1 || st.underruns > u2) && conf_fail_first(&t6)) {
            conf_buf_str(&t6.why, "audio_get_stats says ");
            conf_buf_u64(&t6.why, st.underruns);
            conf_buf_str(&t6.why, ", the ring says ");
            conf_buf_u64(&t6.why, u1);
            conf_buf_str(&t6.why, " to ");
            conf_buf_u64(&t6.why, u2);
        }
        conf_buf_str(&t6.note, "underruns ");
        conf_buf_u64(&t6.note, u0);
        conf_buf_str(&t6.note, " to ");
        conf_buf_u64(&t6.note, u2);
        conf_buf_str(&t6.note, ", the stats call agrees");
    } else {
        conf_check(&t6, rc == AAR_E_NOSYS, "audio_get_stats returned neither AAR_OK nor AAR_E_NOSYS");
        conf_buf_str(&t6.note, "underruns ");
        conf_buf_u64(&t6.note, u0);
        conf_buf_str(&t6.note, " to ");
        conf_buf_u64(&t6.note, u2);
        conf_buf_str(&t6.note, " (no stats call)");
    }
    close_stream(&t5);
    conf_end(&t5);
    conf_end(&t6);
}

/* ---- C-AUDIO.7 and C-AUDIO.8 -------------------------------------------------------------------------- */

static void test_stats_size(void)
{
    struct conf_test t;
    aar_audio a;
    aar_audio_stats st;
    uint32_t *words = (uint32_t *)&st;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-AUDIO.7");
    if (open_stream(&t, &a, 22050u)) {
        for (i = 0; i < sizeof st / 4u; i++)
            words[i] = 0xA5A5A5A5u;
        st.size = 8;
        rc = aar_audio_get_stats(&st);
        if (rc == AAR_E_NOSYS) {
            conf_buf_str(&t.note, "audio_stats is absent: AAR_E_NOSYS");
        } else if (rc != AAR_OK) {
            conf_check(&t, 0, "audio_get_stats returned neither AAR_OK nor AAR_E_NOSYS");
        } else {
            conf_check(&t, st.size == 8u, "the size field was changed");
            for (i = 2; i < sizeof st / 4u; i++) {
                if (words[i] != 0xA5A5A5A5u && conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "a size of 8 changed word ");
                    conf_buf_u64(&t.why, i);
                    conf_buf_str(&t.why, " of the structure");
                }
            }
            conf_buf_str(&t.note, "a size of 8 filled only size and underruns");
        }
        close_stream(&t);
    }
    conf_end(&t);
}

static void test_close(void)
{
    struct conf_test t;
    aar_audio a;
    int rc;

    conf_begin(&t, "C-AUDIO.8");
    if (open_stream(&t, &a, 22050u)) {
        rc = aar_audio_close();
        conf_check(&t, rc == AAR_OK, "audio_close did not return AAR_OK");
        rc = aar_audio_close();
        conf_check(&t, rc < 0, "a second audio_close was accepted");
        if (open_stream(&t, &a, 22050u))
            close_stream(&t);
    }
    conf_end(&t);
}

/* ---- C-AUDIO.9: one stream per app, a few streams in all --------------------------------------------- */

static void test_busy(void)
{
    struct conf_test t;
    aar_audio a;
    aar_audio b;
    uint32_t k;
    int rc;

    conf_begin(&t, "C-AUDIO.9");
    if (open_stream(&t, &a, 22050u)) {
        for (k = 0; k < sizeof b; k++)
            ((uint8_t *)&b)[k] = 0;
        rc = aar_audio_open(22050u, 2, &b);
        if (rc != AAR_E_BUSY && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "a second audio_open in the same app returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_BUSY");
        }
        if (rc == AAR_OK)
            (void)aar_audio_close();
        if (have_peer) {
            uint32_t ok = 0, opened = 0, refused = 0;

            (void)cmio_kw(CA_K_MORE, 0, 0, 0);
            rc = cmio_result(60000000u, &ok, &opened, 0, 0, &refused, 0);
            if (rc != 1) {
                conf_check(&t, 0, "no answer from the runner");
            } else if (!ok && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "with ");
                conf_buf_u64(&t.why, opened);
                conf_buf_str(&t.why, " other streams open the first refusal was ");
                conf_buf_i64(&t.why, (int32_t)refused);
                conf_buf_str(&t.why, ", not AAR_E_BUSY");
            } else {
                conf_buf_str(&t.note, "second open BUSY; ");
                conf_buf_u64(&t.note, opened);
                conf_buf_str(&t.note, " more apps opened a stream before the first refusal");
            }
        } else {
            conf_buf_str(&t.note, "second open BUSY; no runner for the other apps");
        }
        close_stream(&t);
    }
    conf_end(&t);
}

/* ---- Variants ------------------------------------------------------------------------------------------ */

/* hold: opens a stream, tells the runner how it went and keeps it until it is stopped. */
static void variant_hold(void)
{
    aar_audio a;
    uint8_t m[AAR_MSG_BYTES];
    uint32_t k;
    int rc;

    for (k = 0; k < sizeof a; k++)
        ((uint8_t *)&a)[k] = 0;
    rc = aar_audio_open(22050u, 2, &a);
    ca_hold_marker(m, rc);
    (void)cmio_send(m, 5000000u);
    while (!aar_should_stop())
        (void)aar_msg_wait(1000000u);
    if (rc == AAR_OK)
        (void)aar_audio_close();
}

/* ahi: writes the ramp and waits until the client has read it all. */
static void variant_ahi(void)
{
    aar_audio a;
    uint32_t k;
    uint32_t w;
    uint32_t written = 0;
    uint64_t start;

    for (k = 0; k < sizeof a; k++)
        ((uint8_t *)&a)[k] = 0;
    if (aar_audio_open(22050u, 2, &a) != AAR_OK)
        return;
    w = *a.write_frame;
    start = aar_time_us();
    while (written < CA_RAMP_FRAMES && aar_time_us() - start < 25000000u && !aar_should_stop()) {
        uint32_t space = a.frames - (w - *a.read_frame);
        uint32_t n = CA_RAMP_FRAMES - written;
        uint32_t mask = a.frames - 1u;
        uint32_t j;

        if (n > space)
            n = space;
        if (n > 1024u)
            n = 1024u;
        if (n == 0) {
            aar_sleep_us(1000u);
            continue;
        }
        for (j = 0; j < n; j++) {
            uint32_t idx = (w + j) & mask;

            a.samples[2u * idx] = ca_ramp_left(written + j);
            a.samples[2u * idx + 1u] = ca_ramp_right(written + j);
        }
        (void)aar_publish(a.samples, (uint64_t)a.frames * 4u);
        w += n;
        written += n;
        *a.write_frame = w;
        (void)aar_publish((const void *)a.write_frame, 4);
    }
    while ((int32_t)(*a.read_frame - w) < 0 && aar_time_us() - start < 25000000u &&
           !aar_should_stop())
        aar_sleep_us(1000u);
    (void)aar_audio_close();
}

/* tone: 440 Hz for two seconds (C-AUDIO.12). The sine comes from a two-term recurrence. */
static void variant_tone(int argc, char **argv)
{
    aar_audio a;
    uint32_t k;
    uint32_t w;
    uint32_t total;
    uint32_t written = 0;
    int32_t y1 = 1000;
    int32_t y2 = 0;
    uint64_t start;

    if (!conf_arg(argc, argv, "conf:show"))
        return;
    for (k = 0; k < sizeof a; k++)
        ((uint8_t *)&a)[k] = 0;
    if (aar_audio_open(22050u, 2, &a) != AAR_OK || a.rate != 22050u)
        return;
    total = a.rate * 2u;
    w = *a.write_frame;
    start = aar_time_us();
    while (written < total && aar_time_us() - start < 8000000u && !aar_should_stop()) {
        uint32_t space = a.frames - (w - *a.read_frame);
        uint32_t n = total - written;
        uint32_t mask = a.frames - 1u;
        uint32_t j;

        if (n > space)
            n = space;
        if (n > 512u)
            n = 512u;
        if (n == 0) {
            aar_sleep_us(2000u);
            continue;
        }
        for (j = 0; j < n; j++) {
            uint32_t idx = (w + j) & mask;
            int32_t y0 = ((int32_t)(((int64_t)65024 * y1) >> 15)) - y2;

            y2 = y1;
            y1 = y0;
            a.samples[2u * idx] = (int16_t)y0;
            a.samples[2u * idx + 1u] = (int16_t)y0;
        }
        (void)aar_publish(a.samples, (uint64_t)a.frames * 4u);
        w += n;
        written += n;
        *a.write_frame = w;
        (void)aar_publish((const void *)a.write_frame, 4);
    }
    while ((int32_t)(*a.read_frame - w) < 0 && aar_time_us() - start < 10000000u &&
           !aar_should_stop())
        aar_sleep_us(2000u);
    (void)aar_audio_close();
}

int aar_main(int argc, char **argv)
{
    const char *variant = conf_arg(argc, argv, "conf:variant");

    have_peer = conf_have_peer(argc, argv);
    if (variant && cmio_streq(variant, "hold")) {
        variant_hold();
        return conf_finish("C-AUDIO");
    }
    if (variant && cmio_streq(variant, "ahi")) {
        variant_ahi();
        return conf_finish("C-AUDIO");
    }
    if (variant && cmio_streq(variant, "tone")) {
        variant_tone(argc, argv);
        return conf_finish("C-AUDIO");
    }
    test_grant();
    test_ring_header();
    test_other_rates();
    test_bad_requests();
    test_consumption();
    test_stats_size();
    test_close();
    test_busy();
    return conf_finish("C-AUDIO");
}
