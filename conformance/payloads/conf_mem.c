/*
 * Conformance payload for C-MEM (spec 4.1, 4.4): the private heap. See
 * conformance/TESTS.md for the ids and conformance/payloads/conf.h for the
 * protocol; conf_mem.h has the variants.
 *
 * The note asks for 2 MiB of private heap at load (C-MEM.10). Every test leaves
 * the heap as it found it, so the later tests see the same start. Run without a
 * runner (host runner, qemu loader) the payload does the `main` tests that make
 * sense there and skips C-MEM.9 and C-MEM.12, which need the runner's tags and a
 * pool that can be exhausted.
 *
 * Assumption of C-MEM.12 for the card run: with no client cap the limit is the
 * pool, so a heap_grow loop exhausts the pool and the runtime raises its
 * low-water event.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"
#include "conf_mem.h"

AAR_NOTE(0, 2, 0, 0)

#if (defined(__arm__) || defined(__aarch64__)) && !defined(AAR_HOST)
#define MEM_NOTE 1 /* the note is honoured: the runtime commits min_heap_mib at load */
#endif

#define MIB ((uint64_t)1024u * 1024u)
#define GRAN ((uint64_t)AAR_HEAP_GRANULE)
#define NOTE_HEAP_BYTES (2u * MIB)

enum { MODE_HOST, MODE_MAIN, MODE_CAPS, MODE_LOWMEM };

/* ---- Helpers ------------------------------------------------------------------------ */

static uint8_t *heap_base_now(void)
{
    void *base = 0;
    uint64_t len = 0;

    aar_heap_range(&base, &len);
    return (uint8_t *)base;
}

static uint64_t heap_len_now(void)
{
    void *base = 0;
    uint64_t len = 0;

    aar_heap_range(&base, &len);
    return len;
}

static uint64_t heap_end_now(void)
{
    void *base = 0;
    uint64_t len = 0;

    aar_heap_range(&base, &len);
    return (uint64_t)(uintptr_t)base + len;
}

/* Fills the info with values no runtime reports, then asks. */
static int heap_info_now(aar_heap_info *hi)
{
    hi->committed = 0xDEADBEEFDEADBEEFull;
    hi->limit = 0xDEADBEEFDEADBEEFull;
    hi->pool_free = 0xDEADBEEFDEADBEEFull;
    return aar_get_heap_info(hi);
}

static void put_u64(struct conf_buf *b, uint64_t v)
{
    conf_buf_u64(b, v);
}

static void put_mib(struct conf_buf *b, uint64_t v)
{
    conf_buf_u64(b, v / MIB);
    conf_buf_str(b, " MiB");
}

/* Index of the first byte that differs from `value` among the first `bytes`
 * (a multiple of 4), or -1. */
static int64_t first_not(const uint8_t *p, uint64_t bytes, uint32_t value)
{
    const volatile uint32_t *w = (const volatile uint32_t *)(const void *)p;
    uint64_t i;

    for (i = 0; i < bytes / 4u; i++) {
        if (w[i] != value)
            return (int64_t)(i * 4u);
    }
    return -1;
}

static void fill_words(uint8_t *p, uint64_t bytes, uint32_t value)
{
    volatile uint32_t *w = (volatile uint32_t *)(void *)p;
    uint64_t i;

    for (i = 0; i < bytes / 4u; i++)
        w[i] = value;
}

/* A grow that must succeed. Returns the old break through *old. */
static int grow_ok(struct conf_test *t, uint64_t bytes, uint8_t **old, const char *what)
{
    void *ob = 0;
    int rc = aar_heap_grow(bytes, &ob);

    if (rc != AAR_OK) {
        if (conf_fail_first(t)) {
            conf_buf_str(&t->why, what);
            conf_buf_str(&t->why, ": heap_grow returned ");
            conf_buf_i64(&t->why, rc);
        }
        return 0;
    }
    *old = (uint8_t *)ob;
    return 1;
}

static int shrink_ok(struct conf_test *t, uint64_t bytes, uint8_t **nb, const char *what)
{
    void *b = 0;
    int rc = aar_heap_shrink(bytes, &b);

    if (rc != AAR_OK) {
        if (conf_fail_first(t)) {
            conf_buf_str(&t->why, what);
            conf_buf_str(&t->why, ": heap_shrink returned ");
            conf_buf_i64(&t->why, rc);
        }
        return 0;
    }
    *nb = (uint8_t *)b;
    return 1;
}

/* The heap must hold at least two granules for the tests that shrink; the host
 * runner starts with none. */
static void ensure_heap(void)
{
    uint64_t len = heap_len_now();
    void *ob = 0;

    if (len < 2u * GRAN)
        (void)aar_heap_grow(2u * GRAN - len, &ob);
}

/* ---- C-MEM.1 to C-MEM.7 --------------------------------------------------------------- */

static void test_range_info(void)
{
    struct conf_test t;
    void *base = 0;
    uint64_t len = 0;
    aar_heap_info hi;
    int rc;

    conf_begin(&t, "C-MEM.1");
    aar_heap_range(&base, &len);
    rc = heap_info_now(&hi);
    conf_check(&t, rc == AAR_OK, "heap_info failed");
    conf_check(&t, base != 0, "heap_range returned a NULL base");
    if (rc == AAR_OK && len != hi.committed && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "heap_range length ");
        put_u64(&t.why, len);
        conf_buf_str(&t.why, " differs from heap_info.committed ");
        put_u64(&t.why, hi.committed);
    }
    conf_buf_str(&t.note, "length ");
    put_mib(&t.note, len);
    conf_end(&t);
}

static void test_grow_one(void)
{
    struct conf_test t;
    uint8_t *base0 = heap_base_now();
    uint64_t len0 = heap_len_now();
    uint8_t *ob = 0;
    aar_heap_info hi;
    uint8_t *nb = 0;

    conf_begin(&t, "C-MEM.2");
    if (grow_ok(&t, GRAN, &ob, "one granule")) {
        conf_check(&t, ob == base0 + len0,
                   "the old break is not the previous end (base plus length)");
        conf_check(&t, heap_len_now() == len0 + GRAN,
                   "heap_range did not grow by one granule");
        conf_check(&t, heap_info_now(&hi) == AAR_OK && hi.committed == len0 + GRAN,
                   "heap_info.committed did not grow by one granule");
        (void)shrink_ok(&t, GRAN, &nb, "undoing the grow");
    }
    conf_end(&t);
}

static void test_contiguous(void)
{
    struct conf_test t;
    uint64_t end0 = heap_end_now();
    uint8_t *ob = 0;
    uint32_t i;
    uint32_t grown = 0;

    conf_begin(&t, "C-MEM.3");
    for (i = 0; i < 4; i++) {
        uint64_t before = heap_end_now();

        if (!grow_ok(&t, GRAN, &ob, "a successive grow"))
            break;
        grown++;
        if ((uint64_t)(uintptr_t)ob != before && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "grow ");
            conf_buf_u64(&t.why, i);
            conf_buf_str(&t.why, " returned an old break that is not the previous end");
        }
        if (heap_end_now() != before + GRAN && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "the end did not move by one granule after grow ");
            conf_buf_u64(&t.why, i);
        }
    }
    if (grown) {
        uint8_t *nb = 0;

        (void)shrink_ok(&t, (uint64_t)grown * GRAN, &nb, "undoing the grows");
    }
    conf_check(&t, heap_end_now() == end0, "the heap did not return to its end");
    conf_end(&t);
}

static void test_zeroed(void)
{
    struct conf_test t;
    uint8_t *ob = 0;
    uint8_t *ob2 = 0;
    uint8_t *nb = 0;
    int64_t bad;

    conf_begin(&t, "C-MEM.4");
    if (grow_ok(&t, 2u * GRAN, &ob, "two granules")) {
        bad = first_not(ob, 2u * GRAN, 0u);
        if (bad >= 0 && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "new heap memory is not zero at offset ");
            conf_buf_i64(&t.why, bad);
        }
        fill_words(ob, 2u * GRAN, 0xA5A5A5A5u);
        if (shrink_ok(&t, 2u * GRAN, &nb, "the shrink") &&
            grow_ok(&t, 2u * GRAN, &ob2, "the regrow")) {
            conf_check(&t, ob2 == ob, "the regrow did not return the same range");
            bad = first_not(ob2, 2u * GRAN, 0u);
            if (bad >= 0 && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "memory grown again after a shrink is not zero at offset ");
                conf_buf_i64(&t.why, bad);
            }
            (void)shrink_ok(&t, 2u * GRAN, &nb, "undoing the regrow");
        }
    }
    conf_buf_str(&t.note, "2 MiB zero when new and after a shrink and regrow");
    conf_end(&t);
}

static void test_no_move(void)
{
    struct conf_test t;
    uint8_t *a = 0;
    uint8_t *b = 0;
    uint8_t *nb = 0;
    uint8_t *base0 = heap_base_now();

    conf_begin(&t, "C-MEM.5");
    /* The data lives in memory this test grew itself, so nothing a libc may
     * keep in the start-up heap is touched. */
    if (grow_ok(&t, GRAN, &a, "the first grow")) {
        fill_words(a, GRAN, 0x5A5A1234u);
        if (grow_ok(&t, 2u * GRAN, &b, "the second grow")) {
            int64_t bad = first_not(a, GRAN, 0x5A5A1234u);

            conf_check(&t, heap_base_now() == base0, "heap_range base moved after a grow");
            if (bad >= 0 && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "data written before a grow changed at offset ");
                conf_buf_i64(&t.why, bad);
            }
            (void)shrink_ok(&t, 2u * GRAN, &nb, "undoing the second grow");
        }
        (void)shrink_ok(&t, GRAN, &nb, "undoing the first grow");
    }
    conf_end(&t);
}

static void test_shrink(void)
{
    struct conf_test t;
    uint8_t *ob = 0;
    uint8_t *nb = 0;
    uint64_t len0 = heap_len_now();
    uint64_t end0 = heap_end_now();
    aar_heap_info hi;

    conf_begin(&t, "C-MEM.6");
    if (grow_ok(&t, 2u * GRAN, &ob, "two granules")) {
        if (shrink_ok(&t, GRAN, &nb, "a shrink of one granule")) {
            conf_check(&t, (uint64_t)(uintptr_t)nb == end0 + 2u * GRAN - GRAN,
                       "the new break is not the old end minus the request");
            conf_check(&t, heap_len_now() == len0 + GRAN,
                       "heap_range did not shrink by one granule");
            conf_check(&t, heap_info_now(&hi) == AAR_OK && hi.committed == len0 + GRAN,
                       "heap_info.committed did not shrink by one granule");
        }
        if (shrink_ok(&t, GRAN, &nb, "the second shrink"))
            conf_check(&t, (uint64_t)(uintptr_t)nb == end0, "the second new break is wrong");
    }
    conf_check(&t, heap_end_now() == end0 && heap_len_now() == len0,
               "the heap did not return to its start");
    conf_end(&t);
}

static void test_not_whole(void)
{
    struct conf_test t;
    static const uint64_t sizes[] = { 1u, 4096u, GRAN - 1u, GRAN + 1u, GRAN + GRAN / 2u };
    uint64_t len0 = heap_len_now();
    uint64_t end0 = heap_end_now();
    uint32_t i;

    conf_begin(&t, "C-MEM.7");
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        void *p = 0;
        int rc = aar_heap_grow(sizes[i], &p);

        if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "heap_grow of ");
            conf_buf_u64(&t.why, sizes[i]);
            conf_buf_str(&t.why, " bytes returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_INVAL");
        }
        if (rc == AAR_OK) {
            /* Undo what the runtime wrongly granted, so the rest is judged. */
            uint8_t *nb = 0;

            (void)aar_heap_shrink(heap_len_now() - len0, (void **)&nb);
        }
        p = 0;
        rc = aar_heap_shrink(sizes[i], &p);
        if (rc != AAR_E_INVAL && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "heap_shrink of ");
            conf_buf_u64(&t.why, sizes[i]);
            conf_buf_str(&t.why, " bytes returned ");
            conf_buf_i64(&t.why, rc);
            conf_buf_str(&t.why, ", not AAR_E_INVAL");
        }
        if (heap_len_now() != len0 || heap_end_now() != end0) {
            if (conf_fail_first(&t))
                conf_buf_str(&t.why, "a refused call changed the heap");
            break;
        }
    }
    conf_end(&t);
}

/* ---- C-MEM.8: grow until failure ------------------------------------------------------------ */

/* Grows in steps of 16, 4 and 1 MiB until a step fails; each step that fails
 * with AAR_E_NOMEM is retried smaller. *rc is the last failure code. With a test,
 * the committed size is checked against the limit after every grow. */
static void exhaust(struct conf_test *t, uint64_t *added, int *rc_out, int poll_events);

static void drain_events(void);

static void exhaust(struct conf_test *t, uint64_t *added, int *rc_out, int poll_events)
{
    static const uint64_t steps[3] = { 16u * MIB, 4u * MIB, 1u * MIB };
    uint64_t total = 0;
    int rc = AAR_OK;
    uint32_t s;

    for (s = 0; s < 3; s++) {
        for (;;) {
            void *ob = 0;
            aar_heap_info hi;

            rc = aar_heap_grow(steps[s], &ob);
            if (rc != AAR_OK)
                break;
            total += steps[s];
            if (t && heap_info_now(&hi) == AAR_OK && hi.committed > hi.limit &&
                conf_fail_first(t)) {
                conf_buf_str(&t->why, "committed ");
                put_u64(&t->why, hi.committed);
                conf_buf_str(&t->why, " exceeds the limit ");
                put_u64(&t->why, hi.limit);
            }
            if (poll_events)
                drain_events();
        }
        if (rc != AAR_E_NOMEM)
            break;
    }
    *added = total;
    *rc_out = rc;
}

static void test_exhaust(void)
{
    struct conf_test t;
    uint64_t added = 0;
    uint64_t len0 = heap_len_now();
    uint64_t end0 = heap_end_now();
    uint8_t *nb = 0;
    aar_heap_info hi;
    int rc = AAR_OK;

    conf_begin(&t, "C-MEM.8");
    exhaust(&t, &added, &rc, 0);
    if (rc != AAR_E_NOMEM && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the grow that ended the loop returned ");
        conf_buf_i64(&t.why, rc);
        conf_buf_str(&t.why, ", not AAR_E_NOMEM");
    }
    if (heap_info_now(&hi) == AAR_OK) {
        conf_check(&t, hi.committed <= hi.limit, "committed exceeds limit at the end");
        conf_check(&t, hi.committed == len0 + added,
                   "heap_info.committed is not the start plus what was granted");
        conf_buf_str(&t.note, "grew ");
        put_mib(&t.note, added);
        conf_buf_str(&t.note, ", limit ");
        put_mib(&t.note, hi.limit);
        conf_buf_str(&t.note, ", then AAR_E_NOMEM");
    }
    if (added)
        (void)shrink_ok(&t, added, &nb, "releasing the grown heap");
    conf_check(&t, heap_end_now() == end0 && heap_len_now() == len0,
               "the heap did not return to its start");
    conf_end(&t);
}

/* ---- C-MEM.9: the client's cap (variant caps) -------------------------------------------------- */

static void test_caps(int argc, char **argv)
{
    struct conf_test t;
    uint32_t caps_mib = CONF_MEM_CAPS_MIB;
    uint32_t cap_mib = 0;
    uint64_t added = 0;
    uint8_t *nb = 0;
    aar_heap_info hi;
    int rc = AAR_OK;
    uint64_t len0 = heap_len_now();

    conf_begin(&t, "C-MEM.9");
    (void)conf_arg_u32(argc, argv, "conf:caps_mib", &caps_mib);
    (void)conf_arg_u32(argc, argv, "conf:cap_mib", &cap_mib);
    if (!conf_check(&t, heap_info_now(&hi) == AAR_OK, "heap_info failed")) {
        conf_end(&t);
        return;
    }
    if (hi.limit > (uint64_t)caps_mib * MIB && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "limit ");
        put_mib(&t.why, hi.limit);
        conf_buf_str(&t.why, " is above the client's cap of ");
        conf_buf_u64(&t.why, caps_mib);
        conf_buf_str(&t.why, " MiB");
    }
    if ((cap_mib == 0 || cap_mib >= caps_mib) && hi.limit != (uint64_t)caps_mib * MIB &&
        conf_fail_first(&t)) {
        conf_buf_str(&t.why, "limit ");
        put_mib(&t.why, hi.limit);
        conf_buf_str(&t.why, " is not the client's cap of ");
        conf_buf_u64(&t.why, caps_mib);
        conf_buf_str(&t.why, " MiB");
        if (cap_mib) {
            conf_buf_str(&t.why, " although the platform's own limit is ");
            conf_buf_u64(&t.why, cap_mib);
            conf_buf_str(&t.why, " MiB");
        }
    }
    exhaust(&t, &added, &rc, 0);
    if (rc != AAR_E_NOMEM && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "a grow past the cap returned ");
        conf_buf_i64(&t.why, rc);
        conf_buf_str(&t.why, ", not AAR_E_NOMEM");
    }
    if (heap_info_now(&hi) == AAR_OK) {
        if (hi.committed > (uint64_t)caps_mib * MIB && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "the heap grew to ");
            put_mib(&t.why, hi.committed);
            conf_buf_str(&t.why, ", past the cap");
        }
        conf_buf_str(&t.note, "cap ");
        conf_buf_u64(&t.note, caps_mib);
        conf_buf_str(&t.note, " MiB, limit ");
        put_mib(&t.note, hi.limit);
        conf_buf_str(&t.note, ", committed ");
        put_mib(&t.note, hi.committed);
    }
    if (added)
        (void)shrink_ok(&t, added, &nb, "releasing the grown heap");
    conf_check(&t, heap_len_now() == len0, "the heap did not return to its start");
    conf_end(&t);
}

/* ---- C-MEM.10, C-MEM.11, C-MEM.13 --------------------------------------------------------------- */

static void test_min_heap(uint64_t len_at_entry)
{
#ifdef MEM_NOTE
    struct conf_test t;

    conf_begin(&t, "C-MEM.10");
    if (len_at_entry < NOTE_HEAP_BYTES && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the heap is ");
        put_u64(&t.why, len_at_entry);
        conf_buf_str(&t.why, " bytes at entry, the note asks for 2 MiB");
    }
    conf_buf_str(&t.note, "heap ");
    put_mib(&t.note, len_at_entry);
    conf_buf_str(&t.note, " at entry");
    conf_end(&t);
#else
    (void)len_at_entry;
    conf_skip("C-MEM.10", "host: the host runner reads no AAR note and commits no heap");
#endif
}

static void test_pool_free(void)
{
    struct conf_test t;
    aar_heap_info h0, h1, h2;
    uint8_t *ob = 0;
    uint8_t *nb = 0;
    int rc0, rc1, rc2;

    conf_begin(&t, "C-MEM.11");
    rc0 = heap_info_now(&h0);
    if (grow_ok(&t, GRAN, &ob, "one granule")) {
        rc1 = heap_info_now(&h1);
        if (shrink_ok(&t, GRAN, &nb, "the shrink")) {
            rc2 = heap_info_now(&h2);
            if (rc0 == AAR_OK && rc1 == AAR_OK && rc2 == AAR_OK) {
                if (!(h1.pool_free < h0.pool_free) && conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "pool_free did not fall when the heap grew: ");
                    put_u64(&t.why, h0.pool_free);
                    conf_buf_str(&t.why, " then ");
                    put_u64(&t.why, h1.pool_free);
                }
                if (!(h2.pool_free > h1.pool_free) && conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "pool_free did not rise when the heap shrank: ");
                    put_u64(&t.why, h1.pool_free);
                    conf_buf_str(&t.why, " then ");
                    put_u64(&t.why, h2.pool_free);
                }
            } else {
                conf_check(&t, 0, "heap_info failed");
            }
        }
    }
    conf_end(&t);
}

static void test_isolated(void)
{
    struct conf_test t;
    aar_info info;
#if UINTPTR_MAX > 0xffffffffu
    void *bad = (void *)(uintptr_t)0xFFFF000000001000ull;
#else
    void *bad = (void *)(uintptr_t)0xFFFFF000u;
#endif

    conf_begin(&t, "C-MEM.13");
    info.size = (uint32_t)sizeof info;
    info.flags = 0;
    if (!conf_check(&t, aar_get_info(&info) == AAR_OK, "aar_get_info failed")) {
        conf_end(&t);
        return;
    }
    if ((info.flags & AAR_INFOF_ISOLATED) == 0) {
        conf_buf_str(&t.note, "not isolated");
        conf_end(&t);
        return;
    }
    {
        uint32_t n = 0;
        int rc1 = aar_get_heap_info((aar_heap_info *)bad);
        int rc2 = aar_console_write(bad, 16u, &n);
        int rc3 = aar_msg_poll(bad);

        if ((rc1 != AAR_E_FAULT || rc2 != AAR_E_FAULT || rc3 != AAR_E_FAULT) &&
            conf_fail_first(&t)) {
            conf_buf_str(&t.why, "bad pointers gave heap_info ");
            conf_buf_i64(&t.why, rc1);
            conf_buf_str(&t.why, ", console_write ");
            conf_buf_i64(&t.why, rc2);
            conf_buf_str(&t.why, ", msg_poll ");
            conf_buf_i64(&t.why, rc3);
            conf_buf_str(&t.why, ", not AAR_E_FAULT");
        }
    }
    conf_buf_str(&t.note, "isolated: bad pointers gave AAR_E_FAULT");
    conf_end(&t);
}

/* ---- C-MEM.12: low memory (variant lowmem) ------------------------------------------------------- */

static uint32_t low_events;
static int low_fields_ok = 1;

static void drain_events(void)
{
    aar_event ev[8];
    uint32_t n;
    uint32_t i;

    do {
        n = 0;
        if (aar_event_poll(ev, 8u, &n) != AAR_OK)
            return;
        for (i = 0; i < n; i++) {
            if (ev[i].type != AAR_EV_LOW_MEMORY)
                continue;
            low_events++;
            if (ev[i].code || ev[i].dx || ev[i].dy || ev[i].buttons || ev[i].reserved ||
                ev[i].time_ms)
                low_fields_ok = 0;
        }
    } while (n == 8u);
}

static void sleep_and_drain(uint32_t ms)
{
    aar_sleep_us(ms * 1000u);
    drain_events();
}

static void test_low_memory(void)
{
    struct conf_test t;
    uint64_t added1 = 0;
    uint64_t added2 = 0;
    uint8_t *nb = 0;
    uint64_t len0 = heap_len_now();
    uint32_t c1, c1b, c2;
    int rc1 = AAR_OK;
    int rc2 = AAR_OK;

    conf_begin(&t, "C-MEM.12");
    drain_events();
    low_events = 0; /* anything pending before this test is not its business */

    exhaust(0, &added1, &rc1, 1);
    sleep_and_drain(100);
    c1 = low_events;
    /* Still exhausted: no second event for the same crossing. */
    sleep_and_drain(100);
    sleep_and_drain(100);
    sleep_and_drain(100);
    c1b = low_events;
    if (added1)
        (void)shrink_ok(&t, added1, &nb, "releasing the pool");
    sleep_and_drain(100);

    exhaust(0, &added2, &rc2, 1);
    sleep_and_drain(100);
    c2 = low_events;
    if (added2)
        (void)shrink_ok(&t, added2, &nb, "releasing the pool again");

    conf_check(&t, rc1 == AAR_E_NOMEM && rc2 == AAR_E_NOMEM,
               "the grow loop did not end with AAR_E_NOMEM");
    if (c1 == 0 && conf_fail_first(&t))
        conf_buf_str(&t.why, "no AAR_EV_LOW_MEMORY event after the pool was exhausted");
    if (c1 > 1 && conf_fail_first(&t)) {
        conf_buf_u64(&t.why, c1);
        conf_buf_str(&t.why, " AAR_EV_LOW_MEMORY events for one crossing");
    }
    if (c1b != c1 && conf_fail_first(&t))
        conf_buf_str(&t.why, "a second event arrived while the pool stayed exhausted");
    if (c1 == 1 && c1b == 1 && c2 != 2 && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "after the pool was released and exhausted again there were ");
        conf_buf_u64(&t.why, c2);
        conf_buf_str(&t.why, " events in all, not 2");
    }
    conf_check(&t, low_fields_ok, "an AAR_EV_LOW_MEMORY event has a field other than type that is not 0");
    conf_check(&t, heap_len_now() == len0, "the heap did not return to its start");
    conf_buf_str(&t.note, "pool ");
    put_mib(&t.note, added1);
    conf_buf_str(&t.note, " exhausted twice: ");
    conf_buf_u64(&t.note, c1);
    conf_buf_str(&t.note, " event, then ");
    conf_buf_u64(&t.note, c2 - c1);
    conf_end(&t);
}

/* ---- Peer: the limit for the runner ---------------------------------------------------------------- */

static void send_limit(uint64_t limit)
{
    uint8_t msg[AAR_MSG_BYTES];
    uint32_t i;
    const char *kw = CONF_MSG_MEM_LIMIT;
    uint64_t start;
    int rc;

    for (i = 0; i < AAR_MSG_BYTES; i++)
        msg[i] = 0;
    for (i = 0; kw[i] && i < CONF_MEM_LIMIT_OFFSET; i++)
        msg[i] = (uint8_t)kw[i];
    for (i = 0; i < 8; i++)
        msg[CONF_MEM_LIMIT_OFFSET + i] = (uint8_t)(limit >> (8u * i));
    start = aar_time_us();
    do {
        rc = aar_msg_send(msg);
        if (rc != AAR_E_BUSY)
            break;
        aar_sleep_us(1000);
    } while (aar_time_us() - start < 100000u);
}

/* ---- main ---------------------------------------------------------------------------------------------- */

static int variant_is(const char *v, const char *name)
{
    return v && conf_str_prefix(v, name) && conf_str_prefix(name, v);
}

int aar_main(int argc, char **argv)
{
    const char *variant = conf_arg(argc, argv, "conf:variant");
    uint64_t len_at_entry = heap_len_now();
    aar_heap_info hi;
    int mode;

    if (variant_is(variant, "main"))
        mode = MODE_MAIN;
    else if (variant_is(variant, "caps"))
        mode = MODE_CAPS;
    else if (variant_is(variant, "lowmem"))
        mode = MODE_LOWMEM;
    else
        mode = MODE_HOST;

    if (mode == MODE_CAPS) {
        test_caps(argc, argv);
        return conf_finish("C-MEM");
    }
    if (mode == MODE_LOWMEM) {
        test_low_memory();
        return conf_finish("C-MEM");
    }

    test_min_heap(len_at_entry);
    ensure_heap();
    test_range_info();
    test_grow_one();
    test_contiguous();
    test_zeroed();
    test_no_move();
    test_shrink();
    test_not_whole();
    test_pool_free();
    test_isolated();
    if (mode == MODE_HOST) {
        conf_skip("C-MEM.9", "host: needs the runner's AAR_TAG_PRIVATE_MIB");
        conf_skip("C-MEM.12", "host: the pool cannot be exhausted here, and a low-water crossing cannot be provoked");
    } else if (heap_info_now(&hi) == AAR_OK) {
        send_limit(hi.limit);
    }
    test_exhaust();
    return conf_finish("C-MEM");
}
