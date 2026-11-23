/*
 * Conformance payload for C-FILE (spec 7.1): files from the app's side. The
 * runner (r_file.c) is the client: it prepares the fixture tree, checks the
 * files the app leaves behind and, in the variants stall and leak, serves the
 * app by hand. The fixtures, the variants and the peer protocol are in
 * conf_file.h. Without the runner (the host runner, a run by hand) the tests that
 * need a fixture directory print SKIP host and the rest run in the current
 * directory, on files whose names start with conf-file-.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_file.h"

AAR_NOTE(0, 0, 0, 0)

#define FILE_RW (AAR_FILE_READ | AAR_FILE_WRITE)
#define FILE_NEW (AAR_FILE_WRITE | AAR_FILE_CREATE | AAR_FILE_TRUNCATE)

static int peer;               /* a runner is the client: the fixtures exist */
static uint32_t platform_id;
static aar_info info;

static uint8_t mega[CONF_FILE_PATTERN_BYTES];

/* ---- Strings and paths (no libc) ------------------------------------------------- */

static char path_buf[8][320];
static unsigned path_next;

static uint32_t slen(const char *s)
{
    uint32_t n = 0;

    while (s[n])
        n++;
    return n;
}

static int seq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static char *path_slot(void)
{
    char *p = path_buf[path_next++ % 8u];

    p[0] = '\0';
    return p;
}

static void scat(char *d, const char *s)
{
    uint32_t n = slen(d);

    while (*s && n < 318u)
        d[n++] = *s++;
    d[n] = '\0';
}

/* A path in the fixture directory. */
static const char *fx(const char *name)
{
    char *p = path_slot();

    scat(p, CONF_FILE_DIR "/");
    scat(p, name);
    return p;
}

/* A scratch file: in the fixture directory with a runner, else in the current
 * directory. */
static const char *sc(const char *name)
{
    char *p;

    if (peer)
        return fx(name);
    p = path_slot();
    scat(p, "conf-file-");
    scat(p, name);
    return p;
}

/* A scratch name that does not exist yet: name-N.tmp for the first N that is
 * missing (a host run leaves its files behind; a platform run starts clean). The
 * result has a slot of its own, so that later calls of fx and sc do not
 * overwrite it. */
static const char *uniq(const char *base)
{
    static char ring[8][320];
    static unsigned next;
    char *out = ring[next++ % 8u];
    uint32_t n;

    for (n = 0; n < 9999u; n++) {
        char name[96];
        char num[8];
        aar_stat st;
        uint32_t v = n;
        int i = 0;
        int k;

        name[0] = '\0';
        do {
            num[i++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v);
        scat(name, base);
        scat(name, "-");
        for (k = i - 1; k >= 0; k--) {
            char one[2];

            one[0] = num[k];
            one[1] = '\0';
            scat(name, one);
        }
        scat(name, ".tmp");
        out[0] = '\0';
        scat(out, sc(name));
        if (aar_file_stat(out, &st) == AAR_E_NOENT)
            return out;
    }
    out[0] = '\0';
    scat(out, sc(base));
    return out;
}

/* ---- File helpers ----------------------------------------------------------------------- */

/* Creates or replaces a file with n bytes. Returns AAR_OK or the failing code. */
static int put(const char *path, const void *data, uint32_t n)
{
    uint32_t done = 0;
    int fd = aar_file_open(path, FILE_NEW);
    int rc;

    if (fd < 0)
        return fd;
    while (done < n) {
        uint32_t wrote = 0;

        rc = aar_file_write(fd, (const uint8_t *)data + done, n - done, &wrote);
        if (rc != AAR_OK || wrote == 0) {
            (void)aar_file_close(fd);
            return rc != AAR_OK ? rc : AAR_E_IO;
        }
        done += wrote;
    }
    return aar_file_close(fd);
}

static void small_fill(uint8_t *buf, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        buf[i] = conf_file_small(i);
}

/* The 100 byte scratch file. */
static int put_small(const char *path)
{
    uint8_t buf[100];

    small_fill(buf, sizeof buf);
    return put(path, buf, sizeof buf);
}

/* The size of a file, or -1 with *rc the code. */
static int64_t size_of(const char *path, int *rc)
{
    aar_stat st;

    *rc = aar_file_stat(path, &st);
    return *rc == AAR_OK ? (int64_t)st.size : -1;
}

static void fail_rc(struct conf_test *t, const char *what, int rc)
{
    if (!conf_fail_first(t))
        return;
    conf_buf_str(&t->why, what);
    conf_buf_str(&t->why, " gave ");
    conf_buf_i64(&t->why, rc);
}

/* Fails the test when rc is not want. Returns 1 when it is. */
static int expect(struct conf_test *t, int rc, int want, const char *what)
{
    if (rc == want)
        return 1;
    fail_rc(t, what, rc);
    return 0;
}

/* The call must fail with any negative code. */
static int expect_neg(struct conf_test *t, int rc, const char *what)
{
    if (rc < 0)
        return 1;
    fail_rc(t, what, rc);
    return 0;
}

static int rpc(const char *keyword, uint32_t w0, uint32_t out[4])
{
    int rc = conf_file_send(keyword, w0, 0, 0, 0);

    if (rc != AAR_OK)
        return rc;
    return conf_file_wait(CONF_FILE_RES, CONF_FILE_RPC_WAIT_US, out);
}

static void need_fixtures(const char *id)
{
    conf_skip(id, "host: needs the fixture directory the runner prepares");
}

/* ---- C-FILE.1, C-FILE.2: the path length -------------------------------------------------- */

static void test_long_path(void)
{
    struct conf_test t;
    uint8_t buf[64];
    uint32_t n = 0;
    int fd;
    int rc;

    if (!peer) {
        need_fixtures("C-FILE.1");
        need_fixtures("C-FILE.2");
        return;
    }
    conf_begin(&t, "C-FILE.1");
    fd = aar_file_open(CONF_FILE_LONG255, AAR_FILE_READ);
    if (fd < 0) {
        fail_rc(&t, "open of a path of 255 bytes", fd);
    } else {
        if ((fd < AAR_FD_BASE || fd >= 64) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "the first file id is ");
            conf_buf_i64(&t.why, fd);
            conf_buf_str(&t.why, ", not from 3 to 63");
        }
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        if ((rc != AAR_OK || n != slen(CONF_FILE_LONG_TEXT)) && conf_fail_first(&t))
            conf_buf_str(&t.why, "the file at the end of the long path does not read back");
        (void)aar_file_close(fd);
    }
    conf_buf_str(&t.note, "a path of 255 bytes opens, id ");
    conf_buf_i64(&t.note, fd);
    conf_end(&t);

    conf_begin(&t, "C-FILE.2");
    fd = aar_file_open(CONF_FILE_LONG256, AAR_FILE_READ);
    if (fd >= 0) {
        (void)aar_file_close(fd);
        conf_check(&t, 0, "a path of 256 bytes was opened");
    } else {
        conf_buf_str(&t.note, "a path of 256 bytes, which exists, is refused with ");
        conf_buf_i64(&t.note, fd);
    }
    conf_end(&t);
}

/* ---- C-FILE.3: the client's root ---------------------------------------------------------------- */

static void test_root(void)
{
    struct conf_test t;
    uint8_t buf[128];
    uint32_t n = 0;
    uint32_t res[4] = { 0, 0, 0, 0 };
    uint32_t len = slen(CONF_FILE_TEXT);
    uint32_t i;
    int fd;
    int rc;

    if (!peer) {
        need_fixtures("C-FILE.3");
        return;
    }
    conf_begin(&t, "C-FILE.3");
    fd = aar_file_open(fx(CONF_FILE_TEXT_NAME), AAR_FILE_READ);
    if (fd < 0) {
        fail_rc(&t, "open of the fixture the runner placed under the root", fd);
    } else {
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        if (rc != AAR_OK || n != len) {
            conf_check(&t, 0, "the fixture has not the length the runner wrote");
        } else {
            for (i = 0; i < len; i++) {
                if (buf[i] != (uint8_t)CONF_FILE_TEXT[i]) {
                    conf_check(&t, 0, "the fixture reads back other bytes than the runner wrote");
                    break;
                }
            }
        }
        (void)aar_file_close(fd);
    }
    rc = put(fx("created.tmp"), CONF_FILE_CREATED, slen(CONF_FILE_CREATED));
    if (rc != AAR_OK) {
        fail_rc(&t, "creating a file under the root", rc);
    } else {
        rc = rpc(CONF_FILE_CHECK, 1, res);
        if (rc != AAR_OK || res[0] != 0) {
            if (conf_fail_first(&t))
                conf_buf_str(&t.why, "the runner did not find the file the app created in its root");
        }
    }
    conf_buf_str(&t.note, "the fixture reads back and the created file is in the client's root");
    conf_end(&t);
}

/* ---- C-FILE.4, C-FILE.5: path policy (SHOULD) --------------------------------------------------- */

static void test_policy(void)
{
    struct conf_test t;
    int rc;

    conf_begin(&t, "C-FILE.4");
    rc = aar_file_open("../conf-outside.tmp", AAR_FILE_READ);
    expect(&t, rc, AAR_E_ACCES, "open of ../conf-outside.tmp");
    rc = aar_file_open("/conf-outside.tmp", AAR_FILE_READ);
    expect(&t, rc, AAR_E_ACCES, "open of /conf-outside.tmp");
    if (peer) {
        rc = aar_file_open(CONF_FILE_DIR "/../../conf-outside.tmp", AAR_FILE_READ);
        expect(&t, rc, AAR_E_ACCES, "open of conf-fixtures/../../conf-outside.tmp");
    }
    conf_buf_str(&t.note, "a leading .. and a leading / are AAR_E_ACCES");
    conf_end(&t);

    conf_begin(&t, "C-FILE.5");
    rc = aar_file_open("SYS:conf-outside.tmp", AAR_FILE_READ);
    expect(&t, rc, AAR_E_ACCES, "open of SYS:conf-outside.tmp");
    rc = aar_file_open("RAM:conf-outside.tmp", AAR_FILE_READ);
    expect(&t, rc, AAR_E_ACCES, "open of RAM:conf-outside.tmp");
    conf_buf_str(&t.note, "paths with a volume prefix are AAR_E_ACCES without AAR_SERVEF_ABS_PATHS");
    conf_end(&t);
}

/* ---- C-FILE.7: 16 files and 16 lists ------------------------------------------------------------ */

static void test_many(void)
{
    struct conf_test t;
    int files[16];
    int lists[16];
    uint8_t buf[16];
    uint32_t n = 0;
    const char *file = sc("t7.tmp");
    const char *dir = peer ? fx(CONF_FILE_DIR5_NAME) : ".";
    uint32_t i;
    uint32_t j;
    int rc;

    conf_begin(&t, "C-FILE.7");
    rc = put_small(file);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    for (i = 0; i < 16u; i++)
        files[i] = lists[i] = -1;
    for (i = 0; i < 16u; i++) {
        files[i] = aar_file_open(file, AAR_FILE_READ);
        if (files[i] < AAR_FD_BASE) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "file ");
                conf_buf_u64(&t.why, i + 1u);
                conf_buf_str(&t.why, " of 16 gave ");
                conf_buf_i64(&t.why, files[i]);
            }
            break;
        }
        for (j = 0; j < i; j++) {
            if (files[j] == files[i] && conf_fail_first(&t))
                conf_buf_str(&t.why, "two open files have the same id");
        }
    }
    for (i = 0; i < 16u; i++) {
        lists[i] = aar_file_list_open(dir);
        if (lists[i] < AAR_FD_BASE) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "list ");
                conf_buf_u64(&t.why, i + 1u);
                conf_buf_str(&t.why, " of 16 gave ");
                conf_buf_i64(&t.why, lists[i]);
            }
            break;
        }
        for (j = 0; j < i; j++) {
            if (lists[j] == lists[i] && conf_fail_first(&t))
                conf_buf_str(&t.why, "two open lists have the same id");
        }
    }
    for (i = 0; i < 16u; i++) {
        if (lists[i] >= AAR_FD_BASE)
            (void)aar_file_list_close(lists[i]);
    }
    /* Closing the lists leaves the files open. */
    if (files[0] >= AAR_FD_BASE) {
        rc = aar_file_read(files[0], buf, sizeof buf, &n);
        if ((rc != AAR_OK || n != sizeof buf || buf[5] != conf_file_small(5)) &&
            conf_fail_first(&t))
            conf_buf_str(&t.why, "a file does not read after the lists were closed");
    }
    for (i = 0; i < 16u; i++) {
        if (files[i] >= AAR_FD_BASE)
            (void)aar_file_close(files[i]);
    }
    conf_buf_str(&t.note, "16 files and 16 lists open at once");
    conf_end(&t);
}

/* ---- C-FILE.8: bad ids ---------------------------------------------------------------------------- */

static void test_bad_ids(void)
{
    struct conf_test t;
    const char *file = sc("t8.tmp");
    int ids[6];
    uint8_t buf[8];
    uint32_t n = 0;
    aar_dirent ent;
    int64_t pos;
    uint32_t i;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.8");
    rc = put_small(file);
    fd = rc == AAR_OK ? aar_file_open(file, AAR_FILE_READ) : rc;
    if (fd < 0) {
        fail_rc(&t, "creating and opening the scratch file", fd);
        conf_end(&t);
        return;
    }
    (void)aar_file_close(fd);
    ids[0] = -1;
    ids[1] = 0;
    ids[2] = 1;
    ids[3] = 2;
    ids[4] = 999;
    ids[5] = fd; /* closed */
    for (i = 0; i < 6u; i++) {
        rc = aar_file_close(ids[i]);
        expect(&t, rc, AAR_E_BADF, "close of a bad id");
        rc = aar_file_read(ids[i], buf, sizeof buf, &n);
        expect(&t, rc, AAR_E_BADF, "read of a bad id");
        rc = aar_file_write(ids[i], buf, sizeof buf, &n);
        expect(&t, rc, AAR_E_BADF, "write of a bad id");
        rc = aar_file_seek(ids[i], 0, AAR_SEEK_SET, &pos);
        expect(&t, rc, AAR_E_BADF, "seek of a bad id");
        rc = aar_file_list_next(ids[i], &ent);
        expect(&t, rc, AAR_E_BADF, "list next of a bad id");
    }
    conf_buf_str(&t.note, "-1, 0, 1, 2, 999 and a closed id are AAR_E_BADF");
    conf_end(&t);
}

/* ---- C-FILE.9 to C-FILE.18: open flags ------------------------------------------------------------ */

static void test_modes(void)
{
    struct conf_test t;
    const char *file = sc("t9.tmp");
    uint8_t buf[32];
    uint32_t n = 0;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.9");
    rc = put(file, "hello world", 11);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    fd = aar_file_open(file, AAR_FILE_READ);
    if (fd < 0) {
        fail_rc(&t, "open READ of an existing file", fd);
    } else {
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        conf_check(&t, rc == AAR_OK && n == 11 && buf[0] == 'h', "READ does not read the file");
        (void)aar_file_close(fd);
    }
    fd = aar_file_open(file, AAR_FILE_WRITE);
    if (fd < 0) {
        fail_rc(&t, "open WRITE of an existing file", fd);
    } else {
        rc = aar_file_write(fd, "HELLO", 5, &n);
        conf_check(&t, rc == AAR_OK && n == 5, "WRITE does not write");
        (void)aar_file_close(fd);
    }
    fd = aar_file_open(file, AAR_FILE_READ);
    if (fd >= 0) {
        n = 0;
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        conf_check(&t, rc == AAR_OK && n == 11 && buf[0] == 'H' && buf[5] == ' ' && buf[6] == 'w',
                   "after WRITE the file is not HELLO world");
        (void)aar_file_close(fd);
    }
    conf_buf_str(&t.note, "READ reads, WRITE overwrites in place and keeps the rest [spec question 37]");
    conf_end(&t);
}

static void test_create_truncate(void)
{
    struct conf_test t;
    const char *missing = uniq("m10");
    int64_t size;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.10");
    fd = aar_file_open(missing, FILE_NEW);
    if (fd < 0) {
        fail_rc(&t, "CREATE|TRUNCATE of a missing file", fd);
    } else {
        (void)aar_file_close(fd);
        size = size_of(missing, &rc);
        conf_check(&t, size == 0, "the created file is not empty");
        rc = put(missing, "abc", 3);
        expect(&t, rc, AAR_OK, "writing into the created file");
        fd = aar_file_open(missing, FILE_NEW);
        if (fd < 0) {
            fail_rc(&t, "CREATE|TRUNCATE of an existing file", fd);
        } else {
            (void)aar_file_close(fd);
            size = size_of(missing, &rc);
            conf_check(&t, size == 0, "CREATE|TRUNCATE did not empty the existing file");
        }
    }
    conf_buf_str(&t.note, "a missing file is created, an existing one is emptied");
    conf_end(&t);
}

static void test_create(void)
{
    struct conf_test t;
    const char *file = sc("t11.tmp");
    int64_t size;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.11");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    fd = aar_file_open(file, AAR_FILE_WRITE | AAR_FILE_CREATE);
    if (fd < 0) {
        fail_rc(&t, "CREATE of an existing file", fd);
    } else {
        (void)aar_file_close(fd);
        size = size_of(file, &rc);
        if (size != 3 && conf_fail_first(&t))
            conf_buf_str(&t.why, "CREATE alone changed the contents of an existing file");
    }
    conf_buf_str(&t.note, "CREATE keeps the contents of an existing file");
    conf_end(&t);
}

static void test_truncate(void)
{
    struct conf_test t;
    const char *file = sc("t12.tmp");
    const char *missing = uniq("m12");
    aar_stat st;
    int64_t size;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.12");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    fd = aar_file_open(file, AAR_FILE_WRITE | AAR_FILE_TRUNCATE);
    if (fd < 0) {
        fail_rc(&t, "TRUNCATE of an existing file", fd);
    } else {
        (void)aar_file_close(fd);
        size = size_of(file, &rc);
        conf_check(&t, size == 0, "TRUNCATE did not empty the file");
    }
    fd = aar_file_open(missing, AAR_FILE_WRITE | AAR_FILE_TRUNCATE);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_NOENT, "TRUNCATE of a missing file");
    expect(&t, aar_file_stat(missing, &st), AAR_E_NOENT, "stat after a TRUNCATE of a missing file");
    conf_buf_str(&t.note, "TRUNCATE empties a file and does not create one");
    conf_end(&t);
}

static void test_excl(void)
{
    struct conf_test t;
    const char *file = sc("t13.tmp");
    const char *missing = uniq("m13");
    uint8_t buf[8];
    uint32_t n = 0;
    aar_stat st;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.13");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    fd = aar_file_open(file, AAR_FILE_WRITE | AAR_FILE_CREATE | AAR_FILE_EXCL);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_EXIST, "CREATE|EXCL of an existing file");
    fd = aar_file_open(file, AAR_FILE_READ);
    if (fd >= 0) {
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        conf_check(&t, rc == AAR_OK && n == 3 && buf[0] == 'a' && buf[2] == 'c',
                   "the file was touched by a refused exclusive create");
        (void)aar_file_close(fd);
    }
    fd = aar_file_open(missing, AAR_FILE_WRITE | AAR_FILE_CREATE | AAR_FILE_EXCL);
    if (fd < 0) {
        fail_rc(&t, "CREATE|EXCL of a missing file", fd);
    } else {
        (void)aar_file_close(fd);
        expect(&t, aar_file_stat(missing, &st), AAR_OK, "stat of the exclusively created file");
    }
    conf_buf_str(&t.note, "EXCL fails with AAR_E_EXIST and touches nothing, or creates");
    conf_end(&t);
}

static void test_bad_flags(void)
{
    struct conf_test t;
    const char *file = sc("t14.tmp");
    const char *missing = uniq("m15");
    aar_stat st;
    int64_t size;
    int fd;
    int rc;

    rc = put(file, "abc", 3);

    conf_begin(&t, "C-FILE.14");
    fd = aar_file_open(file, 0);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, rc == AAR_OK ? fd : rc, AAR_E_INVAL, "open with flags 0");
    conf_buf_str(&t.note, "flags 0 is AAR_E_INVAL");
    conf_end(&t);

    conf_begin(&t, "C-FILE.15");
    fd = aar_file_open(missing, AAR_FILE_READ | AAR_FILE_CREATE);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_INVAL, "READ|CREATE");
    expect(&t, aar_file_stat(missing, &st), AAR_E_NOENT, "stat after READ|CREATE (the file was created)");
    fd = aar_file_open(file, AAR_FILE_READ | AAR_FILE_TRUNCATE);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_INVAL, "READ|TRUNCATE");
    size = size_of(file, &rc);
    conf_check(&t, size == 3, "READ|TRUNCATE emptied the file");
    fd = aar_file_open(file, AAR_FILE_CREATE | AAR_FILE_TRUNCATE);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_INVAL, "CREATE|TRUNCATE without WRITE");
    conf_buf_str(&t.note, "CREATE and TRUNCATE without WRITE are AAR_E_INVAL and do nothing");
    conf_end(&t);

    conf_begin(&t, "C-FILE.16");
    fd = aar_file_open(file, AAR_FILE_WRITE | AAR_FILE_EXCL);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_INVAL, "WRITE|EXCL");
    fd = aar_file_open(file, AAR_FILE_READ | AAR_FILE_EXCL);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_INVAL, "READ|EXCL");
    conf_buf_str(&t.note, "EXCL without CREATE is AAR_E_INVAL");
    conf_end(&t);

    conf_begin(&t, "C-FILE.17");
    fd = aar_file_open(file, AAR_FILE_READ | 32u);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_INVAL, "READ with bit 32");
    fd = aar_file_open(file, AAR_FILE_READ | 0x80000000u);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_INVAL, "READ with bit 31");
    fd = aar_file_open(file, AAR_FILE_READ | 0x100u);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_INVAL, "READ with bit 256");
    conf_buf_str(&t.note, "unknown flag bits are AAR_E_INVAL");
    conf_end(&t);

    conf_begin(&t, "C-FILE.18");
    missing = uniq("m18");
    fd = aar_file_open(missing, AAR_FILE_READ);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_NOENT, "READ of a missing file");
    fd = aar_file_open(missing, AAR_FILE_WRITE);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_NOENT, "WRITE of a missing file");
    fd = aar_file_open(missing, FILE_RW);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect(&t, fd, AAR_E_NOENT, "READ|WRITE of a missing file");
    expect(&t, aar_file_stat(missing, &st), AAR_E_NOENT, "stat after the opens (a file was created)");
    conf_buf_str(&t.note, "a missing file without CREATE is AAR_E_NOENT");
    conf_end(&t);
}

/* ---- C-FILE.19, C-FILE.20: seek and read ----------------------------------------------------------- */

static void test_seek(void)
{
    struct conf_test t;
    const char *file = sc("t19.tmp");
    uint8_t buf[4];
    uint32_t n = 99;
    int64_t pos = -77;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.19");
    rc = put_small(file);
    fd = rc == AAR_OK ? aar_file_open(file, AAR_FILE_READ) : rc;
    if (fd < 0) {
        fail_rc(&t, "creating and opening the scratch file", fd);
        conf_end(&t);
        return;
    }
#define SEEK(off, whence, want, what)                                                       \
    do {                                                                                    \
        pos = -77;                                                                          \
        rc = aar_file_seek(fd, (off), (whence), &pos);                                      \
        if (rc != AAR_OK || pos != (want)) {                                                \
            if (conf_fail_first(&t)) {                                                      \
                conf_buf_str(&t.why, what);                                                 \
                conf_buf_str(&t.why, ": result ");                                          \
                conf_buf_i64(&t.why, rc);                                                   \
                conf_buf_str(&t.why, ", position ");                                        \
                conf_buf_i64(&t.why, pos);                                                  \
            }                                                                               \
        }                                                                                   \
    } while (0)
    SEEK(10, AAR_SEEK_SET, 10, "SET 10");
    SEEK(5, AAR_SEEK_CUR, 15, "CUR +5");
    SEEK(-3, AAR_SEEK_CUR, 12, "CUR -3");
    SEEK(0, AAR_SEEK_END, 100, "END 0");
    SEEK(-10, AAR_SEEK_END, 90, "END -10");
    SEEK(0, AAR_SEEK_SET, 0, "SET 0");
#undef SEEK
    rc = aar_file_seek(fd, -1, AAR_SEEK_SET, &pos);
    expect(&t, rc, AAR_E_INVAL, "seek to -1");
    rc = aar_file_seek(fd, -1, AAR_SEEK_CUR, &pos);
    expect(&t, rc, AAR_E_INVAL, "seek to before the start from position 0");
    rc = aar_file_read(fd, buf, 1, &n);
    conf_check(&t, rc == AAR_OK && n == 1 && buf[0] == conf_file_small(0),
               "a refused seek moved the position");
    /* A seek past the end: AmigaDOS refuses it on a handle that cannot grow the
     * file (AAR_E_INVAL); where it works, a read there finds the end [spec question 55]. */
    rc = aar_file_seek(fd, 1000, AAR_SEEK_SET, &pos);
    if (rc == AAR_OK) {
        conf_check(&t, pos == 1000, "a seek past the end did not return the new position");
        n = 99;
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        conf_check(&t, rc == AAR_OK && n == 0, "a read past the end is not AAR_OK with 0 bytes");
    } else if (rc != AAR_E_INVAL) {
        fail_rc(&t, "seek past the end", rc);
    }
    (void)aar_file_close(fd);
    conf_buf_str(&t.note, "SET, CUR and END return the position, before the start is AAR_E_INVAL, past the end is accepted or AAR_E_INVAL");
    conf_end(&t);
}

static void test_read(void)
{
    struct conf_test t;
    const char *file = sc("t20.tmp");
    uint8_t buf[300];
    uint32_t n = 0;
    uint32_t i;
    int64_t pos;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.20");
    rc = put_small(file);
    fd = rc == AAR_OK ? aar_file_open(file, AAR_FILE_READ) : rc;
    if (fd < 0) {
        fail_rc(&t, "creating and opening the scratch file", fd);
        conf_end(&t);
        return;
    }
    rc = aar_file_read(fd, buf, sizeof buf, &n);
    if (rc != AAR_OK || n != 100u) {
        fail_rc(&t, "a read of 300 bytes from a file of 100 (n must be 100): result", rc);
    } else {
        for (i = 0; i < 100u; i++) {
            if (buf[i] != conf_file_small(i)) {
                conf_check(&t, 0, "the bytes read are not the bytes of the file");
                break;
            }
        }
    }
    n = 99;
    rc = aar_file_read(fd, buf, sizeof buf, &n);
    conf_check(&t, rc == AAR_OK && n == 0, "a read at the end is not AAR_OK with 0 bytes");
    rc = aar_file_seek(fd, 90, AAR_SEEK_SET, &pos);
    n = 0;
    rc = aar_file_read(fd, buf, 50, &n);
    conf_check(&t, rc == AAR_OK && n == 10u && buf[0] == conf_file_small(90),
               "a read of 50 bytes 10 from the end did not return 10");
    (void)aar_file_close(fd);
    conf_buf_str(&t.note, "n is the byte count: 100, 0 at the end, what is left");
    conf_end(&t);
}

/* ---- C-FILE.21, C-FILE.22: 1 MiB in one call -------------------------------------------------------- */

static void test_write_big(void)
{
    struct conf_test t;
    const char *file = sc("out1m.tmp");
    uint32_t n = 0;
    uint32_t i;
    uint32_t res[4] = { 0, 0, 0, 0 };
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.22");
    for (i = 0; i < CONF_FILE_PATTERN_BYTES; i++)
        mega[i] = conf_file_pat2(i);
    fd = aar_file_open(file, FILE_NEW | AAR_FILE_READ);
    if (fd < 0) {
        fail_rc(&t, "creating the 1 MiB file", fd);
        conf_end(&t);
        return;
    }
    rc = aar_file_write(fd, mega, CONF_FILE_PATTERN_BYTES, &n);
    if ((rc != AAR_OK || n != CONF_FILE_PATTERN_BYTES) && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "one write of 1 MiB gave ");
        conf_buf_i64(&t.why, rc);
        conf_buf_str(&t.why, " and a count of ");
        conf_buf_u64(&t.why, n);
    }
    (void)aar_file_close(fd);
    for (i = 0; i < CONF_FILE_PATTERN_BYTES; i++)
        mega[i] = 0;
    fd = aar_file_open(file, AAR_FILE_READ);
    if (fd >= 0) {
        n = 0;
        rc = aar_file_read(fd, mega, CONF_FILE_PATTERN_BYTES, &n);
        if (rc != AAR_OK || n != CONF_FILE_PATTERN_BYTES) {
            conf_check(&t, 0, "the 1 MiB file does not read back in one call");
        } else {
            for (i = 0; i < CONF_FILE_PATTERN_BYTES; i++) {
                if (mega[i] != conf_file_pat2(i)) {
                    if (conf_fail_first(&t)) {
                        conf_buf_str(&t.why, "byte ");
                        conf_buf_u64(&t.why, i);
                        conf_buf_str(&t.why, " of the 1 MiB file reads back wrong");
                    }
                    break;
                }
            }
        }
        (void)aar_file_close(fd);
    }
    if (peer && !t.failed) {
        rc = rpc(CONF_FILE_CHECK, 2, res);
        if (rc != AAR_OK || res[0] != 0) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "the runner finds other bytes in the file than the app wrote (");
                conf_buf_i64(&t.why, (int32_t)res[0]);
                conf_buf_str(&t.why, ", ");
                conf_buf_u64(&t.why, res[1]);
                conf_buf_chr(&t.why, ')');
            }
        }
    }
    conf_buf_str(&t.note, peer ? "1 MiB in one call, read back and found by the runner"
                               : "1 MiB in one call, read back");
    conf_end(&t);
}

static void test_read_big(void)
{
    struct conf_test t;
    const char *file = peer ? fx(CONF_FILE_PATTERN_NAME) : 0;
    uint32_t n = 0;
    uint32_t i;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.21");
    if (!file) {
        /* The host: the same pattern, written by the app first. */
        for (i = 0; i < CONF_FILE_PATTERN_BYTES; i++)
            mega[i] = conf_file_pat(i);
        file = sc("pattern.tmp");
        rc = put(file, mega, CONF_FILE_PATTERN_BYTES);
        if (rc != AAR_OK) {
            fail_rc(&t, "writing the 1 MiB file", rc);
            conf_end(&t);
            return;
        }
        for (i = 0; i < CONF_FILE_PATTERN_BYTES; i++)
            mega[i] = 0;
    }
    fd = aar_file_open(file, AAR_FILE_READ);
    if (fd < 0) {
        fail_rc(&t, "open of the 1 MiB file", fd);
        conf_end(&t);
        return;
    }
    rc = aar_file_read(fd, mega, CONF_FILE_PATTERN_BYTES, &n);
    if (rc != AAR_OK || n != CONF_FILE_PATTERN_BYTES) {
        if (conf_fail_first(&t)) {
            conf_buf_str(&t.why, "one read of 1 MiB gave ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, " and a count of ");
            conf_buf_u64(&t.why, n);
        }
    } else {
        for (i = 0; i < CONF_FILE_PATTERN_BYTES; i++) {
            if (mega[i] != conf_file_pat(i)) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "byte ");
                    conf_buf_u64(&t.why, i);
                    conf_buf_str(&t.why, " of the 1 MiB file is wrong");
                }
                break;
            }
        }
    }
    (void)aar_file_close(fd);
    conf_buf_str(&t.note, "one read returned all 1 MiB");
    conf_end(&t);
}

/* ---- C-FILE.23 to C-FILE.25: stat and lists ---------------------------------------------------------------- */

static void test_stat(void)
{
    struct conf_test t;
    aar_stat st;
    int rc;

    conf_begin(&t, "C-FILE.23");
    if (peer) {
        rc = aar_file_stat(fx(CONF_FILE_TEXT_NAME), &st);
        if (rc != AAR_OK || st.size != slen(CONF_FILE_TEXT) || st.mode != AAR_MODE_FILE)
            fail_rc(&t, "stat of the text fixture (size and mode 1 expected): result", rc);
        rc = aar_file_stat(fx(CONF_FILE_DIR5_NAME), &st);
        if (rc != AAR_OK || st.mode != AAR_MODE_DIR)
            fail_rc(&t, "stat of the directory fixture (mode 2 expected): result", rc);
    } else {
        rc = put_small(sc("t23.tmp"));
        if (rc == AAR_OK)
            rc = aar_file_stat(sc("t23.tmp"), &st);
        if (rc != AAR_OK || st.size != 100u || st.mode != AAR_MODE_FILE)
            fail_rc(&t, "stat of the scratch file (size 100, mode 1 expected): result", rc);
    }
    rc = aar_file_stat(sc("t23-missing.tmp"), &st);
    expect(&t, rc, AAR_E_NOENT, "stat of a missing path");
    conf_buf_str(&t.note, "size, mode 1 for a file, mode 2 for a directory, AAR_E_NOENT");
    conf_end(&t);
}

static void test_mtime(void)
{
    struct conf_test t;
    aar_stat st;
    int rc;

    if (!peer) {
        need_fixtures("C-FILE.24");
        return;
    }
    conf_begin(&t, "C-FILE.24");
    rc = aar_file_stat(fx(CONF_FILE_DATED_NAME), &st);
    if (rc != AAR_OK) {
        fail_rc(&t, "stat of the dated fixture", rc);
    } else if (st.mtime != CONF_FILE_MTIME && st.mtime != 0 && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "mtime is ");
        conf_buf_i64(&t.why, st.mtime);
        conf_buf_str(&t.why, ", the fixture is dated 1000000000 and 0 means no date");
    }
    conf_buf_str(&t.note, "mtime ");
    conf_buf_i64(&t.note, rc == AAR_OK ? st.mtime : -1);
    conf_end(&t);
}

static void test_list(void)
{
    static const char *const want[CONF_FILE_DIR5_COUNT] = CONF_FILE_DIR5_NAMES;
    struct conf_test t;
    aar_dirent ent;
    uint32_t seen[CONF_FILE_DIR5_COUNT];
    uint32_t count = 0;
    uint32_t i;
    int list;
    int rc;

    if (!peer) {
        need_fixtures("C-FILE.25");
        return;
    }
    conf_begin(&t, "C-FILE.25");
    for (i = 0; i < CONF_FILE_DIR5_COUNT; i++)
        seen[i] = 0;
    list = aar_file_list_open(fx(CONF_FILE_DIR5_NAME));
    if (list < AAR_FD_BASE) {
        fail_rc(&t, "list open of the directory fixture", list);
        conf_end(&t);
        return;
    }
    while (count < 64u) {
        uint32_t k;
        int known = 0;

        rc = aar_file_list_next(list, &ent);
        if (rc != AAR_OK)
            break;
        count++;
        if (slen(ent.name) > AAR_NAME_MAX && conf_fail_first(&t))
            conf_buf_str(&t.why, "a name is longer than 255 bytes");
        if (ent.mode != AAR_MODE_FILE && conf_fail_first(&t))
            conf_buf_str(&t.why, "an entry of the directory of files has not mode 1");
        for (k = 0; k < CONF_FILE_DIR5_COUNT; k++) {
            if (seq(ent.name, want[k])) {
                known = 1;
                if (seen[k]++ && conf_fail_first(&t))
                    conf_buf_str(&t.why, "an entry came twice");
            }
        }
        if (!known && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "an unknown entry: ");
            conf_buf_str(&t.why, ent.name);
        }
    }
    if (rc != AAR_E_NOENT && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the list ended with ");
        conf_buf_i64(&t.why, rc);
        conf_buf_str(&t.why, ", not AAR_E_NOENT");
    }
    if (count != CONF_FILE_DIR5_COUNT && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the list holds ");
        conf_buf_u64(&t.why, count);
        conf_buf_str(&t.why, " entries, not 5");
    }
    (void)aar_file_list_close(list);
    rc = aar_file_list_open(fx("conf-no-such-dir"));
    if (rc >= 0)
        (void)aar_file_list_close(rc);
    expect(&t, rc, AAR_E_NOENT, "list open of a missing directory");
    conf_buf_str(&t.note, "5 entries once each, then AAR_E_NOENT");
    conf_end(&t);
}

/* ---- C-FILE.29, C-FILE.30: map_file ----------------------------------------------------------------------------- */

static void test_map(void)
{
    struct conf_test t;
    const char *file = peer ? fx(CONF_FILE_PATTERN_NAME) : sc("pattern.tmp");
    const void *data = 0;
    uint64_t bytes = 0;
    uint8_t buf[16];
    uint32_t n = 0;
    uint32_t i;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.29");
    rc = aar_map_file(file, &data, &bytes);
    if (rc == AAR_E_NOSYS) {
        conf_buf_str(&t.note, "aar_map_file is not provided (AAR_E_NOSYS is allowed)");
        conf_end(&t);
        return;
    }
    if (rc != AAR_OK) {
        fail_rc(&t, "aar_map_file of the 1 MiB file", rc);
        conf_end(&t);
        return;
    }
    if (bytes != CONF_FILE_PATTERN_BYTES && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the mapping is ");
        conf_buf_u64(&t.why, bytes);
        conf_buf_str(&t.why, " bytes, not 1 MiB");
    } else {
        const uint8_t *p = (const uint8_t *)data;

        for (i = 0; i < CONF_FILE_PATTERN_BYTES; i++) {
            if (p[i] != conf_file_pat(i)) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "byte ");
                    conf_buf_u64(&t.why, i);
                    conf_buf_str(&t.why, " of the mapping is wrong");
                }
                break;
            }
        }
        /* The buffer stays valid while other calls run. */
        fd = aar_file_open(sc("t29.tmp"), FILE_NEW);
        if (fd >= 0) {
            (void)aar_file_write(fd, "x", 1, &n);
            (void)aar_file_close(fd);
        }
        fd = aar_file_open(file, AAR_FILE_READ);
        if (fd >= 0) {
            (void)aar_file_read(fd, buf, sizeof buf, &n);
            (void)aar_file_close(fd);
        }
        for (i = 0; i < CONF_FILE_PATTERN_BYTES; i += 4093u) {
            if (p[i] != conf_file_pat(i)) {
                conf_check(&t, 0, "the mapping changed while other calls ran");
                break;
            }
        }
    }
    aar_unmap_file(data);
    conf_buf_str(&t.note, "the mapping is the 1 MiB pattern and stays valid until aar_unmap_file");
    conf_end(&t);
}

static void test_map_zz9000(void)
{
    struct conf_test t;
    const void *data = 0;
    const void *big = 0;
    uint64_t bytes = 0;
    uint64_t used0;
    aar_info now;
    int rc;

    conf_begin(&t, "C-FILE.30");
    if (!AAR_API_COVERS(aar_api_ptr, map_file) || aar_api_ptr->map_file == 0)
        conf_check(&t, 0, "aar_map_file is not in the table");
    now.size = sizeof now;
    (void)aar_get_info(&now);
    used0 = now.shared_used;
    rc = aar_map_file(fx(CONF_FILE_PATTERN_NAME), &data, &bytes);
    if (rc != AAR_OK) {
        fail_rc(&t, "aar_map_file of the 1 MiB file", rc);
    } else {
        (void)aar_get_info(&now);
        if (now.shared_used < used0 + CONF_FILE_PATTERN_BYTES && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "shared_used rose from ");
            conf_buf_u64(&t.why, used0);
            conf_buf_str(&t.why, " to ");
            conf_buf_u64(&t.why, now.shared_used);
            conf_buf_str(&t.why, " for a file of 1 MiB");
        }
        aar_unmap_file(data);
    }
    rc = aar_map_file(fx(CONF_FILE_BIG_NAME), &big, &bytes);
    if (rc == AAR_OK) {
        aar_unmap_file(big);
        conf_check(&t, 0, "a file over 16 MiB was mapped");
    } else {
        expect(&t, rc, AAR_E_NOSPC, "aar_map_file of a file over 16 MiB");
    }
    conf_buf_str(&t.note, "shared_used rises while mapped, 16 MiB and one byte is AAR_E_NOSPC");
    conf_end(&t);
}

/* ---- C-FILE.6: after a stop request ------------------------------------------------------------------------------- */

static void test_stop(void)
{
    struct conf_test t;
    const char *keep = fx("stopkeep.tmp");
    const char *wfile = fx("stopw.tmp");
    const char *created = fx("afterstop.tmp");
    uint8_t buf[128];
    uint32_t n = 0;
    uint32_t len = slen(CONF_FILE_TEXT);
    int wfd;
    int fd;
    int rc;

    if (!peer) {
        conf_skip("C-FILE.6", "host: no runner to stop the app");
        return;
    }
    conf_begin(&t, "C-FILE.6");
    rc = put(keep, "keep", 4);
    wfd = aar_file_open(wfile, FILE_NEW);
    if (rc != AAR_OK || wfd < 0) {
        fail_rc(&t, "preparing the files for the stop test", rc != AAR_OK ? rc : wfd);
        conf_end(&t);
        return;
    }
    rc = conf_peer_send(CONF_MSG_STOP);
    if (rc != AAR_OK) {
        conf_check(&t, 0, "the stop request message was not accepted");
        conf_end(&t);
        return;
    }
    aar_sleep_us(5000000u);
    conf_check(&t, aar_should_stop() == 1, "the client did not stop the app");
    /* After the stop request a parked call can be ended by the stop (AAR_E_IO),
     * and the call after it can find the abandoned request still open (AAR_E_BUSY):
     * every answer is negative. What the client did with the requests is judged
     * by the runner when the app has ended [spec question 56]. */
    fd = aar_file_open(created, AAR_FILE_WRITE | AAR_FILE_CREATE);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect_neg(&t, fd, "a create after the stop request");
    fd = aar_file_open(keep, AAR_FILE_WRITE | AAR_FILE_TRUNCATE);
    if (fd >= 0)
        (void)aar_file_close(fd);
    expect_neg(&t, fd, "a truncate after the stop request");
    n = 0;
    rc = aar_file_write(wfd, "data", 4, &n);
    if (rc >= 0 && n > 0 && conf_fail_first(&t))
        conf_buf_str(&t.why, "a write after the stop request took bytes");
    /* A read is served (then it is right) or ended by the stop. */
    fd = aar_file_open(fx(CONF_FILE_TEXT_NAME), AAR_FILE_READ);
    if (fd >= 0) {
        n = 0;
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        if (rc == AAR_OK && (n != len || buf[0] != (uint8_t)CONF_FILE_TEXT[0]))
            conf_check(&t, 0, "a read after the stop request returned other bytes");
        (void)aar_file_close(fd);
    }
    (void)aar_file_close(wfd);
    conf_buf_str(&t.note, "create, truncate and write after the stop request give negative codes");
    conf_end(&t);
}

/* ---- The variant abs ------------------------------------------------------------------------------------------------ */

static void run_abs(const char *abs)
{
    struct conf_test t;
    uint8_t buf[8];
    uint32_t n = 0;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.5");
    fd = aar_file_open(abs, AAR_FILE_READ);
    if (fd < 0) {
        fail_rc(&t, "open of a path with the volume of the root, with AAR_SERVEF_ABS_PATHS", fd);
    } else {
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        conf_check(&t, rc == AAR_OK && n == sizeof buf && buf[0] == (uint8_t)CONF_FILE_TEXT[0],
                   "the file opened by its volume path is not the fixture");
        (void)aar_file_close(fd);
    }
    conf_buf_str(&t.note, "with AAR_SERVEF_ABS_PATHS a path with the root's volume opens");
    conf_end(&t);
}

/* ---- The variant stall ------------------------------------------------------------------------------------------------- */

static void run_stall(void)
{
    uint32_t out[4];
    aar_stat st;
    uint64_t t0;
    uint32_t ms;
    uint32_t bad = 0;
    uint32_t rounds = 0;
    uint32_t i;
    int rc;

    t0 = aar_time_us();
    rc = aar_file_open(CONF_FILE_STALL_A, AAR_FILE_READ);
    ms = (uint32_t)((aar_time_us() - t0) / 1000u);
    (void)conf_file_send(CONF_FILE_STALL, 1, (uint32_t)rc, ms, 0);
    if (conf_file_wait(CONF_FILE_GO, 20000000u, out) != AAR_OK)
        return;
    for (i = 0; i < CONF_FILE_STALL_ROUNDS; i++) {
        int fd = aar_file_open(CONF_FILE_STALL_B, AAR_FILE_READ);

        if (fd < 0) {
            bad = (uint32_t)fd;
            break;
        }
        rc = aar_file_close(fd);
        if (rc != AAR_OK) {
            bad = (uint32_t)rc;
            break;
        }
        rounds++;
    }
    (void)conf_file_send(CONF_FILE_STALL, 2, bad, 0, rounds);
    (void)conf_file_send(CONF_FILE_STALL, 3, 0, 0, 0);
    t0 = aar_time_us();
    rc = aar_file_stat(CONF_FILE_STALL_C, &st);
    ms = (uint32_t)((aar_time_us() - t0) / 1000u);
    (void)conf_file_send(CONF_FILE_STALL, 4, (uint32_t)rc, ms, 0);
}

/* ---- The variant leak -------------------------------------------------------------------------------------------------- */

static void run_leak(void)
{
    (void)aar_file_open(fx("leak-a.tmp"), FILE_NEW);
    (void)aar_file_open(fx("leak-b.tmp"), FILE_NEW | AAR_FILE_READ);
    (void)aar_file_list_open(fx(CONF_FILE_LEAKDIR_NAME));
    (void)aar_file_list_open(fx(CONF_FILE_DIR5_NAME));
    /* ends with the files and the lists open */
}

/* ---- main --------------------------------------------------------------------------------------------------------------- */

int aar_main(int argc, char **argv)
{
    const char *variant = conf_arg(argc, argv, "conf:variant");
    const char *abs = conf_arg(argc, argv, "conf:abs");

    peer = conf_have_peer(argc, argv);
    info.size = sizeof info;
    (void)aar_get_info(&info);
    if (!conf_arg_u32(argc, argv, "info:platform_id", &platform_id))
        platform_id = 0;

    if (variant && seq(variant, CONF_FILE_VARIANT_STALL)) {
        run_stall();
        return 0;
    }
    if (variant && seq(variant, CONF_FILE_VARIANT_LEAK)) {
        run_leak();
        return 0;
    }
    if (variant && seq(variant, CONF_FILE_VARIANT_ABS)) {
        if (abs)
            run_abs(abs);
        return conf_finish("C-FILE");
    }

    test_long_path();
    test_root();
    test_policy();
    test_many();
    test_bad_ids();
    test_modes();
    test_create_truncate();
    test_create();
    test_truncate();
    test_excl();
    test_bad_flags();
    test_seek();
    test_read();
    test_write_big();
    test_read_big();
    test_stat();
    test_mtime();
    test_list();
    test_map();
    if (platform_id == AAR_PLATFORM_ZZ9000)
        test_map_zz9000();
    test_stop();
    return conf_finish("C-FILE");
}
