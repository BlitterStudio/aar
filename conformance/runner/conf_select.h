/*
 * Three decisions of the runner core, apart from the Amiga code so that a host test
 * can run them (conformance/tests/select_test.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_SELECT_H
#define AAR_CONF_SELECT_H

#include "../payloads/conf_proto.h"

enum { CONF_R_NONE = 0, CONF_R_PASS, CONF_R_FAIL, CONF_R_SKIP };

/* May a new result replace the old one? A result of a test that plan() skipped (a
 * group the platform does not declare) is locked: nothing changes it, so an
 * observer or a payload that reports on it cannot turn a planned SKIP into a FAIL.
 * Otherwise the first FAIL stays and a PASS or SKIP never replaces a result. */
static inline int conf_update_allowed(int locked, int old_status, int new_status)
{
    if (locked)
        return 0;
    if (old_status == CONF_R_FAIL)
        return 0;
    if (old_status != CONF_R_NONE && new_status != CONF_R_FAIL)
        return 0;
    return 1;
}

/* Does a variant of an area's payload run?
 *   normal   a payload test of the area is selected and still without a result
 *   pending  a runner-judged test that is judged while this variant runs (a peer
 *            handler or an observer records it) is selected and without a result
 *   solo     the variant carries no payload ids: it exists only for those tests
 * A solo variant runs only for its pending tests, so a test of an undeclared group
 * (already skipped) does not make it run for the core tests of the area. */
static inline int conf_variant_runs(int normal, int pending, int solo)
{
    return pending || (normal && !solo);
}

/* May a MUST test end in a SKIP? Only with the reason that the catalogue allows for an
 * optional route (CONF_SKIP_NO_PLATFORM_ROUTE: C-AUDIO.1 to C-AUDIO.9, spec 8.4). A SKIP of
 * a MUST test with any other reason fails the test. */
static inline int conf_must_skip_ok(const char *reason)
{
    const char *want = CONF_SKIP_NO_PLATFORM_ROUTE;

    while (*want) {
        if (*reason++ != *want++)
            return 0;
    }
    return 1;
}

#endif
