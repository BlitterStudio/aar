/*
 * Host test of the decisions of the runner core (conformance/runner/conf_select.h):
 * a planned skip of an undeclared group cannot be changed by a payload line or an
 * observer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "../runner/conf_select.h"

static int failures;

static void expect(const char *name, int cond)
{
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

int main(void)
{
    expect("a first result is taken", conf_update_allowed(0, CONF_R_NONE, CONF_R_PASS));
    expect("a first FAIL is taken", conf_update_allowed(0, CONF_R_NONE, CONF_R_FAIL));
    expect("a FAIL replaces a PASS", conf_update_allowed(0, CONF_R_PASS, CONF_R_FAIL));
    expect("a PASS does not replace a PASS", !conf_update_allowed(0, CONF_R_PASS, CONF_R_PASS));
    expect("a PASS does not replace a FAIL", !conf_update_allowed(0, CONF_R_FAIL, CONF_R_PASS));
    expect("a second FAIL does not replace the first", !conf_update_allowed(0, CONF_R_FAIL, CONF_R_FAIL));
    expect("a SKIP does not replace a PASS", !conf_update_allowed(0, CONF_R_PASS, CONF_R_SKIP));
    /* The planned skip of a test of an undeclared group (C-INPUT.9 without the input
     * group): an observer that records FAIL, or a payload that reports anything, must
     * leave it. */
    expect("a planned SKIP is not turned into FAIL", !conf_update_allowed(1, CONF_R_SKIP, CONF_R_FAIL));
    expect("a planned SKIP is not turned into PASS", !conf_update_allowed(1, CONF_R_SKIP, CONF_R_PASS));
    expect("a planned SKIP is not replaced by another SKIP", !conf_update_allowed(1, CONF_R_SKIP, CONF_R_SKIP));
    /* Without the lock the old rule let FAIL replace the SKIP: the case that was wrong. */
    expect("an unlocked SKIP is still replaced by FAIL (the old rule)",
           conf_update_allowed(0, CONF_R_SKIP, CONF_R_FAIL));

    if (failures) {
        printf("%d select tests failed\n", failures);
        return 1;
    }
    printf("select: ok\n");
    return 0;
}
