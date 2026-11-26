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
    } else if (fd != AAR_E_INVAL) {
        fail_rc(&t, "open of a path of 256 bytes (AAR_E_INVAL is due)", fd);
    } else {
        conf_buf_str(&t.note, "a path of 256 bytes, which exists, is refused with AAR_E_INVAL");
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
    conf_buf_str(&t.note, "READ reads, WRITE overwrites in place and keeps the rest (the wrong direction is C-FILE.37)");
    conf_end(&t);
}

/* ---- C-FILE.37: an id is used only for the direction it was opened for ------------------------------ */

static void test_directions(void)
{
    struct conf_test t;
    const char *file = sc("t37.tmp");
    uint8_t buf[32];
    uint32_t n = 77;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.37");
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
        rc = aar_file_write(fd, "HELLO", 5, &n);
        expect(&t, rc, AAR_E_BADF, "a write through a READ id");
        (void)aar_file_close(fd);
    }
    fd = aar_file_open(file, AAR_FILE_WRITE);
    if (fd < 0) {
        fail_rc(&t, "open WRITE of an existing file", fd);
    } else {
        n = 0;
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        expect(&t, rc, AAR_E_BADF, "a read through a WRITE id");
        (void)aar_file_close(fd);
    }
    /* The refused calls changed nothing. */
    fd = aar_file_open(file, AAR_FILE_READ);
    if (fd >= 0) {
        n = 0;
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        conf_check(&t, rc == AAR_OK && n == 11 && buf[0] == 'h' && buf[4] == 'o' && buf[10] == 'd',
                   "after the refused calls the file is not hello world");
        (void)aar_file_close(fd);
    }
    conf_buf_str(&t.note, "a write through READ and a read through WRITE are AAR_E_BADF; the file is unchanged");
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
     * file (AAR_E_INVAL); where it works, a read there finds the end [spec question 56]. */
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

/* ---- C-FILE.38 to C-FILE.48: delete, rename, directories, dates, protection, truncate, volume --------------- */

/* A client that does not serve a call answers AAR_E_NOSYS (spec 7.1). The row then passes and says so;
 * C-FILE.48 is where that is judged. Returns 1 when rc is that answer. */
static int not_served(struct conf_test *t, int rc, const char *call)
{
    if (rc != AAR_E_NOSYS)
        return 0;
    conf_buf_str(&t->note, call);
    conf_buf_str(&t->note, ": AAR_E_NOSYS, the client does not serve it");
    return 1;
}

/* dir/name in a path slot of its own. */
static const char *join(const char *dir, const char *name)
{
    char *p = path_slot();

    scat(p, dir);
    scat(p, "/");
    scat(p, name);
    return p;
}

/* Fails the test when the path exists. */
static void expect_gone(struct conf_test *t, const char *path, const char *what)
{
    aar_stat st;
    int rc = aar_file_stat(path, &st);

    if (rc != AAR_E_NOENT)
        fail_rc(t, what, rc);
}

/* Fails the test when the path is not a file of that size (-1: any size). */
static void expect_file(struct conf_test *t, const char *path, int64_t size, const char *what)
{
    aar_stat st;
    int rc = aar_file_stat(path, &st);

    if (rc != AAR_OK || st.mode != AAR_MODE_FILE || (size >= 0 && (int64_t)st.size != size))
        fail_rc(t, what, rc);
}

static void expect_dir(struct conf_test *t, const char *path, const char *what)
{
    aar_stat st;
    int rc = aar_file_stat(path, &st);

    if (rc != AAR_OK || st.mode != AAR_MODE_DIR)
        fail_rc(t, what, rc);
}

/* C-FILE.38 */
static void test_delete(void)
{
    struct conf_test t;
    const char *file = uniq("d38f");
    const char *dir = uniq("d38d");
    const char *inner;
    int rc;

    conf_begin(&t, "C-FILE.38");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    rc = aar_file_delete(file);
    if (not_served(&t, rc, "file_delete")) {
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "file_delete of a file");
    expect_gone(&t, file, "stat after file_delete of a file");
    expect(&t, aar_file_delete(file), AAR_E_NOENT, "file_delete of a missing path");

    rc = aar_dir_create(dir);
    if (rc == AAR_OK) {
        expect(&t, aar_file_delete(dir), AAR_OK, "file_delete of an empty directory");
        expect_gone(&t, dir, "stat after file_delete of a directory");
        rc = aar_dir_create(dir);
    }
    if (rc == AAR_OK) {
        inner = join(dir, "in.tmp");
        rc = put(inner, "x", 1);
        if (rc != AAR_OK) {
            fail_rc(&t, "creating a file in the directory", rc);
        } else {
            expect(&t, aar_file_delete(dir), AAR_E_EXIST, "file_delete of a directory that is not empty");
            expect_dir(&t, dir, "stat of the directory after a refused file_delete");
            expect_file(&t, inner, 1, "stat of the file in it after a refused file_delete");
            (void)aar_file_delete(inner);
        }
        (void)aar_file_delete(dir);
    }
    conf_buf_str(&t.note, "a file and an empty directory go; a missing path is AAR_E_NOENT; a full directory is AAR_E_EXIST");
    conf_end(&t);
}

/* C-FILE.39 */
static void test_delete_protected(void)
{
    struct conf_test t;
    const char *file = uniq("d39");
    int rc;

    conf_begin(&t, "C-FILE.39");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    rc = aar_file_set_protect(file, AAR_PROT_DELETE);
    if (not_served(&t, rc, "file_set_protect")) {
        (void)aar_file_delete(file);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "file_set_protect with AAR_PROT_DELETE");
    expect(&t, aar_file_delete(file), AAR_E_ACCES, "file_delete of a delete-protected file");
    expect_file(&t, file, 3, "stat of the delete-protected file after the refused file_delete");
    (void)aar_file_set_protect(file, 0);
    expect(&t, aar_file_delete(file), AAR_OK, "file_delete after the protection was cleared");
    conf_buf_str(&t.note, "a delete-protected file is AAR_E_ACCES and stays");
    conf_end(&t);
}

/* C-FILE.40 */
static void test_delete_busy(void)
{
    struct conf_test t;
    const char *file = uniq("d40");
    int fd;
    int rc;

    if (!peer) {
        /* A POSIX host deletes a file that is open; AmigaDOS refuses it. */
        conf_skip("C-FILE.40", "host: the host runner deletes open files as POSIX does");
        return;
    }
    conf_begin(&t, "C-FILE.40");
    rc = put(file, "abc", 3);
    fd = rc == AAR_OK ? aar_file_open(file, FILE_RW) : rc;
    if (fd < 0) {
        fail_rc(&t, "preparing the open file", fd);
        conf_end(&t);
        return;
    }
    rc = aar_file_delete(file);
    if (not_served(&t, rc, "file_delete")) {
        (void)aar_file_close(fd);
        (void)aar_file_delete(file);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_E_BUSY, "file_delete of a file that is open for writing");
    expect_file(&t, file, 3, "stat after the refused file_delete");
    (void)aar_file_close(fd);
    (void)aar_file_delete(file);
    conf_buf_str(&t.note, "a file in use is AAR_E_BUSY and stays");
    conf_end(&t);
}

/* C-FILE.41 */
static void test_rename(void)
{
    struct conf_test t;
    const char *a = uniq("r41a");
    const char *b = uniq("r41b");
    const char *c = uniq("r41c");
    const char *d = uniq("r41d");
    const char *d1 = uniq("r41e");
    const char *d2 = uniq("r41f");
    int rc;

    conf_begin(&t, "C-FILE.41");
    rc = put_small(a);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    rc = aar_file_rename(a, b, 0);
    if (not_served(&t, rc, "file_rename")) {
        (void)aar_file_delete(a);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "file_rename of a file");
    expect_gone(&t, a, "stat of the old name after file_rename");
    expect_file(&t, b, 100, "stat of the new name after file_rename");

    rc = aar_dir_create(d1);
    if (rc == AAR_OK) {
        expect(&t, aar_file_rename(d1, d2, 0), AAR_OK, "file_rename of a directory");
        expect_gone(&t, d1, "stat of the old directory name");
        expect_dir(&t, d2, "stat of the new directory name");
        (void)aar_file_delete(d2);
    }

    /* REPLACE deletes an existing target first. */
    rc = put(c, "abc", 3);
    if (rc == AAR_OK)
        rc = put(d, "0123456789", 10);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the files for REPLACE", rc);
    } else {
        expect(&t, aar_file_rename(c, d, AAR_RENAME_REPLACE), AAR_OK, "file_rename with REPLACE onto a file");
        expect_gone(&t, c, "stat of the source after REPLACE");
        expect_file(&t, d, 3, "stat of the target after REPLACE (it holds the source's bytes)");
    }
    (void)aar_file_delete(b);
    (void)aar_file_delete(c);
    (void)aar_file_delete(d);
    conf_buf_str(&t.note, "a file and a directory are renamed; REPLACE overwrites an existing target");
    conf_end(&t);
}

/* C-FILE.42 */
static void test_rename_errors(void)
{
    struct conf_test t;
    const char *x = uniq("r42x");
    const char *y = uniq("r42y");
    const char *z = uniq("r42z");
    const char *missing = uniq("r42m");
    int rc;

    conf_begin(&t, "C-FILE.42");
    rc = put(x, "xxx", 3);
    if (rc == AAR_OK)
        rc = put(y, "yyyyy", 5);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch files", rc);
        conf_end(&t);
        return;
    }
    rc = aar_file_rename(x, y, 0);
    if (not_served(&t, rc, "file_rename")) {
        (void)aar_file_delete(x);
        (void)aar_file_delete(y);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_E_EXIST, "file_rename onto an existing file without REPLACE");
    expect_file(&t, x, 3, "the source after the refused rename");
    expect_file(&t, y, 5, "the target after the refused rename");
    expect(&t, aar_file_rename(missing, z, 0), AAR_E_NOENT, "file_rename of a missing file");
    expect(&t, aar_file_rename(missing, z, AAR_RENAME_REPLACE), AAR_E_NOENT,
           "file_rename of a missing file with REPLACE");
    expect_gone(&t, z, "stat of the name a failed rename was asked for");
    expect(&t, aar_file_rename(x, z, 2u), AAR_E_INVAL, "file_rename with the flag bit 1");
    expect(&t, aar_file_rename(x, z, 0x80000000u), AAR_E_INVAL, "file_rename with the flag bit 31");
    expect_file(&t, x, 3, "the source after a rename with a bad flag");
    expect_gone(&t, z, "stat after a rename with a bad flag");
    (void)aar_file_delete(x);
    (void)aar_file_delete(y);
    conf_buf_str(&t.note, "EXIST without REPLACE, NOENT for a missing source, INVAL for an unknown flag; nothing moved");
    conf_end(&t);
}

/* C-FILE.52: a rename of an object onto itself changes nothing (spec 7.1). The client compares objects, so a
 * second spelling that it resolves to the same file (another letter case on AmigaDOS) is tried too; a client
 * whose names are case-sensitive has no such file and the spelling is left out. */
static void test_rename_self(void)
{
    struct conf_test t;
    const char *file = uniq("r52");
    const char *missing = uniq("r52m");
    char *up = path_slot();
    uint8_t buf[8];
    uint32_t n = 0;
    aar_stat st;
    uint32_t i;
    int fd;
    int rc;

    conf_begin(&t, "C-FILE.52");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    rc = aar_file_rename(file, file, 0);
    if (not_served(&t, rc, "file_rename")) {
        (void)aar_file_delete(file);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "file_rename onto itself");
    expect_file(&t, file, 3, "stat after file_rename onto itself");
    expect(&t, aar_file_rename(file, file, AAR_RENAME_REPLACE), AAR_OK, "file_rename onto itself with REPLACE");
    expect_file(&t, file, 3, "stat after file_rename onto itself with REPLACE");
    fd = aar_file_open(file, AAR_FILE_READ);
    if (fd >= 0) {
        rc = aar_file_read(fd, buf, sizeof buf, &n);
        conf_check(&t, rc == AAR_OK && n == 3u && buf[0] == 'a' && buf[2] == 'c',
                   "the file's bytes changed after a rename onto itself");
        (void)aar_file_close(fd);
    }
    /* The same path spelled in upper case. */
    for (i = 0; file[i] && i < 318u; i++)
        up[i] = (file[i] >= 'a' && file[i] <= 'z') ? (char)(file[i] - 'a' + 'A') : file[i];
    up[i] = '\0';
    if (!seq(up, file) && aar_file_stat(up, &st) == AAR_OK) {
        expect(&t, aar_file_rename(up, file, AAR_RENAME_REPLACE), AAR_OK, "file_rename of a second spelling onto the file, with REPLACE");
        expect(&t, aar_file_rename(file, up, 0), AAR_OK, "file_rename of the file onto a second spelling");
        expect_file(&t, file, 3, "stat after the renames between two spellings");
        conf_buf_str(&t.note, "a second spelling of the path was tried; ");
    }
    expect(&t, aar_file_rename(missing, missing, 0), AAR_E_NOENT, "file_rename of a missing path onto itself");
    expect(&t, aar_file_rename(missing, missing, AAR_RENAME_REPLACE), AAR_E_NOENT,
           "file_rename of a missing path onto itself with REPLACE");
    (void)aar_file_delete(file);
    conf_buf_str(&t.note, "onto itself is AAR_OK and nothing changes, with and without REPLACE");
    conf_end(&t);
}

/* C-FILE.43 */
static void test_dir_create(void)
{
    struct conf_test t;
    const char *dir = uniq("c43d");
    const char *file = uniq("c43f");
    const char *orphan = uniq("c43o");
    const char *inner;
    int rc;

    conf_begin(&t, "C-FILE.43");
    rc = aar_dir_create(dir);
    if (not_served(&t, rc, "dir_create")) {
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "dir_create of a new directory");
    expect_dir(&t, dir, "stat of the new directory (mode 2 expected)");
    expect(&t, aar_dir_create(dir), AAR_E_EXIST, "dir_create of an existing directory");
    inner = join(dir, "in.tmp");
    expect(&t, put(inner, "x", 1), AAR_OK, "creating a file in the new directory");
    rc = put(file, "x", 1);
    if (rc == AAR_OK)
        expect(&t, aar_dir_create(file), AAR_E_EXIST, "dir_create where a file has that name");
    expect(&t, aar_dir_create(join(orphan, "sub")), AAR_E_NOENT, "dir_create below a missing parent");
    expect_gone(&t, orphan, "stat of the missing parent after the refused dir_create");
    (void)aar_file_delete(inner);
    (void)aar_file_delete(dir);
    (void)aar_file_delete(file);
    conf_buf_str(&t.note, "a directory is made; an existing name is AAR_E_EXIST; a missing parent is AAR_E_NOENT");
    conf_end(&t);
}

/* C-FILE.44 */
static void test_set_date(void)
{
    struct conf_test t;
    const char *file = uniq("s44f");
    const char *dir = uniq("s44d");
    const char *missing = uniq("s44m");
    aar_stat st;
    int rc;

    conf_begin(&t, "C-FILE.44");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    rc = aar_file_set_date(file, CONF_FILE_MTIME);
    if (not_served(&t, rc, "file_set_date")) {
        (void)aar_file_delete(file);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "file_set_date to 1000000000");
    rc = aar_file_stat(file, &st);
    if (rc != AAR_OK || st.mtime != CONF_FILE_MTIME)
        fail_rc(&t, "mtime after file_set_date to 1000000000 (a stat result or the mtime is wrong): result", rc);
    expect(&t, aar_file_set_date(file, 1500000000), AAR_OK, "file_set_date to 1500000000");
    rc = aar_file_stat(file, &st);
    if (rc != AAR_OK || st.mtime != 1500000000)
        fail_rc(&t, "mtime after file_set_date to 1500000000: result", rc);
    /* A time before the filesystem's epoch is refused or kept: 1970 is before AmigaDOS's 1978. */
    rc = aar_file_set_date(file, 0);
    if (rc != AAR_OK && rc != AAR_E_INVAL)
        fail_rc(&t, "file_set_date to 0 (AAR_OK or AAR_E_INVAL expected)", rc);
    if (aar_dir_create(dir) == AAR_OK) {
        expect(&t, aar_file_set_date(dir, CONF_FILE_MTIME), AAR_OK, "file_set_date of a directory");
        rc = aar_file_stat(dir, &st);
        if (rc != AAR_OK || st.mtime != CONF_FILE_MTIME)
            fail_rc(&t, "mtime of the directory after file_set_date: result", rc);
        (void)aar_file_delete(dir);
    }
    expect(&t, aar_file_set_date(missing, CONF_FILE_MTIME), AAR_E_NOENT, "file_set_date of a missing path");
    (void)aar_file_delete(file);
    conf_buf_str(&t.note, "the date set is the date stat reports; a missing path is AAR_E_NOENT");
    conf_end(&t);
}

/* C-FILE.45 */
static void test_set_protect(void)
{
    struct conf_test t;
    const char *file = uniq("p45f");
    const char *missing = uniq("p45m");
    aar_stat st;
    int rc;

    conf_begin(&t, "C-FILE.45");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    rc = aar_file_stat(file, &st);
    if (rc == AAR_OK && st.protect > 0xFFu && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "stat reports protection bits above bit 7: ");
        conf_buf_u64(&t.why, st.protect);
    }
    rc = aar_file_set_protect(file, AAR_PROT_ARCHIVE | AAR_PROT_SCRIPT);
    if (not_served(&t, rc, "file_set_protect")) {
        (void)aar_file_delete(file);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "file_set_protect with ARCHIVE and SCRIPT");
    rc = aar_file_stat(file, &st);
    if (rc != AAR_OK || (st.protect & (AAR_PROT_ARCHIVE | AAR_PROT_SCRIPT)) !=
                            (AAR_PROT_ARCHIVE | AAR_PROT_SCRIPT))
        fail_rc(&t, "stat after file_set_protect with ARCHIVE and SCRIPT: result", rc);
    /* Bits 0 to 3 deny. */
    expect(&t, aar_file_set_protect(file, AAR_PROT_WRITE | AAR_PROT_DELETE), AAR_OK,
           "file_set_protect with WRITE and DELETE denied");
    rc = aar_file_stat(file, &st);
    if (rc != AAR_OK || (st.protect & 0xFu) != (AAR_PROT_WRITE | AAR_PROT_DELETE))
        fail_rc(&t, "stat after file_set_protect with WRITE and DELETE denied: result", rc);
    expect(&t, aar_file_set_protect(file, 0), AAR_OK, "file_set_protect with 0");
    rc = aar_file_stat(file, &st);
    if (rc != AAR_OK || (st.protect & 0xFu) != 0u)
        fail_rc(&t, "stat after file_set_protect with 0: result", rc);
    /* A bit that no AAR_PROT_* constant names. */
    expect(&t, aar_file_set_protect(file, 0x100u), AAR_E_INVAL, "file_set_protect with bit 8");
    expect(&t, aar_file_set_protect(file, 0x80000000u), AAR_E_INVAL, "file_set_protect with bit 31");
    expect(&t, aar_file_set_protect(missing, 0), AAR_E_NOENT, "file_set_protect of a missing path");
    (void)aar_file_delete(file);
    conf_buf_str(&t.note, "the bits set are the bits stat reports (0 to 3 deny); unknown bits are AAR_E_INVAL");
    conf_end(&t);
}

/* C-FILE.46 */
static void test_truncate_fd(void)
{
    struct conf_test t;
    const char *file = uniq("t46");
    uint8_t buf[16];
    uint32_t n = 0;
    int64_t pos = 0;
    int fd;
    int rc;
    int i;

    conf_begin(&t, "C-FILE.46");
    rc = put(file, "hello", 5);
    fd = rc == AAR_OK ? aar_file_open(file, FILE_RW) : rc;
    if (fd < 0) {
        fail_rc(&t, "preparing the scratch file", fd);
        conf_end(&t);
        return;
    }
    rc = aar_file_truncate(fd, 2);
    if (not_served(&t, rc, "file_truncate")) {
        (void)aar_file_close(fd);
        (void)aar_file_delete(file);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "file_truncate to 2");
    expect(&t, aar_file_seek(fd, 0, AAR_SEEK_END, &pos), AAR_OK, "seek to the end after file_truncate");
    conf_check(&t, pos == 2, "the length after file_truncate to 2 is not 2");
    expect(&t, aar_file_truncate(fd, 6), AAR_OK, "file_truncate to 6 (an extension)");
    expect(&t, aar_file_seek(fd, 0, AAR_SEEK_END, &pos), AAR_OK, "seek to the end after the extension");
    conf_check(&t, pos == 6, "the length after file_truncate to 6 is not 6");
    expect(&t, aar_file_seek(fd, 0, AAR_SEEK_SET, &pos), AAR_OK, "seek to the start");
    for (i = 0; i < 16; i++)
        buf[i] = 0xAAu;
    rc = aar_file_read(fd, buf, sizeof buf, &n);
    if (rc != AAR_OK || n != 6u || buf[0] != 'h' || buf[1] != 'e' || buf[2] != 0 || buf[3] != 0 ||
        buf[4] != 0 || buf[5] != 0)
        fail_rc(&t, "contents after the shorten and the extension (\"he\" and four zero bytes expected): result", rc);
    expect(&t, aar_file_truncate(fd, 0), AAR_OK, "file_truncate to 0");
    expect(&t, aar_file_seek(fd, 0, AAR_SEEK_END, &pos), AAR_OK, "seek to the end after the truncate to 0");
    conf_check(&t, pos == 0, "the length after file_truncate to 0 is not 0");
    (void)aar_file_close(fd);
    /* An id that was not opened for writing, and ids that are not open. */
    fd = aar_file_open(file, AAR_FILE_READ);
    if (fd >= 0) {
        expect(&t, aar_file_truncate(fd, 0), AAR_E_BADF, "file_truncate of an id opened for reading");
        (void)aar_file_close(fd);
    }
    expect(&t, aar_file_truncate(-1, 0), AAR_E_BADF, "file_truncate of id -1");
    expect(&t, aar_file_truncate(999, 0), AAR_E_BADF, "file_truncate of id 999");
    (void)aar_file_delete(file);
    conf_buf_str(&t.note, "shortens, extends with zero bytes, AAR_E_BADF for a read-only or unopened id");
    conf_end(&t);
}

/* C-FILE.47 */
static void test_volume_info(void)
{
    struct conf_test t;
    const char *file = uniq("v47");
    const char *missing = uniq("v47m");
    struct {
        aar_volume_info v;
        uint8_t tail[16];
    } big;
    uint8_t raw[sizeof(aar_volume_info) + 8];
    uint32_t i;
    int rc;

    conf_begin(&t, "C-FILE.47");
    rc = put(file, "abc", 3);
    if (rc != AAR_OK) {
        fail_rc(&t, "creating the scratch file", rc);
        conf_end(&t);
        return;
    }
    for (i = 0; i < sizeof big; i++)
        ((uint8_t *)&big)[i] = 0xAAu;
    big.v.size = (uint32_t)sizeof big;
    rc = aar_get_volume_info(file, &big.v);
    if (not_served(&t, rc, "volume_info")) {
        (void)aar_file_delete(file);
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "volume_info of a file's volume");
    if (rc == AAR_OK) {
        conf_check(&t, big.v.block_bytes > 0u, "block_bytes is 0");
        conf_check(&t, big.v.total_bytes > 0u, "total_bytes is 0");
        conf_check(&t, big.v.free_bytes <= big.v.total_bytes, "free_bytes exceeds total_bytes");
        conf_check(&t, (big.v.flags & ~(uint32_t)AAR_VOLF_READONLY) == 0u, "flags has an undefined bit");
        conf_check(&t, big.v.reserved[0] == 0u && big.v.reserved[1] == 0u && big.v.reserved[2] == 0u,
                   "reserved is not 0");
        for (i = 0; i < sizeof big.tail; i++) {
            if (big.tail[i] != 0u) {
                conf_check(&t, 0, "the bytes past the structure, inside size, were not zeroed");
                break;
            }
        }
        conf_buf_str(&t.note, "block ");
        conf_buf_u64(&t.note, big.v.block_bytes);
        conf_buf_str(&t.note, " bytes, total ");
        conf_buf_u64(&t.note, big.v.total_bytes);
        conf_buf_str(&t.note, ", free ");
        conf_buf_u64(&t.note, big.v.free_bytes);
    }
    /* A caller with an older, shorter structure: nothing past `size` is touched. */
    for (i = 0; i < sizeof raw; i++)
        raw[i] = 0xAAu;
    ((aar_volume_info *)(void *)raw)->size = 8u;
    rc = aar_get_volume_info(file, (aar_volume_info *)(void *)raw);
    expect(&t, rc, AAR_OK, "volume_info with size 8");
    for (i = 8; i < sizeof raw; i++) {
        if (raw[i] != 0xAAu) {
            conf_check(&t, 0, "volume_info wrote past the size the caller set");
            break;
        }
    }
    {
        aar_volume_info v;

        v.size = (uint32_t)sizeof v;
        expect(&t, aar_get_volume_info(missing, &v), AAR_E_NOENT, "volume_info of a missing path");
    }
    (void)aar_file_delete(file);
    conf_end(&t);
}

/* C-FILE.48: the client serves the calls of API 0.6 (spec 7.1 allows AAR_E_NOSYS; the reference serving loop serves them). */
static void test_served(void)
{
    struct conf_test t;
    const char *missing = uniq("e48");
    const char *other = uniq("e48o");
    aar_volume_info v;
    int rc;

    conf_begin(&t, "C-FILE.48");
    rc = aar_file_delete(missing);
    conf_check(&t, rc != AAR_E_NOSYS, "file_delete is not served");
    rc = aar_file_rename(missing, other, 0);
    conf_check(&t, rc != AAR_E_NOSYS, "file_rename is not served");
    rc = aar_dir_create(join(missing, "sub"));
    conf_check(&t, rc != AAR_E_NOSYS, "dir_create is not served");
    rc = aar_file_set_date(missing, CONF_FILE_MTIME);
    conf_check(&t, rc != AAR_E_NOSYS, "file_set_date is not served");
    rc = aar_file_set_protect(missing, 0);
    conf_check(&t, rc != AAR_E_NOSYS, "file_set_protect is not served");
    rc = aar_file_truncate(999, 0);
    conf_check(&t, rc != AAR_E_NOSYS, "file_truncate is not served");
    v.size = (uint32_t)sizeof v;
    rc = aar_get_volume_info(missing, &v);
    conf_check(&t, rc != AAR_E_NOSYS, "volume_info is not served");
    conf_buf_str(&t.note, "none of the seven file calls of API 0.6 answers AAR_E_NOSYS");
    conf_end(&t);
}

/* ---- C-FILE.49: the new calls after a stop request ------------------------------------------------------------- */

struct stop49 {
    int fd;                 /* an id opened for writing on stoptr.tmp, before the stop */
};

/* Prepares the objects the calls after the stop are aimed at. Returns AAR_OK or the failing code. The runner
 * (r_file.c) checks them when the app has ended. */
static int stop49_prepare(struct stop49 *s)
{
    const char *tr = fx("stoptr.tmp");
    int rc;

    s->fd = -1;
    rc = put(fx("stopdel.tmp"), "12345", 5);
    if (rc == AAR_OK)
        rc = put(fx("stopold.tmp"), "12345", 5);
    if (rc == AAR_OK)
        rc = put(fx("stopdate.tmp"), "12345", 5);
    if (rc == AAR_OK)
        rc = put(fx("stopprot.tmp"), "12345", 5);
    if (rc == AAR_OK)
        rc = put(tr, "12345", 5);
    if (rc == AAR_OK) {
        s->fd = aar_file_open(tr, FILE_RW);
        if (s->fd < 0)
            rc = s->fd;
    }
    return rc;
}

static void stop49_run(struct conf_test *t, const struct stop49 *s)
{
    expect_neg(t, aar_file_delete(fx("stopdel.tmp")), "file_delete after the stop request");
    expect_neg(t, aar_file_rename(fx("stopold.tmp"), fx("stopnew.tmp"), 0), "file_rename after the stop request");
    expect_neg(t, aar_dir_create(fx("stopdir.tmp")), "dir_create after the stop request");
    expect_neg(t, aar_file_set_date(fx("stopdate.tmp"), CONF_FILE_MTIME), "file_set_date after the stop request");
    expect_neg(t, aar_file_set_protect(fx("stopprot.tmp"), 0xFu), "file_set_protect after the stop request");
    expect_neg(t, aar_file_truncate(s->fd, 0), "file_truncate after the stop request");
    if (s->fd >= 0)
        (void)aar_file_close(s->fd);
    conf_buf_str(&t->note, "delete, rename, mkdir, set_date, set_protect and truncate after the stop request give negative codes");
}

/* ---- C-FILE.6: after a stop request ------------------------------------------------------------------------------- */

static void test_stop(void)
{
    struct conf_test t;
    struct conf_test t49;
    struct stop49 s49;
    int rc49;
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
    conf_begin(&t49, "C-FILE.49");
    rc49 = stop49_prepare(&s49);
    rc = conf_peer_send(CONF_MSG_STOP);
    if (rc != AAR_OK) {
        conf_check(&t, 0, "the stop request message was not accepted");
        conf_check(&t49, 0, "the stop request message was not accepted");
        conf_end(&t);
        conf_end(&t49);
        return;
    }
    aar_sleep_us(5000000u);
    conf_check(&t, aar_should_stop() == 1, "the client did not stop the app");
    /* After the stop request a parked call can be ended by the stop (AAR_E_IO),
     * and the call after it can find the abandoned request still open (AAR_E_BUSY):
     * every answer is negative. What the client did with the requests is judged
     * by the runner when the app has ended [spec question 57]. */
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
    if (rc49 == AAR_OK)
        stop49_run(&t49, &s49);
    else
        fail_rc(&t49, "preparing the files for the stop test", rc49);
    conf_end(&t49);
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
    test_directions();
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
    test_delete();
    test_delete_protected();
    test_delete_busy();
    test_rename();
    test_rename_errors();
    test_rename_self();
    test_dir_create();
    test_set_date();
    test_set_protect();
    test_truncate_fd();
    test_volume_info();
    test_served();
    if (platform_id == AAR_PLATFORM_ZZ9000)
        test_map_zz9000();
    test_stop();
    return conf_finish("C-FILE");
}
