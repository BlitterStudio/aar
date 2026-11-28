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
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <utime.h>
#include <fcntl.h>
#include <unistd.h>

#include "aar/app.h"
#include "aar/note.h"
#include "aar/ext_zz9000.h"

AAR_NOTE(0, 0, 0, 0)

/* newlib declares sbrk only outside strict C11. */
extern void *sbrk(ptrdiff_t increment);

/* newlib's <utime.h> has the structure and no prototype. */
extern int utime(const char *path, const struct utimbuf *times);

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
    memcpy(buf, "old", sizeof buf);
    CHECK(aar_file_request(0, NULL, NULL, buf, sizeof buf) == AAR_E_NOSYS);
    CHECK(memcmp(buf, "old", sizeof buf) == 0);
    return 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    CHECK(f && fputs(text, f) >= 0 && fclose(f) == 0);
    return 0;
}

static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/* unlink, remove, mkdir, rmdir, rename. */
static int test_names(void)
{
    char buf[16];

    CHECK(write_text("u.txt", "x") == 0);
    CHECK(unlink("u.txt") == 0 && !exists("u.txt"));
    errno = 0;
    CHECK(unlink("u.txt") == -1 && errno == ENOENT);
    CHECK(write_text("u.txt", "x") == 0);
    CHECK(remove("u.txt") == 0 && !exists("u.txt"));

    CHECK(mkdir("d1", 0755) == 0 && exists("d1"));
    errno = 0;
    CHECK(mkdir("d1", 0755) == -1 && errno == EEXIST);
    errno = 0;
    CHECK(mkdir("nope/sub", 0755) == -1 && errno == ENOENT);
    CHECK(write_text("d1/in.txt", "x") == 0);
    errno = 0;
    CHECK(rmdir("d1") == -1 && errno == ENOTEMPTY);
    errno = 0;
    CHECK(rmdir("d1/in.txt") == -1 && errno == ENOTDIR && exists("d1/in.txt"));
    CHECK(unlink("d1/in.txt") == 0);
    CHECK(rmdir("d1") == 0 && !exists("d1"));
    errno = 0;
    CHECK(rmdir("d1") == -1 && errno == ENOENT);
    CHECK(mkdir("d2", 0755) == 0 && remove("d2") == 0 && !exists("d2"));

    /* rename replaces an existing target, as POSIX says. */
    CHECK(write_text("a.txt", "A") == 0 && write_text("b.txt", "B") == 0);
    CHECK(rename("a.txt", "b.txt") == 0);
    CHECK(!exists("a.txt") && slurp("b.txt", buf, sizeof buf) == 1 && buf[0] == 'A');
    CHECK(rename("b.txt", "c.txt") == 0 && !exists("b.txt") && exists("c.txt"));
    errno = 0;
    CHECK(rename("a.txt", "c.txt") == -1 && errno == ENOENT);
    CHECK(slurp("c.txt", buf, sizeof buf) == 1 && buf[0] == 'A');
    /* A rename onto itself changes nothing. */
    CHECK(rename("c.txt", "c.txt") == 0 && slurp("c.txt", buf, sizeof buf) == 1);
    errno = 0;
    CHECK(rename("a.txt", "a.txt") == -1 && errno == ENOENT);
    CHECK(mkdir("d3", 0755) == 0 && rename("d3", "d4") == 0 && exists("d4"));
    CHECK(rmdir("d4") == 0 && unlink("c.txt") == 0);
    return 0;
}

static int test_truncate(void)
{
    char buf[32];
    struct stat st;
    int fd;

    fd = open("tr.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "hello world", 11) == 11);
    CHECK(ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 && st.st_size == 5);
    CHECK(lseek(fd, 0, SEEK_CUR) == 11);
    CHECK(ftruncate(fd, 8) == 0 && fstat(fd, &st) == 0 && st.st_size == 8);
    CHECK(close(fd) == 0);
    CHECK(slurp("tr.txt", buf, sizeof buf) == 8 && memcmp(buf, "hello\0\0\0", 8) == 0);

    CHECK(truncate("tr.txt", 2) == 0 && stat("tr.txt", &st) == 0 && st.st_size == 2);
    CHECK(truncate("tr.txt", 0) == 0 && stat("tr.txt", &st) == 0 && st.st_size == 0);
    errno = 0;
    CHECK(truncate("tr-missing.txt", 0) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(truncate("tr.txt", -1) == -1 && errno == EINVAL);

    /* An id opened for reading only cannot be truncated; the console is not
     * a file. */
    fd = open("tr.txt", O_RDONLY);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(ftruncate(fd, 0) == -1 && errno == EBADF);
    CHECK(close(fd) == 0);
    errno = 0;
    CHECK(ftruncate(1, 0) == -1 && errno == EINVAL);
    CHECK(unlink("tr.txt") == 0);
    return 0;
}

static mode_t perm_of(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (st.st_mode & 0777) : (mode_t)~0u;
}

/* chmod, the permission bits of stat and fstat, and the protection bits that
 * chmod leaves alone. */
static int test_modes(void)
{
    struct stat st;
    aar_stat as;
    int fd;

    CHECK(write_text("m.txt", "m") == 0);
    CHECK(chmod("m.txt", 0755) == 0 && perm_of("m.txt") == 0755);
    CHECK(chmod("m.txt", 0644) == 0 && perm_of("m.txt") == 0644);
    CHECK(chmod("m.txt", 0444) == 0 && perm_of("m.txt") == 0444);
    fd = open("m.txt", O_RDONLY);
    /* fstat knows only the descriptor: a regular file with the default
     * permission bits and no date. The real bits are those of stat. */
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
    CHECK((st.st_mode & 0777) == 0644 && st.st_size == 1 && st.st_mtime == 0);
    CHECK(close(fd) == 0);
    /* The client refuses to delete what is write protected. */
    errno = 0;
    CHECK(unlink("m.txt") == -1 && errno == EACCES && exists("m.txt"));
    CHECK(aar_file_stat("m.txt", &as) == AAR_OK);
    CHECK((as.protect & (AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_EXECUTE)) ==
          (AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_EXECUTE));
    CHECK(!(as.protect & AAR_PROT_READ));

    CHECK(chmod("m.txt", 0644) == 0);
    CHECK(aar_file_set_protect("m.txt", AAR_PROT_ARCHIVE | AAR_PROT_SCRIPT) == AAR_OK);
    CHECK(perm_of("m.txt") == 0755);
    CHECK(chmod("m.txt", 0600) == 0);
    CHECK(aar_file_stat("m.txt", &as) == AAR_OK);
    CHECK(as.protect == (AAR_PROT_ARCHIVE | AAR_PROT_SCRIPT | AAR_PROT_EXECUTE));
    CHECK(perm_of("m.txt") == 0644);
    errno = 0;
    CHECK(chmod("m-missing.txt", 0644) == -1 && errno == ENOENT);

    CHECK(mkdir("md", 0755) == 0 && stat("md", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(rmdir("md") == 0 && unlink("m.txt") == 0);
    return 0;
}

/* fstat describes the descriptor, not the path: after the path was renamed
 * and replaced, and after it was deleted and made again, fstat still gives
 * the size of the open file and the default bits, while stat of the path
 * gives the replacement's size and date. The raw AAR calls do the renaming,
 * so nothing in the libc layer can track it. */
static int test_fstat_descriptor(void)
{
    struct stat st;
    struct utimbuf ub;
    int fd;

    CHECK(write_text("fd-open.txt", "12345") == 0);
    fd = open("fd-open.txt", O_RDONLY);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 5);

    /* The path is renamed away and a dated file of another size takes its
     * name. */
    CHECK(aar_file_rename("fd-open.txt", "fd-moved.txt", 0) == AAR_OK);
    CHECK(write_text("fd-repl.txt", "abcdefg") == 0);
    ub.actime = 1500000000;
    ub.modtime = 1500000000;
    CHECK(utime("fd-repl.txt", &ub) == 0);
    CHECK(aar_file_rename("fd-repl.txt", "fd-open.txt", AAR_RENAME_REPLACE) == AAR_OK);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 5 && (st.st_mode & 0777) == 0644 &&
          st.st_mtime == 0);
    CHECK(stat("fd-open.txt", &st) == 0 && st.st_size == 7 && st.st_mtime == 1500000000);
    CHECK(lseek(fd, 0, SEEK_END) == 5);

    /* The path is deleted and made again with other contents. */
    CHECK(aar_file_delete("fd-open.txt") == AAR_OK);
    CHECK(write_text("fd-open.txt", "x") == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 5);
    CHECK(stat("fd-open.txt", &st) == 0 && st.st_size == 1);
    CHECK(close(fd) == 0);
    CHECK(unlink("fd-open.txt") == 0 && unlink("fd-moved.txt") == 0);
    return 0;
}

static int test_utime(void)
{
    struct utimbuf ub;
    struct timeval tv[2];
    struct stat st;
    time_t now;

    CHECK(write_text("ut.txt", "t") == 0);
    ub.actime = 1000000000;
    ub.modtime = 1500000000;
    CHECK(utime("ut.txt", &ub) == 0 && stat("ut.txt", &st) == 0);
    CHECK(st.st_mtime == 1500000000);
    tv[0].tv_sec = 1;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = 1600000000;
    tv[1].tv_usec = 500000;
    CHECK(utimes("ut.txt", tv) == 0 && stat("ut.txt", &st) == 0);
    CHECK(st.st_mtime == 1600000000);
    /* A NULL time is now. */
    CHECK(utime("ut.txt", NULL) == 0 && stat("ut.txt", &st) == 0);
    now = time(NULL);
    CHECK(st.st_mtime >= now - 5 && st.st_mtime <= now + 5);
    CHECK(utimes("ut.txt", NULL) == 0 && stat("ut.txt", &st) == 0);
    CHECK(st.st_mtime >= now - 5 && st.st_mtime <= now + 5);
    errno = 0;
    CHECK(utime("ut-missing.txt", &ub) == -1 && errno == ENOENT);
    CHECK(unlink("ut.txt") == 0);
    return 0;
}

/* runtime_test.sh starts the loader with UNIX_US_BASE=1700000000123456. */
static int test_clock(void)
{
    struct timeval a, b;
    struct tms t;
    clock_t c0, c1;
    time_t now, again;

    CHECK(gettimeofday(&a, NULL) == 0);
    CHECK(a.tv_sec >= 1700000000 && a.tv_sec < 1700000000 + 120);
    CHECK(a.tv_usec >= 0 && a.tv_usec < 1000000);
    CHECK(gettimeofday(&b, NULL) == 0);
    CHECK(b.tv_sec > a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_usec >= a.tv_usec));
    now = time(&again);
    CHECK(now == again && now >= a.tv_sec && now <= a.tv_sec + 5);

    c0 = clock();
    CHECK(c0 != (clock_t)-1);
    aar_sleep_us(30000);
    c1 = clock();
    CHECK(c1 - c0 >= (clock_t)(25 * CLOCKS_PER_SEC / 1000));
    CHECK(c1 - c0 < (clock_t)(5 * CLOCKS_PER_SEC));
    CHECK(times(&t) != (clock_t)-1 && t.tms_utime >= c1 && t.tms_stime == 0);
    return 0;
}

static unsigned host_env_calls(void)
{
    char buf[16];
    uint32_t n = 0;

    /* The loader's counter, read around newlib's cache. */
    if (aar_env_get("AAR_ENV_CALLS", buf, sizeof buf, &n) != AAR_OK)
        return ~0u;
    return (unsigned)atoi(buf);
}

/* runtime_test.sh starts the loader with AAR_TEST_VAR=hello and an
 * AAR_TEST_LONG of 300 characters. */
static int test_env(void)
{
    const char *v, *again;
    unsigned calls;
    struct tm local, utc;
    time_t t = 1700000000;

    calls = host_env_calls();
    CHECK(calls != ~0u);
    v = getenv("AAR_TEST_VAR");
    CHECK(v && strcmp(v, "hello") == 0);
    again = getenv("AAR_TEST_VAR");
    CHECK(again == v);
    CHECK(host_env_calls() == calls + 1);

    v = getenv("AAR_TEST_LONG");
    CHECK(v && strlen(v) == 300 && v[299] == 'z');
    CHECK(getenv("AAR_TEST_MISSING") == NULL);
    calls = host_env_calls();
    CHECK(getenv("AAR_TEST_MISSING") == NULL);
    CHECK(host_env_calls() == calls);
    CHECK(getenv("") == NULL && getenv("A=B") == NULL);

    /* The program's changes stay in the program. */
    CHECK(setenv("AAR_TEST_VAR", "kept", 0) == 0);
    CHECK(strcmp(getenv("AAR_TEST_VAR"), "hello") == 0);
    CHECK(setenv("AAR_TEST_VAR", "changed", 1) == 0);
    CHECK(strcmp(getenv("AAR_TEST_VAR"), "changed") == 0);
    CHECK(unsetenv("AAR_TEST_VAR") == 0 && getenv("AAR_TEST_VAR") == NULL);
    CHECK(setenv("AAR_TEST_VAR", "again", 0) == 0);
    CHECK(strcmp(getenv("AAR_TEST_VAR"), "again") == 0);
    CHECK(putenv("AAR_TEST_PUT=1") == 0 && strcmp(getenv("AAR_TEST_PUT"), "1") == 0);
    CHECK(putenv("AAR_TEST_PUT") == 0 && getenv("AAR_TEST_PUT") == NULL);
    errno = 0;
    CHECK(setenv("", "x", 1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(setenv("A=B", "x", 1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(unsetenv("A=B") == -1 && errno == EINVAL);

    /* newlib's own readers (here tzset) see the same table. */
    CHECK(setenv("TZ", "XYZ-1", 1) == 0);
    tzset();
    local = *localtime(&t);
    utc = *gmtime(&t);
    CHECK(local.tm_hour == (utc.tm_hour + 1) % 24);
    CHECK(unsetenv("TZ") == 0);
    return 0;
}

int aar_main(int argc, char **argv)
{
    CHECK(argc == 2 && strcmp(argv[0], "app") == 0 && strcmp(argv[1], "one") == 0);
    CHECK(aar_api_ptr != NULL);
    if (test_init_arrays() || test_heap() || test_files() ||
        test_open_flags() || test_big_size() || test_wrappers() ||
        test_names() || test_truncate() || test_modes() || test_fstat_descriptor() ||
        test_utime() ||
        test_clock() || test_env())
        return 1;
    printf("printf %d %s %5.2f\n", 42, "ok", 3.14159);
    puts("runtime_test PASS");
    return 0;
}
