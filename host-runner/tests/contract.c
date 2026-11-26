/*
 * Host-runner contracts that the templates do not cover: the table and its
 * groups, aar_get_info, path rejection, colon paths, message queue
 * backpressure, services, shared buffers and their tokens, display layout,
 * async flip and latest mode, and the audio ring. Exit status is the failing
 * check.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/ext_zz9000.h"
#include "aar/transport.h"
#include "aar_host.h"

#include <stdio.h>
#include <string.h>

static int fail(int code, const char *msg)
{
    fprintf(stderr, "contract: %s\n", msg);
    return code;
}

static int read_all(int fd, char *buf, uint32_t n)
{
    uint32_t done = 0;

    while (done < n) {
        uint32_t got = 0;
        int rc = aar_file_read(fd, buf + done, n - done, &got);
        if (rc != AAR_OK || got == 0u)
            return rc != AAR_OK ? rc : AAR_E_IO;
        done += got;
    }
    return AAR_OK;
}

static int test_table(int no_gl)
{
    const aar_api *api = aar_api_ptr;
    aar_info info;
    unsigned char big[sizeof(aar_info) + 16];
    unsigned char small[sizeof(aar_info)];
    uint32_t i;

    if (!api || api->size != sizeof(aar_api))
        return fail(110, "table size");
    if (!AAR_API_AT_LEAST(api, AAR_API_MINOR))
        return fail(111, "table version");
    if ((api->groups & ~AAR_GROUP_GL) != (AAR_GROUP_DISPLAY | AAR_GROUP_INPUT |
                                          AAR_GROUP_AUDIO |
                                          AAR_GROUP_SERVICES) ||
        api->platform_id != AAR_PLATFORM_NONE)
        return fail(112, "table groups or platform");
    if (no_gl && (api->groups & AAR_GROUP_GL))
        return fail(118, "--no-gl left the GL group set");
    if (!api->exit || !api->yield || !api->should_stop || !api->time_us ||
        !api->sleep_us || !api->info || !api->heap_range || !api->heap_grow ||
        !api->heap_shrink || !api->heap_info || !api->shared_alloc ||
        !api->shared_free || !api->publish || !api->acquire || !api->log ||
        !api->file_open || !api->file_close || !api->file_read ||
        !api->file_write || !api->file_seek || !api->file_stat ||
        !api->file_list_open || !api->file_list_next ||
        !api->file_list_close || !api->console_write || !api->console_read ||
        !api->event_poll || !api->msg_send || !api->msg_poll || !api->msg_wait)
        return fail(113, "a core slot is NULL");
    if (!api->display_open || !api->display_flip || !api->display_flip_async ||
        !api->display_flip_idle || !api->display_refresh ||
        !api->display_vsync || !api->display_set_palette ||
        !api->display_close || !api->display_latest_dropped)
        return fail(114, "a display slot is NULL");
    if (!api->audio_open || !api->audio_close || !api->audio_stats)
        return fail(115, "an audio slot is NULL");
    if (!api->sha256 || !api->decompress || !api->image_open ||
        !api->image_decode || !api->image_close || !api->audio_decode_open ||
        !api->audio_decode || !api->audio_decode_close)
        return fail(116, "a services slot is NULL");
    if ((api->groups & AAR_GROUP_GL) != 0u) {
        if (!api->gl_bind || !api->gl_swap || !api->gl_unbind)
            return fail(117, "a GL slot is NULL with the GL group");
    } else if (api->gl_bind || api->gl_swap || api->gl_unbind) {
        return fail(117, "a GL slot is set without the GL group");
    }

    memset(&info, 0xaa, sizeof info);
    info.size = (uint32_t)sizeof info;
    if (aar_get_info(&info) != AAR_OK)
        return fail(120, "info");
    if (info.size != sizeof info ||
        info.profile != (sizeof(void *) == 8u ? AAR_PROFILE_A64 : AAR_PROFILE_A32) ||
        info.platform_id != AAR_PLATFORM_NONE || info.groups != api->groups ||
        (info.flags & AAR_INFOF_ISOLATED) != 0u ||
        (info.flags & AAR_INFOF_SHARED_COHERENT) == 0u)
        return fail(121, "info identity");
    if (info.heap_granule != AAR_HEAP_GRANULE ||
        info.shared_granule < 64u ||
        (info.shared_granule & (info.shared_granule - 1u)) != 0u ||
        info.msg_capacity != AAR_MSG_CAPACITY ||
        info.stall_limit_ms < AAR_STALL_LIMIT_MIN_MS ||
        info.max_files < AAR_MIN_FILES ||
        info.shared_limit != ((uint64_t)AAR_SHARED_DEFAULT_MIB << 20) ||
        info.shared_used != 0u || info.cpu_features != 0u)
        return fail(122, "info limits");
    for (i = 0; i < 5u; i++)
        if (info.reserved[i] != 0u)
            return fail(123, "info reserved");

    /* A short size fills only that much; a long size is zero past the end. */
    memset(small, 0xaa, sizeof small);
    ((aar_info *)(void *)small)->size = 8u;
    if (aar_get_info((aar_info *)(void *)small) != AAR_OK)
        return fail(124, "short info");
    for (i = 8u; i < sizeof small; i++)
        if (small[i] != 0xaau)
            return fail(125, "info wrote past the caller's size");
    memset(big, 0xaa, sizeof big);
    ((aar_info *)(void *)big)->size = (uint32_t)sizeof big;
    if (aar_get_info((aar_info *)(void *)big) != AAR_OK)
        return fail(126, "long info");
    for (i = sizeof info; i < sizeof big; i++)
        if (big[i] != 0u)
            return fail(127, "info did not zero past what it knows");
    if (aar_get_info(0) != AAR_E_INVAL)
        return fail(128, "NULL info");
    if (aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN) != 0)
        return fail(129, "the host has no ZZ9000 extension");
    return 0;
}

static int test_reject(void)
{
    int fd;
    char buf[8];
    uint32_t n = 0;
    const void *mapped = 0;
    uint64_t bytes = 0;
    aar_info info;
    aar_dirent ent;
    aar_stat st;
    int list;
    int saw_ok = 0;
    int saw_dir = 0;
    int saw_dot = 0;

    if (aar_file_open("../outside", AAR_FILE_READ) != AAR_E_ACCES)
        return fail(2, "parent escape was not rejected");
    if (aar_file_open("foo/../../outside", AAR_FILE_READ) != AAR_E_ACCES)
        return fail(3, "nested parent escape was not rejected");
    if (aar_file_open("vol:data", AAR_FILE_READ) != AAR_E_ACCES)
        return fail(4, "colon path was not rejected");
    if (aar_file_open("missing-no-such", AAR_FILE_READ) != AAR_E_NOENT)
        return fail(5, "missing file was not ENOENT");
    if (aar_file_open("ok.txt", 8u) != AAR_E_INVAL)
        return fail(22, "unknown open flag was not EINVAL");
    if (aar_file_open("", AAR_FILE_READ) != AAR_E_INVAL ||
        aar_file_open(0, AAR_FILE_READ) != AAR_E_INVAL)
        return fail(23, "empty or NULL path was not EINVAL");
    fd = aar_file_open("ok.txt", AAR_FILE_READ);
    if (fd < AAR_FD_BASE)
        return fail(6, "ok.txt did not open");
    if (read_all(fd, buf, 3u) != AAR_OK || memcmp(buf, "ok\n", 3) != 0)
        return fail(7, "ok.txt bytes");
    if (aar_file_close(fd) != AAR_OK || aar_file_close(fd) != AAR_E_BADF)
        return fail(24, "close, then close again");
    /* '//' is a parent step, so foo//bar.txt is root/bar.txt. */
    fd = aar_file_open("foo//bar.txt", AAR_FILE_READ);
    if (fd < 0)
        return fail(8, "double-slash parent step failed");
    if (read_all(fd, buf, 4u) != AAR_OK || memcmp(buf, "bar\n", 4) != 0)
        return fail(9, "double-slash resolved to the wrong file");
    aar_file_close(fd);
    if (aar_file_stat("ok.txt", &st) != AAR_OK || st.size != 3u ||
        st.mode != AAR_MODE_FILE || st.mtime <= 0)
        return fail(25, "stat of a file");
    if (aar_file_stat("foo", &st) != AAR_OK || st.mode != AAR_MODE_DIR)
        return fail(26, "stat of a directory");
    if (aar_file_stat("ok.txt", 0) != AAR_E_INVAL)
        return fail(27, "stat into NULL");
    if (aar_map_file("empty.txt", &mapped, &bytes) != AAR_E_NOMEM)
        return fail(10, "empty map_file was not ENOMEM");
    if (aar_map_file("big.bin", &mapped, &bytes) != AAR_E_NOSPC)
        return fail(11, "oversize map_file was not ENOSPC");
    if (aar_map_file("ok.txt", &mapped, &bytes) != AAR_OK || bytes != 3u ||
        memcmp(mapped, "ok\n", 3) != 0)
        return fail(12, "map_file bytes");
    info.size = (uint32_t)sizeof info;
    if (aar_get_info(&info) != AAR_OK || info.shared_used == 0u)
        return fail(28, "a mapped file is not charged to the shared limit");
    aar_unmap_file(mapped);
    if (aar_get_info(&info) != AAR_OK || info.shared_used != 0u)
        return fail(29, "unmap did not release the shared charge");
    list = aar_file_list_open(".");
    if (list < AAR_FD_BASE)
        return fail(13, "list_open");
    for (;;) {
        int rc = aar_file_list_next(list, &ent);
        if (rc == AAR_E_NOENT)
            break;
        if (rc != AAR_OK)
            return fail(14, "list_next");
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0)
            saw_dot = 1;
        if (strcmp(ent.name, "ok.txt") == 0)
            saw_ok = 1;
        if (strcmp(ent.name, "foo") == 0 && ent.mode == AAR_MODE_DIR)
            saw_dir = 1;
    }
    aar_file_list_close(list);
    if (saw_dot)
        return fail(15, "list returned . or ..");
    if (!saw_ok)
        return fail(16, "list missed ok.txt");
    if (!saw_dir)
        return fail(21, "list did not report foo as a directory");
    fd = aar_file_open("round.bin",
                       AAR_FILE_READ | AAR_FILE_WRITE | AAR_FILE_CREATE |
                       AAR_FILE_TRUNCATE);
    if (fd < 0)
        return fail(17, "create");
    if (aar_file_write(fd, "abcd", 4u, &n) != AAR_OK || n != 4u)
        return fail(18, "write");
    {
        int64_t pos = -1;
        if (aar_file_seek(fd, 1, AAR_SEEK_SET, &pos) != AAR_OK || pos != 1)
            return fail(19, "seek");
        if (aar_file_seek(fd, 0, AAR_SEEK_END, 0) != AAR_OK)
            return fail(30, "seek with a NULL out_pos");
        if (aar_file_seek(fd, 1, 7, &pos) != AAR_E_INVAL)
            return fail(31, "bad whence");
        if (aar_file_seek(fd, 1, AAR_SEEK_SET, 0) != AAR_OK)
            return fail(32, "seek back");
    }
    if (read_all(fd, buf, 3u) != AAR_OK || memcmp(buf, "bcd", 3) != 0)
        return fail(20, "read after seek");
    aar_file_close(fd);
    /* An id remembers how it was opened (spec 7.1). */
    fd = aar_file_open("round.bin", AAR_FILE_READ);
    if (fd < 0)
        return fail(192, "open read-only");
    if (aar_file_write(fd, "x", 1u, &n) != AAR_E_BADF)
        return fail(192, "write through a read-only id");
    aar_file_close(fd);
    fd = aar_file_open("round.bin", AAR_FILE_WRITE);
    if (fd < 0)
        return fail(193, "open write-only");
    if (aar_file_read(fd, buf, 1u, &n) != AAR_E_BADF)
        return fail(193, "read through a write-only id");
    aar_file_close(fd);
    (void)n;
    return 0;
}

static int test_abs(void)
{
    int fd;
    char buf[8];

    fd = aar_file_open("vol:data", AAR_FILE_READ);
    if (fd < 0)
        return fail(40, "abs colon path did not open");
    if (read_all(fd, buf, 6u) != AAR_OK || memcmp(buf, "abs-ok", 6) != 0)
        return fail(41, "abs colon bytes");
    aar_file_close(fd);
    return 0;
}

static int test_input(void)
{
    aar_event ev;
    uint32_t n = 0;
    int saw_key = 0;
    int saw_break = 0;
    int saw_lowmem = 0;
    int i;

    if (aar_event_poll(&ev, 1u, 0) != AAR_E_INVAL ||
        aar_event_poll(0, 1u, &n) != AAR_E_INVAL)
        return fail(50, "poll with NULL arguments");
    for (i = 0; i < 8; i++) {
        if (aar_event_poll(&ev, 1u, &n) != AAR_OK)
            return fail(51, "poll");
        if (n == 0u)
            break;
        if (ev.type == AAR_EV_KEY_DOWN && ev.code == 0x22u && ev.buttons == 8u)
            saw_key = 1;
        if (ev.type == AAR_EV_BREAK && ev.code == AAR_BREAK_CTRL_D)
            saw_break = 1;
        if (ev.type == AAR_EV_LOW_MEMORY && ev.code == 0u && ev.dx == 0 &&
            ev.dy == 0 && ev.buttons == 0u)
            saw_lowmem = 1;
    }
    if (!saw_key || !saw_break)
        return fail(52, "scripted key or break missing");
    if (!saw_lowmem)
        return fail(53, "scripted low-memory event missing");
    return 0;
}

static int test_display(void)
{
    aar_surface s;
    aar_surface t;
    uint32_t rgb = 0x00ff0000u;
    int idle;

    if (aar_display_flip_idle() != AAR_E_INVAL ||
        aar_display_vsync() != AAR_E_INVAL)
        return fail(60, "calls without a display");
    if (aar_display_open(4u, 2u, 99u, AAR_DISPLAY_VSYNC, &s) != AAR_E_INVAL ||
        aar_display_open(0u, 2u, AAR_FMT_RGB565, 0, &s) != AAR_E_INVAL ||
        aar_display_open(AAR_DISPLAY_MAX_DIM + 1u, 2u, AAR_FMT_RGB565, 0, &s) !=
            AAR_E_INVAL ||
        aar_display_open(4u, 2u, AAR_FMT_RGB565, 4u, &s) != AAR_E_INVAL ||
        aar_display_open(4u, 2u, AAR_FMT_RGB565, 0, 0) != AAR_E_INVAL)
        return fail(61, "bad open arguments");

    if (aar_display_open(4u, 2u, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, &s) != AAR_OK)
        return fail(62, "rgb565 open");
    if (s.layout != AAR_LAYOUT_RGB565_BE || !s.back || s.back == s.front ||
        s.format != AAR_FMT_RGB565 || s.pitch < 8u)
        return fail(63, "rgb565 layout or buffers");
    if ((s.caps & AAR_DISPCAP_ASYNC) == 0u || (s.caps & AAR_DISPCAP_LATEST) != 0u)
        return fail(64, "plain open caps");
    if (aar_display_open(4u, 2u, AAR_FMT_RGB565, 0, &t) != AAR_E_BUSY)
        return fail(65, "second open was not EBUSY");
    if (aar_display_latest_dropped() != 0u)
        return fail(66, "dropped count without latest");
    if (aar_display_flip_async(s.id) != AAR_OK)
        return fail(67, "async flip");
    idle = aar_display_flip_idle();
    if (idle != 0)
        return fail(68, "async flip was already idle");
    if (aar_display_flip(s.id) != AAR_E_BUSY ||
        aar_display_flip_async(s.id) != AAR_E_BUSY ||
        aar_display_refresh(&s) != AAR_E_BUSY)
        return fail(69, "a pending flip was not EBUSY");
    aar_yield();
    if (aar_display_flip_idle() != 1)
        return fail(70, "async flip did not complete on yield");
    if (aar_display_refresh(&s) != AAR_OK)
        return fail(71, "refresh after the flip");
    if (aar_display_flip(s.id + 1u) != AAR_E_INVAL ||
        aar_display_close(s.id + 1u) != AAR_E_INVAL)
        return fail(72, "bad surface id");
    if (aar_display_close(s.id) != AAR_OK)
        return fail(73, "close");

    memset(&s, 0, sizeof s);
    if (aar_display_open(4u, 2u, AAR_FMT_RGB565,
                         AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST, &s) != AAR_OK)
        return fail(74, "latest open");
    if ((s.caps & AAR_DISPCAP_LATEST) == 0u)
        return fail(75, "latest was not granted");
    if (aar_display_flip_async(s.id) != AAR_OK ||
        aar_display_flip_idle() != 1)
        return fail(76, "latest idle");
    if (aar_display_latest_dropped() != 0u)
        return fail(77, "dropped before a replacement");
    if (aar_display_flip_async(s.id) != AAR_OK)
        return fail(78, "second latest flip");
    if (aar_display_latest_dropped() != 1u)
        return fail(79, "dropped count");
    aar_display_close(s.id);

    if (aar_display_open(4u, 2u, AAR_FMT_CLUT8, 0, &s) != AAR_OK)
        return fail(80, "clut8 open");
    if (s.layout != AAR_LAYOUT_CLUT8 || (s.caps & AAR_DISPCAP_CLUT8) == 0u)
        return fail(81, "clut8 layout or caps");
    if (aar_display_set_palette(s.id, 0u, 1u, &rgb) != AAR_OK)
        return fail(82, "palette");
    if (aar_display_set_palette(s.id, 0u, 1u, 0) != AAR_E_INVAL ||
        aar_display_set_palette(s.id, 255u, 2u, &rgb) != AAR_E_INVAL)
        return fail(83, "bad palette");
    aar_display_close(s.id);
    if (aar_display_open(2u, 2u, AAR_FMT_BGRA8888, 0, &s) != AAR_OK)
        return fail(84, "bgra open");
    if (s.layout != AAR_LAYOUT_BGRA8888)
        return fail(85, "bgra layout");
    if (aar_display_set_palette(s.id, 0u, 1u, &rgb) != AAR_E_INVAL)
        return fail(86, "palette on a non-CLUT8 surface");
    aar_display_close(s.id);
    return 0;
}

static int test_services(void)
{
    static const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t expect[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    static const uint8_t empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
        0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
        0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    uint8_t digest[32];
    uint8_t msg[AAR_MSG_BYTES];
    uint32_t i;
    int rc;

    if (aar_sha256(abc, 3u, digest) != AAR_OK || memcmp(digest, expect, 32) != 0)
        return fail(90, "sha256");
    if (aar_sha256(0, 0u, digest) != AAR_OK || memcmp(digest, empty, 32) != 0)
        return fail(91, "sha256 of nothing");
    if (aar_sha256(abc, 3u, 0) != AAR_E_INVAL)
        return fail(92, "sha256 into NULL");
    if (aar_decompress(AAR_COMPRESS_ZLIB, 0, abc, 3u, digest, 32u, 0) !=
        AAR_E_NOSYS)
        return fail(93, "decompress");
    if (aar_image_open(0, 0, 0) != AAR_E_NOSYS)
        return fail(94, "image");
    if (aar_audio_decode_open(AAR_AUDIO_MP3, 0) != AAR_E_NOSYS)
        return fail(95, "audio decode");
    if (aar_gl_bind(1u) != 0)
        return fail(96, "gl bind");
    memset(msg, 0x5a, sizeof msg);
    for (i = 0; i < AAR_MSG_CAPACITY; i++) {
        rc = aar_msg_send(msg);
        if (rc != AAR_OK)
            return fail(97, "msg send before full");
    }
    if (aar_msg_send(msg) != AAR_E_BUSY)
        return fail(98, "msg queue did not report EBUSY at AAR_MSG_CAPACITY");
    if (aar_msg_send(0) != AAR_E_INVAL || aar_msg_poll(0) != AAR_E_INVAL)
        return fail(99, "msg NULL arguments");
    if (aar_msg_poll(msg) != 1)
        return fail(100, "msg poll");
    if (aar_msg_send(msg) != AAR_OK)
        return fail(101, "msg send after poll");
    return 0;
}

static int test_memory(void)
{
    aar_shared a;
    aar_shared b;
    aar_shared blocks[AAR_SHARED_MIN_BLOCKS];
    aar_info info;
    void *p = 0;
    uint8_t *raw;
    uint32_t i;

    info.size = (uint32_t)sizeof info;
    if (aar_get_info(&info) != AAR_OK)
        return fail(130, "info");
    if (aar_shared_alloc(100u, 0u, 0u, &a) != AAR_OK)
        return fail(131, "shared alloc");
    if (a.bytes != info.shared_granule || !a.ptr || a.haddr == 0u ||
        (a.flags & AAR_SHAREDF_COHERENT) == 0u ||
        ((uintptr_t)a.ptr & (info.shared_granule - 1u)) != 0u)
        return fail(132, "shared block fields");
    for (i = 0; i < 100u; i++)
        if (((uint8_t *)a.ptr)[i] != 0u)
            return fail(133, "shared block is not zeroed");
    if (aar_shared_alloc(100u, 0u, 1u, &b) != AAR_E_INVAL ||
        aar_shared_alloc(0u, 0u, 0u, &b) != AAR_E_INVAL ||
        aar_shared_alloc(100u, 0u, 0u, 0) != AAR_E_INVAL ||
        aar_shared_alloc(100u, 3u * info.shared_granule, 0u, &b) != AAR_E_INVAL)
        return fail(134, "bad shared alloc arguments");
    if (aar_get_info(&info) != AAR_OK || info.shared_used != a.bytes)
        return fail(135, "shared_used after one block");

    /* The token resolves to the block, and only to the block. */
    if (aar_host_shared_resolve(a.haddr, 0u, 100u, &p) != AAR_OK || p != a.ptr)
        return fail(136, "resolve the whole block");
    if (aar_host_shared_resolve(a.haddr, 64u, 8u, &p) != AAR_OK ||
        p != (uint8_t *)a.ptr + 64)
        return fail(137, "resolve an interior range");
    if (aar_host_shared_resolve(a.haddr, a.bytes, 1u, &p) != AAR_E_INVAL ||
        aar_host_shared_resolve(a.haddr, 1u, a.bytes, &p) != AAR_E_INVAL ||
        aar_host_shared_resolve(a.haddr + 1u, 0u, 1u, &p) != AAR_E_INVAL)
        return fail(138, "resolve outside the block");
    raw = (uint8_t *)a.ptr;
    raw[5] = 0x7e;
    if (aar_publish(a.ptr, 8u) != AAR_OK || aar_acquire(a.ptr, 8u) != AAR_OK ||
        raw[5] != 0x7e)
        return fail(139, "publish and acquire");

    /* Alignment past the granule applies to the pointer and the token. */
    if (aar_shared_alloc(1u, 4u * info.shared_granule, 0u, &b) != AAR_OK ||
        ((uintptr_t)b.ptr & (4u * info.shared_granule - 1u)) != 0u ||
        (b.haddr & (4u * info.shared_granule - 1u)) != 0u)
        return fail(140, "aligned shared block");
    if (b.haddr < a.haddr + a.bytes && a.haddr < b.haddr + b.bytes)
        return fail(141, "tokens overlap");
    if (aar_shared_free((uint8_t *)b.ptr + 1) != AAR_E_INVAL ||
        aar_shared_free(0) != AAR_E_INVAL)
        return fail(142, "free of a non-base pointer");
    if (aar_shared_free(b.ptr) != AAR_OK || aar_shared_free(b.ptr) != AAR_E_INVAL)
        return fail(143, "free, then free again");
    if (aar_host_shared_resolve(b.haddr, 0u, 1u, &p) != AAR_E_INVAL)
        return fail(144, "a freed token still resolves");

    /* The default limit is 16 MiB; the promised block count is met. */
    if (aar_shared_alloc(16u << 20, 0u, 0u, &b) != AAR_E_NOMEM)
        return fail(145, "the limit does not count the live block");
    if (aar_shared_free(a.ptr) != AAR_OK)
        return fail(146, "free the first block");
    if (aar_shared_alloc(16u << 20, 0u, 0u, &b) != AAR_OK)
        return fail(147, "a block of the whole limit");
    if (aar_shared_alloc(1u, 0u, 0u, &a) != AAR_E_NOMEM)
        return fail(148, "alloc past the limit");
    if (aar_shared_free(b.ptr) != AAR_OK)
        return fail(149, "free the big block");
    for (i = 0; i < AAR_SHARED_MIN_BLOCKS; i++)
        if (aar_shared_alloc(1u, 0u, 0u, &blocks[i]) != AAR_OK)
            return fail(150, "fewer blocks than AAR_SHARED_MIN_BLOCKS");
    if (aar_shared_alloc(1u, 0u, 0u, &a) != AAR_E_NOMEM)
        return fail(151, "alloc past the block count");
    for (i = 0; i < AAR_SHARED_MIN_BLOCKS; i++)
        if (aar_shared_free(blocks[i].ptr) != AAR_OK)
            return fail(152, "free a block");
    if (aar_get_info(&info) != AAR_OK || info.shared_used != 0u)
        return fail(153, "shared_used after freeing everything");

    /* The private allocator. */
    p = aar_alloc(100u, 64u);
    if (!p || ((uintptr_t)p & 63u) != 0u)
        return fail(154, "aar_alloc alignment");
    aar_free(p);
    p = aar_alloc(10u, 4096u);
    if (!p || ((uintptr_t)p & 4095u) != 0u)
        return fail(155, "aar_alloc page alignment");
    aar_free(p);
    p = aar_alloc(10u, 0u);
    if (!p)
        return fail(156, "aar_alloc default alignment");
    aar_free(p);
    if (aar_alloc(10u, 3u) != 0)
        return fail(157, "aar_alloc accepted an odd alignment");
    aar_free(0);
    return 0;
}

static int test_audio(void)
{
    aar_audio audio;
    aar_audio_stats stats;
    uint32_t i;

    if (aar_audio_close() != AAR_E_INVAL)
        return fail(160, "close without a stream");
    if (aar_audio_open(11025u, 2u, &audio) != AAR_E_INVAL ||
        aar_audio_open(AAR_AUDIO_RATE_22050, 1u, &audio) != AAR_E_INVAL ||
        aar_audio_open(AAR_AUDIO_RATE_22050, 2u, 0) != AAR_E_INVAL)
        return fail(161, "bad audio open arguments");
    memset(&audio, 0xaa, sizeof audio);
    if (aar_audio_open(AAR_AUDIO_RATE_22050, 2u, &audio) != AAR_OK)
        return fail(162, "audio open");
    if (audio.rate != AAR_AUDIO_RATE_22050 || audio.channels != 2u ||
        audio.flags != 0u || audio.frames == 0u ||
        (audio.frames & (audio.frames - 1u)) != 0u || !audio.samples ||
        !audio.read_frame || !audio.write_frame)
        return fail(163, "audio fields");
    if (*audio.read_frame != 0u || *audio.write_frame != 0u)
        return fail(164, "audio indices start at 0");
    if (aar_audio_open(AAR_AUDIO_RATE_22050, 2u, &audio) != AAR_E_BUSY)
        return fail(165, "second audio open was not EBUSY");
    memset(&stats, 0xaa, sizeof stats);
    stats.size = (uint32_t)sizeof stats;
    if (aar_audio_get_stats(&stats) != AAR_OK || stats.underruns != 0u)
        return fail(166, "audio stats");
    for (i = 0; i < 6u; i++)
        if (stats.reserved[i] != 0u)
            return fail(167, "audio stats reserved");
    if (aar_audio_get_stats(0) != AAR_E_INVAL)
        return fail(168, "audio stats into NULL");
    if (aar_audio_close() != AAR_OK)
        return fail(169, "audio close");
    if (aar_audio_get_stats(&stats) != AAR_E_INVAL)
        return fail(170, "stats on a closed stream");
    if (aar_audio_open(AAR_AUDIO_RATE_48000, 2u, &audio) != AAR_OK ||
        audio.rate != AAR_AUDIO_RATE_48000)
        return fail(171, "48000 Hz was not granted");
    if (aar_audio_close() != AAR_OK)
        return fail(172, "second close");
    return 0;
}

/* aar_msg_wait without a script: sends loop back into the queue. */
static int test_msgwait(void)
{
    uint8_t msg[AAR_MSG_BYTES];
    uint64_t start;
    uint64_t took;

    memset(msg, 0x11, sizeof msg);
    if (aar_msg_wait(0u) != AAR_E_TIMEDOUT)
        return fail(180, "zero timeout on an empty queue");
    start = aar_time_us();
    if (aar_msg_wait(40000u) != AAR_E_TIMEDOUT)
        return fail(181, "empty queue did not time out");
    took = aar_time_us() - start;
    if (took < 35000u || took > 5000000u)
        return fail(182, "timeout length");
    if (aar_msg_send(msg) != AAR_OK)
        return fail(183, "loopback send");
    start = aar_time_us();
    if (aar_msg_wait(5000000u) != AAR_OK)
        return fail(184, "queued message not ready");
    if (aar_time_us() - start > 1000000u)
        return fail(185, "a queued message still waited");
    if (aar_msg_wait(0u) != AAR_OK)
        return fail(186, "wait consumed the message");
    if (aar_msg_poll(msg) != 1)
        return fail(187, "poll after wait");
    if (aar_msg_wait(0u) != AAR_E_TIMEDOUT)
        return fail(188, "queue not empty after poll");
    return 0;
}

/* Parks on an empty queue; the test harness stops the runner. */
static int test_msgwaitstop(void)
{
    if (aar_msg_wait(30000000u) != AAR_E_IO)
        return fail(190, "stop did not end the wait with AAR_E_IO");
    return 0;
}

/* An empty wait must sleep: the harness measures the CPU time it burns. */
static int test_msgwaitidle(void)
{
    if (aar_msg_wait(1500000u) != AAR_E_TIMEDOUT)
        return fail(191, "idle wait did not time out");
    return 0;
}

int aar_main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";

    if (strcmp(mode, "table") == 0)
        return test_table(argc > 2 && strcmp(argv[2], "nogl") == 0);
    if (strcmp(mode, "reject") == 0)
        return test_reject();
    if (strcmp(mode, "abs") == 0)
        return test_abs();
    if (strcmp(mode, "input") == 0)
        return test_input();
    if (strcmp(mode, "display") == 0)
        return test_display();
    if (strcmp(mode, "services") == 0)
        return test_services();
    if (strcmp(mode, "memory") == 0)
        return test_memory();
    if (strcmp(mode, "audio") == 0)
        return test_audio();
    if (strcmp(mode, "msgwait") == 0)
        return test_msgwait();
    if (strcmp(mode, "msgwaitstop") == 0)
        return test_msgwaitstop();
    if (strcmp(mode, "msgwaitidle") == 0)
        return test_msgwaitidle();
    return fail(1, "usage: contract <table|reject|abs|input|display|services|"
                   "memory|audio|msgwait|msgwaitstop|msgwaitidle>");
}
