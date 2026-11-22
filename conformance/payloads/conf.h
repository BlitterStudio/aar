/*
 * Conformance payload protocol (see conformance/README.md and TESTS.md).
 *
 * A payload is an AAR app that judges one area of the standard from the inside
 * and reports through its console, one line per test id:
 *
 *   AAR-CONF <id> PASS|FAIL|SKIP <detail>
 *
 * and, as its last line,
 *
 *   AAR-CONF-END <area> pass=<n> fail=<n> skip=<n>
 *
 * The runner (conformance/runner/aar-conf.c) relays the result lines and fails
 * every id of TESTS.md that this payload is meant to report (judged by payload,
 * impl yes, group declared) and did not report: a payload that crashed, was
 * refused or hung cannot pass by silence. The end line tells the runner the
 * payload ran to its end; its counts must match the lines.
 *
 * Arguments (argv) the runner passes to every payload:
 *   argv[1]  alpha
 *   argv[2]  beta gamma            (one argument that holds a space)
 *   then, in any order,
 *   conf:peer=1       a runner is the client and may act as the peer of a test
 *   info:<key>=<n>    the runner's AARInfo, decimal (groups, platform_id,
 *                     msg_capacity, stall_limit_ms, stop_grace_ms, ...)
 * A payload run by hand, by the host runner or under a test loader gets none of
 * the conf: and info: arguments and must then SKIP the tests that need them,
 * with a detail that starts with "host:".
 *
 * Peer messages: a payload asks the runner for something by sending a message
 * (32 bytes) that starts with an ASCII keyword. Today the keyword is
 * CONF-STOP-NOW (the runner stops the app, aar_serve_stop).
 *
 * Output goes through aar_console_write, so a payload does not need a libc, and
 * the same source builds as an Arm app and for the host runner.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_H
#define AAR_CONF_H

#include <stdint.h>

#include "aar/app.h"

#include "conf_proto.h"

#if defined(__aarch64__)
#define CONF_PROFILE "a64"
#elif defined(__arm__)
#define CONF_PROFILE "a32"
#else
#define CONF_PROFILE "host"
#endif

/* The longest line a payload prints, with its newline. */
#define CONF_LINE_MAX 240u

/* ---- Line buffer ------------------------------------------------------------ */

struct conf_buf {
    char     b[CONF_LINE_MAX];
    uint32_t n;
};

static inline void conf_buf_clear(struct conf_buf *c)
{
    c->n = 0;
    c->b[0] = '\0';
}

static inline void conf_buf_chr(struct conf_buf *c, char ch)
{
    /* One byte stays free for the newline and one for the NUL. */
    if (c->n + 2u >= CONF_LINE_MAX)
        return;
    if (ch == '\n' || ch == '\r')
        ch = ' ';
    c->b[c->n++] = ch;
    c->b[c->n] = '\0';
}

static inline void conf_buf_str(struct conf_buf *c, const char *s)
{
    while (*s)
        conf_buf_chr(c, *s++);
}

static inline void conf_buf_u64(struct conf_buf *c, uint64_t v)
{
    char tmp[24];
    uint32_t i = 0;

    do {
        tmp[i++] = (char)('0' + (uint32_t)(v % 10u));
        v /= 10u;
    } while (v);
    while (i)
        conf_buf_chr(c, tmp[--i]);
}

static inline void conf_buf_i64(struct conf_buf *c, int64_t v)
{
    if (v < 0) {
        conf_buf_chr(c, '-');
        conf_buf_u64(c, (uint64_t)0 - (uint64_t)v);
    } else {
        conf_buf_u64(c, (uint64_t)v);
    }
}

static inline void conf_buf_hex(struct conf_buf *c, uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[16];
    uint32_t i = 0;

    do {
        tmp[i++] = digits[v & 0xfu];
        v >>= 4;
    } while (v);
    conf_buf_chr(c, '0');
    conf_buf_chr(c, 'x');
    while (i)
        conf_buf_chr(c, tmp[--i]);
}

/* ---- Console output ----------------------------------------------------------- */

/* Writes the whole line and a newline. console_write takes every byte or
 * returns an error; on an error the line is lost and the runner will fail the
 * id it belongs to. */
static inline void conf_emit(const struct conf_buf *c)
{
    char line[CONF_LINE_MAX];
    uint32_t done = 0;
    uint32_t n = c->n;
    uint32_t i;

    for (i = 0; i < n; i++)
        line[i] = c->b[i];
    line[n++] = '\n';
    while (done < n) {
        uint32_t wrote = 0;

        if (aar_console_write(line + done, n - done, &wrote) != AAR_OK ||
            wrote == 0)
            return;
        done += wrote;
    }
}

/* ---- Counters and results ------------------------------------------------------- */

struct conf_totals {
    uint32_t pass, fail, skip;
};

static struct conf_totals conf_totals;

static inline void conf_result(const char *id, const char *verdict,
                               const char *detail)
{
    struct conf_buf c;

    conf_buf_clear(&c);
    conf_buf_str(&c, "AAR-CONF ");
    conf_buf_str(&c, id);
    conf_buf_chr(&c, ' ');
    conf_buf_str(&c, verdict);
    conf_buf_chr(&c, ' ');
    conf_buf_str(&c, detail);
    conf_emit(&c);
}

static inline void conf_pass(const char *id, const char *detail)
{
    conf_totals.pass++;
    conf_result(id, "PASS", detail);
}

static inline void conf_fail(const char *id, const char *detail)
{
    conf_totals.fail++;
    conf_result(id, "FAIL", detail);
}

/* A skip always has a reason. Use "host: ..." for what only a platform with a
 * client can do (the host runner and the qemu loader are not platforms). */
static inline void conf_skip(const char *id, const char *reason)
{
    conf_totals.skip++;
    conf_result(id, "SKIP", reason);
}

/* One test with several checks: the first failed check names the failure, a
 * note adds to the PASS detail.
 *
 *   struct conf_test t;
 *   conf_begin(&t, "C-AREA.1");
 *   conf_check(&t, x == y, "x differs from y");
 *   conf_end(&t);
 */
struct conf_test {
    const char     *id;
    int             failed;
    struct conf_buf why;
    struct conf_buf note;
};

static inline void conf_begin(struct conf_test *t, const char *id)
{
    t->id = id;
    t->failed = 0;
    conf_buf_clear(&t->why);
    conf_buf_clear(&t->note);
}

/* Records a failure once; later failures are dropped. Returns cond. */
static inline int conf_check(struct conf_test *t, int cond, const char *what)
{
    if (!cond && !t->failed) {
        t->failed = 1;
        conf_buf_str(&t->why, what);
    }
    return cond;
}

/* Starts a failure text the caller completes with conf_buf_* on t->why; use
 * when the text holds values. Returns 1 if this is the first failure (the
 * caller then fills t->why), 0 if one was already recorded. */
static inline int conf_fail_first(struct conf_test *t)
{
    if (t->failed)
        return 0;
    t->failed = 1;
    return 1;
}

static inline void conf_end(struct conf_test *t)
{
    if (t->failed)
        conf_fail(t->id, t->why.b);
    else
        conf_pass(t->id, t->note.b[0] ? t->note.b : "ok");
}

/* The last line of a payload. Returns the exit code for aar_main: 0 when no
 * test failed, 10 (the AmigaDOS ERROR return code) otherwise. */
static inline int conf_finish(const char *area)
{
    struct conf_buf c;

    conf_buf_clear(&c);
    conf_buf_str(&c, "AAR-CONF-END ");
    conf_buf_str(&c, area);
    conf_buf_str(&c, " pass=");
    conf_buf_u64(&c, conf_totals.pass);
    conf_buf_str(&c, " fail=");
    conf_buf_u64(&c, conf_totals.fail);
    conf_buf_str(&c, " skip=");
    conf_buf_u64(&c, conf_totals.skip);
    conf_emit(&c);
    return conf_totals.fail ? 10 : 0;
}

/* ---- Runner arguments ------------------------------------------------------------ */

static inline int conf_str_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

/* The text after "name=" of the first argument that starts with "name=", or
 * NULL. name includes its prefix, for example "info:groups" or "conf:peer". */
static inline const char *conf_arg(int argc, char **argv, const char *name)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *n = name;

        while (*n && *a == *n) {
            a++;
            n++;
        }
        if (*n == '\0' && *a == '=')
            return a + 1;
    }
    return 0;
}

/* Decimal value of an argument. Returns 1 and sets *out, or 0 when the
 * argument is absent or not a number. */
static inline int conf_arg_u32(int argc, char **argv, const char *name,
                               uint32_t *out)
{
    const char *v = conf_arg(argc, argv, name);
    uint32_t n = 0;

    if (!v || *v == '\0')
        return 0;
    while (*v) {
        if (*v < '0' || *v > '9')
            return 0;
        n = n * 10u + (uint32_t)(*v - '0');
        v++;
    }
    *out = n;
    return 1;
}

/* True when the runner is the client and acts as the peer of tests. */
static inline int conf_have_peer(int argc, char **argv)
{
    const char *v = conf_arg(argc, argv, "conf:peer");

    return v && v[0] == '1' && v[1] == '\0';
}

/* Sends a peer keyword to the runner. Returns the message call's result; a
 * full queue is retried for up to 100 ms. */
static inline int conf_peer_send(const char *keyword)
{
    uint8_t msg[AAR_MSG_BYTES];
    uint32_t i = 0;
    uint64_t start = aar_time_us();
    int rc;

    for (i = 0; i < AAR_MSG_BYTES; i++)
        msg[i] = 0;
    for (i = 0; keyword[i] && i < AAR_MSG_BYTES; i++)
        msg[i] = (uint8_t)keyword[i];
    do {
        rc = aar_msg_send(msg);
        if (rc != AAR_E_BUSY)
            break;
        aar_sleep_us(1000);
    } while (aar_time_us() - start < 100000u);
    return rc;
}

#endif
