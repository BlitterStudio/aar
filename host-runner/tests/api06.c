/*
 * The API 0.6 calls of the host runner, through the table: files, time,
 * locale, environment, clipboard, display info and modes, title, pointer,
 * resize, vertical blank estimate, the new flags, and the events (SDL events
 * that the app pushes itself, and the --input script). check_runner.py runs
 * each mode with the options and the environment it needs, and compares what
 * the app prints. The exit status is the failing check, 77 when the host has
 * no game controller support for the virtual joystick of "pads".
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(code, msg) \
    do { \
        fprintf(stderr, "api06: %s (line %d)\n", (msg), __LINE__); \
        return (code); \
    } while (0)
#define CHECK(cond, code) \
    do { \
        if (!(cond)) \
            FAIL((code), #cond); \
    } while (0)
#define CHECK_RC(call, want, code) \
    do { \
        int rc_ = (call); \
        if (rc_ != (want)) { \
            fprintf(stderr, "api06: %s returned %d, want %d (line %d)\n", \
                    #call, rc_, (int)(want), __LINE__); \
            return (code); \
        } \
    } while (0)

#define ALL_FLAGS (AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE | \
                   AAR_DISPLAY_POINTER_ABS | AAR_DISPLAY_TEXT_INPUT | \
                   AAR_DISPLAY_CLOSE_REQUEST | AAR_DISPLAY_SCALE)
#define ALL_CAPS (AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE | \
                  AAR_DISPCAP_POINTER_ABS | AAR_DISPCAP_TEXT_INPUT | \
                  AAR_DISPCAP_CLOSE_REQUEST | AAR_DISPCAP_SCALED)

static int write_text(const char *path, const char *text)
{
    int fd = aar_file_open(path, AAR_FILE_WRITE | AAR_FILE_CREATE |
                                     AAR_FILE_TRUNCATE);
    uint32_t n = 0;
    int rc;

    if (fd < 0)
        return fd;
    rc = aar_file_write(fd, text, (uint32_t)strlen(text), &n);
    aar_file_close(fd);
    return rc;
}

static int size_of(const char *path)
{
    aar_stat st;

    if (aar_file_stat(path, &st) != AAR_OK)
        return -1;
    return (int)st.size;
}

/* ---- files ---------------------------------------------------------------- */

static int test_files(void)
{
    aar_stat st;
    aar_volume_info vi;
    uint32_t n = 0;
    char buf[16];
    int fd;
    int list;
#ifndef _WIN32
    aar_dirent ent;
    int saw_dir, saw_file;
#endif

    CHECK_RC(aar_dir_create("sub"), AAR_OK, 10);
    CHECK_RC(aar_dir_create("sub"), AAR_E_EXIST, 11);
    CHECK_RC(aar_dir_create("nope/sub"), AAR_E_NOENT, 12);
    CHECK_RC(aar_file_stat("sub", &st), AAR_OK, 13);
    CHECK(st.mode == AAR_MODE_DIR, 14);

    /* truncate */
    CHECK_RC(write_text("sub/a.txt", "hello"), AAR_OK, 20);
    fd = aar_file_open("sub/a.txt", AAR_FILE_READ | AAR_FILE_WRITE);
    CHECK(fd >= AAR_FD_BASE, 21);
    CHECK_RC(aar_file_truncate(fd, 2u), AAR_OK, 22);
    CHECK_RC(aar_file_truncate(fd, 6u), AAR_OK, 23);
    CHECK_RC(aar_file_close(fd), AAR_OK, 24);
    CHECK(size_of("sub/a.txt") == 6, 25);
    fd = aar_file_open("sub/a.txt", AAR_FILE_READ);
    CHECK_RC(aar_file_read(fd, buf, sizeof buf, &n), AAR_OK, 26);
    CHECK(n == 6u && memcmp(buf, "he\0\0\0\0", 6) == 0, 27);
    CHECK_RC(aar_file_truncate(fd, 1u), AAR_E_BADF, 28); /* opened for reading */
    aar_file_close(fd);
    CHECK_RC(aar_file_truncate(77, 1u), AAR_E_BADF, 29);
    CHECK_RC(aar_file_truncate(-5, 1u), AAR_E_BADF, 30);
    /* A read that reached the end set the stream's end-of-file flag: after
     * an extension the next read, without a seek, returns the new bytes. */
    CHECK_RC(write_text("eof.txt", "abc"), AAR_OK, 33);
    fd = aar_file_open("eof.txt", AAR_FILE_READ | AAR_FILE_WRITE);
    CHECK(fd >= AAR_FD_BASE, 33);
    CHECK_RC(aar_file_read(fd, buf, 8u, &n), AAR_OK, 33);
    CHECK(n == 3u, 33);
    CHECK_RC(aar_file_read(fd, buf, 8u, &n), AAR_OK, 33);
    CHECK(n == 0u, 33); /* the end of the file */
    CHECK_RC(aar_file_truncate(fd, 6u), AAR_OK, 33);
    memset(buf, 'x', sizeof buf);
    CHECK_RC(aar_file_read(fd, buf, 8u, &n), AAR_OK, 33);
    CHECK(n == 3u && buf[0] == 0 && buf[2] == 0 && buf[3] == 'x', 33);
    CHECK_RC(aar_file_read(fd, buf, 8u, &n), AAR_OK, 33);
    CHECK(n == 0u, 33); /* the new end */
    CHECK_RC(aar_file_close(fd), AAR_OK, 33);
    CHECK(size_of("eof.txt") == 6, 33);

    /* rename */
    CHECK_RC(aar_file_rename("sub/a.txt", "sub/b.txt", 0), AAR_OK, 40);
    CHECK_RC(aar_file_stat("sub/a.txt", &st), AAR_E_NOENT, 41);
    CHECK_RC(aar_file_rename("sub/b.txt", "ok.txt", 0), AAR_E_EXIST, 42);
    CHECK(size_of("ok.txt") == 3 && size_of("sub/b.txt") == 6, 43);
    CHECK_RC(aar_file_rename("sub/b.txt", "ok.txt", AAR_RENAME_REPLACE), AAR_OK, 44);
    CHECK(size_of("ok.txt") == 6 && size_of("sub/b.txt") == -1, 45);
    CHECK_RC(aar_file_rename("ok.txt", "ok2.txt", 2u), AAR_E_INVAL, 46);
    CHECK_RC(aar_file_rename("absent.txt", "x.txt", 0), AAR_E_NOENT, 47);
    /* The same object, spelled alike or not: nothing happens, with or without the flag. */
    CHECK_RC(aar_file_rename("ok.txt", "ok.txt", 0), AAR_OK, 48);
    CHECK_RC(aar_file_rename("ok.txt", "./ok.txt", AAR_RENAME_REPLACE), AAR_OK, 48);
    CHECK_RC(aar_file_rename("ok.txt", "sub//ok.txt", 0), AAR_OK, 48);
    CHECK_RC(aar_file_rename("sub", "sub", AAR_RENAME_REPLACE), AAR_OK, 48);
    CHECK(size_of("ok.txt") == 6 && size_of("sub") >= 0, 48);
    /* The REPLACE target is deleted first: one that is in use is refused and
     * keeps its contents. The same-object no-op comes before that check. */
    CHECK_RC(write_text("rt.txt", "old"), AAR_OK, 49);
    CHECK_RC(write_text("rs.txt", "new"), AAR_OK, 49);
    fd = aar_file_open("rt.txt", AAR_FILE_READ);
    CHECK(fd >= AAR_FD_BASE, 49);
    CHECK_RC(aar_file_rename("rs.txt", "rt.txt", AAR_RENAME_REPLACE), AAR_E_BUSY, 49);
    CHECK_RC(aar_file_rename("rs.txt", "rt.txt", 0), AAR_E_EXIST, 49);
    CHECK(size_of("rt.txt") == 3 && size_of("rs.txt") == 3, 49);
    CHECK_RC(aar_file_read(fd, buf, sizeof buf, &n), AAR_OK, 49);
    CHECK(n == 3u && memcmp(buf, "old", 3) == 0, 49);
    CHECK_RC(aar_file_rename("rt.txt", "rt.txt", AAR_RENAME_REPLACE), AAR_OK, 49);
    CHECK_RC(aar_file_close(fd), AAR_OK, 49);
    CHECK_RC(aar_file_rename("rs.txt", "rt.txt", AAR_RENAME_REPLACE), AAR_OK, 49);
    CHECK(size_of("rt.txt") == 3 && size_of("rs.txt") == -1, 49);
    fd = aar_file_open("rt.txt", AAR_FILE_READ);
    CHECK_RC(aar_file_read(fd, buf, sizeof buf, &n), AAR_OK, 49);
    CHECK(n == 3u && memcmp(buf, "new", 3) == 0, 49);
    CHECK_RC(aar_file_close(fd), AAR_OK, 49);
    CHECK_RC(aar_file_delete("rt.txt"), AAR_OK, 49);
    /* An empty directory that a listing reads is in use too. */
    CHECK_RC(aar_dir_create("rl.dir"), AAR_OK, 49);
    CHECK_RC(aar_dir_create("rs.dir"), AAR_OK, 49);
    list = aar_file_list_open("rl.dir");
    CHECK(list >= AAR_FD_BASE, 49);
    CHECK_RC(aar_file_rename("rs.dir", "rl.dir", AAR_RENAME_REPLACE), AAR_E_BUSY, 49);
    CHECK_RC(aar_dir_create("rs.dir"), AAR_E_EXIST, 49); /* nothing moved */
    CHECK_RC(aar_file_rename("rl.dir", "rl.dir", AAR_RENAME_REPLACE), AAR_OK, 49);
    CHECK_RC(aar_file_list_close(list), AAR_OK, 49);
    CHECK_RC(aar_file_rename("rs.dir", "rl.dir", AAR_RENAME_REPLACE), AAR_OK, 49);
    CHECK_RC(aar_file_delete("rl.dir"), AAR_OK, 49);

    /* set_date: Python reads the modification time of dated.txt afterwards */
    CHECK_RC(write_text("dated.txt", "d"), AAR_OK, 50);
    CHECK_RC(aar_file_set_date("dated.txt", 1000000000), AAR_OK, 51);
    CHECK_RC(aar_file_stat("dated.txt", &st), AAR_OK, 52);
    CHECK(st.mtime == 1000000000, 53);
    CHECK_RC(aar_file_set_date("dated.txt", -1), AAR_E_INVAL, 54);
    CHECK_RC(aar_file_set_date("absent.txt", 1000000000), AAR_E_NOENT, 55);
    CHECK_RC(aar_file_set_date("sub", 1000000000), AAR_OK, 56);
    /* A filesystem keeps only the times it can: an extreme time is refused
     * with AAR_E_INVAL, or kept exactly. */
    {
        static const int64_t extremes[] = {253402300799ll, 1ll << 40, INT64_MAX};
        uint32_t i;

        for (i = 0; i < 3u; i++) {
            int rc = aar_file_set_date("dated.txt", extremes[i]);

            if (rc == AAR_OK) {
                CHECK_RC(aar_file_stat("dated.txt", &st), AAR_OK, 57);
                CHECK(st.mtime == extremes[i], 57);
            } else {
                CHECK(rc == AAR_E_INVAL, 57);
            }
        }
    }
    CHECK_RC(aar_file_set_date("dated.txt", 1000000000), AAR_OK, 57);

    /* set_protect: Python reads the mode bits of perm.txt afterwards */
    CHECK_RC(write_text("perm.txt", "p"), AAR_OK, 60);
    CHECK_RC(aar_file_stat("perm.txt", &st), AAR_OK, 61);
    CHECK((st.protect & (AAR_PROT_READ | AAR_PROT_WRITE | AAR_PROT_DELETE)) == 0u, 62);
    CHECK_RC(aar_file_set_protect("perm.txt", AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_EXECUTE), AAR_OK, 63);
    CHECK_RC(aar_file_stat("perm.txt", &st), AAR_OK, 64);
    CHECK(st.protect == (AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_EXECUTE), 65);
    CHECK_RC(aar_file_delete("perm.txt"), AAR_E_ACCES, 66);
    CHECK_RC(aar_file_set_protect("perm.txt", 0x100u), AAR_E_INVAL, 67);
    CHECK_RC(aar_file_set_protect("absent.txt", 0u), AAR_E_NOENT, 68);
    /* A mask of known bits that the host cannot store is AAR_E_NOSYS, and
     * the protection does not change. */
    CHECK_RC(aar_file_set_protect("perm.txt", AAR_PROT_SCRIPT), AAR_E_NOSYS, 68);
    CHECK_RC(aar_file_set_protect("perm.txt", AAR_PROT_ARCHIVE), AAR_E_NOSYS, 68);
    CHECK_RC(aar_file_set_protect("perm.txt", AAR_PROT_DELETE), AAR_E_NOSYS, 68);
    CHECK_RC(aar_file_set_protect("perm.txt", AAR_PROT_WRITE), AAR_E_NOSYS, 68);
    CHECK_RC(aar_file_stat("perm.txt", &st), AAR_OK, 68);
    CHECK(st.protect == (AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_EXECUTE), 68);
    CHECK_RC(aar_file_set_protect("perm.txt", AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_EXECUTE), AAR_OK, 69);

    /* volume_info */
    memset(&vi, 0xaa, sizeof vi);
    vi.size = (uint32_t)sizeof vi;
    CHECK_RC(aar_get_volume_info(".", &vi), AAR_OK, 70);
    printf("volume block=%u total=%llu free=%llu\n", (unsigned)vi.block_bytes,
           (unsigned long long)vi.total_bytes, (unsigned long long)vi.free_bytes);
    CHECK(vi.block_bytes > 0u && vi.total_bytes >= vi.free_bytes && vi.total_bytes > 0u, 71);
    CHECK(vi.flags == 0u && vi.reserved[0] == 0u, 72);
    CHECK_RC(aar_get_volume_info("sub", &vi), AAR_OK, 73);
    CHECK_RC(aar_get_volume_info("absent", &vi), AAR_E_NOENT, 74);
    CHECK_RC(aar_get_volume_info(".", 0), AAR_E_INVAL, 75);

    /* delete */
    CHECK_RC(write_text("sub/c.txt", "c"), AAR_OK, 80);
    CHECK_RC(aar_file_delete("sub"), AAR_E_EXIST, 81); /* not empty */
    fd = aar_file_open("sub/c.txt", AAR_FILE_READ);
    CHECK_RC(aar_file_delete("sub/c.txt"), AAR_E_BUSY, 82); /* open */
    aar_file_close(fd);
    CHECK_RC(aar_file_delete("sub/c.txt"), AAR_OK, 83);
    CHECK_RC(aar_file_delete("sub/c.txt"), AAR_E_NOENT, 84);
    CHECK_RC(aar_file_delete("sub"), AAR_OK, 85);
    CHECK_RC(aar_file_delete("ok2.txt"), AAR_E_NOENT, 86);
    /* An empty directory that a listing reads is in use. */
    CHECK_RC(aar_dir_create("ldir"), AAR_OK, 87);
    list = aar_file_list_open("ldir");
    CHECK(list >= AAR_FD_BASE, 87);
    CHECK_RC(aar_file_delete("ldir"), AAR_E_BUSY, 87);
    CHECK_RC(aar_file_list_close(list), AAR_OK, 87);
#ifndef _WIN32
    /* A listing classifies its entries through the open directory: it keeps
     * doing so after the directory was renamed. */
    CHECK_RC(aar_dir_create("top"), AAR_OK, 88);
    CHECK_RC(aar_dir_create("top/inner"), AAR_OK, 88);
    CHECK_RC(write_text("top/plain.txt", "p"), AAR_OK, 88);
    list = aar_file_list_open("top");
    CHECK(list >= AAR_FD_BASE, 88);
    CHECK_RC(aar_file_rename("top", "top2", 0), AAR_OK, 88);
    saw_dir = saw_file = 0;
    for (;;) {
        int rc = aar_file_list_next(list, &ent);

        if (rc == AAR_E_NOENT)
            break;
        CHECK_RC(rc, AAR_OK, 88);
        if (strcmp(ent.name, "inner") == 0 && ent.mode == AAR_MODE_DIR)
            saw_dir++;
        if (strcmp(ent.name, "plain.txt") == 0 && ent.mode == AAR_MODE_FILE)
            saw_file++;
    }
    CHECK(saw_dir == 1 && saw_file == 1, 88);
    CHECK_RC(aar_file_list_close(list), AAR_OK, 88);
    CHECK_RC(aar_file_delete("top2/inner"), AAR_OK, 88);
    CHECK_RC(aar_file_delete("top2/plain.txt"), AAR_OK, 88);
    CHECK_RC(aar_file_delete("top2"), AAR_OK, 88);
#endif
    CHECK_RC(aar_file_delete("ldir"), AAR_OK, 87);

    /* the path rules and the policy of file_open, for every call */
    /* aar06-outside.txt is a file next to the root that Python made. */
    CHECK_RC(aar_file_delete("../aar06-outside.txt"), AAR_E_ACCES, 90);
    CHECK_RC(aar_file_rename("../aar06-outside.txt", "y", 0), AAR_E_ACCES, 91);
    CHECK_RC(aar_file_rename("ok.txt", "../aar06-outside.txt", AAR_RENAME_REPLACE), AAR_E_ACCES, 92);
    CHECK_RC(aar_dir_create("../aar06-outside-dir"), AAR_E_ACCES, 93);
    CHECK_RC(aar_file_set_date("../aar06-outside.txt", 1000000000), AAR_E_ACCES, 94);
    CHECK_RC(aar_file_set_protect("../aar06-outside.txt", AAR_PROT_WRITE), AAR_E_ACCES, 95);
    CHECK_RC(aar_get_volume_info("../aar06-outside.txt", &vi), AAR_E_ACCES, 96);
    CHECK_RC(aar_file_delete("vol:x"), AAR_E_ACCES, 97);
    CHECK_RC(aar_file_delete(""), AAR_E_INVAL, 98);
    CHECK_RC(aar_file_delete(0), AAR_E_INVAL, 99);
    CHECK_RC(aar_dir_create(""), AAR_E_INVAL, 100);
    CHECK_RC(aar_file_rename("", "y", 0), AAR_E_INVAL, 101);
    CHECK_RC(aar_file_set_date("", 0), AAR_E_INVAL, 102);
    CHECK_RC(aar_file_set_protect("", 0u), AAR_E_INVAL, 103);
    return 0;
}

/* ---- time, locale ----------------------------------------------------------- */

static int test_time(void)
{
    int64_t a = 0;
    int64_t b = 0;

    CHECK_RC(aar_time_unix_us(0), AAR_E_INVAL, 110);
    CHECK_RC(aar_time_unix_us(&a), AAR_OK, 111);
    CHECK_RC(aar_time_unix_us(&b), AAR_OK, 112);
    CHECK(b >= a && b - a < 1000000, 113);
    printf("unix_us=%lld\n", (long long)a);
    return 0;
}

static int test_locale(void)
{
    aar_locale_info li;
    unsigned char big[sizeof(aar_locale_info) + 8];
    unsigned char part[sizeof(aar_locale_info)];
    uint32_t i;

    CHECK_RC(aar_get_locale_info(0), AAR_E_INVAL, 120);
    memset(&li, 0xaa, sizeof li);
    li.size = 2u;
    CHECK_RC(aar_get_locale_info(&li), AAR_E_INVAL, 121);
    memset(&li, 0xaa, sizeof li);
    li.size = (uint32_t)sizeof li;
    CHECK_RC(aar_get_locale_info(&li), AAR_OK, 122);
    CHECK(li.size == sizeof li && li.reserved[0] == 0u && li.reserved[2] == 0u, 123);
    CHECK(memchr(li.language, 0, sizeof li.language) != 0, 124);
    CHECK(memchr(li.country, 0, sizeof li.country) != 0, 125);
    CHECK(li.decimal_point[0] != 0 && memchr(li.decimal_point, 0, 4) != 0, 126);
    CHECK(memchr(li.group_separator, 0, 4) != 0, 127);
    CHECK((li.flags & AAR_LOCALEF_OFFSET_KNOWN) != 0u, 128);
    printf("utc=%d flags=%u lang=%s country=%s dec=%s grp=%s\n",
           (int)li.utc_offset_min, (unsigned)li.flags, li.language, li.country,
           li.decimal_point, li.group_separator);
    /* The caller's size bounds the write; past what the runtime knows is zero. */
    memset(part, 0xaa, sizeof part);
    ((aar_locale_info *)(void *)part)->size = 12u;
    CHECK_RC(aar_get_locale_info((aar_locale_info *)(void *)part), AAR_OK, 129);
    for (i = 12u; i < sizeof part; i++)
        CHECK(part[i] == 0xaau, 130);
    memset(big, 0xaa, sizeof big);
    ((aar_locale_info *)(void *)big)->size = (uint32_t)sizeof big;
    CHECK_RC(aar_get_locale_info((aar_locale_info *)(void *)big), AAR_OK, 131);
    for (i = sizeof li; i < sizeof big; i++)
        CHECK(big[i] == 0u, 132);
    return 0;
}

/* ---- environment ---------------------------------------------------------- */

static int test_env_dir(void)
{
    char buf[64];
    char longname[300];
    uint32_t n = 99;

    memset(buf, 'x', sizeof buf);
    CHECK_RC(aar_env_get("greeting", buf, sizeof buf, &n), AAR_OK, 140);
    CHECK(n == 11u && strcmp(buf, "hello world") == 0, 141);
    memset(buf, 'x', sizeof buf);
    CHECK_RC(aar_env_get("greeting", buf, 11u, &n), AAR_E_NOSPC, 142);
    CHECK(n == 11u && memcmp(buf, "hello world", 11) == 0 && buf[11] == 'x', 143);
    CHECK_RC(aar_env_get("greeting", buf, 12u, &n), AAR_OK, 144);
    CHECK(buf[11] == 0, 145);
    n = 0;
    CHECK_RC(aar_env_get("greeting", 0, 0u, &n), AAR_E_NOSPC, 146);
    CHECK(n == 11u, 147);
    CHECK_RC(aar_env_get("empty", buf, sizeof buf, &n), AAR_OK, 148);
    CHECK(n == 0u && buf[0] == 0, 149);
    CHECK_RC(aar_env_get("empty", 0, 0u, &n), AAR_E_NOSPC, 150);
    CHECK_RC(aar_env_get("sub/inner", buf, sizeof buf, &n), AAR_OK, 151);
    CHECK(n == 2u && strcmp(buf, "in") == 0, 152);
    CHECK_RC(aar_env_get("bin", buf, sizeof buf, &n), AAR_OK, 153);
    CHECK(n == 3u && buf[0] == 0 && buf[1] == 1 && buf[2] == 2 && buf[3] == 0, 154);
    CHECK_RC(aar_env_get("missing", buf, sizeof buf, &n), AAR_E_NOENT, 155);
    CHECK_RC(aar_env_get("sub", buf, sizeof buf, &n), AAR_E_NOENT, 156); /* a directory */
    CHECK_RC(aar_env_get("../x", buf, sizeof buf, &n), AAR_E_ACCES, 157);
    CHECK_RC(aar_env_get("sub/../greeting", buf, sizeof buf, &n), AAR_E_ACCES, 158);
    CHECK_RC(aar_env_get("/greeting", buf, sizeof buf, &n), AAR_E_ACCES, 159);
    CHECK_RC(aar_env_get("sub//inner", buf, sizeof buf, &n), AAR_E_ACCES, 160);
    CHECK_RC(aar_env_get("a:b", buf, sizeof buf, &n), AAR_E_ACCES, 161);
    CHECK_RC(aar_env_get("a\\b", buf, sizeof buf, &n), AAR_E_ACCES, 162);
    CHECK_RC(aar_env_get("", buf, sizeof buf, &n), AAR_E_INVAL, 163);
    CHECK_RC(aar_env_get(0, buf, sizeof buf, &n), AAR_E_INVAL, 164);
    memset(longname, 'a', sizeof longname);
    longname[256] = 0;
    CHECK_RC(aar_env_get(longname, buf, sizeof buf, &n), AAR_E_INVAL, 165);
    longname[255] = 0;
    CHECK_RC(aar_env_get(longname, buf, sizeof buf, &n), AAR_E_NOENT, 166);
    CHECK_RC(aar_env_get("greeting", buf, sizeof buf, 0), AAR_E_INVAL, 167);
    CHECK_RC(aar_env_get("greeting", 0, 8u, &n), AAR_E_INVAL, 168);
    /* With --env the process environment is not served. */
    CHECK_RC(aar_env_get("AARTEST_ENV", buf, sizeof buf, &n), AAR_E_NOENT, 169);
    return 0;
}

static int test_env_proc(void)
{
    char buf[64];
    uint32_t n = 99;

    CHECK_RC(aar_env_get("AARTEST_ENV", buf, sizeof buf, &n), AAR_OK, 170);
    CHECK(n == 12u && strcmp(buf, "from-process") == 0, 171);
    CHECK_RC(aar_env_get("AARTEST_ENV", buf, 12u, &n), AAR_E_NOSPC, 172);
    CHECK(n == 12u, 173);
    CHECK_RC(aar_env_get("AARTEST_ENV", 0, 0u, &n), AAR_E_NOSPC, 174);
    CHECK_RC(aar_env_get("AARTEST_NOT_SET", buf, sizeof buf, &n), AAR_E_NOENT, 175);
    CHECK_RC(aar_env_get("", buf, sizeof buf, &n), AAR_E_INVAL, 176);
    CHECK_RC(aar_env_get("AARTEST_ENV", buf, sizeof buf, 0), AAR_E_INVAL, 177);
    return 0;
}

/* ---- clipboard ------------------------------------------------------------ */

static int test_clip(void)
{
    static const char text[] = "h\xc3\xa9llo w\xc3\xb6rld";
    uint32_t len = (uint32_t)(sizeof text - 1u);
    char buf[64];
    uint32_t n = 0;

    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, buf, sizeof buf, &n), AAR_E_NOENT, 180); /* empty */
    CHECK_RC(aar_clip_write(0u, AAR_CLIP_TEXT, text, len), AAR_OK, 181);
    memset(buf, 'x', sizeof buf);
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, buf, sizeof buf, &n), AAR_OK, 182);
    CHECK(n == len && memcmp(buf, text, len) == 0 && buf[len] == 'x', 183);
    memset(buf, 'x', sizeof buf);
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, buf, 4u, &n), AAR_E_NOSPC, 184);
    CHECK(n == len && memcmp(buf, text, 4) == 0 && buf[4] == 'x', 185);
    n = 0;
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, 0, 0u, &n), AAR_E_NOSPC, 186);
    CHECK(n == len, 187);
    /* Other units and IFF: nothing to read, and no way to write. */
    CHECK_RC(aar_clip_read(1u, AAR_CLIP_TEXT, buf, sizeof buf, &n), AAR_E_NOENT, 188);
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_IFF, buf, sizeof buf, &n), AAR_E_NOENT, 189);
    CHECK_RC(aar_clip_write(1u, AAR_CLIP_TEXT, text, len), AAR_E_NOSYS, 190);
    CHECK_RC(aar_clip_write(0u, AAR_CLIP_IFF, text, len), AAR_E_NOSYS, 191);
    /* Bad arguments. */
    CHECK_RC(aar_clip_read(AAR_CLIP_UNITS, AAR_CLIP_TEXT, buf, sizeof buf, &n), AAR_E_INVAL, 192);
    CHECK_RC(aar_clip_write(AAR_CLIP_UNITS, AAR_CLIP_TEXT, text, len), AAR_E_INVAL, 193);
    CHECK_RC(aar_clip_read(0u, 3u, buf, sizeof buf, &n), AAR_E_INVAL, 194);
    CHECK_RC(aar_clip_read(0u, 0u, buf, sizeof buf, &n), AAR_E_INVAL, 195);
    CHECK_RC(aar_clip_write(0u, 3u, text, len), AAR_E_INVAL, 196);
    CHECK_RC(aar_clip_write(0u, AAR_CLIP_TEXT, 0, 4u), AAR_E_INVAL, 197);
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, buf, sizeof buf, 0), AAR_E_INVAL, 198);
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, 0, 4u, &n), AAR_E_INVAL, 199);
    /* A failed call left the clip alone. */
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, buf, sizeof buf, &n), AAR_OK, 200);
    CHECK(n == len, 201);
    /* The text ends at a NUL byte. */
    CHECK_RC(aar_clip_write(0u, AAR_CLIP_TEXT, "ab\0cd", 5u), AAR_OK, 202);
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, buf, sizeof buf, &n), AAR_OK, 203);
    CHECK(n == 2u && memcmp(buf, "ab", 2) == 0, 204);
    /* Writing nothing empties the clip. */
    CHECK_RC(aar_clip_write(0u, AAR_CLIP_TEXT, 0, 0u), AAR_OK, 205);
    CHECK_RC(aar_clip_read(0u, AAR_CLIP_TEXT, buf, sizeof buf, &n), AAR_E_NOENT, 206);
    return 0;
}

/* ---- display ---------------------------------------------------------------- */

static SDL_Window *runner_window(void)
{
    uint32_t id;

    for (id = 1u; id < 64u; id++) {
        SDL_Window *w = SDL_GetWindowFromID(id);

        if (w)
            return w;
    }
    return 0;
}

static int test_display_closed(void)
{
    aar_display_info di;
    aar_display_mode modes[64];
    aar_display_mode one;
    aar_surface s;
    uint32_t n = 0;
    uint32_t i;
    uint32_t preferred = 0;

    memset(&di, 0, sizeof di);
    di.size = (uint32_t)sizeof di;
    CHECK_RC(aar_display_get_info(1u, &di), AAR_E_INVAL, 210);
    CHECK_RC(aar_display_set_title(1u, "x"), AAR_E_INVAL, 211);
    CHECK_RC(aar_display_pointer(1u, AAR_POINTER_HIDDEN, 0), AAR_E_INVAL, 212);
    CHECK_RC(aar_display_wait_vblank(1000u), AAR_E_INVAL, 213);
    CHECK_RC(aar_display_resize(1u, 8u, 8u, &s), AAR_E_INVAL, 214);
    CHECK(aar_display_last_present_us() == 0u, 215);

    /* The modes need no open display. */
    CHECK_RC(aar_display_modes(modes, 64u, &n), AAR_OK, 216);
    CHECK(n >= 1u && n <= 64u, 217);
    for (i = 0; i < n; i++) {
        CHECK(modes[i].width > 0u && modes[i].height > 0u, 218);
        CHECK(modes[i].formats == ((1u << AAR_FMT_RGB565) | (1u << AAR_FMT_BGRA8888) |
                                   (1u << AAR_FMT_CLUT8)), 219);
        CHECK(modes[i].refresh_mhz % 1000u == 0u, 220);
        CHECK((modes[i].flags & ~(uint32_t)AAR_MODEF_PREFERRED) == 0u, 221);
        CHECK(modes[i].reserved == 0u, 222);
        if (modes[i].flags & AAR_MODEF_PREFERRED)
            preferred++;
    }
    CHECK(preferred == 1u, 223);
    CHECK_RC(aar_display_modes(0, 0u, &n), n > 0u ? AAR_E_NOSPC : AAR_OK, 224);
    CHECK(n >= 1u, 225);
    {
        uint32_t all = n;

        memset(&one, 0, sizeof one);
        CHECK_RC(aar_display_modes(&one, 1u, &n), all > 1u ? AAR_E_NOSPC : AAR_OK, 226);
        CHECK(n == all && one.width > 0u, 227);
    }
    CHECK_RC(aar_display_modes(0, 4u, &n), AAR_E_INVAL, 228);
    CHECK_RC(aar_display_modes(modes, 4u, 0), AAR_E_INVAL, 229);
    return 0;
}

static int test_display_caps(void)
{
    static const struct {
        uint32_t flags;
        uint32_t caps;
    } cases[] = {
        {0u, 0u},
        {AAR_DISPLAY_WINDOW, AAR_DISPCAP_WINDOW},
        {AAR_DISPLAY_RESIZABLE, 0u}, /* needs WINDOW */
        {AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE,
         AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE},
        {AAR_DISPLAY_POINTER_ABS, AAR_DISPCAP_POINTER_ABS},
        {AAR_DISPLAY_TEXT_INPUT, AAR_DISPCAP_TEXT_INPUT},
        {AAR_DISPLAY_CLOSE_REQUEST, AAR_DISPCAP_CLOSE_REQUEST},
        {AAR_DISPLAY_SCALE, AAR_DISPCAP_SCALED},
        {ALL_FLAGS, ALL_CAPS},
    };
    aar_surface s;
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t flags = cases[i].flags | AAR_DISPLAY_VSYNC;

        CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, flags, &s), AAR_OK, 230);
        CHECK((s.caps & ALL_CAPS) == cases[i].caps, 231);
        CHECK((s.caps & AAR_DISPCAP_ASYNC) != 0u, 232);
        CHECK_RC(aar_display_close(s.id), AAR_OK, 233);
    }
    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, 1u << 8, &s), AAR_E_INVAL, 234);
    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, 0x80000000u, &s), AAR_E_INVAL, 235);
    /* The latest flag still works with the new ones. */
    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565,
                              AAR_DISPLAY_LATEST | ALL_FLAGS, &s), AAR_OK, 236);
    CHECK((s.caps & AAR_DISPCAP_LATEST) != 0u && (s.caps & ALL_CAPS) == ALL_CAPS, 237);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 238);
    return 0;
}

static int test_display_info(void)
{
    aar_display_info di;
    unsigned char big[sizeof(aar_display_info) + 8];
    unsigned char part[sizeof(aar_display_info)];
    aar_surface s;
    uint32_t i;

    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, ALL_FLAGS, &s), AAR_OK, 240);
    memset(&di, 0xaa, sizeof di);
    di.size = (uint32_t)sizeof di;
    CHECK_RC(aar_display_get_info(s.id, &di), AAR_OK, 241);
    CHECK(di.size == sizeof di && di.width == 64u && di.height == 48u, 242);
    CHECK(di.refresh_mhz % 1000u == 0u, 243);
    CHECK(di.screen_width > 0u && di.screen_height > 0u, 244);
    CHECK((di.flags & AAR_DISPINFO_WINDOW) != 0u, 245);
    CHECK((di.flags & AAR_DISPINFO_SCALED) != 0u, 246); /* AAR_DISPLAY_SCALE was asked for */
    CHECK((di.flags & ~7u) == 0u, 247);
    for (i = 0; i < 5u; i++)
        CHECK(di.reserved[i] == 0u, 248);
    printf("refresh_mhz=%u screen=%ux%u\n", (unsigned)di.refresh_mhz,
           (unsigned)di.screen_width, (unsigned)di.screen_height);
    CHECK_RC(aar_display_get_info(s.id + 1u, &di), AAR_E_INVAL, 249);
    CHECK_RC(aar_display_get_info(s.id, 0), AAR_E_INVAL, 250);
    di.size = 2u;
    CHECK_RC(aar_display_get_info(s.id, &di), AAR_E_INVAL, 251);
    memset(part, 0xaa, sizeof part);
    ((aar_display_info *)(void *)part)->size = 12u;
    CHECK_RC(aar_display_get_info(s.id, (aar_display_info *)(void *)part), AAR_OK, 252);
    for (i = 12u; i < sizeof part; i++)
        CHECK(part[i] == 0xaau, 253);
    memset(big, 0xaa, sizeof big);
    ((aar_display_info *)(void *)big)->size = (uint32_t)sizeof big;
    CHECK_RC(aar_display_get_info(s.id, (aar_display_info *)(void *)big), AAR_OK, 254);
    for (i = sizeof di; i < sizeof big; i++)
        CHECK(big[i] == 0u, 255);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 256);
    /* Without the scale flag the window is not scaled (--scale 1). */
    CHECK_RC(aar_display_open(32u, 24u, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW, &s), AAR_OK, 257);
    di.size = (uint32_t)sizeof di;
    CHECK_RC(aar_display_get_info(s.id, &di), AAR_OK, 258);
    CHECK(di.width == 32u && di.height == 24u, 259);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 260);
    return 0;
}

static int test_title_pointer(void)
{
    aar_surface s;
    aar_pointer ptr;
    uint32_t pixels[16 * 16];
    char title[300];
    SDL_Window *win;
    size_t i;

    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW, &s), AAR_OK, 270);
    win = runner_window();
    CHECK(win != 0, 271);
    CHECK(strcmp(SDL_GetWindowTitle(win), "aar-host") == 0, 272);
    CHECK_RC(aar_display_set_title(s.id, "T\xc3\xa4st"), AAR_OK, 273);
    CHECK(strcmp(SDL_GetWindowTitle(win), "T\xc3\xa4st") == 0, 274);
    CHECK_RC(aar_display_set_title(s.id, ""), AAR_OK, 275);
    memset(title, 'a', sizeof title);
    title[255] = 0;
    CHECK_RC(aar_display_set_title(s.id, title), AAR_OK, 276);
    CHECK(strlen(SDL_GetWindowTitle(win)) == 255u, 277);
    title[256] = 0;
    title[255] = 'a';
    CHECK_RC(aar_display_set_title(s.id, title), AAR_E_INVAL, 278);
    CHECK_RC(aar_display_set_title(s.id, 0), AAR_E_INVAL, 279);
    CHECK_RC(aar_display_set_title(s.id + 1u, "x"), AAR_E_INVAL, 280);
    CHECK_RC(aar_display_set_title(s.id, "kept"), AAR_OK, 281);

    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_HIDDEN, 0), AAR_OK, 282);
    CHECK(SDL_ShowCursor(SDL_QUERY) == SDL_DISABLE, 283);
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_DEFAULT, 0), AAR_OK, 284);
    CHECK(SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE, 285);
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_BUSY, 0), AAR_OK, 286);
    CHECK(SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE, 287);
    for (i = 0; i < 16u * 16u; i++)
        pixels[i] = (i & 1u) ? 0xff00ff00u : 0x00000000u;
    memset(&ptr, 0, sizeof ptr);
    ptr.width = 16u;
    ptr.height = 16u;
    ptr.hot_x = 3;
    ptr.hot_y = 4;
    ptr.argb = pixels;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_HIDDEN, 0), AAR_OK, 288);
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_OK, 289);
    CHECK(SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE, 290);
    pixels[0] = 0; /* the runtime copied the pixels: changing them is harmless */
    ptr.width = 64u;
    ptr.height = 64u;
    ptr.hot_x = 63;
    ptr.hot_y = 63;
    {
        static uint32_t big[64 * 64];

        ptr.argb = big;
        CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_OK, 291);
    }
    ptr.argb = pixels;
    ptr.width = 16u;
    ptr.height = 16u;
    ptr.hot_x = 3;
    ptr.hot_y = 4;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, 0), AAR_E_INVAL, 292);
    ptr.argb = 0;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_E_INVAL, 293);
    ptr.argb = pixels;
    ptr.width = 0u;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_E_INVAL, 294);
    ptr.width = 65u;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_E_INVAL, 295);
    ptr.width = 16u;
    ptr.height = 65u;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_E_INVAL, 296);
    ptr.height = 16u;
    ptr.hot_x = -1;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_E_INVAL, 297);
    ptr.hot_x = 16;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_E_INVAL, 298);
    ptr.hot_x = 3;
    ptr.hot_y = 16;
    CHECK_RC(aar_display_pointer(s.id, AAR_POINTER_CUSTOM, &ptr), AAR_E_INVAL, 299);
    CHECK_RC(aar_display_pointer(s.id, 4u, 0), AAR_E_INVAL, 300);
    CHECK_RC(aar_display_pointer(s.id + 1u, AAR_POINTER_DEFAULT, 0), AAR_E_INVAL, 301);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 302);
    /* A closed display leaves the default pointer. */
    CHECK(SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE, 303);
    return 0;
}

static int test_present_vblank(void)
{
    aar_surface s;
    aar_display_info di;
    uint64_t t0;
    uint64_t t1;
    uint64_t shown;
    uint64_t period;
    uint64_t start;
    uint64_t end;
    int rc;

    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW, &s), AAR_OK, 310);
    CHECK(aar_display_last_present_us() == 0u, 311);
    di.size = (uint32_t)sizeof di;
    CHECK_RC(aar_display_get_info(s.id, &di), AAR_OK, 312);
    CHECK_RC(aar_display_wait_vblank(0u), AAR_E_INVAL, 313);
    t0 = aar_time_us();
    CHECK_RC(aar_display_flip(s.id), AAR_OK, 314);
    t1 = aar_time_us();
    shown = aar_display_last_present_us();
    CHECK(shown >= t0 && shown <= t1 && shown != 0u, 315);
    /* An async flip counts when it completes. */
    CHECK_RC(aar_display_flip_async(s.id), AAR_OK, 316);
    CHECK(aar_display_last_present_us() == shown, 317);
    t0 = aar_time_us();
    aar_yield();
    t1 = aar_time_us();
    CHECK(aar_display_last_present_us() >= t0 && aar_display_last_present_us() <= t1, 318);
    CHECK(aar_display_last_present_us() >= shown, 319);

    if (di.refresh_mhz == 0u) {
        CHECK_RC(aar_display_wait_vblank(1000u), AAR_E_NOSYS, 320);
        printf("vblank=nosys\n");
        return aar_display_close(s.id);
    }
    period = 1000000000ull / di.refresh_mhz;
    /* The blank after the last flip is one period after it. A call ends at it. */
    CHECK_RC(aar_display_flip(s.id), AAR_OK, 321);
    shown = aar_display_last_present_us();
    start = aar_time_us();
    CHECK_RC(aar_display_wait_vblank(1000000u), AAR_OK, 322);
    end = aar_time_us();
    CHECK(end >= start && end >= shown + period - 1000u, 324);
    CHECK(end <= start + period + 50000u, 325); /* at most one period, and the sleep's slack */
    /* The one after it is a period later. */
    start = aar_time_us();
    CHECK_RC(aar_display_wait_vblank(1000000u), AAR_OK, 326);
    end = aar_time_us() - start;
    CHECK(end <= period + 50000u, 327);
    /* A timeout shorter than the wait is reported. */
    start = aar_time_us();
    rc = aar_display_wait_vblank(1u);
    CHECK(rc == AAR_E_TIMEDOUT, 328);
    CHECK(aar_time_us() - start < 4u * period + 50000u, 329);
    printf("vblank=ok period_us=%llu\n", (unsigned long long)period);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 330);
    return 0;
}

static int fill(void *p, uint32_t bytes)
{
    memset(p, 0x5a, bytes);
    return 0;
}

static int test_resize(void)
{
    aar_surface s;
    aar_surface r;
    aar_display_info di;
    aar_event ev[8];
    uint32_t n = 0;
    uint32_t i;
    int saw = 0;

    /* Without the cap. */
    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW, &s), AAR_OK, 340);
    CHECK_RC(aar_display_resize(s.id, 80u, 60u, &r), AAR_E_NOSYS, 341);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 342);

    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565,
                              AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, &s), AAR_OK, 343);
    /* The scripted resize event is delivered with the cap. */
    CHECK_RC(aar_event_poll(ev, 8u, &n), AAR_OK, 344);
    for (i = 0; i < n; i++) {
        if (ev[i].type == AAR_EV_RESIZE) {
            CHECK(ev[i].dx == 80 && ev[i].dy == 60, 345);
            saw = 1;
        }
    }
    CHECK(saw, 346);
    CHECK_RC(aar_display_resize(s.id + 1u, 80u, 60u, &r), AAR_E_INVAL, 347);
    CHECK_RC(aar_display_resize(s.id, 0u, 60u, &r), AAR_E_INVAL, 348);
    CHECK_RC(aar_display_resize(s.id, 80u, 0u, &r), AAR_E_INVAL, 349);
    CHECK_RC(aar_display_resize(s.id, AAR_DISPLAY_MAX_DIM + 1u, 60u, &r), AAR_E_INVAL, 350);
    CHECK_RC(aar_display_resize(s.id, 80u, AAR_DISPLAY_MAX_DIM + 1u, &r), AAR_E_INVAL, 351);
    CHECK_RC(aar_display_resize(s.id, 80u, 60u, 0), AAR_E_INVAL, 352);
    /* A pending flip. */
    CHECK_RC(aar_display_flip_async(s.id), AAR_OK, 353);
    CHECK_RC(aar_display_resize(s.id, 80u, 60u, &r), AAR_E_BUSY, 354);
    aar_yield();
    memset(&r, 0xaa, sizeof r);
    CHECK_RC(aar_display_resize(s.id, 80u, 60u, &r), AAR_OK, 355);
    CHECK(r.id == s.id && r.width == 80u && r.height == 60u, 356);
    CHECK(r.format == AAR_FMT_RGB565 && r.layout == AAR_LAYOUT_RGB565_BE, 357);
    CHECK(r.pitch >= 160u && r.back != 0 && r.front != r.back, 358);
    CHECK((r.caps & AAR_DISPCAP_RESIZE) != 0u, 359);
    fill(r.back, r.pitch * r.height); /* the new buffers hold the whole surface */
    di.size = (uint32_t)sizeof di;
    CHECK_RC(aar_display_get_info(s.id, &di), AAR_OK, 360);
    CHECK(di.width == 80u && di.height == 60u, 361);
    CHECK_RC(aar_display_flip(s.id), AAR_OK, 362);
    CHECK_RC(aar_display_refresh(&r), AAR_OK, 363);
    CHECK(r.width == 80u && r.height == 60u, 364);
    fill(r.back, r.pitch * r.height);
    CHECK_RC(aar_display_resize(s.id, 40u, 30u, &r), AAR_OK, 365);
    CHECK(r.width == 40u && r.height == 30u, 366);
    fill(r.back, r.pitch * r.height);
    CHECK_RC(aar_display_flip(s.id), AAR_OK, 367);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 368);

    /* A CLUT8 surface keeps its format. */
    CHECK_RC(aar_display_open(16u, 16u, AAR_FMT_CLUT8,
                              AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, &s), AAR_OK, 369);
    CHECK_RC(aar_display_resize(s.id, 20u, 10u, &r), AAR_OK, 370);
    CHECK(r.pitch >= 20u && r.layout == AAR_LAYOUT_CLUT8, 371);
    fill(r.back, r.pitch * r.height);
    CHECK_RC(aar_display_flip(s.id), AAR_OK, 372);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 373);
    return 0;
}

/* ---- events ---------------------------------------------------------------- */

/* A visible window can get real focus events at any time (under Xvfb, say),
 * so drain leaves AAR_EV_ACTIVE out unless a test is about focus. */
static int want_active;

/* The runner handles what SDL queued when the app yields. */
static int drain(aar_event *out, int max)
{
    int total = 0;
    uint32_t n = 0;
    int i;
    int kept = 0;

    aar_yield();
    while (total < max) {
        if (aar_event_poll(out + total, (uint32_t)(max - total), &n) != AAR_OK || n == 0u)
            break;
        total += (int)n;
    }
    for (i = 0; i < total; i++) {
        if (!want_active && out[i].type == AAR_EV_ACTIVE)
            continue;
        out[kept++] = out[i];
    }
    return kept;
}

static void push_window(uint8_t what, uint32_t id, int d1, int d2)
{
    SDL_Event e;

    memset(&e, 0, sizeof e);
    e.type = SDL_WINDOWEVENT;
    e.window.type = SDL_WINDOWEVENT;
    e.window.windowID = id;
    e.window.event = what;
    e.window.data1 = d1;
    e.window.data2 = d2;
    SDL_PushEvent(&e);
}

static void push_wheel(int x, int y, uint32_t direction)
{
    SDL_Event e;

    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEWHEEL;
    e.wheel.type = SDL_MOUSEWHEEL;
    e.wheel.x = x;
    e.wheel.y = y;
    e.wheel.direction = direction;
    SDL_PushEvent(&e);
}

static void push_text(const char *utf8)
{
    SDL_Event e;

    memset(&e, 0, sizeof e);
    e.type = SDL_TEXTINPUT;
    e.text.type = SDL_TEXTINPUT;
    snprintf(e.text.text, sizeof e.text.text, "%s", utf8);
    SDL_PushEvent(&e);
}

static void push_motion(int x, int y, int xrel, int yrel)
{
    SDL_Event e;

    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEMOTION;
    e.motion.type = SDL_MOUSEMOTION;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = xrel;
    e.motion.yrel = yrel;
    SDL_PushEvent(&e);
}

static void push_button(int down, uint8_t button, int x, int y)
{
    SDL_Event e;

    memset(&e, 0, sizeof e);
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.type = e.type;
    e.button.button = button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

static int info_flags(uint32_t id, uint32_t *flags)
{
    aar_display_info di;

    di.size = (uint32_t)sizeof di;
    if (aar_display_get_info(id, &di) != AAR_OK)
        return -1;
    *flags = di.flags;
    return 0;
}

static int test_events(void)
{
    aar_surface s;
    aar_event ev[16];
    SDL_Window *win;
    uint32_t wid;
    uint32_t flags = 0;
    int n;

    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, ALL_FLAGS, &s), AAR_OK, 400);
    win = runner_window();
    CHECK(win != 0, 401);
    wid = SDL_GetWindowID(win);
    (void)drain(ev, 16);

    /* The wheel: dx right, dy down; SDL's y is up and flips with the direction. */
    push_wheel(1, 2, SDL_MOUSEWHEEL_NORMAL);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_WHEEL && ev[0].dx == 1 && ev[0].dy == -2 &&
          ev[0].code == 0u && ev[0].buttons == 0u && ev[0].reserved == 0u, 402);
    push_wheel(1, 2, SDL_MOUSEWHEEL_FLIPPED);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_WHEEL && ev[0].dx == -1 && ev[0].dy == 2, 403);
    push_wheel(0, -3, SDL_MOUSEWHEEL_NORMAL);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].dx == 0 && ev[0].dy == 3, 404);
    push_wheel(0, 0, SDL_MOUSEWHEEL_NORMAL);
    n = drain(ev, 16);
    CHECK(n == 0, 405);

    /* Text: code points up to U+FFFF; anything outside, and bad UTF-8, is dropped. */
    push_text("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z");
    n = drain(ev, 16);
    CHECK(n == 4, 406);
    CHECK(ev[0].type == AAR_EV_TEXT && ev[0].code == 0x61u, 407);
    CHECK(ev[1].type == AAR_EV_TEXT && ev[1].code == 0xe9u, 408);
    CHECK(ev[2].type == AAR_EV_TEXT && ev[2].code == 0x20acu, 409);
    CHECK(ev[3].type == AAR_EV_TEXT && ev[3].code == 0x7au, 410);
    push_text("\xc3");
    push_text("b\xff" "c");
    push_text("\xc0\x80");
    n = drain(ev, 16);
    CHECK(n == 2 && ev[0].code == 0x62u && ev[1].code == 0x63u, 411);

    /* Focus: one event per change, only for the window of the display. */
    want_active = 1;
    push_window(SDL_WINDOWEVENT_FOCUS_LOST, wid, 0, 0);
    (void)drain(ev, 16);
    CHECK(info_flags(s.id, &flags) == 0 && (flags & AAR_DISPINFO_ACTIVE) == 0u, 412);
    push_window(SDL_WINDOWEVENT_FOCUS_GAINED, wid + 100u, 0, 0);
    n = drain(ev, 16);
    CHECK(n == 0, 413);
    push_window(SDL_WINDOWEVENT_FOCUS_GAINED, wid, 0, 0);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_ACTIVE && ev[0].code == 1u, 414);
    CHECK(info_flags(s.id, &flags) == 0 && (flags & AAR_DISPINFO_ACTIVE) != 0u, 415);
    push_window(SDL_WINDOWEVENT_FOCUS_GAINED, wid, 0, 0);
    n = drain(ev, 16);
    CHECK(n == 0, 416);
    push_window(SDL_WINDOWEVENT_FOCUS_LOST, wid, 0, 0);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_ACTIVE && ev[0].code == 0u, 417);
    CHECK(info_flags(s.id, &flags) == 0 && (flags & AAR_DISPINFO_ACTIVE) == 0u, 418);
    want_active = 0;
    /* Resize: the window's new size, once. */
    push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid, 64, 48); /* the size it has */
    n = drain(ev, 16);
    CHECK(n == 0, 419);
    push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid, 100, 80);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_RESIZE && ev[0].dx == 100 && ev[0].dy == 80, 420);
    push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid, 100, 80);
    n = drain(ev, 16);
    CHECK(n == 0, 421);
    push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid + 100u, 120, 90);
    n = drain(ev, 16);
    CHECK(n == 0, 422);
    {
        aar_surface r;

        CHECK_RC(aar_display_resize(s.id, 100u, 80u, &r), AAR_OK, 423);
        /* SDL reports the size that the runner set: not news. */
        push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid, 100, 80);
        n = drain(ev, 16);
        CHECK(n == 0, 424);
        /* Back to the old size is news: the app was last told 100x80. */
        push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid, 64, 48);
        n = drain(ev, 16);
        CHECK(n == 1 && ev[0].type == AAR_EV_RESIZE && ev[0].dx == 64 && ev[0].dy == 48, 425);
    }

    /* Mouse with the position: every event is absolute. */
    push_motion(10, 20, 3, 4);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_MOUSE && ev[0].code == 0u &&
          ev[0].dx == 10 && ev[0].dy == 20 && ev[0].buttons == AAR_MOUSE_ABSOLUTE, 426);
    push_motion(10, 20, 0, 0); /* a motion without movement still reports where it is */
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].dx == 10 && ev[0].dy == 20, 427);
    push_button(1, SDL_BUTTON_LEFT, 11, 21);
    push_motion(-5, 300, -16, 280);
    push_button(1, SDL_BUTTON_RIGHT, 12, 22);
    push_button(0, SDL_BUTTON_LEFT, 13, 23);
    push_button(1, SDL_BUTTON_MIDDLE, 14, 24);
    push_button(0, SDL_BUTTON_MIDDLE, 14, 24);
    push_button(0, SDL_BUTTON_RIGHT, 15, 25);
    push_button(1, SDL_BUTTON_X1, 15, 25); /* not a button of the API */
    n = drain(ev, 16);
    CHECK(n == 7, 428);
    CHECK(ev[0].code == 1u && ev[0].buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT) &&
          ev[0].dx == 11 && ev[0].dy == 21, 429);
    CHECK(ev[1].code == 0u && ev[1].buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT) &&
          ev[1].dx == -5 && ev[1].dy == 300, 430); /* outside the surface, button held */
    CHECK(ev[2].code == 2u && ev[2].buttons ==
          (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_LEFT | AAR_MOUSE_RIGHT), 431);
    CHECK(ev[3].code == 1u && ev[3].buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_RIGHT), 432);
    CHECK(ev[4].code == 3u && ev[4].buttons ==
          (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_RIGHT | AAR_MOUSE_MIDDLE), 433);
    CHECK(ev[5].code == 3u && ev[5].buttons == (AAR_MOUSE_ABSOLUTE | AAR_MOUSE_RIGHT), 434);
    CHECK(ev[6].code == 2u && ev[6].buttons == AAR_MOUSE_ABSOLUTE &&
          ev[6].dx == 15 && ev[6].dy == 25, 435);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 436);

    /* A close request instead of a stop, and the SDL_QUIT that follows it. */
    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, ALL_FLAGS, &s), AAR_OK, 437);
    win = runner_window();
    wid = SDL_GetWindowID(win);
    (void)drain(ev, 16);
    push_window(SDL_WINDOWEVENT_CLOSE, wid, 0, 0);
    {
        SDL_Event q;

        memset(&q, 0, sizeof q);
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    }
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_CLOSE_REQUEST && ev[0].code == 0u, 438);
    CHECK(aar_should_stop() == 0, 439);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 440);
    return 0;
}

/* Without the caps: relative motion, no text, no resize, a close is a stop. */
static int test_events_nocap(void)
{
    aar_surface s;
    aar_event ev[16];
    SDL_Window *win;
    uint32_t wid;
    int n;
    int stop = 0;
    int close_req = 0;
    int i;

    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, &s), AAR_OK, 450);
    win = runner_window();
    wid = SDL_GetWindowID(win);
    (void)drain(ev, 16);
    push_motion(10, 20, 3, 4);
    push_motion(11, 21, 0, 0);
    push_button(1, SDL_BUTTON_LEFT, 11, 21);
    push_motion(12, 22, -2, 1);
    n = drain(ev, 16);
    CHECK(n == 3, 451);
    CHECK(ev[0].type == AAR_EV_MOUSE && ev[0].code == 0u && ev[0].dx == 3 && ev[0].dy == 4 &&
          ev[0].buttons == 0u, 452);
    CHECK(ev[1].code == 1u && ev[1].dx == 0 && ev[1].dy == 0 && ev[1].buttons == AAR_MOUSE_LEFT, 453);
    CHECK(ev[2].code == 0u && ev[2].dx == -2 && ev[2].dy == 1 && ev[2].buttons == AAR_MOUSE_LEFT, 454);
    push_button(0, SDL_BUTTON_LEFT, 0, 0);
    (void)drain(ev, 16);
    /* No text without AAR_DISPLAY_TEXT_INPUT. */
    push_text("abc");
    n = drain(ev, 16);
    CHECK(n == 0, 455);
    /* The window resize is reported with the cap that was granted. */
    push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid, 70, 50);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_RESIZE && ev[0].dx == 70 && ev[0].dy == 50, 456);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 457);
    /* A display that is not resizable hears nothing of it. */
    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, 0, &s), AAR_OK, 458);
    win = runner_window();
    wid = SDL_GetWindowID(win);
    push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid, 70, 50);
    n = drain(ev, 16);
    CHECK(n == 0, 459);
    /* A close is a stop. */
    push_window(SDL_WINDOWEVENT_CLOSE, wid, 0, 0);
    n = drain(ev, 16);
    for (i = 0; i < n; i++) {
        if (ev[i].type == AAR_EV_STOP)
            stop++;
        if (ev[i].type == AAR_EV_CLOSE_REQUEST)
            close_req++;
    }
    CHECK(stop == 1 && close_req == 0, 460);
    CHECK(aar_should_stop() == 1, 461);
    return 0;
}

/* Run with --scale 2: surface pixels are half the window's. */
static int test_events_scaled(void)
{
    aar_surface s;
    aar_event ev[16];
    SDL_Window *win;
    uint32_t wid;
    int n;

    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565,
                              AAR_DISPLAY_POINTER_ABS | AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, &s), AAR_OK, 470);
    win = runner_window();
    wid = SDL_GetWindowID(win);
    (void)drain(ev, 16);
    push_motion(10, 21, 1, 1);
    push_motion(-1, -2, 1, 1); /* left of the surface: the floor, not zero */
    n = drain(ev, 16);
    CHECK(n == 2 && ev[0].dx == 5 && ev[0].dy == 10 && ev[0].buttons == AAR_MOUSE_ABSOLUTE, 471);
    CHECK(ev[1].dx == -1 && ev[1].dy == -1, 472);
    /* The window is twice the surface: 200x100 pixels are a 100x50 surface. */
    push_window(SDL_WINDOWEVENT_SIZE_CHANGED, wid, 200, 100);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].type == AAR_EV_RESIZE && ev[0].dx == 100 && ev[0].dy == 50, 473);
    CHECK_RC(aar_display_close(s.id), AAR_OK, 474);

    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565,
                              AAR_DISPLAY_POINTER_ABS | AAR_DISPLAY_SCALE, &s), AAR_OK, 475);
    (void)drain(ev, 16);
    push_motion(10, 21, 1, 1);
    n = drain(ev, 16);
    CHECK(n == 1 && ev[0].dx == 10 && ev[0].dy == 21, 476); /* SDL already scales it */
    CHECK_RC(aar_display_close(s.id), AAR_OK, 477);
    return 0;
}

/* The scripted events, in order, as the app sees them with the flags in
 * argv[2]. The script has events for every cap. */
static int test_script(int argc, char **argv)
{
    aar_surface s;
    aar_event ev[64];
    uint32_t flags = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 0u;
    int n;
    int i;

    want_active = 1; /* the script's focus events are part of what is compared */
    CHECK_RC(aar_display_open(64u, 48u, AAR_FMT_RGB565, flags, &s), AAR_OK, 480);
    n = drain(ev, 64);
    for (i = 0; i < n; i++)
        printf("ev %u %u %d %d %u\n", (unsigned)ev[i].type, (unsigned)ev[i].code,
               (int)ev[i].dx, (int)ev[i].dy, (unsigned)ev[i].buttons);
    return 0;
}

/* Scripted motion that the queue could not hold: merged, never lost. */
static int test_merge(int argc, char **argv)
{
    aar_event ev[64];
    uint32_t n = 0;
    int abs_mode = argc > 2 && strcmp(argv[2], "abs") == 0;
    long sum_x = 0;
    long sum_y = 0;
    long sum_after_x = 0;
    long sum_after_y = 0;
    int button_seen = 0;
    int events = 0;
    int last_x = 0;
    int last_y = 0;

    for (;;) {
        uint32_t i;

        if (aar_event_poll(ev, 64u, &n) != AAR_OK || n == 0u)
            break;
        for (i = 0; i < n; i++) {
            events++;
            if (ev[i].type != AAR_EV_MOUSE)
                FAIL(490, "an event that is not mouse");
            if (abs_mode) {
                if (!(ev[i].buttons & AAR_MOUSE_ABSOLUTE))
                    FAIL(491, "a relative event");
                last_x = ev[i].dx;
                last_y = ev[i].dy;
            } else if (ev[i].code != 0u) {
                button_seen++;
                if (ev[i].buttons != AAR_MOUSE_LEFT)
                    FAIL(492, "the button state");
            } else if (button_seen) {
                sum_after_x += ev[i].dx;
                sum_after_y += ev[i].dy;
                if (ev[i].buttons != AAR_MOUSE_LEFT)
                    FAIL(493, "buttons of the motion after the press");
            } else {
                sum_x += ev[i].dx;
                sum_y += ev[i].dy;
                if (ev[i].buttons != 0u)
                    FAIL(494, "buttons of the motion before the press");
            }
        }
    }
    if (abs_mode) {
        printf("abs events=%d last=%d,%d\n", events, last_x, last_y);
    } else {
        printf("rel events=%d before=%ld,%ld buttons=%d after=%ld,%ld\n", events,
               sum_x, sum_y, button_seen, sum_after_x, sum_after_y);
    }
    return 0;
}

/* ---- game controllers --------------------------------------------------------- */

static int expect_joy(const char *what, aar_event *ev, int n, uint16_t port,
                      int dx, int dy, uint16_t buttons)
{
    if (n != 1 || ev[0].type != AAR_EV_JOY || ev[0].code != port ||
        ev[0].dx != dx || ev[0].dy != dy || ev[0].buttons != buttons) {
        fprintf(stderr, "api06: %s: %d events", what, n);
        if (n > 0)
            fprintf(stderr, ", first type %u code %u dx %d dy %d buttons %#x",
                    (unsigned)ev[0].type, (unsigned)ev[0].code, (int)ev[0].dx,
                    (int)ev[0].dy, (unsigned)ev[0].buttons);
        fprintf(stderr, "\n");
        return 0;
    }
    return 1;
}

static int test_pads(void)
{
    static const struct {
        int sdl_button;
        uint16_t bit;
        const char *name;
    } map[] = {
        {0, AAR_JOY_BUTTON1, "A"},      {1, AAR_JOY_BUTTON2, "B"},
        {2, AAR_JOY_BUTTON3, "X"},      {3, AAR_JOY_BUTTON4, "Y"},
        {4, AAR_JOY_PLAY, "start"},     {6, AAR_JOY_FORWARD, "right shoulder"},
        {5, AAR_JOY_REVERSE, "left shoulder"},
    };
    char guid[64];
    char mapping[512];
    SDL_Event e;
    SDL_Joystick *joy;
    aar_event ev[16];
    int idx;
    int n;
    size_t i;

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "api06: no game controller support: %s\n", SDL_GetError());
        return 77;
    }
    idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    if (idx < 0) {
        fprintf(stderr, "api06: no virtual joystick: %s\n", SDL_GetError());
        return 77;
    }
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(idx), guid, sizeof guid);
    snprintf(mapping, sizeof mapping,
             "%s,Virtual,a:b0,b:b1,x:b2,y:b3,start:b4,leftshoulder:b5,"
             "rightshoulder:b6,dpup:b7,dpdown:b8,dpleft:b9,dpright:b10,"
             "leftx:a0,lefty:a1,", guid); /* SDL appends its own key after the last comma */
    if (SDL_GameControllerAddMapping(mapping) < 0 || !SDL_IsGameController(idx)) {
        fprintf(stderr, "api06: the virtual joystick is no game controller: %s\n", SDL_GetError());
        return 77;
    }
    joy = SDL_JoystickOpen(idx);
    CHECK(joy != 0, 500);
    (void)drain(ev, 16);
    /* SDL announces the controller; so does this event if SDL did not yet. */
    memset(&e, 0, sizeof e);
    e.type = SDL_CONTROLLERDEVICEADDED;
    e.cdevice.type = SDL_CONTROLLERDEVICEADDED;
    e.cdevice.which = idx;
    SDL_PushEvent(&e);
    n = drain(ev, 16);
    CHECK(n == 0, 501); /* nothing is pressed */

    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        SDL_JoystickSetVirtualButton(joy, map[i].sdl_button, 1);
        n = drain(ev, 16);
        if (!expect_joy(map[i].name, ev, n, 2u, 0, 0, map[i].bit))
            return 502;
        SDL_JoystickSetVirtualButton(joy, map[i].sdl_button, 0);
        n = drain(ev, 16);
        if (!expect_joy("release", ev, n, 2u, 0, 0, 0))
            return 503;
    }
    /* The d-pad and the left stick. */
    SDL_JoystickSetVirtualButton(joy, 9, 1);
    n = drain(ev, 16);
    if (!expect_joy("d-pad left", ev, n, 2u, -1, 0, 0))
        return 504;
    SDL_JoystickSetVirtualButton(joy, 9, 0);
    SDL_JoystickSetVirtualButton(joy, 8, 1);
    SDL_JoystickSetVirtualButton(joy, 10, 1);
    n = drain(ev, 16);
    if (!expect_joy("d-pad down right", ev, n, 2u, 1, 1, 0))
        return 505;
    SDL_JoystickSetVirtualButton(joy, 8, 0);
    SDL_JoystickSetVirtualButton(joy, 10, 0);
    SDL_JoystickSetVirtualButton(joy, 7, 1);
    n = drain(ev, 16);
    if (!expect_joy("d-pad up", ev, n, 2u, 0, -1, 0))
        return 506;
    SDL_JoystickSetVirtualButton(joy, 7, 0);
    SDL_JoystickSetVirtualAxis(joy, 0, 30000);
    SDL_JoystickSetVirtualAxis(joy, 1, -30000);
    n = drain(ev, 16);
    if (!expect_joy("left stick", ev, n, 2u, 1, -1, 0))
        return 507;
    SDL_JoystickSetVirtualAxis(joy, 0, 8000); /* inside the dead zone */
    n = drain(ev, 16);
    if (!expect_joy("stick x to the dead zone", ev, n, 2u, 0, -1, 0))
        return 508;
    SDL_JoystickSetVirtualAxis(joy, 1, 0);
    n = drain(ev, 16);
    if (!expect_joy("stick centred", ev, n, 2u, 0, 0, 0))
        return 509;
    /* Two buttons at once, then the device goes away with one held. */
    SDL_JoystickSetVirtualButton(joy, 0, 1);
    SDL_JoystickSetVirtualButton(joy, 3, 1);
    n = drain(ev, 16);
    if (!expect_joy("A and Y", ev, n, 2u, 0, 0, AAR_JOY_BUTTON1 | AAR_JOY_BUTTON4))
        return 510;
    SDL_JoystickClose(joy);
    SDL_JoystickDetachVirtual(idx);
    n = drain(ev, 16);
    if (!expect_joy("removal releases the buttons", ev, n, 2u, 0, 0, 0))
        return 511;
    return 0;
}

/* ---- main ----------------------------------------------------------------------- */

int aar_main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";

    if (strcmp(mode, "files") == 0)
        return test_files();
    if (strcmp(mode, "time") == 0)
        return test_time();
    if (strcmp(mode, "locale") == 0)
        return test_locale();
    if (strcmp(mode, "envdir") == 0)
        return test_env_dir();
    if (strcmp(mode, "envproc") == 0)
        return test_env_proc();
    if (strcmp(mode, "clip") == 0)
        return test_clip();
    if (strcmp(mode, "display") == 0) {
        int rc = test_display_closed();

        if (!rc)
            rc = test_display_caps();
        if (!rc)
            rc = test_display_info();
        if (!rc)
            rc = test_title_pointer();
        return rc;
    }
    if (strcmp(mode, "present") == 0)
        return test_present_vblank();
    if (strcmp(mode, "resize") == 0)
        return test_resize();
    if (strcmp(mode, "events") == 0)
        return test_events();
    if (strcmp(mode, "events_nocap") == 0)
        return test_events_nocap();
    if (strcmp(mode, "events_scaled") == 0)
        return test_events_scaled();
    if (strcmp(mode, "script") == 0)
        return test_script(argc, argv);
    if (strcmp(mode, "merge") == 0)
        return test_merge(argc, argv);
    if (strcmp(mode, "pads") == 0)
        return test_pads();
    fprintf(stderr, "usage: api06 <files|time|locale|envdir|envproc|clip|display|present|"
                    "resize|events|events_nocap|events_scaled|script|merge|pads>\n");
    return 1;
}
