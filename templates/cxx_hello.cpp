/*
 * C++ in an AAR app: a global object with a constructor and a destructor, the
 * standard containers, operator new and delete on the libc heap, a virtual
 * call, and (when the build has exceptions) a throw and catch. Every result is
 * written with aar_log and checked; the exit status is 0 when all held.
 *
 * Build it like any app, with .cpp in SRCS (mk/aar.mk) or as a source of
 * aar_add_executable (cmake/aar.cmake). The libc flavour only: the
 * freestanding flavour has no C++ runtime.
 *
 * SPDX-License-Identifier: MIT
 */
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

namespace {

void say(const std::string &line)
{
    aar_log(line.data(), static_cast<uint32_t>(line.size()));
}

/* Runs before aar_main, from .init_array, and again at exit from the
 * __cxa_atexit list that exit() walks. */
struct Global {
    int value;
    Global() : value(42) { say("global constructor ran\n"); }
    ~Global() { say("global destructor ran\n"); }
};
Global g_global;

/* The earliest priority an app may use. Its constructor throws and catches
 * before any other constructor runs, so the unwinder's tables must already be
 * registered (on A64 libaar does that from .preinit_array). */
struct EarlyCatch {
    bool caught;
    EarlyCatch() : caught(false)
    {
#if defined(__cpp_exceptions)
        try {
            throw 7;
        } catch (int) {
            caught = true;
        }
#else
        caught = true;
#endif
    }
};
EarlyCatch g_early __attribute__((init_priority(101)));

struct Shape {
    virtual ~Shape() {}
    virtual int sides() const = 0;
};
struct Triangle : Shape {
    int sides() const override { return 3; }
};
struct Square : Shape {
    int sides() const override { return 4; }
};

/* Initialised on first use, through the guard variable of libsupc++. */
const std::string &static_name()
{
    static const std::string name("function-level static");
    return name;
}

int fail_count;

void expect(bool ok, const char *what)
{
    if (!ok) {
        ++fail_count;
        say(std::string("FAIL ") + what + "\n");
    }
}

} // namespace

extern "C" int aar_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    expect(g_global.value == 42, "global constructor");
    expect(g_early.caught, "throw and catch in a priority-101 constructor");

    std::vector<int> numbers;
    for (int i = 0; i < 1000; ++i)
        numbers.push_back(i * i);
    expect(numbers.size() == 1000 && numbers[999] == 999 * 999, "vector");

    std::string text = "hello ";
    text += "from c++";
    expect(text == "hello from c++", "string");
    say(text + "\n");

    expect(static_name() == "function-level static" &&
               &static_name() == &static_name(),
           "function-level static");

    int *one = new int(7);
    int *many = new int[256];
    std::memset(many, 0, 256 * sizeof(int));
    expect(*one == 7 && many[255] == 0, "new");
    delete one;
    delete[] many;

    Shape *shapes[2] = {new Triangle, new Square};
    int total = 0;
    for (Shape *s : shapes) {
        total += s->sides();
        delete s;
    }
    expect(total == 7, "virtual call");

#if defined(__cpp_rtti)
    Triangle tri;
    Shape &base = tri;
    expect(dynamic_cast<Square *>(&base) == nullptr, "dynamic_cast");
    expect(typeid(base) == typeid(Triangle), "typeid");
#endif

#if defined(__cpp_exceptions)
    bool caught = false;
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception &e) {
        caught = std::strcmp(e.what(), "boom") == 0;
    }
    expect(caught, "throw and catch");

    caught = false;
    try {
        (void)numbers.at(5000);
    } catch (const std::out_of_range &) {
        caught = true;
    }
    expect(caught, "library exception");

    caught = false;
    try {
        std::string guard = "unwound";
        throw 17;
    } catch (int v) {
        caught = v == 17;
    }
    expect(caught, "throw int");
    say("exceptions: on, 3 caught\n");
#else
    say("exceptions: off\n");
#endif

    say(fail_count == 0 ? "cxx_hello PASS\n" : "cxx_hello FAIL\n");
    return fail_count == 0 ? 0 : 1;
}
