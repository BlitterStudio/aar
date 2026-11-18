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
#include <fcntl.h>
#include <unistd.h>

#include "aar/app.h"
#include "aar/note.h"
#include "aar/ext_zz9000.h"

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

    /* O_APPEND holds for every write, not only at open: an lseek back to 0
     * must not make the next write overwrite the start. */
    {
        int fd = open("t1.txt", O_WRONLY | O_APPEND);

        CHECK(fd >= 0);
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        CHECK(write(fd, "X", 1) == 1);
        CHECK(close(fd) == 0);
    }
    f = fopen("t1.txt", "r");
    CHECK(f && fgets(buf, sizeof buf, f));
    CHECK(strcmp(buf, "abcX") == 0);
    fclose(f);
    return 0;
}

static int slurp(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return (int)n;
}

static unsigned stat_calls_so_far(void)
{
    struct stat st;

    /* loader.c answers this path with the count of the other stat calls. */
    return stat("stat-calls.magic", &st) == 0 ? (unsigned)st.st_size : ~0u;
}

/* O_CREAT, O_TRUNC and O_EXCL reach the runtime as one open call each. */
static int test_open_flags(void)
{
    char buf[16];
    unsigned before;
    int fd;

    fd = open("flags.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "keep", 4) == 4 && close(fd) == 0);

    /* O_CREAT | O_EXCL on an existing file fails and leaves it alone. */
    errno = 0;
    CHECK(open("flags.txt", O_WRONLY | O_CREAT | O_EXCL, 0644) == -1);
    CHECK(errno == EEXIST);
    CHECK(slurp("flags.txt", buf, sizeof buf) == 4 && strcmp(buf, "keep") == 0);

    /* O_CREAT without O_TRUNC keeps the contents, and a write lands at 0. */
    fd = open("flags.txt", O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0);
    CHECK(read(fd, buf, 2) == 2 && memcmp(buf, "ke", 2) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && write(fd, "K", 1) == 1);
    CHECK(close(fd) == 0);
    CHECK(slurp("flags.txt", buf, sizeof buf) == 4 && strcmp(buf, "Keep") == 0);

    /* O_TRUNC alone truncates an existing file and does not create. */
    fd = open("flags.txt", O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(slurp("flags.txt", buf, sizeof buf) == 0);
    errno = 0;
    CHECK(open("flags-missing.txt", O_WRONLY | O_TRUNC) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(open("flags-missing.txt", O_WRONLY) == -1 && errno == ENOENT);

    /* O_CREAT | O_EXCL on a missing file creates it, and the open is one call
     * of the runtime: no stat of the path comes first that a file appearing
     * in between could outrun. */
    before = stat_calls_so_far();
    fd = open("flags-new.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0 && write(fd, "new", 3) == 3 && close(fd) == 0);
    CHECK(stat_calls_so_far() == before);
    CHECK(slurp("flags-new.txt", buf, sizeof buf) == 3 && strcmp(buf, "new") == 0);

    /* O_CREAT | O_TRUNC creates or replaces. */
    fd = open("flags-new.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "z", 1) == 1 && close(fd) == 0);
    CHECK(slurp("flags-new.txt", buf, sizeof buf) == 1 && strcmp(buf, "z") == 0);
    CHECK(stat_calls_so_far() == before);
    return 0;
}

/* A size the 32-bit off_t of A32 cannot hold is an error, not a wrapped
 * value; on A64 the size is exact. The loader answers "huge.bin" with 3 GiB. */
static int test_big_size(void)
{
    struct stat st;
    int fd;

    errno = 0;
    if (sizeof(off_t) == 4) {
        CHECK(stat("huge.bin", &st) == -1 && errno == EOVERFLOW);
    } else {
        CHECK(stat("huge.bin", &st) == 0);
        CHECK((long long)st.st_size == (3ll << 30));
    }
    fd = open("huge.bin", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(lseek(fd, 5, SEEK_SET) == 5);
    errno = 0;
    if (sizeof(off_t) == 4) {
        CHECK(fstat(fd, &st) == -1 && errno == EOVERFLOW);
    } else {
        CHECK(fstat(fd, &st) == 0);
        CHECK((long long)st.st_size == (3ll << 30));
    }
    /* The failed or successful fstat put the position back. */
    CHECK(lseek(fd, 0, SEEK_CUR) == 5);
    CHECK(close(fd) == 0);
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
    /* No extension table: the bind wrapper finds nothing and binds nothing. */
    CHECK(aar_zz9000_gl_bind_flags(1, 0) == NULL);
    glClear(1);
    CHECK(aar_display_latest_dropped() == 0);
    return 0;
}

int aar_main(int argc, char **argv)
{
    CHECK(argc == 2 && strcmp(argv[0], "app") == 0 && strcmp(argv[1], "one") == 0);
    CHECK(aar_api_ptr != NULL);
    if (test_init_arrays() || test_heap() || test_files() ||
        test_open_flags() || test_big_size() || test_wrappers())
        return 1;
    printf("printf %d %s %5.2f\n", 42, "ok", 3.14159);
    puts("runtime_test PASS");
    return 0;
}
