/*
 * Measures one SVC round trip at each firmware probe boundary (A32 only: the
 * probes are a firmware build option of the ZZ9000), then three table calls
 * with the cycle counter. User mode needs PMUSERENR.EN set for the counter.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

#define SVCPROBE_SAMPLES 10000u
#define SVCPROBE_WARMUP 256u

static uint32_t samples[SVCPROBE_SAMPLES];
static volatile uint32_t sink;

#if defined(__aarch64__)

static uint32_t read_pmccntr(void)
{
    uint64_t value;

    __asm__ volatile("isb\n\tmrs %0, pmccntr_el0"
                     : "=r"(value) : : "memory");
    return (uint32_t)value;
}

static uint32_t read_pmuserenr(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, pmuserenr_el0" : "=r"(value) : : "memory");
    return (uint32_t)value;
}

#else

enum {
    SVC_PROBE_P0 = 0x100u,
    SVC_PROBE_P1,
    SVC_PROBE_P2,
    SVC_PROBE_P3,
    SVC_PROBE_P4
};

static uint32_t read_pmccntr(void)
{
    uint32_t value;

    __asm__ volatile("isb\n\tmrc p15, 0, %0, c9, c13, 0"
                     : "=r"(value) : : "memory");
    return value;
}

static uint32_t read_pmuserenr(void)
{
    uint32_t value;

    __asm__ volatile("mrc p15, 0, %0, c9, c14, 0"
                     : "=r"(value) : : "memory");
    return value;
}

static int svc_probe(uint32_t number)
{
    register uint32_t result __asm__("r0") = 0U;
    register uint32_t call __asm__("r12") = number;

    __asm__ volatile("svc 0" : "+r"(result), "+r"(call) : :
                     "r1", "r2", "r3", "lr", "memory", "cc");
    return (int)result;
}

#endif

static int compare_u32(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;

    return a < b ? -1 : a != b;
}

static uint32_t subtract_overhead(uint32_t value, uint32_t overhead)
{
    return value > overhead ? value - overhead : 0U;
}

static uint32_t measure_read_pair(void)
{
    uint32_t sample;

    for (sample = 0U; sample < SVCPROBE_WARMUP; sample++) {
        uint32_t begin = read_pmccntr();
        uint32_t end = read_pmccntr();

        sink = end - begin;
    }
    for (sample = 0U; sample < SVCPROBE_SAMPLES; sample++) {
        uint32_t begin = read_pmccntr();
        uint32_t end = read_pmccntr();

        samples[sample] = end - begin;
    }
    qsort(samples, SVCPROBE_SAMPLES, sizeof(samples[0]), compare_u32);
    return samples[SVCPROBE_SAMPLES / 2U];
}

static void report(const char *name, uint32_t overhead)
{
    uint32_t p99 = (SVCPROBE_SAMPLES * 99U + 99U) / 100U - 1U;

    qsort(samples, SVCPROBE_SAMPLES, sizeof(samples[0]), compare_u32);
    printf("svcprobe: %s min %lu median %lu p99 %lu cycles\n", name,
           (unsigned long)subtract_overhead(samples[0], overhead),
           (unsigned long)subtract_overhead(samples[SVCPROBE_SAMPLES / 2U],
                                            overhead),
           (unsigned long)subtract_overhead(samples[p99], overhead));
}

#define MEASURE(name, expression, overhead) do { \
    uint32_t sample_; \
    for (sample_ = 0U; sample_ < SVCPROBE_WARMUP; sample_++) \
        sink = (uint32_t)(expression); \
    for (sample_ = 0U; sample_ < SVCPROBE_SAMPLES; sample_++) { \
        uint32_t begin_ = read_pmccntr(); \
        uint32_t result_ = (uint32_t)(expression); \
        uint32_t end_ = read_pmccntr(); \
        samples[sample_] = end_ - begin_; \
        sink = result_; \
    } \
    report((name), (overhead)); \
} while (0)

static int get_info(aar_info *info)
{
    info->size = (uint32_t)sizeof *info;
    return aar_get_info(info);
}

int aar_main(int argc, char **argv)
{
    uint32_t pair_cost;
    uint32_t pmuserenr;
    aar_info info;

    (void)argc;
    (void)argv;
#if !defined(__aarch64__)
    {
        int status = svc_probe(SVC_PROBE_P0);

        if (status == AAR_E_NOSYS) {
            printf("svcprobe: firmware built without SVC probes\n");
            return 0;
        }
        if (status != AAR_OK) {
            printf("svcprobe: unexpected P0 result %d\n", status);
            return 1;
        }
    }
#endif
    pmuserenr = read_pmuserenr();
    if ((pmuserenr & 1U) == 0U) {
        printf("svcprobe: PMUSERENR.EN is clear\n");
        return 1;
    }

    pair_cost = measure_read_pair();
    printf("svcprobe: empty PMCCNTR read pair median %lu cycles (subtracted)\n",
           (unsigned long)pair_cost);
#if !defined(__aarch64__)
    MEASURE("P0", svc_probe(SVC_PROBE_P0), pair_cost);
    MEASURE("P1", svc_probe(SVC_PROBE_P1), pair_cost);
    MEASURE("P2", svc_probe(SVC_PROBE_P2), pair_cost);
    MEASURE("P3", svc_probe(SVC_PROBE_P3), pair_cost);
    MEASURE("P4", svc_probe(SVC_PROBE_P4), pair_cost);
#endif
    MEASURE("should_stop", aar_should_stop(), pair_cost);
    MEASURE("time_us", aar_time_us(), pair_cost);
    MEASURE("info", get_info(&info), pair_cost);
    return 0;
}
