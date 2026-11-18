/*
 * Floating-point environment probe. Prints the control register as the app
 * sees it on entry and checks the denormal behaviour the developer guide's
 * bit-identical maths section states. Exit status 0 means every check held.
 *
 * A32: FPSCR must be 0 on entry; a VFP add keeps a denormal result and a NEON
 * add flushes it to zero.
 * A64: FPCR must be 0 on entry; a scalar add and an ASIMD add both keep the
 * denormal, because FPCR.FZ is clear.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include <arm_neon.h>

AAR_NOTE(0, 0, 0, 0)

#if defined(__aarch64__)
#define CONTROL_NAME "entry fpcr="
#define SCALAR_NAME "scalar denormal sum="
#define SIMD_NAME "asimd denormal sum="
#define SIMD_EXPECT 2u

static uint32_t read_control(void)
{
    uint64_t v;

    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (uint32_t)v;
}
#else
#define CONTROL_NAME "entry fpscr="
#define SCALAR_NAME "vfp denormal sum="
#define SIMD_NAME "neon denormal sum="
#define SIMD_EXPECT 0u

static uint32_t read_control(void)
{
    uint32_t v;

    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    return v;
}
#endif

static char *put_str(char *out, const char *s)
{
    while (*s)
        *out++ = *s++;
    return out;
}

static char *put_hex(char *out, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    for (shift = 28; shift >= 0; shift -= 4)
        *out++ = digits[(v >> shift) & 0xfu];
    return out;
}

static int report(const char *what, uint32_t value, int ok)
{
    char line[64];
    char *end = put_str(line, "fpenv: ");

    end = put_str(end, what);
    end = put_hex(end, value);
    end = put_str(end, ok ? " ok\n" : " FAIL\n");
    aar_log(line, (uint32_t)(end - line));
    return ok;
}

static uint32_t bits_of(float f)
{
    union {
        float f;
        uint32_t u;
    } v;

    v.f = f;
    return v.u;
}

int aar_main(int argc, char **argv)
{
    /* Read the control register before any floating-point work in the app. */
    uint32_t entry = read_control();
    union {
        uint32_t u;
        float f;
    } tiny;
    volatile float denormal;
    volatile float scalar_sum;
    float32x4_t quad;
    float simd_sum;
    int ok = 1;

    (void)argc;
    (void)argv;

    tiny.u = 1u; /* the smallest positive denormal */
    denormal = tiny.f;
    scalar_sum = denormal + denormal;
    quad = vaddq_f32(vdupq_n_f32(denormal), vdupq_n_f32(denormal));
    simd_sum = vgetq_lane_f32(quad, 0);

    ok &= report(CONTROL_NAME, entry, entry == 0u);
    ok &= report(SCALAR_NAME, bits_of(scalar_sum),
                 bits_of(scalar_sum) == 2u);
    ok &= report(SIMD_NAME, bits_of(simd_sum), bits_of(simd_sum) == SIMD_EXPECT);
    return ok ? 0 : 1;
}
