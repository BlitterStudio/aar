/*
 * C-INPUT.20 and C-INPUT.21: the decision, apart from the Amiga runner so that a
 * host test can run it (conformance/tests/cd32_judge_test.c). Each of the seven
 * CD32 buttons counts only when it is pressed and released on its own on the
 * probe's port. The operator starts with every button released, so a judge
 * zeroed apart from .port is already neutral; polling may not emit that
 * neutral. Qualify a button only as neutral, then exactly that button, then
 * neutral. A chord, a bit outside the seven, or a switch to another nonzero
 * state is not a press, and it cancels a release that is still open until the
 * pad is neutral again. The same state twice changes nothing. A sample that is
 * not AAR_EV_JOY for j->port is ignored.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_CD32_JUDGE_H
#define AAR_CONF_CD32_JUDGE_H

#include <aar/app.h>

/* Every AAR_JOY_* bit a CD32 pad reports. */
#define CONF_CD32_BUTTONS \
    (AAR_JOY_BUTTON1 | AAR_JOY_BUTTON2 | AAR_JOY_BUTTON3 | AAR_JOY_BUTTON4 | \
     AAR_JOY_PLAY | AAR_JOY_FORWARD | AAR_JOY_REVERSE)

/* Start with `{ .port = n }`. The other fields are the probe's own memory:
 * previous is the last accepted button word (0 is neutral, including before
 * any sample), pending is the one button whose release is still open, and
 * pressed and released are the buttons seen on their own. */
struct conf_cd32_judge {
    uint32_t port;
    uint32_t previous;
    uint32_t pending;
    uint32_t pressed;
    uint32_t released;
};

/* One sample from the app. `buttons` is judged whole: a bit outside the seven
 * disqualifies a single press, and is not masked off first. */
static inline void conf_cd32_event(struct conf_cd32_judge *j, uint32_t type, uint32_t port,
                                   uint32_t buttons)
{
    if (type != AAR_EV_JOY || port != j->port || buttons == j->previous)
        return;
    if (buttons == 0) {
        if (j->pending)
            j->released |= j->pending;
        j->pending = 0;
        j->previous = 0;
        return;
    }
    /* Nonzero, and not a repeat of the word already accepted. */
    if (j->previous == 0 && (buttons & (buttons - 1u)) == 0 &&
        (buttons & CONF_CD32_BUTTONS) == buttons) {
        j->pressed |= buttons;
        j->pending = buttons;
    } else {
        j->pending = 0;
    }
    j->previous = buttons;
}

/* Buttons of CONF_CD32_BUTTONS not yet pressed on their own. */
static inline uint32_t conf_cd32_missing_presses(const struct conf_cd32_judge *j)
{
    return CONF_CD32_BUTTONS & ~j->pressed;
}

/* Buttons pressed on their own whose release has not been seen. A button that
 * was only ever part of a chord is not included. */
static inline uint32_t conf_cd32_missing_releases(const struct conf_cd32_judge *j)
{
    return j->pressed & ~j->released;
}

#endif
