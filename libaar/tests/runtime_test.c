/*
 * libaar libc-flavour test app, run by runtime_test.sh under tests/loader.c.
 * Checks the entry contract (argc, argv, table), .preinit_array and
 * .init_array (priorities too), newlib over libaar layer 2 (printf, malloc
 * across heap grow and shrink, aar_alloc, fopen for every mode, stat), and the
 * wrappers on a table without the optional slots. Prints "runtime_test PASS"
 * and returns 0 when everything holds.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

/* newlib declares sbrk only outside strict C11. */
extern void *sbrk(ptrdiff_t increment);

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL line %d: %s (errno %d)\n", __LINE__, #cond, errno); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static int order[4];
static int steps;

static void note_step(int id)
{
    if (steps < 4)
        order[steps++] = id;
}

static void preinit_fn(void)
{
    note_step(1);
}

static void (*const preinit_entry)(void)
    __attribute__((section(".preinit_array"), used)) = preinit_fn;

__attribute__((constructor(101))) static void early_ctor(void)
{
    note_step(2);
}

__attribute__((constructor)) static void default_ctor(void)
{
    note_step(3);
}

static int test_init_arrays(void)
{
    CHECK(steps == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);
    return 0;
}

static int test_heap(void)
{
    aar_heap_info h0, h1, h2;
    char *big;
    void *p;
    int i;

    big = malloc(5u << 20);
    CHECK(big);
    memset(big, 0xa5, 5u << 20);
    CHECK((unsigned char)big[(5u << 20) - 1] == 0xa5);
    free(big);
    for (i = 0; i < 200; i++) {
        p = malloc(100000);
        CHECK(p);
        memset(p, i, 100000);
        free(p);
    }

    /* sbrk beyond the committed heap grows it; giving it back shrinks it. */
    CHECK(aar_get_heap_info(&h0) == AAR_OK);
    p = sbrk(6 << 20);
    CHECK(p != (void *)-1);
    memset(p, 1, 6u << 20);
    CHECK(aar_get_heap_info(&h1) == AAR_OK && h1.committed >= h0.committed);
    CHECK(sbrk(-(6 << 20)) != (void *)-1);
    CHECK(aar_get_heap_info(&h2) == AAR_OK && h2.committed < h1.committed);
    errno = 0;
    CHECK(sbrk(100 << 20) == (void *)-1 && errno == ENOMEM);

    p = aar_alloc(100, 64);
    CHECK(p && ((size_t)p & 63) == 0);
    aar_free(p);
    p = aar_alloc(10, 4096);
    CHECK(p && ((size_t)p & 4095) == 0);
    aar_free(p);
    p = aar_alloc(10, 0);
    CHECK(p);
    aar_free(p);
    CHECK(aar_alloc(10, 3) == NULL);
    return 0;
}

static int test_files(void)
{
    char buf[64];
    struct stat st;
    FILE *f;
    long n;

    f = fopen("t1.txt", "w");
    CHECK(f && fprintf(f, "hello file %d\n", 7) > 0 && fclose(f) == 0);
    f = fopen("t1.txt", "r");
    CHECK(f && fgets(buf, sizeof buf, f));
    CHECK(strcmp(buf, "hello file 7\n") == 0 && fgets(buf, sizeof buf, f) == NULL);
    fclose(f);

    f = fopen("t1.txt", "a");
    CHECK(f);
    fputs("more\n", f);
    fclose(f);
    f = fopen("t1.txt", "r+");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    CHECK(ftell(f) == 18);
    fseek(f, 6, SEEK_SET);
    CHECK(fgetc(f) == 'f');
    fclose(f);

    CHECK(stat("t1.txt", &st) == 0 && st.st_size == 18 && S_ISREG(st.st_mode));
    errno = 0;
    CHECK(stat("nope.txt", &st) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(fopen("nope.txt", "r") == NULL && errno == ENOENT);

    f = fopen("t1.txt", "w+");
    CHECK(f);
    fputs("abc", f);
    rewind(f);
    CHECK(fgetc(f) == 'a');
    fclose(f);
    f = fopen("t1.txt", "r");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    CHECK(n == 3);
    fclose(f);
    return 0;
}

static int test_wrappers(void)
{
    aar_info info;
    char buf[4];

    info.size = (uint32_t)sizeof info;
    CHECK(aar_get_info(&info) == AAR_OK);
    CHECK(aar_publish(buf, 1) == AAR_OK && aar_acquire(buf, 1) == AAR_OK);
    /* The table has no display, audio or GL group (and with CORE_ONLY no
     * optional slot at all). */
    CHECK(aar_display_vsync() == AAR_E_NOSYS);
    CHECK(aar_audio_close() == AAR_E_NOSYS);
    CHECK(aar_gl_bind(1) == NULL);
    glClear(1);
    CHECK(aar_ext(AAR_PLATFORM_ZZ9000, 1) == NULL);
    CHECK(aar_display_latest_dropped() == 0);
    return 0;
}

int aar_main(int argc, char **argv)
{
    CHECK(argc == 2 && strcmp(argv[0], "app") == 0 && strcmp(argv[1], "one") == 0);
    CHECK(aar_api_ptr != NULL);
    if (test_init_arrays() || test_heap() || test_files() || test_wrappers())
        return 1;
    printf("printf %d %s %5.2f\n", 42, "ok", 3.14159);
    puts("runtime_test PASS");
    return 0;
}
