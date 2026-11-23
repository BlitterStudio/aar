/*
 * Conformance payload for C-SHARED (spec 4.2, 4.3): shared buffers and
 * visibility, from the app's side. The tests that need the client (C-SHARED.9,
 * 10, 11, 14) use the peer protocol of conf_shared.h; the runner (r_shared.c) is
 * the other half. See conformance/TESTS.md for the ids.
 *
 * Variants (conf:variant=<name>, given by the runner in every run): "default"
 * runs every test but C-SHARED.8; "caps" (the runner sets AAR_TAG_SHARED_MIB to
 * CONF_SHARED_CAPS_MIB) runs only C-SHARED.8. Without a variant argument (the
 * host runner, a run by hand) the default tests run, the ones that need the
 * runner and C-SHARED.8 print SKIP host.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_shared.h"

AAR_NOTE(0, 0, 0, 0)

#define MEGA (1024ull * 1024ull)
#define LIVE 64u  /* AAR_SHARED_MIN_BLOCKS */
#define SCRATCH "conf-shared.tmp"

/* The pause between the rounds of C-SHARED.11. conf_shared.h says about 2 ms;
 * the client reads 4 KiB through an uncached map, which takes the 68k longer than
 * that, so that a round never held still for a whole read and the first card run
 * counted one sample of the five required. 10 ms leaves most reads whole. */
#define SEQ_PAUSE_US 10000u

static aar_info info;
static uint64_t granule;

static uint64_t round_up(uint64_t v, uint64_t g)
{
    return (v + g - 1u) / g * g;
}

static void text_rc(struct conf_buf *b, int rc)
{
    conf_buf_i64(b, (int64_t)rc);
}

/* Writes the whole range and reads it back. Returns the offset of the first bad
 * byte or 0xFFFFFFFFFFFFFFFF. A seed makes two blocks differ. */
static uint64_t write_check(void *p, uint64_t bytes, uint32_t seed)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    uint64_t i;

    for (i = 0; i < bytes; i++)
        b[i] = conf_shared_pattern(seed, (uint32_t)i);
    for (i = 0; i < bytes; i++) {
        if (b[i] != conf_shared_pattern(seed, (uint32_t)i))
            return i;
    }
    return ~(uint64_t)0;
}

/* ---- C-SHARED.1 -------------------------------------------------------------------- */

static void test_flags(void)
{
    static const uint32_t flags[] = { AAR_SHAREDF_COHERENT, 2u, 0x80000000u, 0xFFFFFFFFu };
    struct conf_test t;
    uint32_t i;

    conf_begin(&t, "C-SHARED.1");
    for (i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        aar_shared out = { 0, 0, 0, 0 };
        int rc = aar_shared_alloc(granule, 1, flags[i], &out);

        if (rc == AAR_OK) {
            (void)aar_shared_free(out.ptr);
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "flags ");
                conf_buf_hex(&t.why, flags[i]);
                conf_buf_str(&t.why, " were accepted");
            }
        } else if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "flags ");
            conf_buf_hex(&t.why, flags[i]);
            conf_buf_str(&t.why, " gave ");
            text_rc(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_INVAL");
        }
    }
    conf_buf_str(&t.note, "flags 1, 2, 0x80000000 and 0xFFFFFFFF refused with AAR_E_INVAL");
    conf_end(&t);
}

/* ---- C-SHARED.2 -------------------------------------------------------------------- */

static void test_grant(void)
{
    struct req { uint64_t bytes; uint32_t align; };
    struct req reqs[4];
    struct conf_test t;
    uint32_t i;

    reqs[0].bytes = 1;
    reqs[0].align = 1;
    reqs[1].bytes = granule + 1u;
    reqs[1].align = 1;
    reqs[2].bytes = 3u * granule;
    reqs[2].align = (uint32_t)(2u * granule);
    reqs[3].bytes = 100;
    reqs[3].align = 4096;
    conf_begin(&t, "C-SHARED.2");
    for (i = 0; i < 4; i++) {
        aar_shared out = { 0, 0, 0, 0 };
        uint64_t want = round_up(reqs[i].bytes, granule);
        uint64_t align = round_up(reqs[i].align, granule);
        int rc = aar_shared_alloc(reqs[i].bytes, reqs[i].align, 0, &out);
        uint64_t bad;

        if (rc != AAR_OK) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "alloc of ");
                conf_buf_u64(&t.why, reqs[i].bytes);
                conf_buf_str(&t.why, " bytes gave ");
                text_rc(&t.why, rc);
            }
            continue;
        }
        if (out.bytes != want && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "alloc of ");
            conf_buf_u64(&t.why, reqs[i].bytes);
            conf_buf_str(&t.why, " bytes granted ");
            conf_buf_u64(&t.why, out.bytes);
            conf_buf_str(&t.why, ", want ");
            conf_buf_u64(&t.why, want);
        }
        if ((out.ptr == 0 || ((uintptr_t)out.ptr % align) != 0) && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "ptr ");
            conf_buf_hex(&t.why, (uintptr_t)out.ptr);
            conf_buf_str(&t.why, " is not aligned to ");
            conf_buf_u64(&t.why, align);
        }
        if ((out.flags & ~(uint32_t)AAR_SHAREDF_COHERENT) != 0 && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "flags ");
            conf_buf_hex(&t.why, out.flags);
            conf_buf_str(&t.why, " hold more than AAR_SHAREDF_COHERENT");
        }
        if (out.ptr) {
            bad = write_check(out.ptr, out.bytes, i + 1u);
            if (bad != ~(uint64_t)0 && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "byte ");
                conf_buf_u64(&t.why, bad);
                conf_buf_str(&t.why, " of the block does not hold what was written");
            }
            (void)aar_shared_free(out.ptr);
        }
    }
    conf_buf_str(&t.note, "granule ");
    conf_buf_u64(&t.note, granule);
    conf_buf_str(&t.note, ", sizes and alignments rounded, every byte writable");
    conf_end(&t);
}

/* ---- C-SHARED.3 -------------------------------------------------------------------- */

static void test_free(void)
{
    struct conf_test t;
    aar_shared out = { 0, 0, 0, 0 };
    int rc;

    conf_begin(&t, "C-SHARED.3");
    rc = aar_shared_alloc(2u * granule, 1, 0, &out);
    if (rc != AAR_OK) {
        conf_check(&t, 0, "alloc for the free test failed");
        conf_end(&t);
        return;
    }
    rc = aar_shared_free((uint8_t *)out.ptr + granule);
    if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "free of an interior pointer gave ");
        text_rc(&t.why, rc);
    }
    rc = aar_shared_free((uint8_t *)out.ptr + 1);
    if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "free of ptr + 1 gave ");
        text_rc(&t.why, rc);
    }
    rc = aar_shared_free(0);
    if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "free of NULL gave ");
        text_rc(&t.why, rc);
    }
    rc = aar_shared_free(out.ptr);
    if (rc != AAR_OK && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "free of the base of a live block gave ");
        text_rc(&t.why, rc);
    }
    rc = aar_shared_free(out.ptr);
    if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "a second free gave ");
        text_rc(&t.why, rc);
    }
    conf_buf_str(&t.note, "interior, NULL and repeated frees are AAR_E_INVAL, the base is AAR_OK");
    conf_end(&t);
}

/* ---- C-SHARED.4 and C-SHARED.5 ------------------------------------------------------- */

/* Takes LIVE blocks of one granule, tags each, checks that they do not overlap
 * and frees them. Returns the failure text in why (non-empty) or leaves it empty. */
static void live_blocks(struct conf_buf *why)
{
    static aar_shared blk[LIVE];
    uint32_t i;
    uint32_t got = 0;

    for (i = 0; i < LIVE; i++) {
        int rc = aar_shared_alloc(granule, 1, 0, &blk[i]);

        if (rc != AAR_OK) {
            conf_buf_str(why, "block ");
            conf_buf_u64(why, i);
            conf_buf_str(why, " of 64 failed with ");
            text_rc(why, rc);
            break;
        }
        got++;
        (void)write_check(blk[i].ptr, blk[i].bytes, i + 1u);
    }
    if (got == LIVE) {
        for (i = 0; i < LIVE; i++) {
            uint64_t bad = ~(uint64_t)0;
            volatile uint8_t *b = (volatile uint8_t *)blk[i].ptr;
            uint64_t k;

            for (k = 0; k < blk[i].bytes; k++) {
                if (b[k] != conf_shared_pattern(i + 1u, (uint32_t)k)) {
                    bad = k;
                    break;
                }
            }
            if (bad != ~(uint64_t)0) {
                conf_buf_str(why, "block ");
                conf_buf_u64(why, i);
                conf_buf_str(why, " was overwritten (blocks overlap)");
                break;
            }
        }
    }
    for (i = 0; i < got; i++) {
        if (aar_shared_free(blk[i].ptr) != AAR_OK && why->n == 0)
            conf_buf_str(why, "a free of a live block failed");
    }
}

static void test_live(void)
{
    struct conf_test t;
    struct conf_buf why;

    conf_begin(&t, "C-SHARED.4");
    conf_buf_clear(&why);
    live_blocks(&why);
    if (why.n && conf_fail_first(&t))
        conf_buf_str(&t.why, why.b);
    conf_buf_str(&t.note, "64 live blocks of one granule, none overlapping");
    conf_end(&t);
}

/* The runtime's own service buffers exist while the blocks are taken: the
 * console has written many lines and a file read is open. */
static void test_live_services(void)
{
    static uint8_t buf[4096];
    struct conf_test t;
    struct conf_buf why;
    uint32_t n = 0;
    uint32_t i;
    int fd;
    int rc;
    int64_t pos = -1;
    uint64_t used0 = 0;

    conf_begin(&t, "C-SHARED.5");
    for (i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 7u);
    fd = aar_file_open(SCRATCH, AAR_FILE_READ | AAR_FILE_WRITE | AAR_FILE_CREATE |
                                    AAR_FILE_TRUNCATE);
    if (fd < 0) {
        conf_fail_first(&t);
        conf_buf_str(&t.why, "cannot create the scratch file ");
        conf_buf_str(&t.why, SCRATCH);
        conf_buf_str(&t.why, ": ");
        text_rc(&t.why, fd);
        conf_end(&t);
        return;
    }
    rc = aar_file_write(fd, buf, sizeof buf, &n);
    if (rc != AAR_OK || n != sizeof buf)
        conf_check(&t, 0, "write to the scratch file failed");
    rc = aar_file_seek(fd, 0, AAR_SEEK_SET, &pos);
    if (rc != AAR_OK || pos != 0)
        conf_check(&t, 0, "seek in the scratch file failed");
    for (i = 0; i < sizeof buf; i++)
        buf[i] = 0;
    n = 0;
    rc = aar_file_read(fd, buf, sizeof buf, &n);
    if (rc != AAR_OK || n != sizeof buf || buf[100] != (uint8_t)(100u * 7u))
        conf_check(&t, 0, "read of the scratch file failed");
    if (aar_get_info(&info) == AAR_OK)
        used0 = info.shared_used;
    conf_buf_clear(&why);
    live_blocks(&why);
    if (why.n && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "with the console and a file open: ");
        conf_buf_str(&t.why, why.b);
    }
    (void)aar_file_close(fd);
    conf_buf_str(&t.note, "64 live blocks with console and file buffers in use, shared_used ");
    conf_buf_u64(&t.note, used0);
    conf_end(&t);
}

/* ---- C-SHARED.6 and C-SHARED.8 ---------------------------------------------------------- */

#define FILL_MAX 96u

static aar_shared fill_blk[FILL_MAX];

/* Takes blocks of size `size` until a call fails or FILL_MAX blocks are held.
 * Returns the number held; *rc is the failing call's result (AAR_OK when none
 * failed) and *total the bytes granted. */
static uint32_t fill_until_fail(uint64_t size, int *rc, uint64_t *total)
{
    uint32_t n = 0;

    *total = 0;
    *rc = AAR_OK;
    while (n < FILL_MAX) {
        *rc = aar_shared_alloc(size, 1, 0, &fill_blk[n]);
        if (*rc != AAR_OK)
            break;
        *total += fill_blk[n].bytes;
        n++;
    }
    return n;
}

static void release_fill(uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        (void)aar_shared_free(fill_blk[i].ptr);
}

static uint64_t block_size_for(uint64_t limit)
{
    uint64_t size = round_up(limit / 8u, granule);

    if (size < granule)
        size = granule;
    if (size > 64u * MEGA)
        size = 64u * MEGA;
    return size;
}

static void test_until_failure(void)
{
    struct conf_test t;
    aar_info now;
    uint64_t total;
    int rc;
    uint32_t n;

    conf_begin(&t, "C-SHARED.6");
    n = fill_until_fail(block_size_for(info.shared_limit), &rc, &total);
    now.size = sizeof now;
    (void)aar_get_info(&now);
    if (rc == AAR_OK && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "no failure after ");
        conf_buf_u64(&t.why, total);
        conf_buf_str(&t.why, " bytes, limit ");
        conf_buf_u64(&t.why, info.shared_limit);
    } else if (rc != AAR_OK && rc != AAR_E_NOMEM && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the failure was ");
        text_rc(&t.why, rc);
        conf_buf_str(&t.why, ", not AAR_E_NOMEM");
    }
    if (total > info.shared_limit && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "granted ");
        conf_buf_u64(&t.why, total);
        conf_buf_str(&t.why, " bytes, over the limit ");
        conf_buf_u64(&t.why, info.shared_limit);
    }
    if (now.shared_used > now.shared_limit && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "shared_used ");
        conf_buf_u64(&t.why, now.shared_used);
        conf_buf_str(&t.why, " exceeds shared_limit ");
        conf_buf_u64(&t.why, now.shared_limit);
    }
    release_fill(n);
    conf_buf_u64(&t.note, n);
    conf_buf_str(&t.note, " blocks, ");
    conf_buf_u64(&t.note, total);
    conf_buf_str(&t.note, " of ");
    conf_buf_u64(&t.note, info.shared_limit);
    conf_buf_str(&t.note, " bytes, then AAR_E_NOMEM");
    conf_end(&t);
}

/* ---- C-SHARED.7 ------------------------------------------------------------------------- */

static void test_used(void)
{
    struct conf_test t;
    aar_info a;
    aar_shared out = { 0, 0, 0, 0 };
    uint64_t before;
    uint64_t during;
    uint64_t after;

    conf_begin(&t, "C-SHARED.7");
    a.size = sizeof a;
    (void)aar_get_info(&a);
    before = a.shared_used;
    if (aar_shared_alloc(2u * granule, 1, 0, &out) != AAR_OK) {
        conf_check(&t, 0, "alloc failed");
        conf_end(&t);
        return;
    }
    (void)aar_get_info(&a);
    during = a.shared_used;
    (void)aar_shared_free(out.ptr);
    (void)aar_get_info(&a);
    after = a.shared_used;
    if (during < before + out.bytes && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "shared_used rose from ");
        conf_buf_u64(&t.why, before);
        conf_buf_str(&t.why, " to ");
        conf_buf_u64(&t.why, during);
        conf_buf_str(&t.why, " for a block of ");
        conf_buf_u64(&t.why, out.bytes);
    }
    if (after + out.bytes > during && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "shared_used was ");
        conf_buf_u64(&t.why, during);
        conf_buf_str(&t.why, " with the block and ");
        conf_buf_u64(&t.why, after);
        conf_buf_str(&t.why, " after the free");
    }
    conf_buf_str(&t.note, "shared_used ");
    conf_buf_u64(&t.note, before);
    conf_buf_str(&t.note, " -> ");
    conf_buf_u64(&t.note, during);
    conf_buf_str(&t.note, " -> ");
    conf_buf_u64(&t.note, after);
    conf_end(&t);
}

/* ---- C-SHARED.8 (variant caps) ------------------------------------------------------------ */

static void test_caps(void)
{
    struct conf_test t;
    aar_info now;
    uint64_t limit = CONF_SHARED_CAPS_MIB * MEGA;
    uint64_t total;
    uint64_t total2 = 0;
    uint32_t n;
    uint32_t i;
    int rc;
    int rc2 = AAR_OK;

    conf_begin(&t, "C-SHARED.8");
    if (info.shared_limit != limit && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "shared_limit is ");
        conf_buf_u64(&t.why, info.shared_limit);
        conf_buf_str(&t.why, ", the client asked for ");
        conf_buf_u64(&t.why, limit);
    }
    n = fill_until_fail(block_size_for(limit), &rc, &total);
    if (rc == AAR_OK && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "no failure after ");
        conf_buf_u64(&t.why, total);
        conf_buf_str(&t.why, " bytes");
    } else if (rc != AAR_OK && rc != AAR_E_NOMEM && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the failure was ");
        text_rc(&t.why, rc);
    }
    /* Smaller blocks use what is left; none may go over the limit. */
    for (i = n; i < FILL_MAX && rc != AAR_OK; i++) {
        rc2 = aar_shared_alloc(granule, 1, 0, &fill_blk[i]);
        if (rc2 != AAR_OK)
            break;
        total2 += fill_blk[i].bytes;
        n++;
    }
    if (rc2 != AAR_OK && rc2 != AAR_E_NOMEM && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "a small block failed with ");
        text_rc(&t.why, rc2);
    }
    now.size = sizeof now;
    (void)aar_get_info(&now);
    if (total + total2 > limit && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "granted ");
        conf_buf_u64(&t.why, total + total2);
        conf_buf_str(&t.why, " bytes, over ");
        conf_buf_u64(&t.why, limit);
    }
    if (now.shared_used > limit && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "shared_used ");
        conf_buf_u64(&t.why, now.shared_used);
        conf_buf_str(&t.why, " is over the limit");
    }
    release_fill(n);
    conf_buf_str(&t.note, "limit ");
    conf_buf_u64(&t.note, info.shared_limit);
    conf_buf_str(&t.note, ", granted ");
    conf_buf_u64(&t.note, total + total2);
    conf_end(&t);
}

/* ---- C-SHARED.13 --------------------------------------------------------------------------- */

static void test_visibility_calls(void)
{
    struct conf_test t;
    aar_shared out = { 0, 0, 0, 0 };
    int coherent;
    int rc;
    uint32_t i;
    uint64_t t0;
    uint64_t dt = 0;

    conf_begin(&t, "C-SHARED.13");
    if (aar_shared_alloc(2u * granule, 1, 0, &out) != AAR_OK) {
        conf_check(&t, 0, "alloc failed");
        conf_end(&t);
        return;
    }
    coherent = (info.flags & AAR_INFOF_SHARED_COHERENT) != 0 ||
               (out.flags & AAR_SHAREDF_COHERENT) != 0;
    rc = aar_publish(out.ptr, out.bytes);
    conf_check(&t, rc == AAR_OK, "aar_publish of the whole block failed");
    rc = aar_acquire(out.ptr, out.bytes);
    conf_check(&t, rc == AAR_OK, "aar_acquire of the whole block failed");
    rc = aar_publish((uint8_t *)out.ptr + 3, 5);
    conf_check(&t, rc == AAR_OK, "aar_publish of an inner range failed");
    rc = aar_acquire((uint8_t *)out.ptr + 3, 5);
    conf_check(&t, rc == AAR_OK, "aar_acquire of an inner range failed");
    rc = aar_publish((uint8_t *)out.ptr + granule, granule);
    conf_check(&t, rc == AAR_OK, "aar_publish of the second granule failed");
    rc = aar_acquire((uint8_t *)out.ptr + granule, granule);
    conf_check(&t, rc == AAR_OK, "aar_acquire of the second granule failed");
    if (coherent) {
        t0 = aar_time_us();
        for (i = 0; i < 5000u; i++) {
            (void)aar_publish(out.ptr, 64);
            (void)aar_acquire(out.ptr, 64);
        }
        dt = aar_time_us() - t0;
        if (dt > 250000u && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "10000 calls on a coherent runtime took ");
            conf_buf_u64(&t.why, dt);
            conf_buf_str(&t.why, " us: not at once");
        }
        conf_buf_str(&t.note, "coherent, 10000 calls in ");
        conf_buf_u64(&t.note, dt);
        conf_buf_str(&t.note, " us");
    } else {
        conf_buf_str(&t.note, "not coherent, ranges accepted");
    }
    (void)aar_shared_free(out.ptr);
    conf_end(&t);
}

/* ---- The tests with the runner as the peer ---------------------------------------------- */

static uint64_t peer_block_bytes(void)
{
    /* The runner takes at most 64 granules and at least 2. */
    if (granule <= 4096u)
        return 64u * granule;
    return 4u * granule;
}

static void peer_failure(struct conf_test *t, const char *what, int rc,
                         const uint32_t ack[4])
{
    if (!conf_fail_first(t))
        return;
    conf_buf_str(&t->why, what);
    if (rc != AAR_OK) {
        conf_buf_str(&t->why, ": no ACK, ");
        text_rc(&t->why, rc);
    } else {
        conf_buf_str(&t->why, ": the runner says ");
        conf_buf_i64(&t->why, (int32_t)ack[0]);
        conf_buf_str(&t->why, " (w1 ");
        conf_buf_u64(&t->why, ack[1]);
        conf_buf_chr(&t->why, ')');
    }
}

/* C-SHARED.9: the client reads what the app wrote. */
static void test_map(void)
{
    struct conf_test t;
    aar_shared out = { 0, 0, 0, 0 };
    uint32_t ack[4] = { 0, 0, 0, 0 };
    uint64_t bytes = peer_block_bytes();
    int rc;

    conf_begin(&t, "C-SHARED.9");
    if (aar_shared_alloc(bytes, 1, 0, &out) != AAR_OK) {
        conf_check(&t, 0, "alloc failed");
        conf_end(&t);
        return;
    }
    (void)write_check(out.ptr, out.bytes, 1);
    (void)aar_publish(out.ptr, out.bytes);
    rc = conf_shared_send(CONF_SHARED_MAP, out.haddr, (uint32_t)out.bytes, 1, 0);
    if (rc != AAR_OK) {
        peer_failure(&t, "the request was not sent", rc, ack);
    } else {
        rc = conf_shared_wait_ack(ack);
        if (rc != AAR_OK || ack[0] != 0)
            peer_failure(&t, "the client did not read the app's pattern", rc, ack);
    }
    (void)aar_shared_free(out.ptr);
    conf_buf_u64(&t.note, out.bytes);
    conf_buf_str(&t.note, " bytes mapped and read by the client");
    conf_end(&t);
}

/* C-SHARED.10: the app reads what the client wrote. */
static void test_fill(void)
{
    struct conf_test t;
    aar_shared out = { 0, 0, 0, 0 };
    uint32_t ack[4] = { 0, 0, 0, 0 };
    uint64_t bytes = peer_block_bytes();
    uint32_t bad;
    int rc;

    conf_begin(&t, "C-SHARED.10");
    if (aar_shared_alloc(bytes, 1, 0, &out) != AAR_OK) {
        conf_check(&t, 0, "alloc failed");
        conf_end(&t);
        return;
    }
    (void)write_check(out.ptr, out.bytes, 5);
    (void)aar_publish(out.ptr, out.bytes);
    rc = conf_shared_send(CONF_SHARED_FILL, out.haddr, (uint32_t)out.bytes, 9, 0);
    if (rc != AAR_OK) {
        peer_failure(&t, "the request was not sent", rc, ack);
    } else {
        rc = conf_shared_wait_ack(ack);
        if (rc != AAR_OK || ack[0] != 0) {
            peer_failure(&t, "the client could not write the block", rc, ack);
        } else {
            (void)aar_acquire(out.ptr, out.bytes);
            bad = conf_shared_check((const volatile uint8_t *)out.ptr, 0,
                                    (uint32_t)out.bytes, 9);
            if (bad != 0xFFFFFFFFu && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "after acquire, byte ");
                conf_buf_u64(&t.why, bad);
                conf_buf_str(&t.why, " is not what the client wrote");
            }
        }
    }
    (void)aar_shared_free(out.ptr);
    conf_buf_u64(&t.note, out.bytes);
    conf_buf_str(&t.note, " bytes written by the client and read by the app");
    conf_end(&t);
}

/* C-SHARED.11: counter and pattern, the client reads while the app writes. */
static void test_seq(void)
{
    struct conf_test t;
    aar_shared out = { 0, 0, 0, 0 };
    uint32_t ack[4] = { 0, 0, 0, 0 };
    uint64_t bytes = round_up(4096u, granule);
    volatile uint8_t *b;
    uint32_t n;
    int rc;

    conf_begin(&t, "C-SHARED.11");
    if (bytes < 2u * granule)
        bytes = 2u * granule;
    if (aar_shared_alloc(bytes, 1, 0, &out) != AAR_OK) {
        conf_check(&t, 0, "alloc failed");
        conf_end(&t);
        return;
    }
    b = (volatile uint8_t *)out.ptr;
    conf_shared_put_be32(b, 0);
    (void)aar_publish(out.ptr, out.bytes);
    rc = conf_shared_send(CONF_SHARED_BEGIN_SEQ, out.haddr, (uint32_t)out.bytes, 0, 0);
    if (rc != AAR_OK) {
        peer_failure(&t, "the request was not sent", rc, ack);
        (void)aar_shared_free(out.ptr);
        conf_end(&t);
        return;
    }
    for (n = 1; n <= CONF_SHARED_SEQ_ROUNDS; n++) {
        conf_shared_put_be32(b, 2u * n - 1u);
        (void)aar_publish(out.ptr, granule);
        conf_shared_fill(b, CONF_SHARED_SEQ_BODY, (uint32_t)out.bytes, n);
        (void)aar_publish(out.ptr, out.bytes);
        conf_shared_put_be32(b, 2u * n);
        (void)aar_publish(out.ptr, granule);
        aar_sleep_us(SEQ_PAUSE_US);
    }
    rc = conf_shared_send(CONF_SHARED_END_SEQ, 2u * CONF_SHARED_SEQ_ROUNDS, 0, 0, 0);
    if (rc != AAR_OK) {
        peer_failure(&t, "the end request was not sent", rc, ack);
    } else {
        rc = conf_shared_wait_ack(ack);
        if (rc != AAR_OK || ack[0] != 0)
            peer_failure(&t, "the client saw stale or torn reads", rc, ack);
        if (rc == AAR_OK && ack[0] != 0) {
            conf_buf_str(&t.why, ", stale ");
            conf_buf_u64(&t.why, ack[1]);
            conf_buf_str(&t.why, " torn ");
            conf_buf_u64(&t.why, ack[2]);
            conf_buf_str(&t.why, " samples ");
            conf_buf_u64(&t.why, ack[3]);
        }
        conf_buf_str(&t.note, "no stale and no torn reads in ");
        conf_buf_u64(&t.note, ack[3]);
        conf_buf_str(&t.note, " samples");
    }
    (void)aar_shared_free(out.ptr);
    conf_end(&t);
}

/* C-SHARED.14: adjacent granules with different writers. */
static void test_granules(void)
{
    struct conf_test t;
    aar_shared out = { 0, 0, 0, 0 };
    uint32_t ack[4] = { 0, 0, 0, 0 };
    uint32_t g = (uint32_t)granule;
    uint64_t bytes = (uint64_t)CONF_SHARED_GRAN_COUNT * g;
    volatile uint8_t *b;
    uint32_t r;
    uint32_t k;
    int rc = AAR_OK;

    conf_begin(&t, "C-SHARED.14");
    if (aar_shared_alloc(bytes, 1, 0, &out) != AAR_OK || out.bytes != bytes) {
        conf_check(&t, 0, "alloc of 8 granules failed");
        if (out.ptr)
            (void)aar_shared_free(out.ptr);
        conf_end(&t);
        return;
    }
    b = (volatile uint8_t *)out.ptr;
    for (k = 0; k < CONF_SHARED_GRAN_COUNT; k++)
        conf_shared_fill(b, k * g, (k + 1u) * g, (k & 1u) ? 1u : 0u);
    (void)aar_publish(out.ptr, out.bytes);
    for (r = 1; r <= CONF_SHARED_GRAN_ROUNDS && !t.failed; r++) {
        for (k = 0; k < CONF_SHARED_GRAN_COUNT; k += 2u) {
            conf_shared_fill(b, k * g, (k + 1u) * g, 2u * r);
            (void)aar_publish((uint8_t *)out.ptr + (uint64_t)k * g, g);
        }
        rc = conf_shared_send(CONF_SHARED_GRAN, out.haddr, (uint32_t)out.bytes, g, r);
        if (rc != AAR_OK) {
            peer_failure(&t, "the request was not sent", rc, ack);
            break;
        }
        rc = conf_shared_wait_ack(ack);
        if (rc != AAR_OK || ack[0] != 0) {
            peer_failure(&t, "the client found its granules wrong or failed", rc, ack);
            break;
        }
        (void)aar_acquire(out.ptr, out.bytes);
        for (k = 0; k < CONF_SHARED_GRAN_COUNT; k++) {
            uint32_t seed = (k & 1u) ? 2u * r + 1u : 2u * r;
            uint32_t bad = conf_shared_check(b, k * g, (k + 1u) * g, seed);

            if (bad != 0xFFFFFFFFu && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "round ");
                conf_buf_u64(&t.why, r);
                conf_buf_str(&t.why, ": granule ");
                conf_buf_u64(&t.why, k);
                conf_buf_str(&t.why, (k & 1u) ? " (the client's)" : " (the app's)");
                conf_buf_str(&t.why, " does not hold the owner's data at byte ");
                conf_buf_u64(&t.why, bad - k * g);
                break;
            }
        }
    }
    (void)aar_shared_free(out.ptr);
    conf_buf_u64(&t.note, CONF_SHARED_GRAN_ROUNDS);
    conf_buf_str(&t.note, " rounds of 8 granules of ");
    conf_buf_u64(&t.note, granule);
    conf_buf_str(&t.note, " bytes, no granule saw the other side's bytes");
    conf_end(&t);
}

/* ---- main ----------------------------------------------------------------------------------- */

int aar_main(int argc, char **argv)
{
    const char *variant = conf_arg(argc, argv, "conf:variant");
    int caps = variant && conf_str_prefix(variant, CONF_SHARED_VARIANT_CAPS) &&
               variant[sizeof CONF_SHARED_VARIANT_CAPS - 1] == '\0';
    int peer = conf_have_peer(argc, argv);

    info.size = sizeof info;
    (void)aar_get_info(&info);
    granule = info.shared_granule ? info.shared_granule : 64u;

    if (caps) {
        test_caps();
        return conf_finish("C-SHARED");
    }
    test_flags();
    test_grant();
    test_free();
    test_live();
    test_live_services();
    test_until_failure();
    test_used();
    /* C-SHARED.8 is judged in the variant caps; a run without a variant is not
     * a platform run. */
    if (!variant)
        conf_skip("C-SHARED.8", "host: no client sets the shared limit");
    test_visibility_calls();
    if (peer) {
        test_map();
        test_fill();
        test_seq();
        test_granules();
    } else {
        conf_skip("C-SHARED.9", "host: no client to map the block");
        conf_skip("C-SHARED.10", "host: no client to write the block");
        conf_skip("C-SHARED.11", "host: no client to read while the app writes");
        conf_skip("C-SHARED.14", "host: no client to own the odd granules");
    }
    return conf_finish("C-SHARED");
}
