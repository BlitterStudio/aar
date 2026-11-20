/*
 * Host tests of the decisions the launcher aar-run makes: arguments and tool types,
 * the library options, the texts for a refused claim, load or start, the checks of
 * what AARQuery said, the exit code, and the ZZ9000 list helpers.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aar_run_policy.h"

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

static void test_empty_argument_is_allowed(void)
{
    assert(aar_run_arg_length_allowed(0u));
    assert(aar_run_arg_length_allowed(127u));
    assert(!aar_run_arg_length_allowed(128u));
}

static void test_maxmem_requires_positive_decimal_mib(void)
{
    uint32_t mib = 0;

    assert(aar_run_parse_maxmem("64", &mib) && mib == 64u);
    assert(aar_run_parse_maxmem("4294967295", &mib) && mib == UINT32_MAX);
    assert(!aar_run_parse_maxmem("0", &mib));
    assert(!aar_run_parse_maxmem("", &mib));
    assert(!aar_run_parse_maxmem("64MiB", &mib));
    assert(!aar_run_parse_maxmem("4294967296", &mib));
}

static void test_hostmem_requires_positive_decimal_mib(void)
{
    uint32_t mib = 7u;

    assert(aar_run_parse_hostmem("8", &mib) && mib == 8u);
    assert(aar_run_parse_hostmem("62", &mib) && mib == 62u);
    assert(aar_run_parse_hostmem("4095", &mib) && mib == 4095u);
    mib = 7u;
    assert(!aar_run_parse_hostmem("4096", &mib) && mib == 7u);
    assert(!aar_run_parse_hostmem("4294967296", &mib));
    assert(!aar_run_parse_hostmem("0", &mib));
    assert(!aar_run_parse_hostmem("", &mib));
    assert(!aar_run_parse_hostmem("-1", &mib));
    assert(!aar_run_parse_hostmem("8MiB", &mib));
    assert(!aar_run_parse_hostmem(" 8", &mib));
    assert(!aar_run_parse_hostmem(NULL, &mib));
    assert(!aar_run_parse_hostmem("8", NULL));
}

static AARInfo zz9000_info(void)
{
    AARInfo info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.api_major = AAR_API_MAJOR;
    info.platform_id = AAR_PLATFORM_ZZ9000;
    info.platform[AAR_ZZ9000_INFO_CAPS] = AAR_ZZ9000_CAP_Z3;
    info.max_args = 16u;
    info.max_arg_bytes = 128u;
    info.max_image_bytes = 16u * AAR_RUN_MIB_BYTES;
    info.weight_max = 256u;
    return info;
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
    lib->info = zz9000_info();
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
        case AAR_TAG_SHARED_MIB:
            if (t->ti_Data > 4095u)
                return AAR_E_INVAL;
            next.shared_mib = t->ti_Data;
            break;
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

static void test_weight_is_one_to_the_platform_bound(void)
{
    uint32_t w = 7u;

    assert(AAR_RUN_WEIGHT_MAX == 256u);
    assert(aar_run_parse_weight("1", &w) && w == 1u);
    assert(aar_run_parse_weight("100", &w) && w == 100u);
    assert(aar_run_parse_weight("256", &w) && w == 256u);
    assert(aar_run_parse_weight("0256", &w) && w == 256u);
    w = 7u;
    assert(!aar_run_parse_weight("0", &w) && w == 7u);
    assert(!aar_run_parse_weight("257", &w) && w == 7u);
    assert(!aar_run_parse_weight("4294967296", &w));
    assert(!aar_run_parse_weight("-1", &w));
    assert(!aar_run_parse_weight("", &w));
    assert(!aar_run_parse_weight("abc", &w));
    assert(!aar_run_parse_weight("8x", &w));
    assert(!aar_run_parse_weight("8 ", &w));
    assert(!aar_run_parse_weight(" 8", &w));
    assert(!aar_run_parse_weight("+8", &w));
    assert(!aar_run_parse_weight(NULL, &w));
    assert(!aar_run_parse_weight("8", NULL));
    assert(w == 7u);
}

static void test_weight_argument_forms(void)
{
    uint32_t w = 0u;

    assert(aar_run_weight_arg("WEIGHT=1", &w) == AAR_RUN_ARG_TAKEN && w == 1u);
    assert(aar_run_weight_arg("WEIGHT=256", &w) == AAR_RUN_ARG_TAKEN && w == 256u);
    assert(aar_run_weight_arg("weight=3", &w) == AAR_RUN_ARG_TAKEN && w == 3u);
    assert(aar_run_weight_arg("Weight=17", &w) == AAR_RUN_ARG_TAKEN && w == 17u);
    w = 5u;
    assert(aar_run_weight_arg("WEIGHT=0", &w) == AAR_RUN_ARG_BAD);
    assert(aar_run_weight_arg("WEIGHT=257", &w) == AAR_RUN_ARG_BAD);
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
        char *bad[] = {"+x", "WEIGHT=257", "y"};

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
    n = aar_run_build_tags(tags, 0u, 0u, AAR_RUN_WEIGHT_MAX, 0);
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

    /* The largest values aar-run accepts on the command line are accepted. */
    fake_library_init(&lib);
    (void)aar_run_build_tags(tags, 4095u, AAR_RUN_HOSTMEM_MAX_MIB,
                             AAR_RUN_WEIGHT_MAX, 0);
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
    (void)aar_run_build_tags(tags, 64u, 0u, AAR_RUN_WEIGHT_MAX + 1u, 0);
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
    /* No platform, or no mmu.library: both come back as NOSYS. */
    aar_run_claim_failure_line(line, sizeof(line), AAR_E_NOSYS,
                               AAR_REASON_NONE);
    assert(strstr(line, "claim failed") && strstr(line, "mmu.library 41"));
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
    AARInfo info = zz9000_info();

    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_OK);
    assert(aar_run_query_verdict_text(AAR_RUN_QUERY_OK)[0] == '\0');
    /* Not a Zorro III board. */
    info.platform[AAR_ZZ9000_INFO_CAPS] = AAR_ZZ9000_CAP_DISPLAY;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_NO_Z3);
    assert(strstr(aar_run_query_verdict_text(AAR_RUN_QUERY_NO_Z3), "Zorro III"));
    /* Another platform's words do not mean ZZ9000 caps. */
    info = zz9000_info();
    info.platform_id = AAR_PLATFORM_ZZ9000 + 1u;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_NOT_ZZ9000);
    /* The API major aar-run was built for. */
    info = zz9000_info();
    info.api_major = AAR_API_MAJOR + 1u;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_API);
    assert(strstr(aar_run_query_verdict_text(AAR_RUN_QUERY_API),
                  "app-runtime firmware"));
    /* aar-run passes up to 16 strings of 127 bytes: the platform must take
     * them (this replaces the old ZZAPP_ARGV_* constants). */
    info = zz9000_info();
    info.max_args = AAR_RUN_ARGV_MAX - 1u;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_ARGS);
    assert(!aar_run_args_fit(&info));
    assert(strstr(aar_run_query_verdict_text(AAR_RUN_QUERY_ARGS), "arguments"));
    info = zz9000_info();
    info.max_arg_bytes = AAR_RUN_ARGV_STR_MAX - 1u;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_ARGS);
    info = zz9000_info();
    info.max_args = 32u;
    info.max_arg_bytes = 256u;
    assert(aar_run_query_check(&info) == AAR_RUN_QUERY_OK);
    /* The longest string aar_run_arg_length_allowed() lets through, plus its
     * NUL, is what max_arg_bytes counts. */
    assert(aar_run_arg_length_allowed(AAR_RUN_ARGV_STR_MAX - 1u));
    assert(!aar_run_arg_length_allowed(AAR_RUN_ARGV_STR_MAX));
}

static void test_query_failure_lines(void)
{
    char line[256];

    aar_run_query_failure_line(line, sizeof(line), AAR_E_NOSYS);
    assert(strstr(line, "zz9k.library 36") && strstr(line, "not found"));
    aar_run_query_failure_line(line, sizeof(line), AAR_E_IO);
    assert(strstr(line, "AARQuery failed (io error)"));
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

int main(void)
{
    test_ahi_open_failure_exits_20();
    test_corrupt_queue_exits_20();
    test_empty_argument_is_allowed();
    test_maxmem_requires_positive_decimal_mib();
    test_hostmem_requires_positive_decimal_mib();
    test_weight_is_one_to_the_platform_bound();
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
    test_query_failure_lines();
    test_error_names();
    test_status_keyword_is_case_insensitive();
    test_claim_loss_does_not_look_like_a_clean_exit();
    test_load_break_consumed_by_packet_returns_warn();
    puts("run_policy_test: ok");
    return 0;
}
