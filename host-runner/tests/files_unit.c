/*
 * Unit checks for the host runner file proxy. Includes files.c directly so
 * the descriptor table is observable from the test. memory.c is linked for
 * the shared buffers that map_file uses.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/files.c"

#include <assert.h>

/* files.c refuses the calls that change a file after a stop; the runner's
 * stop flag is this one here. */
static int unit_stop;

int host_stop_requested(void)
{
    return unit_stop;
}

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

static int read_file(const char *leaf, char *buf, size_t n)
{
    char path[4096];
    FILE *fp;
    size_t got;

    snprintf(path, sizeof path, "%s/%s", UNIT_ROOT, leaf);
    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    got = fread(buf, 1, n, fp);
    fclose(fp);
    return (int)got;
}

static void test_delete_and_dir_create(void)
{
    int fd;

    host_files_init(UNIT_ROOT, 0);
    put_file("del.txt", "x");
    assert(api_file_delete("del.txt") == AAR_OK);
    assert(file_size("del.txt") == -1);
    assert(api_file_delete("del.txt") == AAR_E_NOENT);
    assert(api_dir_create("deldir") == AAR_OK);
    assert(api_dir_create("deldir") == AAR_E_EXIST);
    assert(api_dir_create("nope/sub") == AAR_E_NOENT);
    put_file("deldir/in.txt", "x");
    assert(api_file_delete("deldir") == AAR_E_EXIST); /* not empty */
    assert(api_file_delete("deldir/in.txt") == AAR_OK);
    assert(api_file_delete("deldir") == AAR_OK);
    /* The path rules of file_open. */
    assert(api_file_delete("../outside") == AAR_E_ACCES);
    assert(api_file_delete("vol:x") == AAR_E_ACCES);
    assert(api_file_delete("") == AAR_E_INVAL);
    assert(api_file_delete(0) == AAR_E_INVAL);
    assert(api_dir_create("../outside") == AAR_E_ACCES);
    assert(api_dir_create("") == AAR_E_INVAL);
#ifndef _WIN32
    /* An open id holds its file. */
    put_file("busy.txt", "x");
    fd = api_file_open("busy.txt", AAR_FILE_READ);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_delete("busy.txt") == AAR_E_BUSY);
    assert(file_size("busy.txt") == 1);
    assert(api_file_close(fd) == AAR_OK);
    assert(api_file_delete("busy.txt") == AAR_OK);
#else
    (void)fd;
#endif
    /* After a stop nothing is created or removed. */
    put_file("stop.txt", "x");
    unit_stop = 1;
    assert(api_file_delete("stop.txt") == AAR_E_ACCES);
    assert(api_dir_create("stopdir") == AAR_E_ACCES);
    unit_stop = 0;
    assert(file_size("stop.txt") == 1);
    assert(api_dir_create("stopdir") == AAR_OK);
    assert(api_file_delete("stopdir") == AAR_OK);
    assert(api_file_delete("stop.txt") == AAR_OK);
    host_files_shutdown();
}

static void test_rename(void)
{
    char buf[8];

    host_files_init(UNIT_ROOT, 0);
    put_file("ra.txt", "aaa");
    put_file("rb.txt", "b");
    assert(api_file_rename("ra.txt", "rc.txt", 0) == AAR_OK);
    assert(file_size("ra.txt") == -1 && file_size("rc.txt") == 3);
    /* An existing target is refused unless the flag says replace. */
    assert(api_file_rename("rc.txt", "rb.txt", 0) == AAR_E_EXIST);
    assert(file_size("rb.txt") == 1 && file_size("rc.txt") == 3);
    assert(api_file_rename("rc.txt", "rb.txt", AAR_RENAME_REPLACE) == AAR_OK);
    assert(file_size("rb.txt") == 3 && file_size("rc.txt") == -1);
    assert(read_file("rb.txt", buf, sizeof buf) == 3 && memcmp(buf, "aaa", 3) == 0);
    assert(api_file_rename("rb.txt", "rb2.txt", 2u) == AAR_E_INVAL);
    assert(api_file_rename("absent.txt", "x.txt", 0) == AAR_E_NOENT);
    assert(api_file_rename("rb.txt", "../out.txt", 0) == AAR_E_ACCES);
    assert(api_file_rename("../out.txt", "rb.txt", 0) == AAR_E_ACCES);
    assert(api_file_rename("", "rb.txt", 0) == AAR_E_INVAL);
    assert(api_file_rename("rb.txt", 0, 0) == AAR_E_INVAL);
#ifndef _WIN32
    assert(api_dir_create("rd1") == AAR_OK);
    assert(api_dir_create("rd2") == AAR_OK);
    put_file("rd2/f", "x");
    assert(api_file_rename("rd1", "rd2", 0) == AAR_E_EXIST);
    assert(api_file_rename("rd1", "rd2", AAR_RENAME_REPLACE) == AAR_E_EXIST);
    assert(api_file_rename("rd2", "rd2/sub", 0) == AAR_E_INVAL);
    assert(api_file_rename("rb.txt", "rd2", AAR_RENAME_REPLACE) == AAR_E_EXIST);
    assert(api_file_delete("rd2/f") == AAR_OK);
    assert(api_file_delete("rd2") == AAR_OK);
    assert(api_file_delete("rd1") == AAR_OK);
#endif
    unit_stop = 1;
    assert(api_file_rename("rb.txt", "rb3.txt", 0) == AAR_E_ACCES);
    unit_stop = 0;
    assert(file_size("rb.txt") == 3);
    assert(api_file_delete("rb.txt") == AAR_OK);
    host_files_shutdown();
}

static void test_truncate(void)
{
    char buf[16];
    uint32_t n = 0;
    int fd;
    int ro;

    host_files_init(UNIT_ROOT, 0);
    put_file("tr.bin", "abcdef");
    fd = api_file_open("tr.bin", AAR_FILE_READ | AAR_FILE_WRITE);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_truncate(fd, 3u) == AAR_OK);
    assert(api_file_close(fd) == AAR_OK);
    assert(file_size("tr.bin") == 3);
    /* Longer: the new bytes are zero. */
    fd = api_file_open("tr.bin", AAR_FILE_READ | AAR_FILE_WRITE);
    assert(api_file_truncate(fd, 8u) == AAR_OK);
    assert(api_file_close(fd) == AAR_OK);
    assert(read_file("tr.bin", buf, sizeof buf) == 8);
    assert(memcmp(buf, "abc\0\0\0\0\0", 8) == 0);
    /* A write that is still buffered is flushed first, and cut. */
    fd = api_file_open("tr.bin", AAR_FILE_READ | AAR_FILE_WRITE);
    assert(api_file_write(fd, "xyz", 3u, &n) == AAR_OK && n == 3u);
    assert(api_file_truncate(fd, 2u) == AAR_OK);
    assert(api_file_close(fd) == AAR_OK);
    assert(read_file("tr.bin", buf, sizeof buf) == 2 && memcmp(buf, "xy", 2) == 0);
    /* The id must be open for writing. */
    ro = api_file_open("tr.bin", AAR_FILE_READ);
    assert(api_file_truncate(ro, 0u) == AAR_E_BADF);
    assert(api_file_close(ro) == AAR_OK);
    assert(api_file_truncate(HOST_FD_BASE + 15, 0u) == AAR_E_BADF);
    assert(api_file_truncate(-1, 0u) == AAR_E_BADF);
    assert(api_file_truncate(HOST_FD_BASE, UINT64_MAX) != AAR_OK);
    fd = api_file_open("tr.bin", AAR_FILE_READ | AAR_FILE_WRITE);
    unit_stop = 1;
    assert(api_file_truncate(fd, 0u) == AAR_E_ACCES);
    unit_stop = 0;
    assert(api_file_truncate(fd, (uint64_t)INT64_MAX + 1u) == AAR_E_INVAL);
    assert(api_file_close(fd) == AAR_OK);
    assert(file_size("tr.bin") == 2);
    assert(api_file_delete("tr.bin") == AAR_OK);
    host_files_shutdown();
}

static void test_protect_and_date(void)
{
    aar_stat st;

    host_files_init(UNIT_ROOT, 0);
    put_file("pm.txt", "x");
    assert(api_file_stat("pm.txt", &st) == AAR_OK);
    assert((st.protect & (AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_READ)) == 0u);
#ifndef _WIN32
    /* READ, WRITE and EXECUTE are the owner's bits with the sense of
     * AmigaDOS; DELETE follows WRITE; the rest is not stored. */
    assert(api_file_set_protect("pm.txt", AAR_PROT_WRITE | AAR_PROT_DELETE) == AAR_OK);
    assert(api_file_stat("pm.txt", &st) == AAR_OK);
    assert(st.protect == (AAR_PROT_WRITE | AAR_PROT_DELETE));
    assert(api_file_delete("pm.txt") == AAR_E_ACCES); /* delete-protected */
    assert(api_file_set_protect("pm.txt", AAR_PROT_READ | AAR_PROT_EXECUTE) == AAR_OK);
    assert(api_file_stat("pm.txt", &st) == AAR_OK);
    assert(st.protect == (AAR_PROT_READ | AAR_PROT_EXECUTE));
    assert(api_file_set_protect("pm.txt", AAR_PROT_ARCHIVE | AAR_PROT_PURE |
                                AAR_PROT_SCRIPT | AAR_PROT_HOLD) == AAR_OK);
    assert(api_file_stat("pm.txt", &st) == AAR_OK);
    assert(st.protect == 0u);
    assert(api_file_set_protect("pm.txt", 0x100u) == AAR_E_INVAL);
    assert(api_file_set_protect("pm.txt", 0u) == AAR_OK);
    assert(api_file_set_protect("absent.txt", 0u) == AAR_E_NOENT);
    assert(api_file_set_protect("../out", 0u) == AAR_E_ACCES);
    assert(api_dir_create("pd") == AAR_OK);
    assert(api_file_set_protect("pd", AAR_PROT_EXECUTE) == AAR_OK);
    assert(api_file_stat("pd", &st) == AAR_OK);
    assert(st.mode == AAR_MODE_DIR && st.protect == 0u); /* a directory has no EXECUTE */
    assert(api_file_delete("pd") == AAR_OK);
#endif
    assert(api_file_set_date("pm.txt", 1000000000) == AAR_OK);
    assert(api_file_stat("pm.txt", &st) == AAR_OK);
    assert(st.mtime == 1000000000);
    assert(api_file_set_date("pm.txt", -1) == AAR_E_INVAL);
    assert(api_file_set_date("absent.txt", 1000000000) == AAR_E_NOENT);
    assert(api_file_set_date("../out", 1000000000) == AAR_E_ACCES);
    assert(api_file_set_date("", 1000000000) == AAR_E_INVAL);
    unit_stop = 1;
    assert(api_file_set_date("pm.txt", 1100000000) == AAR_E_ACCES);
    assert(api_file_set_protect("pm.txt", AAR_PROT_WRITE) == AAR_E_ACCES);
    unit_stop = 0;
    assert(api_file_stat("pm.txt", &st) == AAR_OK && st.mtime == 1000000000);
    assert(api_file_delete("pm.txt") == AAR_OK);
    host_files_shutdown();
}

static void test_volume_info(void)
{
    aar_volume_info vi;
    unsigned char big[sizeof(aar_volume_info) + 8];
    uint32_t i;

    host_files_init(UNIT_ROOT, 0);
    put_file("vol.txt", "x");
    memset(&vi, 0xaa, sizeof vi);
    vi.size = (uint32_t)sizeof vi;
    assert(api_volume_info(".", &vi) == AAR_OK);
    assert(vi.size == sizeof vi && vi.block_bytes > 0u && vi.total_bytes > 0u);
    assert(vi.free_bytes <= vi.total_bytes);
    for (i = 0; i < 3u; i++)
        assert(vi.reserved[i] == 0u);
    memset(&vi, 0, sizeof vi);
    vi.size = (uint32_t)sizeof vi;
    assert(api_volume_info("vol.txt", &vi) == AAR_OK && vi.total_bytes > 0u);
    assert(api_volume_info("absent.txt", &vi) == AAR_E_NOENT);
    assert(api_volume_info("../out", &vi) == AAR_E_ACCES);
    assert(api_volume_info("", &vi) == AAR_E_INVAL);
    assert(api_volume_info(".", 0) == AAR_E_INVAL);
    vi.size = 2u;
    assert(api_volume_info(".", &vi) == AAR_E_INVAL);
    /* The caller's size bounds what is written. */
    memset(big, 0xaa, sizeof big);
    ((aar_volume_info *)(void *)big)->size = 8u;
    assert(api_volume_info(".", (aar_volume_info *)(void *)big) == AAR_OK);
    assert(((aar_volume_info *)(void *)big)->block_bytes > 0u);
    for (i = 8u; i < sizeof big; i++)
        assert(big[i] == 0xaau);
    memset(big, 0xaa, sizeof big);
    ((aar_volume_info *)(void *)big)->size = (uint32_t)sizeof big;
    assert(api_volume_info(".", (aar_volume_info *)(void *)big) == AAR_OK);
    for (i = sizeof(aar_volume_info); i < sizeof big; i++)
        assert(big[i] == 0u);
    assert(api_file_delete("vol.txt") == AAR_OK);
    host_files_shutdown();
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
    test_delete_and_dir_create();
    test_rename();
    test_truncate();
    test_protect_and_date();
    test_volume_info();
    return 0;
}
