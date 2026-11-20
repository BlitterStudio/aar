/*
 * The fake system under the host test of the AmigaOS shell (client/src/aar_shell.c).
 *
 * aar_shell.c reaches Exec, dos.library and utility.library only through X_*
 * bindings. This header, included by it when AAR_SHELL_HOST is defined,
 * implements them with a small simulated machine: a table of tasks that the
 * test can start, kill and give another name, signal bits, message ports, a
 * counted Disable() and semaphore, a pool of memory below 4 GiB, files in
 * memory and a tag list walker. Each binding checks the rule the real call
 * has (a message is never posted twice, a semaphore is released as often as
 * it was obtained, memory is not used after it is freed) and aborts the test
 * when the shell breaks it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SHELL_HOST_H
#define SHELL_HOST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/ports.h>
#include <exec/types.h>
#include <utility/tagitem.h>

#define HX_FAIL(msg)                                                    \
    do {                                                                \
        fprintf(stderr, "fake system: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        abort();                                                        \
    } while (0)

#define NT_MESSAGE 5

struct Library {
    UWORD lib_OpenCnt;
    UBYTE lib_Flags;
};

/* ---- Tasks and signals ------------------------------------------------------ */

#define HX_TASKS 16
#define HX_DISP 15    /* the task of the dispatcher process */
#define HX_SIGBITS 12 /* a task has about a dozen free signal bits */
#define SIGF_SINGLE (1u << 4)       /* Exec's one-shot signal: never handed out by AllocSignal */
#define SIGBREAKF_CTRL_C (1u << 12)

struct hx_task {
    int used;
    int listed;                  /* on one of Exec's task lists */
    const void *name;
    uint32_t number;
    uint32_t kind;
    uint32_t signals;            /* pending */
    uint32_t sig_alloc;          /* bits allocated */
    uint32_t signalled;          /* Signal() calls received */
};

struct hx_task hx_tasks[HX_TASKS];
struct hx_task *hx_cur;

/* ---- The counted locks --------------------------------------------------------- */

struct hx_sem {
    int excl, shared;
    unsigned long obtains;
};

int hx_disabled;
int hx_disable_max;
unsigned long hx_disable_calls;
unsigned long hx_delays;
unsigned long hx_dispatcher_starts, hx_dispatcher_stops;
int hx_forbid;                   /* Forbid() nesting of the running task */
unsigned long hx_waits;
/* Exec's semaphores wait on SIGF_SINGLE too: a contended ObtainSemaphore of a
 * task that has the bit set returns without owning the semaphore. The fake
 * counts the obtains made with the bit set (and takes the bit, as that Wait
 * would). */
unsigned long hx_stale_obtains;
/* Called after every Signal(): the test lets a higher-priority process run at
 * once. And: whether a SIGF_SINGLE to this task is expected (a handshake is in
 * progress); the fake aborts on one that is not. */
void (*hx_on_signal)(const void *task, uint32_t mask);
int (*hx_single_ok)(const void *task);
int hx_spurious;                 /* the next n Waits end early, with nothing happened */
void (*hx_bg)(void);             /* what the other processes do while the current task waits */

#define HX_UNUSED __attribute__((unused))

HX_UNUSED static void hx_sem_obtain(struct hx_sem *s, int shared)
{
    if (hx_cur->signals & SIGF_SINGLE) {
        hx_stale_obtains++;
        hx_cur->signals &= ~SIGF_SINGLE;
    }
    if (shared) {
        if (s->excl)
            HX_FAIL("shared obtain while exclusively held (one thread would deadlock)");
        s->shared++;
    } else {
        if (s->excl || s->shared)
            HX_FAIL("exclusive obtain while held (one thread would deadlock)");
        s->excl = 1;
    }
    s->obtains++;
}

HX_UNUSED static void hx_sem_release(struct hx_sem *s)
{
    if (s->excl)
        s->excl = 0;
    else if (s->shared)
        s->shared--;
    else
        HX_FAIL("release of a semaphore that is not held");
}

HX_UNUSED static void hx_disable(void)
{
    hx_disabled++;
    hx_disable_calls++;
    if (hx_disabled > hx_disable_max)
        hx_disable_max = hx_disabled;
}

HX_UNUSED static void hx_enable(void)
{
    if (hx_disabled <= 0)
        HX_FAIL("Enable without Disable");
    hx_disabled--;
}

/* ---- Memory below 4 GiB, from a static pool ----------------------------------- */

#define HX_SLOT 8192u
#define HX_SLOTS 640u

static uint8_t hx_pool[HX_SLOTS][HX_SLOT] __attribute__((aligned(16)));
static uint8_t hx_pool_used[HX_SLOTS];
unsigned long hx_live_blocks;
unsigned long hx_live_bytes;     /* bytes allocated and not freed */
static uint32_t hx_pool_size[HX_SLOTS];
unsigned long hx_alloc_calls;
int hx_fail_alloc;               /* the next n allocations fail */

HX_UNUSED static void *hx_alloc(uint32_t bytes)
{
    uint32_t i;

    hx_alloc_calls++;
    if (hx_fail_alloc > 0) {
        hx_fail_alloc--;
        return NULL;
    }
    if (bytes > HX_SLOT)
        HX_FAIL("allocation larger than a pool slot");
    for (i = 0u; i < HX_SLOTS; i++) {
        if (!hx_pool_used[i]) {
            hx_pool_used[i] = 1u;
            hx_live_blocks++;
            hx_live_bytes += bytes;
            hx_pool_size[i] = bytes;
            memset(hx_pool[i], 0, HX_SLOT);
            return hx_pool[i];
        }
    }
    return NULL;
}

HX_UNUSED static void hx_free(void *p)
{
    uint32_t i;

    for (i = 0u; i < HX_SLOTS; i++) {
        if ((void *)hx_pool[i] == p) {
            if (!hx_pool_used[i])
                HX_FAIL("double free");
            hx_pool_used[i] = 0u;
            hx_live_blocks--;
            hx_live_bytes -= hx_pool_size[i];
            /* A pointer that is used after this shows at once. */
            memset(hx_pool[i], 0xA5, HX_SLOT);
            return;
        }
    }
    HX_FAIL("free of a block the pool does not own");
}

/* ---- Message ports --------------------------------------------------------------- */

struct MsgPort {
    struct Message *queue[8];
    int n;
    unsigned long posts;
};

#define HX_PORTS 8
static struct MsgPort *hx_ports[HX_PORTS];

HX_UNUSED static void hx_port_register(struct MsgPort *p)
{
    int i;

    for (i = 0; i < HX_PORTS; i++) {
        if (!hx_ports[i]) {
            hx_ports[i] = p;
            return;
        }
    }
    HX_FAIL("too many ports");
}

HX_UNUSED static int hx_msg_queued(struct MsgPort *p, struct Message *m)
{
    int i;

    for (i = 0; i < p->n; i++) {
        if (p->queue[i] == m)
            return 1;
    }
    return 0;
}

HX_UNUSED static void hx_putmsg(struct MsgPort *p, struct Message *m)
{
    if (hx_msg_queued(p, m))
        HX_FAIL("PutMsg of a message that is already on the port");
    if (p->n >= 8)
        HX_FAIL("port overflow");
    p->queue[p->n++] = m;
    p->posts++;
}

/* GetMsg of the test: takes the oldest message. */
HX_UNUSED static struct Message *hx_getmsg(struct MsgPort *p)
{
    struct Message *m;
    int i;

    if (p->n == 0)
        return NULL;
    m = p->queue[0];
    for (i = 1; i < p->n; i++)
        p->queue[i - 1] = p->queue[i];
    p->n--;
    return m;
}

HX_UNUSED static void hx_msg_remove(struct Message *m)
{
    int i, j;

    for (i = 0; i < HX_PORTS; i++) {
        struct MsgPort *p = hx_ports[i];

        if (!p)
            continue;
        for (j = 0; j < p->n; j++) {
            if (p->queue[j] == m) {
                for (; j + 1 < p->n; j++)
                    p->queue[j] = p->queue[j + 1];
                p->n--;
                return;
            }
        }
    }
}

HX_UNUSED static void hx_msg_init(struct Message *m, UWORD len)
{
    m->mn_Node.ln_Type = NT_MESSAGE;
    m->mn_ReplyPort = 0;
    m->mn_Length = len;
}

/* ---- Files and Ctrl-C ----------------------------------------------------------------- */

struct hx_file {
    const char *path;
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
    int open;
};

#define HX_FILES 8
struct hx_file hx_files[HX_FILES];
unsigned long hx_file_reads;
unsigned long hx_files_open;
int hx_ctrl_c;                    /* SIGBREAKF_CTRL_C of the current task */
int hx_ctrl_c_after_reads;        /* set Ctrl-C when this many reads are done (0: never) */
int hx_fail_read_at;              /* the read with this number (1-based) fails (0: none) */

HX_UNUSED static struct hx_file *hx_file_open(const char *path)
{
    int i;

    for (i = 0; i < HX_FILES; i++) {
        if (hx_files[i].path && strcmp(hx_files[i].path, path) == 0) {
            hx_files[i].pos = 0u;
            hx_files[i].open = 1;
            hx_files_open++;
            return &hx_files[i];
        }
    }
    return NULL;
}

HX_UNUSED static long hx_file_read(struct hx_file *f, void *dst, uint32_t n)
{
    hx_file_reads++;
    if (hx_fail_read_at != 0 && (unsigned long)hx_fail_read_at == hx_file_reads)
        return -1;
    if (f->pos + n > f->len)
        n = f->len - f->pos;
    memcpy(dst, f->data + f->pos, n);
    f->pos += n;
    if (hx_ctrl_c_after_reads != 0 && (unsigned long)hx_ctrl_c_after_reads == hx_file_reads)
        hx_ctrl_c = 1;
    return (long)n;
}

HX_UNUSED static void hx_file_close(struct hx_file *f)
{
    if (!f->open)
        HX_FAIL("close of a file that is not open");
    f->open = 0;
    hx_files_open--;
}

/* NextTagItem of utility.library. */
HX_UNUSED static struct TagItem *hx_next_tag(struct TagItem **listp)
{
    struct TagItem *t = *listp;

    while (t) {
        switch (t->ti_Tag) {
        case TAG_DONE:
            *listp = NULL;
            return NULL;
        case TAG_IGNORE:
            t++;
            break;
        case TAG_MORE:
            t = (struct TagItem *)(uintptr_t)t->ti_Data;
            break;
        case TAG_SKIP:
            t += t->ti_Data + 1u;
            break;
        default:
            *listp = t + 1;
            return t;
        }
    }
    *listp = NULL;
    return NULL;
}

/* ---- The tasks seen by the shell ------------------------------------------------------- */

HX_UNUSED static void hx_stamp(struct hx_task *t, aar_owner_stamp *st)
{
    st->id = t;
    st->name = t->name;
    st->number = t->number;
    st->kind = t->kind;
}

HX_UNUSED static int hx_task_lookup(const void *wanted, aar_owner_stamp *st)
{
    struct hx_task *t = (struct hx_task *)(uintptr_t)wanted;
    int i;

    for (i = 0; i < HX_TASKS; i++) {
        if (&hx_tasks[i] == t) {
            if (!t->used || !t->listed)
                return 0;
            hx_stamp(t, st);
            return 1;
        }
    }
    return 0;
}

HX_UNUSED static int hx_alloc_signal(void)
{
    int bit;

    /* Bit 4 is SIGF_SINGLE, which AllocSignal never returns. */
    for (bit = 0; bit < HX_SIGBITS; bit++) {
        if (!(hx_cur->sig_alloc & (1u << (bit + 5)))) {
            hx_cur->sig_alloc |= 1u << (bit + 5);
            return bit + 5;
        }
    }
    return -1;
}

HX_UNUSED static void hx_free_signal(int bit)
{
    if (!(hx_cur->sig_alloc & (1u << bit)))
        HX_FAIL("FreeSignal of a bit that is not allocated");
    hx_cur->sig_alloc &= ~(1u << bit);
}

HX_UNUSED static void hx_signal(const void *task, uint32_t mask)
{
    struct hx_task *t = (struct hx_task *)(uintptr_t)task;
    aar_owner_stamp st;

    /* Signalling a task that is gone writes into freed memory. */
    if (!hx_task_lookup(task, &st))
        HX_FAIL("Signal to a task that is not on a task list");
    if (mask == SIGF_SINGLE && hx_single_ok && !hx_single_ok(task))
        HX_FAIL("SIGF_SINGLE sent to a task that is not in a handshake");
    t->signals |= mask;
    if (mask != SIGF_SINGLE) /* `signalled` counts notifications, not handshakes */
        t->signalled++;
    if (hx_on_signal)
        hx_on_signal(task, mask);
}

/*
 * Wait() of the current task. A signal that is pending ends it at once. Else
 * the other processes get to run (hx_bg: the dispatcher's next step), and if
 * still nothing is pending the task would sleep for ever: the test aborts
 * instead of hanging.
 */
HX_UNUSED static uint32_t hx_wait(uint32_t mask)
{
    uint32_t got;

    hx_waits++;
    if (hx_spurious > 0) {
        hx_spurious--;
        hx_cur->signals |= mask; /* a stale or early signal */
    }
    if (!(hx_cur->signals & mask) && hx_bg) {
        void (*run)(void) = hx_bg;

        hx_bg = NULL;
        run();
    }
    got = hx_cur->signals & mask;
    if (!got)
        HX_FAIL("Wait() that no signal will ever end");
    hx_cur->signals &= ~got;
    return got;
}

/* ---- The bindings ----------------------------------------------------------------------- */

#define X_SEMTYPE struct hx_sem
#define X_SEM_INIT(s) memset((s), 0, sizeof(*(s)))
#define X_SEM_OBTAIN(s) hx_sem_obtain((s), 0)
#define X_SEM_OBTAIN_SHARED(s) hx_sem_obtain((s), 1)
#define X_SEM_RELEASE(s) hx_sem_release(s)
#define X_DISABLE() hx_disable()
#define X_ENABLE() hx_enable()
#define X_SELF() ((void *)hx_cur)
#define X_ALLOC(bytes) hx_alloc(bytes)
#define X_FREE(p, bytes) hx_free(p)
#define X_SIGNAL(task, mask) hx_signal((task), (mask))
#define X_ALLOC_SIGNAL() hx_alloc_signal()
#define X_FREE_SIGNAL(bit) hx_free_signal(bit)
#define X_DELAY1() (hx_delays++)
#define X_PUTMSG(port, msg) hx_putmsg((struct MsgPort *)(port), (struct Message *)(msg))
#define X_MSG_INIT(msg, len) hx_msg_init((struct Message *)(msg), (UWORD)(len))
#define X_MSG_QUEUED(port, msg) hx_msg_queued((struct MsgPort *)(port), (struct Message *)(msg))
#define X_MSG_REMOVE(msg) hx_msg_remove((struct Message *)(msg))
#define X_FILE struct hx_file *
#define X_FILE_OPEN(path) hx_file_open(path)
#define X_FILE_SIZE(f) ((long)(f)->len)
#define X_FILE_SEEK(f, off) (((f)->pos = (uint32_t)(off)), 1)
#define X_FILE_READ(f, dst, n) hx_file_read((f), (void *)(uintptr_t)(dst), (n))
#define X_FILE_CLOSE(f) hx_file_close(f)
#define X_CTRL_C() (hx_ctrl_c != 0)
#define X_NEXT_TAG(listp) hx_next_tag((struct TagItem **)(listp))
#define X_TASK_LOOKUP(task, st) hx_task_lookup((task), (st))
#define X_TASK_LOOKUP_LOCKED(task, st) hx_task_lookup((task), (st))
#define X_STAMP(task, st) hx_stamp((struct hx_task *)(uintptr_t)(task), (st))
#define X_FORBID() (hx_forbid++)
#define X_PERMIT() \
    do {                                                       \
        if (hx_forbid <= 0)                                    \
            HX_FAIL("Permit without Forbid");                  \
        hx_forbid--;                                           \
    } while (0)
#define X_WAIT(mask) hx_wait(mask)
#define X_SINGLE SIGF_SINGLE
#define X_BREAK SIGBREAKF_CTRL_C
#define X_SINGLE_CLEAR() (hx_cur->signals &= ~SIGF_SINGLE)
#define X_SPAWN(base) hx_spawn(base)
#define X_VBLANK_INSTALL(base) hx_vblank_install(base)
#define X_VBLANK_REMOVE(base) hx_vblank_remove(base)

struct AARBase;
static int hx_spawn(struct AARBase *base);
static void hx_vblank_install(struct AARBase *base);
static void hx_vblank_remove(struct AARBase *base);

#endif /* SHELL_HOST_H */
