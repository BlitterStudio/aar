/*
 * Conformance payload for C-TABLE (spec 3.3 to 3.5, 8.1, 8.7, 8.8): the table the
 * runtime hands the app, aar_info, the groups and the extension call. See
 * conformance/TESTS.md for the ids and conformance/payloads/conf.h for the
 * protocol.
 *
 * The runner passes its own view of the platform as info: words (groups,
 * platform_id, msg_capacity, stall_limit_ms, stop_grace_ms, and from the C-TABLE
 * variant api_minor, shared_default_mib and aarinfo, which holds the client's flags,
 * shared_granule and profiles). A test that compares the table with
 * the client's AARInfo does so when the word is there; without it (host runner,
 * qemu loader) the test checks what it can and ends in a skip that starts with
 * "host:" if it passed.
 *
 * The tests of a group (C-TABLE.4 to C-TABLE.7, C-TABLE.23) are printed only when
 * the table or the client declares the group (the platform for the ZZ9000).
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/ext_zz9000.h"
#include "aar/note.h"

#include "conf.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

AAR_NOTE(0, 0, 0, 0)

#if (defined(__arm__) || defined(__aarch64__)) && !defined(AAR_HOST)
#define TABLE_ARM 1
#endif

#define MIB ((uint64_t)1024u * 1024u)

/* ---- The table as words ------------------------------------------------------------------- */

/* Slot i of the table (after the 16-byte header), as a pointer-sized word. */
#define SLOT_CORE      0u   /* exit .. clip_write: 42 slots */
#define SLOT_EXT       42u  /* ext, map_file, unmap_file */
#define SLOT_COMMENT   45u  /* file_get_comment, file_set_comment: optional, C-FILE.53 to C-FILE.55 */
#define SLOT_DISPLAY   47u  /* display_open .. display_present_poll: 18 slots */
#define SLOT_AUDIO     65u  /* audio_open, audio_close, audio_stats */
#define SLOT_GL        68u  /* gl_bind, gl_swap, gl_unbind */
#define SLOT_SERVICES  71u  /* sha256 .. audio_decode_close: 8 slots */
#define SLOT_COUNT     82u

static const char *const slot_names[SLOT_COUNT] = {
    "exit", "yield", "should_stop", "time_us", "sleep_us", "info",
    "heap_range", "heap_grow", "heap_shrink", "heap_info",
    "shared_alloc", "shared_free", "publish", "acquire",
    "log", "file_open", "file_close", "file_read", "file_write", "file_seek",
    "file_stat", "file_list_open", "file_list_next", "file_list_close",
    "console_write", "console_read",
    "event_poll", "msg_send", "msg_poll", "msg_wait",
    "file_delete", "file_rename", "dir_create", "file_set_date",
    "file_set_protect", "file_truncate", "volume_info",
    "time_unix_us", "locale_info", "env_get", "clip_read", "clip_write",
    "ext", "map_file", "unmap_file", "file_get_comment", "file_set_comment",
    "display_open", "display_flip", "display_flip_async", "display_flip_idle",
    "display_refresh", "display_vsync", "display_set_palette", "display_close",
    "display_latest_dropped",
    "display_info", "display_modes", "display_resize", "display_set_title",
    "display_pointer", "display_wait_vblank", "display_last_present_us",
    "display_present", "display_present_poll",
    "audio_open", "audio_close", "audio_stats",
    "gl_bind", "gl_swap", "gl_unbind",
    "sha256", "decompress", "image_open", "image_decode", "image_close",
    "audio_decode_open", "audio_decode", "audio_decode_close",
    "file_read_async", "file_write_async", "file_collect",
};

static uint32_t slot_end(uint32_t i)
{
    return 16u + (i + 1u) * (uint32_t)sizeof(void *);
}

/* The table's size covers slot i. */
static int slot_covered(const aar_api *api, uint32_t i)
{
    return api->size >= slot_end(i);
}

/* The word of slot i; the caller has checked slot_covered. */
static const void *slot_word(const aar_api *api, uint32_t i)
{
    const void *const *words = (const void *const *)(const void *)((const uint8_t *)api + 16);

    return words[i];
}

/* Checks slots [first, first + count): covered and non-NULL. */
static void check_slots(struct conf_test *t, const aar_api *api, uint32_t first, uint32_t count,
                        const char *what)
{
    uint32_t i;

    for (i = first; i < first + count; i++) {
        if (!slot_covered(api, i)) {
            if (conf_fail_first(t)) {
                conf_buf_str(&t->why, "the table size ");
                conf_buf_u64(&t->why, api->size);
                conf_buf_str(&t->why, " does not cover ");
                conf_buf_str(&t->why, what);
                conf_buf_str(&t->why, " slot ");
                conf_buf_str(&t->why, slot_names[i]);
            }
            return;
        }
        if (slot_word(api, i) == 0 && conf_fail_first(t)) {
            conf_buf_str(&t->why, what);
            conf_buf_str(&t->why, " slot ");
            conf_buf_str(&t->why, slot_names[i]);
            conf_buf_str(&t->why, " is NULL");
        }
    }
}

/* ---- What the runner says ------------------------------------------------------------------ */

struct cli {
    int have_groups, have_platform, have_msg_capacity, have_stall, have_grace;
    int have_minor, have_shared_default, have_aarinfo;
    uint32_t groups, platform_id, msg_capacity, stall_limit_ms, stop_grace_ms;
    uint32_t api_minor, shared_default_mib;
    uint32_t flags, shared_granule, profiles;
};

/* "info:aarinfo=<flags>,<shared_granule>,<profiles>": three decimal words of the client's
 * AARInfo in one argument (the argument list of an app holds 16 strings). */
static int read_aarinfo(struct cli *c, int argc, char **argv)
{
    const char *v = conf_arg(argc, argv, "info:aarinfo");
    uint32_t w[3];
    uint32_t i;

    if (!v)
        return 0;
    for (i = 0; i < 3u; i++) {
        uint32_t n = 0;
        int digits = 0;

        while (*v >= '0' && *v <= '9') {
            n = n * 10u + (uint32_t)(*v - '0');
            v++;
            digits++;
        }
        if (!digits || (i < 2u ? *v != ',' : *v != '\0'))
            return 0;
        if (i < 2u)
            v++;
        w[i] = n;
    }
    c->flags = w[0];
    c->shared_granule = w[1];
    c->profiles = w[2];
    return 1;
}

static void read_cli(struct cli *c, int argc, char **argv)
{
    c->have_groups = conf_arg_u32(argc, argv, "info:groups", &c->groups);
    c->have_platform = conf_arg_u32(argc, argv, "info:platform_id", &c->platform_id);
    c->have_msg_capacity = conf_arg_u32(argc, argv, "info:msg_capacity", &c->msg_capacity);
    c->have_stall = conf_arg_u32(argc, argv, "info:stall_limit_ms", &c->stall_limit_ms);
    c->have_grace = conf_arg_u32(argc, argv, "info:stop_grace_ms", &c->stop_grace_ms);
    c->have_minor = conf_arg_u32(argc, argv, "info:api_minor", &c->api_minor);
    c->have_shared_default =
        conf_arg_u32(argc, argv, "info:shared_default_mib", &c->shared_default_mib);
    c->have_aarinfo = read_aarinfo(c, argc, argv);
}

/* A test that passed but lacked a word of the runner ends in a skip. */
static void end_or_skip(struct conf_test *t, int partial)
{
    if (!t->failed && partial)
        conf_skip(t->id, "host: the runner's AARInfo word is not there");
    else
        conf_end(t);
}

static void need_info(struct conf_test *t, aar_info *info)
{
    info->size = (uint32_t)sizeof *info;
    conf_check(t, aar_get_info(info) == AAR_OK, "aar_get_info failed");
}

static int is_pow2(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

/* ---- C-TABLE.1 to C-TABLE.3, C-TABLE.10 ------------------------------------------------------ */

static void test_core_slots(const aar_api *api)
{
    struct conf_test t;

    conf_begin(&t, "C-TABLE.1");
    check_slots(&t, api, SLOT_CORE, 42u, "core");
    conf_buf_str(&t.note, "42 core slots, table size ");
    conf_buf_u64(&t.note, api->size);
    conf_end(&t);
}

static void test_version(const aar_api *api, const struct cli *c)
{
    struct conf_test t;

    conf_begin(&t, "C-TABLE.2");
    conf_check(&t, (api->version >> 16) == AAR_API_MAJOR,
               "the high 16 bits of version are not AAR_API_MAJOR");
    if (c->have_minor && (api->version & 0xFFFFu) != c->api_minor && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the low 16 bits of version are ");
        conf_buf_u64(&t.why, api->version & 0xFFFFu);
        conf_buf_str(&t.why, ", AARInfo.api_minor is ");
        conf_buf_u64(&t.why, c->api_minor);
    }
    conf_buf_str(&t.note, "version ");
    conf_buf_hex(&t.note, api->version);
    end_or_skip(&t, !c->have_minor);
}

static void test_groups_word(const aar_api *api, const struct cli *c)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.3");
    need_info(&t, &info);
    if (!t.failed && api->groups != info.groups && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the table groups ");
        conf_buf_hex(&t.why, api->groups);
        conf_buf_str(&t.why, " differ from aar_info.groups ");
        conf_buf_hex(&t.why, info.groups);
    }
    if (c->have_groups && api->groups != c->groups && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the table groups ");
        conf_buf_hex(&t.why, api->groups);
        conf_buf_str(&t.why, " differ from the client's AARInfo.groups ");
        conf_buf_hex(&t.why, c->groups);
    }
    if (c->have_aarinfo && info.flags != c->flags && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "aar_info.flags ");
        conf_buf_hex(&t.why, info.flags);
        conf_buf_str(&t.why, " differ from the client's AARInfo.flags ");
        conf_buf_hex(&t.why, c->flags);
    }
    conf_buf_str(&t.note, "groups ");
    conf_buf_hex(&t.note, api->groups);
    end_or_skip(&t, !c->have_groups || !c->have_aarinfo);
}

static void test_platform_id(const aar_api *api, const struct cli *c)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.10");
    need_info(&t, &info);
    if (!t.failed && api->platform_id != info.platform_id && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the table platform_id ");
        conf_buf_u64(&t.why, api->platform_id);
        conf_buf_str(&t.why, " differs from aar_info.platform_id ");
        conf_buf_u64(&t.why, info.platform_id);
    }
    if (c->have_platform && api->platform_id != c->platform_id && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the table platform_id ");
        conf_buf_u64(&t.why, api->platform_id);
        conf_buf_str(&t.why, " differs from the client's AARInfo.platform_id ");
        conf_buf_u64(&t.why, c->platform_id);
    }
    if (c->have_aarinfo && info.shared_granule != c->shared_granule && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "shared_granule ");
        conf_buf_u64(&t.why, info.shared_granule);
        conf_buf_str(&t.why, " differs from the client's ");
        conf_buf_u64(&t.why, c->shared_granule);
    }
    conf_buf_str(&t.note, "platform_id ");
    conf_buf_u64(&t.note, api->platform_id);
    end_or_skip(&t, !c->have_platform || !c->have_aarinfo);
}

/* ---- C-TABLE.4 to C-TABLE.7: slots of the groups the platform declares ----------------------- */

static void test_group_slots(const aar_api *api, uint32_t declared)
{
    struct conf_test t;

    if (declared & AAR_GROUP_DISPLAY) {
        conf_begin(&t, "C-TABLE.4");
        check_slots(&t, api, SLOT_DISPLAY, 18u, "display");
        conf_end(&t);
    }
    if (declared & AAR_GROUP_AUDIO) {
        conf_begin(&t, "C-TABLE.5");
        /* audio_stats may be NULL; audio_open and audio_close may not. */
        check_slots(&t, api, SLOT_AUDIO, 2u, "audio");
        conf_end(&t);
    }
    if (declared & AAR_GROUP_GL) {
        conf_begin(&t, "C-TABLE.6");
        check_slots(&t, api, SLOT_GL, 3u, "GL");
        conf_end(&t);
    }
    if (declared & AAR_GROUP_SERVICES) {
        conf_begin(&t, "C-TABLE.7");
        check_slots(&t, api, SLOT_SERVICES, 8u, "services");
        conf_end(&t);
    }
}

/* ---- C-TABLE.8: the wrappers of a group that is not declared ------------------------------------- */

/* A clear group bit means every slot of the group that the table covers is absent or
 * NULL (spec 3.3 rule 2), so the wrappers answer AAR_E_NOSYS (0 or NULL for a call that
 * returns no code). A slot that is present although the bit is clear fails the test and
 * is not called. */
static int slot_absent(const aar_api *api, uint32_t i)
{
    return !slot_covered(api, i) || slot_word(api, i) == 0;
}

static void expect_nosys(struct conf_test *t, int rc, const char *name)
{
    if (rc != AAR_E_NOSYS && conf_fail_first(t)) {
        conf_buf_str(&t->why, "aar_");
        conf_buf_str(&t->why, name);
        conf_buf_str(&t->why, " returned ");
        conf_buf_i64(&t->why, rc);
        conf_buf_str(&t->why, " over an absent slot, not AAR_E_NOSYS");
    }
}

static void fail_present(struct conf_test *t, uint32_t slot)
{
    if (conf_fail_first(t)) {
        conf_buf_str(&t->why, "the group bit is clear but slot ");
        conf_buf_str(&t->why, slot_names[slot]);
        conf_buf_str(&t->why, " is not NULL");
    }
}

static void test_clear_groups(const aar_api *api, uint32_t declared)
{
    struct conf_test t;
    aar_surface surf;
    uint64_t frame_id;
    aar_present_result present;
    aar_audio audio;
    aar_audio_stats stats;
    aar_image_params params;
    aar_image_result ires;
    aar_service_result sres;
    aar_audio_decode_result ares;
    uint8_t digest[32];
    uint8_t buf[16];
    uint32_t session = 0;
    uint32_t i;
    uint32_t called = 0;

    conf_begin(&t, "C-TABLE.8");
    for (i = 0; i < (uint32_t)sizeof buf; i++)
        buf[i] = 0;
    surf.id = 0;
    audio.frames = 0;
    stats.size = (uint32_t)sizeof stats;
    params.codec = AAR_IMAGE_PNG;
    params.output_format = AAR_FMT_BGRA8888;
    params.stride = 64u;
    params.rows = 1u;
    params.tile_bytes = 64u;
    params.flags = 0;
    params.tile = buf;
    ires.image_width = 0;
    sres.bytes_written = 0;
    ares.rate = 0;

    if (!(declared & AAR_GROUP_DISPLAY)) {
#define CLEAR_CALL(slot, call, name)           \
    do {                                       \
        if (slot_absent(api, (slot))) {        \
            expect_nosys(&t, (call), (name));  \
            called++;                          \
        } else {                               \
            fail_present(&t, (slot));          \
        }                                      \
    } while (0)
        CLEAR_CALL(SLOT_DISPLAY + 0u, aar_display_open(64u, 64u, AAR_FMT_RGB565, 0u, &surf), "display_open");
        CLEAR_CALL(SLOT_DISPLAY + 1u, aar_display_flip(1u), "display_flip");
        CLEAR_CALL(SLOT_DISPLAY + 2u, aar_display_flip_async(1u), "display_flip_async");
        CLEAR_CALL(SLOT_DISPLAY + 3u, aar_display_flip_idle(), "display_flip_idle");
        CLEAR_CALL(SLOT_DISPLAY + 4u, aar_display_refresh(&surf), "display_refresh");
        CLEAR_CALL(SLOT_DISPLAY + 5u, aar_display_vsync(), "display_vsync");
        CLEAR_CALL(SLOT_DISPLAY + 6u, aar_display_set_palette(1u, 0u, 0u, (const uint32_t *)(const void *)buf),
                   "display_set_palette");
        CLEAR_CALL(SLOT_DISPLAY + 7u, aar_display_close(1u), "display_close");
        if (slot_absent(api, SLOT_DISPLAY + 8u)) {
            conf_check(&t, aar_display_latest_dropped() == 0u,
                       "aar_display_latest_dropped is not 0 over an absent slot");
            called++;
        } else {
            fail_present(&t, SLOT_DISPLAY + 8u);
        }
        CLEAR_CALL(SLOT_DISPLAY + 16u, aar_display_present(1u, 0u, &frame_id), "display_present");
        CLEAR_CALL(SLOT_DISPLAY + 17u, aar_display_present_poll(&present), "display_present_poll");
    }
    if (!(declared & AAR_GROUP_AUDIO)) {
        CLEAR_CALL(SLOT_AUDIO + 0u, aar_audio_open(AAR_AUDIO_RATE_22050, 2u, &audio), "audio_open");
        CLEAR_CALL(SLOT_AUDIO + 1u, aar_audio_close(), "audio_close");
        CLEAR_CALL(SLOT_AUDIO + 2u, aar_audio_get_stats(&stats), "audio_get_stats");
    }
    if (!(declared & AAR_GROUP_GL)) {
        if (slot_absent(api, SLOT_GL + 0u)) {
            conf_check(&t, aar_gl_bind(1u) == 0, "aar_gl_bind is not NULL over an absent slot");
            called++;
        } else {
            fail_present(&t, SLOT_GL + 0u);
        }
        if (slot_absent(api, SLOT_GL + 1u) && slot_absent(api, SLOT_GL + 2u)) {
            aar_gl_swap();
            aar_gl_unbind();
            called += 2;
        } else {
            if (!slot_absent(api, SLOT_GL + 1u))
                fail_present(&t, SLOT_GL + 1u);
            if (!slot_absent(api, SLOT_GL + 2u))
                fail_present(&t, SLOT_GL + 2u);
        }
    }
    if (!(declared & AAR_GROUP_SERVICES)) {
        CLEAR_CALL(SLOT_SERVICES + 0u, aar_sha256(buf, sizeof buf, digest), "sha256");
        CLEAR_CALL(SLOT_SERVICES + 1u, aar_decompress(AAR_COMPRESS_DEFLATE_RAW, 0u, buf, 0u, buf, sizeof buf, &sres),
                   "decompress");
        CLEAR_CALL(SLOT_SERVICES + 2u, aar_image_open(&params, &session, &ires), "image_open");
        CLEAR_CALL(SLOT_SERVICES + 3u, aar_image_decode(1u, buf, sizeof buf, 0u, &ires), "image_decode");
        CLEAR_CALL(SLOT_SERVICES + 4u, aar_image_close(1u), "image_close");
        CLEAR_CALL(SLOT_SERVICES + 5u, aar_audio_decode_open(AAR_AUDIO_MP3, &session), "audio_decode_open");
        CLEAR_CALL(SLOT_SERVICES + 6u,
                   aar_audio_decode(1u, buf, sizeof buf, 0u, (int16_t *)(void *)buf, 4u, &ares),
                   "audio_decode");
        CLEAR_CALL(SLOT_SERVICES + 7u, aar_audio_decode_close(1u), "audio_decode_close");
    }
#undef CLEAR_CALL
    if (called == 0)
        conf_buf_str(&t.note, "every group is declared");
    else {
        conf_buf_u64(&t.note, called);
        conf_buf_str(&t.note, " wrappers over NULL slots returned AAR_E_NOSYS, 0 or NULL");
    }
    conf_end(&t);
}

/* ---- C-TABLE.30: group bits that no version of the standard gives to a platform --------------- */

/* Bits 5 to 27 are reserved for later versions (spec 8.1): a platform does not set one in
 * the table or in aar_info. Bits 28 to 31 are for experiments and are allowed. The client's
 * AARInfo.groups is checked by C-CLIENT.5. */
#define GROUPS_RESERVED 0x0FFFFFE0u

static void test_group_bits(const aar_api *api)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.30");
    need_info(&t, &info);
    if ((api->groups & GROUPS_RESERVED) != 0u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "the table groups ");
        conf_buf_hex(&t.why, api->groups);
        conf_buf_str(&t.why, " set a bit from 5 to 27");
    }
    if (!t.failed && (info.groups & GROUPS_RESERVED) != 0u && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "aar_info.groups ");
        conf_buf_hex(&t.why, info.groups);
        conf_buf_str(&t.why, " sets a bit from 5 to 27");
    }
    conf_buf_str(&t.note, "groups ");
    conf_buf_hex(&t.note, api->groups);
    conf_end(&t);
}

/* ---- C-TABLE.9: map_file, unmap_file, ext ------------------------------------------------------------ */

#define MAP_FIXTURE "conf_table_map.bin"
#define MAP_BYTES 16u

static void test_optional_slots(const aar_api *api)
{
    struct conf_test t;
    const void *data = 0;
    uint64_t bytes = 0;
    int rc;

    conf_begin(&t, "C-TABLE.9");
    if (slot_absent(api, SLOT_EXT)) {
        conf_check(&t, aar_ext(AAR_PLATFORM_NONE, 1u) == 0, "aar_ext is not NULL over an absent slot");
    }
    if (slot_absent(api, SLOT_EXT + 1u) || slot_absent(api, SLOT_EXT + 2u)) {
        /* Absent: the wrappers answer AAR_E_NOSYS and do nothing. A present
         * map_file without unmap_file is not called: the mapping could not be
         * released. */
        if (slot_absent(api, SLOT_EXT + 1u)) {
            rc = aar_map_file(MAP_FIXTURE, &data, &bytes);
            conf_check(&t, rc == AAR_E_NOSYS, "aar_map_file over an absent slot is not AAR_E_NOSYS");
        }
        if (slot_absent(api, SLOT_EXT + 2u))
            aar_unmap_file(&bytes);
        conf_buf_str(&t.note, "absent: AAR_E_NOSYS");
    } else {
        /* Present: it must work. A fixture of 16 bytes is written first. */
        uint8_t content[MAP_BYTES];
        uint32_t i;
        int fd;
        uint32_t n = 0;

        for (i = 0; i < MAP_BYTES; i++)
            content[i] = (uint8_t)(0x30u + i);
        fd = aar_file_open(MAP_FIXTURE, AAR_FILE_WRITE | AAR_FILE_CREATE | AAR_FILE_TRUNCATE);
        if (fd >= AAR_FD_BASE) {
            int wr = aar_file_write(fd, content, MAP_BYTES, &n);

            (void)aar_file_close(fd);
            if (wr != AAR_OK || n != MAP_BYTES)
                fd = -1;
        }
        if (fd < AAR_FD_BASE) {
            /* No fixture: a missing file must at least be refused cleanly. */
            rc = aar_map_file("conf_table_missing.bin", &data, &bytes);
            conf_check(&t, rc == AAR_E_NOENT || rc == AAR_E_NOSYS || rc == AAR_E_ACCES,
                       "aar_map_file of a missing file returned a code other than AAR_E_NOENT, AAR_E_NOSYS or AAR_E_ACCES");
            conf_buf_str(&t.note, "present; no fixture could be written, a missing file was refused");
        } else {
            rc = aar_map_file(MAP_FIXTURE, &data, &bytes);
            if (rc == AAR_E_NOSYS) {
                conf_buf_str(&t.note, "present, answers AAR_E_NOSYS (allowed)");
            } else if (rc != AAR_OK) {
                if (conf_fail_first(&t)) {
                    conf_buf_str(&t.why, "aar_map_file of a file just written returned ");
                    conf_buf_i64(&t.why, rc);
                }
            } else {
                const uint8_t *p = (const uint8_t *)data;

                conf_check(&t, data != 0 && bytes == MAP_BYTES, "aar_map_file returned a wrong size or NULL");
                if (data && bytes == MAP_BYTES) {
                    for (i = 0; i < MAP_BYTES; i++)
                        conf_check(&t, p[i] == content[i], "the mapped bytes differ from the file");
                }
                aar_unmap_file(data);
                conf_buf_str(&t.note, "present and works");
            }
        }
    }
    conf_end(&t);
}

/* ---- C-TABLE.11 to C-TABLE.19: aar_info ------------------------------------------------------------------ */

static void test_info_small(void)
{
    struct conf_test t;
    uint64_t buf[16];
    uint8_t *b = (uint8_t *)buf;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-TABLE.11");
    for (i = 0; i < sizeof buf; i++)
        b[i] = 0xA5u;
    ((aar_info *)(void *)buf)->size = 16u;
    rc = aar_get_info((aar_info *)(void *)buf);
    conf_check(&t, rc == AAR_OK, "aar_get_info with a size of 16 failed");
    for (i = 16u; i < sizeof buf; i++) {
        if (b[i] != 0xA5u) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "a size of 16 changed byte ");
                conf_buf_u64(&t.why, i);
            }
            break;
        }
    }
    conf_end(&t);
}

static void test_info_large(void)
{
    struct conf_test t;
    uint64_t buf[32];
    uint8_t *b = (uint8_t *)buf;
    const uint32_t size = 192u;
    uint32_t i;
    int rc;

    conf_begin(&t, "C-TABLE.12");
    for (i = 0; i < sizeof buf; i++)
        b[i] = 0xA5u;
    ((aar_info *)(void *)buf)->size = size;
    rc = aar_get_info((aar_info *)(void *)buf);
    conf_check(&t, rc == AAR_OK, "aar_get_info with a size of 192 failed");
    for (i = (uint32_t)sizeof(aar_info); i < size; i++) {
        if (b[i] != 0u) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "byte ");
                conf_buf_u64(&t.why, i);
                conf_buf_str(&t.why, " past the structure is not zeroed");
            }
            break;
        }
    }
    for (i = size; i < (uint32_t)sizeof buf; i++) {
        if (b[i] != 0xA5u) {
            if (conf_fail_first(&t)) {
                conf_buf_str(&t.why, "byte ");
                conf_buf_u64(&t.why, i);
                conf_buf_str(&t.why, " past the size was written");
            }
            break;
        }
    }
    conf_end(&t);
}

static void test_info_profile(const struct cli *c)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.13");
    need_info(&t, &info);
    if (c->have_aarinfo &&
        !(info.profile >= 1u && info.profile <= 32u &&
          ((c->profiles >> (info.profile - 1u)) & 1u) != 0u) &&
        conf_fail_first(&t)) {
        conf_buf_str(&t.why, "aar_info.profile ");
        conf_buf_u64(&t.why, info.profile);
        conf_buf_str(&t.why, " is not in the client's AARInfo.profiles ");
        conf_buf_hex(&t.why, c->profiles);
    }
#if defined(TABLE_ARM) && defined(__aarch64__)
    conf_check(&t, info.profile == AAR_PROFILE_A64, "aar_info.profile is not AAR_PROFILE_A64");
    conf_end(&t);
#elif defined(TABLE_ARM)
    conf_check(&t, info.profile == AAR_PROFILE_A32, "aar_info.profile is not AAR_PROFILE_A32");
    conf_end(&t);
#else
    conf_check(&t, info.profile == AAR_PROFILE_A32 || info.profile == AAR_PROFILE_A64,
               "aar_info.profile is neither AAR_PROFILE_A32 nor AAR_PROFILE_A64");
    if (!t.failed)
        conf_skip(t.id, "host: the profile the app was built for is not an Arm profile");
    else
        conf_end(&t);
#endif
}

static void test_info_granules(void)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.14");
    need_info(&t, &info);
    conf_check(&t, info.heap_granule == AAR_HEAP_GRANULE, "heap_granule is not 1 MiB");
    conf_check(&t, is_pow2(info.shared_granule) && info.shared_granule >= 64u,
               "shared_granule is not a power of two of at least 64");
    conf_buf_str(&t.note, "shared granule ");
    conf_buf_u64(&t.note, info.shared_granule);
    conf_end(&t);
}

static void test_info_msg_capacity(const struct cli *c)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.15");
    need_info(&t, &info);
    conf_check(&t, info.msg_capacity >= AAR_MIN_MSG_CAPACITY, "msg_capacity is below 32");
    if (c->have_msg_capacity && info.msg_capacity != c->msg_capacity && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "msg_capacity ");
        conf_buf_u64(&t.why, info.msg_capacity);
        conf_buf_str(&t.why, " differs from the client's ");
        conf_buf_u64(&t.why, c->msg_capacity);
    }
    conf_buf_str(&t.note, "msg_capacity ");
    conf_buf_u64(&t.note, info.msg_capacity);
    end_or_skip(&t, !c->have_msg_capacity);
}

static void test_info_stall(const struct cli *c)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.16");
    need_info(&t, &info);
    conf_check(&t, info.stall_limit_ms >= AAR_STALL_LIMIT_MIN_MS, "stall_limit_ms is below 10000");
    if (c->have_stall && info.stall_limit_ms != c->stall_limit_ms && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "stall_limit_ms ");
        conf_buf_u64(&t.why, info.stall_limit_ms);
        conf_buf_str(&t.why, " differs from the client's ");
        conf_buf_u64(&t.why, c->stall_limit_ms);
    }
    if (c->have_grace && info.stop_grace_ms != c->stop_grace_ms && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "stop_grace_ms ");
        conf_buf_u64(&t.why, info.stop_grace_ms);
        conf_buf_str(&t.why, " differs from the client's ");
        conf_buf_u64(&t.why, c->stop_grace_ms);
    }
    conf_buf_str(&t.note, "stall ");
    conf_buf_u64(&t.note, info.stall_limit_ms);
    conf_buf_str(&t.note, " ms, grace ");
    conf_buf_u64(&t.note, info.stop_grace_ms);
    conf_buf_str(&t.note, " ms");
    end_or_skip(&t, !c->have_stall || !c->have_grace);
}

static void test_info_files(void)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.17");
    need_info(&t, &info);
    conf_check(&t, info.max_files >= AAR_MIN_FILES, "max_files is below 16");
    conf_buf_str(&t.note, "max_files ");
    conf_buf_u64(&t.note, info.max_files);
    conf_end(&t);
}

static void test_info_shared(const struct cli *c)
{
    struct conf_test t;
    aar_info info;

    conf_begin(&t, "C-TABLE.18");
    need_info(&t, &info);
    if (info.shared_limit != (uint64_t)AAR_SHARED_DEFAULT_MIB * MIB && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "shared_limit is ");
        conf_buf_u64(&t.why, info.shared_limit);
        conf_buf_str(&t.why, " bytes, not the default of 16 MiB");
    }
    if (c->have_shared_default && info.shared_limit != (uint64_t)c->shared_default_mib * MIB &&
        conf_fail_first(&t)) {
        conf_buf_str(&t.why, "shared_limit is not the client's shared_default_mib of ");
        conf_buf_u64(&t.why, c->shared_default_mib);
        conf_buf_str(&t.why, " MiB");
    }
    conf_check(&t, info.shared_used <= info.shared_limit, "shared_used exceeds shared_limit");
    conf_buf_str(&t.note, "limit ");
    conf_buf_u64(&t.note, info.shared_limit / MIB);
    conf_buf_str(&t.note, " MiB, used ");
    conf_buf_u64(&t.note, info.shared_used);
    conf_buf_str(&t.note, " bytes");
    conf_end(&t);
}

static void test_heap_info(void)
{
    struct conf_test t;
    aar_heap_info h1, h2;
    int rc1, rc2;
    const uint64_t p1 = 0x1111111111111111ull;
    const uint64_t p2 = 0x2222222222222222ull;

    conf_begin(&t, "C-TABLE.19");
    h1.committed = h1.limit = h1.pool_free = p1;
    h2.committed = h2.limit = h2.pool_free = p2;
    rc1 = aar_get_heap_info(&h1);
    rc2 = aar_get_heap_info(&h2);
    if (!conf_check(&t, rc1 == AAR_OK && rc2 == AAR_OK, "aar_get_heap_info failed")) {
        conf_end(&t);
        return;
    }
    /* A field the runtime wrote is not the pattern in at least one of the two. */
    conf_check(&t, !(h1.committed == p1 && h2.committed == p2), "committed was not filled");
    conf_check(&t, !(h1.limit == p1 && h2.limit == p2), "limit was not filled");
    conf_check(&t, !(h1.pool_free == p1 && h2.pool_free == p2), "pool_free was not filled");
    conf_check(&t, h1.committed <= h1.limit, "committed exceeds limit");
    conf_buf_str(&t.note, "committed ");
    conf_buf_u64(&t.note, h1.committed);
    conf_buf_str(&t.note, ", limit ");
    conf_buf_u64(&t.note, h1.limit);
    conf_buf_str(&t.note, ", pool_free ");
    conf_buf_u64(&t.note, h1.pool_free);
    conf_end(&t);
}

/* ---- C-TABLE.20 and C-TABLE.21: CPU features are honest ------------------------------------------------ */

#if defined(TABLE_ARM) && defined(__arm__)

static int probe_vfpv4(void)
{
    float acc = 1.0f;
    float a = 2.0f;
    float b = 3.0f;

    __asm__ volatile(".cpu cortex-a15\n\t.fpu neon-vfpv4\n\t"
                     "vfma.f32 %0, %1, %2\n\t"
                     ".cpu cortex-a9\n\t.fpu neon\n"
                     : "+t"(acc)
                     : "t"(a), "t"(b));
    return acc == 7.0f;
}

static int probe_idiv(void)
{
    volatile int32_t n = -42;
    volatile int32_t d = 6;
    volatile uint32_t un = 100u;
    volatile uint32_t ud = 7u;
    int32_t q;
    uint32_t uq;

    __asm__ volatile(".cpu cortex-a15\n\t"
                     "sdiv %0, %2, %3\n\t"
                     "udiv %1, %4, %5\n\t"
                     ".cpu cortex-a9\n\t.fpu neon\n"
                     : "=&r"(q), "=&r"(uq)
                     : "r"(n), "r"(d), "r"(un), "r"(ud));
    return q == -7 && uq == 14u;
}

static int probe_fp16(void)
{
    uint32_t half = 0x3C00u; /* 1.0 as a half-precision value */
    uint32_t out;

    __asm__ volatile(".cpu cortex-a15\n\t.fpu neon-vfpv4\n\t"
                     "vmov s0, %1\n\t"
                     "vcvtb.f32.f16 s0, s0\n\t"
                     "vmov %0, s0\n\t"
                     ".cpu cortex-a9\n\t.fpu neon\n"
                     : "=r"(out)
                     : "r"(half)
                     : "s0");
    return out == 0x3F800000u; /* 1.0f */
}

static void test_cpu_features(void)
{
    struct conf_test t;
    aar_info info;
    uint32_t checked = 0;

    conf_begin(&t, "C-TABLE.20");
    need_info(&t, &info);
    if (info.cpu_features & AAR_CPU_A32_VFPV4) {
        conf_check(&t, probe_vfpv4(), "AAR_CPU_A32_VFPV4 is set but a fused multiply-add gave the wrong result");
        checked++;
    }
    if (info.cpu_features & AAR_CPU_A32_IDIV) {
        conf_check(&t, probe_idiv(), "AAR_CPU_A32_IDIV is set but sdiv and udiv gave the wrong result");
        checked++;
    }
    if (info.cpu_features & AAR_CPU_A32_FP16) {
        conf_check(&t, probe_fp16(), "AAR_CPU_A32_FP16 is set but a half-precision convert gave the wrong result");
        checked++;
    }
    conf_buf_str(&t.note, "cpu_features ");
    conf_buf_hex(&t.note, info.cpu_features);
    conf_buf_str(&t.note, ", ");
    conf_buf_u64(&t.note, checked);
    conf_buf_str(&t.note, " set bits executed");
    conf_end(&t);
}

#elif defined(TABLE_ARM) && defined(__aarch64__)

#define A64_ARCH ".arch armv8.2-a+crc+lse+crypto+dotprod+fp16\n\t"
#define A64_BACK "\n\t.arch armv8-a\n"

static int probe_crc32(void)
{
    uint32_t seed = 0u;
    uint32_t byte = 1u;
    uint32_t out;

    __asm__ volatile(A64_ARCH "crc32b %w0, %w1, %w2" A64_BACK : "=r"(out) : "r"(seed), "r"(byte));
    return out == 0x77073096u;
}

static int probe_lse(void)
{
    uint32_t cell = 5u;
    uint32_t add = 3u;
    uint32_t old;

    __asm__ volatile(A64_ARCH "ldadd %w2, %w1, [%3]" A64_BACK
                     : "+m"(cell), "=&r"(old)
                     : "r"(add), "r"(&cell)
                     : "memory");
    return old == 5u && cell == 8u;
}

static int probe_aes(void)
{
    uint8x16_t state = vdupq_n_u8(0);
    uint8x16_t key = vdupq_n_u8(0);
    uint8_t out[16];
    uint32_t i;

    __asm__ volatile(A64_ARCH "aese %0.16b, %1.16b" A64_BACK : "+w"(state) : "w"(key));
    vst1q_u8(out, state);
    for (i = 0; i < 16u; i++) {
        if (out[i] != 0x63u) /* SubBytes of 0 */
            return 0;
    }
    return 1;
}

static int probe_sha(void)
{
    uint32x4_t in = vdupq_n_u32(1u);
    uint32x4_t out = vdupq_n_u32(0u);

    __asm__ volatile(A64_ARCH "sha1h %s0, %s1" A64_BACK : "=w"(out) : "w"(in));
    return vgetq_lane_u32(out, 0) == 0x40000000u; /* 1 rotated left by 30 */
}

static int probe_dotprod(void)
{
    int32x4_t acc = vdupq_n_s32(0);
    int8x16_t a = vdupq_n_s8(1);
    int8x16_t b = vdupq_n_s8(2);

    __asm__ volatile(A64_ARCH "sdot %0.4s, %1.16b, %2.16b" A64_BACK : "+w"(acc) : "w"(a), "w"(b));
    return vgetq_lane_s32(acc, 0) == 8 && vgetq_lane_s32(acc, 3) == 8;
}

static int probe_fp16(void)
{
    uint32_t one = 0x3C00u; /* 1.0 */
    uint32_t two = 0x4000u; /* 2.0 */
    uint32_t out;

    __asm__ volatile(A64_ARCH "fmov s1, %w1\n\tfmov s2, %w2\n\tfadd h0, h1, h2\n\tfmov %w0, s0" A64_BACK
                     : "=r"(out)
                     : "r"(one), "r"(two)
                     : "v0", "v1", "v2");
    return (out & 0xFFFFu) == 0x4200u; /* 3.0 */
}

static void test_cpu_features(void)
{
    struct conf_test t;
    aar_info info;
    uint32_t checked = 0;

    conf_begin(&t, "C-TABLE.21");
    need_info(&t, &info);
    if (info.cpu_features & AAR_CPU_A64_CRC32) {
        conf_check(&t, probe_crc32(), "AAR_CPU_A64_CRC32 is set but crc32b gave the wrong result");
        checked++;
    }
    if (info.cpu_features & AAR_CPU_A64_LSE) {
        conf_check(&t, probe_lse(), "AAR_CPU_A64_LSE is set but ldadd gave the wrong result");
        checked++;
    }
    if (info.cpu_features & AAR_CPU_A64_AES) {
        conf_check(&t, probe_aes(), "AAR_CPU_A64_AES is set but aese gave the wrong result");
        checked++;
    }
    if (info.cpu_features & AAR_CPU_A64_SHA) {
        conf_check(&t, probe_sha(), "AAR_CPU_A64_SHA is set but sha1h gave the wrong result");
        checked++;
    }
    if (info.cpu_features & AAR_CPU_A64_DOTPROD) {
        conf_check(&t, probe_dotprod(), "AAR_CPU_A64_DOTPROD is set but sdot gave the wrong result");
        checked++;
    }
    if (info.cpu_features & AAR_CPU_A64_FP16) {
        conf_check(&t, probe_fp16(), "AAR_CPU_A64_FP16 is set but fadd on halves gave the wrong result");
        checked++;
    }
    conf_buf_str(&t.note, "cpu_features ");
    conf_buf_hex(&t.note, info.cpu_features);
    conf_buf_str(&t.note, ", ");
    conf_buf_u64(&t.note, checked);
    conf_buf_str(&t.note, " set bits executed");
    conf_end(&t);
}

#endif

/* ---- C-TABLE.22 and C-TABLE.23: aar_ext ------------------------------------------------------------------- */

static void test_ext_unknown(const aar_api *api)
{
    struct conf_test t;

    conf_begin(&t, "C-TABLE.22");
    conf_check(&t, aar_ext(0x7FFFFFF0u, 1u) == 0, "aar_ext of an unknown platform id is not NULL");
    conf_check(&t, aar_ext(api->platform_id, 0x7FFFFFF0u) == 0,
               "aar_ext of an unknown extension id on this platform is not NULL");
    conf_check(&t, aar_ext(AAR_PLATFORM_ZZ9000, 0x7FFFFFF0u) == 0,
               "aar_ext of an unknown ZZ9000 extension id is not NULL");
    conf_end(&t);
}

static void test_ext_zz9000(uint32_t declared)
{
    struct conf_test t;
    const aar_ext_zz9000 *ext;

    conf_begin(&t, "C-TABLE.23");
    ext = (const aar_ext_zz9000 *)aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN);
    if (!conf_check(&t, ext != 0, "aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN) is NULL")) {
        conf_end(&t);
        return;
    }
    conf_check(&t, AAR_EXT_ZZ9000_COVERS(ext, build_id), "the extension table size does not cover build_id");
    if (!(declared & AAR_GROUP_GL) && AAR_EXT_ZZ9000_COVERS(ext, gl_bind_flags))
        conf_check(&t, ext->gl_bind_flags == 0, "gl_bind_flags is not NULL although the GL group is absent");
    conf_buf_str(&t.note, "extension size ");
    conf_buf_u64(&t.note, ext->size);
    conf_end(&t);
}

/* ---- C-TABLE.24: error codes ---------------------------------------------------------------------------------- */

static uint32_t codes_seen;
static int code_bad_call = -1;
static int code_bad_rc;

/* A failing call returns a code of section 3.5, AAR_E_IO (-1) to AAR_E_OWNER (-17). */
static void note_code(int call, int rc)
{
    if (rc >= 0)
        return; /* the call did not fail: nothing to judge */
    codes_seen++;
    if ((rc < AAR_E_OWNER || rc > AAR_E_IO) && code_bad_call < 0) {
        code_bad_call = call;
        code_bad_rc = rc;
    }
}

static void test_error_codes(uint32_t declared)
{
    struct conf_test t;
    uint8_t buf[64];
    aar_stat st;
    aar_dirent de;
    aar_shared sh;
    aar_surface surf;
    aar_audio audio;
    uint32_t n = 0;
    int64_t pos = 0;
    void *p = 0;
    int call = 0;
    int rc;

    conf_begin(&t, "C-TABLE.24");
#define FAILING(expr) do { rc = (expr); note_code(call++, rc); } while (0)
    FAILING(aar_file_open("conf_table_missing/none.bin", AAR_FILE_READ));
    FAILING(aar_file_open("conf_table_missing.bin", 0u));
    FAILING(aar_file_open("conf_table_missing.bin", 0x80u));
    FAILING(aar_file_close(-1));
    FAILING(aar_file_close(0x7FFF));
    FAILING(aar_file_read(-1, buf, 1u, &n));
    FAILING(aar_file_write(-1, buf, 1u, &n));
    FAILING(aar_file_seek(-1, 0, AAR_SEEK_SET, &pos));
    FAILING(aar_file_stat("conf_table_missing.bin", &st));
    FAILING(aar_file_list_open("conf_table_missing_dir"));
    FAILING(aar_file_list_next(-1, &de));
    FAILING(aar_file_list_close(-1));
    FAILING(aar_heap_grow(1u, &p));
    FAILING(aar_heap_shrink(1u, &p));
    FAILING(aar_shared_alloc(0u, 0u, 0u, &sh));
    FAILING(aar_shared_alloc(4096u, 0u, 1u, &sh));
    FAILING(aar_shared_free(buf));
    if (declared & AAR_GROUP_DISPLAY) {
        FAILING(aar_display_open(0u, 0u, AAR_FMT_RGB565, 0u, &surf));
        FAILING(aar_display_close(0xFFFFu));
    }
    if (declared & AAR_GROUP_AUDIO) {
        rc = aar_audio_open(1234u, 7u, &audio);
        if (rc == AAR_OK)
            (void)aar_audio_close();
        else
            note_code(call, rc);
        call++;
    }
#undef FAILING
    if (code_bad_call >= 0 && conf_fail_first(&t)) {
        conf_buf_str(&t.why, "failing call ");
        conf_buf_u64(&t.why, (uint64_t)code_bad_call);
        conf_buf_str(&t.why, " returned ");
        conf_buf_i64(&t.why, code_bad_rc);
        conf_buf_str(&t.why, ", outside -1 to -17");
    }
    conf_check(&t, codes_seen > 0, "no call of the battery failed: nothing was judged");
    conf_buf_u64(&t.note, codes_seen);
    conf_buf_str(&t.note, " failing calls, every code in -1 to -17");
    conf_end(&t);
}

/* ---- main ---------------------------------------------------------------------------------------------------------- */

int aar_main(int argc, char **argv)
{
    const aar_api *api = aar_api_ptr;
    struct cli c;
    uint32_t declared;
    int zz9000;

    read_cli(&c, argc, argv);
    if (!api) {
        conf_fail("C-TABLE.1", "aar_api_ptr is NULL");
        return conf_finish("C-TABLE");
    }
    declared = api->groups | (c.have_groups ? c.groups : 0u);
    zz9000 = api->platform_id == AAR_PLATFORM_ZZ9000 ||
             (c.have_platform && c.platform_id == AAR_PLATFORM_ZZ9000);

    test_core_slots(api);
    test_version(api, &c);
    test_groups_word(api, &c);
    test_group_bits(api);
    test_group_slots(api, declared);
    test_clear_groups(api, declared);
    test_optional_slots(api);
    test_platform_id(api, &c);
    test_info_small();
    test_info_large();
    test_info_profile(&c);
    test_info_granules();
    test_info_msg_capacity(&c);
    test_info_stall(&c);
    test_info_files();
    test_info_shared(&c);
    test_heap_info();
#if defined(TABLE_ARM)
    test_cpu_features();
#endif
    test_ext_unknown(api);
    if (zz9000)
        test_ext_zz9000(declared);
    test_error_codes(declared);
    return conf_finish("C-TABLE");
}
