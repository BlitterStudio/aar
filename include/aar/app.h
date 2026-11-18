/*
 * AAR app interface: the table an app receives, the types and constants it
 * uses, and the prototypes of the wrappers that libaar defines over the table.
 *
 * AAR is the Amiga ARM Runtime standard. An app is a position-independent ARM
 * ELF (profile A32 or A64) that talks to its runtime through one function
 * table, `aar_api`. The table is the only entry into the runtime: never cache,
 * compare or call an address found any other way, and never write to the
 * table. Call the aar_* wrappers; they check the table and return AAR_E_NOSYS
 * (or 0, NULL, nothing) for a slot that is absent or NULL.
 *
 * Layout rules (all structures here):
 *   - Fixed-width integers only. Plain `int` is used only in the signatures of
 *     table calls, where it is a return code or an exit status.
 *   - Every fixed-width field has the same offset on A32 (ILP32) and A64
 *     (LP64). Where a structure carries pointers they come last, at an offset
 *     that is a multiple of 8 on both profiles, so no padding differs. The
 *     table header is four uint32_t (16 bytes), so the first pointer of the
 *     table is at offset 16 on both profiles.
 *   - _Static_assert checks at the end of this file hold for both data models.
 *
 * The old zzapp.h 0.x API maps onto this one as listed in
 * docs/zzapp-to-aar.md. API 0.5 continues after zzapp 0.4. Pre-1.0, any minor
 * may break.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_APP_H
#define AAR_APP_H

#include <stddef.h>
#include <stdint.h>

#include "gl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Versioning (spec 3.6) ------------------------------------------------ */

#define AAR_API_MAJOR 0u
#define AAR_API_MINOR 5u
#define AAR_API_VERSION ((AAR_API_MAJOR << 16) | AAR_API_MINOR)

/* True when the table names a runtime of this major and at least `minor`. The
 * preferred gate for a single call is still AAR_API_COVERS. Apps must not gate
 * on `version` alone. */
#define AAR_API_AT_LEAST(api, minor) \
    ((api) != 0 && ((api)->version >> 16) == AAR_API_MAJOR && \
        ((api)->version & 0xFFFFu) >= (uint32_t)(minor))

/* ---- Profiles and platforms ---------------------------------------------- */

/* A32: ARMv7-A, A32 state, hard-float AAPCS, VFPv3-D32 and NEON, ILP32.
 * A64: ARMv8-A AArch64, AAPCS64, FP and ASIMD, LP64. Both little-endian. */
#define AAR_PROFILE_A32 1u
#define AAR_PROFILE_A64 2u

/* aar_api.platform_id and the first argument of aar_ext(). 0 = unspecified.
 * Ids are allocated in the standard's repository; platform extension headers
 * (for example aar/ext_zz9000.h) define their own. */
#define AAR_PLATFORM_NONE   0u
#define AAR_PLATFORM_ZZ9000 1u

/* ---- Error codes (spec 3.5) ----------------------------------------------- */

/* One code space for the table, the client library and the transport. Calls
 * return AAR_OK or a negative value. The first thirteen keep the numbers of
 * zzapp.h 0.x. */
#define AAR_OK           0
#define AAR_E_IO        -1  /* failed; control block or queue corrupt; wait cancelled by stop */
#define AAR_E_INVAL     -2  /* bad argument, unknown flag, zero or oversize length, unknown id */
#define AAR_E_NOMEM     -3  /* memory, quota, table or session limit */
#define AAR_E_BUSY      -4  /* queue full, resource in use, flip pending */
#define AAR_E_NOENT     -5  /* no such file, or no further directory entry */
#define AAR_E_ACCES     -6  /* refused by the client's policy */
#define AAR_E_NOSYS     -7  /* slot absent or NULL; codec or format not implemented */
#define AAR_E_TIMEDOUT  -8  /* file wait expired, or msg_wait timeout */
#define AAR_E_FAULT     -9  /* bad pointer in a call (isolated runtimes only) */
#define AAR_E_NOSPC    -10
#define AAR_E_EXIST    -11
#define AAR_E_BADF     -12
#define AAR_E_PROFILE  -13  /* image profile or note not acceptable on this platform */
#define AAR_E_NOGROUP  -14  /* image requires a group the runtime lacks */
#define AAR_E_NORESOURCE -15 /* admission refused; the reason is an AAR_REASON_* code */
#define AAR_E_STALE    -16  /* the handle's generation no longer names a live app */
#define AAR_E_OWNER    -17  /* handle used by a task that does not own it */

/* Admission refusal reasons (spec 5.6), reported with AAR_E_NORESOURCE. */
#define AAR_REASON_NONE     0u
#define AAR_REASON_MEMORY   1u /* platform memory for the app's record and stack */
#define AAR_REASON_TABLES   2u /* address-space translation tables */
#define AAR_REASON_HOSTHEAP 3u /* shared memory for the control block and queues */
#define AAR_REASON_LIMIT    4u /* a platform ceiling */

/* ---- Lifecycle values the client reads (spec 5.1, 5.3) --------------------- */

/* Seen through AARStatus and stored in the control block. State 4 (PAUSED) is
 * not defined. */
#define AAR_STATE_IDLE     0u
#define AAR_STATE_LOADING  1u
#define AAR_STATE_STARTING 2u
#define AAR_STATE_RUNNING  3u
#define AAR_STATE_STOPPING 5u
#define AAR_STATE_FAULTED  6u
#define AAR_STATE_CLAIMED  7u

/* Profile-neutral fault classes. */
#define AAR_FAULT_NONE    0u
#define AAR_FAULT_DATA    1u
#define AAR_FAULT_FETCH   2u
#define AAR_FAULT_ILLEGAL 3u
#define AAR_FAULT_ALIGN   4u
#define AAR_FAULT_OTHER   5u

/* End reasons. */
#define AAR_END_NONE             0u
#define AAR_END_EXIT             1u
#define AAR_END_FORCED           2u
#define AAR_END_USER_FAULT       3u
#define AAR_END_STALL            4u
#define AAR_END_STUCK_CALL       5u
#define AAR_END_LEASE            6u
#define AAR_END_PLATFORM_RESET   7u
#define AAR_END_RUNTIME_RESTART  8u
#define AAR_END_QUARANTINED      9u

/* ---- The AAR ELF note (spec 2.3) -------------------------------------------- */

/* Owner name "AAR" (namesz 4 with the NUL), n_type AAR_NOTE_TYPE. The 40-byte
 * descriptor is ten little-endian uint32_t:
 *   0 magic              AAR_NOTE_MAGIC
 *   1 note_version       AAR_NOTE_VERSION
 *   2 profile            AAR_PROFILE_*
 *   3 api_major          major the app was built against
 *   4 api_minor_required lowest minor that has every call the app needs
 *   5 stack_bytes        0 = 1 MiB, else 64 KiB .. platform maximum, mult. of 16
 *   6 min_heap_mib       private heap committed at load, MiB
 *   7 groups_required    AAR_GROUP_* bits; the load fails with AAR_E_NOGROUP
 *   8 groups_optional    hint
 *   9 reserved           0
 * aar/note.h emits it from C. */
#define AAR_NOTE_MAGIC      0x4E524141u /* bytes 'A','A','R','N' */
#define AAR_NOTE_TYPE       1u
#define AAR_NOTE_VERSION    1u
#define AAR_NOTE_DESC_BYTES 40u
#define AAR_NOTE_DEFAULT_STACK_BYTES (1024u * 1024u)

/* ---- Groups (spec 8.1) --------------------------------------------------------- */

/* A group is present when its bit is set in aar_api.groups and aar_info.groups.
 * If the bit is set, every call of that group at the table's minor is non-NULL.
 * AAR_GROUP_INPUT has no table slots: it adds the input events to event_poll. */
#define AAR_GROUP_DISPLAY  (1u << 0)
#define AAR_GROUP_INPUT    (1u << 1)
#define AAR_GROUP_AUDIO    (1u << 2)
#define AAR_GROUP_GL       (1u << 3)
#define AAR_GROUP_SERVICES (1u << 4)
#define AAR_GROUPS_DEFINED 0x0000001Fu
/* Bits 28 to 31 are reserved for experiments. */
#define AAR_GROUPS_EXPERIMENTAL 0xF0000000u

/* ---- Limits the standard fixes ------------------------------------------------- */

#define AAR_MSG_BYTES            32u      /* every message */
#define AAR_PATH_MAX             255u     /* bytes, excluding the NUL */
#define AAR_NAME_MAX             255u     /* directory entry name, excluding the NUL */
#define AAR_FD_BASE              3        /* lowest file and list id */
#define AAR_MIN_FILES            16u      /* open files, and separately lists */
#define AAR_MIN_ARGS             16u
#define AAR_MIN_ARG_BYTES        128u     /* 127 plus the NUL */
#define AAR_LOG_MAX              1024u    /* longer log records are truncated */
#define AAR_HEAP_GRANULE         (1024u * 1024u)
#define AAR_SHARED_MIN_BLOCKS    64u      /* live app blocks per app, at least */
#define AAR_SHARED_DEFAULT_MIB   16u      /* default shared limit */
#define AAR_STALL_LIMIT_MIN_MS   10000u
#define AAR_MIN_MSG_CAPACITY     32u
#define AAR_DISPLAY_MAX_DIM      4096u    /* width and height, at least */
#define AAR_MIN_DECODE_SESSIONS  4u       /* per kind per app */
#define AAR_SHA256_BYTES         32u

/* ---- aar_info (spec 3.4) ----------------------------------------------------------- */

#define AAR_INFOF_ISOLATED        (1u << 0)
#define AAR_INFOF_SHARED_COHERENT (1u << 1)

/* CPU features for aar_info.cpu_features (spec 8.7). Optional instructions are
 * discovered here, never by executing and trapping. */
#define AAR_CPU_A32_VFPV4  (1u << 0) /* VFPv4 and FMA */
#define AAR_CPU_A32_IDIV   (1u << 1) /* integer divide */
#define AAR_CPU_A32_FP16   (1u << 2) /* half-precision convert */
#define AAR_CPU_A64_CRC32  (1u << 0)
#define AAR_CPU_A64_LSE    (1u << 1) /* LSE atomics */
#define AAR_CPU_A64_AES    (1u << 2)
#define AAR_CPU_A64_SHA    (1u << 3) /* SHA1 and SHA2 */
#define AAR_CPU_A64_DOTPROD (1u << 4)
#define AAR_CPU_A64_FP16   (1u << 5) /* FP16 arithmetic */

/* aar_get_info(): the caller sets `size`; the runtime fills at most that many
 * bytes and zeroes the rest. A field the runtime does not know stays 0. */
typedef struct aar_info {
    uint32_t size;
    uint32_t profile;         /* AAR_PROFILE_* */
    uint32_t platform_id;     /* AAR_PLATFORM_* */
    uint32_t flags;           /* AAR_INFOF_* */
    uint32_t groups;          /* AAR_GROUP_* */
    uint32_t heap_granule;    /* bytes, power of two, 1 MiB in v1 */
    uint32_t shared_granule;  /* bytes, power of two, at least 64 */
    uint32_t msg_capacity;    /* messages per direction, at least 32 (63 on a 64-entry queue) */
    uint32_t sleep_granule_us;
    uint32_t stall_limit_ms;  /* longest gap between calls tolerated, at least 10000 */
    uint32_t stop_grace_ms;
    uint32_t cpu_features;    /* AAR_CPU_A32_* or AAR_CPU_A64_* for the profile */
    uint64_t shared_limit;    /* bytes of shared buffers this app may hold */
    uint64_t shared_used;     /* includes the runtime's service buffers */
    uint32_t max_files;       /* open files, and separately lists, at least 16 */
    uint32_t reserved[5];
} aar_info;

typedef struct aar_heap_info {
    uint64_t committed;       /* private memory currently committed, bytes */
    uint64_t limit;           /* effective limit after the launcher cap */
    uint64_t pool_free;       /* memory still available to this app now (snapshot) */
} aar_heap_info;

/* ---- Shared buffers (spec 4.2, 4.3) ------------------------------------------------ */

/* aar_shared_alloc flags. None are defined in this version: pass 0 (a non-zero
 * value is AAR_E_INVAL). */
#define AAR_SHAREDF_COHERENT (1u << 0) /* output only: aar_shared.flags */

/* Non-pointer fields first; the pointer is at offset 16 on both profiles. */
typedef struct aar_shared {
    uint64_t bytes;           /* granted, multiple of shared_granule */
    uint32_t haddr;           /* host address token, opaque; not a pointer */
    uint32_t flags;           /* AAR_SHAREDF_COHERENT */
    void    *ptr;             /* app mapping */
} aar_shared;

/* ---- Events (spec 8.3) --------------------------------------------------------------- */

#define AAR_EV_KEY_DOWN   1u
#define AAR_EV_KEY_UP     2u
#define AAR_EV_MOUSE      3u
#define AAR_EV_JOY        4u
#define AAR_EV_STOP       5u  /* lifecycle; always delivered */
#define AAR_EV_RESIZE     6u  /* defined, never sent in v1 */
#define AAR_EV_BREAK      7u
#define AAR_EV_LOW_MEMORY 8u  /* lifecycle; always delivered; fields other than type are 0 */

#define AAR_BREAK_CTRL_D 4u
#define AAR_BREAK_CTRL_E 5u
#define AAR_BREAK_CTRL_F 6u

/* 16 bytes on both profiles. code: Amiga raw key (up bit removed), or
 * AAR_BREAK_*; dx, dy: mouse deltas or joystick axes -1, 0, 1; buttons: key
 * qualifiers, mouse buttons in bits 0 to 2 (left, right, middle), or joystick
 * fire in bit 0; time_ms: client timestamp. */
typedef struct aar_event {
    uint16_t type, code;
    int16_t  dx, dy;
    uint16_t buttons, reserved;
    uint32_t time_ms;
} aar_event;

/* ---- Files (spec 7.1) ------------------------------------------------------------------ */

#define AAR_FILE_READ     1u
#define AAR_FILE_WRITE    2u
/* Create the file if it does not exist; an existing file keeps its contents.
 * Needs AAR_FILE_WRITE. */
#define AAR_FILE_CREATE   4u
/* Truncate an existing file to length 0. Needs AAR_FILE_WRITE. The old
 * create-or-replace is AAR_FILE_CREATE | AAR_FILE_TRUNCATE. */
#define AAR_FILE_TRUNCATE 8u
/* With AAR_FILE_CREATE, fail with AAR_E_EXIST when the file exists. */
#define AAR_FILE_EXCL     16u
#define AAR_SEEK_SET 0
#define AAR_SEEK_CUR 1
#define AAR_SEEK_END 2
#define AAR_MODE_FILE 1u
#define AAR_MODE_DIR  2u

typedef struct aar_stat {
    uint64_t size;
    int64_t  mtime;           /* Unix seconds, 0 when the filesystem has none */
    uint32_t mode;            /* AAR_MODE_* */
    uint32_t reserved;
} aar_stat;

typedef struct aar_dirent {
    uint32_t mode;            /* AAR_MODE_* */
    char     name[256];       /* NUL-terminated, at most AAR_NAME_MAX bytes */
} aar_dirent;

/* ---- Display (spec 8.2) -------------------------------------------------------------------- */

/* Logical format requested at open. */
#define AAR_FMT_RGB565   1u
#define AAR_FMT_BGRA8888 2u
#define AAR_FMT_CLUT8    3u /* optional: AAR_DISPCAP_CLUT8 */

/* Physical byte layout of the surface. Store pixels by `layout`, not by the
 * format that was requested. */
#define AAR_LAYOUT_RGB565_BE 1u
#define AAR_LAYOUT_RGB565_LE 2u
#define AAR_LAYOUT_BGR565_BE 3u
#define AAR_LAYOUT_BGR565_LE 4u
#define AAR_LAYOUT_BGRA8888  5u
#define AAR_LAYOUT_RGBA8888  6u
#define AAR_LAYOUT_CLUT8     7u

/* display_open flags. LATEST is optional: without AAR_DISPCAP_LATEST in the
 * granted caps the open fell back to plain AAR_DISPLAY_VSYNC. */
#define AAR_DISPLAY_VSYNC  (1u << 0) /* flip waits for scanout acknowledgement */
#define AAR_DISPLAY_LATEST (1u << 1) /* a flip replaces a pending frame instead of waiting */

/* aar_surface.caps: what this display grants. 16 bits, because the transport
 * carries them in the entry flags field. */
#define AAR_DISPCAP_ASYNC  (1u << 0) /* aar_display_flip_async works */
#define AAR_DISPCAP_LATEST (1u << 1) /* AAR_DISPLAY_LATEST is in effect */
#define AAR_DISPCAP_CLUT8  (1u << 2) /* CLUT8 surfaces and palettes work */

/* Non-pointer fields first; front is at offset 32 on both profiles. front and
 * back are the app mapping, NULL if the app does not draw that buffer. The
 * physical card addresses of zzapp.h 0.x are gone. */
typedef struct aar_surface {
    uint32_t id;
    uint32_t width, height;
    uint32_t pitch;           /* bytes; may exceed width * bytes per pixel */
    uint32_t format;          /* AAR_FMT_* requested */
    uint32_t layout;          /* AAR_LAYOUT_* */
    uint32_t caps;            /* AAR_DISPCAP_* */
    uint32_t reserved;
    void    *front;
    void    *back;
} aar_surface;

/* ---- Audio (spec 8.4) ------------------------------------------------------------------------ */

/* Rates a platform may grant; 22050 is mandatory. Channels: 2. */
#define AAR_AUDIO_RATE_22050 22050u
#define AAR_AUDIO_RATE_44100 44100u
#define AAR_AUDIO_RATE_48000 48000u

/* Interleaved S16LE ring shared by the app and the runtime (not the client).
 * The indices are free-running uint32_t frame counters that wrap; `frames` is a
 * power of two. The app writes ahead of *read_frame but never more than
 * `frames` ahead. Non-pointer fields first; the pointers are at offset 16 on
 * both profiles. */
typedef struct aar_audio {
    uint32_t frames;                 /* capacity, power of two */
    uint32_t rate, channels;         /* granted */
    uint32_t flags;                  /* none defined; 0 */
    int16_t           *samples;
    volatile uint32_t *read_frame;   /* the runtime advances */
    volatile uint32_t *write_frame;  /* the app advances */
} aar_audio;

typedef struct aar_audio_stats {
    uint32_t size;                   /* the caller sets it */
    uint32_t underruns;              /* cumulative since open */
    uint32_t reserved[6];
} aar_audio_stats;

/* ---- Services (spec 8.6) ------------------------------------------------------------------------ */

/* Algorithm and codec numbers are those of zzapp.h 0.x. */
#define AAR_COMPRESS_NONE        0u /* never valid: a bad algorithm */
#define AAR_COMPRESS_DEFLATE_RAW 1u
#define AAR_COMPRESS_ZLIB        2u
#define AAR_COMPRESS_GZIP        3u
#define AAR_COMPRESS_LZ4_BLOCK   4u
#define AAR_COMPRESS_LZMA_ALONE  5u
#define AAR_COMPRESS_LZMA2       6u
#define AAR_COMPRESS_LH1         7u
#define AAR_COMPRESS_LH5         8u
#define AAR_COMPRESS_LH6         9u
#define AAR_COMPRESS_LH7         10u

#define AAR_IMAGE_JPEG 1u
#define AAR_IMAGE_PNG  2u
#define AAR_IMAGE_WEBP 4u

#define AAR_AUDIO_MP3    1u
#define AAR_AUDIO_FLAC   2u
#define AAR_AUDIO_VORBIS 3u

#define AAR_AUDIO_DECODE_EOF   (1u << 0) /* this buffer ends the stream (FLAC, Vorbis) */
#define AAR_AUDIO_DECODE_DRAIN (1u << 1) /* emit buffered PCM without more input */

/* Byte counts of whole buffers are uint64_t. */
typedef struct aar_service_result {
    uint64_t bytes_consumed;
    uint64_t bytes_written;
    uint32_t flags;
    uint32_t reserved;
} aar_service_result;

/* Tile-buffer image decode: tile must hold stride * rows bytes. The only
 * output format is AAR_FMT_BGRA8888. Per-call chunk sizes are uint32_t. The
 * pointer is last, at offset 24 on both profiles. */
typedef struct aar_image_params {
    uint32_t codec;           /* AAR_IMAGE_* */
    uint32_t output_format;   /* AAR_FMT_BGRA8888 */
    uint32_t stride;
    uint32_t rows;
    uint32_t tile_bytes;
    uint32_t flags;
    void    *tile;
} aar_image_params;

typedef struct aar_image_result {
    uint32_t image_width;     /* 0 until the codec has a header */
    uint32_t image_height;
    uint32_t bytes_consumed;
    uint32_t bytes_written;
    uint32_t flags;
} aar_image_result;

typedef struct aar_audio_decode_result {
    uint32_t bytes_consumed;
    uint32_t frames_written;
    uint32_t rate;            /* 0 until the codec has parsed a header */
    uint32_t channels;
} aar_audio_decode_result;

/* ---- The table (spec 3.3) ----------------------------------------------------------------------------- */

/*
 * Slot order, fixed until 1.0:
 *   1. Header: size, version, groups, platform_id (16 bytes).
 *   2. Core, 30 slots, exit through msg_wait. All non-NULL in every runtime.
 *   3. Optional: ext, map_file, unmap_file.
 *   4. Groups, in this order: display, audio, gl, services. (Input has no
 *      slots.) When a group's bit is set in `groups`, all of its slots that
 *      the table's size covers are non-NULL, except where a slot is marked
 *      optional below.
 * A slot exists if `size` covers it (AAR_API_COVERS). A slot that exists and is
 * NULL means "not implemented".
 *
 * Calls park on files, console, display flip, msg_wait and sleep, and return
 * when the work completes, on stop (AAR_E_IO), or on their own timeout. No call
 * is reentrant: an app has one thread. No call takes variable arguments.
 */
typedef struct aar_api {
    uint32_t size;          /* sizeof of the table the runtime built */
    uint32_t version;       /* AAR_API_VERSION of the runtime; do not gate on it */
    uint32_t groups;        /* AAR_GROUP_* the runtime implements */
    uint32_t platform_id;   /* AAR_PLATFORM_*, 0 = unspecified */

    /* -- core: lifecycle and time -- */
    void     (*exit)(int code);
    void     (*yield)(void);
    int      (*should_stop)(void);
    uint64_t (*time_us)(void);
    void     (*sleep_us)(uint32_t us);
    int      (*info)(aar_info *out);

    /* -- core: private heap (spec 4.1) -- */
    void     (*heap_range)(void **base, uint64_t *bytes);
    int      (*heap_grow)(uint64_t bytes, void **old_break);
    int      (*heap_shrink)(uint64_t bytes, void **new_break);
    int      (*heap_info)(aar_heap_info *out);

    /* -- core: shared buffers and visibility (spec 4.2, 4.3) -- */
    int      (*shared_alloc)(uint64_t bytes, uint32_t align, uint32_t flags,
                             aar_shared *out);
    int      (*shared_free)(void *p);
    int      (*publish)(const void *p, uint64_t bytes);
    int      (*acquire)(const void *p, uint64_t bytes);

    /* -- core: log, files, console (spec 7) -- */
    void     (*log)(const char *msg, uint32_t len);
    int      (*file_open)(const char *path, uint32_t flags);
    int      (*file_close)(int fd);
    int      (*file_read)(int fd, void *buf, uint32_t len, uint32_t *out_n);
    int      (*file_write)(int fd, const void *buf, uint32_t len, uint32_t *out_n);
    int      (*file_seek)(int fd, int64_t off, int whence, int64_t *out_pos);
    int      (*file_stat)(const char *path, aar_stat *out);
    int      (*file_list_open)(const char *path);
    int      (*file_list_next)(int list, aar_dirent *out);
    int      (*file_list_close)(int list);
    int      (*console_write)(const void *buf, uint32_t len, uint32_t *out_n);
    int      (*console_read)(void *buf, uint32_t len, uint32_t *out_n);

    /* -- core: events and messages (spec 6, 8.3) -- */
    int      (*event_poll)(aar_event *out, uint32_t max, uint32_t *out_n);
    int      (*msg_send)(const void *msg);      /* exactly AAR_MSG_BYTES */
    int      (*msg_poll)(void *msg);            /* 1, 0, or negative */
    int      (*msg_wait)(uint32_t timeout_us);

    /* -- optional -- */
    const void *(*ext)(uint32_t platform_id, uint32_t ext_id);
    int      (*map_file)(const char *path, const void **data, uint64_t *bytes);
    void     (*unmap_file)(const void *data);

    /* -- group AAR_GROUP_DISPLAY -- */
    int      (*display_open)(uint32_t w, uint32_t h, uint32_t format,
                             uint32_t flags, aar_surface *out);
    int      (*display_flip)(uint32_t id);
    int      (*display_flip_async)(uint32_t id);   /* only if AAR_DISPCAP_ASYNC */
    int      (*display_flip_idle)(void);
    int      (*display_refresh)(aar_surface *s);
    int      (*display_vsync)(void);
    int      (*display_set_palette)(uint32_t id, uint32_t first, uint32_t count,
                                    const uint32_t *rgb);
    int      (*display_close)(uint32_t id);
    uint32_t (*display_latest_dropped)(void);      /* only if AAR_DISPCAP_LATEST */

    /* -- group AAR_GROUP_AUDIO -- */
    int      (*audio_open)(uint32_t rate, uint32_t channels, aar_audio *out);
    int      (*audio_close)(void);
    int      (*audio_stats)(aar_audio_stats *out); /* optional: may be NULL */

    /* -- group AAR_GROUP_GL -- */
    const aar_gl *(*gl_bind)(uint32_t surface_id);
    void     (*gl_swap)(void);
    void     (*gl_unbind)(void);

    /* -- group AAR_GROUP_SERVICES: every call may return AAR_E_NOSYS for an
     *    algorithm or codec the platform does not provide -- */
    int      (*sha256)(const void *data, uint64_t bytes, uint8_t digest[32]);
    int      (*decompress)(uint32_t algorithm, uint32_t flags,
                           const void *src, uint64_t src_bytes,
                           void *dst, uint64_t dst_bytes,
                           aar_service_result *out);
    int      (*image_open)(const aar_image_params *params, uint32_t *session,
                           aar_image_result *out);
    int      (*image_decode)(uint32_t session, const void *src,
                             uint32_t src_bytes, uint32_t flags,
                             aar_image_result *out);
    int      (*image_close)(uint32_t session);
    int      (*audio_decode_open)(uint32_t codec, uint32_t *session);
    int      (*audio_decode)(uint32_t session, const void *src,
                             uint32_t src_bytes, uint32_t flags,
                             int16_t *pcm, uint32_t pcm_frames,
                             aar_audio_decode_result *out);
    int      (*audio_decode_close)(uint32_t session);
} aar_api;

/* True when size covers field. A NULL function pointer still means absent. */
#define AAR_API_COVERS(api, field) \
    ((api) != 0 && (uint32_t)(api)->size >= \
        (uint32_t)(offsetof(aar_api, field) + sizeof((api)->field)))

/* ---- Byte order helpers (spec 1.3) -------------------------------------------------------------------------- */

/* The standard never swaps bytes for you. Values that an app and its client
 * exchange through a message or shared buffer are the business of their
 * protocol; these swap a value when the protocol says big-endian. */
static inline uint16_t aar_bswap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t aar_bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) |
           (v << 24);
}

static inline uint64_t aar_bswap64(uint64_t v)
{
    return ((uint64_t)aar_bswap32((uint32_t)v) << 32) |
           aar_bswap32((uint32_t)(v >> 32));
}

/* ---- Entry contract and the wrappers libaar defines -------------------------------------------------------- */

/*
 * crt0 stores the table pointer (r0 on A32, x0 on A64) here before it runs
 * the init arrays and calls aar_main. The table is const; the pointer is not,
 * but apps must not replace it.
 */
extern const aar_api *aar_api_ptr;

/* The app defines this. crt0 passes its result to exit (libc flavor) or
 * aar_exit (freestanding flavor). */
int aar_main(int argc, char **argv);

/* The wrappers gate on AAR_API_COVERS and a non-NULL slot. A missing slot
 * returns AAR_E_NOSYS; a void or pointer wrapper returns nothing, 0 or NULL.
 * aar_exit does not return even then. The info calls are aar_get_info and
 * aar_get_heap_info because aar_info and aar_heap_info are type names. */

void     aar_exit(int code);
void     aar_yield(void);
int      aar_should_stop(void);
uint64_t aar_time_us(void);      /* monotonic, platform epoch, not wall-clock */
void     aar_sleep_us(uint32_t us);
int      aar_get_info(aar_info *out);

void     aar_heap_range(void **base, uint64_t *bytes);
/* Whole heap_granule units. Returns the old end, which must equal the previous
 * end. New memory is zeroed and existing pointers never move. */
int      aar_heap_grow(uint64_t bytes, void **old_break);
int      aar_heap_shrink(uint64_t bytes, void **new_break);
int      aar_get_heap_info(aar_heap_info *out);

/* bytes and align are rounded up to shared_granule. A shared buffer that the
 * runtime itself uses for the console, the audio ring or file payloads is
 * charged to shared_used and to the shared limit but does not count toward the
 * AAR_SHARED_MIN_BLOCKS the app is promised. */
int      aar_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                          aar_shared *out);
int      aar_shared_free(void *p);
/* Writer: make my stores visible to the host, then send the message that
 * announces the data. Reader: after the message arrives, call aar_acquire
 * before reading. A granule has one writer at a time. */
int      aar_publish(const void *p, uint64_t bytes);
int      aar_acquire(const void *p, uint64_t bytes);

void     aar_log(const char *msg, uint32_t len);
int      aar_file_open(const char *path, uint32_t flags);
int      aar_file_close(int fd);
int      aar_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n);
int      aar_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n);
int      aar_file_seek(int fd, int64_t off, int whence, int64_t *out_pos);
int      aar_file_stat(const char *path, aar_stat *out);
int      aar_file_list_open(const char *path);
int      aar_file_list_next(int list, aar_dirent *out);
int      aar_file_list_close(int list);
/* console_write writes every byte or returns an error. console_read parks
 * until the client has input and returns AAR_OK with *out_n == 0 at the end of
 * input. */
int      aar_console_write(const void *buf, uint32_t len, uint32_t *out_n);
int      aar_console_read(void *buf, uint32_t len, uint32_t *out_n);

/* Never blocks. Always returns AAR_EV_STOP and AAR_EV_LOW_MEMORY; with
 * AAR_GROUP_INPUT also the input events. */
int      aar_event_poll(aar_event *out, uint32_t max, uint32_t *out_n);
/* Exactly AAR_MSG_BYTES. aar_msg_send returns AAR_E_BUSY when the queue is
 * full and never blocks. aar_msg_poll returns 1 and copies one message, 0 when
 * empty, or a negative code. aar_msg_wait returns AAR_OK with the message
 * still queued, AAR_E_TIMEDOUT, or AAR_E_IO on stop; timeout 0 only tests. */
int      aar_msg_send(const void *msg);
int      aar_msg_poll(void *msg);
int      aar_msg_wait(uint32_t timeout_us);

const void *aar_ext(uint32_t platform_id, uint32_t ext_id);
int      aar_map_file(const char *path, const void **data, uint64_t *bytes);
void     aar_unmap_file(const void *data);

int      aar_display_open(uint32_t w, uint32_t h, uint32_t format,
                          uint32_t flags, aar_surface *out);
int      aar_display_flip(uint32_t id);
int      aar_display_flip_async(uint32_t id);
/* 1 when the last queued flip has been shown, 0 while pending, negative on
 * error. */
int      aar_display_flip_idle(void);
int      aar_display_refresh(aar_surface *s);
int      aar_display_vsync(void);
int      aar_display_set_palette(uint32_t id, uint32_t first, uint32_t count,
                                 const uint32_t *rgb);
int      aar_display_close(uint32_t id);
/* Cumulative frames replaced in AAR_DISPLAY_LATEST mode; 0 when not active. */
uint32_t aar_display_latest_dropped(void);

int      aar_audio_open(uint32_t rate, uint32_t channels, aar_audio *out);
int      aar_audio_close(void);
int      aar_audio_get_stats(aar_audio_stats *out);

const aar_gl *aar_gl_bind(uint32_t surface_id);
void     aar_gl_swap(void);
void     aar_gl_unbind(void);

int      aar_sha256(const void *data, uint64_t bytes, uint8_t digest[32]);
int      aar_decompress(uint32_t algorithm, uint32_t flags,
                        const void *src, uint64_t src_bytes,
                        void *dst, uint64_t dst_bytes,
                        aar_service_result *out);
int      aar_image_open(const aar_image_params *params, uint32_t *session,
                        aar_image_result *out);
int      aar_image_decode(uint32_t session, const void *src, uint32_t src_bytes,
                          uint32_t flags, aar_image_result *out);
int      aar_image_close(uint32_t session);
int      aar_audio_decode_open(uint32_t codec, uint32_t *session);
int      aar_audio_decode(uint32_t session, const void *src, uint32_t src_bytes,
                          uint32_t flags, int16_t *pcm, uint32_t pcm_frames,
                          aar_audio_decode_result *out);
int      aar_audio_decode_close(uint32_t session);

#ifndef AAR_FREESTANDING
/* Private allocator, libc flavor only (layer 2 of libaar). Shared memory is
 * aar_shared_alloc. A freestanding build (AAR_FREESTANDING) has no malloc and
 * does not declare these: allocate from aar_heap_range and aar_heap_grow. */
void    *aar_alloc(uint64_t bytes, uint32_t align);
void     aar_free(void *p);
#endif

/* ---- Layout checks (hold on ILP32 and LP64) ----------------------------------------------------------------- */

#if defined(__cplusplus)
#define AAR_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#define AAR_ALIGNOF(type) alignof(type)
#else
#define AAR_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define AAR_ALIGNOF(type) _Alignof(type)
#endif

/* Size of a structure made of `fixed` bytes of fixed-width fields followed by
 * `nptr` pointers, rounded to the structure's own alignment. */
#define AAR_ABI_SIZE(type, fixed, nptr) \
    ((((size_t)(fixed) + (size_t)(nptr) * sizeof(void *)) + \
      AAR_ALIGNOF(type) - 1u) & ~(AAR_ALIGNOF(type) - 1u))

/* Number of pointer slots in the table after the 16-byte header. */
#define AAR_API_SLOT_COUNT 56u
#define AAR_API_CORE_SLOT_COUNT 30u

AAR_STATIC_ASSERT(sizeof(aar_info) == 88, "aar_info size");
AAR_STATIC_ASSERT(offsetof(aar_info, shared_limit) == 48, "aar_info.shared_limit");
AAR_STATIC_ASSERT(offsetof(aar_info, shared_used) == 56, "aar_info.shared_used");
AAR_STATIC_ASSERT(offsetof(aar_info, max_files) == 64, "aar_info.max_files");
AAR_STATIC_ASSERT(offsetof(aar_info, reserved) == 68, "aar_info.reserved");

AAR_STATIC_ASSERT(sizeof(aar_heap_info) == 24, "aar_heap_info size");
AAR_STATIC_ASSERT(offsetof(aar_heap_info, limit) == 8, "aar_heap_info.limit");
AAR_STATIC_ASSERT(offsetof(aar_heap_info, pool_free) == 16, "aar_heap_info.pool_free");

AAR_STATIC_ASSERT(offsetof(aar_shared, haddr) == 8, "aar_shared.haddr");
AAR_STATIC_ASSERT(offsetof(aar_shared, flags) == 12, "aar_shared.flags");
AAR_STATIC_ASSERT(offsetof(aar_shared, ptr) == 16, "aar_shared.ptr");
AAR_STATIC_ASSERT(sizeof(aar_shared) == AAR_ABI_SIZE(aar_shared, 16, 1), "aar_shared size");

AAR_STATIC_ASSERT(sizeof(aar_event) == 16, "aar_event size");
AAR_STATIC_ASSERT(offsetof(aar_event, code) == 2, "aar_event.code");
AAR_STATIC_ASSERT(offsetof(aar_event, dx) == 4, "aar_event.dx");
AAR_STATIC_ASSERT(offsetof(aar_event, dy) == 6, "aar_event.dy");
AAR_STATIC_ASSERT(offsetof(aar_event, buttons) == 8, "aar_event.buttons");
AAR_STATIC_ASSERT(offsetof(aar_event, reserved) == 10, "aar_event.reserved");
AAR_STATIC_ASSERT(offsetof(aar_event, time_ms) == 12, "aar_event.time_ms");

AAR_STATIC_ASSERT(sizeof(aar_stat) == 24, "aar_stat size");
AAR_STATIC_ASSERT(offsetof(aar_stat, mtime) == 8, "aar_stat.mtime");
AAR_STATIC_ASSERT(offsetof(aar_stat, mode) == 16, "aar_stat.mode");
AAR_STATIC_ASSERT(sizeof(aar_dirent) == 260, "aar_dirent size");
AAR_STATIC_ASSERT(offsetof(aar_dirent, name) == 4, "aar_dirent.name");

AAR_STATIC_ASSERT(offsetof(aar_surface, caps) == 24, "aar_surface.caps");
AAR_STATIC_ASSERT(offsetof(aar_surface, front) == 32, "aar_surface.front");
AAR_STATIC_ASSERT(offsetof(aar_surface, back) == 32 + sizeof(void *), "aar_surface.back");
AAR_STATIC_ASSERT(sizeof(aar_surface) == AAR_ABI_SIZE(aar_surface, 32, 2), "aar_surface size");

AAR_STATIC_ASSERT(offsetof(aar_audio, rate) == 4, "aar_audio.rate");
AAR_STATIC_ASSERT(offsetof(aar_audio, flags) == 12, "aar_audio.flags");
AAR_STATIC_ASSERT(offsetof(aar_audio, samples) == 16, "aar_audio.samples");
AAR_STATIC_ASSERT(offsetof(aar_audio, read_frame) == 16 + sizeof(void *), "aar_audio.read_frame");
AAR_STATIC_ASSERT(offsetof(aar_audio, write_frame) == 16 + 2 * sizeof(void *), "aar_audio.write_frame");
AAR_STATIC_ASSERT(sizeof(aar_audio) == AAR_ABI_SIZE(aar_audio, 16, 3), "aar_audio size");
AAR_STATIC_ASSERT(sizeof(aar_audio_stats) == 32, "aar_audio_stats size");

AAR_STATIC_ASSERT(sizeof(aar_service_result) == 24, "aar_service_result size");
AAR_STATIC_ASSERT(offsetof(aar_service_result, flags) == 16, "aar_service_result.flags");
AAR_STATIC_ASSERT(offsetof(aar_image_params, flags) == 20, "aar_image_params.flags");
AAR_STATIC_ASSERT(offsetof(aar_image_params, tile) == 24, "aar_image_params.tile");
AAR_STATIC_ASSERT(sizeof(aar_image_params) == AAR_ABI_SIZE(aar_image_params, 24, 1), "aar_image_params size");
AAR_STATIC_ASSERT(sizeof(aar_image_result) == 20, "aar_image_result size");
AAR_STATIC_ASSERT(sizeof(aar_audio_decode_result) == 16, "aar_audio_decode_result size");

AAR_STATIC_ASSERT(offsetof(aar_api, platform_id) == 12, "aar_api header is 16 bytes");
AAR_STATIC_ASSERT(offsetof(aar_api, exit) == 16, "aar_api first slot at offset 16");
AAR_STATIC_ASSERT(offsetof(aar_api, ext) == 16 + AAR_API_CORE_SLOT_COUNT * sizeof(void *),
                  "aar_api core slots precede ext");
AAR_STATIC_ASSERT(offsetof(aar_api, msg_wait) == 16 + (AAR_API_CORE_SLOT_COUNT - 1) * sizeof(void *),
                  "aar_api core ends at msg_wait");
AAR_STATIC_ASSERT(sizeof(aar_api) == 16 + AAR_API_SLOT_COUNT * sizeof(void *),
                  "aar_api has no padding");

#ifdef __cplusplus
}
#endif

#endif
