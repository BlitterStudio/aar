/*
 * Host test of the C-FREE.8 decision (conformance/runner/free_judge.h): only an
 * explicit AAR_E_NOSYS marks a decode kind unavailable; a platform that leaks
 * sessions until none open, or refuses with another code, fails with the run
 * and the code.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../runner/free_judge.h"

static int failures;

static void expect(const char *name, int cond)
{
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

/* Judge a sequence of (opened, code) runs; returns the run number that failed, or 0. */
static unsigned long judge(const unsigned long *opened, const long *code, unsigned n, char *why)
{
    struct free_kind k;
    unsigned i;

    memset(&k, 0, sizeof k);
    why[0] = '\0';
    for (i = 0; i < n; i++) {
        if (!free_judge_kind(&k, i + 1, "image", opened[i], code[i], 4, why, 160))
            return i + 1;
    }
    return 0;
}

int main(void)
{
    char why[160];
    static const unsigned long ok4[] = { 4, 4, 4 };
    static const long ok4c[] = { 0, 0, 0 };
    static const unsigned long leak[] = { 4, 4, 3, 0 };
    static const long leakc[] = { 0, 0, -3, -3 };
    static const unsigned long leak_nosys[] = { 4, 4, 0 };
    static const long leak_nosysc[] = { 0, 0, -7 };
    static const unsigned long none[] = { 0, 0, 0 };
    static const long nonec[] = { -7, -7, -7 };
    static const unsigned long nomem_first[] = { 0, 0 };
    static const long nomem_firstc[] = { -3, -3 };
    static const unsigned long late[] = { 0, 4 };
    static const long latec[] = { -7, 0 };

    expect("every run opens four", judge(ok4, ok4c, 3, why) == 0);
    expect("a leak down to three fails at the run that opens three", judge(leak, leakc, 4, why) == 3);
    expect("the failure names the code", strstr(why, "-3") != NULL);
    expect("a platform that leaks until none open and then says NOSYS fails",
           judge(leak_nosys, leak_nosysc, 3, why) == 3);
    expect("the failure says earlier runs opened them", strstr(why, "earlier runs opened") != NULL);
    expect("a kind that always says NOSYS is unavailable, not a failure",
           judge(none, nonec, 3, why) == 0);
    expect("zero opens with another code fails at once", judge(nomem_first, nomem_firstc, 2, why) == 1);
    expect("a kind that says NOSYS and then opens is inconsistent", judge(late, latec, 2, why) == 2);
    {
        /* The scenario of the review: four sessions on the first run, every slot leaked
         * at exit, AAR_E_NOMEM on every later run. Zero opens with NOMEM is a failure,
         * never an exemption. */
        static const unsigned long leaked[] = { 4, 0, 0, 0 };
        static const long leakedc[] = { 0, -3, -3, -3 };

        expect("four sessions, then NOMEM with none open, fails at the second run",
               judge(leaked, leakedc, 4, why) == 2);
        expect("the failure carries the code", strstr(why, "-3") != NULL);
    }

    if (failures) {
        printf("%d free judge tests failed\n", failures);
        return 1;
    }
    printf("free judge: ok\n");
    return 0;
}
