/*
 * CPU-bound scheduler fairness probe.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>

#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

#define SPIN_BATCH 1024u

static volatile uint32_t spin_sink;

int aar_main(int argc, char **argv)
{
    const char *const name = argc > 1 ? argv[1] : "default";
    uint64_t start = aar_time_us();
    uint64_t count = 0u;

    for (;;) {
        uint32_t i;
        uint64_t now;

        for (i = 0u; i < SPIN_BATCH; i++)
            spin_sink = spin_sink * 1664525u + 1013904223u;
        count += SPIN_BATCH;
        now = aar_time_us();
        if (aar_should_stop())
            break;
        if (now - start < 1000000u)
            continue;
        printf("spin %s: %llu/s\n", name, (unsigned long long)count);
        fflush(stdout);
        count = 0u;
        start = now;
    }
    return 0;
}
