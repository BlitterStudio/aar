/*
 * Decisions of the runner core, apart from the Amiga code so that a host test can
 * run them (conformance/tests/select_test.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_SELECT_H
#define AAR_CONF_SELECT_H

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

#endif
