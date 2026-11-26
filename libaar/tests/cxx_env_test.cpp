/*
 * The host environment from a C++ app. libstdc++ references getenv, so its
 * archive pulls newlib's getenv.o out of libc unless the link wraps getenv and
 * _getenv_r to libaar's glue (see mk/aar.mk and cmake/aar.cmake). The app reads
 * what the host gave: AAR_TEST_VAR is "hello" and AAR_TEST_MISSING is not set
 * (tests/cxx_runtime_test.sh starts the loader that way). It calls std::getenv
 * and newlib's reentrant _getenv_r, and throws first so the libstdc++ members
 * that use getenv are in the image.
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "aar/app.h"
#include "aar/note.h"

#include <reent.h>

extern "C" char *_getenv_r(struct _reent *, const char *);

AAR_NOTE(0, 0, 0, 0)

namespace {

int fail_count;

void say(const std::string &line)
{
    aar_log(line.data(), static_cast<uint32_t>(line.size()));
}

void expect(bool ok, const char *what)
{
    if (!ok) {
        ++fail_count;
        say(std::string("FAIL ") + what + "\n");
    }
}

bool equals(const char *value, const char *expected)
{
    return value && std::strcmp(value, expected) == 0;
}

} // namespace

extern "C" int aar_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#if defined(__cpp_exceptions)
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception &) {
    }
#endif

    expect(equals(std::getenv("AAR_TEST_VAR"), "hello"), "std::getenv reads the host");
    expect(std::getenv("AAR_TEST_MISSING") == nullptr, "std::getenv of a missing name");

    expect(equals(_getenv_r(_REENT, "AAR_TEST_VAR"), "hello"), "_getenv_r reads the host");
    expect(_getenv_r(_REENT, "AAR_TEST_MISSING") == nullptr, "_getenv_r of a missing name");

    expect(setenv("AAR_TEST_VAR", "changed", 1) == 0 &&
               equals(std::getenv("AAR_TEST_VAR"), "changed") &&
               equals(_getenv_r(_REENT, "AAR_TEST_VAR"), "changed"),
           "setenv is seen by both");

    say(fail_count == 0 ? "cxx_env PASS\n" : "cxx_env FAIL\n");
    return fail_count == 0 ? 0 : 1;
}
