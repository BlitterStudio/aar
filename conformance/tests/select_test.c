/*
 * Host test of the runner's selection and result rules
 * (conformance/runner/conf_select.h).
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
    /* Results. */
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

    /* Variants: normal = a payload test of the area is selected, pending = a runner test
     * that this variant judges is selected, solo = the variant has no payload ids. */
    expect("a variant with payload ids runs for the area's tests", conf_variant_runs(1, 0, 0));
    expect("a variant runs for its pending runner test", conf_variant_runs(0, 1, 0));
    expect("a solo variant runs for its pending test", conf_variant_runs(0, 1, 1));
    expect("a solo variant runs for its pending test beside payload tests", conf_variant_runs(1, 1, 1));
    expect("a solo variant does not run for the area's other tests (C-INPUT.1 and .2 without the "
           "input group must not run overfull)", !conf_variant_runs(1, 0, 1));
    expect("nothing selected: nothing runs", !conf_variant_runs(0, 0, 0));
    expect("nothing selected: a solo variant does not run", !conf_variant_runs(0, 0, 1));

    /* A broad interactive check must never drive a pad's port pins. */
    expect("an exact-only row skips in a full interactive suite",
           conf_operator_skip_reason(1, 1, "C-INPUT.20", NULL) != NULL);
    expect("an exact-only row skips with an area prefix",
           conf_operator_skip_reason(1, 1, "C-INPUT.20", "C-INPUT") != NULL);
    expect("selecting port 1 does not opt in port 0",
           conf_operator_skip_reason(1, 1, "C-INPUT.20", "C-INPUT.21") != NULL);
    expect("an exact port id works with lower-case input",
           conf_operator_skip_reason(1, 1, "C-INPUT.20", "c-input.20") == NULL);
    expect("an exact port id still requires INTERACTIVE",
           conf_operator_skip_reason(0, 1, "C-INPUT.20", "C-INPUT.20") != NULL);
    expect("ordinary operator rows run in a full interactive suite",
           conf_operator_skip_reason(1, 0, "C-INPUT.11", NULL) == NULL);
    expect("area prefixes still select ordinary rows", conf_id_matches("C-INPUT.11", "c-input", 0));
    expect("a full run still selects ordinary rows", conf_id_matches("C-INPUT.11", NULL, 0));

    /* A MUST test may skip only for the optional route, and only a row the catalogue marks
     * (C-AUDIO.1 to C-AUDIO.9 without a PLATFORM route, spec 8.4): `route_skip` is that mark. */
    expect("the no-PLATFORM-route reason is accepted for a marked row",
           conf_must_skip_ok(1, "no PLATFORM route: audio_open returned AAR_E_NOSYS"));
    expect("the same reason is not accepted for any other MUST row (C-TIME.1)",
           !conf_must_skip_ok(0, "no PLATFORM route: audio_open returned AAR_E_NOSYS"));
    expect("a host: reason is not accepted for a marked row", !conf_must_skip_ok(1, "host: no pool"));
    expect("an empty reason is not accepted for a marked row", !conf_must_skip_ok(1, ""));
    expect("a reason that only contains the text is not accepted",
           !conf_must_skip_ok(1, "the no PLATFORM route"));

    if (failures) {
        printf("%d select tests failed\n", failures);
        return 1;
    }
    printf("select: ok\n");
    return 0;
}
