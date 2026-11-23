/*
 * Conformance payload for C-CON (spec 7.2, 3.8, 10.4): the console and the log.
 * The runner (r_con.c) is the client: it captures the console, shapes it (slow
 * output, scripted input, input that never comes) and checks what the app wrote.
 * The peer protocol and the variants are in conf_con.h. Without the runner
 * (the host runner, a run by hand) the tests that need it print SKIP host.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_con.h"

#if defined(__arm__) || defined(__aarch64__)
#include <unistd.h>
#define CONF_HAVE_LIBC 1
#endif

AAR_NOTE(0, 0, 0, 0)

static uint8_t big[CONF_CON_BIG_BYTES + 64u];

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t cstrlen(const char *s)
{
    uint32_t n = 0;

    while (s[n])
        n++;
    return n;
}

static void rpc_failure(struct conf_test *t, const char *what, int rc,
                        const uint32_t res[4])
{
    if (!conf_fail_first(t))
        return;
    conf_buf_str(&t->why, what);
    if (rc != AAR_OK) {
        conf_buf_str(&t->why, ": no answer from the runner, ");
        conf_buf_i64(&t->why, rc);
    } else {
        conf_buf_str(&t->why, ": the runner says ");
        conf_buf_i64(&t->why, (int32_t)res[0]);
        conf_buf_str(&t->why, " (w1 ");
        conf_buf_u64(&t->why, res[1]);
        conf_buf_chr(&t->why, ')');
    }
}

/* Writes the begin marker, the block and the end marker with one call and
 * returns its result; *wrote is the byte count, *total the length asked. */
static int write_block(uint32_t seed, uint32_t length, uint32_t *wrote,
                       uint32_t *total)
{
    uint32_t b = cstrlen(CONF_CON_BEGIN);
    uint32_t e = cstrlen(CONF_CON_END);
    uint32_t i;

    for (i = 0; i < b; i++)
        big[i] = (uint8_t)CONF_CON_BEGIN[i];
    conf_con_block_fill(seed, big + b, length);
    for (i = 0; i < e; i++)
        big[b + length + i] = (uint8_t)CONF_CON_END[i];
    *total = b + length + e;
    *wrote = 0;
    return aar_console_write(big, *total, wrote);
}

/* ---- C-CON.1: 200 KiB ---------------------------------------------------------------- */

static void test_big(int peer)
{
    struct conf_test t;
    uint32_t wrote;
    uint32_t total;
    uint32_t res[4] = { 0, 0, 0, 0 };
    int rc;

    conf_begin(&t, "C-CON.1");
    rc = write_block(3, CONF_CON_BIG_BYTES, &wrote, &total);
    if (!peer) {
        /* The capture is the client's: the app can only check what it was told. */
        if (rc != AAR_OK || wrote != total) {
            conf_check(&t, 0, "console_write of 200 KiB did not take every byte");
            conf_end(&t);
        } else {
            conf_skip("C-CON.1", "host: no client captures the console");
        }
        return;
    }
    if (rc != AAR_OK || wrote != total) {
        conf_fail_first(&t);
        conf_buf_str(&t.why, "console_write of ");
        conf_buf_u64(&t.why, total);
        conf_buf_str(&t.why, " bytes gave ");
        conf_buf_i64(&t.why, rc);
        conf_buf_str(&t.why, " and n ");
        conf_buf_u64(&t.why, wrote);
        conf_end(&t);
        return;
    }
    rc = conf_con_rpc(CONF_CON_VERIFY, CONF_CON_BIG_BYTES, 3, 0, res);
    if (rc != AAR_OK || res[0] != 0)
        rpc_failure(&t, "the client's capture is not the 200 KiB pattern", rc, res);
    conf_buf_u64(&t.note, CONF_CON_BIG_BYTES);
    conf_buf_str(&t.note, " bytes in one call, the capture holds them in order");
    conf_end(&t);
}

/* ---- C-CON.2: output held back --------------------------------------------------------- */

static void test_slow(void)
{
    struct conf_test t;
    uint32_t wrote;
    uint32_t total;
    uint32_t res[4] = { 0, 0, 0, 0 };
    uint64_t t0;
    uint64_t elapsed;
    int rc;

    conf_begin(&t, "C-CON.2");
    t0 = aar_time_us();
    rc = write_block(7, CONF_CON_SLOW_BYTES, &wrote, &total);
    elapsed = aar_time_us() - t0;
    if ((rc != AAR_OK || wrote != total) && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "console_write gave ");
        conf_buf_i64(&t.why, rc);
        conf_buf_str(&t.why, " and n ");
        conf_buf_u64(&t.why, wrote);
        conf_buf_str(&t.why, " of ");
        conf_buf_u64(&t.why, total);
    }
    if (elapsed < CONF_CON_SLOW_MIN_US && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the write returned after ");
        conf_buf_u64(&t.why, elapsed);
        conf_buf_str(&t.why, " us, the client held its output 500 ms: it did not park");
    }
    rc = conf_con_rpc(CONF_CON_VERIFY, CONF_CON_SLOW_BYTES, 7, 0, res);
    if (rc != AAR_OK || res[0] != 0)
        rpc_failure(&t, "a byte of the held-back write was lost or changed", rc, res);
    conf_buf_str(&t.note, "the write parked ");
    conf_buf_u64(&t.note, elapsed);
    conf_buf_str(&t.note, " us and lost no byte");
    conf_end(&t);
}

/* ---- C-CON.3, C-CON.6, C-CON.4: scripted input --------------------------------------- */

/* Reads exactly `want` bytes with aar_console_read (it may return less at a
 * time). Returns the bytes obtained; *rc is the last call's result. */
static uint32_t read_exact(uint8_t *buf, uint32_t want, int *rc, uint64_t *first_us)
{
    uint32_t have = 0;
    uint64_t t0 = aar_time_us();
    int first = 1;

    *rc = AAR_OK;
    while (have < want) {
        uint32_t n = 0;

        *rc = aar_console_read(buf + have, want - have, &n);
        if (first) {
            *first_us = aar_time_us() - t0;
            first = 0;
        }
        if (*rc != AAR_OK || n == 0)
            break;
        have += n;
    }
    return have;
}

static void test_read(void)
{
    struct conf_test t;
    uint8_t buf[sizeof CONF_CON_INPUT_A + 8];
    uint32_t want = (uint32_t)sizeof CONF_CON_INPUT_A - 1u;
    uint32_t have;
    uint64_t first = 0;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-CON.3");
    have = read_exact(buf, want, &rc, &first);
    if (have != want && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "got ");
        conf_buf_u64(&t.why, have);
        conf_buf_str(&t.why, " of ");
        conf_buf_u64(&t.why, want);
        conf_buf_str(&t.why, " bytes, last result ");
        conf_buf_i64(&t.why, rc);
    } else {
        for (i = 0; i < want; i++) {
            if (buf[i] != (uint8_t)CONF_CON_INPUT_A[i]) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "byte ");
                    conf_buf_u64(&t.why, i);
                    conf_buf_str(&t.why, " is not what the client fed");
                }
                break;
            }
        }
    }
    if (first < CONF_CON_INPUT_MIN_US && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the first read returned after ");
        conf_buf_u64(&t.why, first);
        conf_buf_str(&t.why, " us, the client had no input for 300 ms: it did not park");
    }
    conf_buf_str(&t.note, "the first read parked ");
    conf_buf_u64(&t.note, first);
    conf_buf_str(&t.note, " us and the line arrived intact");
    conf_end(&t);
}

static void test_libc(void)
{
#ifdef CONF_HAVE_LIBC
    struct conf_test t;
    uint8_t buf[sizeof CONF_CON_INPUT_B + 8];
    uint32_t want = (uint32_t)sizeof CONF_CON_INPUT_B - 1u;
    uint32_t have = 0;
    uint32_t res[4] = { 0, 0, 0, 0 };
    ssize_t n;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-CON.6");
    n = write(1, CONF_CON_FD1, cstrlen(CONF_CON_FD1));
    if (n != (ssize_t)cstrlen(CONF_CON_FD1))
        conf_check(&t, 0, "write(1, ...) did not take every byte");
    n = write(2, CONF_CON_FD2, cstrlen(CONF_CON_FD2));
    if (n != (ssize_t)cstrlen(CONF_CON_FD2))
        conf_check(&t, 0, "write(2, ...) did not take every byte");
    while (have < want) {
        n = read(0, buf + have, want - have);
        if (n <= 0)
            break;
        have += (uint32_t)n;
    }
    if (have != want) {
        conf_check(&t, 0, "read(0, ...) did not return the input line");
    } else {
        for (i = 0; i < want; i++) {
            if (buf[i] != (uint8_t)CONF_CON_INPUT_B[i]) {
                conf_check(&t, 0, "read(0, ...) returned other bytes than the client fed");
                break;
            }
        }
    }
    conf_check(&t, isatty(0) == 0, "isatty(0) is not 0");
    conf_check(&t, isatty(1) == 0, "isatty(1) is not 0");
    conf_check(&t, isatty(2) == 0, "isatty(2) is not 0");
    rc = conf_con_rpc(CONF_CON_FIND, 0, 0, "CONF-CON-FD1 ok", res);
    if (rc != AAR_OK || res[0] != 0)
        rpc_failure(&t, "what write(1, ...) wrote is not in the console capture", rc, res);
    rc = conf_con_rpc(CONF_CON_FIND, 0, 0, "CONF-CON-FD2 ok", res);
    if (rc != AAR_OK || res[0] != 0)
        rpc_failure(&t, "what write(2, ...) wrote is not in the console capture", rc, res);
    conf_buf_str(&t.note, "write(1), write(2), read(0) over the console, isatty 0 on 0, 1 and 2");
    conf_end(&t);
#else
    conf_skip("C-CON.6", "host: the libc glue is the Arm runtime's");
#endif
}

static void test_eof(void)
{
    struct conf_test t;
    uint8_t buf[16];
    uint32_t n;
    int rc;
    int k;

    conf_begin(&t, "C-CON.4");
    for (k = 0; k < 2; k++) {
        n = 99;
        rc = aar_console_read(buf, sizeof buf, &n);
        if (rc != AAR_OK && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "a read at the end of input gave ");
            conf_buf_i64(&t.why, rc);
        } else if (rc == AAR_OK && n != 0 && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "a read at the end of input returned ");
            conf_buf_u64(&t.why, n);
            conf_buf_str(&t.why, " bytes");
        }
    }
    conf_buf_str(&t.note, "AAR_OK with 0 bytes, twice");
    conf_end(&t);
}

/* ---- C-CON.5: a stop ends a console wait ---------------------------------------------------- */

static void test_stop(void)
{
    struct conf_test t;
    uint32_t res[4] = { 0, 0, 0, 0 };
    uint8_t buf[16];
    uint32_t n = 99;
    uint64_t t0;
    uint64_t elapsed;
    int rc;

    conf_begin(&t, "C-CON.5");
    rc = conf_con_rpc(CONF_CON_STOPAT, CONF_CON_STOP_DELAY_MS, 0, 0, res);
    if (rc != AAR_OK) {
        rpc_failure(&t, "the stop request was not answered", rc, res);
        conf_end(&t);
        return;
    }
    t0 = aar_time_us();
    rc = aar_console_read(buf, sizeof buf, &n);
    elapsed = aar_time_us() - t0;
    if (rc != AAR_E_IO && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the read gave ");
        conf_buf_i64(&t.why, rc);
        conf_buf_str(&t.why, " after ");
        conf_buf_u64(&t.why, elapsed);
        conf_buf_str(&t.why, " us, not AAR_E_IO");
    }
    conf_check(&t, aar_should_stop() == 1, "should_stop is 0 after the read");
    conf_buf_str(&t.note, "the read ended with AAR_E_IO after ");
    conf_buf_u64(&t.note, elapsed);
    conf_buf_str(&t.note, " us");
    conf_end(&t);
}

/* ---- C-CON.7 to C-CON.9: the log -------------------------------------------------------------- */

static void send_log(uint32_t k)
{
    static uint8_t rec[5000];
    uint32_t len = conf_con_log_len(k);

    conf_con_log_fill(k, rec, len);
    aar_log((const char *)rec, len);
    /* The platform may rate-limit the log (32 a second to the client): space them. */
    aar_sleep_us(100000u);
}

static void test_log(uint32_t k, const char *id)
{
    struct conf_test t;
    uint32_t res[4] = { 0, 0, 0, 0 };
    int rc;

    conf_begin(&t, id);
    send_log(k);
    rc = conf_con_rpc(CONF_CON_LOGCHK, 1, k, 0, res);
    if (rc != AAR_OK || res[0] != 0)
        rpc_failure(&t, "the client's log hook did not get the record as sent", rc, res);
    if (k == 0)
        conf_buf_str(&t.note, "the 40 byte record arrived intact, and not in the console");
    else
        conf_buf_str(&t.note, "the 2000 byte record arrived as one piece of at most 1024 bytes");
    conf_end(&t);
}

static void test_log_long(void)
{
    struct conf_test t;
    uint32_t res[4] = { 0, 0, 0, 0 };
    uint32_t k;
    int rc;

    conf_begin(&t, "C-CON.9");
    for (k = 2; k < CONF_CON_LOG_KINDS; k++)
        send_log(k);
    rc = conf_con_rpc(CONF_CON_LOGCHK, 2, 0, 0, res);
    if (rc != AAR_OK || res[0] != 0)
        rpc_failure(&t, "a record the client received is not a prefix of one the app sent",
                    rc, res);
    conf_buf_u64(&t.note, res[1]);
    conf_buf_str(&t.note, " records, every one a prefix, none wrapped or split");
    conf_end(&t);
}

/* ---- main ------------------------------------------------------------------------------------- */

int aar_main(int argc, char **argv)
{
    const char *variant = conf_arg(argc, argv, "conf:variant");
    int peer = conf_have_peer(argc, argv);

    if (variant && streq(variant, CONF_CON_VARIANT_SLOWOUT)) {
        test_slow();
    } else if (variant && streq(variant, CONF_CON_VARIANT_INPUT)) {
        test_read();
        test_libc();
        test_eof();
    } else if (variant && streq(variant, CONF_CON_VARIANT_BLOCK)) {
        test_stop();
    } else {
        test_big(peer);
        if (peer) {
            test_log(0, "C-CON.7");
            test_log(1, "C-CON.8");
            test_log_long();
        } else {
            conf_skip("C-CON.2", "host: no client holds the output back");
            conf_skip("C-CON.3", "host: no client feeds the input");
            conf_skip("C-CON.4", "host: no client ends the input");
            conf_skip("C-CON.5", "host: no client stops the app");
            conf_skip("C-CON.6", "host: no client feeds the input, and the libc glue is the Arm runtime's");
            conf_skip("C-CON.7", "host: no client log hook");
            conf_skip("C-CON.8", "host: no client log hook");
            conf_skip("C-CON.9", "host: no client log hook");
        }
    }
    return conf_finish("C-CON");
}
