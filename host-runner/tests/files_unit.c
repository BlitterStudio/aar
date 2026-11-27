/*
 * Unit checks for the host runner file proxy. Includes files.c directly so
 * the descriptor table is observable from the test. memory.c is linked for
 * the shared buffers that map_file uses.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/files.c"

#include <SDL_timer.h>
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

    snprintf(path, sizeof path, "%s/%s", UNIT_ROOT, leaf);
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) == 0 ? (int)st.st_size : -1;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (int)st.st_size : -1;
#endif
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

#ifndef _WIN32
/* A listing classifies its entries through the open directory, so it keeps
 * doing so after the directory, or a parent of it, was renamed: the path it
 * was opened with no longer names it. */
static void test_list_survives_a_rename(void)
{
    aar_dirent ent;
    int list;
    int dirs = 0;
    int files = 0;

    host_files_init(UNIT_ROOT, 0);
    assert(api_dir_create("top") == AAR_OK);
    assert(api_dir_create("top/mid") == AAR_OK);
    assert(api_dir_create("top/mid/inner") == AAR_OK);
    put_file("top/mid/plain.txt", "x");

    /* The listed directory itself is renamed. */
    list = api_file_list_open("top/mid");
    assert(list >= HOST_FD_BASE);
    assert(api_file_rename("top/mid", "top/moved", 0) == AAR_OK);
    while (api_file_list_next(list, &ent) == AAR_OK) {
        if (strcmp(ent.name, "inner") == 0 && ent.mode == AAR_MODE_DIR)
            dirs++;
        if (strcmp(ent.name, "plain.txt") == 0 && ent.mode == AAR_MODE_FILE)
            files++;
    }
    assert(dirs == 1 && files == 1);
    assert(api_file_list_close(list) == AAR_OK);

    /* An ancestor of the listed directory is renamed. */
    list = api_file_list_open("top/moved");
    assert(list >= HOST_FD_BASE);
    assert(api_file_rename("top", "top2", 0) == AAR_OK);
    dirs = files = 0;
    while (api_file_list_next(list, &ent) == AAR_OK) {
        if (strcmp(ent.name, "inner") == 0 && ent.mode == AAR_MODE_DIR)
            dirs++;
        if (strcmp(ent.name, "plain.txt") == 0 && ent.mode == AAR_MODE_FILE)
            files++;
    }
    assert(dirs == 1 && files == 1);
    assert(api_file_list_close(list) == AAR_OK);
    assert(api_file_delete("top2/moved/plain.txt") == AAR_OK);
    assert(api_file_delete("top2/moved/inner") == AAR_OK);
    assert(api_file_delete("top2/moved") == AAR_OK);
    assert(api_file_delete("top2") == AAR_OK);
    host_files_shutdown();
}
#endif

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
    int list;

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
    /* An open id holds its file: by identity, so another spelling of the
     * path counts too. */
    put_file("busy.txt", "x");
    fd = api_file_open("busy.txt", AAR_FILE_READ);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_delete("busy.txt") == AAR_E_BUSY);
    assert(api_file_delete("./busy.txt") == AAR_E_BUSY);
    assert(file_size("busy.txt") == 1);
#ifndef _WIN32
    /* ... and so does a hard link to it. */
    {
        char a[4096];
        char b[4096];

        snprintf(a, sizeof a, "%s/busy.txt", UNIT_ROOT);
        snprintf(b, sizeof b, "%s/busy.link", UNIT_ROOT);
        assert(link(a, b) == 0);
        assert(api_file_delete("busy.link") == AAR_E_BUSY);
        assert(api_file_close(fd) == AAR_OK);
        assert(api_file_delete("busy.link") == AAR_OK);
    }
#else
    assert(api_file_close(fd) == AAR_OK);
#endif
    assert(api_file_delete("busy.txt") == AAR_OK);
    /* An open listing holds its directory. */
    assert(api_dir_create("listdir") == AAR_OK);
    list = api_file_list_open("listdir");
    assert(list >= HOST_FD_BASE);
    assert(api_file_delete("listdir") == AAR_E_BUSY);
    assert(api_file_list_close(list) == AAR_OK);
    assert(api_file_delete("listdir") == AAR_OK);
    /* A listing of the parent does not hold a child. */
    list = api_file_list_open(".");
    put_file("free.txt", "x");
    assert(api_file_delete("free.txt") == AAR_OK);
    assert(api_file_list_close(list) == AAR_OK);
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
    /* The same object: success and no change, with or without the flag. */
    assert(api_file_rename("rb.txt", "rb.txt", 0) == AAR_OK);
    assert(api_file_rename("rb.txt", "./rb.txt", AAR_RENAME_REPLACE) == AAR_OK);
    assert(file_size("rb.txt") == 3);
#ifndef _WIN32
    {
        char from[4096];
        char to[4096];

        snprintf(from, sizeof from, "%s/rb.txt", UNIT_ROOT);
        snprintf(to, sizeof to, "%s/rb.link", UNIT_ROOT);
        assert(link(from, to) == 0);
        assert(api_file_rename("rb.txt", "rb.link", 0) == AAR_OK);
        assert(api_file_rename("rb.link", "rb.txt", AAR_RENAME_REPLACE) == AAR_OK);
        assert(file_size("rb.txt") == 3 && file_size("rb.link") == 3);
        assert(unlink(to) == 0);
    }
#endif
    /* Replacing an open target is refused and preserves its contents.
     * The same-object no-op comes before that check. */
    put_file("rt.txt", "old");
    put_file("rs.txt", "new");
    {
        int tfd = api_file_open("rt.txt", AAR_FILE_READ);
        int lst;

        assert(tfd >= HOST_FD_BASE);
        assert(api_file_rename("rs.txt", "rt.txt", AAR_RENAME_REPLACE) == AAR_E_BUSY);
        assert(api_file_rename("rs.txt", "rt.txt", 0) == AAR_E_EXIST);
        assert(read_file("rt.txt", buf, sizeof buf) == 3 && memcmp(buf, "old", 3) == 0);
        assert(file_size("rs.txt") == 3);
        assert(api_file_rename("rt.txt", "rt.txt", AAR_RENAME_REPLACE) == AAR_OK);
        assert(api_file_close(tfd) == AAR_OK);
        assert(api_file_rename("rs.txt", "rt.txt", AAR_RENAME_REPLACE) == AAR_OK);
        assert(read_file("rt.txt", buf, sizeof buf) == 3 && memcmp(buf, "new", 3) == 0);
        assert(file_size("rs.txt") == -1);
        assert(api_file_delete("rt.txt") == AAR_OK);
        /* An empty directory that a listing reads is in use too. */
        assert(api_dir_create("rl.dir") == AAR_OK);
        assert(api_dir_create("rs.dir") == AAR_OK);
        put_file("rs.dir/source.txt", "x");
        lst = api_file_list_open("rl.dir");
        assert(lst >= HOST_FD_BASE);
        assert(api_file_rename("rs.dir", "rl.dir", AAR_RENAME_REPLACE) == AAR_E_BUSY);
        assert(api_dir_create("rs.dir") == AAR_E_EXIST); /* nothing moved */
        assert(api_file_rename("rl.dir", "rl.dir", AAR_RENAME_REPLACE) == AAR_OK);
        assert(api_file_list_close(lst) == AAR_OK);
#ifdef _WIN32
        assert(api_file_rename("rs.dir", "rl.dir", AAR_RENAME_REPLACE) == AAR_E_NOSYS);
        assert(file_size("rs.dir/source.txt") == 1);
        assert(api_file_delete("rs.dir/source.txt") == AAR_OK);
        assert(api_file_delete("rs.dir") == AAR_OK);
#else
        assert(api_file_rename("rs.dir", "rl.dir", AAR_RENAME_REPLACE) == AAR_OK);
        assert(api_file_delete("rl.dir/source.txt") == AAR_OK);
#endif
        assert(api_file_delete("rl.dir") == AAR_OK);
    }
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
    /* A read that reached the end set the stream's end-of-file flag. After
     * an extension the next read, without a seek, returns the new bytes. */
    put_file("eof.bin", "abc");
    fd = api_file_open("eof.bin", AAR_FILE_READ | AAR_FILE_WRITE);
    assert(fd >= HOST_FD_BASE);
    assert(api_file_read(fd, buf, 8u, &n) == AAR_OK && n == 3u);
    assert(api_file_read(fd, buf, 8u, &n) == AAR_OK && n == 0u); /* the end */
    assert(api_file_truncate(fd, 6u) == AAR_OK);
    memset(buf, 'x', sizeof buf);
    assert(api_file_read(fd, buf, 8u, &n) == AAR_OK && n == 3u);
    assert(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 'x');
    assert(api_file_read(fd, buf, 8u, &n) == AAR_OK && n == 0u); /* the new end */
    assert(api_file_close(fd) == AAR_OK);
    assert(file_size("eof.bin") == 6);
    assert(api_file_delete("eof.bin") == AAR_OK);
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
    /* Both hosts map the read-only state to WRITE and DELETE together. */
    assert(api_file_set_protect("pm.txt", AAR_PROT_WRITE | AAR_PROT_DELETE) == AAR_OK);
    assert(api_file_stat("pm.txt", &st) == AAR_OK);
    assert(st.protect == (AAR_PROT_WRITE | AAR_PROT_DELETE));
    assert(api_file_delete("pm.txt") == AAR_E_ACCES); /* delete-protected */
#ifndef _WIN32
    /* Every mask of the four lower bits that the host can store round-trips;
     * any other is AAR_E_NOSYS and leaves the protection as it was. */
    {
        uint32_t stored = AAR_PROT_WRITE | AAR_PROT_DELETE;
        uint32_t mask;
        int storable;

        for (mask = 0u; mask < 16u; mask++) {
            storable = ((mask & AAR_PROT_WRITE) != 0u) ==
                       ((mask & AAR_PROT_DELETE) != 0u);
            assert(api_file_set_protect("pm.txt", mask) ==
                   (storable ? AAR_OK : AAR_E_NOSYS));
            assert(api_file_stat("pm.txt", &st) == AAR_OK);
            assert(st.protect == (storable ? mask : stored));
            stored = st.protect;
        }
    }
#else
    assert(api_file_set_protect("pm.txt", AAR_PROT_READ) == AAR_E_NOSYS);
    assert(api_file_set_protect("pm.txt", AAR_PROT_EXECUTE) == AAR_E_NOSYS);
#endif
    /* Bits the host has nowhere to keep, and DELETE without WRITE (or the
     * other way round): refused before anything changes. */
    assert(api_file_set_protect("pm.txt", AAR_PROT_ARCHIVE | AAR_PROT_PURE |
                                AAR_PROT_SCRIPT | AAR_PROT_HOLD) == AAR_E_NOSYS);
    assert(api_file_set_protect("pm.txt", AAR_PROT_SCRIPT) == AAR_E_NOSYS);
    assert(api_file_set_protect("pm.txt", AAR_PROT_DELETE) == AAR_E_NOSYS);
    assert(api_file_set_protect("pm.txt", AAR_PROT_WRITE) == AAR_E_NOSYS);
    assert(api_file_stat("pm.txt", &st) == AAR_OK);
#ifndef _WIN32
    assert(st.protect == (AAR_PROT_READ | AAR_PROT_WRITE | AAR_PROT_DELETE |
                          AAR_PROT_EXECUTE)); /* the last mask of the loop */
#else
    assert(st.protect == (AAR_PROT_WRITE | AAR_PROT_DELETE));
#endif
    assert(api_file_set_protect("pm.txt", 0x100u) == AAR_E_INVAL);
    assert(api_file_set_protect("pm.txt", 0u) == AAR_OK);
    assert(api_file_stat("pm.txt", &st) == AAR_OK && st.protect == 0u);
    assert(api_file_set_protect("absent.txt", 0u) == AAR_E_NOENT);
    assert(api_file_set_protect("absent.txt", AAR_PROT_SCRIPT) == AAR_E_NOENT);
    assert(api_file_set_protect("../out", 0u) == AAR_E_ACCES);
    assert(api_file_set_protect("../out", AAR_PROT_SCRIPT) == AAR_E_ACCES);
#ifndef _WIN32
    assert(api_dir_create("pd") == AAR_OK);
    /* A directory has no EXECUTE: the mask is refused, not ignored. */
    assert(api_file_set_protect("pd", AAR_PROT_EXECUTE) == AAR_E_NOSYS);
    assert(api_file_stat("pd", &st) == AAR_OK);
    assert(st.mode == AAR_MODE_DIR && st.protect == 0u);
    assert(api_file_set_protect("pd", AAR_PROT_WRITE | AAR_PROT_DELETE) == AAR_OK);
    assert(api_file_stat("pd", &st) == AAR_OK);
    assert(st.protect == (AAR_PROT_WRITE | AAR_PROT_DELETE));
    assert(api_file_set_protect("pd", AAR_PROT_WRITE | AAR_PROT_DELETE |
                                AAR_PROT_EXECUTE) == AAR_E_NOSYS);
    assert(api_file_stat("pd", &st) == AAR_OK);
    assert(st.protect == (AAR_PROT_WRITE | AAR_PROT_DELETE));
    assert(api_file_set_protect("pd", 0u) == AAR_OK);
    assert(api_file_delete("pd") == AAR_OK);
#endif
    /* A filesystem keeps only the times it can. An ordinary time is kept
     * exactly; an extreme one is refused, or kept exactly where the
     * filesystem has the range for it. */
    assert(api_file_set_date("pm.txt", 1000000000) == AAR_OK);
    assert(api_file_stat("pm.txt", &st) == AAR_OK);
    assert(st.mtime == 1000000000);
    {
        static const int64_t extremes[] = {253402300799ll, 1ll << 40, INT64_MAX};
        size_t i;

        for (i = 0; i < sizeof extremes / sizeof extremes[0]; i++) {
            int rc = api_file_set_date("pm.txt", extremes[i]);

            assert(rc == AAR_OK || rc == AAR_E_INVAL);
            assert(api_file_stat("pm.txt", &st) == AAR_OK);
            if (rc == AAR_OK)
                assert(st.mtime == extremes[i]);
        }
    }
    assert(api_file_set_date("pm.txt", 1000000000) == AAR_OK);
    assert(api_file_stat("pm.txt", &st) == AAR_OK && st.mtime == 1000000000);
    assert(api_file_set_date("pm.txt", -1) == AAR_E_INVAL);
    assert(api_file_set_date("absent.txt", 1000000000) == AAR_E_NOENT);
    assert(api_file_set_date("../out", 1000000000) == AAR_E_ACCES);
    assert(api_file_set_date("", 1000000000) == AAR_E_INVAL);
    unit_stop = 1;
    assert(api_file_set_date("pm.txt", 1100000000) == AAR_E_ACCES);
    assert(api_file_set_protect("pm.txt", AAR_PROT_WRITE | AAR_PROT_DELETE) == AAR_E_ACCES);
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

static void collect_async_result(const aar_api *api, uint32_t ticket,
                                 int32_t *status, uint32_t *n)
{
    Uint32 start = SDL_GetTicks();
    int rc;

    do {
        rc = api->file_collect(ticket, status, n);
        if (rc != AAR_E_BUSY)
            break;
        SDL_Delay(1);
    } while (SDL_GetTicks() - start < 2000u);
    assert(rc == AAR_OK);
}

static void test_async_files(void)
{
    aar_api api = {0};
    char buf[8];
    uint32_t ticket = 0, old, rejected = UINT32_MAX, n = 99;
    int32_t status = 99;
    int64_t pos = -1;
    int fd, other;

    assert(host_files_init(UNIT_ROOT, 0) == 0);
    host_files_bind(&api);
    assert(api.file_read_async && api.file_write_async && api.file_collect);
    put_file("async.bin", "abcdef");
    fd = api.file_open("async.bin", AAR_FILE_READ | AAR_FILE_WRITE);
    other = api.file_open("async.bin", AAR_FILE_READ);
    assert(fd >= HOST_FD_BASE && other >= HOST_FD_BASE && other != fd);
    assert(api.file_read_async(fd, NULL, 1, &rejected) == AAR_E_INVAL);
    assert(api.file_read_async(fd, buf, 1, NULL) == AAR_E_INVAL);
    assert(api.file_read_async(-1, NULL, 0, &rejected) == AAR_E_BADF);
    assert(api.file_write_async(other, NULL, 0, &rejected) == AAR_E_BADF);
    assert(rejected == UINT32_MAX);

    assert(api.file_seek(fd, 2, AAR_SEEK_SET, NULL) == AAR_OK);
    memset(buf, 'x', sizeof buf);
    assert(api.file_read_async(fd, buf, sizeof buf, &ticket) == AAR_OK);
    assert(ticket != 0);
    assert(api.file_write_async(other, NULL, 0, &rejected) == AAR_E_BADF);
    assert(api.file_read_async(other, NULL, 0, &rejected) == AAR_E_BUSY);
    assert(rejected == UINT32_MAX);
    assert(api.file_read(fd, buf, 1, &n) == AAR_E_BUSY);
    assert(api.file_write(fd, "X", 1, &n) == AAR_E_BUSY);
    assert(api.file_seek(fd, 0, AAR_SEEK_SET, &pos) == AAR_E_BUSY);
    assert(api.file_truncate(fd, 0) == AAR_E_BUSY);
    assert(api.file_close(fd) == AAR_E_BUSY);
    assert(api.file_close(other) == AAR_OK);
    assert(api.file_collect(0, &status, &n) == AAR_E_INVAL);
    assert(api.file_collect(ticket, NULL, &n) == AAR_E_INVAL);
    assert(api.file_collect(ticket, &status, NULL) == AAR_E_INVAL);
    assert(api.file_collect(ticket + 1u, &status, &n) == AAR_E_STALE);
    assert(pos == -1 && n == 99 && status == 99);
    assert(memcmp(buf, "xxxxxxxx", sizeof buf) == 0);
    collect_async_result(&api, ticket, &status, &n);
    assert(status == AAR_OK && n == 4 && memcmp(buf, "cdefxxxx", sizeof buf) == 0);
    old = ticket;
    assert(api.file_collect(old, &status, &n) == AAR_E_STALE);
    assert(status == AAR_OK && n == 4);

    assert(api.file_read_async(fd, NULL, 0, &ticket) == AAR_OK);
    assert(ticket && ticket != old);
    assert(api.file_close(fd) == AAR_E_BUSY);
    assert(api.file_collect(ticket, &status, &n) == AAR_OK);
    assert(status == AAR_OK && n == 0);
    assert(api.file_seek(fd, 0, AAR_SEEK_CUR, &pos) == AAR_OK && pos == 6);

    old = ticket;
    assert(api.file_seek(fd, 2, AAR_SEEK_SET, NULL) == AAR_OK);
    assert(api.file_write_async(fd, "XYZ", 3, &ticket) == AAR_OK);
    assert(ticket && ticket != old);
    collect_async_result(&api, ticket, &status, &n);
    assert(status == AAR_OK && n == 3);
    assert(api.file_seek(fd, 0, AAR_SEEK_CUR, &pos) == AAR_OK && pos == 5);
    assert(api.file_seek(fd, 0, AAR_SEEK_SET, NULL) == AAR_OK);
    assert(api.file_read(fd, buf, sizeof buf, &n) == AAR_OK);
    assert(n == 6 && memcmp(buf, "abXYZf", 6) == 0);
    assert(api.file_close(fd) == AAR_OK);

    /* Teardown drains an accepted but uncollected write before closing
     * the stream, which also flushes the write. */
    fd = api.file_open("async.bin", AAR_FILE_WRITE);
    assert(fd >= HOST_FD_BASE);
    assert(api.file_write_async(fd, "Q", 1, &ticket) == AAR_OK);
    host_files_shutdown();
    assert(read_file("async.bin", buf, sizeof buf) == 6);
    assert(memcmp(buf, "QbXYZf", 6) == 0);
    rm_file("async.bin");
}

static void test_async_stop_while_native_read_is_pending(void)
{
    aar_api api = {0};
    char buf[4] = "app";
    uint32_t ticket = 0, rejected = UINT32_MAX, n = 99;
    int32_t status = 99;
    int ends[2];
    FILE *writer;
    Uint32 start;
    int rc;

    assert(host_files_init(UNIT_ROOT, 0) == 0);
    host_files_bind(&api);
    /* An empty pipe with a live writer holds the real fread until the test
     * releases it, regardless of when the SDL worker gets scheduled. */
#ifdef _WIN32
    assert(_pipe(ends, 4096, HOST_O_BINARY) == 0);
#else
    assert(pipe(ends) == 0);
#endif
    fds[0].fp = host_fdopen(ends[0], "rb");
    writer = host_fdopen(ends[1], "wb");
    assert(fds[0].fp && writer);
    fds[0].used = 1;
    fds[0].access = AAR_FILE_READ;

    assert(api.file_read_async(HOST_FD_BASE, buf, sizeof buf, &ticket) == AAR_OK);
    assert(api.file_collect(ticket, &status, &n) == AAR_E_BUSY);
    assert(status == 99 && n == 99 && memcmp(buf, "app", sizeof buf) == 0);
    unit_stop = 1;
    host_files_stop();
    assert(api.file_collect(ticket, &status, &n) == AAR_OK);
    assert(status == AAR_E_IO && n == 0 && memcmp(buf, "app", sizeof buf) == 0);
    assert(api.file_collect(ticket, &status, &n) == AAR_E_STALE);
    assert(api.file_read_async(HOST_FD_BASE, NULL, 0, &rejected) == AAR_E_IO);
    assert(rejected == UINT32_MAX);
    assert(api.file_close(HOST_FD_BASE) == AAR_E_BUSY);
    assert(api.file_read(HOST_FD_BASE, buf, 1, &n) == AAR_E_BUSY && n == 0);

    /* Reuse the app buffer while the worker is still in flight. */
    memcpy(buf, "new", sizeof buf);
    assert(fwrite("data", 1, sizeof buf, writer) == sizeof buf);
    assert(fclose(writer) == 0);
    start = SDL_GetTicks();
    do {
        rc = api.file_close(HOST_FD_BASE);
        if (rc != AAR_E_BUSY)
            break;
        SDL_Delay(1);
    } while (SDL_GetTicks() - start < 2000u);
    assert(rc == AAR_OK && memcmp(buf, "new", sizeof buf) == 0);
    assert(api.file_read(HOST_FD_BASE, buf, 1, &n) == AAR_E_BADF);
    unit_stop = 0;
    host_files_shutdown();
}

int main(void)
{
    make_dir(UNIT_ROOT);
    test_create_keeps_file_when_slots_full();
    test_create_truncate_excl();
    test_lists_have_their_own_table();
#ifndef _WIN32
    test_list_survives_a_rename();
#endif
    test_seek_out_pos_is_optional();
    test_close_reports_flush_failure();
    test_access_mode_is_kept_per_id();
    test_map_file_is_a_runtime_block();
    test_delete_and_dir_create();
    test_rename();
    test_truncate();
    test_protect_and_date();
    test_volume_info();
    test_async_files();
    test_async_stop_while_native_read_is_pending();
    return 0;
}
