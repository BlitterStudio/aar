/*
 * C-FREE.8: the decision, apart from the Amiga code so that a host test can run it
 * (conformance/tests/free_judge_test.c). A decode kind (image or audio) is
 * unavailable only when its calls say so with AAR_E_NOSYS for every codec (the
 * payload reports that as opened 0 and code -7). Any other refusal, and any run
 * that opens fewer sessions than the first runs did, is a leak or a limit.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_FREE_JUDGE_H
#define AAR_CONF_FREE_JUDGE_H

#include <stdio.h>

#define FREE_JUDGE_NOSYS (-7L) /* AAR_E_NOSYS */

struct free_kind {
    int seen;        /* a run opened sessions of this kind */
    int unavailable; /* a run said AAR_E_NOSYS for every codec, none had opened any */
};

/* One run's result for one kind: `opened` sessions, then `code` (0 when all `want`
 * opened, else the result of the refused open). Returns 1 when the run is fine, or 0
 * and fills `why` (at most `size` bytes) with the iteration and the code. */
static inline int free_judge_kind(struct free_kind *k, unsigned long run, const char *kind,
                                  unsigned long opened, long code, unsigned long want,
                                  char *why, unsigned long size)
{
    if (opened == 0 && code == FREE_JUDGE_NOSYS) {
        if (k->seen) {
            snprintf(why, size,
                     "run %lu: %s sessions refused with AAR_E_NOSYS after earlier runs opened them",
                     run, kind);
            return 0;
        }
        k->unavailable = 1;
        return 1;
    }
    if (opened < want) {
        snprintf(why, size, "run %lu: opened %lu of %lu %s decode sessions, then %ld", run,
                 opened, want, kind, code);
        return 0;
    }
    if (k->unavailable) {
        snprintf(why, size, "run %lu: %s sessions open now but an earlier run said AAR_E_NOSYS",
                 run, kind);
        return 0;
    }
    k->seen = 1;
    return 1;
}

#endif
