/*
 * Host tests of the decisions the launcher aar-run makes: arguments and tool types,
 * the limits it takes from AARQuery, the probe of the image before a claim, the
 * library options, the texts for a refused claim, load or start, the check of
 * what AARQuery said, the exit code, and the ZZ9000 list helpers.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar_run_policy.h"

static void test_a_break_that_was_seen_stays_seen(void)
{
    int latched = 0;

    /* the load saw Ctrl-C; later reads of the signal find it consumed */
    latched = aar_run_break_latch(latched, 1, 0);
    latched = aar_run_break_latch(latched, 0, 0);
    assert(latched == 1);
    assert(aar_run_load_failure_exit(latched, 0) == 5);
    /* the loop's own record of a user stop counts too */
    assert(aar_run_break_latch(0, 0, 1) == 1);
    /* no break: a failed load is a launcher error */
    assert(aar_run_load_failure_exit(aar_run_break_latch(0, 0, 0), 0) == 20);
}

static void test_the_stack_is_the_one_the_command_runs_on(void)
{
    /* Stack 8192 before the command: refused although the task bounds are large */
    assert(!aar_run_stack_ok(65536u, 1, 2048u));
    assert(aar_run_stack_ok(65536u, 1, 8192u));       /* Stack 32768 */
    assert(aar_run_stack_ok(16384u, 1, 4096u));       /* exactly the minimum */
    assert(!aar_run_stack_ok(16380u, 1, 8192u));
    assert(!aar_run_stack_ok(65536u, 1, 4095u));
    /* the CLI default is a floor, not a ceiling: small task bounds still refuse */
    assert(!aar_run_stack_ok(8192u, 1, 8192u));
    /* Workbench: no CLI, the icon's stack is the task bounds */
    assert(aar_run_stack_ok(32768u, 0, 0u));
    assert(!aar_run_stack_ok(8192u, 0, 100000u));
    /* a huge longword count does not wrap */
    assert(aar_run_stack_ok(65536u, 1, 0x40000000u));
}

static void test_ahi_open_failure_exits_20(void)
{
    assert(aar_run_process_exit(0, 0, 0, 1, 0) == 20);
    assert(aar_run_process_exit(0, 0, 0, 0, 0) == 0);
    assert(aar_run_process_exit(7, 0, 0, 1, 0) == 20);
    assert(aar_run_process_exit(0, 1, 0, 0, 0) == 20);
    assert(aar_run_process_exit(0, 0, 1, 0, 0) == 20);
}

static void test_corrupt_queue_exits_20(void)
{
    /* The app was stopped for the corrupt queue: not a user stop, whatever
     * exit code or end reason it then reports. */
    assert(aar_run_process_exit(0, 0, 0, 0, 1) == 20);
    assert(aar_run_process_exit(7, 0, 0, 0, 1) == 20);
    assert(aar_run_process_exit(aar_run_app_exit_code(AAR_END_FORCED, 0, 1), 0, 0, 0,
                                1) == 20);
}

/* ---- Limits: what the platform says, not what aar-run assumes ----------------- */

static void test_positive_mib_parse(void)
{
    uint32_t mib = 0;

    /* MAXMEM, HOSTMEM and WEIGHT share one syntax: a positive decimal. The
     * bound each of them has is the platform's, checked after AARQuery. */
    assert(aar_run_parse_positive("64", &mib) && mib == 64u);
    assert(aar_run_parse_positive("4096", &mib) && mib == 4096u);
    assert(aar_run_parse_positive("4294967295", &mib) && mib == UINT32_MAX);
    mib = 7u;
    assert(!aar_run_parse_positive("0", &mib) && mib == 7u);
    assert(!aar_run_parse_positive("", &mib));
    assert(!aar_run_parse_positive("64MiB", &mib));
    assert(!aar_run_parse_positive("4294967296", &mib));
    assert(!aar_run_parse_positive("-1", &mib));
    assert(!aar_run_parse_positive(" 8", &mib));
    assert(!aar_run_parse_positive("+8", &mib));
    assert(!aar_run_parse_positive(NULL, &mib));
    assert(!aar_run_parse_positive("8", NULL));
    assert(mib == 7u);
}

/* A platform that is not the ZZ9000: no Zorro III word, a smaller argument
 * budget than the ZZ9000's, one profile. aar-run has to run on it. */
static AARInfo platform_info(void)
{
    AARInfo info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.api_major = AAR_API_MAJOR;
    info.api_minor = AAR_API_MINOR;
    info.platform_id = AAR_PLATFORM_ZZ9000 + 1u;
    info.profiles = AAR_PROFILEF_A32;
    info.groups = AAR_GROUPS_DEFINED;
    info.max_args = 16u;
    info.max_arg_bytes = 128u;
    info.max_image_bytes = 16u * AAR_RUN_MIB_BYTES;
    info.stop_grace_ms = 500u;
    info.shared_default_mib = 16u;
    info.shared_max_mib = 4095u;
    info.weight_max = 256u;
    return info;
}

static AARInfo zz9000_info(void)
{
    AARInfo info = platform_info();

    info.platform_id = AAR_PLATFORM_ZZ9000;
    info.profiles = AAR_PROFILEF_A32 | AAR_PROFILEF_A64;
    info.platform[AAR_ZZ9000_INFO_CAPS] = AAR_ZZ9000_CAP_Z3;
    return info;
}

static void test_limits_are_read_from_aarinfo(void)
{
    AARInfo info = platform_info();
    struct aar_run_limits lim = aar_run_limits_from_info(&info);

    assert(lim.max_args == 16u && lim.max_arg_bytes == 128u);
    assert(lim.weight_max == 256u && lim.hostmem_max_mib == 4095u);
    assert(lim.max_image_bytes == 16u * AAR_RUN_MIB_BYTES);

    info.max_args = 6u;
    info.max_arg_bytes = 40u;
    info.weight_max = 8u;
    info.shared_max_mib = 64u;
    info.max_image_bytes = 3u * AAR_RUN_MIB_BYTES;
    lim = aar_run_limits_from_info(&info);
    assert(lim.max_args == 6u && lim.max_arg_bytes == 40u);
    assert(lim.weight_max == 8u && lim.hostmem_max_mib == 64u);
    assert(lim.max_image_bytes == 3u * AAR_RUN_MIB_BYTES);

    /* The library's own rule for AAR_TAG_SHARED_MIB: the platform's largest,
     * or 4095 when it reports none or more. */
    info.shared_max_mib = 0u;
    assert(aar_run_limits_from_info(&info).hostmem_max_mib == 4095u);
    info.shared_max_mib = 9000u;
    assert(aar_run_limits_from_info(&info).hostmem_max_mib == 4095u);

    /* 0 is "no limit" for the argument budget and "no weighting" for WEIGHT. */
    info.max_args = 0u;
    info.max_arg_bytes = 0u;
    info.weight_max = 0u;
    lim = aar_run_limits_from_info(&info);
    assert(lim.max_args == 0u && lim.max_arg_bytes == 0u && lim.weight_max == 0u);
}

static void test_argument_budget_comes_from_the_platform(void)
{
    AARInfo info = platform_info();
    struct aar_run_limits lim = aar_run_limits_from_info(&info);

    /* argv[0] is one of the max_args strings, so the app gets one fewer. */
    assert(aar_run_limits_check(&lim, 15u, 127u, 0u, 0u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 16u, 1u, 0u, 0u) == AAR_RUN_LIMIT_ARGS);
    /* max_arg_bytes counts the NUL. */
    assert(aar_run_limits_check(&lim, 1u, 127u, 0u, 0u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 1u, 128u, 0u, 0u) == AAR_RUN_LIMIT_ARG_BYTES);
    assert(aar_run_limits_check(&lim, 0u, 0u, 0u, 0u) == AAR_RUN_LIMIT_OK);

    /* A platform with a smaller budget: aar-run does not pass 16 strings of
     * 127 bytes to it. */
    info.max_args = 4u;
    info.max_arg_bytes = 32u;
    lim = aar_run_limits_from_info(&info);
    assert(aar_run_limits_check(&lim, 3u, 31u, 0u, 0u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 4u, 1u, 0u, 0u) == AAR_RUN_LIMIT_ARGS);
    assert(aar_run_limits_check(&lim, 1u, 32u, 0u, 0u) == AAR_RUN_LIMIT_ARG_BYTES);

    /* A platform with a larger one takes more than the ZZ9000 does. */
    info.max_args = 64u;
    info.max_arg_bytes = 1024u;
    lim = aar_run_limits_from_info(&info);
    assert(aar_run_limits_check(&lim, 63u, 1023u, 0u, 0u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 64u, 1u, 0u, 0u) == AAR_RUN_LIMIT_ARGS);

    /* 0: the platform has no limit. */
    info.max_args = 0u;
    info.max_arg_bytes = 0u;
    lim = aar_run_limits_from_info(&info);
    assert(aar_run_limits_check(&lim, 5000u, 100000u, 0u, 0u) == AAR_RUN_LIMIT_OK);
}

static void test_weight_bound_comes_from_the_platform(void)
{
    AARInfo info = platform_info();
    struct aar_run_limits lim = aar_run_limits_from_info(&info);

    assert(aar_run_limits_check(&lim, 0u, 0u, 0u, 0u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 0u, 0u, 1u, 0u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 0u, 0u, 256u, 0u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 0u, 0u, 257u, 0u) == AAR_RUN_LIMIT_WEIGHT);

    info.weight_max = 8u;
    lim = aar_run_limits_from_info(&info);
    assert(aar_run_limits_check(&lim, 0u, 0u, 8u, 0u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 0u, 0u, 9u, 0u) == AAR_RUN_LIMIT_WEIGHT);

    /* No weighting: the library would answer AAR_E_NOSYS to the tag. A launch
     * that sets no weight is fine. */
    info.weight_max = 0u;
    lim = aar_run_limits_from_info(&info);
    assert(aar_run_limits_check(&lim, 0u, 0u, 1u, 0u) == AAR_RUN_LIMIT_NO_WEIGHT);
    assert(aar_run_limits_check(&lim, 0u, 0u, 0u, 0u) == AAR_RUN_LIMIT_OK);
}

static void test_hostmem_bound_comes_from_the_platform(void)
{
    AARInfo info = platform_info();
    struct aar_run_limits lim = aar_run_limits_from_info(&info);

    assert(aar_run_limits_check(&lim, 0u, 0u, 0u, 4095u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 0u, 0u, 0u, 4096u) == AAR_RUN_LIMIT_HOSTMEM);

    info.shared_max_mib = 64u;
    lim = aar_run_limits_from_info(&info);
    assert(aar_run_limits_check(&lim, 0u, 0u, 0u, 64u) == AAR_RUN_LIMIT_OK);
    assert(aar_run_limits_check(&lim, 0u, 0u, 0u, 65u) == AAR_RUN_LIMIT_HOSTMEM);
    /* Unset leaves the platform default alone. */
    assert(aar_run_limits_check(&lim, 0u, 0u, 0u, 0u) == AAR_RUN_LIMIT_OK);
}

static void test_limit_lines_name_the_platform_value(void)
{
    AARInfo info = platform_info();
    struct aar_run_limits lim;
    char line[200];

    info.max_args = 6u;
    info.max_arg_bytes = 40u;
    info.weight_max = 8u;
    info.shared_max_mib = 64u;
    lim = aar_run_limits_from_info(&info);

    aar_run_limit_line(line, sizeof(line), AAR_RUN_LIMIT_ARGS, &lim);
    assert(strstr(line, "at most 5 arguments"));
    aar_run_limit_line(line, sizeof(line), AAR_RUN_LIMIT_ARG_BYTES, &lim);
    assert(strstr(line, "longer than 39 bytes"));
    aar_run_limit_line(line, sizeof(line), AAR_RUN_LIMIT_WEIGHT, &lim);
    assert(strstr(line, "WEIGHT") && strstr(line, "1 to 8"));
    aar_run_limit_line(line, sizeof(line), AAR_RUN_LIMIT_NO_WEIGHT, &lim);
    assert(strstr(line, "WEIGHT") && strstr(line, "does not weight"));
    aar_run_limit_line(line, sizeof(line), AAR_RUN_LIMIT_HOSTMEM, &lim);
    assert(strstr(line, "HOSTMEM") && strstr(line, "1 to 64"));
    /* A platform with a 1 string budget takes no app arguments at all. */
    info.max_args = 1u;
    lim = aar_run_limits_from_info(&info);
    aar_run_limit_line(line, sizeof(line), AAR_RUN_LIMIT_ARGS, &lim);
    assert(strstr(line, "takes no arguments"));
}


/* A fake of the library's AARSetTagList: it follows the rules of
 * docs/aar-client-api.md (the whole list is checked before anything is
 * applied; an unknown id is AAR_E_INVAL; a range error is AAR_E_INVAL; the
 * options are refused with AAR_E_BUSY once the load has begun). The real
 * library is not part of the host tests. */
struct fake_library {
    AARInfo info;
    int load_begun;
    ULONG private_mib;
    ULONG shared_mib;
    ULONG weight;
    ULONG client_caps;
    ULONG audio_route;
    int lease_extended;
};

static void fake_library_init(struct fake_library *lib)
{
    memset(lib, 0, sizeof(*lib));
    lib->info = platform_info();
    lib->lease_extended = 1;
    lib->audio_route = AAR_AUDIO_ROUTE_PLATFORM;
}

static LONG fake_set_tag_list(struct fake_library *lib,
                              const struct TagItem *tags)
{
    const struct TagItem *t;
    struct fake_library next = *lib;

    if (lib->load_begun)
        return AAR_E_BUSY;
    for (t = tags; t->ti_Tag != TAG_DONE; t++) {
        if (t->ti_Tag == TAG_IGNORE)
            continue;
        switch (t->ti_Tag) {
        case AAR_TAG_PRIVATE_MIB:
            if (t->ti_Data > 4095u)
                return AAR_E_INVAL;
            next.private_mib = t->ti_Data;
            break;
        case AAR_TAG_SHARED_MIB: {
            /* The library's rule: the platform's largest, or 4095. */
            ULONG limit = lib->info.shared_max_mib != 0u &&
                                  lib->info.shared_max_mib < 4095u ?
                              lib->info.shared_max_mib : 4095u;

            if (t->ti_Data > limit)
                return AAR_E_INVAL;
            next.shared_mib = t->ti_Data;
            break;
        }
        case AAR_TAG_WEIGHT:
            if (lib->info.weight_max == 0u)
                return AAR_E_NOSYS;
            if (t->ti_Data > lib->info.weight_max)
                return AAR_E_INVAL;
            next.weight = t->ti_Data;
            break;
        case AAR_TAG_LEASE_EXTENDED:
            next.lease_extended = t->ti_Data != 0u;
            break;
        case AAR_TAG_AUDIO_ROUTE:
            if (t->ti_Data != AAR_AUDIO_ROUTE_PLATFORM &&
                t->ti_Data != AAR_AUDIO_ROUTE_AHI)
                return AAR_E_INVAL;
            next.audio_route = t->ti_Data;
            break;
        case AAR_TAG_CLIENT_CAPS:
            if (t->ti_Data & ~AAR_CCAP_KNOWN)
                return AAR_E_INVAL;
            next.client_caps = t->ti_Data;
            break;
        default:
            return AAR_E_INVAL;
        }
    }
    *lib = next;
    return AAR_OK;
}

static void test_weight_argument_forms(void)
{
    uint32_t w = 0u;

    assert(aar_run_weight_arg("WEIGHT=1", &w) == AAR_RUN_ARG_TAKEN && w == 1u);
    assert(aar_run_weight_arg("WEIGHT=256", &w) == AAR_RUN_ARG_TAKEN && w == 256u);
    assert(aar_run_weight_arg("weight=3", &w) == AAR_RUN_ARG_TAKEN && w == 3u);
    assert(aar_run_weight_arg("Weight=17", &w) == AAR_RUN_ARG_TAKEN && w == 17u);
    /* The bound is the platform's: the word is taken here and refused after
     * AARQuery by aar_run_limits_check. */
    assert(aar_run_weight_arg("WEIGHT=257", &w) == AAR_RUN_ARG_TAKEN && w == 257u);
    w = 5u;
    assert(aar_run_weight_arg("WEIGHT=0", &w) == AAR_RUN_ARG_BAD);
    assert(aar_run_weight_arg("WEIGHT=-1", &w) == AAR_RUN_ARG_BAD);
    assert(aar_run_weight_arg("WEIGHT=", &w) == AAR_RUN_ARG_BAD);
    assert(aar_run_weight_arg("WEIGHT=high", &w) == AAR_RUN_ARG_BAD);
    assert(aar_run_weight_arg("WEIGHT=8junk", &w) == AAR_RUN_ARG_BAD);
    assert(aar_run_weight_arg("WEIGHT=8=9", &w) == AAR_RUN_ARG_BAD);
    assert(w == 5u);
    /* Other words belong to the app or to the other options. */
    assert(aar_run_weight_arg("WEIGHT", &w) == AAR_RUN_ARG_OTHER);
    assert(aar_run_weight_arg("WEIGHT:8", &w) == AAR_RUN_ARG_OTHER);
    assert(aar_run_weight_arg("WEIGH=8", &w) == AAR_RUN_ARG_OTHER);
    assert(aar_run_weight_arg("-WEIGHT=8", &w) == AAR_RUN_ARG_OTHER);
    assert(aar_run_weight_arg("MAXMEM=8", &w) == AAR_RUN_ARG_OTHER);
    assert(aar_run_weight_arg("+timedemo", &w) == AAR_RUN_ARG_OTHER);
    assert(aar_run_weight_arg("", &w) == AAR_RUN_ARG_OTHER);
    assert(aar_run_weight_arg(NULL, &w) == AAR_RUN_ARG_OTHER);
    assert(w == 5u);
}

/* The scan aar-run runs over the words after the ELF: WEIGHT is taken, the
 * rest is kept for the app in order. Returns -1 for a bad WEIGHT. */
static int scan_weight(int argc, char **argv, uint32_t *weight, char **kept,
                       int *nkept)
{
    int i;

    *nkept = 0;
    for (i = 0; i < argc; i++) {
        int r = aar_run_weight_arg(argv[i], weight);

        if (r == AAR_RUN_ARG_BAD)
            return -1;
        if (r == AAR_RUN_ARG_TAKEN)
            continue;
        kept[(*nkept)++] = argv[i];
    }
    return 0;
}

static void test_weight_is_stripped_from_the_app_arguments(void)
{
    char *argv[] = {"+timedemo", "weight=12", "demo1", "MAXMEM=8"};
    char *kept[4];
    int nkept;
    uint32_t w = 0u;

    assert(scan_weight(4, argv, &w, kept, &nkept) == 0);
    assert(w == 12u);
    assert(nkept == 3);
    assert(strcmp(kept[0], "+timedemo") == 0);
    assert(strcmp(kept[1], "demo1") == 0);
    assert(strcmp(kept[2], "MAXMEM=8") == 0);

    /* Without WEIGHT nothing is stripped and the value stays 0. */
    {
        char *plain[] = {"+timedemo", "demo1"};

        w = 0u;
        assert(scan_weight(2, plain, &w, kept, &nkept) == 0);
        assert(w == 0u && nkept == 2);
    }

    /* The last WEIGHT wins, like the other options. */
    {
        char *twice[] = {"WEIGHT=2", "WEIGHT=9"};

        assert(scan_weight(2, twice, &w, kept, &nkept) == 0);
        assert(w == 9u && nkept == 0);
    }

    /* A bad value ends the scan before anything is claimed. */
    {
        char *bad[] = {"+x", "WEIGHT=0", "y"};

        assert(scan_weight(3, bad, &w, kept, &nkept) == -1);
    }
}

static unsigned tag_count(const struct TagItem *tags)
{
    unsigned n = 0;

    while (tags[n].ti_Tag != TAG_DONE)
        n++;
    return n;
}

static ULONG tag_value(const struct TagItem *tags, ULONG id, int *found)
{
    unsigned i;

    *found = 0;
    for (i = 0; tags[i].ti_Tag != TAG_DONE; i++) {
        if (tags[i].ti_Tag == id) {
            *found = 1;
            return tags[i].ti_Data;
        }
    }
    return 0;
}

static void test_tag_list_carries_only_the_set_limits(void)
{
    struct TagItem tags[AAR_RUN_TAG_MAX];
    int found;
    unsigned n;

    /* A launch with no limits and no AUDIO: the serving capabilities and
     * the extended lease only. A zero limit keeps the platform default
     * and is left out. */
    n = aar_run_build_tags(tags, 0u, 0u, 0u, 0);
    assert(n == 2u && tag_count(tags) == 2u);
    assert(tag_value(tags, AAR_TAG_CLIENT_CAPS, &found) ==
               aar_run_client_ccaps() && found);
    assert(tag_value(tags, AAR_TAG_LEASE_EXTENDED, &found) == 1u && found);
    tag_value(tags, AAR_TAG_PRIVATE_MIB, &found);
    assert(!found);
    tag_value(tags, AAR_TAG_SHARED_MIB, &found);
    assert(!found);
    tag_value(tags, AAR_TAG_WEIGHT, &found);
    assert(!found);
    tag_value(tags, AAR_TAG_AUDIO_ROUTE, &found);
    assert(!found);

    /* MAXMEM, HOSTMEM and WEIGHT each have their own tag. */
    n = aar_run_build_tags(tags, 256u, 32u, 1u, 0);
    assert(n == 5u && tag_count(tags) == 5u);
    assert(tag_value(tags, AAR_TAG_PRIVATE_MIB, &found) == 256u && found);
    assert(tag_value(tags, AAR_TAG_SHARED_MIB, &found) == 32u && found);
    assert(tag_value(tags, AAR_TAG_WEIGHT, &found) == 1u && found);

    /* Only the ones that are set. */
    n = aar_run_build_tags(tags, 0u, 0u, 256u, 0);
    assert(n == 3u);
    assert(tag_value(tags, AAR_TAG_WEIGHT, &found) == 256u && found);
    tag_value(tags, AAR_TAG_PRIVATE_MIB, &found);
    assert(!found);

    /* AUDIO=AHI names the AHI route. */
    n = aar_run_build_tags(tags, 0u, 0u, 0u, 1);
    assert(n == 3u);
    assert(tag_value(tags, AAR_TAG_AUDIO_ROUTE, &found) ==
               AAR_AUDIO_ROUTE_AHI && found);

    /* Everything at once fits the array and ends with TAG_DONE. */
    n = aar_run_build_tags(tags, 1u, 2u, 3u, 1);
    assert(n == 6u && n < AAR_RUN_TAG_MAX);
    assert(tags[n].ti_Tag == TAG_DONE);
}

static void test_tag_list_follows_the_library_rules(void)
{
    struct TagItem tags[AAR_RUN_TAG_MAX];
    struct fake_library lib;
    unsigned i;

    /* No tag aar-run builds is a launcher-policy tag or an id outside the
     * ones the library defines: ROOT, ABS, the console and the screen
     * mode stay in aar-run. */
    (void)aar_run_build_tags(tags, 1u, 2u, 3u, 1);
    for (i = 0; tags[i].ti_Tag != TAG_DONE; i++) {
        assert(tags[i].ti_Tag > AAR_TAG_BASE);
        assert(tags[i].ti_Tag <= AAR_TAG_BASE + 6u);
    }

    fake_library_init(&lib);
    (void)aar_run_build_tags(tags, 256u, 32u, 4u, 1);
    assert(fake_set_tag_list(&lib, tags) == AAR_OK);
    assert(lib.private_mib == 256u && lib.shared_mib == 32u);
    assert(lib.weight == 4u);
    assert(lib.client_caps == aar_run_client_ccaps());
    assert(lib.lease_extended);
    assert(lib.audio_route == AAR_AUDIO_ROUTE_AHI);

    /* The platform route is the default and aar-run leaves it alone. */
    fake_library_init(&lib);
    (void)aar_run_build_tags(tags, 0u, 0u, 0u, 0);
    assert(fake_set_tag_list(&lib, tags) == AAR_OK);
    assert(lib.audio_route == AAR_AUDIO_ROUTE_PLATFORM);
    assert(lib.private_mib == 0u && lib.shared_mib == 0u && lib.weight == 0u);

    /* The largest values this platform grants are accepted. */
    fake_library_init(&lib);
    (void)aar_run_build_tags(tags, 4095u, lib.info.shared_max_mib,
                             lib.info.weight_max, 0);
    assert(fake_set_tag_list(&lib, tags) == AAR_OK);
    assert(lib.shared_mib == 4095u && lib.weight == 256u);
}

static void test_tag_list_is_refused_the_way_the_library_would(void)
{
    struct TagItem tags[AAR_RUN_TAG_MAX];
    struct fake_library lib;
    char line[256];
    LONG rc;

    /* A value past the platform bound: the library refuses the whole
     * list and changes nothing. */
    fake_library_init(&lib);
    (void)aar_run_build_tags(tags, 64u, 0u, lib.info.weight_max + 1u, 0);
    rc = fake_set_tag_list(&lib, tags);
    assert(rc == AAR_E_INVAL);
    assert(lib.private_mib == 0u && lib.weight == 0u && lib.client_caps == 0u);
    aar_run_tags_failure_line(line, sizeof(line), rc);
    assert(strstr(line, "out of range"));

    (void)aar_run_build_tags(tags, 0u, 4096u, 0u, 0);
    assert(fake_set_tag_list(&lib, tags) == AAR_E_INVAL);

    /* A platform with no weighting refuses WEIGHT. */
    fake_library_init(&lib);
    lib.info.weight_max = 0u;
    (void)aar_run_build_tags(tags, 0u, 0u, 2u, 0);
    rc = fake_set_tag_list(&lib, tags);
    assert(rc == AAR_E_NOSYS);
    aar_run_tags_failure_line(line, sizeof(line), rc);
    assert(strstr(line, "does not take WEIGHT"));
    /* The same platform takes a launch that sets no weight. */
    (void)aar_run_build_tags(tags, 0u, 0u, 0u, 0);
    assert(fake_set_tag_list(&lib, tags) == AAR_OK);

    /* After the load began the options come too late. */
    fake_library_init(&lib);
    lib.load_begun = 1;
    rc = fake_set_tag_list(&lib, tags);
    assert(rc == AAR_E_BUSY);
    aar_run_tags_failure_line(line, sizeof(line), rc);
    assert(strcmp(line, "aar-run: options failed (busy)") == 0);
    assert(lib.client_caps == 0u);
}

static void test_handle_parse(void)
{
    uint32_t handle = 7u;

    assert(aar_run_parse_handle("65539", &handle) && handle == 65539u);
    assert(aar_run_parse_handle("$00020003", &handle) && handle == 0x20003u);
    assert(aar_run_parse_handle("0x20003", &handle) && handle == 0x20003u);
    assert(aar_run_parse_handle("0XfFfFffff", &handle) && handle == 0xffffffffu);
    handle = 7u;
    assert(!aar_run_parse_handle("0", &handle) && handle == 7u);
    assert(!aar_run_parse_handle("", &handle));
    assert(!aar_run_parse_handle("$", &handle));
    assert(!aar_run_parse_handle("0x", &handle));
    assert(!aar_run_parse_handle("12a", &handle));
    assert(!aar_run_parse_handle("$12g", &handle));
    assert(!aar_run_parse_handle("4294967296", &handle));
    assert(!aar_run_parse_handle("$100000000", &handle));
    assert(!aar_run_parse_handle("-1", &handle));
    assert(!aar_run_parse_handle(NULL, &handle));
    assert(!aar_run_parse_handle("1", NULL));
}

static void test_refusal_text_names_each_reason(void)
{
    assert(strstr(aar_run_refusal_text(AAR_REASON_MEMORY), "record"));
    assert(strstr(aar_run_refusal_text(AAR_REASON_TABLES), "tables"));
    assert(strstr(aar_run_refusal_text(AAR_REASON_HOSTHEAP), "shared"));
    assert(strstr(aar_run_refusal_text(AAR_REASON_LIMIT), "HOSTMEM"));
    assert(strcmp(aar_run_refusal_text(99u), "refused by the platform") == 0);
    assert(strcmp(aar_run_refusal_text(AAR_REASON_NONE),
                  "refused by the platform") == 0);
}

static void test_client_ccaps(void)
{
    /* aar-run serves CLUT8 screens, acknowledges pan flips and accepts
     * platform-owned flips; the lease is the library's, not a cap. */
    assert(aar_run_client_ccaps() == (AAR_CCAP_DISPLAY_CLUT8 |
                                      AAR_CCAP_DISPLAY_PAN_ACK |
                                      AAR_CCAP_DISPLAY_LATEST));
    assert((aar_run_client_ccaps() & ~AAR_CCAP_KNOWN) == 0u);
}

static void test_app_exit_code(void)
{
    assert(aar_run_app_exit_code(AAR_END_EXIT, 0, 0) == 0);
    assert(aar_run_app_exit_code(AAR_END_EXIT, 7, 0) == 7);
    assert(aar_run_app_exit_code(AAR_END_EXIT, 255, 0) == 255);
    assert(aar_run_app_exit_code(AAR_END_EXIT, 256, 0) == 20);
    assert(aar_run_app_exit_code(AAR_END_EXIT, -1, 0) == 20);
    assert(aar_run_app_exit_code(AAR_END_EXIT, -1, 1) == 0);
    assert(aar_run_app_exit_code(AAR_END_USER_FAULT, 0, 0) == 20);
    assert(aar_run_app_exit_code(AAR_END_STALL, 0, 0) == 20);
    assert(aar_run_app_exit_code(AAR_END_STUCK_CALL, 0, 1) == 20);
    assert(aar_run_app_exit_code(AAR_END_LEASE, 0, 0) == 20);
    assert(aar_run_app_exit_code(AAR_END_PLATFORM_RESET, 0, 0) == 20);
    assert(aar_run_app_exit_code(AAR_END_RUNTIME_RESTART, 0, 0) == 20);
    assert(aar_run_app_exit_code(AAR_END_QUARANTINED, 0, 0) == 20);
    /* A forced end is a launcher error unless the user asked for it. */
    assert(aar_run_app_exit_code(AAR_END_FORCED, 0, 0) == 20);
    assert(aar_run_app_exit_code(AAR_END_FORCED, 0, 1) == 0);
    assert(aar_run_app_exit_code(AAR_END_FORCED, 3, 1) == 3);
}

static void test_release_is_for_finished_claims(void)
{
    /* an app that ended, faulted, or whose claim the card quarantined */
    assert(aar_run_release_allowed(AAR_STATE_IDLE, AAR_END_EXIT));
    assert(aar_run_release_allowed(AAR_STATE_FAULTED, AAR_END_USER_FAULT));
    assert(aar_run_release_allowed(AAR_STATE_STOPPING, AAR_END_QUARANTINED));
    assert(aar_run_release_allowed(AAR_STATE_RUNNING, AAR_END_LEASE));
    /* a claim somebody is using */
    assert(!aar_run_release_allowed(AAR_STATE_RUNNING, AAR_END_NONE));
    assert(!aar_run_release_allowed(AAR_STATE_CLAIMED, AAR_END_NONE));
    assert(!aar_run_release_allowed(AAR_STATE_LOADING, AAR_END_NONE));
    assert(!aar_run_release_allowed(AAR_STATE_STARTING, AAR_END_NONE));
    assert(!aar_run_release_allowed(AAR_STATE_STOPPING, AAR_END_FORCED));
}

static void test_block_reason_names_match_protocol(void)
{
    assert(strcmp(aar_run_block_reason_name(AAR_RUN_BLOCK_NONE), "none") == 0);
    assert(strcmp(aar_run_block_reason_name(AAR_RUN_BLOCK_FILE), "file") == 0);
    assert(strcmp(aar_run_block_reason_name(AAR_RUN_BLOCK_CONSOLE), "console") == 0);
    assert(strcmp(aar_run_block_reason_name(AAR_RUN_BLOCK_EVENT), "event") == 0);
    assert(strcmp(aar_run_block_reason_name(AAR_RUN_BLOCK_SLEEP), "sleep") == 0);
    assert(strcmp(aar_run_block_reason_name(AAR_RUN_BLOCK_VSYNC), "vsync") == 0);
    assert(strcmp(aar_run_block_reason_name(AAR_RUN_BLOCK_AUDIO), "audio") == 0);
    assert(strcmp(aar_run_block_reason_name(7u), "unknown") == 0);
    assert(strcmp(aar_run_block_reason_name(255u), "unknown") == 0);
}

static void test_end_reason_names(void)
{
    assert(strcmp(aar_run_end_reason_name(AAR_END_FORCED), "forced stop") == 0);
    assert(strcmp(aar_run_end_reason_name(AAR_END_QUARANTINED),
                  "quarantined") == 0);
    assert(strcmp(aar_run_end_reason_name(AAR_END_LEASE), "lease loss") == 0);
}

/* ---- Refusal and failure lines -------------------------------------------
 * What aar-run prints for each answer the library can give. */

static void test_open_failure_lines(void)
{
    char line[256];

    aar_run_claim_failure_line(line, sizeof(line), AAR_E_NORESOURCE,
                               AAR_REASON_TABLES);
    assert(strcmp(line, "aar-run: claim refused: no address-space translation "
                        "tables left") == 0);
    aar_run_claim_failure_line(line, sizeof(line), AAR_E_NORESOURCE,
                               AAR_REASON_MEMORY);
    assert(strstr(line, "claim refused:") && strstr(line, "record"));
    aar_run_claim_failure_line(line, sizeof(line), AAR_E_NORESOURCE,
                               AAR_REASON_HOSTHEAP);
    assert(strstr(line, "shared card memory"));
    aar_run_claim_failure_line(line, sizeof(line), AAR_E_NORESOURCE,
                               AAR_REASON_LIMIT);
    assert(strstr(line, "HOSTMEM"));
    /* A refusal without a known reason still says it was a refusal. */
    aar_run_claim_failure_line(line, sizeof(line), AAR_E_NORESOURCE,
                               AAR_REASON_NONE);
    assert(strcmp(line, "aar-run: claim refused: refused by the platform") == 0);
    /* The reason is only read for a refusal. */
    aar_run_claim_failure_line(line, sizeof(line), AAR_E_NOMEM,
                               AAR_REASON_TABLES);
    assert(strcmp(line, "aar-run: claim failed (no memory)") == 0);
    /* No platform, or no mmu.library: both come back as NOSYS. The line names
     * no board, because aar-run runs on any platform. */
    aar_run_claim_failure_line(line, sizeof(line), AAR_E_NOSYS,
                               AAR_REASON_NONE);
    assert(strstr(line, "claim failed") && strstr(line, "cannot admit"));
    assert(!strstr(line, "ZZ9000") && !strstr(line, "zz9k"));
    aar_run_claim_failure_line(line, sizeof(line), AAR_E_IO, AAR_REASON_NONE);
    assert(strcmp(line, "aar-run: claim failed (io error)") == 0);
}

static void test_tags_failure_lines(void)
{
    char line[256];

    aar_run_tags_failure_line(line, sizeof(line), AAR_E_INVAL);
    assert(strstr(line, "MAXMEM, HOSTMEM or WEIGHT is out of range"));
    aar_run_tags_failure_line(line, sizeof(line), AAR_E_NOSYS);
    assert(strstr(line, "does not take WEIGHT"));
    aar_run_tags_failure_line(line, sizeof(line), AAR_E_BUSY);
    assert(strcmp(line, "aar-run: options failed (busy)") == 0);
    aar_run_tags_failure_line(line, sizeof(line), AAR_E_STALE);
    assert(strcmp(line, "aar-run: options failed (claim revoked)") == 0);
}

static void test_load_failure_lines(void)
{
    char line[256];

    /* Ctrl-C comes back as AAR_E_IO with the break signal left set. */
    aar_run_load_failure_line(line, sizeof(line), AAR_E_IO, AAR_REASON_NONE, 1,
                              16u * AAR_RUN_MIB_BYTES);
    assert(strcmp(line, "aar-run: stopped during load") == 0);
    /* The same code without a break is a failed read. */
    aar_run_load_failure_line(line, sizeof(line), AAR_E_IO, AAR_REASON_NONE, 0,
                              16u * AAR_RUN_MIB_BYTES);
    assert(strcmp(line, "aar-run: load failed (io error)") == 0);
    aar_run_load_failure_line(line, sizeof(line), AAR_E_NOENT, AAR_REASON_NONE,
                              0, 16u * AAR_RUN_MIB_BYTES);
    assert(strstr(line, "cannot open the ELF"));
    /* An empty or oversized image, or one the loader refuses. The limit
     * comes from AARInfo, not from a constant of aar-run. */
    aar_run_load_failure_line(line, sizeof(line), AAR_E_INVAL, AAR_REASON_NONE,
                              0, 16u * AAR_RUN_MIB_BYTES);
    assert(strstr(line, "larger than the 16 MiB image limit"));
    aar_run_load_failure_line(line, sizeof(line), AAR_E_INVAL, AAR_REASON_NONE,
                              0, 32u * AAR_RUN_MIB_BYTES);
    assert(strstr(line, "larger than the 32 MiB image limit"));
    /* A refusal for lack of a resource names it, from the status. */
    aar_run_load_failure_line(line, sizeof(line), AAR_E_NORESOURCE,
                              AAR_REASON_LIMIT, 0, 16u * AAR_RUN_MIB_BYTES);
    assert(strstr(line, "load refused:") && strstr(line, "HOSTMEM"));
    aar_run_load_failure_line(line, sizeof(line), AAR_E_PROFILE,
                              AAR_REASON_NONE, 0, 16u * AAR_RUN_MIB_BYTES);
    assert(strcmp(line, "aar-run: load failed (profile not supported)") == 0);
    aar_run_load_failure_line(line, sizeof(line), AAR_E_NOGROUP,
                              AAR_REASON_NONE, 0, 16u * AAR_RUN_MIB_BYTES);
    assert(strstr(line, "service group not available"));
}

static void test_start_and_generic_failure_lines(void)
{
    char line[256];

    aar_run_failure_line(line, sizeof(line), "start", AAR_E_INVAL,
                         AAR_REASON_NONE);
    assert(strcmp(line, "aar-run: start failed (bad request)") == 0);
    aar_run_failure_line(line, sizeof(line), "start", AAR_E_NORESOURCE,
                         AAR_REASON_MEMORY);
    assert(strstr(line, "start refused:") && strstr(line, "record"));
    /* A short buffer truncates and stays a string. */
    {
        /* Not a constant, so the compiler does not warn about the truncation
         * this asks for. */
        volatile size_t room = 8u;

        aar_run_failure_line(line, room, "start", AAR_E_INVAL, AAR_REASON_NONE);
    }
    assert(strlen(line) == 7u);
}

static void test_query_verdicts(void)
{
    AARInfo info = platform_info();

    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_OK);
    assert(aar_run_query_verdict_text(AAR_RUN_QUERY_OK)[0] == '\0');
    /* No platform is required: not the ZZ9000, not another by number, and
     * not one that reports no Zorro III word. aar-run runs on any. */
    info.platform_id = AAR_PLATFORM_NONE;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_OK);
    info.platform_id = 99u;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_OK);
    info = zz9000_info();
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_OK);
    info.platform[AAR_ZZ9000_INFO_CAPS] = AAR_ZZ9000_CAP_DISPLAY;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_OK);
    /* The API major aar-run was built for. */
    info = platform_info();
    info.api_major = AAR_API_MAJOR + 1u;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_API);
    assert(strstr(aar_run_query_verdict_text(AAR_RUN_QUERY_API), "API major"));
    assert(!strstr(aar_run_query_verdict_text(AAR_RUN_QUERY_API), "Zorro"));
    /* What the platform takes is not a question of the query check: the
     * limits are read from AARInfo and checked against the launch. */
    info = platform_info();
    info.max_args = 1u;
    info.max_arg_bytes = 8u;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_OK);
}

static void test_zz9000_note_is_for_the_zz9000_only(void)
{
    AARInfo info = zz9000_info();

    /* The one ZZ9000 specific rule aar-run keeps: a board that is not a
     * Zorro III board gets a note, and runs on. */
    assert(!aar_run_zz9000_lacks_z3(&info));
    info.platform[AAR_ZZ9000_INFO_CAPS] = AAR_ZZ9000_CAP_DISPLAY;
    assert(aar_run_zz9000_lacks_z3(&info));
    /* Another platform's words do not mean ZZ9000 caps. */
    info = platform_info();
    info.platform[AAR_ZZ9000_INFO_CAPS] = 0u;
    assert(!aar_run_zz9000_lacks_z3(&info));
}

static void test_query_failure_lines(void)
{
    char line[256];

    aar_run_query_failure_line(line, sizeof(line), AAR_E_NOSYS);
    assert(strstr(line, "no AAR platform"));
    assert(!strstr(line, "ZZ9000") && !strstr(line, "zz9k"));
    aar_run_query_failure_line(line, sizeof(line), AAR_E_IO);
    assert(strstr(line, "AARQuery failed (io error)"));
    assert(!strstr(line, "Zorro"));
}

/* ---- The probe before the claim (D5) ----------------------------------------- */

static AARImageInfo image_info(void)
{
    AARImageInfo img;

    memset(&img, 0, sizeof(img));
    img.size = sizeof(img);
    img.profile = AAR_PROFILE_A32;
    img.api_major = AAR_API_MAJOR;
    img.api_minor_required = AAR_API_MINOR;
    img.groups_required = AAR_GROUP_DISPLAY | AAR_GROUP_INPUT;
    img.groups_optional = AAR_GROUP_AUDIO;
    img.note_version = 1u;
    img.image_bytes = 4096u;
    return img;
}

static void test_probe_accepts_an_image_the_platform_runs(void)
{
    AARInfo info = platform_info();
    AARImageInfo img = image_info();

    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_OK);
    /* Optional groups are a hint: the platform may lack them. */
    info.groups = AAR_GROUP_DISPLAY | AAR_GROUP_INPUT;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_OK);
    /* An app that wants less than the platform has. */
    img.api_minor_required = 0u;
    img.groups_required = 0u;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_OK);
    /* A platform with both profiles runs either. */
    info.profiles = AAR_PROFILEF_A32 | AAR_PROFILEF_A64;
    img.profile = AAR_PROFILE_A64;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_OK);
}

static void test_probe_refuses_each_cause(void)
{
    AARInfo info = platform_info();
    AARImageInfo img = image_info();

    /* The probe's own answers: the file, then the note. */
    assert(aar_run_probe_check(AAR_E_NOENT, &img, &info) == AAR_RUN_PROBE_NOENT);
    assert(aar_run_probe_check(AAR_E_IO, &img, &info) == AAR_RUN_PROBE_IO);
    assert(aar_run_probe_check(AAR_E_INVAL, &img, &info) == AAR_RUN_PROBE_NOT_AAR);
    assert(aar_run_probe_check(AAR_E_NOMEM, &img, &info) == AAR_RUN_PROBE_FAILED);

    /* The wrong profile: an A64 image on a platform that runs A32. */
    img.profile = AAR_PROFILE_A64;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_PROFILE);
    /* ... and an A32 image on a platform that runs A64 only. */
    info.profiles = AAR_PROFILEF_A64;
    img.profile = AAR_PROFILE_A32;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_PROFILE);
    /* A profile the platform has no bit for, and one outside the mask. */
    info = platform_info();
    img.profile = 9u;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_PROFILE);
    img.profile = 0u;
    info.profiles = 0xFFFFFFFFu;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_PROFILE);
    img.profile = 33u;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_PROFILE);
    /* A profile the launcher does not know but the platform runs is the
     * platform's to judge: it is not refused here. */
    img.profile = 9u;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_OK);

    /* A group the app requires and the platform lacks. */
    img = image_info();
    img.groups_required = AAR_GROUP_DISPLAY | AAR_GROUP_GL;
    info.groups = AAR_GROUP_DISPLAY | AAR_GROUP_INPUT;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_GROUP);
    info.groups = 0u;
    img.groups_required = AAR_GROUP_SERVICES;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_GROUP);

    /* An app that needs a newer API than the platform has: a newer minor, or
     * another major. The library's probe refuses a note newer than the
     * library, so this is a library that is newer than its platform. */
    info = platform_info();
    img = image_info();
    info.api_minor = AAR_API_MINOR - 1u;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_API);
    info = platform_info();
    img.api_major = AAR_API_MAJOR + 1u;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_API);

    /* One image with several faults is refused for the first: the profile, as
     * the more basic, then the API, then the groups. */
    img = image_info();
    img.profile = AAR_PROFILE_A64;
    img.api_minor_required = AAR_API_MINOR + 1u;
    img.groups_required = AAR_GROUP_GL;
    info.groups = 0u;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_PROFILE);
    img.profile = AAR_PROFILE_A32;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_API);
    img.api_minor_required = 0u;
    assert(aar_run_probe_check(AAR_OK, &img, &info) == AAR_RUN_PROBE_GROUP);
}

static void test_probe_lines_name_the_cause(void)
{
    AARInfo info = platform_info();
    AARImageInfo img = image_info();
    char line[300];

    aar_run_probe_line(line, sizeof(line), AAR_RUN_PROBE_NOENT, "RAM:a.elf", &img, &info);
    assert(strstr(line, "cannot open RAM:a.elf"));
    aar_run_probe_line(line, sizeof(line), AAR_RUN_PROBE_IO, "RAM:a.elf", &img, &info);
    assert(strstr(line, "cannot read RAM:a.elf"));
    aar_run_probe_line(line, sizeof(line), AAR_RUN_PROBE_NOT_AAR, "RAM:a.elf", &img,
                       &info);
    assert(strstr(line, "RAM:a.elf is not an AAR image"));
    aar_run_probe_line(line, sizeof(line), AAR_RUN_PROBE_FAILED, "RAM:a.elf", &img,
                       &info);
    assert(strstr(line, "cannot probe RAM:a.elf"));

    img.profile = AAR_PROFILE_A64;
    aar_run_probe_line(line, sizeof(line), AAR_RUN_PROBE_PROFILE, "RAM:a.elf", &img,
                       &info);
    assert(strstr(line, "A64 app") && strstr(line, "runs A32 apps only"));
    info.profiles = AAR_PROFILEF_A32 | AAR_PROFILEF_A64;
    img.profile = 9u;
    aar_run_probe_line(line, sizeof(line), AAR_RUN_PROBE_PROFILE, "RAM:a.elf", &img,
                       &info);
    assert(strstr(line, "unknown profile 9") && strstr(line, "A32 and A64"));

    info = platform_info();
    info.api_minor = 3u;
    img = image_info();
    img.api_minor_required = 5u;
    aar_run_probe_line(line, sizeof(line), AAR_RUN_PROBE_API, "RAM:a.elf", &img, &info);
    assert(strstr(line, "needs AAR API 0.5") && strstr(line, "has 0.3"));

    img = image_info();
    img.groups_required = AAR_GROUP_DISPLAY | AAR_GROUP_GL | AAR_GROUP_SERVICES;
    info = platform_info();
    info.groups = AAR_GROUP_DISPLAY | AAR_GROUP_INPUT;
    aar_run_probe_line(line, sizeof(line), AAR_RUN_PROBE_GROUP, "RAM:a.elf", &img, &info);
    assert(strstr(line, "lacks: gl, services"));
    assert(!strstr(line, "display"));

    /* A name that fills the line is cut, never overrun. The buffer comes from
     * the heap so a fortified build cannot see its size and reject the cut
     * this case is about. */
    {
        volatile size_t room = 24u;
        char *tiny = malloc(room);

        assert(tiny);
        aar_run_probe_line(tiny, room, AAR_RUN_PROBE_NOT_AAR,
                           "RAM:a-rather-long-name.elf", &img, &info);
        assert(strlen(tiny) == room - 1u);
        free(tiny);
    }
}

static void test_group_names(void)
{
    char text[64];

    aar_run_group_names(text, sizeof(text), AAR_GROUP_DISPLAY);
    assert(strcmp(text, "display") == 0);
    aar_run_group_names(text, sizeof(text), AAR_GROUPS_DEFINED);
    assert(strcmp(text, "display, input, audio, gl, services") == 0);
    aar_run_group_names(text, sizeof(text), AAR_GROUP_INPUT | 0x100u);
    assert(strcmp(text, "input, 0x100") == 0);
    aar_run_group_names(text, sizeof(text), 0u);
    assert(strcmp(text, "none") == 0);
}

static void test_probe_refusal_exit_codes(void)
{
    /* A refusal is a launcher error: 20, like a refused load. A Ctrl-C that
     * stopped the probe's open or read is the user's: 5. */
    assert(aar_run_probe_exit(AAR_RUN_PROBE_NOENT, 0, 0) == 20);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_IO, 0, 0) == 20);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_NOT_AAR, 0, 0) == 20);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_PROFILE, 0, 0) == 20);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_API, 0, 0) == 20);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_GROUP, 0, 0) == 20);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_FAILED, 0, 0) == 20);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_IO, 1, 0) == 5);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_NOENT, 0, 1) == 5);
    /* A decided refusal stays 20 whatever else is pending. */
    assert(aar_run_probe_exit(AAR_RUN_PROBE_PROFILE, 1, 1) == 20);
    assert(aar_run_probe_exit(AAR_RUN_PROBE_NOT_AAR, 1, 0) == 20);
}

static void test_probe_may_wait_for_the_locate(void)
{
    /* A name with a volume or assign colon may be a multi-directory assign:
     * the library's own open tries one directory, and aar-run finds the file
     * with its DOS packets once it has a serving loop. Only that case waits;
     * every other verdict, and a missing plain path, is final at once. */
    assert(aar_run_probe_may_wait(AAR_RUN_PROBE_NOENT, "SYS:a.elf"));
    assert(aar_run_probe_may_wait(AAR_RUN_PROBE_NOENT, "Games:q/a.elf"));
    assert(!aar_run_probe_may_wait(AAR_RUN_PROBE_NOENT, "a.elf"));
    assert(!aar_run_probe_may_wait(AAR_RUN_PROBE_NOENT, "dir/a.elf"));
    assert(!aar_run_probe_may_wait(AAR_RUN_PROBE_NOT_AAR, "SYS:a.elf"));
    assert(!aar_run_probe_may_wait(AAR_RUN_PROBE_PROFILE, "SYS:a.elf"));
    assert(!aar_run_probe_may_wait(AAR_RUN_PROBE_OK, "SYS:a.elf"));
    assert(!aar_run_probe_may_wait(AAR_RUN_PROBE_NOENT, NULL));
}

/* ---- STATUS and RELEASE through AARExt ---------------------------------------- */

static LONG fake_list(ULONG cursor, AARZZ9000List *info)
{
    (void)cursor;
    (void)info;
    return 0;
}

static LONG fake_release(ULONG handle)
{
    (void)handle;
    return AAR_OK;
}

static void test_admin_commands_need_the_extension(void)
{
    aar_client_ext_zz9000 ext;
    char line[160];

    /* AARExt returns NULL on a platform without the table: not supported. */
    assert(!aar_run_admin_available(NULL));
    aar_run_admin_unsupported_line(line, sizeof(line), "STATUS");
    assert(strcmp(line, "aar-run: STATUS is not supported on this platform.") == 0);
    aar_run_admin_unsupported_line(line, sizeof(line), "RELEASE");
    assert(strcmp(line, "aar-run: RELEASE is not supported on this platform.") == 0);

    /* A table that does not reach both calls is not enough either. */
    memset(&ext, 0, sizeof(ext));
    ext.size = offsetof(aar_client_ext_zz9000, release_handle);
    ext.list = fake_list;
    ext.release_handle = fake_release;
    assert(!aar_run_admin_available(&ext));
    /* Both calls covered by the size but one not set: still not usable. */
    ext.size = sizeof(ext);
    ext.release_handle = NULL;
    assert(!aar_run_admin_available(&ext));
    ext.release_handle = fake_release;
    assert(aar_run_admin_available(&ext));
    ext.list = NULL;
    assert(!aar_run_admin_available(&ext));
}

/* ---- The launcher's own constants and diagnostics (D6, D7, D13, D14) ---------- */

static void test_launcher_constants(void)
{
    assert(strcmp(AAR_RUN_VERSION, "0.1") == 0);
    assert(AAR_SERVE_MIN_STACK == 16384UL);
    assert(AAR_RUN_DEFAULT_POLL_US == 1000UL);
    assert(AAR_RUN_DEFAULT_POLL_US == AAR_SERVE_DEFAULT_POLL_US);
}

static void test_poll_argument(void)
{
    uint32_t us = 1000u;

    assert(aar_run_poll_arg("POLL=20000", &us) == AAR_RUN_ARG_TAKEN && us == 20000u);
    assert(aar_run_poll_arg("poll=250", &us) == AAR_RUN_ARG_TAKEN && us == 250u);
    /* 0 is the library signal alone. */
    assert(aar_run_poll_arg("POLL=0", &us) == AAR_RUN_ARG_TAKEN && us == 0u);
    assert(aar_run_poll_arg("POLL=999999", &us) == AAR_RUN_ARG_TAKEN && us == 999999u);
    us = 77u;
    /* The timer's microsecond field is below one second. */
    assert(aar_run_poll_arg("POLL=1000000", &us) == AAR_RUN_ARG_BAD);
    assert(aar_run_poll_arg("POLL=-1", &us) == AAR_RUN_ARG_BAD);
    assert(aar_run_poll_arg("POLL=", &us) == AAR_RUN_ARG_BAD);
    assert(aar_run_poll_arg("POLL=fast", &us) == AAR_RUN_ARG_BAD);
    assert(aar_run_poll_arg("POLL=10ms", &us) == AAR_RUN_ARG_BAD);
    assert(us == 77u);
    assert(aar_run_poll_arg("POLL", &us) == AAR_RUN_ARG_OTHER);
    assert(aar_run_poll_arg("POLLING=5", &us) == AAR_RUN_ARG_OTHER);
    assert(aar_run_poll_arg("WEIGHT=2", &us) == AAR_RUN_ARG_OTHER);
    assert(aar_run_poll_arg("", &us) == AAR_RUN_ARG_OTHER);
    assert(aar_run_poll_arg(NULL, &us) == AAR_RUN_ARG_OTHER);
    assert(us == 77u);
}

static void test_display_argument(void)
{
    uint32_t strategy = 9u;

    assert(aar_run_display_arg("DISPLAY=BUFFERS", &strategy) == AAR_RUN_ARG_TAKEN &&
           strategy == AAR_SERVE_DISPLAY_BUFFERS);
    assert(aar_run_display_arg("DISPLAY=PAN3", &strategy) == AAR_RUN_ARG_TAKEN &&
           strategy == AAR_SERVE_DISPLAY_PAN3);
    assert(aar_run_display_arg("display=pan2", &strategy) == AAR_RUN_ARG_TAKEN &&
           strategy == AAR_SERVE_DISPLAY_PAN2);
    assert(aar_run_display_arg("Display=Single", &strategy) == AAR_RUN_ARG_TAKEN &&
           strategy == AAR_SERVE_DISPLAY_SINGLE);
    strategy = 9u;
    assert(aar_run_display_arg("DISPLAY=PAN4", &strategy) == AAR_RUN_ARG_BAD);
    assert(aar_run_display_arg("DISPLAY=", &strategy) == AAR_RUN_ARG_BAD);
    assert(aar_run_display_arg("DISPLAY=BUFFERS2", &strategy) == AAR_RUN_ARG_BAD);
    assert(aar_run_display_arg("DISPLAY=1", &strategy) == AAR_RUN_ARG_BAD);
    assert(strategy == 9u);
    assert(aar_run_display_arg("DISPLAY", &strategy) == AAR_RUN_ARG_OTHER);
    assert(aar_run_display_arg("DISPLAYS=PAN2", &strategy) == AAR_RUN_ARG_OTHER);
    assert(aar_run_display_arg("POLL=5", &strategy) == AAR_RUN_ARG_OTHER);
    assert(aar_run_display_arg(NULL, &strategy) == AAR_RUN_ARG_OTHER);
    assert(strategy == 9u);
}

static void test_error_names(void)
{
    assert(strcmp(aar_run_error_name(AAR_OK), "ok") == 0);
    assert(strcmp(aar_run_error_name(AAR_E_STALE), "claim revoked") == 0);
    assert(strcmp(aar_run_error_name(AAR_E_OWNER),
                  "handle belongs to another task") == 0);
    assert(strcmp(aar_run_error_name(-99), "error") == 0);
}

/* "aar-run status" lists the claims; it must not try to load a file called "status". */
static void test_status_keyword_is_case_insensitive(void)
{
    assert(aar_run_is_status_keyword("STATUS"));
    assert(aar_run_is_status_keyword("status"));
    assert(aar_run_is_status_keyword("Status"));
    assert(!aar_run_is_status_keyword("status.elf"));
    assert(!aar_run_is_status_keyword("STATU"));
    assert(!aar_run_is_status_keyword(""));
}

static void test_claim_loss_does_not_look_like_a_clean_exit(void)
{
    assert(strcmp(aar_run_claim_loss_text(0, AAR_END_LEASE), "lease expired") == 0);
    assert(strcmp(aar_run_claim_loss_text(0, AAR_END_QUARANTINED), "quarantined") == 0);
    assert(strcmp(aar_run_claim_loss_text(1, AAR_END_NONE),
                  "stale handle; final status unavailable") == 0);
    assert(aar_run_claim_loss_text(0, AAR_END_EXIT) == NULL);
    assert(aar_run_claim_loss_text(0, AAR_END_NONE) == NULL);
}

static void test_load_break_consumed_by_packet_returns_warn(void)
{
    /* dos_packet_async() clears the signal and records user_stop. */
    assert(aar_run_load_failure_exit(0, 1) == 5);
    assert(aar_run_load_failure_exit(1, 0) == 5);
    assert(aar_run_load_failure_exit(1, 1) == 5);
    assert(aar_run_load_failure_exit(0, 0) == 20);
}

/* ---- Shipping an app: ToolTypes, ARGS, documents, profile files ---------------- */

/* An icon is a NULL-ended list of key, value pairs. */
static const char *fake_tool_type(void *icon, const char *key)
{
    const char *const *p = (const char *const *)icon;

    for (; p[0]; p += 2)
        if (strcmp(p[0], key) == 0)
            return p[1];
    return NULL;
}

static struct aar_run_tt_chain chain_of(const char *const *project,
                                        const char *const *tool)
{
    struct aar_run_tt_chain chain;

    chain.lookup = fake_tool_type;
    chain.project = (void *)project;
    chain.tool = (void *)tool;
    return chain;
}

static void test_project_tooltypes_come_before_the_tool_icon(void)
{
    static const char *const project[] = {"MODE", "0x5000", "ARGS", "-w 640",
                                          "HOSTMEM", "8", NULL};
    static const char *const tool[] = {"MODE", "$1234", "ARGS", "ignored",
                                       "MAXMEM", "64", "APP", "Work:g.elf",
                                       "HOSTMEM", "4", "JOYPORT0", "",
                                       "AUDIO", "AHI", "ROOT", "Work:g",
                                       "WEIGHT", "3", NULL};
    struct aar_run_tt_chain chain = chain_of(project, tool);
    struct aar_run_settings s;
    const char *bad = "x";

    assert(aar_run_tt_settings(&chain, &s, &bad));
    assert(bad == NULL);
    /* what the project sets wins ... */
    assert(s.have_mode && s.mode == 0x5000u);
    assert(s.hostmem_mib == 8u);
    assert(s.args && strcmp(s.args, "-w 640") == 0);
    /* ... and the tool icon fills in the rest */
    assert(s.maxmem_mib == 64u && s.weight == 3u);
    assert(strcmp(s.app, "Work:g.elf") == 0 && strcmp(s.root, "Work:g") == 0);
    assert(s.joy_port0 && s.audio_ahi && !s.abs_paths);
}

static void test_a_project_flag_set_off_beats_the_tool_icon(void)
{
    static const char *const project[] = {"ABS", "NO", "JOYPORT0", "0",
                                          "ROOT", "", NULL};
    static const char *const tool[] = {"ABS", "", "JOYPORT0", "YES",
                                       "AUDIO", "AHI", "ROOT", "Work:g",
                                       "APP", "Work:t.elf", NULL};
    struct aar_run_tt_chain chain = chain_of(project, tool);
    struct aar_run_settings s;
    const char *bad;

    assert(aar_run_tt_settings(&chain, &s, &bad));
    assert(!s.abs_paths && !s.joy_port0);
    /* AUDIO has no off value: the tool's stays */
    assert(s.audio_ahi);
    /* an empty ROOT of the project is a value: the tool's is not used */
    assert(s.root[0] == '\0');
    /* a key only the tool has */
    assert(strcmp(s.app, "Work:t.elf") == 0);
}

static void test_tooltypes_without_a_project_icon(void)
{
    static const char *const tool[] = {"ABS", "", "MAXMEM", "128", NULL};
    struct aar_run_tt_chain chain = chain_of(NULL, tool);
    struct aar_run_settings s;
    const char *bad;

    assert(aar_run_tt_settings(&chain, &s, &bad));
    assert(s.abs_paths && s.maxmem_mib == 128u && !s.have_mode && !s.args);
    /* no icon at all */
    chain = chain_of(NULL, NULL);
    assert(aar_run_tt_settings(&chain, &s, &bad));
    assert(!s.abs_paths && !s.joy_port0 && !s.audio_ahi && s.maxmem_mib == 0u &&
           s.app[0] == '\0' && s.root[0] == '\0' && !s.args);
}

static void test_a_bad_project_value_is_not_masked_by_the_tool_icon(void)
{
    static const char *const project[] = {"MAXMEM", "0", NULL};
    static const char *const tool[] = {"MAXMEM", "64", NULL};
    static const char *const project_host[] = {"HOSTMEM", "12x", NULL};
    static const char *const project_weight[] = {"WEIGHT", "", NULL};
    static const char *const project_mode[] = {"MODE", "zz", NULL};
    static const char *const project_mode_empty[] = {"MODE", "", NULL};
    static const char *const project_audio[] = {"AUDIO", "PAULA", NULL};
    char long_value[257]; /* 256 characters: one more than APP and ROOT take */
    const char *project_long_app[3];
    const char *project_long_root[3];
    struct aar_run_tt_chain chain = chain_of(project, tool);
    struct aar_run_settings s;
    const char *bad = NULL;

    project_long_app[0] = "APP";
    project_long_app[1] = long_value;
    project_long_app[2] = NULL;
    project_long_root[0] = "ROOT";
    project_long_root[1] = long_value;
    project_long_root[2] = NULL;

    assert(!aar_run_tt_settings(&chain, &s, &bad));
    assert(strcmp(bad, "MAXMEM") == 0);
    chain = chain_of(project_host, tool);
    assert(!aar_run_tt_settings(&chain, &s, &bad));
    assert(strcmp(bad, "HOSTMEM") == 0);
    chain = chain_of(project_weight, tool);
    assert(!aar_run_tt_settings(&chain, &s, &bad));
    assert(strcmp(bad, "WEIGHT") == 0);
    /* a MODE that is not a number, an AUDIO other than AHI and a name that
     * does not fit are refused too, and so is an empty MODE */
    chain = chain_of(project_mode, tool);
    assert(!aar_run_tt_settings(&chain, &s, &bad) && strcmp(bad, "MODE") == 0);
    chain = chain_of(project_mode_empty, tool);
    assert(!aar_run_tt_settings(&chain, &s, &bad) && strcmp(bad, "MODE") == 0);
    chain = chain_of(project_audio, tool);
    assert(!aar_run_tt_settings(&chain, &s, &bad) && strcmp(bad, "AUDIO") == 0);
    memset(long_value, 'a', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    chain = chain_of(project_long_app, tool);
    assert(!aar_run_tt_settings(&chain, &s, &bad) && strcmp(bad, "APP") == 0);
    chain = chain_of(project_long_root, tool);
    assert(!aar_run_tt_settings(&chain, &s, &bad) && strcmp(bad, "ROOT") == 0);
}

static void test_tooltype_value_forms(void)
{
    uint32_t mode = 0;

    assert(aar_run_flag_value("") && aar_run_flag_value("1") &&
           aar_run_flag_value("Y") && aar_run_flag_value("yes") &&
           aar_run_flag_value("True"));
    assert(!aar_run_flag_value(NULL) && !aar_run_flag_value("0") &&
           !aar_run_flag_value("no") && !aar_run_flag_value("11"));
    assert(aar_run_tool_is_ahi("AHI") && aar_run_tool_is_ahi("ahi") &&
           !aar_run_tool_is_ahi("AH") && !aar_run_tool_is_ahi("AHIX") &&
           !aar_run_tool_is_ahi(NULL));
    assert(aar_run_parse_mode("$5000", &mode) && mode == 0x5000u);
    assert(aar_run_parse_mode("0X5001", &mode) && mode == 0x5001u);
    assert(aar_run_parse_mode("20480", &mode) && mode == 20480u);
    assert(!aar_run_parse_mode("", &mode) && !aar_run_parse_mode("12z", &mode) &&
           !aar_run_parse_mode(NULL, &mode));
}

/* The words of an ARGS value in a fresh list (the list of the call before is freed). */
static int words_of(const char *text, struct aar_run_args *a)
{
    aar_run_args_free(a);
    return aar_run_args_add_words(a, text);
}

static void test_args_words_and_quotes(void)
{
    struct aar_run_args a = {0};

    /* plain words */
    assert(words_of("  one\ttwo   three ", &a) == AAR_RUN_ARGS_OK && a.n == 3);
    assert(strcmp(a.v[0], "one") == 0 && strcmp(a.v[1], "two") == 0 &&
           strcmp(a.v[2], "three") == 0 && a.longest == 5u);
    aar_run_args_free(&a);
    /* a quoted part keeps its spaces and tabs */
    assert(words_of("\"My Games:a b.elf\" -x \"c\td\"", &a) == AAR_RUN_ARGS_OK &&
           a.n == 3);
    assert(strcmp(a.v[0], "My Games:a b.elf") == 0 && strcmp(a.v[1], "-x") == 0 &&
           strcmp(a.v[2], "c\td") == 0);
    aar_run_args_free(&a);
    /* a quoted part inside a word joins the pieces */
    assert(words_of("--name=\"a b\"c d", &a) == AAR_RUN_ARGS_OK && a.n == 2);
    assert(strcmp(a.v[0], "--name=a bc") == 0 && strcmp(a.v[1], "d") == 0);
    aar_run_args_free(&a);
    /* "" is an empty word; adjacent quotes make one word */
    assert(words_of("\"\" x \"a\"\"b\"", &a) == AAR_RUN_ARGS_OK && a.n == 3);
    assert(a.v[0][0] == '\0' && strcmp(a.v[1], "x") == 0 &&
           strcmp(a.v[2], "ab") == 0);
    aar_run_args_free(&a);
    /* single quotes and the AmigaDOS star are plain characters */
    assert(words_of("'a b' *\"x\"", &a) == AAR_RUN_ARGS_OK && a.n == 3);
    assert(strcmp(a.v[0], "'a") == 0 && strcmp(a.v[1], "b'") == 0 &&
           strcmp(a.v[2], "*x") == 0);
    aar_run_args_free(&a);
    /* nothing */
    assert(words_of("", &a) == AAR_RUN_ARGS_OK && a.n == 0);
    assert(words_of("   \t", &a) == AAR_RUN_ARGS_OK && a.n == 0);
    assert(words_of(NULL, &a) == AAR_RUN_ARGS_OK && a.n == 0);
    aar_run_args_free(&a);
}

static void test_args_with_an_unbalanced_quote_add_nothing(void)
{
    struct aar_run_args a = {0};

    assert(words_of("\"abc", &a) == AAR_RUN_ARGS_QUOTE && a.n == 0);
    assert(words_of("x \"a b", &a) == AAR_RUN_ARGS_QUOTE && a.n == 0);
    assert(words_of("x y\"", &a) == AAR_RUN_ARGS_QUOTE && a.n == 0);
    assert(words_of("\"a\" \"", &a) == AAR_RUN_ARGS_QUOTE && a.n == 0);
    /* what was there before the bad ARGS stays */
    assert(aar_run_args_add(&a, "first") == AAR_RUN_ARGS_OK);
    assert(aar_run_args_add_words(&a, "ok \"bad") == AAR_RUN_ARGS_QUOTE);
    assert(a.n == 1 && strcmp(a.v[0], "first") == 0);
    aar_run_args_free(&a);
}

static void test_args_grow_past_their_first_block(void)
{
    struct aar_run_args a;
    char text[200];
    int i;

    memset(&a, 0, sizeof(a));
    for (i = 0; i < 40; i++)
        assert(aar_run_args_add(&a, "x") == AAR_RUN_ARGS_OK);
    assert(a.n == 40 && a.cap >= 40);
    text[0] = '\0';
    for (i = 0; i < 30; i++)
        strcat(text, "ab ");
    assert(aar_run_args_add_words(&a, text) == AAR_RUN_ARGS_OK && a.n == 70);
    aar_run_args_free(&a);
    assert(a.n == 0 && a.v == NULL);
}

static void expect_doc(const char *root, const char *path, int kind,
                       const char *text)
{
    char out[256];

    assert(aar_run_doc_arg(root, path, out, sizeof(out)) == kind);
    assert(strcmp(out, text) == 0);
}

static void test_documents_are_relative_under_the_root(void)
{
    expect_doc("Work:Games/Q", "Work:Games/Q/maps/e1.bsp", AAR_RUN_DOC_RELATIVE,
               "maps/e1.bsp");
    expect_doc("Work:Games/Q", "Work:Games/Q/save", AAR_RUN_DOC_RELATIVE, "save");
    /* a volume root: the name after the colon, no slash to skip */
    expect_doc("Work:", "Work:docs/a.txt", AAR_RUN_DOC_RELATIVE, "docs/a.txt");
    expect_doc("Work:", "Work:a.txt", AAR_RUN_DOC_RELATIVE, "a.txt");
    /* a trailing slash of the root is not part of it */
    expect_doc("Work:Games/", "Work:Games/a.txt", AAR_RUN_DOC_RELATIVE, "a.txt");
    expect_doc("Work:Games//", "Work:Games/a.txt", AAR_RUN_DOC_RELATIVE, "a.txt");
    /* AmigaDOS names do not depend on case; the path keeps its own */
    expect_doc("WORK:games", "Work:Games/Maps/E1", AAR_RUN_DOC_RELATIVE, "Maps/E1");
}

static void test_documents_outside_the_root_are_absolute(void)
{
    /* another directory, another volume, a sibling with the root's name as a prefix */
    expect_doc("Work:Games/Q", "Work:Docs/a.txt", AAR_RUN_DOC_ABSOLUTE,
               "Work:Docs/a.txt");
    expect_doc("Work:Games/Q", "Ram:a.txt", AAR_RUN_DOC_ABSOLUTE, "Ram:a.txt");
    expect_doc("Work:Games/Q", "Work:Games/Q2/a", AAR_RUN_DOC_ABSOLUTE,
               "Work:Games/Q2/a");
    expect_doc("Work:Games", "Work:Gam", AAR_RUN_DOC_ABSOLUTE, "Work:Gam");
    expect_doc("Work:Games/Q", "Work:Games", AAR_RUN_DOC_ABSOLUTE, "Work:Games");
    /* the root itself, and the volume root as a document of a deeper root */
    expect_doc("Work:Games", "Work:Games", AAR_RUN_DOC_ABSOLUTE, "Work:Games");
    expect_doc("Work:", "Work:", AAR_RUN_DOC_ABSOLUTE, "Work:");
    expect_doc("Work:Games", "Work:", AAR_RUN_DOC_ABSOLUTE, "Work:");
    /* a path with nothing after the root's slash has no relative form */
    expect_doc("Work:Games", "Work:Games/", AAR_RUN_DOC_ABSOLUTE, "Work:Games/");
    /* no root known */
    expect_doc("", "Work:a", AAR_RUN_DOC_ABSOLUTE, "Work:a");
    expect_doc(NULL, "Work:a", AAR_RUN_DOC_ABSOLUTE, "Work:a");
}

static void test_a_document_that_does_not_fit_is_reported(void)
{
    char out[9];

    assert(aar_run_doc_arg("Work:", "Work:abcdefgh", out, sizeof(out)) ==
           AAR_RUN_DOC_RELATIVE);
    assert(strcmp(out, "abcdefgh") == 0);
    assert(aar_run_doc_arg("Work:", "Work:abcdefghi", out, sizeof(out)) ==
           AAR_RUN_DOC_NOFIT);
    /* the absolute form is the one that does not fit */
    assert(aar_run_doc_arg("Work:", "Ram:abcdef", out, sizeof(out)) ==
           AAR_RUN_DOC_NOFIT);
}

static void test_too_many_documents_hit_the_platform_limit(void)
{
    AARInfo info = platform_info(); /* 16 strings: argv[0] and 15 arguments */
    struct aar_run_limits lim = aar_run_limits_from_info(&info);
    struct aar_run_args a;
    char text[160];
    char rel[160];
    int i;

    memset(&a, 0, sizeof(a));
    assert(aar_run_args_add_words(&a, "-w 640 -f") == AAR_RUN_ARGS_OK);
    for (i = 0; i < 12; i++)
        assert(aar_run_args_add(&a, "doc") == AAR_RUN_ARGS_OK);
    assert(a.n == 15);
    assert(aar_run_limits_check(&lim, (uint32_t)a.n, a.longest, 0u, 0u) ==
           AAR_RUN_LIMIT_OK);
    assert(aar_run_args_add(&a, "one too many") == AAR_RUN_ARGS_OK);
    assert(aar_run_limits_check(&lim, (uint32_t)a.n, a.longest, 0u, 0u) ==
           AAR_RUN_LIMIT_ARGS);
    aar_run_args_free(&a);

    /* the platform's argument size counts the document's path: a deep
     * document passes relative to the root and fails in full */
    memset(&a, 0, sizeof(a));
    memset(text, 'd', 136);
    memcpy(text, "Work:Games/", 11);
    text[136] = '\0';
    assert(aar_run_doc_arg("Work:Games", text, rel, sizeof(rel)) ==
           AAR_RUN_DOC_RELATIVE);
    assert(strlen(rel) == 125);
    assert(aar_run_args_add(&a, rel) == AAR_RUN_ARGS_OK);
    assert(aar_run_limits_check(&lim, (uint32_t)a.n, a.longest, 0u, 0u) ==
           AAR_RUN_LIMIT_OK);
    assert(aar_run_doc_arg("Ram:", text, rel, sizeof(rel)) ==
           AAR_RUN_DOC_ABSOLUTE);
    assert(aar_run_args_add(&a, rel) == AAR_RUN_ARGS_OK);
    assert(aar_run_limits_check(&lim, (uint32_t)a.n, a.longest, 0u, 0u) ==
           AAR_RUN_LIMIT_ARG_BYTES);
    aar_run_args_free(&a);
}

static void expect_parent(const char *full, const char *want)
{
    char out[64];

    memset(out, 'x', sizeof(out));
    if (want) {
        assert(aar_run_path_parent(full, out, sizeof(out)));
        assert(strcmp(out, want) == 0);
    } else {
        assert(!aar_run_path_parent(full, out, sizeof(out)));
    }
}

static void test_the_default_root_is_the_directory_of_the_image(void)
{
    char small[8];

    expect_parent("Work:Games/Q/quake.elf", "Work:Games/Q");
    expect_parent("Work:Games/quake.a64.elf", "Work:Games");
    /* a file in the root of a volume: the volume, with its colon */
    expect_parent("Work:quake.elf", "Work:");
    /* a name with no directory part */
    expect_parent("quake.elf", NULL);
    expect_parent("Work:", NULL);
    expect_parent("Work:Games/", NULL);
    expect_parent("", NULL);
    expect_parent("/quake.elf", NULL);
    expect_parent("Work:a//quake.elf", NULL);
    /* the result has to fit with its NUL */
    assert(aar_run_path_parent("Work:Ga/x", small, sizeof(small)) == 1 &&
           strcmp(small, "Work:Ga") == 0);
    assert(aar_run_path_parent("Work:Gam/x", small, sizeof(small)) == 0);
    assert(aar_run_path_parent("Work:Games/Q/quake.elf", small, sizeof(small)) == 0);
}

static void test_a_drawer_project_has_an_icon_by_its_path(void)
{
    char out[32];

    /* the drawer's lock is its own, with no name: its path without a slash */
    assert(aar_run_drawer_icon_path("Work:Games", out, sizeof(out)) &&
           strcmp(out, "Work:Games") == 0);
    assert(aar_run_drawer_icon_path("Work:Games/Q", out, sizeof(out)) &&
           strcmp(out, "Work:Games/Q") == 0);
    assert(aar_run_drawer_icon_path("Work:Games/", out, sizeof(out)) &&
           strcmp(out, "Work:Games") == 0);
    assert(aar_run_drawer_icon_path("Work:Games//", out, sizeof(out)) &&
           strcmp(out, "Work:Games") == 0);
    /* a volume keeps its colon: icon.library reads Work:disk.info */
    assert(aar_run_drawer_icon_path("Work:", out, sizeof(out)) &&
           strcmp(out, "Work:") == 0);
    /* nothing to look up */
    assert(!aar_run_drawer_icon_path("", out, sizeof(out)));
    assert(!aar_run_drawer_icon_path("/", out, sizeof(out)));
    /* the path has to fit with its NUL */
    assert(aar_run_drawer_icon_path("Work:Ga", out, 8) && strcmp(out, "Work:Ga") == 0);
    assert(!aar_run_drawer_icon_path("Work:Gam", out, 8));
}

static void test_the_first_project_is_the_app_unless_it_is_not_an_image(void)
{
    /* an AAR image, or a file the probe could not open or read: the app */
    assert(aar_run_project_is_app(0, AAR_OK));
    assert(aar_run_project_is_app(0, AAR_E_NOENT));
    assert(aar_run_project_is_app(0, AAR_E_IO));
    /* not an AAR image: a document */
    assert(!aar_run_project_is_app(0, AAR_E_INVAL));
    /* a drawer is never the app */
    assert(!aar_run_project_is_app(1, AAR_OK));
    assert(!aar_run_project_is_app(1, AAR_E_NOENT));
}

static void expect_candidates(const char *name, uint32_t profiles, size_t count,
                              const char *c1, const char *c2, const char *c3)
{
    char list[AAR_RUN_CAND_MAX][256];
    const char *want[3];
    size_t i;

    want[0] = c1;
    want[1] = c2;
    want[2] = c3;
    assert(aar_run_image_candidates(name, profiles, &list[0][0],
                                    sizeof(list[0])) == count);
    assert(strcmp(list[0], name) == 0);
    for (i = 1; i < count; i++)
        assert(strcmp(list[i], want[i - 1]) == 0);
}

static void test_profile_files_are_tried_after_the_file_asked_for(void)
{
    const uint32_t both = AAR_PROFILEF_A32 | AAR_PROFILEF_A64;

    /* name.elf: both profile files, then the stem */
    expect_candidates("Game.elf", both, 4, "Game.a64.elf", "Game.a32.elf", "Game");
    /* name: the two profile files */
    expect_candidates("Game", both, 3, "Game.a64.elf", "Game.a32.elf", NULL);
    /* the platform's profile first; the other after it */
    expect_candidates("Game", AAR_PROFILEF_A32, 3, "Game.a32.elf", "Game.a64.elf",
                      NULL);
    expect_candidates("Game", AAR_PROFILEF_A64, 3, "Game.a64.elf", "Game.a32.elf",
                      NULL);
    expect_candidates("Game.elf", AAR_PROFILEF_A32, 4, "Game.a32.elf",
                      "Game.a64.elf", "Game");
    /* a platform that lists none: A64 then A32 */
    expect_candidates("Game", 0u, 3, "Game.a64.elf", "Game.a32.elf", NULL);
    /* paths, any case of the suffix, a dot in the name */
    expect_candidates("Work:Games/Q/quake.ELF", both, 4,
                      "Work:Games/Q/quake.a64.elf", "Work:Games/Q/quake.a32.elf",
                      "Work:Games/Q/quake");
    expect_candidates("v1.2/tool", both, 3, "v1.2/tool.a64.elf",
                      "v1.2/tool.a32.elf", NULL);
    /* a profile file asked for by name has the same two siblings */
    expect_candidates("Game.a32.elf", both, 4, "Game.a32.a64.elf",
                      "Game.a32.a32.elf", "Game.a32");
}

static void test_profile_candidates_for_names_without_a_file_part(void)
{
    char list[AAR_RUN_CAND_MAX][256];
    char longname[300];

    /* only the name itself: nothing to build a sibling from */
    assert(aar_run_image_candidates("", AAR_PROFILEF_A64, &list[0][0],
                                    sizeof(list[0])) == 1);
    assert(aar_run_image_candidates("Work:", AAR_PROFILEF_A64, &list[0][0],
                                    sizeof(list[0])) == 1);
    assert(aar_run_image_candidates("dir/", AAR_PROFILEF_A64, &list[0][0],
                                    sizeof(list[0])) == 1);
    assert(aar_run_image_candidates(".elf", AAR_PROFILEF_A64, &list[0][0],
                                    sizeof(list[0])) == 3);
    /* a name that fills the buffer has no room for a sibling */
    memset(longname, 'n', sizeof(longname));
    longname[255] = '\0';
    assert(aar_run_image_candidates(longname, AAR_PROFILEF_A64, &list[0][0],
                                    sizeof(list[0])) == 1);
    longname[250] = '\0'; /* 251 bytes with the NUL: no room for ".a64.elf" */
    assert(aar_run_image_candidates(longname, AAR_PROFILEF_A64, &list[0][0],
                                    sizeof(list[0])) == 1);
    longname[240] = '\0'; /* 241 + 8 fits 256 */
    assert(aar_run_image_candidates(longname, AAR_PROFILEF_A64, &list[0][0],
                                    sizeof(list[0])) == 3);
    /* a name that does not fit at all */
    memset(longname, 'n', sizeof(longname));
    longname[299] = '\0';
    assert(aar_run_image_candidates(longname, AAR_PROFILEF_A64, &list[0][0],
                                    sizeof(list[0])) == 0);
}

static void test_the_refusal_reported_is_the_first_image_that_exists(void)
{
    enum aar_run_probe_verdict v[4];

    /* nothing exists: the file asked for */
    v[0] = v[1] = v[2] = v[3] = AAR_RUN_PROBE_NOENT;
    assert(aar_run_report_candidate(v, 4) == 0);
    /* the asked-for file is an image of the other profile; the sibling is missing */
    v[0] = AAR_RUN_PROBE_PROFILE;
    assert(aar_run_report_candidate(v, 4) == 0);
    /* the asked-for file is missing; a sibling exists and cannot run here */
    v[0] = AAR_RUN_PROBE_NOENT;
    v[1] = AAR_RUN_PROBE_PROFILE;
    assert(aar_run_report_candidate(v, 4) == 1);
    v[1] = AAR_RUN_PROBE_NOENT;
    v[2] = AAR_RUN_PROBE_NOT_AAR;
    assert(aar_run_report_candidate(v, 4) == 2);
    /* the first one wins */
    v[1] = AAR_RUN_PROBE_API;
    assert(aar_run_report_candidate(v, 4) == 1);
    /* a file that cannot be read is a refusal too */
    v[0] = AAR_RUN_PROBE_IO;
    assert(aar_run_report_candidate(v, 4) == 0);
}

static void test_the_choice_line_names_both_files(void)
{
    char line[160];

    aar_run_choice_line(line, sizeof(line), "Game.elf", "Game.a64.elf");
    assert(strcmp(line, "aar-run: using Game.a64.elf instead of Game.elf") == 0);
}

int main(void)
{
    test_project_tooltypes_come_before_the_tool_icon();
    test_a_project_flag_set_off_beats_the_tool_icon();
    test_tooltypes_without_a_project_icon();
    test_a_bad_project_value_is_not_masked_by_the_tool_icon();
    test_tooltype_value_forms();
    test_args_words_and_quotes();
    test_args_with_an_unbalanced_quote_add_nothing();
    test_args_grow_past_their_first_block();
    test_documents_are_relative_under_the_root();
    test_documents_outside_the_root_are_absolute();
    test_a_document_that_does_not_fit_is_reported();
    test_too_many_documents_hit_the_platform_limit();
    test_the_default_root_is_the_directory_of_the_image();
    test_a_drawer_project_has_an_icon_by_its_path();
    test_the_first_project_is_the_app_unless_it_is_not_an_image();
    test_profile_files_are_tried_after_the_file_asked_for();
    test_profile_candidates_for_names_without_a_file_part();
    test_the_refusal_reported_is_the_first_image_that_exists();
    test_the_choice_line_names_both_files();
    test_ahi_open_failure_exits_20();
    test_a_break_that_was_seen_stays_seen();
    test_the_stack_is_the_one_the_command_runs_on();
    test_corrupt_queue_exits_20();
    test_positive_mib_parse();
    test_limits_are_read_from_aarinfo();
    test_argument_budget_comes_from_the_platform();
    test_weight_bound_comes_from_the_platform();
    test_hostmem_bound_comes_from_the_platform();
    test_limit_lines_name_the_platform_value();
    test_launcher_constants();
    test_poll_argument();
    test_display_argument();
    test_weight_argument_forms();
    test_weight_is_stripped_from_the_app_arguments();
    test_tag_list_carries_only_the_set_limits();
    test_tag_list_follows_the_library_rules();
    test_tag_list_is_refused_the_way_the_library_would();
    test_handle_parse();
    test_refusal_text_names_each_reason();
    test_client_ccaps();
    test_app_exit_code();
    test_release_is_for_finished_claims();
    test_block_reason_names_match_protocol();
    test_end_reason_names();
    test_open_failure_lines();
    test_tags_failure_lines();
    test_load_failure_lines();
    test_start_and_generic_failure_lines();
    test_query_verdicts();
    test_zz9000_note_is_for_the_zz9000_only();
    test_query_failure_lines();
    test_probe_accepts_an_image_the_platform_runs();
    test_probe_refuses_each_cause();
    test_probe_lines_name_the_cause();
    test_group_names();
    test_probe_refusal_exit_codes();
    test_probe_may_wait_for_the_locate();
    test_admin_commands_need_the_extension();
    test_error_names();
    test_status_keyword_is_case_insensitive();
    test_claim_loss_does_not_look_like_a_clean_exit();
    test_load_break_consumed_by_packet_returns_warn();
    puts("run_policy_test: ok");
    return 0;
}
