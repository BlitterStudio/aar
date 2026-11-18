/*
 * Latest-mode three-buffer rotation checks. The drawing buffer must always be
 * one of the three live buffers, after async-then-sync and sync-then-async
 * flips; a wrong rotation hands the app a NULL back buffer. The name is the
 * old mailbox mode, which is AAR_DISPLAY_LATEST now.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"

#include <stdio.h>

static int fail(const char *what)
{
    fprintf(stderr, "mailbox: %s\n", what);
    return 1;
}

int aar_main(int argc, char **argv)
{
    aar_surface s = {0};
    const void *p0;
    const void *p1;
    const void *p2;
    const void *last;

    (void)argc;
    (void)argv;
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
