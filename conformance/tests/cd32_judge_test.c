/*
 * Host test of the CD32 pad decision (conformance/runner/cd32_judge.h): each of
 * the seven buttons counts only when that port shows it alone, between neutrals.
 * Another port, the keyboard, a chord, or a switch without a neutral must not
 * complete the probe.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "../runner/cd32_judge.h"

static int failures;

static void expect(const char *name, int cond)
{
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void joy(struct conf_cd32_judge *j, uint32_t port, uint32_t buttons)
{
    conf_cd32_event(j, AAR_EV_JOY, port, buttons);
}

int main(void)
{
    static const uint32_t pad[] = {
        AAR_JOY_BUTTON1, AAR_JOY_BUTTON2, AAR_JOY_BUTTON3, AAR_JOY_BUTTON4,
        AAR_JOY_PLAY, AAR_JOY_FORWARD, AAR_JOY_REVERSE
    };
    unsigned i;

    expect("CONF_CD32_BUTTONS is the seven pad buttons",
           CONF_CD32_BUTTONS == (AAR_JOY_BUTTON1 | AAR_JOY_BUTTON2 | AAR_JOY_BUTTON3 |
                                 AAR_JOY_BUTTON4 | AAR_JOY_PLAY | AAR_JOY_FORWARD |
                                 AAR_JOY_REVERSE));

    /* Hands off: `{ .port = n }` is neutral. Nothing has been pressed, so no
     * release is outstanding. */
    {
        struct conf_cd32_judge j = { .port = 1 };

        expect("a fresh probe is missing every press",
               conf_cd32_missing_presses(&j) == CONF_CD32_BUTTONS);
        expect("a fresh probe is missing no release", conf_cd32_missing_releases(&j) == 0);
    }

    /* A finished sequence on the other port, or the keyboard on the watched port,
     * must not finish the probe. */
    {
        struct conf_cd32_judge j = { .port = 1 };

        for (i = 0; i < sizeof pad / sizeof pad[0]; i++) {
            joy(&j, 0, pad[i]);
            joy(&j, 0, 0);
            conf_cd32_event(&j, AAR_EV_KEY_DOWN, 1, pad[i]);
            conf_cd32_event(&j, AAR_EV_KEY_UP, 1, 0);
        }
        expect("the other port does not complete the probe",
               conf_cd32_missing_presses(&j) == CONF_CD32_BUTTONS &&
                   conf_cd32_missing_releases(&j) == 0);
        expect("the keyboard does not complete the probe",
               conf_cd32_missing_presses(&j) == CONF_CD32_BUTTONS &&
                   conf_cd32_missing_releases(&j) == 0);
    }

    /* A real press stays open until its own port returns to neutral. */
    {
        struct conf_cd32_judge j = { .port = 1 };

        joy(&j, 1, AAR_JOY_BUTTON1);
        joy(&j, 0, 0);
        conf_cd32_event(&j, AAR_EV_KEY_UP, 1, 0);
        expect("a press without its own release stays open",
               conf_cd32_missing_presses(&j) == (CONF_CD32_BUTTONS & ~AAR_JOY_BUTTON1) &&
                   conf_cd32_missing_releases(&j) == AAR_JOY_BUTTON1);
        joy(&j, 1, 0);
        expect("neutral on the watched port closes that release",
               conf_cd32_missing_presses(&j) == (CONF_CD32_BUTTONS & ~AAR_JOY_BUTTON1) &&
                   conf_cd32_missing_releases(&j) == 0);
    }

    /* A chord, or one tracked bit with an unexpected bit, is not a press. The
     * extra bit is not stripped before the sample is judged. */
    {
        struct conf_cd32_judge j = { .port = 1 };

        joy(&j, 1, AAR_JOY_BUTTON1 | AAR_JOY_BUTTON2);
        joy(&j, 1, 0);
        joy(&j, 1, AAR_JOY_BUTTON1 | (1u << 7));
        joy(&j, 1, AAR_JOY_BUTTON1);
        joy(&j, 1, 0);
        joy(&j, 1, 1u << 7);
        joy(&j, 1, 0);
        joy(&j, 1, CONF_CD32_BUTTONS);
        joy(&j, 1, 0);
        expect("combined buttons do not qualify a press or a release",
               conf_cd32_missing_presses(&j) == CONF_CD32_BUTTONS &&
                   conf_cd32_missing_releases(&j) == 0);
    }

    /* Leaving one button for another, without neutral, cancels the open release
     * and does not press the second. Coming back to the first before neutral
     * does not open the release again. */
    {
        struct conf_cd32_judge j = { .port = 1 };

        joy(&j, 1, AAR_JOY_BUTTON1);
        joy(&j, 1, AAR_JOY_BUTTON2);
        joy(&j, 1, AAR_JOY_BUTTON1);
        joy(&j, 1, 0);
        expect("a switch without neutral leaves the first release open",
               conf_cd32_missing_presses(&j) == (CONF_CD32_BUTTONS & ~AAR_JOY_BUTTON1) &&
                   conf_cd32_missing_releases(&j) == AAR_JOY_BUTTON1);
        joy(&j, 1, AAR_JOY_FORWARD);
        joy(&j, 1, AAR_JOY_FORWARD | AAR_JOY_REVERSE);
        joy(&j, 1, 0);
        expect("a chord cancels the open release and presses neither extra button",
               conf_cd32_missing_presses(&j) ==
                       (CONF_CD32_BUTTONS & ~(AAR_JOY_BUTTON1 | AAR_JOY_FORWARD)) &&
                   conf_cd32_missing_releases(&j) == (AAR_JOY_BUTTON1 | AAR_JOY_FORWARD));
        joy(&j, 1, AAR_JOY_BUTTON1);
        joy(&j, 1, 0);
        joy(&j, 1, AAR_JOY_FORWARD);
        joy(&j, 1, 0);
        expect("a later clean cycle releases the buttons a switch or chord cancelled",
               (conf_cd32_missing_presses(&j) & (AAR_JOY_BUTTON1 | AAR_JOY_FORWARD)) == 0 &&
                   conf_cd32_missing_releases(&j) == 0);
    }

    /* Port 0 is a real probe. Each button is pressed from the assumed neutral,
     * including a repeated sample, and released only by that port. */
    {
        struct conf_cd32_judge j = { .port = 0 };

        for (i = 0; i < sizeof pad / sizeof pad[0]; i++) {
            joy(&j, 0, pad[i]);
            joy(&j, 0, pad[i]);
            joy(&j, 1, 0);
            conf_cd32_event(&j, AAR_EV_KEY_UP, 0, 0);
            expect("a held button is pressed and still missing its release",
                   (conf_cd32_missing_presses(&j) & pad[i]) == 0 &&
                       conf_cd32_missing_releases(&j) == pad[i]);
            joy(&j, 0, 0);
            joy(&j, 0, 0);
        }
        expect("seven independent presses and releases finish the probe",
               conf_cd32_missing_presses(&j) == 0 && conf_cd32_missing_releases(&j) == 0);
    }

    if (failures) {
        printf("%d cd32 judge tests failed\n", failures);
        return 1;
    }
    printf("cd32 judge: ok\n");
    return 0;
}
