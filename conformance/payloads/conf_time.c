/*
 * Conformance payload for C-TIME (spec 7.3, 3.7): monotonic time and sleep.
 * See conformance/TESTS.md for the ids and conformance/payloads/conf.h for the
 * protocol.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

#include "conf.h"

AAR_NOTE(0, 0, 0, 0)

#define READS 200000u

/* C-TIME.1: time never goes backwards, over many calls and over a sleep. */
static void test_monotonic(void)
{
    struct conf_test t;
    uint64_t prev = aar_time_us();
    uint64_t first = prev;
    uint64_t now;
    uint32_t i;

    conf_begin(&t, "C-TIME.1");
    for (i = 0; i < READS; i++) {
        now = aar_time_us();
        if (now < prev && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "went back at read ");
            conf_buf_u64(&t.why, i);
            conf_buf_str(&t.why, ": ");
            conf_buf_u64(&t.why, prev);
            conf_buf_str(&t.why, " then ");
            conf_buf_u64(&t.why, now);
            break;
        }
        prev = now;
    }
    aar_sleep_us(20000);
    now = aar_time_us();
    if (now < prev && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "went back over a 20 ms sleep: ");
        conf_buf_u64(&t.why, prev);
        conf_buf_str(&t.why, " then ");
        conf_buf_u64(&t.why, now);
    }
    conf_buf_u64(&t.note, READS);
    conf_buf_str(&t.note, " reads and a sleep, ");
    conf_buf_u64(&t.note, now - first);
    conf_buf_str(&t.note, " us in all");
    conf_end(&t);
}

/* C-TIME.2: resolution of 1 ms or finer: the smallest step between two
 * different readings over 50 ms. */
static void test_resolution(void)
{
    struct conf_test t;
    uint64_t start = aar_time_us();
    uint64_t prev = start;
    uint64_t now = start;
    uint64_t min_step = 0;
    uint32_t steps = 0;

    conf_begin(&t, "C-TIME.2");
    while (now - start < 50000u) {
        now = aar_time_us();
        if (now > prev) {
            uint64_t step = now - prev;

            if (steps == 0 || step < min_step)
                min_step = step;
            steps++;
            prev = now;
        }
    }
    if (steps == 0) {
        conf_check(&t, 0, "the clock did not change in 50 ms");
    } else if (min_step > 1000u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "smallest step ");
        conf_buf_u64(&t.why, min_step);
        conf_buf_str(&t.why, " us is coarser than 1 ms");
    }
    conf_buf_str(&t.note, "smallest step ");
    conf_buf_u64(&t.note, min_step);
    conf_buf_str(&t.note, " us in ");
    conf_buf_u64(&t.note, steps);
    conf_buf_str(&t.note, " steps");
    conf_end(&t);
}

static const uint32_t sleeps_us[] = { 1000u, 10000u, 50000u };
#define SLEEP_COUNT (sizeof sleeps_us / sizeof sleeps_us[0])

/* C-TIME.3: a sleep lasts at least the time asked for. */
static void test_sleep_floor(void)
{
    struct conf_test t;
    uint32_t i;
    uint32_t round;

    conf_begin(&t, "C-TIME.3");
    for (i = 0; i < SLEEP_COUNT; i++) {
        for (round = 0; round < 5u; round++) {
            uint64_t t0 = aar_time_us();
            uint64_t elapsed;

            aar_sleep_us(sleeps_us[i]);
            elapsed = aar_time_us() - t0;
            if (elapsed < sleeps_us[i] && conf_fail_first(&t)) {
                conf_buf_str(&t.why, "sleep of ");
                conf_buf_u64(&t.why, sleeps_us[i]);
                conf_buf_str(&t.why, " us lasted ");
                conf_buf_u64(&t.why, elapsed);
                conf_buf_str(&t.why, " us");
            }
        }
    }
    conf_buf_str(&t.note, "1 ms, 10 ms and 50 ms, 5 rounds each");
    conf_end(&t);
}

/* C-TIME.4: a sleep takes no more than the time asked for plus four sleep
 * granules (at least 1 ms each) plus 10 ms. The best of five rounds counts, so
 * a busy machine does not fail the test. */
static void test_sleep_ceiling(void)
{
    struct conf_test t;
    aar_info info;
    uint64_t granule = 1000u;
    uint32_t i;
    uint32_t round;

    conf_begin(&t, "C-TIME.4");
    info.size = sizeof info;
    if (aar_get_info(&info) == AAR_OK && info.sleep_granule_us > granule)
        granule = info.sleep_granule_us;
    for (i = 0; i < SLEEP_COUNT; i++) {
        uint64_t best = ~(uint64_t)0;
        uint64_t limit = (uint64_t)sleeps_us[i] + 4u * granule + 10000u;

        for (round = 0; round < 5u; round++) {
            uint64_t t0 = aar_time_us();
            uint64_t elapsed;

            aar_sleep_us(sleeps_us[i]);
            elapsed = aar_time_us() - t0;
            if (elapsed < best)
                best = elapsed;
        }
        if (best > limit && conf_fail_first(&t)) {
            conf_buf_str(&t.why, "best sleep of ");
            conf_buf_u64(&t.why, sleeps_us[i]);
            conf_buf_str(&t.why, " us lasted ");
            conf_buf_u64(&t.why, best);
            conf_buf_str(&t.why, " us, limit ");
            conf_buf_u64(&t.why, limit);
        }
    }
    conf_buf_str(&t.note, "granule ");
    conf_buf_u64(&t.note, granule);
    conf_buf_str(&t.note, " us");
    conf_end(&t);
}

/* C-TIME.5: a stop request ends a long sleep early (spec 3.7). The runner is the
 * peer: it stops the app when it gets the keyword message. This test comes
 * last, because the app is on its way out afterwards. */
static void test_sleep_stop(int argc, char **argv)
{
    struct conf_test t;
    uint64_t t0;
    uint64_t elapsed;
    int rc;

    if (!conf_have_peer(argc, argv)) {
        conf_skip("C-TIME.5", "host: no runner to stop the app");
        return;
    }
    conf_begin(&t, "C-TIME.5");
    rc = conf_peer_send(CONF_MSG_STOP);
    if (rc != AAR_OK) {
        conf_check(&t, 0, "the stop request message was not accepted");
        conf_end(&t);
        return;
    }
    t0 = aar_time_us();
    aar_sleep_us(10000000u);
    elapsed = aar_time_us() - t0;
    conf_check(&t, aar_should_stop() == 1, "should_stop is 0 after the sleep");
    if (elapsed >= 9000000u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the sleep ran ");
        conf_buf_u64(&t.why, elapsed);
        conf_buf_str(&t.why, " us and was not ended by the stop");
    }
    conf_buf_str(&t.note, "a 10 s sleep ended after ");
    conf_buf_u64(&t.note, elapsed);
    conf_buf_str(&t.note, " us");
    conf_end(&t);
}

int aar_main(int argc, char **argv)
{
    test_monotonic();
    test_resolution();
    test_sleep_floor();
    test_sleep_ceiling();
    test_sleep_stop(argc, argv);
    return conf_finish("C-TIME");
}
