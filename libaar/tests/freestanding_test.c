/*
 * libaar freestanding-flavour test app, run by runtime_test.sh under
 * tests/loader.c. No libc: crt0 passes the status to aar_exit, init arrays run,
 * the weak mem* routines serve compiler-generated calls, and the private heap
 * comes from aar_heap_range and aar_heap_grow. Logs "freestanding_test PASS"
 * and returns 0 when everything holds.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

static int constructed;

__attribute__((constructor)) static void ctor(void)
{
    constructed = 1;
}

typedef struct {
    uint8_t bytes[300];
} block;

static void say(const char *s)
{
    uint32_t n = 0;

    while (s[n])
        n++;
    aar_log(s, n);
}

int aar_main(int argc, char **argv)
{
    static block src, dst;
    void *base = 0;
    void *old_break = 0;
    uint64_t bytes = 0;
    uint8_t *heap;
    unsigned i;

    if (!constructed || argc != 2 || !argv || !argv[0] || !aar_api_ptr) {
        say("freestanding_test FAIL entry\n");
        return 1;
    }
    for (i = 0; i < sizeof src.bytes; i++)
        src.bytes[i] = (uint8_t)(i * 7u);
    dst = src; /* a struct copy: GCC calls memcpy */
    for (i = 0; i < sizeof dst.bytes; i++) {
        if (dst.bytes[i] != src.bytes[i]) {
            say("freestanding_test FAIL memcpy\n");
            return 1;
        }
    }
    aar_heap_range(&base, &bytes);
    if (!base || aar_heap_grow(2u * AAR_HEAP_GRANULE, &old_break) != AAR_OK ||
        (uint8_t *)old_break != (uint8_t *)base + bytes) {
        say("freestanding_test FAIL heap\n");
        return 1;
    }
    heap = (uint8_t *)old_break;
    for (i = 0; i < 2u * AAR_HEAP_GRANULE; i += 4096u)
        heap[i] = 1;
    if (heap[0] != 1 || heap[AAR_HEAP_GRANULE] != 1) {
        say("freestanding_test FAIL heap write\n");
        return 1;
    }
    say("freestanding_test PASS\n");
    return 0;
}
