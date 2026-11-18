/*
 * Measures table-call round trips without attempting PMU access from user
 * mode. The cycle estimate assumes the ZZ9000's 667 MHz core.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

#define SVCBENCH_CALLS 100000u
#define SVCBENCH_CALLS_PER_SAMPLE 100u
#define SVCBENCH_SAMPLES (SVCBENCH_CALLS / SVCBENCH_CALLS_PER_SAMPLE)
#define CPU_CYCLES_PER_US 667u

static uint32_t should_stop_samples[SVCBENCH_SAMPLES];
static uint32_t time_us_samples[SVCBENCH_SAMPLES];
static uint32_t info_samples[SVCBENCH_SAMPLES];

static int compare_u32(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;

    return a < b ? -1 : a != b;
}

static uint32_t timer_cycles(uint64_t elapsed_us)
{
    return (uint32_t)((elapsed_us * CPU_CYCLES_PER_US) /
                      SVCBENCH_CALLS_PER_SAMPLE);
}

static void report(const char *call, uint32_t samples[SVCBENCH_SAMPLES])
{
    uint32_t p99_index = (SVCBENCH_SAMPLES * 99u + 99u) / 100u - 1u;

    qsort(samples, SVCBENCH_SAMPLES, sizeof samples[0], compare_u32);
    printf("svcbench: %s median %lu p99 %lu cycles\n", call,
           (unsigned long)samples[SVCBENCH_SAMPLES / 2u],
           (unsigned long)samples[p99_index]);
}

static int benchmark_should_stop(void)
{
    uint32_t sample;

    for (sample = 0u; sample < SVCBENCH_SAMPLES; sample++) {
        uint64_t begin = aar_time_us();
        uint32_t call;

        for (call = 0u; call < SVCBENCH_CALLS_PER_SAMPLE; call++)
            if (aar_should_stop())
                return 1;
        should_stop_samples[sample] = timer_cycles(aar_time_us() - begin);
    }
    return 0;
}

static void benchmark_time_us(void)
{
    uint32_t sample;

    for (sample = 0u; sample < SVCBENCH_SAMPLES; sample++) {
        uint64_t begin = aar_time_us();
        uint32_t call;

        for (call = 0u; call < SVCBENCH_CALLS_PER_SAMPLE; call++)
            (void)aar_time_us();
        time_us_samples[sample] = timer_cycles(aar_time_us() - begin);
    }
}

static int benchmark_info(void)
{
    uint32_t sample;
    aar_info info;

    for (sample = 0u; sample < SVCBENCH_SAMPLES; sample++) {
        uint64_t begin = aar_time_us();
        uint32_t call;
        int rc = AAR_OK;

        for (call = 0u; call < SVCBENCH_CALLS_PER_SAMPLE; call++) {
            info.size = (uint32_t)sizeof info;
            rc = aar_get_info(&info);
            if (rc != AAR_OK)
                return rc;
        }
        info_samples[sample] = timer_cycles(aar_time_us() - begin);
    }
    return AAR_OK;
}

int aar_main(int argc, char **argv)
{
    int rc;

    (void)argc;
    (void)argv;
    printf("svcbench: aar_time_us batches; 667 MHz timer-derived cycle estimates "
           "(PMU unavailable in user mode)\n");
    if (benchmark_should_stop())
        return 0;
    report("should_stop", should_stop_samples);
    benchmark_time_us();
    report("time_us", time_us_samples);
    rc = benchmark_info();
    if (rc != AAR_OK) {
        printf("svcbench: info returned %d\n", rc);
        return 20;
    }
    report("info", info_samples);
    return 0;
}
