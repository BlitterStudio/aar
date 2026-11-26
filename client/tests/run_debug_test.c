/*
 * Host tests of what aar-run does for debugging and profiling (client/run/
 * aar_run_debug.h): the options, the register lines, the crash report and the
 * profile file header. The crash report is compared with the sample file that
 * the Python tests of tools/aar-symbolize read too (tests/data/crash-sample.txt),
 * so that the writer and the reader agree.
 *
 * Run from the repository root: it opens tests/data/crash-sample.txt.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar_run_debug.h"

#define SAMPLE_PATH "tests/data/crash-sample.txt"

static void test_profile_option(void)
{
    struct aar_run_debug d;

    memset(&d, 0, sizeof(d));
    assert(aar_run_debug_arg("PROFILE=100", &d) == AAR_RUN_ARG_TAKEN && d.profile_hz == 100u);
    assert(aar_run_debug_arg("profile=250", &d) == AAR_RUN_ARG_TAKEN && d.profile_hz == 250u);
    assert(aar_run_debug_arg("PROFILE=10000", &d) == AAR_RUN_ARG_TAKEN && d.profile_hz == 10000u);
    /* A bad value leaves the setting as it was. */
    assert(aar_run_debug_arg("PROFILE=10001", &d) == AAR_RUN_ARG_BAD && d.profile_hz == 10000u);
    assert(aar_run_debug_arg("PROFILE=0", &d) == AAR_RUN_ARG_BAD);
    assert(aar_run_debug_arg("PROFILE=", &d) == AAR_RUN_ARG_BAD);
    assert(aar_run_debug_arg("PROFILE=12x", &d) == AAR_RUN_ARG_BAD);
    assert(aar_run_debug_arg("PROFILE=-5", &d) == AAR_RUN_ARG_BAD);
    assert(d.profile_hz == 10000u);
    /* PROFILEFILE is not PROFILE with a longer name. */
    memset(&d, 0, sizeof(d));
    assert(aar_run_debug_arg("PROFILEFILE=RAM:p.bin", &d) == AAR_RUN_ARG_TAKEN);
    assert(d.profile_hz == 0u && strcmp(d.profilefile, "RAM:p.bin") == 0);
}

static void test_path_options(void)
{
    struct aar_run_debug d;
    char longpath[AAR_RUN_DEBUG_PATH_MAX + 16];

    memset(&d, 0, sizeof(d));
    assert(aar_run_debug_arg("CRASHLOG=RAM:crash.txt", &d) == AAR_RUN_ARG_TAKEN);
    assert(strcmp(d.crashlog, "RAM:crash.txt") == 0 && d.profilefile[0] == '\0');
    assert(aar_run_debug_arg("crashlog=Work:a b/c.txt", &d) == AAR_RUN_ARG_TAKEN);
    assert(strcmp(d.crashlog, "Work:a b/c.txt") == 0);
    assert(aar_run_debug_arg("CRASHLOG=", &d) == AAR_RUN_ARG_BAD);
    assert(strcmp(d.crashlog, "Work:a b/c.txt") == 0); /* unchanged */
    memset(longpath, 'x', sizeof(longpath));
    memcpy(longpath, "PROFILEFILE=", 12);
    longpath[sizeof(longpath) - 1] = '\0';
    assert(aar_run_debug_arg(longpath, &d) == AAR_RUN_ARG_BAD);
    /* The longest path that fits. */
    memset(longpath, 'y', sizeof(longpath));
    memcpy(longpath, "CRASHLOG=", 9);
    longpath[9 + AAR_RUN_DEBUG_PATH_MAX - 1u] = '\0';
    assert(aar_run_debug_arg(longpath, &d) == AAR_RUN_ARG_TAKEN);
    assert(strlen(d.crashlog) == AAR_RUN_DEBUG_PATH_MAX - 1u);
}

static void test_other_words_are_left_alone(void)
{
    struct aar_run_debug d;

    memset(&d, 0, sizeof(d));
    assert(aar_run_debug_arg("WEIGHT=3", &d) == AAR_RUN_ARG_OTHER);
    assert(aar_run_debug_arg("PROFILER=3", &d) == AAR_RUN_ARG_OTHER);
    assert(aar_run_debug_arg("CRASHLOGS=x", &d) == AAR_RUN_ARG_OTHER);
    assert(aar_run_debug_arg("-v", &d) == AAR_RUN_ARG_OTHER);
    assert(aar_run_debug_arg("", &d) == AAR_RUN_ARG_OTHER);
    assert(d.profile_hz == 0u && d.crashlog[0] == '\0' && d.profilefile[0] == '\0');
    /* A tool type goes through the same rules. */
    assert(aar_run_debug_set(&d, "PROFILE", "50") == AAR_RUN_ARG_TAKEN && d.profile_hz == 50u);
    assert(aar_run_debug_set(&d, "PROFILE", "0") == AAR_RUN_ARG_BAD);
    assert(aar_run_debug_set(&d, "ARGS", "x") == AAR_RUN_ARG_OTHER);
}

static void test_rate_and_file_go_together(void)
{
    struct aar_run_debug d;
    char line[96];

    memset(&d, 0, sizeof(d));
    assert(aar_run_debug_check(&d) == AAR_RUN_DEBUG_OK);
    d.crashlog[0] = 'x'; /* a crash log alone is fine */
    assert(aar_run_debug_check(&d) == AAR_RUN_DEBUG_OK);
    d.profile_hz = 100u;
    assert(aar_run_debug_check(&d) == AAR_RUN_DEBUG_NO_FILE);
    aar_run_debug_line(line, sizeof(line), AAR_RUN_DEBUG_NO_FILE);
    assert(strstr(line, "PROFILE needs PROFILEFILE") != NULL);
    strcpy(d.profilefile, "RAM:p");
    assert(aar_run_debug_check(&d) == AAR_RUN_DEBUG_OK);
    d.profile_hz = 0u;
    assert(aar_run_debug_check(&d) == AAR_RUN_DEBUG_NO_RATE);
    aar_run_debug_line(line, sizeof(line), AAR_RUN_DEBUG_NO_RATE);
    assert(strstr(line, "PROFILEFILE needs PROFILE") != NULL);
    aar_run_debug_line(line, sizeof(line), AAR_RUN_DEBUG_OK);
    assert(line[0] == '\0');
}

static void test_the_tag_is_appended(void)
{
    struct TagItem tags[AAR_RUN_TAG_MAX + 1u];
    unsigned n;

    n = aar_run_build_tags(tags, 0u, 0u, 0u, 0);
    assert(aar_run_debug_tags(tags, n, 0u) == n);
    assert(tags[n].ti_Tag == TAG_DONE);
    assert(aar_run_debug_tags(tags, n, 200u) == n + 1u);
    assert(tags[n].ti_Tag == AAR_TAG_PROFILE_HZ && tags[n].ti_Data == 200u);
    assert(tags[n + 1u].ti_Tag == TAG_DONE);
    /* The most tags aar-run builds, and this one, fit one entry beyond the
     * array of the policy. */
    n = aar_run_build_tags(tags, 1u, 2u, 3u, 1);
    assert(aar_run_debug_tags(tags, n, 1u) == n + 1u);
    assert(n + 1u < AAR_RUN_TAG_MAX + 1u);
}

static const char *fake_tool_type(void *icon, const char *key)
{
    const char *const *p = (const char *const *)icon;

    for (; p[0]; p += 2)
        if (strcmp(p[0], key) == 0)
            return p[1];
    return NULL;
}

static void test_debug_tooltypes_follow_the_icon_order(void)
{
    static const char *const project[] = {"CRASHLOG", "RAM:p.txt", "PROFILE", "50", NULL};
    static const char *const tool[] = {"CRASHLOG", "RAM:t.txt", "PROFILE", "200",
                                       "PROFILEFILE", "RAM:t.bin", NULL};
    struct aar_run_tt_chain chain;
    struct aar_run_debug d;
    const char *bad = NULL;

    chain.lookup = fake_tool_type;
    chain.project = (void *)project;
    chain.tool = (void *)tool;
    memset(&d, 0, sizeof(d));
    /* The project icon first, aar-run's icon for what the project does not set. */
    assert(aar_run_debug_tooltypes(&chain, &d, &bad) && bad == NULL);
    assert(strcmp(d.crashlog, "RAM:p.txt") == 0 && d.profile_hz == 50u);
    assert(strcmp(d.profilefile, "RAM:t.bin") == 0);
    /* No icons: nothing. */
    chain.project = NULL;
    chain.tool = NULL;
    memset(&d, 0, sizeof(d));
    assert(aar_run_debug_tooltypes(&chain, &d, &bad));
    assert(d.profile_hz == 0u && !d.crashlog[0] && !d.profilefile[0]);
}

static void test_a_bad_debug_tooltype_names_its_key(void)
{
    static const char *const rate[] = {"PROFILE", "fast", NULL};
    static const char *const big[] = {"PROFILE", "10001", NULL};
    static const char *const empty_log[] = {"CRASHLOG", "", NULL};
    static const char *const empty_file[] = {"PROFILEFILE", "", NULL};
    static const char *const *const bad[] = {rate, big, empty_log, empty_file};
    static const char *const names[] = {"PROFILE", "PROFILE", "CRASHLOG", "PROFILEFILE"};
    struct aar_run_tt_chain chain;
    unsigned i;

    chain.lookup = fake_tool_type;
    chain.project = NULL;
    for (i = 0; i < 4; i++) {
        struct aar_run_debug d;
        const char *key = NULL;

        memset(&d, 0, sizeof(d));
        chain.tool = (void *)bad[i];
        assert(!aar_run_debug_tooltypes(&chain, &d, &key));
        assert(key && strcmp(key, names[i]) == 0);
        /* A bad value in the project icon is not hidden by a good one in aar-run's. */
        chain.project = (void *)bad[i];
        chain.tool = NULL;
        assert(!aar_run_debug_tooltypes(&chain, &d, &key));
        chain.project = NULL;
    }
}

static void test_which_ends_leave_a_report(void)
{
    assert(aar_run_crash_wanted(AAR_END_USER_FAULT, AAR_FAULT_DATA, 1));
    assert(aar_run_crash_wanted(AAR_END_USER_FAULT, AAR_FAULT_NONE, 0));
    assert(aar_run_crash_wanted(AAR_END_STALL, AAR_FAULT_NONE, 0));
    assert(aar_run_crash_wanted(AAR_END_EXIT, AAR_FAULT_ILLEGAL, 0));
    assert(aar_run_crash_wanted(AAR_END_EXIT, AAR_FAULT_NONE, 1));
    assert(!aar_run_crash_wanted(AAR_END_EXIT, AAR_FAULT_NONE, 0));
    assert(!aar_run_crash_wanted(AAR_END_FORCED, AAR_FAULT_NONE, 0));
    assert(!aar_run_crash_wanted(AAR_END_LEASE, AAR_FAULT_NONE, 0));
    assert(!aar_run_crash_wanted(AAR_END_STUCK_CALL, AAR_FAULT_NONE, 0));
}

static void test_register_line(void)
{
    char line[80];

    aar_run_reg_line(line, sizeof(line), 0x11210u, 0xbffff000u, 0x11150u);
    assert(strcmp(line, "  pc=00011210 sp=bffff000 lr=00011150") == 0);
}

static AARStatusInfo sample_status(void)
{
    AARStatusInfo st;

    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    st.end_reason = AAR_END_USER_FAULT;
    st.fault_class = AAR_FAULT_DATA;
    st.fault_syndrome = 0x805u;
    st.fault_pstate = 0x60000010u;
    st.fault_pc.lo = 0x11210u;
    st.fault_lr.lo = 0x11150u;
    st.fault_sp.lo = 0xbffff000u;
    st.fault_addr.lo = 4u;
    st.load_base.lo = 0x10000u;
    st.profile = AAR_PROFILE_A32;
    st.exit_code = -1;
    st.stack_sp.lo = 0xbffff000u;
    st.stack_bytes = 24u;
    return st;
}

static void put_le_words(uint8_t *out, const uint32_t *words, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        out[i * 4u] = (uint8_t)words[i];
        out[i * 4u + 1u] = (uint8_t)(words[i] >> 8);
        out[i * 4u + 2u] = (uint8_t)(words[i] >> 16);
        out[i * 4u + 3u] = (uint8_t)(words[i] >> 24);
    }
}

static void test_crash_report_is_the_sample_file(void)
{
    static const uint32_t words[] = { 0u, 0x11134u, 0xbffff040u, 0x110c4u, 0x13100u, 0x1122cu };
    AARStatusInfo st = sample_status();
    uint8_t stack[24];
    char report[AAR_RUN_CRASH_BUF];
    char expect[AAR_RUN_CRASH_BUF];
    size_t n, got;
    FILE *f = fopen(SAMPLE_PATH, "rb");

    assert(f != NULL);
    got = fread(expect, 1, sizeof(expect) - 1u, f);
    fclose(f);
    expect[got] = '\0';
    put_le_words(stack, words, 6u);
    n = aar_run_crash_report(report, sizeof(report), "Work:Apps/game.elf", &st, stack, 24u);
    assert(n == strlen(report));
    assert(strcmp(report, expect) == 0);
}

static void test_crash_report_other_shapes(void)
{
    AARStatusInfo st = sample_status();
    uint8_t stack[7] = { 0x01, 0x02, 0x03, 0x04, 0xaa, 0xbb, 0xcc };
    char report[AAR_RUN_CRASH_BUF];
    char tiny[64];
    size_t n;

    /* A stall: the class is none, and there may be no stack. */
    st.end_reason = AAR_END_STALL;
    st.fault_class = AAR_FAULT_NONE;
    st.fault_syndrome = 0;
    st.fault_pstate = 0;
    st.stack_bytes = 0;
    n = aar_run_crash_report(report, sizeof(report), "x.elf", &st, NULL, 0u);
    assert(n == strlen(report));
    assert(strstr(report, "end_reason: 4 stall watchdog\n") != NULL);
    assert(strstr(report, "fault_class: 0 none\n") != NULL);
    assert(strstr(report, "stack_bytes: 0\nstack:\n") != NULL);
    assert(strlen(strstr(report, "stack:\n")) == 7u); /* nothing after the heading */
    /* A last word of 3 bytes has 6 digits. 64-bit values keep their high word. */
    st.fault_pc.hi = 0x1u;
    st.fault_pc.lo = 0x80000000u;
    st.profile = AAR_PROFILE_A64;
    st.stack_sp.hi = 0x2u;
    st.stack_sp.lo = 0xfffffff8u;
    n = aar_run_crash_report(report, sizeof(report), "x.elf", &st, stack, 7u);
    assert(n == strlen(report));
    assert(strstr(report, "pc: 0x0000000180000000\n") != NULL);
    assert(strstr(report, "profile: 2 A64\n") != NULL);
    assert(strstr(report, "stack_sp: 0x00000002fffffff8\n") != NULL);
    assert(strstr(report, "0x00000002fffffff8: 04030201 ccbbaa\n") != NULL);
    /* Over 16 bytes the address of the next line carries into the high word. */
    {
        uint8_t big[20];

        memset(big, 0, sizeof(big));
        n = aar_run_crash_report(report, sizeof(report), "x.elf", &st, big, 20u);
        assert(strstr(report, "0x0000000300000008: 00000000\n") != NULL);
    }
    /* A buffer that is too small cuts the report and stays a string. */
    n = aar_run_crash_report(tiny, sizeof(tiny), "x.elf", &st, stack, 7u);
    assert(n == strlen(tiny) && n < sizeof(tiny));
    assert(strncmp(tiny, "aar-crash 1\nend_reason: 4", 25) == 0);
    n = aar_run_crash_report(tiny, 0u, "x.elf", &st, stack, 7u);
    assert(n == 0u);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void test_profile_header(void)
{
    uint8_t buf[AAR_RUN_PROFILE_HEADER_MAX];
    AARU64 base = { 0x1u, 0x00010000u };
    uint8_t word[4];
    size_t n;

    n = aar_run_profile_header(buf, sizeof(buf), 100u, base, "Work:Apps/game.elf");
    /* 32 fixed bytes, then 18 path bytes padded to 24. */
    assert(n == 56u);
    assert(memcmp(buf, "AARP", 4) == 0);
    assert(be32(buf + 4) == 1u && be32(buf + 8) == 100u && be32(buf + 12) == 0u);
    assert(be32(buf + 16) == 1u && be32(buf + 20) == 0x10000u && be32(buf + 24) == 0u);
    assert(be32(buf + 28) == 18u);
    assert(memcmp(buf + 32, "Work:Apps/game.elf", 18) == 0);
    assert(buf[50] == 0 && buf[55] == 0);
    assert(n % 8u == 0u);
    /* A path whose length is a multiple of 8 gets no padding. */
    assert(aar_run_profile_header(buf, sizeof(buf), 1u, base, "12345678") == 40u);
    assert(aar_run_profile_header(buf, sizeof(buf), 1u, base, "") == 32u);
    /* No room: nothing is written. */
    assert(aar_run_profile_header(buf, 55u, 100u, base, "Work:Apps/game.elf") == 0u);
    assert(aar_run_profile_header(buf, 56u, 100u, base, "Work:Apps/game.elf") == 56u);
    /* The lost count goes into the word at AAR_RUN_PROFILE_LOST_OFFSET. */
    assert(AAR_RUN_PROFILE_LOST_OFFSET == 12u);
    aar_run_profile_lost_word(word, 0x01020304u);
    assert(word[0] == 1 && word[1] == 2 && word[2] == 3 && word[3] == 4);
}

static void test_profile_drain_pace(void)
{
    uint32_t passes = 0u, due = 0u, i;

    for (i = 0; i < AAR_RUN_PROFILE_EVERY * 4u; i++)
        due += (uint32_t)aar_run_profile_due(&passes);
    assert(due == 4u);
    passes = 0u;
    for (i = 0; i + 1u < AAR_RUN_PROFILE_EVERY; i++)
        assert(!aar_run_profile_due(&passes));
    assert(aar_run_profile_due(&passes));
}

static void test_a_drain_pass_is_bounded(void)
{
    uint32_t chunks = 0, taken = 0;

    /* A platform that always has a full chunk waiting: one pass takes its budget and returns. */
    do
        taken += AAR_RUN_PROFILE_CHUNK;
    while (aar_run_profile_drain_more(++chunks, AAR_RUN_PROFILE_PASS_CHUNKS, AAR_RUN_PROFILE_CHUNK));
    assert(chunks == AAR_RUN_PROFILE_PASS_CHUNKS);
    assert(taken == AAR_RUN_PROFILE_PASS_CHUNKS * AAR_RUN_PROFILE_CHUNK);
    /* A chunk that is not full means the ring is empty, whatever is left of the budget. */
    assert(!aar_run_profile_drain_more(1u, AAR_RUN_PROFILE_PASS_CHUNKS, AAR_RUN_PROFILE_CHUNK - 1u));
    assert(!aar_run_profile_drain_more(1u, AAR_RUN_PROFILE_PASS_CHUNKS, 0u));
    assert(aar_run_profile_drain_more(1u, AAR_RUN_PROFILE_PASS_CHUNKS, AAR_RUN_PROFILE_CHUNK));
    /* The pass is small and the last drain large. */
    assert(AAR_RUN_PROFILE_PASS_CHUNKS * AAR_RUN_PROFILE_CHUNK <= 2048u);
    assert(AAR_RUN_PROFILE_END_CHUNKS > AAR_RUN_PROFILE_PASS_CHUNKS);
}

static void test_profile_summary(void)
{
    char line[200];

    aar_run_profile_summary_line(line, sizeof(line), 100u, 1234ul, 5ul, "RAM:p.bin");
    assert(strcmp(line, "aar-run: profile: 1234 samples at 100 Hz, 5 lost, in RAM:p.bin") == 0);
}

int main(void)
{
    test_profile_option();
    test_path_options();
    test_other_words_are_left_alone();
    test_rate_and_file_go_together();
    test_the_tag_is_appended();
    test_debug_tooltypes_follow_the_icon_order();
    test_a_bad_debug_tooltype_names_its_key();
    test_which_ends_leave_a_report();
    test_register_line();
    test_crash_report_is_the_sample_file();
    test_crash_report_other_shapes();
    test_profile_header();
    test_profile_drain_pace();
    test_a_drain_pass_is_bounded();
    test_profile_summary();
    puts("run_debug_test: ok");
    return 0;
}
