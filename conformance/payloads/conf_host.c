/*
 * Conformance payload for C-HOST (spec 7.4, 7.3): the locale, the environment variables
 * and the clipboard that the client serves. The runner (r_host.c) prepares the variables
 * and a clipboard unit and is the peer of the tests that compare with the client's own
 * view; conf_host.h lists the fixtures and the keywords. Without the runner the tests
 * that need it print SKIP host, and the variables of C-HOST.4 to C-HOST.6 are used when
 * the argument conf:fixtures=1 says they exist.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_host.h"
#include "conf_msgio.h"

AAR_NOTE(0, 0, 0, 0)

static int peer;        /* the runner is the client */
static int fixtures;    /* the environment variables of conf_host.h exist */

/* ---- Helpers (no libc) ------------------------------------------------------------------- */

static uint32_t slen(const char *s)
{
    uint32_t n = 0;

    while (s[n])
        n++;
    return n;
}

static int same(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i])
            return 0;
    }
    return 1;
}

static void fill(void *p, uint8_t v, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    uint32_t i;

    for (i = 0; i < n; i++)
        b[i] = v;
}

static int ends_with(const char *s, const char *tail)
{
    uint32_t n = slen(s);
    uint32_t k = slen(tail);

    return n >= k && same(s + n - k, tail, k);
}

/* A string inside its array: a NUL within `max` bytes. */
static int terminated(const char *s, uint32_t max)
{
    uint32_t i;

    for (i = 0; i < max; i++) {
        if (s[i] == '\0')
            return 1;
    }
    return 0;
}

static void why_rc(struct conf_test *t, const char *what, int rc)
{
    if (!conf_fail_first(t))
        return;
    conf_buf_str(&t->why, what);
    conf_buf_str(&t->why, " gave ");
    conf_buf_i64(&t->why, rc);
}

/* A client that does not serve a call answers AAR_E_NOSYS, and may refuse it by policy with
 * AAR_E_ACCES (spec 7.4). The row then passes and says so. Returns 1 for those answers. */
static int not_served(struct conf_test *t, int rc, const char *call)
{
    if (rc != AAR_E_NOSYS && rc != AAR_E_ACCES)
        return 0;
    if (t->note.n)
        conf_buf_str(&t->note, "; ");
    conf_buf_str(&t->note, call);
    conf_buf_str(&t->note, rc == AAR_E_NOSYS ? ": AAR_E_NOSYS, the client does not serve it"
                                              : ": AAR_E_ACCES, the client refuses it by policy");
    return 1;
}

static int expect(struct conf_test *t, int rc, int want, const char *what)
{
    if (rc == want)
        return 1;
    why_rc(t, what, rc);
    return 0;
}

/* ---- C-HOST.1 and C-HOST.2: aar_get_locale_info ------------------------------------------ */

static void test_locale(void)
{
    struct conf_test t;
    aar_locale_info l;
    int rc;

    conf_begin(&t, "C-HOST.1");
    fill(&l, 0xAAu, sizeof l);
    l.size = (uint32_t)sizeof l;
    rc = aar_get_locale_info(&l);
    if (not_served(&t, rc, "locale_info")) {
        conf_end(&t);
        return;
    }
    if (expect(&t, rc, AAR_OK, "aar_get_locale_info")) {
        conf_check(&t, terminated(l.language, sizeof l.language), "language is not NUL-terminated");
        conf_check(&t, terminated(l.country, sizeof l.country), "country is not NUL-terminated");
        conf_check(&t, terminated(l.decimal_point, sizeof l.decimal_point),
                   "decimal_point is not NUL-terminated");
        conf_check(&t, terminated(l.group_separator, sizeof l.group_separator),
                   "group_separator is not NUL-terminated");
        if (terminated(l.language, sizeof l.language))
            conf_check(&t, !ends_with(l.language, ".language"), "language ends in .language");
        conf_check(&t, (l.flags & ~(uint32_t)AAR_LOCALEF_OFFSET_KNOWN) == 0u, "flags has an undefined bit");
        conf_check(&t, l.utc_offset_min >= -1440 && l.utc_offset_min <= 1440,
                   "utc_offset_min is outside -1440 to 1440");
        conf_check(&t, l.reserved[0] == 0u && l.reserved[1] == 0u && l.reserved[2] == 0u,
                   "reserved is not 0");
        if (terminated(l.language, sizeof l.language)) {
            conf_buf_str(&t.note, "language ");
            conf_buf_str(&t.note, l.language);
        }
        conf_buf_str(&t.note, ", utc offset ");
        conf_buf_i64(&t.note, l.utc_offset_min);
        conf_buf_str(&t.note, " min");
    }
    conf_end(&t);
}

static void test_locale_size(void)
{
    struct conf_test t;
    struct {
        aar_locale_info l;
        uint8_t tail[16];
    } big;
    uint8_t raw[sizeof(aar_locale_info) + 8];
    uint32_t i;
    int rc;

    conf_begin(&t, "C-HOST.2");
    fill(&big, 0xAAu, sizeof big);
    big.l.size = (uint32_t)sizeof big;
    rc = aar_get_locale_info(&big.l);
    if (not_served(&t, rc, "locale_info")) {
        conf_end(&t);
        return;
    }
    if (expect(&t, rc, AAR_OK, "aar_get_locale_info with a larger size")) {
        for (i = 0; i < sizeof big.tail; i++) {
            if (big.tail[i] != 0u) {
                conf_check(&t, 0, "the bytes past the structure, inside size, were not zeroed");
                break;
            }
        }
    }
    fill(raw, 0xAAu, sizeof raw);
    ((aar_locale_info *)(void *)raw)->size = 8u;
    rc = aar_get_locale_info((aar_locale_info *)(void *)raw);
    if (expect(&t, rc, AAR_OK, "aar_get_locale_info with size 8")) {
        for (i = 8; i < sizeof raw; i++) {
            if (raw[i] != 0xAAu) {
                conf_check(&t, 0, "aar_get_locale_info wrote past the size the caller set");
                break;
            }
        }
    }
    conf_buf_str(&t.note, "zeroes past the structure, writes nothing past size");
    conf_end(&t);
}

/* C-HOST.3: the runner compares with locale.library. */
static void test_locale_peer(void)
{
    struct conf_test t;
    aar_locale_info l;
    uint32_t ok = 0, count = 0, errors = 0;
    int rc;

    if (!peer) {
        conf_skip("C-HOST.3", "host: no runner to read the client's locale");
        return;
    }
    conf_begin(&t, "C-HOST.3");
    fill(&l, 0, sizeof l);
    l.size = (uint32_t)sizeof l;
    rc = aar_get_locale_info(&l);
    if (not_served(&t, rc, "locale_info")) {
        conf_end(&t);
        return;
    }
    if (!expect(&t, rc, AAR_OK, "aar_get_locale_info") || !terminated(l.language, sizeof l.language) ||
        !terminated(l.decimal_point, sizeof l.decimal_point) ||
        !terminated(l.group_separator, sizeof l.group_separator) ||
        !terminated(l.country, sizeof l.country)) {
        conf_check(&t, 0, "the record cannot be compared");
        conf_end(&t);
        return;
    }
    (void)cmio_kw(CH_K_LOCALE, (uint32_t)l.utc_offset_min, ch_hash(l.language), l.flags);
    rc = cmio_result(10000000u, &ok, &count, &errors, 0, 0, 0);
    if (rc != 1) {
        conf_check(&t, 0, "no answer from the runner");
    } else if (ok == 0u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "utc_offset_min or language differs from the client's locale (client offset ");
        conf_buf_i64(&t.why, (int32_t)count);
        conf_buf_str(&t.why, " min, app ");
        conf_buf_i64(&t.why, l.utc_offset_min);
        conf_buf_str(&t.why, " min)");
    } else if (ok == 2u) {
        conf_buf_str(&t.note, "the runner has no locale.library to compare with");
    }
    if (ok == 1u) {
        /* c is 0 when the client reports no country: the runner then does not compare it. */
        (void)cmio_kw(CH_K_LOC2, ch_hash(l.decimal_point), ch_hash(l.group_separator),
                      l.country[0] ? ch_hash(l.country) : 0u);
        rc = cmio_result(10000000u, &ok, &count, &errors, 0, 0, 0);
        if (rc != 1)
            conf_check(&t, 0, "no answer from the runner");
        else
            conf_check(&t, ok != 0u, "decimal_point, group_separator or country differs from the client's locale");
        if (!t.failed)
            conf_buf_str(&t.note, "offset, language, decimal point and group separator agree with the client");
    }
    conf_end(&t);
}

/* ---- C-HOST.4 to C-HOST.7: environment variables ------------------------------------------ */

static void test_env_hello(void)
{
    struct conf_test t;
    char buf[32];
    uint32_t n;
    int rc;

    if (!fixtures) {
        conf_skip("C-HOST.4", "host: the environment variables of the fixtures do not exist");
        return;
    }
    conf_begin(&t, "C-HOST.4");
    fill(buf, 0xAAu, sizeof buf);
    n = 0xFFFFu;
    rc = aar_env_get(CH_ENV_HELLO, buf, sizeof buf, &n);
    if (not_served(&t, rc, "env_get")) {
        conf_end(&t);
        return;
    }
    if (expect(&t, rc, AAR_OK, "env_get of AAR_CONF_HELLO")) {
        conf_check(&t, n == CH_HELLO_BYTES, "the length is not 11");
        conf_check(&t, same(buf, CH_HELLO_VALUE, CH_HELLO_BYTES) && buf[CH_HELLO_BYTES] == '\0',
                   "the value is not 'hello world' with a NUL");
    }
    n = 0xFFFFu;
    expect(&t, aar_env_get(CH_ENV_HELLO, buf, 0, &n), AAR_E_NOSPC, "env_get with bytes 0");
    conf_check(&t, n == CH_HELLO_BYTES, "*n after bytes 0 is not 11");
    n = 0xFFFFu;
    expect(&t, aar_env_get(CH_ENV_HELLO, buf, CH_HELLO_BYTES, &n), AAR_E_NOSPC, "env_get with bytes 11");
    conf_check(&t, n == CH_HELLO_BYTES, "*n after bytes 11 is not 11");
    n = 0xFFFFu;
    fill(buf, 0xAAu, sizeof buf);
    if (expect(&t, aar_env_get(CH_ENV_HELLO, buf, CH_HELLO_BYTES + 1u, &n), AAR_OK, "env_get with bytes 12"))
        conf_check(&t, same(buf, CH_HELLO_VALUE, CH_HELLO_BYTES) && buf[CH_HELLO_BYTES] == '\0',
                   "the value with bytes 12 is wrong");
    conf_buf_str(&t.note, "value, length, NOSPC with the length set");
    conf_end(&t);
}

static void test_env_long(void)
{
    struct conf_test t;
    static char big[CH_LONG_BYTES + 8u];
    char small[64];
    uint32_t n;
    uint32_t i;
    int rc;

    if (!fixtures) {
        conf_skip("C-HOST.5", "host: the environment variables of the fixtures do not exist");
        return;
    }
    conf_begin(&t, "C-HOST.5");
    n = 0;
    rc = aar_env_get(CH_ENV_LONG, small, sizeof small, &n);
    if (not_served(&t, rc, "env_get")) {
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_E_NOSPC, "env_get of 300 bytes into 64");
    conf_check(&t, n == CH_LONG_BYTES, "*n is not 300");
    n = 0;
    expect(&t, aar_env_get(CH_ENV_LONG, big, CH_LONG_BYTES, &n), AAR_E_NOSPC, "env_get of 300 bytes into 300");
    fill(big, 0, sizeof big);
    n = 0;
    if (expect(&t, aar_env_get(CH_ENV_LONG, big, CH_LONG_BYTES + 1u, &n), AAR_OK,
               "env_get of 300 bytes into 301")) {
        conf_check(&t, n == CH_LONG_BYTES && big[CH_LONG_BYTES] == '\0', "length or NUL wrong");
        for (i = 0; i < CH_LONG_BYTES; i++) {
            if ((uint8_t)big[i] != ch_long_byte(i)) {
                conf_check(&t, 0, "the value differs from the stored bytes");
                break;
            }
        }
    }
    conf_buf_str(&t.note, "300 bytes, NOSPC for 64 and 300, exact for 301");
    conf_end(&t);
}

static void test_env_names(void)
{
    struct conf_test t;
    static char name[260];
    char buf[8];
    uint32_t n = 0;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-HOST.6");
    rc = aar_env_get(CH_ENV_UNSET, buf, sizeof buf, &n);
    if (not_served(&t, rc, "env_get")) {
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_E_NOENT, "env_get of an unset name");
    expect(&t, aar_env_get("", buf, sizeof buf, &n), AAR_E_INVAL, "env_get of a name of 0 bytes");
    for (i = 0; i < 256u; i++)
        name[i] = 'N';
    name[256] = '\0';
    expect(&t, aar_env_get(name, buf, sizeof buf, &n), AAR_E_INVAL, "env_get of a name of 256 bytes");
    name[255] = '\0';
    expect(&t, aar_env_get(name, buf, sizeof buf, &n), AAR_E_NOENT, "env_get of a name of 255 bytes");
    conf_buf_str(&t.note, "NOENT, INVAL for 0 and 256 bytes, NOENT for 255");
    conf_end(&t);
}

static void test_env_local(void)
{
    struct conf_test t;
    char buf[16];
    uint32_t n = 0;
    int rc;

    if (!peer) {
        conf_skip("C-HOST.7", "host: no runner to set a local variable");
        return;
    }
    conf_begin(&t, "C-HOST.7");
    rc = aar_env_get(CH_ENV_LOCAL, buf, sizeof buf, &n);
    if (not_served(&t, rc, "env_get")) {
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_E_NOENT, "env_get of a variable that is local to the runner's process");
    conf_buf_str(&t.note, "a local variable is not in ENV:");
    conf_end(&t);
}

/* ---- C-HOST.8 to C-HOST.14: the clipboard -------------------------------------------------- */

/* Reads the clip of unit/type into buf (room bytes) and returns the call's code; *n is the full size. */
static int clip_get(uint32_t unit, uint32_t type, void *buf, uint32_t room, uint32_t *n)
{
    *n = 0xFFFFFFFFu;
    return aar_clip_read(unit, type, buf, room, n);
}

/* Writes a clip and checks its exact size and bytes if both calls are served. */
static void clip_round(struct conf_test *t, uint32_t unit, uint32_t type, const void *data, uint32_t bytes,
                      void *buf, uint32_t room, const char *what)
{
    uint32_t n;
    int rc = aar_clip_write(unit, type, data, bytes);

    if (not_served(t, rc, "clip_write") || !expect(t, rc, AAR_OK, what))
        return;
    rc = clip_get(unit, type, buf, room, &n);
    if (not_served(t, rc, "clip_read"))
        return;
    if (rc != AAR_OK || n != bytes || !same(buf, data, bytes)) {
        if (conf_fail_first(t)) {
            conf_buf_str(&t->why, what);
            conf_buf_str(&t->why, ": the clip read back differs (read gave ");
            conf_buf_i64(&t->why, rc);
            conf_buf_str(&t->why, ", size ");
            conf_buf_u64(&t->why, n);
            conf_buf_str(&t->why, ", wrote ");
            conf_buf_u64(&t->why, bytes);
            conf_buf_str(&t->why, ")");
        }
    }
}

static void test_clip_text(void)
{
    struct conf_test t;
    static const char first[] = "AAR clip text, first version";
    static const char second[] = "second";
    uint8_t buf[64];
    uint32_t n;
    uint32_t len = (uint32_t)sizeof first - 1u;
    int rc;

    conf_begin(&t, "C-HOST.8");
    rc = aar_clip_write(CH_SCRATCH_UNIT, AAR_CLIP_TEXT, first, len);
    if (not_served(&t, rc, "clip_write") || !expect(&t, rc, AAR_OK, "clip_write of text")) {
        conf_end(&t);
        return;
    }
    rc = clip_get(CH_SCRATCH_UNIT, AAR_CLIP_TEXT, buf, sizeof buf, &n);
    if (!not_served(&t, rc, "full TEXT read") && expect(&t, rc, AAR_OK, "clip_read of the text")) {
        conf_check(&t, n == len, "the size of the clip is not the text size");
        conf_check(&t, same(buf, first, len), "the text read differs from the text written");
    }
    rc = clip_get(CH_SCRATCH_UNIT, AAR_CLIP_TEXT, buf, 0, &n);
    if (!not_served(&t, rc, "zero-byte TEXT read") &&
        expect(&t, rc, AAR_E_NOSPC, "clip_read with bytes 0"))
        conf_check(&t, n == len, "*n after bytes 0 is not the text size");
    fill(buf, 0xAAu, sizeof buf);
    rc = clip_get(CH_SCRATCH_UNIT, AAR_CLIP_TEXT, buf, 5, &n);
    if (!not_served(&t, rc, "short TEXT read") &&
        expect(&t, rc, AAR_E_NOSPC, "clip_read into 5 bytes")) {
        conf_check(&t, n == len, "*n after a short read is not the text size");
        conf_check(&t, same(buf, first, 5) && buf[5] == 0xAAu, "a short read did not copy exactly bytes bytes");
    }
    clip_round(&t, CH_SCRATCH_UNIT, AAR_CLIP_TEXT, second, (uint32_t)sizeof second - 1u, buf, sizeof buf,
               "the second write");
    if (!t.note.n)
        conf_buf_str(&t.note, "round trip, NOSPC with the size, a second write replaces the first");
    conf_end(&t);
}

static void test_clip_charset(void)
{
    struct conf_test t;
    uint8_t buf[64];
    uint32_t n;
    int rc;

    if (!peer) {
        conf_skip("C-HOST.9", "host: the client's character set is not the loader's");
        return;
    }
    conf_begin(&t, "C-HOST.9");
    rc = aar_clip_write(CH_SCRATCH_UNIT, AAR_CLIP_TEXT, CH_APP_UTF8, slen(CH_APP_UTF8));
    if (not_served(&t, rc, "clip_write") ||
        !expect(&t, rc, AAR_OK, "clip_write of text with U+00E9 and U+4E2D")) {
        conf_end(&t);
        return;
    }
    rc = clip_get(CH_SCRATCH_UNIT, AAR_CLIP_TEXT, buf, sizeof buf, &n);
    if (!not_served(&t, rc, "clip_read") && expect(&t, rc, AAR_OK, "clip_read of the converted text")) {
        /* "App clip: caf" U+00E9 " " "?" */
        static const char want[] = "App clip: caf\xC3\xA9 ?";

        conf_check(&t, n == (uint32_t)sizeof want - 1u && same(buf, want, n),
                   "the text read back is not the UTF-8 text with U+4E2D replaced by ?");
    }
    if (!t.note.n)
        conf_buf_str(&t.note, "U+00E9 round-trips, U+4E2D becomes ?");
    conf_end(&t);
}

static void test_clip_iff(void)
{
    struct conf_test t;
    static const uint8_t form[12] = { 'F', 'O', 'R', 'M', 0, 0, 0, 4, 'A', 'A', 'R', 'C' };
    uint8_t buf[32];
    uint32_t n;
    int rc;

    conf_begin(&t, "C-HOST.10");
    rc = aar_clip_write(CH_SCRATCH_UNIT, AAR_CLIP_IFF, form, sizeof form);
    if (not_served(&t, rc, "clip_write") || !expect(&t, rc, AAR_OK, "clip_write of an IFF FORM")) {
        conf_end(&t);
        return;
    }
    rc = clip_get(CH_SCRATCH_UNIT, AAR_CLIP_IFF, buf, sizeof buf, &n);
    if (!not_served(&t, rc, "IFF read") && expect(&t, rc, AAR_OK, "clip_read of the IFF FORM"))
        conf_check(&t, n == sizeof form && same(buf, form, sizeof form), "the FORM read differs from the one written");
    rc = clip_get(CH_SCRATCH_UNIT, AAR_CLIP_TEXT, buf, sizeof buf, &n);
    if (!not_served(&t, rc, "TEXT read after IFF write"))
        expect(&t, rc, AAR_E_NOENT, "clip_read of text from a unit that holds a FORM that is not text");
    if (!t.note.n)
        conf_buf_str(&t.note, "the FORM is unchanged; text from it is AAR_E_NOENT");
    conf_end(&t);
}

static void test_clip_big(void)
{
    struct conf_test t;
    static uint8_t src[CH_BIG_BYTES];
    static uint8_t dst[CH_BIG_BYTES + 8u];
    uint32_t i;

    conf_begin(&t, "C-HOST.11");
    for (i = 0; i < CH_BIG_BYTES; i++)
        src[i] = ch_big_byte(i);
    clip_round(&t, CH_SCRATCH_UNIT, AAR_CLIP_TEXT, src, CH_BIG_BYTES, dst, sizeof dst, "write of 70000 bytes of text");
    /* An IFF FORM of the same size: FORM, size 69992, AARC, then bytes. */
    src[0] = 'F';
    src[1] = 'O';
    src[2] = 'R';
    src[3] = 'M';
    src[4] = 0;
    src[5] = 1;
    src[6] = 0x11;
    src[7] = 0x68;   /* 0x00011168 = 70000 - 8 */
    src[8] = 'A';
    src[9] = 'A';
    src[10] = 'R';
    src[11] = 'C';
    clip_round(&t, CH_SCRATCH_UNIT, AAR_CLIP_IFF, src, CH_BIG_BYTES, dst, sizeof dst, "write of the 70000 byte FORM");
    if (!t.note.n)
        conf_buf_str(&t.note, "70000 bytes as text and as a FORM, exact");
    conf_end(&t);
}

/* C-HOST.12: each direction is judged on its own. The probe of a direction (unit 256) that is not served
 * (AAR_E_NOSYS or AAR_E_ACCES) leaves that direction out, noted; the direction that is served has to
 * answer all its invalid arguments with AAR_E_INVAL. A client that serves neither passes with both noted. */
static void test_clip_args(void)
{
    struct conf_test t;
    uint8_t buf[8];
    uint32_t n = 0;
    int rc_read, rc_write, read_served, write_served;

    conf_begin(&t, "C-HOST.12");
    rc_read = aar_clip_read(AAR_CLIP_UNITS, AAR_CLIP_TEXT, buf, sizeof buf, &n);
    read_served = !not_served(&t, rc_read, "clip_read");
    rc_write = aar_clip_write(AAR_CLIP_UNITS, AAR_CLIP_TEXT, "x", 1);
    write_served = !not_served(&t, rc_write, "clip_write");
    if (read_served) {
        expect(&t, rc_read, AAR_E_INVAL, "clip_read of unit 256");
        expect(&t, aar_clip_read(0xFFFFFFFFu, AAR_CLIP_TEXT, buf, sizeof buf, &n), AAR_E_INVAL,
               "clip_read of unit 2^32-1");
        expect(&t, aar_clip_read(CH_SCRATCH_UNIT, 0, buf, sizeof buf, &n), AAR_E_INVAL, "clip_read of type 0");
        expect(&t, aar_clip_read(CH_SCRATCH_UNIT, 3, buf, sizeof buf, &n), AAR_E_INVAL, "clip_read of type 3");
    }
    if (write_served) {
        expect(&t, rc_write, AAR_E_INVAL, "clip_write to unit 256");
        expect(&t, aar_clip_write(CH_SCRATCH_UNIT, 0, "x", 1), AAR_E_INVAL, "clip_write of type 0");
        expect(&t, aar_clip_write(CH_SCRATCH_UNIT, 3, "x", 1), AAR_E_INVAL, "clip_write of type 3");
    }
    if (read_served || write_served) {
        if (t.note.n)
            conf_buf_str(&t.note, "; ");
        conf_buf_str(&t.note, read_served && write_served ? "unit 256 and types 0 and 3 are AAR_E_INVAL"
                              : read_served ? "only clip_read was checked: unit 256 and types 0 and 3 are AAR_E_INVAL"
                                            : "only clip_write was checked: unit 256 and types 0 and 3 are AAR_E_INVAL");
    }
    conf_end(&t);
}

static void test_clip_from_runner(void)
{
    struct conf_test t;
    uint8_t buf[64];
    uint32_t n;
    uint32_t want = slen(CH_RUNNER_UTF8);
    int rc;

    if (!peer) {
        conf_skip("C-HOST.13", "host: no runner to put text on the clipboard");
        return;
    }
    conf_begin(&t, "C-HOST.13");
    rc = clip_get(CH_RUNNER_UNIT, AAR_CLIP_TEXT, buf, sizeof buf, &n);
    if (not_served(&t, rc, "clip_read")) {
        conf_end(&t);
        return;
    }
    if (expect(&t, rc, AAR_OK, "clip_read of the text the runner put on unit 201"))
        conf_check(&t, n == want && same(buf, CH_RUNNER_UTF8, want),
                   "the text is not the runner's ISO-8859-1 text as UTF-8");
    conf_buf_str(&t.note, "the runner's ISO-8859-1 text reads as UTF-8");
    conf_end(&t);
}

static void test_clip_to_runner(void)
{
    struct conf_test t;
    uint32_t ok = 0, count = 0, errors = 0;
    int rc;

    if (!peer) {
        conf_skip("C-HOST.14", "host: no runner to read the clipboard");
        return;
    }
    conf_begin(&t, "C-HOST.14");
    rc = aar_clip_write(CH_APP_UNIT, AAR_CLIP_TEXT, CH_APP_UTF8, slen(CH_APP_UTF8));
    if (not_served(&t, rc, "clip_write")) {
        conf_end(&t);
        return;
    }
    expect(&t, rc, AAR_OK, "clip_write to unit 202");
    (void)cmio_kw(CH_K_CLIP, CH_APP_UNIT, 0, 0);
    rc = cmio_result(10000000u, &ok, &count, &errors, 0, 0, 0);
    if (rc != 1) {
        conf_check(&t, 0, "no answer from the runner");
    } else if (ok == 0u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the clipboard does not hold the IFF FTXT text of the app (the runner read ");
        conf_buf_u64(&t.why, count);
        conf_buf_str(&t.why, " bytes)");
    } else if (ok == 2u) {
        conf_buf_str(&t.note, "the runner could not open clipboard.device");
    } else {
        conf_buf_str(&t.note, "the clipboard holds the text as ISO-8859-1");
    }
    conf_end(&t);
}

/* C-HOST.15 */
static void test_served(void)
{
    struct conf_test t;
    aar_locale_info l;
    char buf[8];
    uint32_t n = 0;
    int64_t us = 0;

    conf_begin(&t, "C-HOST.15");
    l.size = (uint32_t)sizeof l;
    conf_check(&t, aar_get_locale_info(&l) != AAR_E_NOSYS, "locale_info is not served");
    conf_check(&t, aar_env_get(CH_ENV_UNSET, buf, sizeof buf, &n) != AAR_E_NOSYS, "env_get is not served");
    conf_check(&t, aar_clip_read(CH_SCRATCH_UNIT, AAR_CLIP_TEXT, buf, sizeof buf, &n) != AAR_E_NOSYS,
               "clip_read is not served");
    conf_check(&t, aar_time_unix_us(&us) != AAR_E_NOSYS, "time_unix_us is not served");
    conf_buf_str(&t.note, "none of the four calls answers AAR_E_NOSYS");
    conf_end(&t);
}

int aar_main(int argc, char **argv)
{
    peer = conf_have_peer(argc, argv);
    fixtures = peer || conf_arg(argc, argv, "conf:fixtures") != 0;

    test_locale();
    test_locale_size();
    test_locale_peer();
    test_env_hello();
    test_env_long();
    test_env_names();
    test_env_local();
    test_clip_text();
    test_clip_charset();
    test_clip_iff();
    test_clip_big();
    test_clip_args();
    test_clip_from_runner();
    test_clip_to_runner();
    test_served();
    return conf_finish("C-HOST");
}
