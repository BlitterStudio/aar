/*
 * Conformance payload for C-INPUT (spec 8.3, 9.6, 10.4): events. The runner is the
 * client: it posts events with AARPostEvent and signals its own task for the break
 * events, as the keywords of conf_input.h ask (runner/r_input.c); the app polls them
 * with event_poll and judges what arrived. The rows that need input events are printed
 * only when the platform declares the input group and a runner is present; without a
 * runner (the host runner) the core rows C-INPUT.1 and C-INPUT.2 are the whole payload.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_input.h"
#include "conf_msgio.h"

AAR_NOTE(0, 0, 0, AAR_GROUP_INPUT)

static int have_peer;
static int have_input;
static int saw_resize;
static const char *variant;

/* Polls until want events are in out or timeout_us has passed. Returns how many. */
static uint32_t collect(aar_event *out, uint32_t want, uint32_t timeout_us)
{
    uint32_t n = 0;
    uint64_t start = aar_time_us();

    while (n < want && aar_time_us() - start < timeout_us) {
        uint32_t got = 0;
        uint32_t i;

        if (aar_event_poll(out + n, want - n, &got) != AAR_OK)
            break;
        for (i = 0; i < got; i++) {
            if (out[n + i].type == AAR_EV_RESIZE)
                saw_resize = 1;
        }
        n += got;
        if (!got)
            aar_sleep_us(1000);
    }
    return n;
}

/* Polls until an event of the type shows up (others are skipped). */
static int await_type(uint16_t type, uint32_t timeout_us)
{
    aar_event ev[4];
    uint64_t start = aar_time_us();

    while (aar_time_us() - start < timeout_us) {
        uint32_t got = 0;
        uint32_t i;

        if (aar_event_poll(ev, 4, &got) != AAR_OK)
            return 0;
        for (i = 0; i < got; i++) {
            if (ev[i].type == AAR_EV_RESIZE)
                saw_resize = 1;
            if (ev[i].type == type)
                return 1;
        }
        if (!got)
            aar_sleep_us(1000);
    }
    return 0;
}

static int same(const aar_event *e, const struct ci_ev *w, int with_time)
{
    return e->type == w->type && e->code == w->code && e->dx == w->dx &&
           e->dy == w->dy && e->buttons == w->buttons &&
           (!with_time || e->time_ms == w->time_ms);
}

/* ---- C-INPUT.1 ----------------------------------------------------------------------------- */

static void test_nonblocking(void)
{
    struct conf_test t;
    aar_event ev[4];
    uint64_t t0;
    uint64_t dt;
    uint32_t i;

    conf_begin(&t, "C-INPUT.1");
    t0 = aar_time_us();
    for (i = 0; i < 1000u; i++) {
        uint32_t got = 99;
        int rc = aar_event_poll(ev, 4, &got);

        if ((rc != AAR_OK || got != 0) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "call ");
            conf_buf_u64(&t.why, i);
            conf_buf_str(&t.why, " returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, " with ");
            conf_buf_u64(&t.why, got);
            conf_buf_str(&t.why, " events with nothing pending");
            break;
        }
    }
    dt = aar_time_us() - t0;
    if (dt >= 100000u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "1000 calls took ");
        conf_buf_u64(&t.why, dt);
        conf_buf_str(&t.why, " us");
    }
    conf_buf_str(&t.note, "1000 calls in ");
    conf_buf_u64(&t.note, dt);
    conf_buf_str(&t.note, " us");
    conf_end(&t);
}

/* ---- C-INPUT.3 to C-INPUT.5: keys, mouse, joystick ------------------------------------------ */

static void test_sequence(const char *id, const char *kw, const struct ci_ev *want,
                          uint32_t count, int with_time, const char *what)
{
    struct conf_test t;
    aar_event ev[8];
    uint32_t n;
    uint32_t i;

    conf_begin(&t, id);
    cmio_kw(kw, 0, 0, 0);
    n = collect(ev, count, 3000000u);
    if (n != count && conf_fail_first(&t)) {
        conf_buf_u64(&t.why, n);
        conf_buf_str(&t.why, " of ");
        conf_buf_u64(&t.why, count);
        conf_buf_str(&t.why, " ");
        conf_buf_str(&t.why, what);
        conf_buf_str(&t.why, " events arrived");
    }
    for (i = 0; i < n; i++) {
        if (!same(&ev[i], &want[i], with_time) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, what);
            conf_buf_str(&t.why, " event ");
            conf_buf_u64(&t.why, i);
            conf_buf_str(&t.why, " arrived with type ");
            conf_buf_u64(&t.why, ev[i].type);
            conf_buf_str(&t.why, " code ");
            conf_buf_u64(&t.why, ev[i].code);
            conf_buf_str(&t.why, " dx ");
            conf_buf_i64(&t.why, ev[i].dx);
            conf_buf_str(&t.why, " dy ");
            conf_buf_i64(&t.why, ev[i].dy);
            conf_buf_str(&t.why, " buttons ");
            conf_buf_u64(&t.why, ev[i].buttons);
            conf_buf_str(&t.why, " time ");
            conf_buf_u64(&t.why, ev[i].time_ms);
        }
    }
    conf_buf_u64(&t.note, count);
    conf_buf_str(&t.note, " ");
    conf_buf_str(&t.note, what);
    conf_buf_str(&t.note, " events in order");
    conf_end(&t);
}

static int posted_event_allowed(uint16_t type, uint16_t caps)
{
    if (type == AAR_EV_TEXT)
        return (caps & AAR_DISPCAP_TEXT_INPUT) != 0;
    if (type == AAR_EV_CLOSE_REQUEST)
        return (caps & AAR_DISPCAP_CLOSE_REQUEST) != 0;
    return 1;
}

static void test_new_events(uint32_t groups)
{
    struct conf_test t;
    aar_surface s;
    uint16_t caps = 0;
    uint32_t next = 0, received = 0;
    int opened = 0, done = 0;
    uint64_t started;

    conf_begin(&t, "C-INPUT.15");
    if (groups & AAR_GROUP_DISPLAY) {
        opened = aar_display_open(320, 200, AAR_FMT_RGB565,
                                  AAR_DISPLAY_WINDOW | AAR_DISPLAY_TEXT_INPUT |
                                      AAR_DISPLAY_CLOSE_REQUEST, &s) == AAR_OK;
        conf_check(&t, opened, "cannot open the display for capability-gated events");
        if (opened)
            caps = s.caps;
    }
    cmio_kw(CI_K_NEWEV, 0, 0, 0);
    started = aar_time_us();
    while (!done && aar_time_us() - started < 3000000u) {
        aar_event events[8];
        uint32_t n = 0, i;

        if (aar_event_poll(events, 8, &n) != AAR_OK)
            break;
        for (i = 0; i < n; i++) {
            uint32_t j;

            /* Fixed timestamps distinguish the runner's posts from native
             * input on the window. The last post is the end of the sequence. */
            for (j = 0; j < CI_COUNT(ci_new) && ci_new[j].time_ms != events[i].time_ms; j++)
                ;
            if (j == CI_COUNT(ci_new))
                continue;
            while (next < CI_COUNT(ci_new) && !posted_event_allowed(ci_new[next].type, caps))
                next++;
            conf_check(&t, j == next && same(&events[i], &ci_new[j], 1),
                       "a posted event was changed, missing, out of order or not granted");
            next++;
            received++;
            if (j == CI_COUNT(ci_new) - 1u)
                done = 1;
        }
        if (!n)
            aar_sleep_us(1000);
    }
    conf_check(&t, done, "the runner's final active event did not arrive");
    if (opened)
        conf_check(&t, aar_display_close(s.id) == AAR_OK, "cannot close the test display");
    conf_buf_u64(&t.note, received);
    conf_buf_str(&t.note, " posted events in order with their granted capabilities");
    conf_end(&t);
}

/* ---- C-INPUT.6: break signals ------------------------------------------------------------------ */

static void test_breaks(void)
{
    struct conf_test t;
    static const uint32_t codes[] = { AAR_BREAK_CTRL_D, AAR_BREAK_CTRL_E, AAR_BREAK_CTRL_F };
    uint32_t i;

    conf_begin(&t, "C-INPUT.6");
    for (i = 0; i < 3u; i++) {
        aar_event ev[2];
        uint32_t n;

        cmio_kw(CI_K_BREAK, codes[i], 0, 0);
        n = collect(ev, 1, 3000000u);
        if (n != 1u) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "no AAR_EV_BREAK for code ");
                conf_buf_u64(&t.why, codes[i]);
            }
            continue;
        }
        if ((ev[0].type != AAR_EV_BREAK || ev[0].code != codes[i]) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "signal for code ");
            conf_buf_u64(&t.why, codes[i]);
            conf_buf_str(&t.why, " gave type ");
            conf_buf_u64(&t.why, ev[0].type);
            conf_buf_str(&t.why, " code ");
            conf_buf_u64(&t.why, ev[0].code);
        }
        if (collect(ev, 1, 50000u) != 0 && conf_fail_first(&t))
            conf_buf_str(&t.why, "one signal gave more than one event");
    }
    conf_buf_str(&t.note, "Ctrl-D, Ctrl-E and Ctrl-F gave codes 4, 5 and 6");
    conf_end(&t);
}

/* ---- C-INPUT.8: 64 events kept while the app does not poll --------------------------------------- */

static void test_retention(void)
{
    struct conf_test t;
    aar_event ev[64];
    uint32_t ok = 0, accepted = 0;
    uint32_t n;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-INPUT.8");
    cmio_kw(CI_K_FLOOD, 64, 0, 0);
    aar_sleep_us(900000u); /* not polling */
    rc = cmio_result(5000000u, &ok, &accepted, 0, 0, 0, 0);
    conf_check(&t, rc == 1, "no answer from the runner");
    n = collect(ev, 64, 3000000u);
    if (n != 64u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "only ");
        conf_buf_u64(&t.why, n);
        conf_buf_str(&t.why, " of 64 events arrived (the runner got ");
        conf_buf_u64(&t.why, accepted);
        conf_buf_str(&t.why, " posted)");
    }
    for (i = 0; i < n; i++) {
        if ((ev[i].type != AAR_EV_KEY_DOWN || ev[i].code != i + 1u) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "event ");
            conf_buf_u64(&t.why, i);
            conf_buf_str(&t.why, " is out of order");
        }
    }
    conf_buf_str(&t.note, "64 key events kept and delivered in order");
    conf_end(&t);
}

/* ---- C-INPUT.10: mouse motion while the queue is full --------------------------------------------- */

static void test_mouse_full(void)
{
    struct conf_test t;
    aar_event ev[16];
    uint32_t key_busy = 0, mouse_busy = 0, mouse_ok = 0;
    uint32_t mouse_dropped = 0;
    int32_t sum_dx = 0;
    uint32_t keys = 0;
    uint32_t quiet = 0;
    int rc;

    conf_begin(&t, "C-INPUT.10");
    cmio_kw(CI_K_MOUSEFULL, 0, 0, 0);
    rc = cmio_result(10000000u, 0, &key_busy, &mouse_busy, 0, &mouse_ok, 0);
    conf_check(&t, rc == 1, "no answer from the runner");
    /* The merged motion is posted as soon as there is room: poll until nothing more comes. */
    while (quiet < 40u) {
        uint32_t got = 0;
        uint32_t i;

        if (aar_event_poll(ev, 16, &got) != AAR_OK)
            break;
        for (i = 0; i < got; i++) {
            if (ev[i].type == AAR_EV_MOUSE)
                sum_dx += ev[i].dx;
            else if (ev[i].type == AAR_EV_KEY_DOWN)
                keys++;
            else if (ev[i].type == AAR_EV_RESIZE)
                saw_resize = 1;
        }
        if (got) {
            quiet = 0;
        } else {
            quiet++;
            aar_sleep_us(10000u);
        }
    }
    cmio_kw(CI_K_DROPS, 0, 0, 0);
    rc = cmio_result(10000000u, 0, &mouse_dropped, 0, 0, 0, 0);
    conf_check(&t, rc == 1, "no drop count from the runner");
    if ((uint32_t)sum_dx + mouse_dropped != 20u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "mouse motion posted 20, received sum dx ");
        conf_buf_i64(&t.why, sum_dx);
        conf_buf_str(&t.why, " and ");
        conf_buf_u64(&t.why, mouse_dropped);
        conf_buf_str(&t.why, " dropped");
    }
    conf_buf_str(&t.note, "sum of dx ");
    conf_buf_i64(&t.note, sum_dx);
    conf_buf_str(&t.note, " plus ");
    conf_buf_u64(&t.note, mouse_dropped);
    conf_buf_str(&t.note, " dropped is 20 (");
    conf_buf_u64(&t.note, keys);
    conf_buf_str(&t.note, " keys, ");
    conf_buf_u64(&t.note, key_busy);
    conf_buf_str(&t.note, " refused)");
    conf_end(&t);
}

/* ---- C-INPUT.16: absolute mouse events while the queue is full -------------------------------------- */

/* Absolute events carry a position, so merging them keeps the latest one and never adds them (spec 8.3). The
 * runner posts 100 key events and then 20 absolute positions with no retry; every absolute event the app gets
 * is one of the posted positions, and the last is the last one the client accepted. */
static void test_abs_full(void)
{
    struct conf_test t;
    aar_event ev[16];
    uint32_t key_busy = 0, mouse_busy = 0, last_ok = 0;
    uint32_t seen = 0, bad = 0;
    int16_t last_x = 0, last_y = 0;
    uint32_t quiet = 0;
    int rc;

    conf_begin(&t, "C-INPUT.16");
    cmio_kw(CI_K_ABSFULL, 0, 0, 0);
    rc = cmio_result(10000000u, 0, &key_busy, &mouse_busy, 0, &last_ok, 0);
    conf_check(&t, rc == 1, "no answer from the runner");
    while (quiet < 40u) {
        uint32_t got = 0;
        uint32_t i;

        if (aar_event_poll(ev, 16, &got) != AAR_OK)
            break;
        for (i = 0; i < got; i++) {
            if (ev[i].type == AAR_EV_RESIZE)
                saw_resize = 1;
            if (ev[i].type != AAR_EV_MOUSE)
                continue;
            seen++;
            if ((ev[i].buttons & CI_MOUSE_ABSOLUTE) == 0u || ev[i].dx % 10 != 0 || ev[i].dx < 10 ||
                ev[i].dx > ci_abs_x(CI_ABS_POSTS) || ev[i].dy != ev[i].dx / 2)
                bad++;
            last_x = ev[i].dx;
            last_y = ev[i].dy;
        }
        if (got) {
            quiet = 0;
        } else {
            quiet++;
            aar_sleep_us(10000u);
        }
    }
    if (bad && conf_fail_first(&t)) {
        conf_buf_u64(&t.why, bad);
        conf_buf_str(&t.why, " of ");
        conf_buf_u64(&t.why, seen);
        conf_buf_str(&t.why, " absolute mouse events were not one of the posted positions (a sum, or no AAR_MOUSE_ABSOLUTE)");
    }
    if (last_ok != 0u && (seen == 0u || last_x != ci_abs_x(last_ok) || last_y != ci_abs_y(last_ok)) &&
        conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the last position the app saw is (");
        conf_buf_i64(&t.why, last_x);
        conf_buf_str(&t.why, ", ");
        conf_buf_i64(&t.why, last_y);
        conf_buf_str(&t.why, "), the last accepted post was number ");
        conf_buf_u64(&t.why, last_ok);
    }
    conf_buf_u64(&t.note, seen);
    conf_buf_str(&t.note, " absolute events, last position (");
    conf_buf_i64(&t.note, last_x);
    conf_buf_str(&t.note, ", ");
    conf_buf_i64(&t.note, last_y);
    conf_buf_str(&t.note, ")");
    conf_end(&t);
}

/* ---- C-INPUT.7 and C-INPUT.2 ------------------------------------------------------------------------ */

static void test_no_resize(void)
{
    struct conf_test t;

    conf_begin(&t, "C-INPUT.7");
    conf_check(&t, !saw_resize, "an AAR_EV_RESIZE was delivered");
    conf_buf_str(&t.note, "no resize event in the whole input test");
    conf_end(&t);
}

/* A stop reaches event_poll as type 5, by AARPostEvent and by AARStop. The second one ends
 * the app's run, so this test comes last. */
static void test_stop(void)
{
    struct conf_test t;
    int posted;
    int stopped;

    if (!have_peer) {
        conf_skip("C-INPUT.2", "host: no runner to post the stop event");
        return;
    }
    conf_begin(&t, "C-INPUT.2");
    cmio_kw(CI_K_STOPEV, 0, 0, 0);
    posted = await_type(AAR_EV_STOP, 3000000u);
    conf_check(&t, posted, "an AAR_EV_STOP posted with AARPostEvent did not arrive");
    (void)conf_peer_send(CONF_MSG_STOP);
    /* The stop grace is 500 ms on the ZZ9000 (AARInfo.stop_grace_ms): an app that waits longer
     * is forced before it can report. The event is delivered at once when it is delivered. */
    stopped = await_type(AAR_EV_STOP, 250000u);
    conf_check(&t, stopped, "the stop request of AARStop did not arrive as AAR_EV_STOP");
    conf_buf_str(&t.note, "AAR_EV_STOP by AARPostEvent and by AARStop");
    conf_end(&t);
}

/* ---- Variant overfull (the runner judges C-INPUT.9) ---------------------------------------------------- */

static void variant_overfull(void)
{
    aar_event ev[16];
    uint32_t n = 0;
    uint32_t ok = 1;
    uint32_t quiet = 0;

    cmio_kw(CI_K_OVERFULL, 0, 0, 0);
    aar_sleep_us(2500000u); /* not polling */
    while (quiet < 40u) {
        uint32_t got = 0;
        uint32_t i;

        if (aar_event_poll(ev, 16, &got) != AAR_OK)
            break;
        for (i = 0; i < got; i++) {
            if (ev[i].type != AAR_EV_KEY_DOWN || ev[i].code != n + 1u)
                ok = 0;
            n++;
        }
        if (got) {
            quiet = 0;
        } else {
            quiet++;
            aar_sleep_us(10000u);
        }
    }
    cmio_kw(CI_K_GOT, n, ok, 0);
}

/* Interactive variants open a display for the client's input window and tell
 * the runner what the app received. The CD32 probe only reports joystick events. */
static void variant_show(int argc, char **argv, uint32_t flags, uint32_t ms, int joy_only)
{
    aar_surface s;
    uint64_t start;
    uint8_t m[AAR_MSG_BYTES];
    uint32_t i;

    if (!conf_arg(argc, argv, "conf:show"))
        return;
    if (!have_input ||
        aar_display_open(320, 200, AAR_FMT_RGB565, flags, &s) != AAR_OK) {
        return;
    }
    if (s.back) {
        for (i = 0; i < s.pitch * s.height; i++)
            ((uint8_t *)s.back)[i] = 0x6Bu;
    }
    (void)aar_display_flip(s.id);
    start = aar_time_us();
    while (aar_time_us() - start < (uint64_t)ms * 1000u && !aar_should_stop()) {
        aar_event ev[4];
        uint32_t got = 0;

        if (aar_event_poll(ev, 4, &got) != AAR_OK)
            break;
        for (i = 0; i < got; i++) {
            uint32_t k;

            if (joy_only && ev[i].type != AAR_EV_JOY)
                continue;
            for (k = 0; k < AAR_MSG_BYTES; k++)
                m[k] = 0;
            m[0] = 'S';
            m[1] = 'H';
            m[2] = 'O';
            m[3] = 'W';
            cm_put32(m, 4, ev[i].type);
            cm_put32(m, 8, ev[i].code);
            cm_put32(m, 12, (uint32_t)(int32_t)ev[i].dx);
            cm_put32(m, 16, (uint32_t)(int32_t)ev[i].dy);
            cm_put32(m, 20, ev[i].buttons);
            (void)cmio_send(m, 500000u);
        }
        if (!got)
            aar_sleep_us(5000u);
    }
    (void)aar_display_close(s.id);
}

int aar_main(int argc, char **argv)
{
    aar_info info;

    info.size = sizeof info;
    have_input = aar_get_info(&info) == AAR_OK && (info.groups & AAR_GROUP_INPUT) != 0;
    have_peer = conf_have_peer(argc, argv);
    variant = conf_arg(argc, argv, "conf:variant");

    if (variant && cmio_streq(variant, "overfull")) {
        variant_overfull();
        return conf_finish("C-INPUT");
    }
    if (variant && cmio_streq(variant, "show")) {
        variant_show(argc, argv, AAR_DISPLAY_VSYNC, 12000u, 0);
        return conf_finish("C-INPUT");
    }
    if (variant && cmio_streq(variant, "buttons")) {
        variant_show(argc, argv, AAR_DISPLAY_VSYNC | AAR_DISPLAY_WINDOW | AAR_DISPLAY_POINTER_ABS |
                                     AAR_DISPLAY_TEXT_INPUT, 20000u, 0);
        return conf_finish("C-INPUT");
    }
    if (variant && (cmio_streq(variant, "cd32-port0") || cmio_streq(variant, "cd32-port1"))) {
        variant_show(argc, argv, AAR_DISPLAY_VSYNC, 30000u, 1);
        return conf_finish("C-INPUT");
    }
    test_nonblocking();
    if (have_input && have_peer) {
        test_sequence("C-INPUT.3", CI_K_KEYS, ci_keys, CI_COUNT(ci_keys), 1, "key");
        test_sequence("C-INPUT.4", CI_K_MOUSE, ci_mouse, CI_COUNT(ci_mouse), 0, "mouse");
        test_sequence("C-INPUT.5", CI_K_JOY, ci_joy, CI_COUNT(ci_joy), 0, "joystick");
        test_breaks();
        test_retention();
        test_mouse_full();
        test_abs_full();
        test_no_resize();
        test_new_events(info.groups);
    }
    test_stop();
    return conf_finish("C-INPUT");
}
