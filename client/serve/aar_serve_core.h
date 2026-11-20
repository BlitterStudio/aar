/*
 * The OS-free core of aar_serve: the handlers of the app's file, console, display,
 * audio and event requests, and the policy decisions the serving loop makes. No
 * AmigaOS calls: the DOS, Intuition, P96 and AHI calls sit behind the operation
 * tables, so a host test can run the handlers against a fake request stream.
 * The types of a request and a completion and the console and display tables
 * are in libraries/aar_serve.h.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_SERVE_CORE_H
#define AAR_SERVE_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <libraries/aar.h>
#include <libraries/aar_serve.h>

#define AAR_SERVE_GRACE_MS 500u

static inline uint32_t aar_serve_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void aar_serve_put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

/* Launcher diagnostics share the asynchronous app-output writer. Keep space
 * for status/error text when a stalled destination has filled diagnostics. */
#define AAR_SERVE_LAUNCHER_OUTPUT_BYTES 8192u
#define AAR_SERVE_LAUNCHER_STATUS_RESERVE_BYTES 2048u

#define AAR_SERVE_OUTPUT_NONE 0
#define AAR_SERVE_OUTPUT_LAUNCHER 1
#define AAR_SERVE_OUTPUT_APP 2

static inline int aar_serve_launcher_output_accept(uint32_t used, uint32_t bytes,
                                                   int diagnostic)
{
    uint32_t limit = diagnostic ? AAR_SERVE_LAUNCHER_OUTPUT_BYTES -
                                  AAR_SERVE_LAUNCHER_STATUS_RESERVE_BYTES :
                                  AAR_SERVE_LAUNCHER_OUTPUT_BYTES;

    return used <= limit && bytes <= limit - used;
}

/* A launcher message already staged before a drain precedes app bytes that
 * have not yet been submitted to the shared asynchronous writer. */
static inline int aar_serve_output_next_source(uint32_t launcher_bytes,
                                               uint32_t app_bytes)
{
    if (launcher_bytes)
        return AAR_SERVE_OUTPUT_LAUNCHER;
    return app_bytes ? AAR_SERVE_OUTPUT_APP : AAR_SERVE_OUTPUT_NONE;
}

#if AAR_SERVE_LAUNCHER_STATUS_RESERVE_BYTES >= AAR_SERVE_LAUNCHER_OUTPUT_BYTES
#error "launcher status reserve must leave diagnostic space"
#endif

/* console_read_finish: never stdout. */
#define AAR_SERVE_FINISH_NONE 0
#define AAR_SERVE_FINISH_RETURN_ON_READ_HANDLE 1
#define AAR_SERVE_FINISH_WAIT_ON_USER_CONSOLE 2

struct aar_serve_exit_in {
    uint32_t state;
    uint32_t end_reason;
    uint32_t elapsed_ms;
    int lost;          /* the claim's handle went stale: not this launcher's */
    int entered;
    int stop_asked;
    int user_stop;
    int output_pending;
    int output_failed;
};

/* A failed drain must not keep the launcher alive. Pending output still
 * delays a clean exit so the app's text can be written. */
static inline int aar_serve_output_blocks_exit(int output_pending, int output_failed)
{
    return output_pending && !output_failed;
}

static inline int aar_serve_run_should_exit(const struct aar_serve_exit_in *in)
{
    int blocked;

    if (!in)
        return 0;
    /* A stale handle belongs to nobody this launcher can serve. */
    if (in->lost)
        return 1;
    blocked = aar_serve_output_blocks_exit(in->output_pending, in->output_failed);
    /* Ctrl-C ends the process once both grace periods have elapsed, even
     * when console output is still queued. The faulted path must not skip
     * this: that was the hang. */
    if (in->user_stop && in->elapsed_ms >= AAR_SERVE_GRACE_MS * 2u)
        return 1;
    if ((in->state == AAR_STATE_FAULTED ||
         in->end_reason == AAR_END_RUNTIME_RESTART) &&
        !blocked)
        return 1;
    if (in->entered &&
        (in->state == AAR_STATE_IDLE ||
         (in->state == AAR_STATE_STOPPING && in->stop_asked &&
          in->elapsed_ms >= AAR_SERVE_GRACE_MS)) &&
        (!in->stop_asked || in->elapsed_ms >= AAR_SERVE_GRACE_MS) &&
        !blocked)
        return 1;
    if (in->stop_asked && in->elapsed_ms >= AAR_SERVE_GRACE_MS * 2u &&
        !blocked)
        return 1;
    return 0;
}

/* A CONSOLE_READ request is the demand for input. It starts a DOS read only
 * while the app is RUNNING and no stop was asked: a forced unwind can leave
 * the request outstanding after the app is gone. */
static inline int aar_serve_console_read_allowed(uint32_t state, int stop_asked)
{
    return state == AAR_STATE_RUNNING && !stop_asked;
}

static inline int aar_serve_finish_prompt(int pending, int interactive_read)
{
    if (!pending)
        return AAR_SERVE_FINISH_NONE;
    if (interactive_read)
        return AAR_SERVE_FINISH_RETURN_ON_READ_HANDLE;
    return AAR_SERVE_FINISH_WAIT_ON_USER_CONSOLE;
}

/* Completions that find the completion queue full wait in a backlog of
 * AAR_SERVE_REPLY_BACKLOG entries. A request is taken from the submission queue
 * only while AAR_SERVE_REPLY_RESERVE entries stay free for the completions of
 * requests already taken and still outstanding (a console write, a console
 * read and a display flip), so a completion is never dropped for want of room;
 * the app's request then waits in the queue until the backlog drains. */
#define AAR_SERVE_REPLY_BACKLOG 16u
#define AAR_SERVE_REPLY_RESERVE 8u

static inline int aar_serve_reply_may_take_request(uint32_t backlog_used)
{
    return backlog_used + AAR_SERVE_REPLY_RESERVE < AAR_SERVE_REPLY_BACKLOG;
}

/* Amiga raw key C is 0x33 (0x31 Z, 0x32 X, 0x33 C; 0x20 is A). The key still
 * goes to the app; this decides only whether aar_serve also requests its
 * documented stop. */
static inline int aar_serve_rawkey_requests_stop(uint16_t code, uint16_t qualifiers,
                                                 uint16_t control_qualifier)
{
    return (qualifiers & control_qualifier) != 0 && code == 0x33u;
}

/* A DOS write may advance the consumer only by the completed byte count. A
 * short completion is an output failure after that partial advance. */
static inline uint32_t aar_serve_console_write_advance(uint32_t requested,
                                                    int32_t written)
{
    if (written <= 0 || (uint32_t)written > requested)
        return 0;
    return (uint32_t)written;
}

static inline int aar_serve_console_write_failed(uint32_t requested,
                                                 int32_t written)
{
    return written <= 0 || (uint32_t)written != requested;
}

/* Each successful ChangeScreenBuffer has one SafeMessage before the old
 * buffer and its reply port may be released. */
static inline uint32_t aar_serve_screen_safe_started(uint32_t pending)
{
    return pending + 1u;
}

static inline uint32_t aar_serve_screen_safe_completed(uint32_t pending)
{
    return pending ? pending - 1u : 0u;
}

/* A raw DOS packet has two independent outcomes: delivery to the handler and
 * the handler's DOS result. Callers must never treat a transport failure as a
 * successful zero/one DOS result, and must preserve dp_Res2 as IoErr(). */
static inline int aar_serve_dos_boolean_result(int transport_ok, int32_t result)
{
    return transport_ok && result != 0;
}

static inline int32_t aar_serve_dos_error_code(int transport_ok, int32_t result2,
                                               int32_t transport_error)
{
    return transport_ok ? result2 : transport_error;
}

/* Async DOS packet wait policy: only the reply ends the wait -- aar_serve keeps
 * the packet and its buffers alive until then, even after Ctrl-C or losing
 * the claim; those only mark the result abandoned. */
static inline int aar_serve_packet_abandoned(int ctrl_c, int claim_live)
{
    return ctrl_c || !claim_live;
}

static inline int aar_serve_packet_keep_waiting(int reply_arrived)
{
    return !reply_arrived;
}

/* Completion and abandonment are independent: handlers can allocate an
 * object before aar_serve observes Ctrl-C or a lost claim. */
static inline int aar_serve_packet_result_usable(int completed, int abandoned)
{
    return completed && !abandoned;
}

/* A packet is sent only while its operation is live. A lost claim or a
 * pending Ctrl-C means the result will be discarded, but the handler would
 * still act on the packet first (create, truncate, write), so such a packet
 * stays unsent. Releases (ACTION_END, ACTION_FREE_LOCK) always go out: they
 * give back what an earlier packet acquired. */
static inline int aar_serve_packet_may_send(int release, int ownership_ok,
                                            int ctrl_c_pending)
{
    return release || (ownership_ok && !ctrl_c_pending);
}

/* A resource slot must survive a release packet that was never delivered;
 * after delivery DOS owns no resource and its local handle may be freed even
 * when the handler reported an error. The callback sends ACTION_END or
 * ACTION_FREE_LOCK; fallback closes/unlocks after an undelivered packet. */
struct aar_serve_dos_release_ops {
    void *context;
    int (*send_release)(void *context, int32_t *result);
    void (*free_local)(void *context);
    void (*fallback_release)(void *context);
    void (*forget_slot)(void *context);
};

int aar_serve_dos_close_slot(const struct aar_serve_dos_release_ops *ops,
                             int *delivered_out);
void aar_serve_dos_end_or_close(const struct aar_serve_dos_release_ops *ops);
void aar_serve_dos_release_abandoned(const struct aar_serve_dos_release_ops *ops,
                                     int acquired);

/* Direct DOS packets receive the path relative to the lock selected by
 * GetDeviceProc(): strip only the device/assign prefix. */
static inline const char *aar_serve_dos_packet_path(const char *name)
{
    const char *p;

    if (!name)
        return NULL;
    for (p = name; *p; p++) {
        if (*p == ':')
            return p + 1;
    }
    return name;
}

static inline uint32_t aar_serve_bstr_encode(const char *name, uint8_t *out,
                                             uint32_t cap);

struct aar_serve_dos_path_candidate {
    uintptr_t handler;
    uint32_t directory;
};

struct aar_serve_dos_path_reply {
    int completed;
    int abandoned;
    int32_t result;
    int32_t error;
};

typedef int (*aar_serve_dos_path_resolve_fn)(
    void *context, int advance, struct aar_serve_dos_path_candidate *candidate);
typedef void (*aar_serve_dos_path_send_fn)(
    void *context, const struct aar_serve_dos_path_candidate *candidate,
    const uint8_t *name_bstr, uint32_t name_bstr_bytes,
    uint32_t name_bstr_bptr, struct aar_serve_dos_path_reply *reply);

/* Send an operation to GetDeviceProc candidates. name_bstr is public,
 * longword-aligned storage that remains valid until the sender replies. A
 * failed lookup only tries the next multi-assign directory for
 * ERROR_OBJECT_NOT_FOUND; a colonless name is sent unchanged to the
 * current-directory candidate. selected_out (optional) receives the
 * candidate of the last packet sent: the one that decided the outcome. A
 * caller that must act on that same object afterwards (probe, then open)
 * pins its resolver to this candidate instead of resolving the name again,
 * which would restart at the first directory of a multi-directory assign. */
static inline int aar_serve_dos_dispatch_path(
    const char *name, uint8_t *name_bstr, uint32_t name_bstr_capacity,
    int32_t object_not_found, aar_serve_dos_path_resolve_fn resolve,
    void *resolve_context, aar_serve_dos_path_send_fn send, void *send_context,
    int *attempted_out, int *usable_out,
    struct aar_serve_dos_path_candidate *selected_out)
{
    const char *path = aar_serve_dos_packet_path(name);
    struct aar_serve_dos_path_candidate candidate;
    struct aar_serve_dos_path_reply reply;
    uint32_t name_bstr_bytes;
    uint32_t name_bstr_bptr;
    int advance = 0;

    if (attempted_out)
        *attempted_out = 0;
    if (usable_out)
        *usable_out = 0;
    if (!path || !name_bstr || ((uintptr_t)name_bstr & 3u) != 0u ||
        !resolve || !send)
        return 0;
    name_bstr_bytes = aar_serve_bstr_encode(path, name_bstr, name_bstr_capacity);
    if (name_bstr_bytes == 0u)
        return 0;
    name_bstr_bptr = (uint32_t)((uintptr_t)name_bstr >> 2);
    while (resolve(resolve_context, advance, &candidate)) {
        if (usable_out)
            *usable_out = 0;
        if (selected_out)
            *selected_out = candidate;
        memset(&reply, 0, sizeof(reply));
        send(send_context, &candidate, name_bstr, name_bstr_bytes,
             name_bstr_bptr, &reply);
        if (!reply.completed || reply.abandoned)
            return 0;
        if (attempted_out)
            *attempted_out = 1;
        if (usable_out)
            *usable_out = 1;
        if (reply.result != 0)
            return 1;
        if (reply.error != object_not_found)
            return 0;
        advance = 1;
    }
    return 0;
}

/* DOS packet name arguments are BCPL strings (BSTR): a length byte, the
 * characters, padded to an even total. Returns the encoded byte count, or 0
 * when the name is longer than a BSTR can carry or does not fit the buffer. */
static inline uint32_t aar_serve_bstr_encode(const char *name, uint8_t *out,
                                             uint32_t cap)
{
    uint32_t len;
    uint32_t i;
    uint32_t total;

    if (!name || !out)
        return 0;
    len = 0;
    while (name[len]) {
        if (len >= 255u)
            return 0;
        len++;
    }
    total = 1u + len;
    if (total & 1u)
        total++;
    if (cap < total)
        return 0;
    out[0] = (uint8_t)len;
    for (i = 0; i < len; i++)
        out[1u + i] = (uint8_t)name[i];
    if (!(len & 1u))
        out[1u + len] = 0;
    return total;
}

/* ACTION_EXAMINE_OBJECT/NEXT return a BCPL name in the FIB, not the C string
 * dos.library's Examine/ExNext wrappers return. No trailing NUL is required. */
static inline uint32_t aar_serve_bstr_decode(const uint8_t *name, uint32_t bytes,
                                             char *out, uint32_t cap)
{
    uint32_t n = bytes ? name[0] : 0u;

    if (cap == 0u)
        return 0u;
    if (bytes && n > bytes - 1u)
        n = bytes - 1u;
    if (n >= cap)
        n = cap - 1u;
    memcpy(out, name + (bytes != 0u), n);
    out[n] = 0;
    return n;
}

/* A Ctrl-C seen while a DOS packet is pending must still reach the app as a
 * stop request; abandoning only the packet's result loses the stop. */
static inline int aar_serve_ctrl_c_must_request_stop(int ctrl_c)
{
    return ctrl_c;
}

/* A break counts as a user stop when it is still pending or when a DOS
 * packet wait already consumed it and recorded user_stop. */
static inline int aar_serve_user_break(int ctrl_c_pending, int user_stop)
{
    return ctrl_c_pending != 0 || user_stop != 0;
}

/* How aar_serve opens a file for one valid flags word (AAR_FILE_*). Zero is not
 * a plan: the flags are invalid and aar_serve answers EINVAL before it touches
 * AmigaDOS. */
enum aar_serve_open_plan {
    AAR_SERVE_OPEN_INVALID = 0,
    /* READ only: MODE_OLDFILE, the file must exist. */
    AAR_SERVE_OPEN_READ_EXISTING,
    /* WRITE without CREATE: the file must exist (Lock probe, because
     * MODE_READWRITE creates a missing file); open it MODE_READWRITE. */
    AAR_SERVE_OPEN_UPDATE_EXISTING,
    /* TRUNCATE without CREATE: as above, then SetFileSize(0). Handlers that
     * lack ACTION_SET_FILE_SIZE get MODE_NEWFILE after the probe. */
    AAR_SERVE_OPEN_TRUNCATE_EXISTING,
    /* CREATE only: MODE_READWRITE on the existing file, else MODE_NEWFILE.
     * An existing file keeps its contents. */
    AAR_SERVE_OPEN_CREATE_KEEP,
    /* CREATE|TRUNCATE: MODE_NEWFILE (create or replace). */
    AAR_SERVE_OPEN_CREATE_REPLACE,
    /* CREATE|EXCL, with or without TRUNCATE: Lock probe, EEXIST if the file
     * exists, else MODE_NEWFILE. */
    AAR_SERVE_OPEN_CREATE_EXCLUSIVE
};

static inline enum aar_serve_open_plan aar_serve_open_plan_for_flags(uint32_t flags)
{
    const uint32_t known = AAR_FILE_READ | AAR_FILE_WRITE | AAR_FILE_CREATE |
                           AAR_FILE_TRUNCATE | AAR_FILE_EXCL;
    int create = (flags & AAR_FILE_CREATE) != 0;
    int truncate = (flags & AAR_FILE_TRUNCATE) != 0;
    int write = (flags & AAR_FILE_WRITE) != 0;

    if (flags & ~known)
        return AAR_SERVE_OPEN_INVALID;
    if (!(flags & (AAR_FILE_READ | AAR_FILE_WRITE)))
        return AAR_SERVE_OPEN_INVALID;
    if ((create || truncate) && !write)
        return AAR_SERVE_OPEN_INVALID;
    if ((flags & AAR_FILE_EXCL) && !create)
        return AAR_SERVE_OPEN_INVALID;
    if (flags & AAR_FILE_EXCL)
        return AAR_SERVE_OPEN_CREATE_EXCLUSIVE;
    if (create)
        return truncate ? AAR_SERVE_OPEN_CREATE_REPLACE : AAR_SERVE_OPEN_CREATE_KEEP;
    if (truncate)
        return AAR_SERVE_OPEN_TRUNCATE_EXISTING;
    return write ? AAR_SERVE_OPEN_UPDATE_EXISTING : AAR_SERVE_OPEN_READ_EXISTING;
}

/* Outcome of the existence probe, a Lock(name, ACCESS_READ). A refused lock
 * with ERROR_OBJECT_IN_USE still proves the object exists: the handler holds
 * it for someone else. Any other failure is the caller's error. */
enum aar_serve_probe_result {
    AAR_SERVE_PROBE_ERROR = 0,
    AAR_SERVE_PROBE_EXISTS,
    AAR_SERVE_PROBE_MISSING
};

static inline enum aar_serve_probe_result aar_serve_probe_result_for_lock(
    int locked, int32_t io_error, int32_t object_not_found,
    int32_t object_in_use)
{
    if (locked)
        return AAR_SERVE_PROBE_EXISTS;
    if (io_error == object_not_found)
        return AAR_SERVE_PROBE_MISSING;
    if (io_error == object_in_use)
        return AAR_SERVE_PROBE_EXISTS;
    return AAR_SERVE_PROBE_ERROR;
}

/* Which plans probe the target before the open packet. UPDATE, TRUNCATE and
 * EXCL decide on the answer. CREATE (keep or replace) acts on an existing
 * file whichever directory of a multi-directory assign holds it, so an
 * absolute path probes to find that directory; a relative path already has
 * one fixed parent lock. After an EXISTS answer every later packet of the
 * open (update, truncate, NEWFILE fallback) goes to the probe's candidate;
 * a MISSING answer leaves the name unresolved for the creating open. */
static inline int aar_serve_plan_probes_target(enum aar_serve_open_plan plan,
                                               int absolute)
{
    switch (plan) {
    case AAR_SERVE_OPEN_UPDATE_EXISTING:
    case AAR_SERVE_OPEN_TRUNCATE_EXISTING:
    case AAR_SERVE_OPEN_CREATE_EXCLUSIVE:
        return 1;
    case AAR_SERVE_OPEN_CREATE_KEEP:
    case AAR_SERVE_OPEN_CREATE_REPLACE:
        return absolute != 0;
    default:
        return 0;
    }
}

/* What a failed Lock of a relative name's final component means for one
 * plan. The lock also proves the leaf stays under the ROOT when it succeeds,
 * so a refusal ends the open unless it already answers the plan's question:
 * a missing leaf is what a create plan wants, and an exclusive create must
 * report a leaf that exists but is held by another program (the handler
 * refuses the lock with ERROR_OBJECT_IN_USE) as EEXIST, the same answer an
 * absolute name gets from its probe. Nothing is opened on the EXISTS answer,
 * so no open bypasses the ROOT check. */
enum aar_serve_leaf_action {
    AAR_SERVE_LEAF_REFUSE = 0,
    AAR_SERVE_LEAF_PROCEED,
    AAR_SERVE_LEAF_EXISTS
};

static inline enum aar_serve_leaf_action aar_serve_leaf_lock_failure(
    enum aar_serve_open_plan plan, int32_t io_error, int32_t object_not_found,
    int32_t object_in_use)
{
    int create = plan == AAR_SERVE_OPEN_CREATE_KEEP ||
                 plan == AAR_SERVE_OPEN_CREATE_REPLACE ||
                 plan == AAR_SERVE_OPEN_CREATE_EXCLUSIVE;

    switch (aar_serve_probe_result_for_lock(0, io_error, object_not_found,
                                            object_in_use)) {
    case AAR_SERVE_PROBE_MISSING:
        return create ? AAR_SERVE_LEAF_PROCEED : AAR_SERVE_LEAF_REFUSE;
    case AAR_SERVE_PROBE_EXISTS:
        return plan == AAR_SERVE_OPEN_CREATE_EXCLUSIVE ? AAR_SERVE_LEAF_EXISTS :
                                                     AAR_SERVE_LEAF_REFUSE;
    default:
        return AAR_SERVE_LEAF_REFUSE;
    }
}

/* A step that creates, truncates or replaces a file runs only while the
 * request is live (the claim is still served) and no stop was asked.
 * user_stop records a Ctrl-C that an earlier packet wait consumed, so a
 * cancelled request stays cancelled for the steps that follow it. */
static inline int aar_serve_open_step_allowed(int request_current,
                                              int ctrl_c_pending, int user_stop)
{
    return request_current && !aar_serve_user_break(ctrl_c_pending, user_stop);
}

/* Completes a request that a handler left deferred. */
struct aar_serve_sink {
    void *ctx;
    void (*complete)(void *ctx, const struct aar_serve_req *req,
                     const struct aar_serve_done *done);
};

#define AAR_SERVE_REQ_DONE     0 /* the completion is in *done */
#define AAR_SERVE_REQ_DEFERRED 1 /* the handler completes through the sink */

/* ---- DOS error numbers -------------------------------------------------- */

/* AmigaDOS IoErr() values the handlers branch on. aar_serve.c asserts each
 * against dos/dos.h. */
#define AAR_SERVE_IOERR_NO_FREE_STORE    103
#define AAR_SERVE_IOERR_OBJECT_IN_USE    202
#define AAR_SERVE_IOERR_OBJECT_EXISTS    203
#define AAR_SERVE_IOERR_DIR_NOT_FOUND    204
#define AAR_SERVE_IOERR_OBJECT_NOT_FOUND 205
#define AAR_SERVE_IOERR_ACTION_NOT_KNOWN 209
#define AAR_SERVE_IOERR_OBJECT_TOO_LARGE 207
#define AAR_SERVE_IOERR_OBJECT_WRONG_TYPE 212
#define AAR_SERVE_IOERR_DEVICE_NOT_MOUNTED 218
#define AAR_SERVE_IOERR_SEEK_ERROR       219
#define AAR_SERVE_IOERR_DISK_FULL        221
#define AAR_SERVE_IOERR_DELETE_PROTECTED 222
#define AAR_SERVE_IOERR_WRITE_PROTECTED  223
#define AAR_SERVE_IOERR_READ_PROTECTED   224
#define AAR_SERVE_IOERR_NO_MORE_ENTRIES  232
#define AAR_SERVE_IOERR_BREAK            304

/* An AmigaDOS error as the AAR_E_* code the app sees; 0 maps to AAR_OK. */
int32_t aar_serve_errno_from_io(int32_t io_error);

/* Seconds from the Amiga epoch (1978) to the Unix epoch (1970). */
#define AAR_SERVE_UNIX_DELTA 252460800UL

/* DateStamp fields as Unix seconds, 0 for an unset stamp. */
uint32_t aar_serve_amiga_unix(uint32_t days, uint32_t minutes, uint32_t ticks);

/* ---- Files -------------------------------------------------------------- */

#define AAR_SERVE_FD_COUNT AAR_MIN_FILES
#define AAR_SERVE_IO_CHUNK 8192u
#define AAR_SERVE_PATH_BYTES (AAR_PATH_MAX + 1u)

enum aar_serve_mode {
    AAR_SERVE_MODE_OLD = 0,  /* MODE_OLDFILE */
    AAR_SERVE_MODE_READWRITE, /* MODE_READWRITE */
    AAR_SERVE_MODE_NEW        /* MODE_NEWFILE */
};

struct aar_serve_info {
    int is_dir;
    uint32_t size;
    uint32_t mtime; /* Unix seconds */
    char name[AAR_NAME_MAX + 1u];
};

/*
 * The DOS operations the file handlers use. Locks, file handles and list
 * objects are opaque non-zero values; 0 is "none". Every call that can fail
 * reports *transport_ok = 0 when the DOS packet was never delivered or its
 * result was abandoned, and otherwise the DOS result with the IoErr() value
 * in *io.
 */
struct aar_serve_dos_ops {
    void *ctx;
    /* The request is still wanted: the claim is served and no stop was
     * asked in a way that cancels it. */
    int (*live)(void *ctx);
    /* A Ctrl-C is pending, or one was consumed by an earlier packet wait. */
    int (*user_break)(void *ctx);

    uintptr_t (*dup_lock)(void *ctx, uintptr_t lock);
    void (*unlock)(void *ctx, uintptr_t lock);
    uintptr_t (*parent_dir)(void *ctx, uintptr_t lock);
    int (*same_lock)(void *ctx, uintptr_t a, uintptr_t b);
    /* ACCESS_READ lock of name relative to dir. */
    uintptr_t (*lock_rel)(void *ctx, uintptr_t dir, const char *name,
                          int *transport_ok, int32_t *io);
    /* ACCESS_READ lock of an absolute (device:path) name, through every
     * GetDeviceProc candidate. */
    uintptr_t (*lock_abs)(void *ctx, const char *name, int *transport_ok,
                          int32_t *io);
    int (*lock_examine)(void *ctx, uintptr_t lock, struct aar_serve_info *info,
                        int *transport_ok, int32_t *io);

    /* One open acts on one target. dir is the parent lock of a relative
     * name, or 0 for an absolute name that GetDeviceProc resolves; the
     * probe pins the resolver to the candidate that holds the file, so
     * every later packet of the open reaches that file. */
    void (*target_begin)(void *ctx, uintptr_t dir, const char *name);
    /* Returns 1 with *exists set, or 0 with *transport_ok and *io. */
    int (*target_probe)(void *ctx, int *exists, int *transport_ok,
                        int32_t *io);
    uintptr_t (*target_open)(void *ctx, enum aar_serve_mode mode,
                             int *transport_ok, int32_t *io);
    void (*target_end)(void *ctx);

    /* File handles. Reads and writes return the byte count, or -1. */
    int32_t (*fh_set_size0)(void *ctx, uintptr_t fh, int *transport_ok,
                            int32_t *io);
    int32_t (*fh_read)(void *ctx, uintptr_t fh, uint8_t *dst, uint32_t bytes,
                       int *transport_ok, int32_t *io);
    int32_t (*fh_write)(void *ctx, uintptr_t fh, const uint8_t *src,
                        uint32_t bytes, int *transport_ok, int32_t *io);
    /* 0 = OFFSET_BEGINNING, 1 = OFFSET_CURRENT, 2 = OFFSET_END. Returns the
     * previous position, or -1. */
    int32_t (*fh_seek)(void *ctx, uintptr_t fh, int32_t offset, int whence,
                       int *transport_ok, int32_t *io);
    /* ACTION_END, falling back to Close() when it was never delivered. */
    void (*fh_end_or_close)(void *ctx, uintptr_t fh);
    /* ACTION_END for an open slot. *delivered says whether DOS got it; only
     * then is the handle gone. Returns the handler's boolean result. */
    int (*fh_close)(void *ctx, uintptr_t fh, int *delivered, int32_t *io);

    /* A list takes over lock when list_open succeeds. */
    int (*list_open)(void *ctx, uintptr_t lock, uintptr_t *list,
                     int *transport_ok, int32_t *io);
    int (*list_next)(void *ctx, uintptr_t list, struct aar_serve_info *info,
                     int *transport_ok, int32_t *io);
    void (*list_close)(void *ctx, uintptr_t list);
};

struct aar_serve_files {
    const struct aar_serve_dos_ops *dos;
    uintptr_t root;
    int abs_paths;
    uintptr_t fh[AAR_SERVE_FD_COUNT];
    uintptr_t list[AAR_SERVE_FD_COUNT];
    uint8_t io[AAR_SERVE_IO_CHUNK];
};

void aar_serve_files_init(struct aar_serve_files *files, const struct aar_serve_dos_ops *dos,
                          uintptr_t root, int abs_paths);
/* Closes every open file and list. */
void aar_serve_files_close_all(struct aar_serve_files *files);
/* Serves one AAR_OP_FILE_* or AAR_OP_LIST_* request. */
void aar_serve_files_serve(struct aar_serve_files *files, const struct aar_serve_req *req,
                           struct aar_serve_done *done);

/* ---- Console ------------------------------------------------------------ */

#define AAR_SERVE_CONSOLE_READ_BYTES  256u
#define AAR_SERVE_CONSOLE_WRITE_BYTES 1024u

struct aar_serve_console {
    struct aar_serve_console_ops ops;
    const struct aar_serve_sink *sink;

    uint8_t launcher[AAR_SERVE_LAUNCHER_OUTPUT_BYTES];
    uint32_t launcher_head;
    uint32_t launcher_tail;
    uint32_t launcher_used;
    uint32_t launcher_dropped;

    int write_pending;
    int write_source;
    uint32_t write_bytes;
    int app_write_active; /* app_write holds an outstanding CONSOLE_WRITE */
    struct aar_serve_req app_write;
    uint32_t app_write_done;

    int read_active; /* read_req holds an outstanding CONSOLE_READ */
    int read_pending; /* its DOS read was started */
    struct aar_serve_req read_req;
    uint32_t read_max;
    int eof;
    int failed; /* output could not be written */
};

void aar_serve_console_init(struct aar_serve_console *c,
                            const struct aar_serve_console_ops *ops,
                            const struct aar_serve_sink *sink);
/* Queues launcher text. A diagnostic that does not fit is dropped and
 * counted; a plain status message is never dropped for want of the reserve. */
void aar_serve_console_launcher_put(struct aar_serve_console *c, const uint8_t *data,
                                    uint32_t bytes, int diagnostic);
void aar_serve_console_launcher_status(struct aar_serve_console *c, const char *msg);
/* One AAR_OP_LOG record from the app: queued as a diagnostic line, with a
 * newline added unless the record already ends in one. */
void aar_serve_console_launcher_log(struct aar_serve_console *c, const uint8_t *data,
                                    uint32_t bytes);
/* CONSOLE_WRITE and CONSOLE_READ. Both complete through the sink. */
void aar_serve_console_write_request(struct aar_serve_console *c,
                                     const struct aar_serve_req *req,
                                     struct aar_serve_done *done, int *deferred);
void aar_serve_console_read_request(struct aar_serve_console *c,
                                    const struct aar_serve_req *req,
                                    struct aar_serve_done *done, int *deferred);
/* Starts the next write or read that may start. Call it on every pass. */
void aar_serve_console_pump(struct aar_serve_console *c);
/* The DOS write started by write_start finished with this result. */
void aar_serve_console_write_done(struct aar_serve_console *c, int32_t written);
/* The DOS read finished: got bytes in data, or <= 0 for end of input. */
void aar_serve_console_read_done(struct aar_serve_console *c, int32_t got,
                                 const uint8_t *data);
/* Output the app or the launcher still has to write. */
int aar_serve_console_output_pending(const struct aar_serve_console *c);

/* ---- Display ------------------------------------------------------------ */

#define AAR_SERVE_SCREEN_MAX 4096u
#define AAR_SERVE_DISPLAY_ID 1u

struct aar_serve_display {
    struct aar_serve_display_ops ops;
    const struct aar_serve_sink *sink;

    int open;
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t layout;
    uint32_t flags; /* AAR_DISPLAY_* the app asked for */
    uint32_t front; /* tokens of page 0, 1 and 2 (0 when absent) */
    uint32_t back;
    uint32_t spare;
    int have_two;   /* two P96 screen buffers, flipped by ChangeScreenBuffer */
    int have_pan;   /* one tall screen, flipped by panning */
    uint32_t pan_count;
    uint32_t showing;

    int flip_active; /* a ChangeScreenBuffer flip waits for its SafeMessage */
    struct aar_serve_req flip_req;
    int flip_queued; /* one more flip arrived meanwhile */
    struct aar_serve_req flip_next;
};

void aar_serve_display_init(struct aar_serve_display *d,
                            const struct aar_serve_display_ops *ops,
                            const struct aar_serve_sink *sink);

/* DISPLAY_OPEN arguments: AAR_OK, or AAR_E_INVAL for a size or format the
 * runtime must not have asked for. */
int32_t aar_serve_display_open_check(const struct aar_serve_req *req);
/* The surface depth in bits and the AAR_LAYOUT_* of one AAR_FMT_*. */
uint32_t aar_serve_display_depth(uint32_t format);
uint32_t aar_serve_display_layout(uint32_t format);
/* The AAR_DISPCAP_* bits aar_serve grants for the AAR_DISPLAY_* flags asked. */
uint16_t aar_serve_display_caps(uint32_t open_flags);
/* Records the screen DISPLAY_OPEN made and fills its completion. front,
 * back and spare are tokens (board offsets) of the bitmap memory: for
 * panning, front is page 0 and the other pages follow at pitch * height. */
void aar_serve_display_opened(struct aar_serve_display *d, const struct aar_serve_req *req,
                              uint32_t pitch, uint32_t front, uint32_t back,
                              uint32_t spare, int have_two, int have_pan,
                              uint32_t pan_count, struct aar_serve_done *done);
/* Forgets the screen (the P96 screen is closed elsewhere). */
void aar_serve_display_reset(struct aar_serve_display *d);
void aar_serve_display_flip_request(struct aar_serve_display *d,
                                    const struct aar_serve_req *req,
                                    struct aar_serve_done *done, int *deferred);
/* A SafeMessage of a ChangeScreenBuffer flip came back. */
void aar_serve_display_flip_safe(struct aar_serve_display *d);
void aar_serve_display_palette_request(struct aar_serve_display *d,
                                       const struct aar_serve_req *req,
                                       struct aar_serve_done *done);
void aar_serve_display_close_request(struct aar_serve_display *d,
                                     const struct aar_serve_req *req,
                                     struct aar_serve_done *done);

/* ---- Audio (the AHI route) ---------------------------------------------- */

struct aar_serve_audio_ops {
    void *ctx;
    /* Opens AHI at rate and channels and binds the ring (token, bytes,
     * frames) the runtime named. 0 = it cannot. */
    int (*open)(void *ctx, uint32_t rate, uint32_t channels, uint32_t token,
                uint32_t bytes, uint32_t frames);
    void (*close)(void *ctx);
};

struct aar_serve_audio {
    struct aar_serve_audio_ops ops;
    int open;
    uint32_t rate;
    uint32_t channels;
    uint32_t frames;
    int open_failed;
};

void aar_serve_audio_init(struct aar_serve_audio *a, const struct aar_serve_audio_ops *ops);
void aar_serve_audio_open_request(struct aar_serve_audio *a, const struct aar_serve_req *req,
                                  struct aar_serve_done *done);
void aar_serve_audio_close_request(struct aar_serve_audio *a,
                                   const struct aar_serve_req *req,
                                   struct aar_serve_done *done);
/* The frames one AHI buffer takes from the ring: what is available, at most
 * 1/50 s of audio and the buffer size. */
uint32_t aar_serve_audio_chunk_frames(uint32_t available_frames, uint32_t rate,
                                      uint32_t channels, uint32_t buffer_bytes);

/* ---- Events ------------------------------------------------------------- */

struct aar_serve_events_ops {
    void *ctx;
    /* AARPostEvent: AAR_OK when the event is posted or merged into
     * a pending mouse event, AAR_E_BUSY when the completion queue is full
     * and the event was dropped (the library counts the drop), another
     * AAR_E_* when the claim is gone. */
    int (*post)(void *ctx, const aar_event *ev);
};

/* The library coalesces mouse motion and keeps event order. What aar_serve adds is
 * the one event that must never be lost: a stop that found the queue full is
 * posted again on every pass until it is delivered. */
struct aar_serve_events {
    struct aar_serve_events_ops ops;
    int stop_pending;
    aar_event stop;
};

void aar_serve_events_init(struct aar_serve_events *e, const struct aar_serve_events_ops *ops);
/* Returns the result of the post (AAR_OK when posted or kept for retry). */
int aar_serve_events_push(struct aar_serve_events *e, uint16_t type, uint16_t code,
                          int16_t dx, int16_t dy, uint16_t buttons,
                          uint32_t time_ms);
/* Posts what could not go earlier. Call it on every pass. */
void aar_serve_events_flush(struct aar_serve_events *e);
int aar_serve_events_pending(const struct aar_serve_events *e);

/* ---- Dispatch ----------------------------------------------------------- */

struct aar_serve_core {
    struct aar_serve_files files;
    struct aar_serve_console console;
    struct aar_serve_display display;
    struct aar_serve_audio audio;
    struct aar_serve_sink sink;
};

void aar_serve_core_init(struct aar_serve_core *s, const struct aar_serve_sink *sink);
/* Serves one request. AAR_SERVE_REQ_DEFERRED: the completion comes later
 * through the sink. An op aar_serve does not serve completes with AAR_E_NOSYS. */
int aar_serve_core_request(struct aar_serve_core *s, const struct aar_serve_req *req,
                           struct aar_serve_done *done);


#endif /* AAR_SERVE_CORE_H */
