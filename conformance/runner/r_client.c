/*
 * Runner side of C-CLIENT (conformance/TESTS.md): aar.library as a client sees
 * it. This file belongs to the C-CLIENT author.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <exec/libraries.h>

#include <stdio.h>
#include <string.h>

#include "aar-conf.h"

static int is_pow2(ULONG v)
{
    return v != 0 && (v & (v - 1UL)) == 0;
}

static void pass_fail(int idx, int ok, const char *detail)
{
    conf_set_result(idx, ok ? CONF_R_PASS : CONF_R_FAIL, detail);
}

/* C-CLIENT.1 */
static void t_lib_version(int idx)
{
    char d[CONF_DETAIL_MAX];
    int ok = AARBase->lib_Version == AAR_LIB_VERSION &&
             AAR_LIB_VERSION == AAR_API_MAJOR;

    conf_detailf(d, "aar.library %u.%u, API major %u", (unsigned)AARBase->lib_Version,
                 (unsigned)AARBase->lib_Revision, (unsigned)AAR_API_MAJOR);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.2: AARQuery with a small size must change only that many bytes; with
 * a size larger than the structure it must write no more than the structure. */
static void t_query_size(int idx)
{
    ULONG buf[40];
    UBYTE *b = (UBYTE *)buf;
    LONG rc;
    int i;
    char d[CONF_DETAIL_MAX];

    for (i = 0; i < 160; i++)
        b[i] = 0xA5;
    buf[0] = 16;
    rc = AARQuery((AARInfo *)buf);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARQuery with size 16 returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    for (i = 16; i < 160; i++) {
        if (b[i] != 0xA5) {
            conf_detailf(d, "size 16 changed byte %d", i);
            pass_fail(idx, 0, d);
            return;
        }
    }
    for (i = 0; i < 160; i++)
        b[i] = 0xA5;
    buf[0] = 160;
    rc = AARQuery((AARInfo *)buf);
    if (rc != AAR_OK) {
        conf_detailf(d, "AARQuery with size 160 returned %ld", (long)rc);
        pass_fail(idx, 0, d);
        return;
    }
    for (i = (int)sizeof(AARInfo); i < 160; i++) {
        if (b[i] != 0xA5) {
            conf_detailf(d, "size 160 wrote beyond the structure, byte %d", i);
            pass_fail(idx, 0, d);
            return;
        }
    }
    if (conf_g.info.reserved0 != 0 || conf_g.info.reserved1[0] != 0 ||
        conf_g.info.reserved1[1] != 0 || conf_g.info.reserved1[2] != 0 ||
        conf_g.info.reserved1[3] != 0) {
        pass_fail(idx, 0, "reserved words of AARInfo are not 0");
        return;
    }
    pass_fail(idx, 1, "size 16 and size 160 respected, reserved words 0");
}

/* C-CLIENT.3 */
static void t_api_version(int idx)
{
    char d[CONF_DETAIL_MAX];
    int ok = conf_g.info.api_major == AAR_API_MAJOR &&
             conf_g.info.api_minor >= AAR_API_MINOR;

    conf_detailf(d, "platform API %lu.%lu, suite %u.%u",
                 (unsigned long)conf_g.info.api_major,
                 (unsigned long)conf_g.info.api_minor, (unsigned)AAR_API_MAJOR,
                 (unsigned)AAR_API_MINOR);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.4 */
static void t_profiles(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG p = conf_g.info.profiles;
    int ok = p != 0 && (p & ~(AAR_PROFILEF_A32 | AAR_PROFILEF_A64)) == 0 &&
             (p & (1UL << (conf_g.profile_value - 1UL))) != 0;

    conf_detailf(d, "profiles 0x%lx, testing %s", (unsigned long)p,
                 conf_g.profile_name);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.5 */
static void t_groups(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG g = conf_g.info.groups;
    int ok = (g & ~(ULONG)(AAR_GROUPS_DEFINED | AAR_GROUPS_EXPERIMENTAL)) == 0;

    conf_detailf(d, "groups 0x%lx", (unsigned long)g);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.6 */
static void t_limits(int idx)
{
    char d[CONF_DETAIL_MAX];
    int ok = conf_g.info.max_image_bytes >= 16UL * 1024UL * 1024UL &&
             conf_g.info.max_args >= 16UL && conf_g.info.max_arg_bytes >= 128UL;

    conf_detailf(d, "max image %lu, args %lu of %lu bytes",
                 (unsigned long)conf_g.info.max_image_bytes,
                 (unsigned long)conf_g.info.max_args,
                 (unsigned long)conf_g.info.max_arg_bytes);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.7 */
static void t_granules(int idx)
{
    char d[CONF_DETAIL_MAX];
    const AARInfo *i = &conf_g.info;
    int ok = is_pow2(i->shared_granule) && i->shared_granule >= 64UL &&
             i->msg_capacity >= 32UL && i->stall_limit_ms >= 10000UL &&
             i->shared_default_mib == AAR_SHARED_DEFAULT_MIB &&
             i->shared_max_mib >= i->shared_default_mib;

    conf_detailf(d, "granule %lu, msg %lu, stall %lu ms, shared %lu of %lu MiB",
                 (unsigned long)i->shared_granule, (unsigned long)i->msg_capacity,
                 (unsigned long)i->stall_limit_ms, (unsigned long)i->shared_default_mib,
                 (unsigned long)i->shared_max_mib);
    pass_fail(idx, ok, d);
}

/* C-CLIENT.8 */
static void t_queue_depth(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG depth = conf_g.info.queue_depth;
    int ok = is_pow2(depth) && depth >= 64UL && depth <= 4096UL &&
             conf_g.info.msg_capacity == depth - 1UL;

    conf_detailf(d, "queue depth %lu, msg capacity %lu", (unsigned long)depth,
                 (unsigned long)conf_g.info.msg_capacity);
    pass_fail(idx, ok, d);
}

static const struct conf_r_test tests[] = {
    { "C-CLIENT.1", t_lib_version },
    { "C-CLIENT.2", t_query_size },
    { "C-CLIENT.3", t_api_version },
    { "C-CLIENT.4", t_profiles },
    { "C-CLIENT.5", t_groups },
    { "C-CLIENT.6", t_limits },
    { "C-CLIENT.7", t_granules },
    { "C-CLIENT.8", t_queue_depth },
};

const struct conf_area conf_area_client = {
    .name = "C-CLIENT",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
};
