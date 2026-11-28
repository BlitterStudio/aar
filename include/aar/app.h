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
 * The specification is spec/aar.md in the aar repository. Pre-1.0, any minor
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
#define AAR_API_MINOR 6u
/* The oldest minor whose table layout this header describes. Before 1.0 a
 * minor may move slots (0.6 did), so a runtime built from this header refuses
 * a note with an api_minor_required below this value as well as one above
 * AAR_API_MINOR (spec 3.6). */
#define AAR_API_MINOR_OLDEST 6u
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
 * return AAR_OK or a negative value. */
#define AAR_OK           0
#define AAR_E_IO        -1  /* failed; control block or queue corrupt; wait cancelled by stop */
#define AAR_E_INVAL     -2  /* bad argument, unknown flag, unknown format, zero or oversize length, unknown id */
#define AAR_E_NOMEM     -3  /* memory, quota, table or session limit */
#define AAR_E_BUSY      -4  /* queue full, resource in use, flip pending */
#define AAR_E_NOENT     -5  /* no such file, or no further directory entry */
#define AAR_E_ACCES     -6  /* refused by the client's policy */
#define AAR_E_NOSYS     -7  /* slot absent or NULL; codec or format not implemented */
#define AAR_E_TIMEDOUT  -8  /* file wait expired, or msg_wait timeout */
#define AAR_E_FAULT     -9  /* bad pointer in a call (isolated runtimes only) */
#define AAR_E_NOSPC    -10  /* a result does not fit */
#define AAR_E_EXIST    -11  /* the file exists and AAR_FILE_EXCL was set */
#define AAR_E_BADF     -12  /* bad id, or an id used for a direction it was not opened for */
#define AAR_E_PROFILE  -13  /* image profile or note not acceptable on this platform */
#define AAR_E_NOGROUP  -14  /* image requires a group the runtime lacks */
#define AAR_E_NORESOURCE -15 /* admission or load refused; the reason is an AAR_REASON_* code */
#define AAR_E_STALE    -16  /* a handle generation or async ticket is no longer current */
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
/* Bits 5 to 27 are reserved for later versions of the standard: a platform
 * must not set them. Bits 28 to 31 are for experiments of a platform. */
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
#define AAR_EV_RESIZE     6u  /* sent only with AAR_DISPCAP_RESIZE: dx, dy = new inner size, pixels */
#define AAR_EV_BREAK      7u
#define AAR_EV_LOW_MEMORY 8u  /* lifecycle; always delivered; fields other than type are 0 */
/* Minor 6. An app MUST ignore an event type it does not know. */
#define AAR_EV_TEXT          9u  /* code: Unicode code point 1..0xFFFF; buttons: qualifiers. Only with AAR_DISPCAP_TEXT_INPUT */
#define AAR_EV_WHEEL        10u  /* dx: horizontal steps (+ right), dy: vertical steps (+ down, towards the user); buttons: qualifiers. Sent when the client can produce it; the wheel is then not sent as raw keys */
#define AAR_EV_CLOSE_REQUEST 11u /* the close gadget was used; only with AAR_DISPCAP_CLOSE_REQUEST, which then replaces the stop that gadget sends */
#define AAR_EV_ACTIVE       12u  /* code 1: the display gained input focus, 0: lost it. Sent when the client can produce it */

/* AAR_EV_MOUSE: code is 0 for motion only, or the button whose state changed
 * (1 left, 2 right, 3 middle). buttons holds the state of all buttons. */
#define AAR_MOUSE_LEFT     (1u << 0)
#define AAR_MOUSE_RIGHT    (1u << 1)
#define AAR_MOUSE_MIDDLE   (1u << 2)
/* In buttons: dx and dy are the pointer position relative to the surface's
 * top-left pixel, not deltas. Set on every AAR_EV_MOUSE of a display granted
 * AAR_DISPCAP_POINTER_ABS; the position may be outside the surface while a
 * button is held. */
#define AAR_MOUSE_ABSOLUTE (1u << 15)

/* AAR_EV_JOY: code is the port (0 and 1 are the Amiga game ports; 2 and up
 * are other devices that the client numbers); dx, dy are the axes; buttons: */
#define AAR_JOY_BUTTON1 (1u << 0) /* fire; CD32 red */
#define AAR_JOY_BUTTON2 (1u << 1) /* second button; CD32 blue */
#define AAR_JOY_BUTTON3 (1u << 2) /* third button; CD32 green */
#define AAR_JOY_BUTTON4 (1u << 3) /* CD32 yellow */
#define AAR_JOY_PLAY    (1u << 4) /* CD32 play/pause */
#define AAR_JOY_FORWARD (1u << 5) /* CD32 right shoulder */
#define AAR_JOY_REVERSE (1u << 6) /* CD32 left shoulder */

#define AAR_BREAK_CTRL_D 4u
#define AAR_BREAK_CTRL_E 5u
#define AAR_BREAK_CTRL_F 6u

/* 16 bytes on both profiles. code: Amiga raw key (up bit removed), AAR_BREAK_*,
 * the mouse button that changed, the joystick port, or a code point (TEXT);
 * dx, dy: mouse deltas or position, joystick axes -1, 0, 1, wheel steps, or
 * the new size (RESIZE); buttons: key qualifiers, AAR_MOUSE_* bits or
 * AAR_JOY_* bits; time_ms: a timestamp in milliseconds that the client
 * chooses. */
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

/* file_rename flags. */
#define AAR_RENAME_REPLACE (1u << 0) /* delete an existing target first; not atomic on AmigaDOS */

/* Protection bits, at the AmigaDOS FIBF_* positions. Bits 0 to 3 are set to
 * DENY, as in AmigaDOS: a set AAR_PROT_WRITE forbids writing. */
#define AAR_PROT_DELETE  (1u << 0)
#define AAR_PROT_EXECUTE (1u << 1)
#define AAR_PROT_WRITE   (1u << 2)
#define AAR_PROT_READ    (1u << 3)
#define AAR_PROT_ARCHIVE (1u << 4)
#define AAR_PROT_PURE    (1u << 5)
#define AAR_PROT_SCRIPT  (1u << 6)
#define AAR_PROT_HOLD    (1u << 7)

#define AAR_FILE_COMMENT_MAX 79u /* native bytes, excluding any terminator */

#define AAR_VOLF_READONLY (1u << 0) /* aar_volume_info.flags */

typedef struct aar_stat {
    uint64_t size;
    int64_t  mtime;           /* Unix seconds, 0 when the filesystem has none */
    uint32_t mode;            /* AAR_MODE_* */
    uint32_t protect;         /* AAR_PROT_* */
} aar_stat;

typedef struct aar_dirent {
    uint32_t mode;            /* AAR_MODE_* */
    char     name[256];       /* NUL-terminated, at most AAR_NAME_MAX bytes */
} aar_dirent;

/* aar_get_volume_info(): the caller sets `size`; the runtime fills at most
 * that many bytes and zeroes the rest. */
typedef struct aar_volume_info {
    uint32_t size;
    uint32_t block_bytes;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t flags;           /* AAR_VOLF_* */
    uint32_t reserved[3];
} aar_volume_info;

/* ---- Host environment (spec 7.3, 7.4) ---------------------------------------------------------------- */

/* aar_get_locale_info(): the caller sets `size`; the runtime fills at most
 * that many bytes and zeroes the rest. */
#define AAR_LOCALEF_OFFSET_KNOWN (1u << 0) /* utc_offset_min is the user's setting */

typedef struct aar_locale_info {
    uint32_t size;
    int32_t  utc_offset_min;  /* minutes EAST of UTC (locale.library loc_GMTOffset is minutes west: negate) */
    uint32_t flags;           /* AAR_LOCALEF_* */
    char     language[32];    /* NUL-terminated Amiga language name without ".language", e.g. "english", "deutsch"; "" unknown */
    char     country[8];      /* NUL-terminated vehicle code: the non-zero bytes of loc_CountryCode, most significant first, e.g. "D", "GB"; "" unknown */
    char     decimal_point[4];   /* UTF-8, NUL-terminated */
    char     group_separator[4]; /* UTF-8, NUL-terminated */
    uint32_t reserved[3];
} aar_locale_info;

/* Clipboard types and units (aar_clip_read, aar_clip_write). */
#define AAR_CLIP_TEXT  1u   /* UTF-8 text; the client converts to and from IFF FTXT */
#define AAR_CLIP_IFF   2u   /* one IFF FORM, passed through unchanged */
#define AAR_CLIP_UNITS 256u /* units 0..255 as clipboard.device; 0 is the primary clipboard */

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
/* Flags of minor 6: requests; the granted caps say what took effect. */
#define AAR_DISPLAY_WINDOW        (1u << 2) /* show the surface in a window on the default public screen */
#define AAR_DISPLAY_RESIZABLE     (1u << 3) /* the window may be resized; needs WINDOW */
#define AAR_DISPLAY_POINTER_ABS   (1u << 4) /* mouse events carry the pointer position */
#define AAR_DISPLAY_TEXT_INPUT    (1u << 5) /* send AAR_EV_TEXT */
#define AAR_DISPLAY_CLOSE_REQUEST (1u << 6) /* send AAR_EV_CLOSE_REQUEST for the close gadget */
#define AAR_DISPLAY_SCALE         (1u << 7) /* the client scales the surface, keeping the aspect ratio */

/* aar_surface.caps: what this display grants. 16 bits, because the transport
 * carries them in the entry flags field. */
#define AAR_DISPCAP_ASYNC  (1u << 0) /* aar_display_flip_async works */
#define AAR_DISPCAP_LATEST (1u << 1) /* AAR_DISPLAY_LATEST is in effect */
#define AAR_DISPCAP_CLUT8  (1u << 2) /* CLUT8 surfaces and palettes work */
#define AAR_DISPCAP_WINDOW        (1u << 3)  /* the surface is in a window */
#define AAR_DISPCAP_RESIZE        (1u << 4)  /* AAR_EV_RESIZE is sent and aar_display_resize works */
#define AAR_DISPCAP_POINTER_ABS   (1u << 5)  /* mouse events are absolute */
#define AAR_DISPCAP_TEXT_INPUT    (1u << 6)  /* AAR_EV_TEXT is sent */
#define AAR_DISPCAP_CLOSE_REQUEST (1u << 7)  /* AAR_EV_CLOSE_REQUEST is sent */
#define AAR_DISPCAP_SCALED        (1u << 8)  /* the client scales the surface */
#define AAR_DISPCAP_PRESENT_FEEDBACK (1u << 9) /* tracked presents are supported */

/* Flags for aar_display_present(). With neither flag, flip synchronously. */
#define AAR_PRESENT_ASYNC (1u << 0) /* asynchronous buffer flip */
#define AAR_PRESENT_GL    (1u << 1) /* swap GL; do not combine with ASYNC */

#define AAR_PRESENT_ACKNOWLEDGED 1u
#define AAR_PRESENT_REPLACED     2u
#define AAR_PRESENT_UNCONFIRMED  3u

#define AAR_PRESENT_REASON_NONE      0u
#define AAR_PRESENT_REASON_TIMEOUT   1u
#define AAR_PRESENT_REASON_CANCELLED 2u
#define AAR_PRESENT_REASON_BACKEND   3u

typedef struct aar_present_result {
    uint64_t frame_id;      /* opaque; unique within this app instance */
    uint64_t completed_us;  /* aar_time_us() clock, not a scanout timestamp */
    uint32_t outcome;       /* AAR_PRESENT_* result */
    uint32_t reason;        /* AAR_PRESENT_REASON_* */
} aar_present_result;

/* Non-pointer fields first; front is at offset 32 on both profiles. front and
 * back are the app mapping, NULL if the app does not draw that buffer. */
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

/* aar_display_get_info(): the caller sets `size`; the runtime fills at most
 * that many bytes and zeroes the rest. */
#define AAR_DISPINFO_WINDOW (1u << 0) /* shown in a window on a public screen */
#define AAR_DISPINFO_SCALED (1u << 1) /* the client scales the surface */
#define AAR_DISPINFO_ACTIVE (1u << 2) /* the window or screen has input focus */

typedef struct aar_display_info {
    uint32_t size;
    uint32_t width, height;   /* current surface size */
    uint32_t refresh_mhz;     /* refresh rate of the mode shown, millihertz; 0 unknown */
    uint32_t screen_width, screen_height; /* the Amiga screen the surface is on */
    uint32_t flags;           /* AAR_DISPINFO_* */
    uint32_t reserved[5];
} aar_display_info;

#define AAR_MODEF_PREFERRED (1u << 0) /* aar_display_mode.flags: the client's preferred mode (native size) */

typedef struct aar_display_mode {
    uint32_t width, height;
    uint32_t refresh_mhz;     /* 0 unknown */
    uint32_t formats;         /* bit (1u << AAR_FMT_*) for each format this mode can show */
    uint32_t flags;           /* AAR_MODEF_* */
    uint32_t reserved;
} aar_display_mode;

/* aar_display_pointer() modes. */
#define AAR_POINTER_DEFAULT 0u
#define AAR_POINTER_HIDDEN  1u
#define AAR_POINTER_BUSY    2u
#define AAR_POINTER_CUSTOM  3u /* image required */
#define AAR_POINTER_MAX_DIM 64u

/* Non-pointer fields first; argb is at offset 24 on both profiles. */
typedef struct aar_pointer {
    uint32_t width, height;   /* 1..AAR_POINTER_MAX_DIM each */
    int32_t  hot_x, hot_y;    /* hot spot inside the image */
    uint32_t reserved[2];
    const uint32_t *argb;     /* width * height pixels, 0xAARRGGBB in host order; alpha 0 is transparent */
} aar_pointer;

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

/* Algorithm and codec numbers. AAR_COMPRESS_NONE is never valid. */
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
 *   2. Core, 42 slots, exit through clip_write. All non-NULL in every
 *      runtime. A call that the client serves may still return AAR_E_NOSYS
 *      when the client does not serve it.
 *   3. Optional: ext, map_file, unmap_file.
 *   4. Groups, in this order: display, audio, gl, services. (Input has no
 *      slots.) When a group's bit is set in `groups`, all of its slots that
 *      the table's size covers are non-NULL, except where a slot is marked
 *      optional below.
 *   5. Optional asynchronous file slots.
 * A slot exists if `size` covers it (AAR_API_COVERS). A slot that exists and is
 * NULL means "not implemented".
 *
 * Calls park on synchronous files, console, display flip and msg_wait, and return when the
 * work completes, on stop (AAR_E_IO), or on their own timeout. A sleep is also
 * ended by a stop and has no return value. No call is reentrant: an app has one
 * thread. No call takes variable arguments.
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

    /* -- core: files, minor 6 (spec 7.1) -- */
    int      (*file_delete)(const char *path);
    int      (*file_rename)(const char *from, const char *to, uint32_t flags); /* AAR_RENAME_* */
    int      (*dir_create)(const char *path);
    int      (*file_set_date)(const char *path, int64_t mtime);   /* Unix seconds, UTC */
    int      (*file_set_protect)(const char *path, uint32_t protect); /* AAR_PROT_* */
    int      (*file_truncate)(int fd, uint64_t size);
    int      (*volume_info)(const char *path, aar_volume_info *out);

    /* -- core: host environment, minor 6 (spec 7.3, 7.4) -- */
    int      (*time_unix_us)(int64_t *out_us);
    int      (*locale_info)(aar_locale_info *out);
    int      (*env_get)(const char *name, char *buf, uint32_t bytes,
                        uint32_t *out_n);
    int      (*clip_read)(uint32_t unit, uint32_t type, void *buf,
                          uint32_t bytes, uint32_t *out_n);
    int      (*clip_write)(uint32_t unit, uint32_t type, const void *data,
                           uint32_t bytes);

    /* -- optional -- */
    const void *(*ext)(uint32_t platform_id, uint32_t ext_id);
    int      (*map_file)(const char *path, const void **data, uint64_t *bytes);
    void     (*unmap_file)(const void *data);
    int      (*file_get_comment)(const char *path, void *buf, uint32_t bytes,
                                 uint32_t *out_n);
    int      (*file_set_comment)(const char *path, const void *data, uint32_t bytes);

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
    int      (*display_info)(uint32_t id, aar_display_info *out);
    int      (*display_modes)(aar_display_mode *out, uint32_t max,
                              uint32_t *out_n);
    int      (*display_resize)(uint32_t id, uint32_t w, uint32_t h,
                               aar_surface *out);   /* only with AAR_DISPCAP_RESIZE */
    int      (*display_set_title)(uint32_t id, const char *title);
    int      (*display_pointer)(uint32_t id, uint32_t mode,
                                const aar_pointer *image);   /* AAR_POINTER_* */
    int      (*display_wait_vblank)(uint32_t timeout_us);
    uint64_t (*display_last_present_us)(void);
    int      (*display_present)(uint32_t id, uint32_t flags, uint64_t *frame_id);
    int      (*display_present_poll)(aar_present_result *out);

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

    /* -- optional: async files (spec 7.1) -- */
    int      (*file_read_async)(int fd, void *buf, uint32_t len, uint32_t *ticket);
    int      (*file_write_async)(int fd, const void *buf, uint32_t len, uint32_t *ticket);
    int      (*file_collect)(uint32_t ticket, int32_t *status, uint32_t *out_n);
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
uint64_t aar_time_us(void);      /* monotonic, platform epoch, not wall-clock; see aar_time_unix_us */
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
/* Nonblocking, optional. A successful submission returns a nonzero
 * ticket and borrows buf until a terminal collection. Only one ticket per app.
 * file_collect returns AAR_E_BUSY while pending. On AAR_OK it consumes the ticket
 * and returns the transfer status and confirmed byte count through the outputs. */
int      aar_file_read_async(int fd, void *buf, uint32_t len, uint32_t *ticket);
int      aar_file_write_async(int fd, const void *buf, uint32_t len, uint32_t *ticket);
int      aar_file_collect(uint32_t ticket, int32_t *status, uint32_t *out_n);
int      aar_file_seek(int fd, int64_t off, int whence, int64_t *out_pos);
int      aar_file_stat(const char *path, aar_stat *out);
int      aar_file_list_open(const char *path);
int      aar_file_list_next(int list, aar_dirent *out);
int      aar_file_list_close(int list);
/* Minor 6. Paths follow the rules and policy of aar_file_open. */
int      aar_file_delete(const char *path);
int      aar_file_rename(const char *from, const char *to, uint32_t flags);
int      aar_dir_create(const char *path);
int      aar_file_set_date(const char *path, int64_t mtime);
int      aar_file_set_protect(const char *path, uint32_t protect);
int      aar_file_truncate(int fd, uint64_t size);
int      aar_get_volume_info(const char *path, aar_volume_info *out);
/* Copies up to bytes native comment bytes; *out_n is the full length.
 * A short buffer returns AAR_E_NOSPC. No NUL terminator is written. */
int      aar_file_get_comment(const char *path, void *buf, uint32_t bytes,
                              uint32_t *out_n);
/* Replaces the comment. Zero bytes clears it; embedded NUL is invalid. */
int      aar_file_set_comment(const char *path, const void *data, uint32_t bytes);
/* Microseconds since 1970-01-01 UTC. */
int      aar_time_unix_us(int64_t *out_us);
int      aar_get_locale_info(aar_locale_info *out);
/* *out_n is the length of the value without the NUL; AAR_E_NOSPC with *out_n
 * set when bytes <= *out_n. */
int      aar_env_get(const char *name, char *buf, uint32_t bytes,
                     uint32_t *out_n);
/* *out_n is the full size of the clip in that type; AAR_E_NOSPC when it did
 * not fit. */
int      aar_clip_read(uint32_t unit, uint32_t type, void *buf, uint32_t bytes,
                       uint32_t *out_n);
int      aar_clip_write(uint32_t unit, uint32_t type, const void *data,
                        uint32_t bytes);
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
/* Minor 6. */
int      aar_display_get_info(uint32_t id, aar_display_info *out);
int      aar_display_modes(aar_display_mode *out, uint32_t max, uint32_t *out_n);
/* After AAR_EV_RESIZE; *out is refreshed. Only with AAR_DISPCAP_RESIZE. */
int      aar_display_resize(uint32_t id, uint32_t w, uint32_t h, aar_surface *out);
int      aar_display_set_title(uint32_t id, const char *title);
int      aar_display_pointer(uint32_t id, uint32_t mode, const aar_pointer *image);
/* AAR_OK at the next vertical blank; AAR_E_TIMEDOUT after timeout_us (not 0). */
int      aar_display_wait_vblank(uint32_t timeout_us);
/* The aar_time_us value at which the last flip or GL swap was shown; 0 before
 * the first or when the call is absent. */
uint64_t aar_display_last_present_us(void);
/* Submit with feedback. On acceptance returns AAR_OK and a nonzero ID.
 * A rejected submission sets *frame_id to 0 and leaves the frame unsubmitted. */
int      aar_display_present(uint32_t id, uint32_t flags, uint64_t *frame_id);
/* Returns 1 and consumes a result, 0 if none is ready, or a negative error.
 * May be called after display close. Does not block or pump display events. */
int      aar_display_present_poll(aar_present_result *out);

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
#define AAR_API_SLOT_COUNT 82u
#define AAR_API_CORE_SLOT_COUNT 42u

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
AAR_STATIC_ASSERT(offsetof(aar_stat, protect) == 20, "aar_stat.protect");
AAR_STATIC_ASSERT(sizeof(aar_dirent) == 260, "aar_dirent size");
AAR_STATIC_ASSERT(offsetof(aar_dirent, name) == 4, "aar_dirent.name");
AAR_STATIC_ASSERT(sizeof(aar_volume_info) == 40, "aar_volume_info size");
AAR_STATIC_ASSERT(offsetof(aar_volume_info, block_bytes) == 4, "aar_volume_info.block_bytes");
AAR_STATIC_ASSERT(offsetof(aar_volume_info, total_bytes) == 8, "aar_volume_info.total_bytes");
AAR_STATIC_ASSERT(offsetof(aar_volume_info, free_bytes) == 16, "aar_volume_info.free_bytes");
AAR_STATIC_ASSERT(offsetof(aar_volume_info, flags) == 24, "aar_volume_info.flags");
AAR_STATIC_ASSERT(offsetof(aar_volume_info, reserved) == 28, "aar_volume_info.reserved");
AAR_STATIC_ASSERT(sizeof(aar_locale_info) == 72, "aar_locale_info size");
AAR_STATIC_ASSERT(offsetof(aar_locale_info, utc_offset_min) == 4, "aar_locale_info.utc_offset_min");
AAR_STATIC_ASSERT(offsetof(aar_locale_info, flags) == 8, "aar_locale_info.flags");
AAR_STATIC_ASSERT(offsetof(aar_locale_info, language) == 12, "aar_locale_info.language");
AAR_STATIC_ASSERT(offsetof(aar_locale_info, country) == 44, "aar_locale_info.country");
AAR_STATIC_ASSERT(offsetof(aar_locale_info, decimal_point) == 52, "aar_locale_info.decimal_point");
AAR_STATIC_ASSERT(offsetof(aar_locale_info, group_separator) == 56, "aar_locale_info.group_separator");
AAR_STATIC_ASSERT(offsetof(aar_locale_info, reserved) == 60, "aar_locale_info.reserved");

AAR_STATIC_ASSERT(offsetof(aar_surface, caps) == 24, "aar_surface.caps");
AAR_STATIC_ASSERT(offsetof(aar_surface, front) == 32, "aar_surface.front");
AAR_STATIC_ASSERT(offsetof(aar_surface, back) == 32 + sizeof(void *), "aar_surface.back");
AAR_STATIC_ASSERT(sizeof(aar_surface) == AAR_ABI_SIZE(aar_surface, 32, 2), "aar_surface size");
AAR_STATIC_ASSERT(offsetof(aar_present_result, completed_us) == 8, "aar_present_result.completed_us");
AAR_STATIC_ASSERT(offsetof(aar_present_result, outcome) == 16, "aar_present_result.outcome");
AAR_STATIC_ASSERT(offsetof(aar_present_result, reason) == 20, "aar_present_result.reason");
AAR_STATIC_ASSERT(sizeof(aar_present_result) == 24, "aar_present_result size");

AAR_STATIC_ASSERT(sizeof(aar_display_info) == 48, "aar_display_info size");
AAR_STATIC_ASSERT(offsetof(aar_display_info, refresh_mhz) == 12, "aar_display_info.refresh_mhz");
AAR_STATIC_ASSERT(offsetof(aar_display_info, screen_width) == 16, "aar_display_info.screen_width");
AAR_STATIC_ASSERT(offsetof(aar_display_info, flags) == 24, "aar_display_info.flags");
AAR_STATIC_ASSERT(offsetof(aar_display_info, reserved) == 28, "aar_display_info.reserved");
AAR_STATIC_ASSERT(sizeof(aar_display_mode) == 24, "aar_display_mode size");
AAR_STATIC_ASSERT(offsetof(aar_display_mode, refresh_mhz) == 8, "aar_display_mode.refresh_mhz");
AAR_STATIC_ASSERT(offsetof(aar_display_mode, formats) == 12, "aar_display_mode.formats");
AAR_STATIC_ASSERT(offsetof(aar_display_mode, flags) == 16, "aar_display_mode.flags");
AAR_STATIC_ASSERT(offsetof(aar_pointer, hot_x) == 8, "aar_pointer.hot_x");
AAR_STATIC_ASSERT(offsetof(aar_pointer, reserved) == 16, "aar_pointer.reserved");
AAR_STATIC_ASSERT(offsetof(aar_pointer, argb) == 24, "aar_pointer.argb");
AAR_STATIC_ASSERT(sizeof(aar_pointer) == AAR_ABI_SIZE(aar_pointer, 24, 1), "aar_pointer size");

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
AAR_STATIC_ASSERT(offsetof(aar_api, clip_write) == 16 + (AAR_API_CORE_SLOT_COUNT - 1) * sizeof(void *),
                  "aar_api core ends at clip_write");
AAR_STATIC_ASSERT(offsetof(aar_api, file_delete) ==
                      offsetof(aar_api, msg_wait) + sizeof(void *),
                  "aar_api minor 6 core slots follow msg_wait");
AAR_STATIC_ASSERT(sizeof(aar_api) == 16 + AAR_API_SLOT_COUNT * sizeof(void *),
                  "aar_api has no padding");
AAR_STATIC_ASSERT(offsetof(aar_api, display_info) ==
                      offsetof(aar_api, display_latest_dropped) + sizeof(void *),
                  "aar_api minor 6 display slots follow display_latest_dropped");
AAR_STATIC_ASSERT(offsetof(aar_api, audio_open) ==
                      offsetof(aar_api, display_present_poll) + sizeof(void *),
                  "aar_api audio group follows the display group");
AAR_STATIC_ASSERT(offsetof(aar_api, file_read_async) ==
                      offsetof(aar_api, audio_decode_close) + sizeof(void *),
                  "aar_api async files follow the service group");
AAR_STATIC_ASSERT(offsetof(aar_api, file_collect) ==
                      16 + (AAR_API_SLOT_COUNT - 1) * sizeof(void *),
                  "aar_api ends at file_collect");

#ifdef __cplusplus
}
#endif

#endif
