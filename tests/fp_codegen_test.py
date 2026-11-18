#!/usr/bin/env python3
"""Check the code generation claims in the developer guide's bit-identical
maths section against the installed toolchain. No pytest.

Usage: fp_codegen_test.py [a32|a64]     (default: AAR_PROFILE, else a32)

Set CROSS to the toolchain prefix (default arm-none-eabi- for a32,
aarch64-none-elf- for a64). A claim that the compiler stops honouring fails
here, so a toolchain bump cannot change float results unnoticed.
"""
import os
import re
import subprocess
import sys
import tempfile

PROFILE = (sys.argv[1] if len(sys.argv) > 1
           else os.environ.get("AAR_PROFILE", "a32"))
if PROFILE not in ("a32", "a64"):
    sys.exit("usage: fp_codegen_test.py [a32|a64]")

CC = os.environ.get("CROSS", "arm-none-eabi-" if PROFILE == "a32"
                    else "aarch64-none-elf-") + "gcc"
# The SDK's float flags from mk/aar.mk.
if PROFILE == "a32":
    SDK = ["-mcpu=cortex-a9", "-marm", "-mfpu=neon", "-mfloat-abi=hard"]
else:
    SDK = ["-march=armv8-a"]
SDK += ["-fPIE", "-O2", "-fno-strict-aliasing", "-ffp-contract=off",
        "-std=c11"]

KERNELS = r"""
#include <math.h>
float muladd_f(float a, float b, float c) { return a * b + c; }
double muladd_d(double a, double b, double c) { return a * b + c; }
float fused_f(float a, float b, float c) { return __builtin_fmaf(a, b, c); }
void add_arr(float *restrict d, const float *restrict a,
             const float *restrict b, int n)
{
    int i;
    for (i = 0; i < n; ++i)
        d[i] = a[i] + b[i];
}
float dot(const float *a, const float *b, int n)
{
    float s = 0.0f;
    int i;
    for (i = 0; i < n; ++i)
        s += a[i] * b[i];
    return s;
}
float folded(void) { return sinf(0.5f); }
"""

NEON_MLA = """#include <arm_neon.h>
float32x4_t f(float32x4_t a, float32x4_t b, float32x4_t c)
{ return vmlaq_f32(a, b, c); }
"""

NEON_FMA = """#include <arm_neon.h>
float32x4_t f(float32x4_t a, float32x4_t b, float32x4_t c)
{ return vfmaq_f32(a, b, c); }
"""


def compile_c(source, flags):
    with tempfile.TemporaryDirectory() as root:
        src = os.path.join(root, "t.c")
        with open(src, "w", encoding="ascii") as fh:
            fh.write(source)
        proc = subprocess.run([CC] + flags + ["-S", "-o", "-", src],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True, check=False)
    return proc.returncode, proc.stdout, proc.stderr


def function(asm, name):
    match = re.search(r"^%s:\n(.*?)\t\.size\t%s," % (name, name), asm,
                      re.S | re.M)
    if not match:
        raise SystemExit("function %s not found in assembly" % name)
    return match.group(1)


def check(cond, what):
    if not cond:
        raise SystemExit("fp codegen (%s): %s" % (PROFILE, what))


def replace_opt(flags, new):
    return [f for f in flags if f != "-O2"] + [new]


def check_a32():
    rc, asm, err = compile_c(KERNELS, SDK)
    check(rc == 0, "kernel compile failed: " + err)

    for name in ("muladd_f", "muladd_d"):
        body = function(asm, name)
        check("vfma" not in body and "vfnma" not in body,
              "%s uses a fused multiply-add" % name)
        check("vmla" in body or "vmul" in body, "%s has no VFP multiply" % name)
    check("fmaf" in function(asm, "fused_f"),
          "__builtin_fmaf is not a libm call: the target gained FMA")
    for contract in ("off", "fast"):
        rc2, asm2, _ = compile_c(KERNELS, SDK + ["-ffp-contract=" + contract])
        check(rc2 == 0, "compile with -ffp-contract=%s" % contract)
        check(function(asm2, "muladd_f") == function(asm, "muladd_f"),
              "-ffp-contract=%s changes muladd_f" % contract)

    o3 = replace_opt(SDK, "-O3")
    rc, asm3, err = compile_c(KERNELS, o3)
    check(rc == 0, "-O3 compile failed: " + err)
    for name in ("add_arr", "dot"):
        body = function(asm3, name)
        check(not re.search(r"\tv(add|mla|mul)\.f32\tq", body),
              "-O3 moved %s onto NEON q registers without fast-math" % name)
    rc, asm4, err = compile_c(KERNELS, o3 + ["-ffast-math"])
    check(rc == 0, "-ffast-math compile failed: " + err)
    check(re.search(r"\tvadd\.f32\tq", function(asm4, "add_arr")),
          "control: -ffast-math no longer vectorises add_arr, so the check "
          "above proves nothing")

    check(re.search(r"vldr\.32\ts0, \.L", function(asm, "folded")),
          "sinf(0.5f) is no longer folded")
    rc, asm5, err = compile_c(KERNELS, SDK + ["-fno-builtin"])
    check(rc == 0 and "sinf" in function(asm5, "folded"),
          "-fno-builtin does not call sinf")

    rc, asm6, err = compile_c(NEON_MLA, SDK)
    check(rc == 0 and re.search(r"\tvmla\.f32\tq", asm6),
          "vmlaq_f32 is not a NEON multiply-accumulate")
    check("vfma" not in asm6, "vmlaq_f32 became a fused operation")
    rc, _, err = compile_c(NEON_FMA, SDK)
    check(rc != 0 and "vfmaq_f32" in err,
          "vfmaq_f32 compiles with -mfpu=neon: the target gained FMA")


def check_a64():
    """A64 has fused multiply-add in the base architecture, so the guarantee
    is different: the SDK flags (-ffp-contract=off) keep a multiply and an add
    apart in every language mode, and fusing is only what the program asks for
    (__builtin_fmaf, vfmaq_f32)."""
    rc, asm, err = compile_c(KERNELS, SDK)
    check(rc == 0, "kernel compile failed: " + err)

    for name in ("muladd_f", "muladd_d"):
        body = function(asm, name)
        check("fmadd" not in body and "fnmadd" not in body and
              "fmsub" not in body and "fnmsub" not in body,
              "%s uses a fused multiply-add" % name)
        check("fmul" in body and "fadd" in body,
              "%s is not a multiply and an add" % name)
    check("fmadd" in function(asm, "fused_f"),
          "__builtin_fmaf is no longer an inline fused multiply-add")

    # The flag, not the language mode, is what keeps results apart: GNU mode
    # fuses by default, the SDK flags do not.
    gnu = [f for f in SDK if f != "-std=c11"] + ["-std=gnu11"]
    rc, asm2, err = compile_c(KERNELS, gnu)
    check(rc == 0, "gnu11 compile failed: " + err)
    check(function(asm2, "muladd_f") == function(asm, "muladd_f"),
          "-std=gnu11 with the SDK flags fuses muladd_f")
    no_flag = [f for f in gnu if f != "-ffp-contract=off"]
    rc, asm3, err = compile_c(KERNELS, no_flag)
    check(rc == 0, "gnu11 without the flag failed: " + err)
    check("fmadd" in function(asm3, "muladd_f"),
          "control: -std=gnu11 without -ffp-contract=off no longer fuses, so "
          "the check above proves nothing")

    o3 = replace_opt(SDK, "-O3")
    rc, asm4, err = compile_c(KERNELS, o3)
    check(rc == 0, "-O3 compile failed: " + err)
    # GCC may vectorise the exact products, but the sum stays a scalar chain in
    # source order: no vector add, no fused accumulate, no pairwise add.
    check(not re.search(r"\tf(add|mla)\tv|\tfaddp", function(asm4, "dot")),
          "-O3 reassociated the dot reduction onto vector registers without "
          "fast-math")
    rc, asm5, err = compile_c(KERNELS, o3 + ["-ffast-math"])
    check(rc == 0, "-ffast-math compile failed: " + err)
    check(re.search(r"\tf(add|mla)\tv|\tfaddp", function(asm5, "dot")),
          "control: -ffast-math no longer vectorises dot, so the check above "
          "proves nothing")

    check("bl\t" not in function(asm, "folded"),
          "sinf(0.5f) is no longer folded")
    rc, asm6, err = compile_c(KERNELS, SDK + ["-fno-builtin"])
    check(rc == 0 and "sinf" in function(asm6, "folded"),
          "-fno-builtin does not call sinf")

    rc, asm7, err = compile_c(NEON_MLA, SDK)
    check(rc == 0 and re.search(r"\tfmul\tv", asm7) and
          re.search(r"\tfadd\tv", asm7),
          "vmlaq_f32 is not a separate multiply and add")
    check("fmla" not in asm7, "vmlaq_f32 became a fused operation")
    rc, asm8, err = compile_c(NEON_FMA, SDK)
    check(rc == 0 and re.search(r"\tfmla\tv", asm8),
          "vfmaq_f32 is not a fused multiply-accumulate")


def main():
    if PROFILE == "a32":
        check_a32()
    else:
        check_a64()
    sys.stdout.write("fp codegen (%s): ok\n" % PROFILE)


if __name__ == "__main__":
    main()
