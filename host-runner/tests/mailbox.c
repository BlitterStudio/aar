/*
 * Latest-mode three-buffer rotation checks. The drawing buffer must always be
 * one of the three live buffers, after async-then-sync and sync-then-async
 * flips; a wrong rotation hands the app a NULL back buffer. The name is the
 * old mailbox mode, which is AAR_DISPLAY_LATEST now.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/detail/present_feedback.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *what)
{
    fprintf(stderr, "mailbox: %s\n", what);
    return 1;
}

static int test_rotation(void)
{
    aar_surface s = {0};
    const void *p0;
    const void *p1;
    const void *p2;
    const void *last;

    if (aar_display_open(8, 8, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &s) != AAR_OK)
        return fail("open");
    if ((s.caps & AAR_DISPCAP_LATEST) == 0u)
        return fail("latest not granted");
    p0 = s.front;
    p1 = s.back;
    last = p1;

    /* async flip, let the pump show it, then a synchronous flip */
    if (aar_display_flip_async(s.id) != AAR_OK)
        return fail("async flip");
    if (aar_display_refresh(&s) != AAR_OK || !s.front || !s.back ||
        s.back == s.front || s.back == p0 || s.back == p1)
        return fail("back after async flip");
    p2 = s.back;
    last = p2;
    aar_yield();
    if (aar_display_flip(s.id) != AAR_OK)
        return fail("sync flip after async");
    if (aar_display_refresh(&s) != AAR_OK || !s.front || !s.back ||
        s.back == s.front)
        return fail("rotation broke after sync-after-async");
    if (s.front != p2)
        return fail("sync did not present the drawn buffer");
    last = s.back;

    /* other order: synchronous flip first, then async */
    if (aar_display_flip(s.id) != AAR_OK)
        return fail("sync flip");
    if (aar_display_refresh(&s) != AAR_OK || !s.front || !s.back ||
        s.back == s.front)
        return fail("rotation broke after sync");
    if (s.front != last)
        return fail("sync did not present the drawn buffer");
    if (aar_display_flip_async(s.id) != AAR_OK)
        return fail("async flip after sync");
    if (aar_display_refresh(&s) != AAR_OK || !s.front || !s.back ||
        s.back == s.front)
        return fail("rotation broke after async-after-sync");

    /* a sync flip over a pending one replaces it and counts a drop */
    if (aar_display_latest_dropped() != 0u)
        return fail("a drop was counted without a replacement");
    if (aar_display_flip(s.id) != AAR_OK)
        return fail("sync flip over a pending frame");
    if (aar_display_latest_dropped() != 1u)
        return fail("the replaced frame was not counted");
    return 0;
}

static int test_latest(void)
{
    aar_surface s;
    aar_present_result result;
    uint64_t a, b, begin, end, shown;
    const void *pixels_b;

    if (aar_display_open(8, 8, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &s) != AAR_OK ||
        !(s.caps & AAR_DISPCAP_PRESENT_FEEDBACK))
        return fail("open with feedback");
    memset(s.back, 0x11, s.pitch * s.height);
    if (aar_display_flip(s.id) != AAR_OK || aar_display_refresh(&s) != AAR_OK)
        return fail("initial untracked sync");
    shown = aar_display_last_present_us();

    memset(s.back, 0x22, s.pitch * s.height);
    if (aar_display_present(s.id, AAR_PRESENT_ASYNC, &a) != AAR_OK || a == 0u ||
        aar_display_present_poll(&result) != 0 || aar_display_refresh(&s) != AAR_OK)
        return fail("tracked A did not remain pending without a pump");
    memset(s.back, 0x7c, s.pitch * s.height);
    pixels_b = s.back;
    begin = aar_time_us();
    if (aar_display_present(s.id, AAR_PRESENT_ASYNC, &b) != AAR_OK ||
        b == 0u || b == a)
        return fail("tracked B submission");
    end = aar_time_us();
    if (aar_display_present_poll(&result) != 1 || result.frame_id != a ||
        result.outcome != AAR_PRESENT_REPLACED || result.reason != AAR_PRESENT_REASON_NONE ||
        result.completed_us < begin || result.completed_us > end ||
        aar_display_present_poll(&result) != 0 || aar_display_latest_dropped() != 1u ||
        aar_display_last_present_us() != shown)
        return fail("A should be replaced once without acknowledging a frame");

    begin = aar_time_us();
    aar_yield();
    end = aar_time_us();
    if (aar_display_present_poll(&result) != 1 || result.frame_id != b ||
        result.outcome != AAR_PRESENT_ACKNOWLEDGED || result.reason != AAR_PRESENT_REASON_NONE ||
        result.completed_us < begin || result.completed_us > end ||
        result.completed_us != aar_display_last_present_us() ||
        aar_display_present_poll(&result) != 0)
        return fail("only B should be acknowledged on the pump");
    if (aar_display_refresh(&s) != AAR_OK || s.front != pixels_b || s.front == s.back)
        return fail("B was not the front buffer");
    return AAR_OK;
}

static int test_saturation(void)
{
    aar_surface s;
    aar_present_result result;
    uint64_t ids[AAR_PRESENT_QUEUE_CAPACITY + 1u];
    uint64_t completed[AAR_PRESENT_QUEUE_CAPACITY + 1u];
    uint8_t seen[AAR_PRESENT_QUEUE_CAPACITY + 1u] = {0};
    const unsigned pending = AAR_PRESENT_QUEUE_CAPACITY - 1u;
    const unsigned resumed = AAR_PRESENT_QUEUE_CAPACITY;
    uint64_t replaced_begin, replaced_end, rejected = 1u;
    const void *back, *front;
    unsigned i, j;

    if (aar_display_open(8, 8, AAR_FMT_RGB565, AAR_DISPLAY_LATEST, &s) != AAR_OK)
        return fail("open for saturation");
    for (i = 0u; i < pending; i++) {
        if (aar_display_present(s.id, 0u, &ids[i]) != AAR_OK || ids[i] == 0u)
            return fail("filling with acknowledged frames");
        completed[i] = aar_display_last_present_us();
    }
    if (aar_display_present(s.id, AAR_PRESENT_ASYNC, &ids[pending]) != AAR_OK ||
        ids[pending] == 0u || aar_display_refresh(&s) != AAR_OK)
        return fail("last slot must hold a pending frame");
    front = s.front;
    back = s.back;
    memset(s.back, 0x45, s.pitch * s.height);
    if (aar_display_present(s.id, AAR_PRESENT_ASYNC, &rejected) != AAR_E_BUSY ||
        rejected != 0u)
        return fail("saturated async must reject before accepting");
    rejected = 1u;
    if (aar_display_present(s.id, 0u, &rejected) != AAR_E_BUSY ||
        rejected != 0u || aar_display_refresh(&s) != AAR_OK ||
        s.front != front || s.back != back || aar_display_latest_dropped() != 0u ||
        aar_display_last_present_us() != completed[pending - 1u])
        return fail("saturated sync must not rotate or replace");

    /* An untracked flip can replace the tracked frame while the queue is full. */
    replaced_begin = aar_time_us();
    if (aar_display_flip_async(s.id) != AAR_OK)
        return fail("saturation blocked an untracked flip");
    replaced_end = aar_time_us();
    if (aar_display_latest_dropped() != 1u)
        return fail("untracked flip did not replace the pending frame");
    aar_yield();
    if (aar_display_refresh(&s) != AAR_OK || s.front != back ||
        ((const uint8_t *)s.front)[0] != 0x45u)
        return fail("untracked frame did not reach the front");

    /* Consuming one result restores capacity. Replacing untracked work
     * increments the old counter, but must not manufacture a result. */
    if (aar_display_present_poll(&result) != 1 ||
        aar_display_flip_async(s.id) != AAR_OK ||
        aar_display_present(s.id, 0u, &ids[resumed]) != AAR_OK ||
        ids[resumed] == 0u || aar_display_latest_dropped() != 2u)
        return fail("tracked flip could not resume over an untracked one");
    completed[resumed] = aar_display_last_present_us();

    for (i = 0u; i <= resumed; i++) {
        if (i != 0u && aar_display_present_poll(&result) != 1)
            return fail("lost an accepted result at saturation");
        for (j = 0u; j <= resumed && ids[j] != result.frame_id; j++)
            ;
        if (j > resumed || seen[j])
            return fail("unexpected or duplicated frame ID");
        seen[j] = 1u;
        if (j == pending) {
            if (result.outcome != AAR_PRESENT_REPLACED ||
                result.completed_us < replaced_begin || result.completed_us > replaced_end)
                return fail("the old pending frame was not replaced by the untracked flip");
        } else if (result.outcome != AAR_PRESENT_ACKNOWLEDGED ||
                   result.completed_us != completed[j]) {
            return fail("an acknowledged result was overwritten");
        }
        if (result.reason != AAR_PRESENT_REASON_NONE)
            return fail("unexpected reason at saturation");
    }
    if (aar_display_present_poll(&result) != 0)
        return fail("extra result at saturation");
    return AAR_OK;
}

int aar_main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "latest") == 0)
        return test_latest();
    if (argc > 1 && strcmp(argv[1], "saturation") == 0)
        return test_saturation();
    return test_rotation();
}
