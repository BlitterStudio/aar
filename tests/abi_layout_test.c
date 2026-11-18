/*
 * Layout test for the AAR public headers: every structure size and offset,
 * the table slot order, and the constant values that are part of the ABI.
 * Build it for ILP32 and LP64 on a host (see tests/abi_check.sh):
 *
 *   gcc -m32 -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
 *       tests/abi_layout_test.c -o abi_layout_test32
 *   gcc      -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude \
 *       tests/abi_layout_test.c -o abi_layout_test64
 *
 * The headers also carry _Static_assert checks of their own; compiling this
 * file at all proves those hold for the data model in use.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "aar/app.h"
#include "aar/gl.h"
#include "aar/ext_zz9000.h"
#include "aar/note.h"
#include "aar/transport.h"

/* On a host build this expands to nothing, as ZZAPP_NOTE did. */
AAR_NOTE(0, 0, AAR_GROUP_DISPLAY, AAR_GROUP_AUDIO)
AAR_NOTE_EX(5, 65536, 4, 0, 0)

static unsigned long checks;
static unsigned long failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                \
    } while (0)

#define CHECK_EQ(actual, expected)                                       \
    do {                                                                 \
        unsigned long long a_ = (unsigned long long)(actual);            \
        unsigned long long e_ = (unsigned long long)(expected);          \
        checks++;                                                        \
        if (a_ != e_) {                                                  \
            failures++;                                                  \
            printf("FAIL %s:%d: %s is %llu, expected %llu\n", __FILE__,  \
                   __LINE__, #actual, a_, e_);                           \
        }                                                                \
    } while (0)

#define P (sizeof(void *))

/* The table slots in their documented order. */
#define API_SLOTS(X)                                                     \
    X(exit) X(yield) X(should_stop) X(time_us) X(sleep_us) X(info)       \
    X(heap_range) X(heap_grow) X(heap_shrink) X(heap_info)               \
    X(shared_alloc) X(shared_free) X(publish) X(acquire)                 \
    X(log) X(file_open) X(file_close) X(file_read) X(file_write)         \
    X(file_seek) X(file_stat) X(file_list_open) X(file_list_next)        \
    X(file_list_close) X(console_write) X(console_read)                  \
    X(event_poll) X(msg_send) X(msg_poll) X(msg_wait)                    \
    X(ext) X(map_file) X(unmap_file)                                     \
    X(display_open) X(display_flip) X(display_flip_async)                \
    X(display_flip_idle) X(display_refresh) X(display_vsync)             \
    X(display_set_palette) X(display_close) X(display_latest_dropped)    \
    X(audio_open) X(audio_close) X(audio_stats)                          \
    X(gl_bind) X(gl_swap) X(gl_unbind)                                   \
    X(sha256) X(decompress) X(image_open) X(image_decode)                \
    X(image_close) X(audio_decode_open) X(audio_decode)                  \
    X(audio_decode_close)

#define GL_SLOTS(X)                                                      \
    X(Begin) X(End) X(Vertex2f) X(Vertex3f) X(Vertex3fv) X(TexCoord2f)   \
    X(TexCoord2fv) X(Color3f) X(Color3fv) X(Color3ubv) X(Color4f)        \
    X(Color4fv) X(Color4ub) X(Color4ubv) X(Enable) X(Disable)            \
    X(IsEnabled) X(AlphaFunc) X(BlendFunc) X(DepthFunc) X(DepthMask)     \
    X(DepthRange) X(CullFace) X(FrontFace) X(ShadeModel) X(PolygonMode)  \
    X(Clear) X(ClearColor) X(Viewport) X(Scissor) X(MatrixMode)          \
    X(LoadIdentity) X(LoadMatrixf) X(PushMatrix) X(PopMatrix)            \
    X(Rotatef) X(Translatef) X(Scalef) X(Ortho) X(Frustum)               \
    X(TexImage2D) X(TexSubImage2D) X(TexParameteri) X(TexParameterf)     \
    X(TexEnvi) X(TexEnvf) X(GenTextures) X(DeleteTextures)               \
    X(BindTexture) X(Finish) X(Flush) X(DrawBuffer) X(ReadBuffer)        \
    X(ReadPixels) X(GetString) X(GetIntegerv) X(GetFloatv) X(GetError)   \
    X(Hint) X(PixelStorei) X(Perspective) X(Build2DMipmaps)              \
    X(VertexPointer) X(TexCoordPointer) X(ColorPointer)                  \
    X(EnableClientState) X(DisableClientState)                           \
    X(ClientActiveTextureARB) X(DrawArrays) X(DrawElements)              \
    X(LockArrays) X(UnlockArrays)

static void test_table(void)
{
    size_t index = 0;

#define X(name)                                                          \
    CHECK_EQ(offsetof(aar_api, name), 16 + index * P);                   \
    index++;
    API_SLOTS(X)
#undef X
    CHECK_EQ(index, AAR_API_SLOT_COUNT);
    CHECK_EQ(sizeof(aar_api), 16 + AAR_API_SLOT_COUNT * P);
    CHECK_EQ(offsetof(aar_api, size), 0);
    CHECK_EQ(offsetof(aar_api, version), 4);
    CHECK_EQ(offsetof(aar_api, groups), 8);
    CHECK_EQ(offsetof(aar_api, platform_id), 12);
    CHECK_EQ(offsetof(aar_api, exit), 16);
    CHECK_EQ(offsetof(aar_api, msg_wait), 16 + 29 * P);
    CHECK_EQ(offsetof(aar_api, ext), 16 + 30 * P);
    CHECK_EQ(offsetof(aar_api, map_file), 16 + 31 * P);
    CHECK_EQ(offsetof(aar_api, unmap_file), 16 + 32 * P);
    CHECK_EQ(offsetof(aar_api, display_open), 16 + 33 * P);
    CHECK_EQ(offsetof(aar_api, audio_open), 16 + 42 * P);
    CHECK_EQ(offsetof(aar_api, gl_bind), 16 + 45 * P);
    CHECK_EQ(offsetof(aar_api, sha256), 16 + 48 * P);
    CHECK_EQ(offsetof(aar_api, audio_decode_close), 16 + 55 * P);

    /* AAR_API_COVERS gates on size. */
    {
        aar_api t = {0};

        t.size = (uint32_t)offsetof(aar_api, ext) + (uint32_t)P;
        CHECK(AAR_API_COVERS(&t, ext));
        CHECK(!AAR_API_COVERS(&t, map_file));
        CHECK(AAR_API_COVERS(&t, msg_wait));
        CHECK(!AAR_API_COVERS((const aar_api *)0, exit));
        t.version = AAR_API_VERSION;
        CHECK(AAR_API_AT_LEAST(&t, 5));
        CHECK(!AAR_API_AT_LEAST(&t, 6));
        t.version = (1u << 16) | 9u;
        CHECK(!AAR_API_AT_LEAST(&t, 5));
    }
}

static void test_gl(void)
{
    size_t index = 0;

#define X(name)                                                          \
    CHECK_EQ(offsetof(aar_gl, name), 8 + index * P);                     \
    index++;
    GL_SLOTS(X)
#undef X
    CHECK_EQ(index, AAR_GL_SLOT_COUNT);
    CHECK_EQ(sizeof(aar_gl), 8 + AAR_GL_SLOT_COUNT * P);
    CHECK_EQ(offsetof(aar_gl, size), 0);
    CHECK_EQ(offsetof(aar_gl, version), 4);
    CHECK_EQ(offsetof(aar_gl, Begin), 8);
    CHECK_EQ(offsetof(aar_gl, Build2DMipmaps), 8 + 61 * P);
    CHECK_EQ(offsetof(aar_gl, VertexPointer), 8 + 62 * P);
    CHECK_EQ(offsetof(aar_gl, UnlockArrays), 8 + 71 * P);
    {
        aar_gl g = {0};

        g.size = (uint32_t)(offsetof(aar_gl, Build2DMipmaps) + P);
        CHECK(AAR_GL_COVERS(&g, Perspective));
        CHECK(AAR_GL_COVERS(&g, Build2DMipmaps));
        CHECK(!AAR_GL_COVERS(&g, VertexPointer));
        CHECK(!AAR_GL_COVERS((const aar_gl *)0, Begin));
    }
    CHECK_EQ(AAR_GL_VERSION_V0, 0);
    CHECK_EQ(AAR_GL_VERSION_ARRAYS, 1);
}

static void test_structures(void)
{
    /* aar_info: 88 bytes, same on both profiles. */
    CHECK_EQ(sizeof(aar_info), 88);
    CHECK_EQ(offsetof(aar_info, size), 0);
    CHECK_EQ(offsetof(aar_info, profile), 4);
    CHECK_EQ(offsetof(aar_info, platform_id), 8);
    CHECK_EQ(offsetof(aar_info, flags), 12);
    CHECK_EQ(offsetof(aar_info, groups), 16);
    CHECK_EQ(offsetof(aar_info, heap_granule), 20);
    CHECK_EQ(offsetof(aar_info, shared_granule), 24);
    CHECK_EQ(offsetof(aar_info, msg_capacity), 28);
    CHECK_EQ(offsetof(aar_info, sleep_granule_us), 32);
    CHECK_EQ(offsetof(aar_info, stall_limit_ms), 36);
    CHECK_EQ(offsetof(aar_info, stop_grace_ms), 40);
    CHECK_EQ(offsetof(aar_info, cpu_features), 44);
    CHECK_EQ(offsetof(aar_info, shared_limit), 48);
    CHECK_EQ(offsetof(aar_info, shared_used), 56);
    CHECK_EQ(offsetof(aar_info, max_files), 64);
    CHECK_EQ(offsetof(aar_info, reserved), 68);

    CHECK_EQ(sizeof(aar_heap_info), 24);
    CHECK_EQ(offsetof(aar_heap_info, committed), 0);
    CHECK_EQ(offsetof(aar_heap_info, limit), 8);
    CHECK_EQ(offsetof(aar_heap_info, pool_free), 16);

    /* Fixed fields first, pointers last. */
    CHECK_EQ(offsetof(aar_shared, bytes), 0);
    CHECK_EQ(offsetof(aar_shared, haddr), 8);
    CHECK_EQ(offsetof(aar_shared, flags), 12);
    CHECK_EQ(offsetof(aar_shared, ptr), 16);

    CHECK_EQ(sizeof(aar_event), 16);
    CHECK_EQ(offsetof(aar_event, type), 0);
    CHECK_EQ(offsetof(aar_event, code), 2);
    CHECK_EQ(offsetof(aar_event, dx), 4);
    CHECK_EQ(offsetof(aar_event, dy), 6);
    CHECK_EQ(offsetof(aar_event, buttons), 8);
    CHECK_EQ(offsetof(aar_event, reserved), 10);
    CHECK_EQ(offsetof(aar_event, time_ms), 12);

    CHECK_EQ(sizeof(aar_stat), 24);
    CHECK_EQ(offsetof(aar_stat, size), 0);
    CHECK_EQ(offsetof(aar_stat, mtime), 8);
    CHECK_EQ(offsetof(aar_stat, mode), 16);
    CHECK_EQ(offsetof(aar_stat, reserved), 20);
    CHECK_EQ(sizeof(aar_dirent), 260);
    CHECK_EQ(offsetof(aar_dirent, mode), 0);
    CHECK_EQ(offsetof(aar_dirent, name), 4);

    CHECK_EQ(offsetof(aar_surface, id), 0);
    CHECK_EQ(offsetof(aar_surface, width), 4);
    CHECK_EQ(offsetof(aar_surface, height), 8);
    CHECK_EQ(offsetof(aar_surface, pitch), 12);
    CHECK_EQ(offsetof(aar_surface, format), 16);
    CHECK_EQ(offsetof(aar_surface, layout), 20);
    CHECK_EQ(offsetof(aar_surface, caps), 24);
    CHECK_EQ(offsetof(aar_surface, reserved), 28);
    CHECK_EQ(offsetof(aar_surface, front), 32);
    CHECK_EQ(offsetof(aar_surface, back), 32 + P);

    CHECK_EQ(offsetof(aar_audio, frames), 0);
    CHECK_EQ(offsetof(aar_audio, rate), 4);
    CHECK_EQ(offsetof(aar_audio, channels), 8);
    CHECK_EQ(offsetof(aar_audio, flags), 12);
    CHECK_EQ(offsetof(aar_audio, samples), 16);
    CHECK_EQ(offsetof(aar_audio, read_frame), 16 + P);
    CHECK_EQ(offsetof(aar_audio, write_frame), 16 + 2 * P);
    CHECK_EQ(sizeof(aar_audio_stats), 32);
    CHECK_EQ(offsetof(aar_audio_stats, size), 0);
    CHECK_EQ(offsetof(aar_audio_stats, underruns), 4);

    CHECK_EQ(sizeof(aar_service_result), 24);
    CHECK_EQ(offsetof(aar_service_result, bytes_consumed), 0);
    CHECK_EQ(offsetof(aar_service_result, bytes_written), 8);
    CHECK_EQ(offsetof(aar_service_result, flags), 16);
    CHECK_EQ(offsetof(aar_image_params, codec), 0);
    CHECK_EQ(offsetof(aar_image_params, output_format), 4);
    CHECK_EQ(offsetof(aar_image_params, stride), 8);
    CHECK_EQ(offsetof(aar_image_params, rows), 12);
    CHECK_EQ(offsetof(aar_image_params, tile_bytes), 16);
    CHECK_EQ(offsetof(aar_image_params, flags), 20);
    CHECK_EQ(offsetof(aar_image_params, tile), 24);
    CHECK_EQ(sizeof(aar_image_result), 20);
    CHECK_EQ(offsetof(aar_image_result, image_width), 0);
    CHECK_EQ(offsetof(aar_image_result, image_height), 4);
    CHECK_EQ(offsetof(aar_image_result, bytes_consumed), 8);
    CHECK_EQ(offsetof(aar_image_result, bytes_written), 12);
    CHECK_EQ(offsetof(aar_image_result, flags), 16);
    CHECK_EQ(sizeof(aar_audio_decode_result), 16);
    CHECK_EQ(offsetof(aar_audio_decode_result, bytes_consumed), 0);
    CHECK_EQ(offsetof(aar_audio_decode_result, frames_written), 4);
    CHECK_EQ(offsetof(aar_audio_decode_result, rate), 8);
    CHECK_EQ(offsetof(aar_audio_decode_result, channels), 12);

    /* ZZ9000 extension table. */
    CHECK_EQ(offsetof(aar_ext_zz9000, size), 0);
    CHECK_EQ(offsetof(aar_ext_zz9000, reserved), 4);
    CHECK_EQ(offsetof(aar_ext_zz9000, gl_bind_flags), 8);
    CHECK_EQ(offsetof(aar_ext_zz9000, build_id), 8 + P);
    CHECK_EQ(offsetof(aar_ext_zz9000, gl_build_id), 8 + 2 * P);
    CHECK_EQ(sizeof(aar_ext_zz9000), 8 + 3 * P);
    {
        aar_ext_zz9000 e = {0};

        e.size = (uint32_t)(offsetof(aar_ext_zz9000, build_id) + P);
        CHECK(AAR_EXT_ZZ9000_COVERS(&e, gl_bind_flags));
        CHECK(AAR_EXT_ZZ9000_COVERS(&e, build_id));
        CHECK(!AAR_EXT_ZZ9000_COVERS(&e, gl_build_id));
    }

    /* Whole-structure sizes for the two data models. 64-bit pointers are
     * A64. 32-bit pointers on x86 (the -m32 host run) align uint64_t to 4;
     * on A32 (ARM EABI) it aligns to 8 and rounds the pointer-carrying
     * structures up, which AAR_ABI_SIZE accounts for. */
    if (P == 8) {
        CHECK_EQ(sizeof(aar_shared), 24);
        CHECK_EQ(sizeof(aar_surface), 48);
        CHECK_EQ(sizeof(aar_audio), 40);
        CHECK_EQ(sizeof(aar_image_params), 32);
        CHECK_EQ(sizeof(aar_api), 16 + 56 * 8);
        CHECK_EQ(sizeof(aar_gl), 8 + 72 * 8);
    } else {
        CHECK_EQ(sizeof(aar_surface), 40);
        CHECK_EQ(sizeof(aar_audio), 28);
        CHECK_EQ(sizeof(aar_image_params), 28);
        CHECK_EQ(sizeof(aar_api), 16 + 56 * 4);
        CHECK_EQ(sizeof(aar_gl), 8 + 72 * 4);
#if defined(__arm__)
        CHECK_EQ(sizeof(aar_shared), 24);
#else
        CHECK_EQ(sizeof(aar_shared), 20);
#endif
    }
}

static void test_constants(void)
{
    CHECK_EQ(AAR_API_MAJOR, 0);
    CHECK_EQ(AAR_API_MINOR, 5);
    CHECK_EQ(AAR_API_VERSION, 5);
    CHECK_EQ(AAR_PROFILE_A32, 1);
    CHECK_EQ(AAR_PROFILE_A64, 2);

    CHECK_EQ(AAR_OK, 0);
    CHECK_EQ(AAR_E_IO, -1);
    CHECK_EQ(AAR_E_INVAL, -2);
    CHECK_EQ(AAR_E_NOMEM, -3);
    CHECK_EQ(AAR_E_BUSY, -4);
    CHECK_EQ(AAR_E_NOENT, -5);
    CHECK_EQ(AAR_E_ACCES, -6);
    CHECK_EQ(AAR_E_NOSYS, -7);
    CHECK_EQ(AAR_E_TIMEDOUT, -8);
    CHECK_EQ(AAR_E_FAULT, -9);
    CHECK_EQ(AAR_E_NOSPC, -10);
    CHECK_EQ(AAR_E_EXIST, -11);
    CHECK_EQ(AAR_E_BADF, -12);
    CHECK_EQ(AAR_E_PROFILE, -13);
    CHECK_EQ(AAR_E_NOGROUP, -14);
    CHECK_EQ(AAR_E_NORESOURCE, -15);
    CHECK_EQ(AAR_E_STALE, -16);
    CHECK_EQ(AAR_E_OWNER, -17);

    CHECK_EQ(AAR_REASON_MEMORY, 1);
    CHECK_EQ(AAR_REASON_TABLES, 2);
    CHECK_EQ(AAR_REASON_HOSTHEAP, 3);
    CHECK_EQ(AAR_REASON_LIMIT, 4);

    CHECK_EQ(AAR_STATE_IDLE, 0);
    CHECK_EQ(AAR_STATE_LOADING, 1);
    CHECK_EQ(AAR_STATE_STARTING, 2);
    CHECK_EQ(AAR_STATE_RUNNING, 3);
    CHECK_EQ(AAR_STATE_STOPPING, 5);
    CHECK_EQ(AAR_STATE_FAULTED, 6);
    CHECK_EQ(AAR_STATE_CLAIMED, 7);
    CHECK_EQ(AAR_FAULT_NONE, 0);
    CHECK_EQ(AAR_FAULT_DATA, 1);
    CHECK_EQ(AAR_FAULT_FETCH, 2);
    CHECK_EQ(AAR_FAULT_ILLEGAL, 3);
    CHECK_EQ(AAR_FAULT_ALIGN, 4);
    CHECK_EQ(AAR_FAULT_OTHER, 5);
    CHECK_EQ(AAR_END_NONE, 0);
    CHECK_EQ(AAR_END_EXIT, 1);
    CHECK_EQ(AAR_END_FORCED, 2);
    CHECK_EQ(AAR_END_USER_FAULT, 3);
    CHECK_EQ(AAR_END_STALL, 4);
    CHECK_EQ(AAR_END_STUCK_CALL, 5);
    CHECK_EQ(AAR_END_LEASE, 6);
    CHECK_EQ(AAR_END_PLATFORM_RESET, 7);
    CHECK_EQ(AAR_END_RUNTIME_RESTART, 8);
    CHECK_EQ(AAR_END_QUARANTINED, 9);

    CHECK_EQ(AAR_NOTE_MAGIC, 0x4E524141u);
    CHECK_EQ(AAR_NOTE_TYPE, 1);
    CHECK_EQ(AAR_NOTE_VERSION, 1);
    CHECK_EQ(AAR_NOTE_DESC_BYTES, 40);

    CHECK_EQ(AAR_GROUP_DISPLAY, 1);
    CHECK_EQ(AAR_GROUP_INPUT, 2);
    CHECK_EQ(AAR_GROUP_AUDIO, 4);
    CHECK_EQ(AAR_GROUP_GL, 8);
    CHECK_EQ(AAR_GROUP_SERVICES, 16);
    CHECK_EQ(AAR_GROUPS_EXPERIMENTAL, 0xF0000000u);

    CHECK_EQ(AAR_EV_KEY_DOWN, 1);
    CHECK_EQ(AAR_EV_KEY_UP, 2);
    CHECK_EQ(AAR_EV_MOUSE, 3);
    CHECK_EQ(AAR_EV_JOY, 4);
    CHECK_EQ(AAR_EV_STOP, 5);
    CHECK_EQ(AAR_EV_RESIZE, 6);
    CHECK_EQ(AAR_EV_BREAK, 7);
    CHECK_EQ(AAR_EV_LOW_MEMORY, 8);
    CHECK_EQ(AAR_BREAK_CTRL_D, 4);
    CHECK_EQ(AAR_BREAK_CTRL_E, 5);
    CHECK_EQ(AAR_BREAK_CTRL_F, 6);

    CHECK_EQ(AAR_FMT_RGB565, 1);
    CHECK_EQ(AAR_FMT_BGRA8888, 2);
    CHECK_EQ(AAR_FMT_CLUT8, 3);
    CHECK_EQ(AAR_LAYOUT_RGB565_BE, 1);
    CHECK_EQ(AAR_LAYOUT_RGB565_LE, 2);
    CHECK_EQ(AAR_LAYOUT_BGR565_BE, 3);
    CHECK_EQ(AAR_LAYOUT_BGR565_LE, 4);
    CHECK_EQ(AAR_LAYOUT_BGRA8888, 5);
    CHECK_EQ(AAR_LAYOUT_RGBA8888, 6);
    CHECK_EQ(AAR_LAYOUT_CLUT8, 7);
    CHECK_EQ(AAR_DISPLAY_VSYNC, 1);
    CHECK_EQ(AAR_DISPLAY_LATEST, 2);
    CHECK(AAR_DISPCAP_ASYNC <= 0xFFFFu && AAR_DISPCAP_LATEST <= 0xFFFFu &&
          AAR_DISPCAP_CLUT8 <= 0xFFFFu);

    CHECK_EQ(AAR_FILE_READ, 1);
    CHECK_EQ(AAR_FILE_WRITE, 2);
    CHECK_EQ(AAR_FILE_CREATE, 4);
    CHECK_EQ(AAR_SEEK_SET, 0);
    CHECK_EQ(AAR_SEEK_CUR, 1);
    CHECK_EQ(AAR_SEEK_END, 2);

    CHECK_EQ(AAR_COMPRESS_DEFLATE_RAW, 1);
    CHECK_EQ(AAR_COMPRESS_LH7, 10);
    CHECK_EQ(AAR_IMAGE_JPEG, 1);
    CHECK_EQ(AAR_IMAGE_PNG, 2);
    CHECK_EQ(AAR_IMAGE_WEBP, 4);
    CHECK_EQ(AAR_AUDIO_MP3, 1);
    CHECK_EQ(AAR_AUDIO_FLAC, 2);
    CHECK_EQ(AAR_AUDIO_VORBIS, 3);

    CHECK_EQ(AAR_CPU_A32_VFPV4, 1);
    CHECK_EQ(AAR_CPU_A32_IDIV, 2);
    CHECK_EQ(AAR_CPU_A32_FP16, 4);
    CHECK_EQ(AAR_CPU_A64_CRC32, 1);
    CHECK_EQ(AAR_CPU_A64_LSE, 2);
    CHECK_EQ(AAR_CPU_A64_AES, 4);
    CHECK_EQ(AAR_CPU_A64_SHA, 8);
    CHECK_EQ(AAR_CPU_A64_DOTPROD, 16);
    CHECK_EQ(AAR_CPU_A64_FP16, 32);

    /* The little-endian bytes of the note magic spell AARN. */
    {
        uint32_t magic = AAR_NOTE_MAGIC;
        const unsigned char *b = (const unsigned char *)&magic;

        if (b[0] == 'A')
            CHECK(b[0] == 'A' && b[1] == 'A' && b[2] == 'R' && b[3] == 'N');
    }

    /* aar_bswap helpers. */
    CHECK_EQ(aar_bswap16(0x1234u), 0x3412u);
    CHECK_EQ(aar_bswap32(0x12345678u), 0x78563412u);
    CHECK_EQ(aar_bswap64(0x0102030405060708ull), 0x0807060504030201ull);
}

static void test_transport_layout(void)
{
    /* Every control-block word offset: 4-aligned, inside the block, unique, and
     * 64-bit pairs adjacent. */
    static const unsigned offsets[] = {
        AAR_CB_MAGIC, AAR_CB_VERSION, AAR_CB_SIZE, AAR_CB_HANDLE,
        AAR_CB_PROFILE, AAR_CB_STATE, AAR_CB_END_REASON, AAR_CB_EXIT_CODE,
        AAR_CB_FLAGS, AAR_CB_PROGRESS_HI, AAR_CB_PROGRESS_LO,
        AAR_CB_LEASE_PERIOD_MS, AAR_CB_STOP_REQUEST, AAR_CB_CLIENT_PRESENT,
        AAR_CB_LEASE_COUNTER, AAR_CB_EVENTS_DROPPED, AAR_CB_CQ_FULL,
        AAR_CB_FAULT_CLASS, AAR_CB_FAULT_SYNDROME, AAR_CB_FAULT_PSTATE,
        AAR_CB_FAULT_PC_HI, AAR_CB_FAULT_PC_LO, AAR_CB_FAULT_SP_HI,
        AAR_CB_FAULT_SP_LO, AAR_CB_FAULT_LR_HI, AAR_CB_FAULT_LR_LO,
        AAR_CB_FAULT_ADDR_HI, AAR_CB_FAULT_ADDR_LO, AAR_CB_LOAD_BASE_HI,
        AAR_CB_LOAD_BASE_LO, AAR_CB_PRIVATE_MIB, AAR_CB_SHARED_MIB,
        AAR_CB_WEIGHT, AAR_CB_SHARED_USED_HI, AAR_CB_SHARED_USED_LO,
        AAR_CB_PRIVATE_COMMITTED_HI, AAR_CB_PRIVATE_COMMITTED_LO,
        AAR_CB_SQ_FULL, AAR_CB_LOG_DROPPED,
        AAR_CB_SQ_DESC + AAR_QD_TOKEN, AAR_CB_SQ_DESC + AAR_QD_OFFSET,
        AAR_CB_SQ_DESC + AAR_QD_DEPTH, AAR_CB_SQ_DESC + AAR_QD_ENTRY_BYTES,
        AAR_CB_SQ_TAIL, AAR_CB_SQ_HEAD,
        AAR_CB_CQ_DESC + AAR_QD_TOKEN, AAR_CB_CQ_DESC + AAR_QD_OFFSET,
        AAR_CB_CQ_DESC + AAR_QD_DEPTH, AAR_CB_CQ_DESC + AAR_QD_ENTRY_BYTES,
        AAR_CB_CQ_TAIL, AAR_CB_CQ_HEAD,
    };
    size_t n = sizeof(offsets) / sizeof(offsets[0]);
    size_t i, j;

    for (i = 0; i < n; i++) {
        CHECK((offsets[i] % 4u) == 0u);
        CHECK(offsets[i] + 4u <= AAR_CB_BYTES);
        for (j = i + 1; j < n; j++)
            CHECK(offsets[i] != offsets[j]);
    }
    CHECK_EQ(AAR_CB_PROGRESS_LO, AAR_CB_PROGRESS_HI + 4);
    CHECK_EQ(AAR_CB_FAULT_PC_LO, AAR_CB_FAULT_PC_HI + 4);
    CHECK_EQ(AAR_CB_FAULT_SP_LO, AAR_CB_FAULT_SP_HI + 4);
    CHECK_EQ(AAR_CB_FAULT_LR_LO, AAR_CB_FAULT_LR_HI + 4);
    CHECK_EQ(AAR_CB_FAULT_ADDR_LO, AAR_CB_FAULT_ADDR_HI + 4);
    CHECK_EQ(AAR_CB_LOAD_BASE_LO, AAR_CB_LOAD_BASE_HI + 4);
    CHECK_EQ(AAR_CB_SHARED_USED_LO, AAR_CB_SHARED_USED_HI + 4);
    CHECK_EQ(AAR_CB_PRIVATE_COMMITTED_LO, AAR_CB_PRIVATE_COMMITTED_HI + 4);

    CHECK_EQ(AAR_CB_BYTES, 0x1000);
    CHECK_EQ(AAR_CB_SQ_TAIL, 0x200);
    CHECK_EQ(AAR_CB_SQ_HEAD, 0x240);
    CHECK_EQ(AAR_CB_CQ_TAIL, 0x2C0);
    CHECK_EQ(AAR_CB_CQ_HEAD, 0x300);
    CHECK_EQ(AAR_CB_LEASE_COUNTER, 0x0C0);
    CHECK_EQ(AAR_CB_MAGIC_VALUE, 0x41415243u);

    /* Every word that changes hands owns its 64-byte line: no other listed
     * word sits in the same line unless it has the same writer. */
    {
        static const unsigned index_lines[] = {
            AAR_CB_SQ_TAIL, AAR_CB_SQ_HEAD, AAR_CB_CQ_TAIL, AAR_CB_CQ_HEAD,
        };
        for (i = 0; i < 4; i++) {
            CHECK((index_lines[i] % 64u) == 0u);
            for (j = 0; j < n; j++)
                if (offsets[j] != index_lines[i])
                    CHECK(offsets[j] / 64u != index_lines[i] / 64u);
        }
    }

    CHECK_EQ(AAR_ENTRY_BYTES, 64);
    CHECK_EQ(AAR_QUEUE_DEPTH_DEFAULT, 64);
    CHECK_EQ(AAR_MSG_CAPACITY, 63);
    CHECK_EQ(AAR_ENTRY_OP, 0);
    CHECK_EQ(AAR_ENTRY_FLAGS, 2);
    CHECK_EQ(AAR_ENTRY_ID, 4);
    CHECK_EQ(AAR_ENTRY_RESULT, 8);
    CHECK_EQ(AAR_ENTRY_BODY, 16);
    CHECK_EQ(AAR_ENTRY_ARG(0), 16);
    CHECK_EQ(AAR_ENTRY_ARG(5), 36);
    CHECK_EQ(AAR_ENTRY_BUF, 40);
    CHECK_EQ(AAR_ENTRY_BUF_OFF, 44);
    CHECK_EQ(AAR_ENTRY_BUF_LEN, 48);
    CHECK_EQ(AAR_ENTRY_OUT(5), 36);
    CHECK_EQ(AAR_ENTRY_MSG, 16);
    CHECK_EQ(AAR_PAIR_BYTES, 0x3000);
    CHECK_EQ(aar_pair_bytes(64), 0x3000);
    CHECK_EQ(aar_pair_cq_offset(64), 0x2000);
    CHECK_EQ(aar_pair_bytes(128), 0x1000 + 2 * 128 * 64);

    /* The ops are distinct and non-zero. */
    {
        static const unsigned ops[] = {
            AAR_OP_MSG, AAR_OP_EVENT, AAR_OP_LOG,
            AAR_OP_FILE_OPEN, AAR_OP_FILE_CLOSE, AAR_OP_FILE_READ,
            AAR_OP_FILE_WRITE, AAR_OP_FILE_SEEK, AAR_OP_FILE_STAT,
            AAR_OP_LIST_OPEN, AAR_OP_LIST_NEXT, AAR_OP_LIST_CLOSE,
            AAR_OP_CONSOLE_WRITE, AAR_OP_CONSOLE_READ,
            AAR_OP_DISPLAY_OPEN, AAR_OP_DISPLAY_FLIP,
            AAR_OP_DISPLAY_PALETTE, AAR_OP_DISPLAY_CLOSE,
            AAR_OP_AUDIO_OPEN, AAR_OP_AUDIO_CLOSE,
        };
        size_t m = sizeof(ops) / sizeof(ops[0]);

        for (i = 0; i < m; i++) {
            CHECK(ops[i] != AAR_OP_NONE && ops[i] <= 0xFFFFu);
            for (j = i + 1; j < m; j++)
                CHECK(ops[i] != ops[j]);
        }
    }

    /* Argument and result indices stay inside the six body words. */
    CHECK(AAR_A_FILE_WHENCE < AAR_ENTRY_ARGS);
    CHECK(AAR_R_STAT_MODE < AAR_ENTRY_OUTS);
    CHECK(AAR_R_DISPLAY_OPEN_SPARE < AAR_ENTRY_OUTS);
    CHECK(AAR_R_DISPLAY_FLIP_DROPPED < AAR_ENTRY_OUTS);
    CHECK(AAR_R_AUDIO_OPEN_FRAMES < AAR_ENTRY_OUTS);
    CHECK(AAR_A_AUDIO_OPEN_ROUTE < AAR_ENTRY_ARGS);
}

int main(void)
{
    test_table();
    test_gl();
    test_structures();
    test_constants();
    test_transport_layout();
    printf("abi_layout_test: %lu checks, %lu failures, pointer size %u\n",
           checks, failures, (unsigned)sizeof(void *));
    return failures == 0 ? 0 : 1;
}
