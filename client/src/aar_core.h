/*
 * The portable core (L1) of aar.library: the handle table, the owner and stale
 * checks, the queue pump, the message inbox and credits, events, the doorbell,
 * status, tags, the shared map cache and the audio ring read. It makes no OS
 * call. Everything it needs from the system comes through `struct
 * aar_core_env` (memory, who is calling, two kinds of exclusion) and
 * everything it needs from the hardware through `struct aar_plat`
 * (aar_plat.h). The AmigaOS shell (L2, aar_shell.c) and the host tests supply
 * the first; the ZZ9000 transport (L3) and the reference platform (refplat.c)
 * the second.
 *
 * Each public call of aar.library is one aar_core_* function below, in the
 * same order. L2 adds the Exec parts (the library base, the owner's signal
 * bit, the notify port, files, the dispatcher process) around it.
 *
 * Rules the core keeps:
 *   - A call that takes a handle first looks the pointer up in the registry,
 *     so a pointer that was closed returns AAR_E_STALE and is never
 *     dereferenced; then it compares the calling task (env->self) with the
 *     owner recorded at aar_core_open, AAR_E_OWNER on a mismatch.
 *   - The lease list is walked by the vertical-blank interrupt. It is changed
 *     only between env->irq_off and env->irq_on, and a claim leaves it before
 *     the platform releases it (unlink, release, drop pointers, free).
 *   - The interrupt (aar_core_vblank) only stores words. It calls no platform
 *     operation and takes no lock.
 *   - A handle belongs to one task. Its state needs no lock, except the part
 *     the dispatcher also touches (the `busy`/`bg` handshake below).
 *
 * Host tests: with one thread, alloc, release and self are all a test needs;
 * every other hook may be NULL. Define ULONG and APTR with
 * client/tests/stubs (32 bit words), and keep memory that is reached through
 * an APTR (a map, a handle) below 4 GiB or build with -m32.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CORE_H
#define AAR_CORE_H

#include <stddef.h>
#include <stdint.h>

#include <libraries/aar.h>
#include <libraries/aar_zz9000.h>

#include "aar_plat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Sizes and constants ----------------------------------------------------- */

#define AAR_CORE_MAPS        16u   /* ranges mapped per handle */
#define AAR_CORE_INBOX       64u   /* messages held for AARGetMessage */
#define AAR_CORE_OUTSTANDING 64u   /* requests handed out and not yet answered */
#define AAR_CORE_MAP_STEP    0x2000u /* blocks are mapped in 8 KiB steps */
#define AAR_CORE_LEASE_PERIOD_TICKS 5u    /* one renewal per handle per 5 ticks (about 100 ms) */
#define AAR_CORE_STALL_TICKS 250u         /* the dispatcher gate: about 5 s */
#define AAR_CORE_NOTIFY_RESIGNAL_TICKS 5u /* pending work is signalled again after this */

/* How an owner looks to the platform of the system (owner_state). */
#define AAR_OWNER_ALIVE   0 /* the task at the address still has the recorded stamp */
#define AAR_OWNER_CHANGED 1 /* a task is at the address but it is not the owner */
#define AAR_OWNER_ABSENT  2 /* no task is at the address */

/* Handle states. */
#define AAR_H_LIVE    1u /* registered and usable */
#define AAR_H_CLOSING 2u /* a close or a reap has taken it; calls answer AAR_E_STALE */
#define AAR_H_DEAD    3u /* tombstone: the claim is released, the record stays until the owner is gone */

/* ---- The environment (L2 or the test) ------------------------------------------- */

/*
 * Who a task is. Exec can give a dead task's address to a new task, so the
 * address alone proves nothing: an owner is alive only while the task found at
 * its address still carries the recorded name pointer, node type and CLI task
 * number (spec 5.5(3)).
 */
typedef struct aar_owner_stamp {
    const void *id;       /* the task's address */
    const void *name;     /* its name pointer at open time */
    uint32_t number;      /* CLI task number, 0 for a plain task */
    uint32_t kind;        /* node type */
} aar_owner_stamp;

struct aar_core_env {
    uint32_t size;        /* sizeof */
    void *user;           /* passed to every hook */
    /* Zeroed memory that the CPU and the interrupt may touch; NULL on failure. */
    void *(*alloc)(void *user, uint32_t bytes);
    void (*release)(void *user, void *p, uint32_t bytes);
    /* The calling task. */
    void (*self)(void *user, aar_owner_stamp *out);
    /* Excludes other tasks while the registry changes and the dispatcher walks
     * it. exclusive = 1 for a change, 0 for a walk. Never held across a
     * platform operation. May be NULL (one thread). */
    void (*lock)(void *user, int exclusive);
    void (*unlock)(void *user);
    /* Disable() and Enable(): the registry and the lease list are changed and
     * looked up between them, so the interrupt and AARMessageSignal can read
     * them. Nothing that waits runs inside. May be NULL. */
    void (*irq_off)(void *user);
    void (*irq_on)(void *user);
    /* Gives the CPU away briefly while the dispatcher holds a handle (Delay(1)).
     * May be NULL (spin). */
    void (*yield)(void *user);
    /* Called before a handle's memory is freed: L2 takes back a notify message
     * that the handle still has on the port. May be NULL. */
    void (*retire)(void *user, AARHandle *h);
    uint32_t ticks_per_sec;  /* vertical blanks per second: 50 or 60 */
};

/* ---- The handle -------------------------------------------------------------------- */

typedef struct aar_map_slot {
    uint32_t token;
    uint32_t bytes;       /* rounded to AAR_CORE_MAP_STEP */
    uint32_t mapf;        /* AAR_MAPF_* the platform reported */
    volatile uint8_t *ptr;
} aar_map_slot;

typedef struct aar_outstanding {
    uint16_t op;          /* 0: free */
    uint32_t id;
    uint32_t buf, buf_len;/* the request's buffer, kept for the audio ring */
} aar_outstanding;

struct aar_core;

/*
 * One open app instance. Private: use the functions. It is plain storage
 * (about 3.5 KiB) that aar_core_open allocates together with the platform's
 * claim state (aar_plat_ops.claim_priv_bytes).
 */
struct AARHandle {
    /* ---- registry (changed under env->lock and env->irq_off) ---- */
    struct AARHandle *next;
    struct aar_core *core;
    volatile uint32_t state;      /* AAR_H_* */
    aar_owner_stamp owner;
    uint32_t signal_mask;         /* the owner task's signal bit, from L2 */
    void *notify;                 /* the notify port (opaque to the core) */
    void *opener;                 /* L2's per-task record (opaque) */
    uint32_t alloc_bytes;

    /* ---- the vertical-blank interrupt reads these ---- */
    struct AARHandle *lease_next; /* the lease list (under env->irq_off) */
    volatile uint8_t *volatile cb;/* control block; NULL once dropped */
    volatile uint32_t lease;      /* last value stored in AAR_CB_LEASE_COUNTER */
    volatile uint8_t renew;       /* 1: the interrupt renews this claim */
    uint8_t countdown;            /* ticks to the next store; starts at the phase, spreads the stores */
    volatile uint32_t pause_until;/* lease_pause: no renewal while ticks < this (0: none) */

    /* ---- owner against dispatcher handshake ---- */
    volatile uint8_t busy;        /* the owner is inside a call */
    volatile uint8_t bg;          /* the dispatcher is inside housekeeping */

    /* ---- the claim ---- */
    struct aar_plat_claim claim;
    uint32_t reason;              /* last AAR_REASON_* of a refusal */
    uint8_t revoked;              /* the platform answered AAR_E_STALE; release still owed */
    aar_queue sq, cq;
    uint32_t msg_posted;
    aar_map_slot maps[AAR_CORE_MAPS];
    uint32_t map_next;

    /* ---- limits and load ---- */
    uint32_t private_mib, shared_mib, weight;
    struct aar_plat_load_opts opts;
    volatile uint8_t *load;
    uint32_t chunk_max, image_bytes;
    uint8_t load_begun, started;

    /* ---- messages from the app ---- */
    uint8_t inbox[AAR_CORE_INBOX][AAR_MSG_BYTES];
    volatile uint32_t inbox_head, inbox_tail;
    uint8_t sq_blocked;
    void (*log_sink)(APTR user, const void *data, ULONG len);
    APTR log_user;

    /* ---- requests handed out ---- */
    aar_outstanding out[AAR_CORE_OUTSTANDING];

    /* ---- events and the doorbell ---- */
    aar_event mouse;
    uint8_t mouse_pending;
    uint32_t kick_pending;

    /* ---- audio ---- */
    volatile uint8_t *audio;
    uint32_t audio_frames, audio_channels;

    /* ---- notification (the dispatcher reads and writes these) ---- */
    volatile uint8_t armed;
    uint8_t ended_notified;
    uint32_t last_tail;
    uint32_t last_signal_tick;
    AAREvent event;               /* the message L2 posts to the notify port */

    AARZZ9000Stats stats;
};

/* ---- The core ------------------------------------------------------------------------- */

typedef struct aar_core {
    struct aar_plat *plat;
    struct aar_core_env env;
    int attach_rc;                /* AAR_OK once the platform is attached, else why not */
    struct AARHandle *handles;    /* the registry: every state */
    struct AARHandle *volatile lease_head;
    volatile uint32_t ticks;      /* vertical blanks, counted by aar_core_vblank */
    volatile uint32_t disp_alive; /* `ticks` at the dispatcher's last pass */
    uint32_t opens;               /* counts claims, for the phase */
} aar_core;

/* Sets up `c`. Nothing is attached yet: attach_rc is AAR_E_NOSYS. */
int aar_core_init(aar_core *c, struct aar_plat *plat, const struct aar_core_env *env);

/* Platform attach and detach (ZZ9000: opens zz9k.library). L2 calls them from
 * the dispatcher process, a test from its main thread. attach stores its
 * result in c->attach_rc, which AARQuery and AAROpen return when it is not
 * AAR_OK. */
int aar_core_attach(aar_core *c);
void aar_core_detach(aar_core *c);

/* The final CloseLibrary: closes every handle that is still open, frees the
 * tombstones. */
void aar_core_shutdown(aar_core *c);

/* ---- The calls, in vector order ----------------------------------------------------------- */

/* AAR_OK, AAR_E_STALE or AAR_E_OWNER: whether the calling task may use `h`. */
int aar_core_check(aar_core *c, AARHandle *h);

int aar_core_query(aar_core *c, AARInfo *info);

/*
 * Claims an app instance for the calling task (env->self). Allocates the
 * handle, claims through the platform, maps and checks the control block and
 * both queues, registers the handle and starts its lease. `signal_mask` is the
 * owner task's signal bit (0 when there is none), `notify` the notify port or
 * NULL. *reason gets AAR_REASON_NONE or the platform's refusal reason.
 * AAR_E_NOMEM, AAR_E_NORESOURCE, AAR_E_NOSYS, AAR_E_IO as docs/aar-client-api.md.
 */
int aar_core_open(aar_core *c, uint32_t signal_mask, void *notify, void *opener,
                  AARHandle **out, ULONG *reason);

/* Stops the app if it runs, releases the claim, frees the handle. */
int aar_core_close(aar_core *c, AARHandle *h);

/* Closes every handle of the calling task (the last CloseLibrary of a task).
 * Returns how many. */
uint32_t aar_core_close_owner(aar_core *c);

/*
 * Tags. L2 walks the tag list with utility.library and gives each pair to
 * aar_core_tags_add, which checks it and records it in `t`; a bad pair leaves
 * `t` as it was. aar_core_set_tags then applies the whole set to the handle
 * (AAR_E_BUSY once the load began or the app started). A list with a bad tag
 * therefore changes nothing.
 */
struct aar_core_tags {
    uint32_t set;                 /* bit n: the tag with id AAR_TAG_BASE + n was given */
    uint32_t private_mib, shared_mib, weight, lease_extended, audio_route, caps;
};
void aar_core_tags_init(struct aar_core_tags *t);
int aar_core_tags_add(aar_core *c, struct aar_core_tags *t, uint32_t id, uint32_t data);
int aar_core_set_tags(aar_core *c, AARHandle *h, const struct aar_core_tags *t);

/*
 * Load, in three steps (AARLoadFile reads the file between them, AARLoadMemory
 * is the loop below). `image_bytes` is the length of the whole image: 0 or more
 * than AARInfo.max_image_bytes is AAR_E_INVAL. aar_core_load_begin writes the
 * limits, begins the load through the platform and returns the staging area and
 * the most bytes one chunk takes. AAR_E_BUSY when a load already began or the
 * app started.
 */
int aar_core_load_begin(aar_core *c, AARHandle *h, uint32_t image_bytes,
                        volatile uint8_t **staging, uint32_t *chunk_max);
int aar_core_load_chunk(aar_core *c, AARHandle *h, uint32_t file_off, uint32_t len);
int aar_core_load_end(aar_core *c, AARHandle *h);
int aar_core_load_memory(aar_core *c, AARHandle *h, const void *data, ULONG length);

int aar_core_start(aar_core *c, AARHandle *h, const char *const *argv, ULONG argc);
int aar_core_stop(aar_core *c, AARHandle *h);
int aar_core_status(aar_core *c, AARHandle *h, AARStatusInfo *status);
int aar_core_send_message(aar_core *c, AARHandle *h, const AARMessage *message);
int aar_core_get_message(aar_core *c, AARHandle *h, AARMessage *message);

/* The owner's signal mask, 0 when `h` is not a live handle. Any task may call
 * it, and an interrupt may. The first call arms the handle for the dispatcher's
 * notification scan. */
ULONG aar_core_message_signal(aar_core *c, AARHandle *h);

int aar_core_shared_map(aar_core *c, AARHandle *h, ULONG haddr, ULONG bytes,
                        ULONG flags, struct AARSharedMap *map);
int aar_core_shared_unmap(aar_core *c, AARHandle *h, struct AARSharedMap *map);
int aar_core_shared_publish(aar_core *c, AARHandle *h, struct AARSharedMap *map,
                            ULONG offset, ULONG length);
int aar_core_shared_acquire(aar_core *c, AARHandle *h, struct AARSharedMap *map,
                            ULONG offset, ULONG length);
int aar_core_shared_token(aar_core *c, AARHandle *h, const void *ptr, ULONG bytes,
                          ULONG *token);

/* A request stays outstanding, with its buffer mapped, until aar_core_service_reply
 * posts its completion. */
int aar_core_service_next(aar_core *c, AARHandle *h, AARServiceReq *req);
int aar_core_service_reply(aar_core *c, AARHandle *h, const AARServiceReq *req);
int aar_core_post_event(aar_core *c, AARHandle *h, const aar_event *event);
int aar_core_audio_read(aar_core *c, AARHandle *h, void *dst, ULONG frames, ULONG *got);

/* AARExt: the platform's table for `ext_id` when `platform_id` is the
 * platform's, else NULL. */
const void *aar_core_ext(aar_core *c, ULONG platform_id, ULONG ext_id);

/* ---- The ZZ9000 extension functions that belong to the core --------------------------------- */

/*
 * They take a handle and no core (the table of aar_client_ext_zz9000 has the
 * calling convention of a plain function), so they act on the core most
 * recently initialised. A platform's `ext` fills the table with them next to
 * its own `list` and `release_handle`. Owner and stale checks apply.
 */
LONG aar_core_ext_stats(AARHandle *h, AARZZ9000Stats *out);
LONG aar_core_ext_lease_pause(AARHandle *h, ULONG ms);
LONG aar_core_ext_set_log_sink(AARHandle *h, AARZZ9000LogSink sink, APTR user);

/* ---- Interrupt, dispatcher ---------------------------------------------------------------- */

/*
 * The vertical-blank interrupt body: counts the tick, and while the
 * dispatcher is alive (it stamped disp_alive less than AAR_CORE_STALL_TICKS
 * ago) stores the lease counter of every claim whose phase it is. Stores only.
 */
void aar_core_vblank(aar_core *c);

/*
 * What a dispatcher pass tells L2 to deliver, and what L2 answers. The rule of
 * every notification: it counts as done only when it has been delivered, so
 * the core writes its done-markers (the end announced, the tail and the tick
 * of the last signal) after the answer, never before.
 *   AAR_CORE_NOTIFIED  the owner was signalled and its event is on the port (or
 *                      it has no port)
 *   AAR_CORE_NOTIFY_BUSY  the owner was signalled, but the port still holds an
 *                      earlier event, so nothing new was posted: the work that
 *                      is pending stays visible to the owner through that event
 *                      (and is announced again after a few ticks), but the end
 *                      of the app is not: its event has to carry the end
 *   0                  not delivered (the owner is gone): try again
 */
#define AAR_CORE_NOTIFIED 1
#define AAR_CORE_NOTIFY_BUSY 2
typedef int (*aar_core_notify_fn)(void *user, AARHandle *h, uint32_t type);
typedef int (*aar_core_owner_fn)(void *user, const aar_owner_stamp *owner);

/*
 * One dispatcher pass (each vertical blank): stamps disp_alive, then for every
 * live handle whose owner is not inside a call: retries the doorbell, posts
 * the pending mouse event, lets the platform retire its doorbell request
 * (aar_plat_ops.poll), and, for an armed handle, scans for work. `notify` is
 * called once per handle that has news, with an AAR_NOTIFY_* type, and L2
 * signals the owner and posts the event.
 */
void aar_core_dispatch(aar_core *c, aar_core_notify_fn notify, void *user);

/*
 * The dead-owner pass (about once a second). `owner_state` says how an owner
 * looks now (AAR_OWNER_*). A live handle whose owner is not AAR_OWNER_ALIVE is
 * reaped: it leaves the lease list, the app is stopped and the claim released,
 * and the handle becomes a tombstone (AAR_H_DEAD, calls answer AAR_E_STALE).
 * A tombstone is freed once its owner is AAR_OWNER_ABSENT. Returns the number
 * of handles reaped.
 */
uint32_t aar_core_reap(aar_core *c, aar_core_owner_fn owner_state, void *user);

/*
 * Fills the handle's AAREvent for L2 to post to the notify port: the type, the
 * handle pointer and the status as of now. L2 calls it only when the previous
 * event has been taken off the port.
 */
void aar_core_event_fill(AARHandle *h, uint32_t type);

#ifdef __cplusplus
}
#endif

#endif /* AAR_CORE_H */
