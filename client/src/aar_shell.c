/*
 * The AmigaOS shell (L2) of aar.library: the Exec library (RomTag, Open, Close,
 * Expunge, the vector table), the per-task opener records with their one
 * signal bit, the vertical-blank lease server, the dispatcher process (dead
 * owners, notification scan, doorbell retry, event flush), the notify port,
 * and the calls that need dos.library or utility.library (AARLoadFile,
 * AARProbeFile, tag lists).
 *
 * Every Exec, DOS and utility call goes through an X_* binding, so that a host
 * test can drive this file with a fake system: the test defines AAR_SHELL_HOST
 * and puts client/tests/shell_host.h on the include path, then includes this
 * file. On the Amiga the bindings below are the real calls. Everything that
 * is not a binding runs unchanged in both builds, except the part marked
 * "Amiga glue" at the end (the library skeleton, the interrupt and the
 * dispatcher process), which the host replaces with the same-named
 * bindings.
 *
 * Locking (design 6.4): one SignalSemaphore, base->lock, serialises changes to
 * the handle registry and the opener list against the dispatcher's walks.
 * Disable()/Enable() cover the registry and lease-list lookups that an
 * interrupt may also make. No lock is held across a platform operation or a
 * call into dos.library.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "aar_core.h"
#include "aar_probe.h"

#ifdef AAR_SHELL_HOST
/* ---- The host build: the bindings are the test's fake system ------------------- */
#include "shell_host.h"
#else
/* ---- The Amiga build: the bindings are the real calls --------------------------- */
#include <SDI_compiler.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <hardware/intbits.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <utility/tagitem.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
/* Defined by libnix next to the arithmetic helpers that use it. */
extern struct Library *UtilityBase;

#define X_SEMTYPE struct SignalSemaphore
#define X_SEM_INIT(s) InitSemaphore(s)
#define X_SEM_OBTAIN(s) ObtainSemaphore(s)
#define X_SEM_OBTAIN_SHARED(s) ObtainSemaphoreShared(s)
#define X_SEM_RELEASE(s) ReleaseSemaphore(s)
#define X_DISABLE() Disable()
#define X_ENABLE() Enable()
#define X_SELF() ((void *)SysBase->ThisTask)
#define X_ALLOC(bytes) AllocVec((bytes), MEMF_PUBLIC | MEMF_CLEAR)
#define X_FREE(p, bytes) FreeVec(p)
#define X_SIGNAL(task, mask) Signal((struct Task *)(task), (mask))
#define X_ALLOC_SIGNAL() ((int)AllocSignal(-1))
#define X_FREE_SIGNAL(bit) FreeSignal((BYTE)(bit))
#define X_DELAY1() Delay(1)
#define X_PUTMSG(port, msg) PutMsg((struct MsgPort *)(port), (struct Message *)(msg))
#define X_MSG_INIT(msg, len) aar_x_msg_init((struct Message *)(msg), (UWORD)(len))
#define X_MSG_QUEUED(port, msg) aar_x_msg_queued((struct MsgPort *)(port), (struct Message *)(msg))
#define X_MSG_REMOVE(msg) Remove((struct Node *)(msg))
#define X_FILE BPTR
#define X_FILE_OPEN(path) Open((STRPTR)(path), MODE_OLDFILE)
#define X_FILE_SIZE(f) aar_x_file_size(f)
#define X_FILE_SEEK(f, off) (Seek((f), (LONG)(off), OFFSET_BEGINNING) >= 0)
#define X_FILE_READ(f, dst, n) Read((f), (APTR)(dst), (LONG)(n))
#define X_FILE_CLOSE(f) Close(f)
#define X_CTRL_C() ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0)
#define X_NEXT_TAG(listp) NextTagItem((struct TagItem **)(listp))
#define X_TASK_LOOKUP(task, st) aar_x_task_lookup((task), (st), 1)
#define X_TASK_LOOKUP_LOCKED(task, st) aar_x_task_lookup((task), (st), 0)
#define X_STAMP(task, st) aar_x_stamp((struct Task *)(task), (st))
#define X_DISPATCHER_START(base) aar_x_dispatcher_start(base)
#define X_DISPATCHER_STOP(base) aar_x_dispatcher_stop(base)
#define X_OPEN_SYSTEM() aar_x_open_system()
#define X_CLOSE_SYSTEM() aar_x_close_system()

struct AARBase;
static int aar_x_dispatcher_start(struct AARBase *base);
static void aar_x_dispatcher_stop(struct AARBase *base);

static void aar_x_stamp(struct Task *task, aar_owner_stamp *st)
{
    st->id = task;
    st->name = task->tc_Node.ln_Name;
    st->kind = task->tc_Node.ln_Type;
    st->number = task->tc_Node.ln_Type == NT_PROCESS ?
        (uint32_t)((struct Process *)task)->pr_TaskNum : 0u;
}

/*
 * Whether `wanted` is still on one of Exec's task lists (the running task is
 * on none), and if so what it looks like now. Exec moves a task between its
 * lists with interrupts disabled, so the walk disables them too; Forbid
 * would let it follow a node into the wrong list and miss a live task.
 * Nothing blocks inside. `lock` is 0 when the caller already disabled.
 */
static int aar_x_task_lookup(const void *wanted, aar_owner_stamp *st, int lock)
{
    const struct Node *node;
    int found = 0;

    if (lock)
        Disable();
    if (SysBase->ThisTask == wanted) {
        found = 1;
    } else {
        for (node = SysBase->TaskReady.lh_Head; node->ln_Succ; node = node->ln_Succ) {
            if (node == wanted) {
                found = 1;
                break;
            }
        }
        for (node = SysBase->TaskWait.lh_Head; !found && node->ln_Succ; node = node->ln_Succ) {
            if (node == wanted)
                found = 1;
        }
    }
    if (found)
        aar_x_stamp((struct Task *)wanted, st);
    if (lock)
        Enable();
    return found;
}

static void aar_x_msg_init(struct Message *m, UWORD len)
{
    m->mn_Node.ln_Type = NT_MESSAGE;
    m->mn_ReplyPort = NULL;
    m->mn_Length = len;
}

/* GetMsg only unlinks the node, it does not clear it, so "queued" has to be
 * answered by looking for the node in the port's list. Interrupts are off. */
static int aar_x_msg_queued(struct MsgPort *port, struct Message *msg)
{
    const struct Node *n;

    for (n = port->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        if (n == (const struct Node *)msg)
            return 1;
    }
    return 0;
}

static LONG aar_x_file_size(BPTR f)
{
    (void)Seek(f, 0L, OFFSET_END);
    return Seek(f, 0L, OFFSET_BEGINNING);
}

#define AAR_LIB_REVISION 1
/* A function address as a library vector or an object pointer. */
#define AAR_VEC(f) ((APTR)(uintptr_t)(f))
#define AAR_LIB_IDSTRING AAR_LIB_NAME " 0.1 (2026)\r\n"
#endif /* AAR_SHELL_HOST */

#define AAR_DISPATCH_PRIORITY 5
#define AAR_REAP_PERIOD_TICKS 50u
#define AAR_LOAD_CHUNK_BYTES 65536u

/* ---- Structures ---------------------------------------------------------------------- */

/* One record per task that has the library open (design 3.4). */
struct AAROpener {
    struct AAROpener *next;
    aar_owner_stamp stamp;
    uint32_t refs;               /* OpenLibrary calls of this task not yet closed */
    int sigbit;                  /* the task's one signal bit, -1 until its first AAROpen */
    uint8_t closing;             /* the last CloseLibrary of the task has taken it */
};

struct AARBase {
#ifdef AAR_SHELL_HOST
    struct Library library;
#else
    struct Library library;
    BPTR segment;
#endif
    X_SEMTYPE lock;
    aar_core core;
    struct AAROpener *openers;
    uint32_t reap_last;          /* `ticks` at the last dead-owner pass */
    uint8_t dispatcher_up;       /* the dispatcher has attached the platform and runs */
#ifndef AAR_SHELL_HOST
    /* The dispatcher process and its interrupt: see the Amiga glue. */
    struct Process *disp_process;
    struct Task *disp_task;
    ULONG disp_mask;
    struct Task *handshake_task; /* who waits for the dispatcher to start or stop */
    ULONG handshake_mask;
    int disp_status;
    UBYTE disp_stop;
    struct Interrupt vblank;
    UBYTE vblank_installed;
#endif
};

static struct AARBase *g_base;

/* ---- Task stamps ------------------------------------------------------------------------ */

static int stamp_equal(const aar_owner_stamp *a, const aar_owner_stamp *b)
{
    return a->id == b->id && a->name == b->name && a->number == b->number &&
           a->kind == b->kind;
}

/* How an owner looks now, for the reap: the same task, another task at the
 * address, or none. */
static int shell_owner_state(void *user, const aar_owner_stamp *owner)
{
    aar_owner_stamp now;

    (void)user;
    if (!X_TASK_LOOKUP(owner->id, &now))
        return AAR_OWNER_ABSENT;
    return stamp_equal(owner, &now) ? AAR_OWNER_ALIVE : AAR_OWNER_CHANGED;
}

/* ---- The environment of the core --------------------------------------------------------- */

static void *env_alloc(void *user, uint32_t bytes)
{
    (void)user;
    return X_ALLOC(bytes);
}

static void env_release(void *user, void *p, uint32_t bytes)
{
    (void)user;
    (void)bytes;
    X_FREE(p, bytes);
}

static void env_self(void *user, aar_owner_stamp *out)
{
    (void)user;
    X_STAMP(X_SELF(), out);
}

static void env_lock(void *user, int exclusive)
{
    struct AARBase *b = (struct AARBase *)user;

    if (exclusive)
        X_SEM_OBTAIN(&b->lock);
    else
        X_SEM_OBTAIN_SHARED(&b->lock);
}

static void env_unlock(void *user)
{
    X_SEM_RELEASE(&((struct AARBase *)user)->lock);
}

static void env_irq_off(void *user)
{
    (void)user;
    X_DISABLE();
}

static void env_irq_on(void *user)
{
    (void)user;
    X_ENABLE();
}

static void env_yield(void *user)
{
    (void)user;
    X_DELAY1();
}

/* A notify message that is still on its port must leave before the handle's
 * memory does. A dead owner's port is gone: leave the node alone. */
static void env_retire(void *user, AARHandle *h)
{
    aar_owner_stamp now;

    (void)user;
    if (!h->notify)
        return;
    X_DISABLE();
    if (X_TASK_LOOKUP_LOCKED(h->owner.id, &now) && stamp_equal(&h->owner, &now) &&
        X_MSG_QUEUED(h->notify, &h->event.message))
        X_MSG_REMOVE(&h->event.message);
    X_ENABLE();
}

static void shell_env(struct AARBase *b, struct aar_core_env *env)
{
    memset(env, 0, sizeof(*env));
    env->size = sizeof(*env);
    env->user = b;
    env->alloc = env_alloc;
    env->release = env_release;
    env->self = env_self;
    env->lock = env_lock;
    env->unlock = env_unlock;
    env->irq_off = env_irq_off;
    env->irq_on = env_irq_on;
    env->yield = env_yield;
    env->retire = env_retire;
    env->ticks_per_sec = 50u;
}

/* ---- Openers ------------------------------------------------------------------------------ */

/* The record of the calling task, or NULL. The caller holds the lock. */
static struct AAROpener *opener_find(struct AARBase *b, const aar_owner_stamp *me,
                                     struct AAROpener **previous)
{
    struct AAROpener *o, *prior = NULL;

    for (o = b->openers; o; o = o->next) {
        if (o->stamp.id == me->id && !o->closing) {
            if (previous)
                *previous = prior;
            return o;
        }
        prior = o;
    }
    if (previous)
        *previous = prior;
    return NULL;
}

/* ---- Notification --------------------------------------------------------------------------- */

/* Signals the owner and posts the event, unless the owner is gone. A task that
 * died but is not yet reaped must not be signalled, and its port must not be
 * written: both checks and the calls run with interrupts off. */
static int shell_notify(void *user, AARHandle *h, uint32_t type)
{
    aar_owner_stamp now;
    int alive, queued = 0;

    (void)user;
    X_DISABLE();
    alive = X_TASK_LOOKUP_LOCKED(h->owner.id, &now) && stamp_equal(&h->owner, &now);
    if (alive && h->signal_mask != 0u)
        X_SIGNAL(h->owner.id, h->signal_mask);
    if (alive && h->notify)
        queued = X_MSG_QUEUED(h->notify, &h->event.message);
    X_ENABLE();
    if (!alive)
        return 0;
    if (!h->notify)
        return AAR_CORE_NOTIFIED; /* the signal is all there is to deliver */
    if (queued)
        return AAR_CORE_NOTIFY_BUSY;
    /* One event at a time: the next is posted after the client took this one. */
    aar_core_event_fill(h, type);
    X_MSG_INIT(&h->event.message, sizeof(h->event));
    X_DISABLE();
    alive = X_TASK_LOOKUP_LOCKED(h->owner.id, &now) && stamp_equal(&h->owner, &now);
    if (alive)
        X_PUTMSG(h->notify, &h->event.message);
    X_ENABLE();
    return alive ? AAR_CORE_NOTIFIED : 0;
}

/* Opener records of tasks that are gone: their handles are reaped by the core;
 * the record, not the signal bit (that went with the task), is freed here. */
static void reap_openers(struct AARBase *b)
{
    struct AAROpener *o, *prior, *next;

    X_SEM_OBTAIN(&b->lock);
    prior = NULL;
    for (o = b->openers; o; o = next) {
        next = o->next;
        if (!o->closing && shell_owner_state(b, &o->stamp) != AAR_OWNER_ALIVE) {
            if (prior)
                prior->next = next;
            else
                b->openers = next;
            X_FREE(o, sizeof(*o));
            continue;
        }
        prior = o;
    }
    X_SEM_RELEASE(&b->lock);
}

/*
 * One dispatcher pass, at each vertical blank: the notification scan and the
 * housekeeping of the core, and about once a second the dead-owner check.
 */
static void shell_pass(struct AARBase *b)
{
    aar_core_dispatch(&b->core, shell_notify, b);
    if ((uint32_t)(b->core.ticks - b->reap_last) >= AAR_REAP_PERIOD_TICKS) {
        b->reap_last = b->core.ticks;
        (void)aar_core_reap(&b->core, shell_owner_state, b);
        reap_openers(b);
    }
}

/* The interrupt body: the core's tick, which stores the lease words. */
static void shell_vblank(struct AARBase *b)
{
    aar_core_vblank(&b->core);
}

/* ---- Library level: open, close, init ---------------------------------------------------------- */

static int ensure_dispatcher(struct AARBase *b)
{
    int rc = AAR_OK;

    if (!b->dispatcher_up) {
        rc = X_DISPATCHER_START(b);
        if (rc != AAR_OK)
            return rc;
    }
    return b->core.attach_rc;
}

static int shell_init(struct AARBase *b)
{
    struct aar_core_env env;

    X_SEM_INIT(&b->lock);
    shell_env(b, &env);
    g_base = b;
    return aar_core_init(&b->core, aar_plat_get(), &env);
}

/* OpenLibrary: a record for the calling task, or one more reference on it. A
 * record whose task address now belongs to another task is stale: its task is
 * gone, so it is left to the reap and the new task gets a record of its own. */
static int shell_open(struct AARBase *b)
{
    aar_owner_stamp me;
    struct AAROpener *o, *fresh = NULL;

    env_self(b, &me);
    X_SEM_OBTAIN(&b->lock);
    o = opener_find(b, &me, NULL);
    if (o && !stamp_equal(&o->stamp, &me)) {
        /* Address reuse: hide the old record from this task. */
        o->closing = 1u;
        o = NULL;
    }
    if (!o) {
        X_SEM_RELEASE(&b->lock);
        fresh = (struct AAROpener *)X_ALLOC(sizeof(*fresh));
        if (!fresh)
            return AAR_E_NOMEM;
        fresh->stamp = me;
        fresh->sigbit = -1;
        X_SEM_OBTAIN(&b->lock);
        o = opener_find(b, &me, NULL);
        if (!o) {
            o = fresh;
            fresh = NULL;
            o->next = b->openers;
            b->openers = o;
        }
    }
    o->refs++;
    X_SEM_RELEASE(&b->lock);
    if (fresh)
        X_FREE(fresh, sizeof(*fresh));
    return AAR_OK;
}

/*
 * CloseLibrary: one reference less. The last of a task closes all its handles,
 * in the task's own context, frees its signal bit and drops its record.
 * Returns 1 when this was the last close of the whole library.
 */
static int shell_close(struct AARBase *b)
{
    aar_owner_stamp me;
    struct AAROpener *o, *p, *prior = NULL;
    int sigbit;

    env_self(b, &me);
    X_SEM_OBTAIN(&b->lock);
    o = opener_find(b, &me, NULL);
    if (!o) {
        /* The record was dropped (the task was taken for another and reaped):
         * its last close still closes what it owns and frees its tombstones. */
        X_SEM_RELEASE(&b->lock);
        (void)aar_core_close_owner(&b->core);
        return 0;
    }
    if (o->refs > 1u) {
        o->refs--;
        X_SEM_RELEASE(&b->lock);
        return 0;
    }
    o->refs = 0u;
    o->closing = 1u;
    X_SEM_RELEASE(&b->lock);

    (void)aar_core_close_owner(&b->core);
    sigbit = o->sigbit;
    if (sigbit >= 0)
        X_FREE_SIGNAL(sigbit);

    X_SEM_OBTAIN(&b->lock);
    for (p = b->openers; p; p = p->next) {
        if (p == o) {
            if (prior)
                prior->next = o->next;
            else
                b->openers = o->next;
            break;
        }
        prior = p;
    }
    X_SEM_RELEASE(&b->lock);
    X_FREE(o, sizeof(*o));
    return 0;
}

/* The final CloseLibrary: stop the dispatcher, then close what is left. */
static void shell_final_close(struct AARBase *b)
{
    X_DISPATCHER_STOP(b);
    aar_core_shutdown(&b->core);
}

/* ---- The calls ---------------------------------------------------------------------------------- */

static LONG v_query(AARInfo *info)
{
    int rc = ensure_dispatcher(g_base);

    if (rc != AAR_OK)
        return rc;
    return aar_core_query(&g_base->core, info);
}

static LONG v_open(AARHandle **handlep, struct MsgPort *port, ULONG *reason)
{
    struct AARBase *b = g_base;
    aar_owner_stamp me;
    struct AAROpener *o;
    int rc;

    if (reason)
        *reason = AAR_REASON_NONE;
    if (!handlep)
        return AAR_E_INVAL;
    *handlep = NULL;
    rc = ensure_dispatcher(b);
    if (rc != AAR_OK)
        return rc;
    env_self(b, &me);
    X_SEM_OBTAIN(&b->lock);
    o = opener_find(b, &me, NULL);
    X_SEM_RELEASE(&b->lock);
    if (!o) {
        /* A task that never opened the library but holds its base. */
        rc = shell_open(b);
        if (rc != AAR_OK)
            return rc;
        X_SEM_OBTAIN(&b->lock);
        o = opener_find(b, &me, NULL);
        X_SEM_RELEASE(&b->lock);
        if (!o)
            return AAR_E_NOMEM;
    }
    if (o->sigbit < 0) {
        /* One bit per owner task, shared by all its handles. */
        o->sigbit = X_ALLOC_SIGNAL();
        if (o->sigbit < 0)
            return AAR_E_NOMEM;
    }
    return aar_core_open(&b->core, 1u << o->sigbit, port, o, handlep, reason);
}

static LONG v_close(AARHandle *h)
{
    return aar_core_close(&g_base->core, h);
}

/* ---- Probe ------------------------------------------------------------------------------------------ */

struct file_reader {
    X_FILE file;
    uint32_t at;                 /* where the next read starts */
};

static int file_read(void *user, uint32_t offset, void *dst, uint32_t count)
{
    struct file_reader *r = (struct file_reader *)user;

    if (offset != r->at && !X_FILE_SEEK(r->file, offset))
        return AAR_E_IO;
    if (X_FILE_READ(r->file, dst, count) != (LONG)count)
        return AAR_E_IO;
    r->at = offset + count;
    return AAR_OK;
}

static LONG v_probe_file(const char *path, AARImageInfo *info)
{
    struct file_reader r;
    LONG size;
    int rc;

    if (!path || !info)
        return AAR_E_INVAL;
    r.file = X_FILE_OPEN(path);
    if (!r.file)
        return AAR_E_NOENT;
    r.at = 0u;
    size = X_FILE_SIZE(r.file);
    if (size < 0) {
        X_FILE_CLOSE(r.file);
        return AAR_E_IO;
    }
    rc = aar_probe_source(file_read, &r, (uint32_t)size, info, NULL);
    X_FILE_CLOSE(r.file);
    return rc;
}

static LONG v_probe_memory(const void *data, ULONG length, AARImageInfo *info)
{
    return aar_probe_memory(data, length, info);
}

/* ---- Tags ------------------------------------------------------------------------------------------- */

static LONG v_set_tag_list(AARHandle *h, const struct TagItem *tags)
{
    struct aar_core_tags t;
    struct TagItem *item;
    const struct TagItem *list = tags;
    int rc;

    rc = aar_core_check(&g_base->core, h);
    if (rc != AAR_OK)
        return rc;
    aar_core_tags_init(&t);
    while (list && (item = X_NEXT_TAG(&list)) != NULL) {
        rc = aar_core_tags_add(&g_base->core, &t, item->ti_Tag, item->ti_Data);
        if (rc != AAR_OK) {
            /* A bad handle is reported before a bad tag. */
            int hrc = aar_core_check(&g_base->core, h);

            return hrc != AAR_OK ? hrc : rc;
        }
    }
    return aar_core_set_tags(&g_base->core, h, &t);
}

/* ---- Load -------------------------------------------------------------------------------------------- */

static LONG v_load_file(AARHandle *h, const char *path)
{
    struct aar_core *c = &g_base->core;
    volatile uint8_t *staging = NULL;
    uint32_t chunk_max = 0u, off = 0u, n;
    X_FILE f;
    LONG size;
    int rc;

    /* The handle is judged before the path: a revoked claim is AAR_E_STALE,
     * not a missing file, and a missing path is not reported for a stale handle. */
    rc = aar_core_check(c, h);
    if (rc != AAR_OK)
        return rc;
    if (!path)
        return AAR_E_INVAL;
    f = X_FILE_OPEN(path);
    if (!f)
        return AAR_E_NOENT;
    size = X_FILE_SIZE(f);
    if (size <= 0) {
        X_FILE_CLOSE(f);
        return size < 0 ? AAR_E_IO : AAR_E_INVAL;
    }
    rc = aar_core_load_begin(c, h, (uint32_t)size, &staging, &chunk_max);
    while (rc == AAR_OK && off < (uint32_t)size) {
        n = (uint32_t)size - off;
        if (n > chunk_max)
            n = chunk_max;
        if (n > AAR_LOAD_CHUNK_BYTES)
            n = AAR_LOAD_CHUNK_BYTES;
        /* Ctrl-C is honoured between chunks and left set for the caller. */
        if (X_CTRL_C()) {
            rc = AAR_E_IO;
            break;
        }
        if (X_FILE_READ(f, staging, n) != (LONG)n) {
            rc = AAR_E_IO;
            break;
        }
        rc = aar_core_load_chunk(c, h, off, n);
        off += n;
    }
    X_FILE_CLOSE(f);
    if (rc == AAR_OK)
        rc = aar_core_load_end(c, h);
    return rc;
}

static LONG v_load_memory(AARHandle *h, const void *data, ULONG length)
{
    return aar_core_load_memory(&g_base->core, h, data, length);
}

static LONG v_start(AARHandle *h, const char *const *argv, ULONG argc)
{
    return aar_core_start(&g_base->core, h, argv, argc);
}

static LONG v_stop(AARHandle *h)
{
    return aar_core_stop(&g_base->core, h);
}

static LONG v_status(AARHandle *h, AARStatusInfo *status)
{
    return aar_core_status(&g_base->core, h, status);
}

static LONG v_send_message(AARHandle *h, const AARMessage *message)
{
    return aar_core_send_message(&g_base->core, h, message);
}

static LONG v_get_message(AARHandle *h, AARMessage *message)
{
    return aar_core_get_message(&g_base->core, h, message);
}

static ULONG v_message_signal(AARHandle *h)
{
    return aar_core_message_signal(&g_base->core, h);
}

static LONG v_shared_map(AARHandle *h, ULONG haddr, ULONG bytes, ULONG flags,
                         struct AARSharedMap *map)
{
    return aar_core_shared_map(&g_base->core, h, haddr, bytes, flags, map);
}

static LONG v_shared_unmap(AARHandle *h, struct AARSharedMap *map)
{
    return aar_core_shared_unmap(&g_base->core, h, map);
}

static LONG v_shared_publish(AARHandle *h, struct AARSharedMap *map, ULONG offset,
                             ULONG length)
{
    return aar_core_shared_publish(&g_base->core, h, map, offset, length);
}

static LONG v_shared_acquire(AARHandle *h, struct AARSharedMap *map, ULONG offset,
                             ULONG length)
{
    return aar_core_shared_acquire(&g_base->core, h, map, offset, length);
}

static LONG v_service_next(AARHandle *h, AARServiceReq *req)
{
    return aar_core_service_next(&g_base->core, h, req);
}

static LONG v_service_reply(AARHandle *h, const AARServiceReq *req)
{
    return aar_core_service_reply(&g_base->core, h, req);
}

static LONG v_post_event(AARHandle *h, const aar_event *event)
{
    return aar_core_post_event(&g_base->core, h, event);
}

static LONG v_audio_read(AARHandle *h, void *dst, ULONG frames, ULONG *got)
{
    return aar_core_audio_read(&g_base->core, h, dst, frames, got);
}

static LONG v_shared_token(AARHandle *h, const void *ptr, ULONG bytes, ULONG *token)
{
    return aar_core_shared_token(&g_base->core, h, ptr, bytes, token);
}

static APTR v_ext(ULONG platform_id, ULONG ext_id)
{
    /* The extension table needs the platform: without it there is none. */
    if (ensure_dispatcher(g_base) != AAR_OK)
        return (APTR)0;
    return (APTR)(uintptr_t)aar_core_ext(&g_base->core, platform_id, ext_id);
}

/* ====================================================================== */
/* Amiga glue: the library skeleton, the interrupt, the dispatcher process */
/* ====================================================================== */

#ifndef AAR_SHELL_HOST


/* The interrupt server: the core's tick, then a nudge to the dispatcher. */
static ULONG aar_vblank_isr(REG(a1, struct AARBase *b))
{
    shell_vblank(b);
    if (b->disp_task && b->disp_mask)
        Signal(b->disp_task, b->disp_mask);
    return 0;
}

static void aar_vblank_install(struct AARBase *b)
{
    memset(&b->vblank, 0, sizeof(b->vblank));
    b->vblank.is_Node.ln_Type = NT_INTERRUPT;
    b->vblank.is_Node.ln_Pri = 0;
    b->vblank.is_Node.ln_Name = (char *)"aar.library lease";
    b->vblank.is_Data = b;
    b->vblank.is_Code = (void (*)())(uintptr_t)aar_vblank_isr;
    Forbid();
    AddIntServer(INTB_VERTB, &b->vblank);
    Permit();
    b->vblank_installed = 1;
}

static void aar_vblank_remove(struct AARBase *b)
{
    if (!b->vblank_installed)
        return;
    Forbid();
    RemIntServer(INTB_VERTB, &b->vblank);
    Permit();
    b->vblank_installed = 0;
}

/*
 * The dispatcher process. It holds the one open of zz9k.library (the platform's
 * attach), runs a pass for every vertical blank and ends at Ctrl-C from the
 * final CloseLibrary. Its start and its stop are handshakes with the caller.
 */
static void aar_dispatcher_proc(void)
{
    struct AARBase *b = g_base;
    BYTE bit;
    ULONG mask, signals;
    int rc;

    bit = AllocSignal(-1);
    if (bit < 0) {
        b->disp_status = AAR_E_NOMEM;
        Forbid(); /* held until this process has ended */
        b->disp_process = NULL;
        Signal(b->handshake_task, b->handshake_mask);
        return;
    }
    mask = 1UL << bit;
    rc = aar_core_attach(&b->core);
    if (rc != AAR_OK) {
        FreeSignal(bit);
        b->disp_status = rc;
        Forbid();
        b->disp_process = NULL;
        Signal(b->handshake_task, b->handshake_mask);
        return;
    }
    b->disp_task = FindTask(0);
    b->disp_mask = mask;
    b->reap_last = b->core.ticks;
    b->disp_stop = 0;
    /* The gate of the lease interrupt looks at the stamp of the last pass. */
    b->core.disp_alive = b->core.ticks;
    aar_vblank_install(b);
    b->dispatcher_up = 1;
    b->disp_status = AAR_OK;
    Signal(b->handshake_task, b->handshake_mask);

    for (;;) {
        signals = Wait(mask | SIGBREAKF_CTRL_C);
        if ((signals & SIGBREAKF_CTRL_C) || b->disp_stop)
            break;
        shell_pass(b);
    }

    aar_vblank_remove(b);
    b->dispatcher_up = 0;
    b->disp_task = NULL;
    b->disp_mask = 0;
    aar_core_detach(&b->core);
    FreeSignal(bit);
    /* The stopper may continue once the last use of the base is behind us. */
    Forbid();
    b->disp_process = NULL;
    Signal(b->handshake_task, b->handshake_mask);
}

static int aar_x_dispatcher_start(struct AARBase *b)
{
    struct Process *process;
    BYTE bit;
    int rc;

    bit = AllocSignal(-1);
    if (bit < 0)
        return AAR_E_NOMEM;
    ObtainSemaphore(&b->lock);
    if (b->dispatcher_up || b->disp_process) {
        ReleaseSemaphore(&b->lock);
        FreeSignal(bit);
        return b->dispatcher_up ? AAR_OK : AAR_E_BUSY;
    }
    b->handshake_task = FindTask(0);
    b->handshake_mask = 1UL << bit;
    b->disp_status = AAR_E_BUSY;
    /* Under Forbid, so the new process cannot finish (and clear disp_process)
     * before this task has recorded it. */
    Forbid();
    process = CreateNewProcTags(NP_Entry, (ULONG)aar_dispatcher_proc,
                                NP_Name, (ULONG)"aar.library dispatcher",
                                NP_StackSize, 8192,
                                NP_Priority, AAR_DISPATCH_PRIORITY, TAG_DONE);
    if (process)
        b->disp_process = process;
    Permit();
    ReleaseSemaphore(&b->lock);
    if (!process) {
        FreeSignal(bit);
        return AAR_E_NOMEM;
    }
    Wait(b->handshake_mask);
    rc = b->disp_status;
    b->handshake_task = NULL;
    b->handshake_mask = 0;
    FreeSignal(bit);
    return rc;
}

static void aar_x_dispatcher_stop(struct AARBase *b)
{
    BYTE bit;
    int waited = 0;

    if (!b->disp_process)
        return;
    bit = AllocSignal(-1);
    if (bit >= 0) {
        b->handshake_task = FindTask(0);
        b->handshake_mask = 1UL << bit;
        waited = 1;
    }
    b->disp_stop = 1;
    if (b->disp_task)
        Signal(b->disp_task, SIGBREAKF_CTRL_C);
    if (waited) {
        Wait(b->handshake_mask);
        b->handshake_task = NULL;
        b->handshake_mask = 0;
        FreeSignal(bit);
    }
}

static int aar_x_open_system(void)
{
    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!DOSBase)
        return 0;
    /* GCC's 32-bit multiply and divide helpers jump through UtilityBase.
     * -nostartfiles skips the startup code that opens it. */
    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 37);
    if (!UtilityBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return 0;
    }
    return 1;
}

static void aar_x_close_system(void)
{
    if (DOSBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
}

static struct AARBase *aar_lib_open(REG(a6, struct AARBase *base));
static BPTR aar_lib_close(REG(a6, struct AARBase *base));
static BPTR aar_lib_expunge(REG(a6, struct AARBase *base));
static ULONG aar_lib_null(void);
static struct AARBase *aar_lib_init(REG(a0, BPTR segment));

/* The vector entries: registers as in client/fd/aar_lib.fd. */
static LONG f_query(REG(a0, AARInfo *info))
{
    return v_query(info);
}

static LONG f_open(REG(a0, AARHandle **handlep), REG(a1, struct MsgPort *port),
                   REG(a2, ULONG *reason))
{
    return v_open(handlep, port, reason);
}

static LONG f_close(REG(a0, AARHandle *handle))
{
    return v_close(handle);
}

static LONG f_probe_file(REG(a0, const char *path), REG(a1, AARImageInfo *info))
{
    return v_probe_file(path, info);
}

static LONG f_probe_memory(REG(a0, const void *data), REG(d0, ULONG length),
                           REG(a1, AARImageInfo *info))
{
    return v_probe_memory(data, length, info);
}

static LONG f_set_tag_list(REG(a0, AARHandle *handle), REG(a1, const struct TagItem *tags))
{
    return v_set_tag_list(handle, tags);
}

static LONG f_load_file(REG(a0, AARHandle *handle), REG(a1, const char *path))
{
    return v_load_file(handle, path);
}

static LONG f_load_memory(REG(a0, AARHandle *handle), REG(a1, const void *data),
                          REG(d0, ULONG length))
{
    return v_load_memory(handle, data, length);
}

static LONG f_start(REG(a0, AARHandle *handle), REG(a1, const char *const *argv),
                    REG(d0, ULONG argc))
{
    return v_start(handle, argv, argc);
}

static LONG f_stop(REG(a0, AARHandle *handle))
{
    return v_stop(handle);
}

static LONG f_status(REG(a0, AARHandle *handle), REG(a1, AARStatusInfo *status))
{
    return v_status(handle, status);
}

static LONG f_send_message(REG(a0, AARHandle *handle), REG(a1, const AARMessage *message))
{
    return v_send_message(handle, message);
}

static LONG f_get_message(REG(a0, AARHandle *handle), REG(a1, AARMessage *message))
{
    return v_get_message(handle, message);
}

static ULONG f_message_signal(REG(a0, AARHandle *handle))
{
    return v_message_signal(handle);
}

static LONG f_shared_map(REG(a0, AARHandle *handle), REG(d0, ULONG haddr),
                         REG(d1, ULONG bytes), REG(d2, ULONG flags),
                         REG(a1, struct AARSharedMap *map))
{
    return v_shared_map(handle, haddr, bytes, flags, map);
}

static LONG f_shared_unmap(REG(a0, AARHandle *handle), REG(a1, struct AARSharedMap *map))
{
    return v_shared_unmap(handle, map);
}

static LONG f_shared_publish(REG(a0, AARHandle *handle), REG(a1, struct AARSharedMap *map),
                             REG(d0, ULONG offset), REG(d1, ULONG length))
{
    return v_shared_publish(handle, map, offset, length);
}

static LONG f_shared_acquire(REG(a0, AARHandle *handle), REG(a1, struct AARSharedMap *map),
                             REG(d0, ULONG offset), REG(d1, ULONG length))
{
    return v_shared_acquire(handle, map, offset, length);
}

static LONG f_service_next(REG(a0, AARHandle *handle), REG(a1, AARServiceReq *req))
{
    return v_service_next(handle, req);
}

static LONG f_service_reply(REG(a0, AARHandle *handle), REG(a1, const AARServiceReq *req))
{
    return v_service_reply(handle, req);
}

static LONG f_post_event(REG(a0, AARHandle *handle), REG(a1, const aar_event *event))
{
    return v_post_event(handle, event);
}

static LONG f_audio_read(REG(a0, AARHandle *handle), REG(a1, void *dst),
                         REG(d0, ULONG frames), REG(a2, ULONG *got))
{
    return v_audio_read(handle, dst, frames, got);
}

static LONG f_shared_token(REG(a0, AARHandle *handle), REG(a1, const void *ptr),
                           REG(d0, ULONG bytes), REG(a2, ULONG *token))
{
    return v_shared_token(handle, ptr, bytes, token);
}

static APTR f_ext(REG(d0, ULONG platform_id), REG(d1, ULONG ext_id))
{
    return v_ext(platform_id, ext_id);
}

/* -6 to -24 are Open, Close, Expunge and the reserved slot, then -30 on in the
 * order of the .fd file. */
static const APTR aar_lib_vectors[] = {
    AAR_VEC(aar_lib_open),
    AAR_VEC(aar_lib_close),
    AAR_VEC(aar_lib_expunge),
    AAR_VEC(aar_lib_null),
    AAR_VEC(f_query),
    AAR_VEC(f_open),
    AAR_VEC(f_close),
    AAR_VEC(f_probe_file),
    AAR_VEC(f_probe_memory),
    AAR_VEC(f_set_tag_list),
    AAR_VEC(f_load_file),
    AAR_VEC(f_load_memory),
    AAR_VEC(f_start),
    AAR_VEC(f_stop),
    AAR_VEC(f_status),
    AAR_VEC(f_send_message),
    AAR_VEC(f_get_message),
    AAR_VEC(f_message_signal),
    AAR_VEC(f_shared_map),
    AAR_VEC(f_shared_unmap),
    AAR_VEC(f_shared_publish),
    AAR_VEC(f_shared_acquire),
    AAR_VEC(f_service_next),
    AAR_VEC(f_service_reply),
    AAR_VEC(f_post_event),
    AAR_VEC(f_audio_read),
    AAR_VEC(f_shared_token),
    AAR_VEC(f_ext),
    (APTR)-1
};

static struct AARBase *aar_lib_open(REG(a6, struct AARBase *base))
{
    if (!base || shell_open(base) != AAR_OK)
        return NULL;
    base->library.lib_OpenCnt++;
    base->library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return base;
}

static BPTR aar_lib_close(REG(a6, struct AARBase *base))
{
    if (!base || base->library.lib_OpenCnt == 0)
        return 0;
    (void)shell_close(base);
    base->library.lib_OpenCnt--;
    if (base->library.lib_OpenCnt == 0) {
        shell_final_close(base);
        if (base->library.lib_Flags & LIBF_DELEXP)
            return aar_lib_expunge(base);
    }
    return 0;
}

static BPTR aar_lib_expunge(REG(a6, struct AARBase *base))
{
    BPTR segment;

    if (!base)
        return 0;
    if (base->library.lib_OpenCnt != 0) {
        base->library.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    Remove((struct Node *)base);
    aar_x_close_system();
    segment = base->segment;
    g_base = NULL;
    FreeMem((UBYTE *)base - base->library.lib_NegSize,
            base->library.lib_NegSize + base->library.lib_PosSize);
    return segment;
}

static ULONG aar_lib_null(void)
{
    return 0;
}

static struct AARBase *aar_lib_init(REG(a0, BPTR segment))
{
    struct AARBase *base;

    if (!aar_x_open_system())
        return NULL;
    base = (struct AARBase *)MakeLibrary((CONST_APTR)aar_lib_vectors, 0, 0, sizeof(*base), 0);
    if (!base) {
        aar_x_close_system();
        return NULL;
    }
    base->library.lib_Node.ln_Type = NT_LIBRARY;
    base->library.lib_Node.ln_Name = (char *)AAR_LIB_NAME;
    base->library.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
    base->library.lib_Version = AAR_LIB_VERSION;
    base->library.lib_Revision = AAR_LIB_REVISION;
    base->library.lib_IdString = (APTR)AAR_LIB_IDSTRING;
    base->segment = segment;
    if (shell_init(base) != AAR_OK) {
        FreeMem((UBYTE *)base - base->library.lib_NegSize,
                base->library.lib_NegSize + base->library.lib_PosSize);
        aar_x_close_system();
        return NULL;
    }
    AddLibrary((struct Library *)base);
    return base;
}

static const struct Resident aar_romtag __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&aar_romtag,
    (APTR)(&aar_romtag + 1),
    0,
    AAR_LIB_VERSION,
    NT_LIBRARY,
    0,
    (char *)AAR_LIB_NAME,
    (char *)AAR_LIB_IDSTRING,
    AAR_VEC(aar_lib_init)
};

#endif /* !AAR_SHELL_HOST */
