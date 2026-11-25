/*
 * Unit checks for the host runner file proxy. Includes files.c directly so
 * the descriptor table is observable from the test. memory.c is linked for
 * the shared buffers that map_file uses.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/files.c"

#include <assert.h>

#define UNIT_ROOT "aar_files_unit_root"

static void make_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

static void put_file(const char *leaf, const char *data)
{
    char path[4096];
    FILE *fp;
    size_t n = strlen(data);

    snprintf(path, sizeof path, "%s/%s", UNIT_ROOT, leaf);
    fp = fopen(path, "wb");
    assert(fp != NULL);
    if (n)
        assert(fwrite(data, 1, n, fp) == n);
    assert(fclose(fp) == 0);
}

static int file_size(const char *leaf)
{
    char path[4096];
    struct stat st;

    snprintf(path, sizeof path, "%s/%s", UNIT_ROOT, leaf);
    if (path_stat(path, &st) != 0)
        return -1;
    return (int)st.st_size;
}

static void rm_file(const char *leaf)
{
    char path[4096];

    snprintf(path, sizeof path, "%s/%s", UNIT_ROOT, leaf);
    (void)remove(path);
}

/* CREATE, TRUNCATE and EXCL are independent, like O_CREAT, O_TRUNC and
 * O_EXCL, and each open is one call of the host. */
static void test_create_truncate_excl(void)
{
    uint32_t n = 0;
    char buf[8];
    int fd;

    host_files_init(UNIT_ROOT, 0);
    put_file("flags.bin", "keep");
    rm_file("flags-new.bin");
    rm_file("flags-missing.bin");

    /* CREATE | EXCL on an existing file: AAR_E_EXIST, contents intact. */
    assert(api_file_open("flags.bin", AAR_FILE_WRITE | AAR_FILE_CREATE |
                                      AAR_FILE_EXCL) == AAR_E_EXIST);
    assert(file_size("flags.bin") == 4);

    /* CREATE alone keeps the contents of an existing file. */
    fd = api_file_open("flags.bin",
                       AAR_FILE_READ | AAR_FILE_WRITE | AAR_FILE_CREATE);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_read(fd, buf, 4u, &n) == AAR_OK && n == 4u &&
           memcmp(buf, "keep", 4) == 0);
    assert(api_file_close(fd) == AAR_OK);
    assert(file_size("flags.bin") == 4);

    /* TRUNCATE alone empties an existing file but does not create one. */
    assert(api_file_open("flags-missing.bin",
                         AAR_FILE_WRITE | AAR_FILE_TRUNCATE) == AAR_E_NOENT);
    assert(file_size("flags-missing.bin") < 0);
    fd = api_file_open("flags.bin", AAR_FILE_WRITE | AAR_FILE_TRUNCATE);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_close(fd) == AAR_OK);
    assert(file_size("flags.bin") == 0);

    /* CREATE | EXCL creates a missing file once; the second try fails. */
    fd = api_file_open("flags-new.bin", AAR_FILE_WRITE | AAR_FILE_CREATE |
                                        AAR_FILE_EXCL);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_write(fd, "new", 3u, &n) == AAR_OK && n == 3u);
    assert(api_file_close(fd) == AAR_OK);
    assert(api_file_open("flags-new.bin", AAR_FILE_WRITE | AAR_FILE_CREATE |
                                          AAR_FILE_EXCL) == AAR_E_EXIST);
    assert(file_size("flags-new.bin") == 3);

    /* CREATE | TRUNCATE is create or replace. */
    fd = api_file_open("flags-new.bin", AAR_FILE_WRITE | AAR_FILE_CREATE |
                                        AAR_FILE_TRUNCATE);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_close(fd) == AAR_OK);
    assert(file_size("flags-new.bin") == 0);

    /* Combinations the standard rejects, and a bit it does not define. */
    assert(api_file_open("flags.bin", AAR_FILE_READ | AAR_FILE_EXCL) ==
           AAR_E_INVAL);
    assert(api_file_open("flags.bin", AAR_FILE_WRITE | AAR_FILE_EXCL) ==
           AAR_E_INVAL);
    assert(api_file_open("flags.bin", AAR_FILE_READ | AAR_FILE_CREATE) ==
           AAR_E_INVAL);
    assert(api_file_open("flags.bin", AAR_FILE_READ | AAR_FILE_TRUNCATE) ==
           AAR_E_INVAL);
    assert(api_file_open("flags.bin", AAR_FILE_CREATE) == AAR_E_INVAL);
    assert(api_file_open("flags.bin", 0u) == AAR_E_INVAL);
    assert(api_file_open("flags.bin", AAR_FILE_WRITE | 32u) == AAR_E_INVAL);
    host_files_shutdown();
}

static void test_create_keeps_file_when_slots_full(void)
{
    char name[32];
    int fd[HOST_FD_COUNT];
    int i;

    host_files_init(UNIT_ROOT, 0);
    put_file("victim.bin", "keep me");
    for (i = 0; i < HOST_FD_COUNT; i++) {
        snprintf(name, sizeof name, "f%02d.bin", i);
        put_file(name, "x");
        fd[i] = api_file_open(name, AAR_FILE_READ);
        assert(fd[i] == HOST_FD_BASE + i);
    }
    /* Every slot taken: the create must fail without truncating victim. */
    assert(api_file_open("victim.bin",
                         AAR_FILE_READ | AAR_FILE_WRITE | AAR_FILE_CREATE |
                         AAR_FILE_TRUNCATE) == AAR_E_NOMEM);
    /* Nor may an exclusive create leave a new file behind. */
    rm_file("fresh.bin");
    assert(api_file_open("fresh.bin", AAR_FILE_WRITE | AAR_FILE_CREATE |
                                      AAR_FILE_EXCL) == AAR_E_NOMEM);
    assert(file_size("fresh.bin") < 0);
    for (i = 0; i < HOST_FD_COUNT; i++)
        assert(api_file_close(fd[i]) == AAR_OK);
    assert(file_size("victim.bin") == 7);
    host_files_shutdown();
}

/* Files and lists are separate tables of AAR_MIN_FILES, from one id range. */
static void test_lists_have_their_own_table(void)
{
    int fd[HOST_FD_COUNT];
    int list[HOST_FD_COUNT];
    int i;

    assert(HOST_FD_COUNT >= (int)AAR_MIN_FILES);
    host_files_init(UNIT_ROOT, 0);
    put_file("f00.bin", "x");
    for (i = 0; i < HOST_FD_COUNT; i++) {
        fd[i] = api_file_open("f00.bin", AAR_FILE_READ);
        assert(fd[i] == HOST_FD_BASE + i);
    }
    for (i = 0; i < HOST_FD_COUNT; i++) {
        list[i] = api_file_list_open(".");
        assert(list[i] == HOST_FD_BASE + i);
    }
    assert(api_file_open("f00.bin", AAR_FILE_READ) == AAR_E_NOMEM);
    assert(api_file_list_open(".") == AAR_E_NOMEM);
    for (i = 0; i < HOST_FD_COUNT; i++) {
        assert(api_file_close(fd[i]) == AAR_OK);
        assert(api_file_list_close(list[i]) == AAR_OK);
    }
    host_files_shutdown();
}

static void test_seek_out_pos_is_optional(void)
{
    int fd;
    int64_t pos = -1;

    host_files_init(UNIT_ROOT, 0);
    put_file("seek.bin", "0123456789");
    fd = api_file_open("seek.bin", AAR_FILE_READ);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_seek(fd, 4, AAR_SEEK_SET, NULL) == AAR_OK);
    assert(api_file_seek(fd, 2, AAR_SEEK_CUR, &pos) == AAR_OK && pos == 6);
    assert(api_file_seek(fd, -1, AAR_SEEK_END, &pos) == AAR_OK && pos == 9);
    assert(api_file_seek(fd, 0, 3, &pos) == AAR_E_INVAL);
    assert(api_file_close(fd) == AAR_OK);
    host_files_shutdown();
}

/* An id remembers how it was opened (spec 7.1): a write through an id opened
 * without AAR_FILE_WRITE and a read through an id opened without AAR_FILE_READ
 * fail with AAR_E_BADF and change nothing. */
static void test_access_mode_is_kept_per_id(void)
{
    char buf[8];
    uint32_t n = 99;
    int ro, wo, rw;

    host_files_init(UNIT_ROOT, 0);
    put_file("mode.bin", "abcdef");
    ro = api_file_open("mode.bin", AAR_FILE_READ);
    wo = api_file_open("mode.bin", AAR_FILE_WRITE);
    rw = api_file_open("mode.bin", AAR_FILE_READ | AAR_FILE_WRITE);
    assert(ro >= HOST_FD_BASE && wo >= HOST_FD_BASE && rw >= HOST_FD_BASE);

    assert(api_file_write(ro, "XY", 2, &n) == AAR_E_BADF);
    assert(n == 99u);
    assert(api_file_read(wo, buf, sizeof buf, &n) == AAR_E_BADF);
    assert(n == 99u);
    /* The refused calls moved no position and wrote no byte. */
    assert(api_file_read(ro, buf, sizeof buf, &n) == AAR_OK);
    assert(n == 6u && memcmp(buf, "abcdef", 6) == 0);
    assert(api_file_write(wo, "XY", 2, &n) == AAR_OK && n == 2u);
    assert(api_file_close(wo) == AAR_OK); /* flushes */
    assert(api_file_seek(rw, 0, AAR_SEEK_SET, NULL) == AAR_OK);
    assert(api_file_read(rw, buf, 6, &n) == AAR_OK);
    assert(n == 6u && memcmp(buf, "XYcdef", 6) == 0);
    assert(api_file_seek(rw, 0, AAR_SEEK_SET, NULL) == AAR_OK);
    assert(api_file_write(rw, "Z", 1, &n) == AAR_OK && n == 1u);
    assert(api_file_seek(rw, 0, AAR_SEEK_SET, NULL) == AAR_OK);
    assert(api_file_read(rw, buf, 6, &n) == AAR_OK);
    assert(n == 6u && memcmp(buf, "ZYcdef", 6) == 0);

    assert(api_file_close(ro) == AAR_OK);
    assert(api_file_close(rw) == AAR_OK);
    assert(file_size("mode.bin") == 6);
    host_files_shutdown();
}

static void test_close_reports_flush_failure(void)
{
#ifndef _WIN32
    FILE *fp;

    host_files_init(UNIT_ROOT, 0);
    fp = fopen("/dev/full", "wb");
    if (!fp)
        return; /* environment without /dev/full; nothing to assert */
    /* Buffered: the failure surfaces when fclose flushes. */
    assert(fwrite("x", 1, 1, fp) == 1);
    fds[0].used = 1;
    fds[0].fp = fp;
    assert(api_file_close(HOST_FD_BASE) == AAR_E_NOSPC);
    assert(!fds[0].used); /* the descriptor is released despite the error */
    assert(api_file_close(HOST_FD_BASE) == AAR_E_BADF);
    host_files_shutdown();
#endif
}

static void test_map_file_is_a_runtime_block(void)
{
    const void *data = 0;
    uint64_t bytes = 0;

    host_memory_init();
    host_files_init(UNIT_ROOT, 0);
    put_file("map.bin", "mapped");
    assert(api_map_file("map.bin", &data, &bytes) == AAR_OK);
    assert(bytes == 6u && memcmp(data, "mapped", 6) == 0);
    assert(host_shared_used() == HOST_SHARED_GRANULE);
    api_unmap_file(data);
    assert(host_shared_used() == 0u);
    put_file("empty.bin", "");
    assert(api_map_file("empty.bin", &data, &bytes) == AAR_E_NOMEM);
    assert(api_map_file("absent.bin", &data, &bytes) == AAR_E_NOENT);
    host_files_shutdown();
    host_memory_shutdown();
}

int main(void)
{
    make_dir(UNIT_ROOT);
    test_create_keeps_file_when_slots_full();
    test_create_truncate_excl();
    test_lists_have_their_own_table();
    test_seek_out_pos_is_optional();
    test_close_reports_flush_failure();
    test_access_mode_is_kept_per_id();
    test_map_file_is_a_runtime_block();
    return 0;
}
