/*
 * Host test of the leak probe (client/tests/amiga/aarleak.c). The probe is
 * compiled against a model of the AmigaOS calls it makes (AARLEAK_HOST,
 * client/tests/aarleak_host.h) and run through its own entry point, leak_main.
 *
 * The model runs the children as threads, so child_main is the probe's real
 * code, and it can fail every call the probe makes, the Nth time it is made. The
 * table below fails each call of each mode in turn and requires what the probe
 * promises: a failing exit status (10; 20 when the library cannot be opened),
 * every process that started released and joined, no handle, file, message port
 * or library opening left, and no wait for a process that does not exist. The
 * same table with no failure must run clean, so that the failures are the only
 * reason for the status.
 *
 * SPDX-License-Identifier: MIT
 */
#define AARLEAK_HOST 1
#define AARLEAK_WAIT_TICKS 5000UL
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <time.h>

#include "amiga/aarleak.c"

static unsigned long checks, failures;
static const char *cur_test = "";

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("FAIL [%s] %s:%d: %s\n", cur_test, __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

/* ---- The model --------------------------------------------------------------------- */

enum {
    F_NONE = 0,
    F_OPENLIBRARY,
    F_AAROPEN,
    F_AARCLOSE,
    F_SIGNAL,
    F_PROBE,
    F_EXT,
    F_EXTLIST,
    F_LISTEND, /* the claim list ends early: it answers 0 at the Nth call */
    F_CREATEPORT,
    F_OPEN,
    F_SEEK,
    F_READ,
    F_CLOSE,
    F_CREATEPROC,
    F_STALL, /* the Nth OpenLibrary, made by a child, does not return until released */
    F_COUNT
};

#define MAXPROC 16
#define MAXFNS 4
#define MAX_CLAIMS 8       /* the reference platform */
#define DELAY_LIMIT 20000UL

static pthread_mutex_t model_lock = PTHREAD_MUTEX_INITIALIZER; /* Forbid/Permit and the model's state */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int inject_kind, inject_at;
static int calls[F_COUNT];
static volatile int stall_release;
static int libs_open, handles_open, ports_open, files_open;
static int started;
static pthread_t threads[MAXPROC];
static volatile int proc_exited[MAXPROC];
static unsigned long delays;
static jmp_buf *hang_env;
static void (*fns[MAXFNS])(void);
static int nfns;
static void (*proc_entry[MAXPROC])(void);
static __thread int in_child;

/* True when the call being made is the one the test fails. */
static int injected(int kind)
{
    int hit;

    pthread_mutex_lock(&state_lock);
    calls[kind]++;
    hit = inject_kind == kind && calls[kind] == inject_at;
    pthread_mutex_unlock(&state_lock);
    return hit;
}

static void model_add(int *counter, int delta)
{
    pthread_mutex_lock(&state_lock);
    *counter += delta;
    pthread_mutex_unlock(&state_lock);
}

void Forbid(void)
{
    pthread_mutex_lock(&model_lock);
}

void Permit(void)
{
    pthread_mutex_unlock(&model_lock);
}

ULONG aarleak_fn_id(void (*fn)(void))
{
    int i;

    for (i = 0; i < nfns; i++)
        if (fns[i] == fn)
            return (ULONG)i + 1u;
    if (nfns >= MAXFNS)
        abort();
    fns[nfns] = fn;
    return (ULONG)nfns++ + 1u;
}

ULONG AvailMem(ULONG attributes)
{
    (void)attributes;
    return 1000000UL;
}

BPTR Open(STRPTR name, LONG mode)
{
    (void)name;
    (void)mode;
    if (injected(F_OPEN))
        return 0;
    model_add(&files_open, 1);
    return 1;
}

LONG Seek(BPTR file, LONG position, LONG mode)
{
    (void)file;
    (void)position;
    (void)mode;
    return injected(F_SEEK) ? -1 : 0;
}

LONG Read(BPTR file, void *buffer, LONG length)
{
    (void)file;
    (void)buffer;
    (void)length;
    return injected(F_READ) ? -1 : 0;
}

LONG Close(BPTR file)
{
    (void)file;
    model_add(&files_open, -1); /* a close that fails still releases the file */
    return injected(F_CLOSE) ? 0 : 1;
}

struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    int stall = 0;

    (void)name;
    (void)version;
    pthread_mutex_lock(&state_lock);
    calls[F_OPENLIBRARY]++;
    if (inject_kind == F_OPENLIBRARY && calls[F_OPENLIBRARY] == inject_at) {
        pthread_mutex_unlock(&state_lock);
        return NULL;
    }
    calls[F_STALL]++;
    stall = inject_kind == F_STALL && calls[F_STALL] == inject_at && in_child;
    pthread_mutex_unlock(&state_lock);
    while (stall && !stall_release)
        Delay(1);
    model_add(&libs_open, 1);
    return (struct Library *)(uintptr_t)0x1000u;
}

void CloseLibrary(struct Library *library)
{
    if (library)
        model_add(&libs_open, -1);
}

struct MsgPort *CreateMsgPort(void)
{
    if (injected(F_CREATEPORT))
        return NULL;
    model_add(&ports_open, 1);
    return (struct MsgPort *)(uintptr_t)0x2000u;
}

void DeleteMsgPort(struct MsgPort *port)
{
    if (port)
        model_add(&ports_open, -1);
}

LONG AAROpen(AARHandle **handlep, struct MsgPort *port, ULONG *reason)
{
    int full;

    (void)port;
    (void)reason;
    pthread_mutex_lock(&state_lock);
    full = handles_open >= MAX_CLAIMS;
    pthread_mutex_unlock(&state_lock);
    if (injected(F_AAROPEN) || full)
        return AAR_E_NORESOURCE;
    model_add(&handles_open, 1);
    *handlep = (AARHandle *)(uintptr_t)0x3000u;
    return AAR_OK;
}

LONG AARClose(AARHandle *handle)
{
    (void)handle;
    model_add(&handles_open, -1); /* a close that fails still frees the handle */
    return injected(F_AARCLOSE) ? AAR_E_IO : AAR_OK;
}

ULONG AARMessageSignal(AARHandle *handle)
{
    (void)handle;
    return injected(F_SIGNAL) ? 0UL : 0x100UL;
}

LONG AARProbeFile(const char *path, AARImageInfo *info)
{
    (void)path;
    (void)info;
    return injected(F_PROBE) ? AAR_E_IO : AAR_OK;
}

static LONG model_list(ULONG cursor, AARZZ9000List *info)
{
    int held;

    if (injected(F_EXTLIST))
        return AAR_E_IO;
    if (injected(F_LISTEND))
        return 0;
    pthread_mutex_lock(&state_lock);
    held = handles_open;
    pthread_mutex_unlock(&state_lock);
    if (cursor >= (ULONG)held)
        return 0;
    info->index = cursor;
    return 1;
}

static aar_client_ext_zz9000 model_ext;

APTR AARExt(ULONG platform_id, ULONG ext_id)
{
    (void)platform_id;
    (void)ext_id;
    if (injected(F_EXT))
        return 0;
    model_ext.list = model_list;
    return 1u; /* an identifier, see aarleak_ptr */
}

const void *aarleak_ptr(APTR id)
{
    return id == 1u ? (const void *)&model_ext : NULL;
}

static void *trampoline(void *arg)
{
    int slot = (int)(intptr_t)arg;

    in_child = 1;
    proc_entry[slot]();
    proc_exited[slot] = 1;
    return NULL;
}

struct Process *CreateNewProcTags(ULONG tag, ...)
{
    va_list ap;
    ULONG entry = 0, t = tag, v;
    int slot;

    if (injected(F_CREATEPROC))
        return NULL;
    va_start(ap, tag);
    while (t != TAG_DONE) {
        v = va_arg(ap, ULONG);
        if (t == NP_Entry)
            entry = v;
        t = va_arg(ap, ULONG);
    }
    va_end(ap);
    if (entry == 0u || started >= MAXPROC)
        abort();
    slot = started++;
    proc_entry[slot] = fns[entry - 1u];
    if (pthread_create(&threads[slot], NULL, trampoline, (void *)(intptr_t)slot) != 0)
        abort();
    return (struct Process *)(uintptr_t)(0x4000u + (unsigned)slot);
}

/* Time passes. The parent's waits are counted: a probe that waits for a
 * process that does not exist would sleep for ever on the Amiga, and the model
 * stops it and reports a hang. */
void Delay(ULONG ticks)
{
    struct timespec ts = { 0, 200000 };

    (void)ticks;
    if (!in_child && ++delays > DELAY_LIMIT && hang_env)
        longjmp(*hang_env, 1);
    nanosleep(&ts, NULL);
}

/* ---- The tests ------------------------------------------------------------------- */

static void reset(int kind, int at)
{
    memset(calls, 0, sizeof(calls));
    inject_kind = kind;
    inject_at = at;
    stall_release = 0;
    libs_open = handles_open = ports_open = files_open = 0;
    started = 0;
    nfns = 0;
    memset((void *)proc_exited, 0, sizeof(proc_exited));
    delays = 0;
    child_go = 0;
    child_hold = 0;
    child_ready = child_exited = 0;
    child_failed = 0;
}

struct run_result {
    int rc;
    int hung;
    int alive_at_return;   /* processes still running when the probe returned */
    int released;          /* child_go was given */
    unsigned long delays;
};

/* Runs the probe; then releases anything held by the model, joins the threads
 * and reports what was left. */
static struct run_result run_probe(int argc, char **argv, int want_alive)
{
    struct run_result r;
    jmp_buf env;
    int i;

    memset(&r, 0, sizeof(r));
    hang_env = &env;
    delays = 0;
    if (setjmp(env) != 0) {
        r.hung = 1;
        r.rc = -1;
    } else {
        r.rc = leak_main(argc, argv);
    }
    hang_env = NULL;
    r.delays = delays;
    r.released = child_go;
    /* A child that was told to go or never waited ends within moments: give the
     * threads time to run to their end. One that nobody released stays blocked
     * however long this waits. */
    for (i = 0; i < 300; i++) {
        struct timespec ts = { 0, 1000000 };
        int alive = 0, k;

        for (k = 0; k < started; k++)
            if (!proc_exited[k])
                alive++;
        if (alive <= want_alive)
            break;
        nanosleep(&ts, NULL);
    }
    for (i = 0; i < started; i++)
        if (!proc_exited[i])
            r.alive_at_return++;
    stall_release = 1;
    child_go = 1;
    for (i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    return r;
}

/*
 * One case: the mode and its arguments (after the library path), the call that
 * fails (kind) and which call of its kind it is (at, counted from 1 over the
 * whole run, the warm-up pass included), the exit status required, and whether
 * a child is still waiting when the probe returns (a stalled one: it never
 * answered, so it cannot be joined, but it has been told to go).
 */
struct tcase {
    const char *name;
    const char *args[4];
    int kind, at, rc, stalled;
};

#define LIB "aar.library"
static const struct tcase cases[] = {
    /* clean runs: the status is 0 */
    { "churn clean", { "churn", "3" }, F_NONE, 0, 0, 0 },
    { "notify clean", { "notify", "3" }, F_NONE, 0, 0, 0 },
    { "list clean", { "list", "3" }, F_NONE, 0, 0, 0 },
    { "probe clean", { "probe", "f.elf", "3" }, F_NONE, 0, 0, 0 },
    { "dos clean", { "dos", "f.bin", "3" }, F_NONE, 0, 0, 0 },
    { "tasks clean", { "tasks", "3" }, F_NONE, 0, 0, 0 },
    { "bare clean", { "bare", "3" }, F_NONE, 0, 0, 0 },
    { "hold clean", { "hold", "4" }, F_NONE, 0, 0, 0 },
    { "holdbare clean", { "holdbare", "4" }, F_NONE, 0, 0, 0 },
    { "hold toolflags clean", { "hold", "4", "TOOLFLAGS" }, F_NONE, 0, 0, 0 },
    /* the library cannot be opened */
    { "parent OpenLibrary", { "churn", "3" }, F_OPENLIBRARY, 1, 20, 0 },
    /* churn and notify: the warm-up pass, a pass of the loop */
    { "churn AAROpen warm-up", { "churn", "3" }, F_AAROPEN, 1, 10, 0 },
    { "churn AAROpen loop", { "churn", "3" }, F_AAROPEN, 3, 10, 0 },
    { "churn AARClose warm-up", { "churn", "3" }, F_AARCLOSE, 1, 10, 0 },
    { "churn AARClose loop", { "churn", "3" }, F_AARCLOSE, 3, 10, 0 },
    { "notify CreateMsgPort", { "notify", "3" }, F_CREATEPORT, 1, 10, 0 },
    { "notify AAROpen", { "notify", "3" }, F_AAROPEN, 2, 10, 0 },
    { "notify AARClose", { "notify", "3" }, F_AARCLOSE, 2, 10, 0 },
    /* list */
    { "list AARExt", { "list", "3" }, F_EXT, 1, 10, 0 },
    { "list AAROpen", { "list", "3" }, F_AAROPEN, 2, 10, 0 },
    { "list claim list", { "list", "3" }, F_EXTLIST, 1, 10, 0 },
    { "list claim list later", { "list", "3" }, F_EXTLIST, 3, 10, 0 },
    { "list claim list at the end", { "list", "3" }, F_EXTLIST, 4, 10, 0 },
    { "list claim list too short", { "list", "3" }, F_LISTEND, 2, 10, 0 },
    { "list AARClose", { "list", "3" }, F_AARCLOSE, 1, 10, 0 },
    /* probe and dos */
    { "probe AARProbeFile warm-up", { "probe", "f.elf", "3" }, F_PROBE, 1, 10, 0 },
    { "probe AARProbeFile loop", { "probe", "f.elf", "3" }, F_PROBE, 3, 10, 0 },
    { "dos Open warm-up", { "dos", "f.bin", "3" }, F_OPEN, 1, 10, 0 },
    { "dos Open loop", { "dos", "f.bin", "3" }, F_OPEN, 3, 10, 0 },
    { "dos Seek to the end", { "dos", "f.bin", "3" }, F_SEEK, 1, 10, 0 },
    { "dos Seek to the start", { "dos", "f.bin", "3" }, F_SEEK, 2, 10, 0 },
    { "dos Seek in the loop", { "dos", "f.bin", "3" }, F_SEEK, 5, 10, 0 },
    { "dos Read", { "dos", "f.bin", "3" }, F_READ, 1, 10, 0 },
    { "dos Close", { "dos", "f.bin", "3" }, F_CLOSE, 1, 10, 0 },
    /* tasks and bare: one child after the other (two warm-up children) */
    { "tasks child OpenLibrary", { "tasks", "3" }, F_OPENLIBRARY, 2, 10, 0 },
    { "tasks child AAROpen", { "tasks", "3" }, F_AAROPEN, 1, 10, 0 },
    { "tasks child AARClose", { "tasks", "3" }, F_AARCLOSE, 1, 10, 0 },
    { "tasks CreateNewProc first", { "tasks", "3" }, F_CREATEPROC, 1, 10, 0 },
    { "tasks CreateNewProc loop", { "tasks", "3" }, F_CREATEPROC, 4, 10, 0 },
    { "tasks child stalled", { "tasks", "3" }, F_STALL, 2, 10, 1 },
    { "bare CreateNewProc warm-up", { "bare", "3" }, F_CREATEPROC, 2, 10, 0 },
    { "bare CreateNewProc loop", { "bare", "3" }, F_CREATEPROC, 4, 10, 0 },
    /* hold and holdbare: all children at once (two warm-up children) */
    { "hold child OpenLibrary", { "hold", "4" }, F_OPENLIBRARY, 2, 10, 0 },
    { "hold child OpenLibrary in the run", { "hold", "4" }, F_OPENLIBRARY, 5, 10, 0 },
    { "hold child AAROpen", { "hold", "4" }, F_AAROPEN, 1, 10, 0 },
    { "hold child AAROpen in the run", { "hold", "4" }, F_AAROPEN, 4, 10, 0 },
    { "hold child AARMessageSignal", { "hold", "4" }, F_SIGNAL, 1, 10, 0 },
    { "hold child AARMessageSignal in the run", { "hold", "4" }, F_SIGNAL, 5, 10, 0 },
    { "hold child AARClose", { "hold", "4" }, F_AARCLOSE, 1, 10, 0 },
    { "hold child AARClose in the run", { "hold", "4" }, F_AARCLOSE, 6, 10, 0 },
    { "hold CreateNewProc first", { "hold", "4" }, F_CREATEPROC, 1, 10, 0 },
    { "hold CreateNewProc second", { "hold", "4" }, F_CREATEPROC, 2, 10, 0 },
    { "hold CreateNewProc in the run", { "hold", "4" }, F_CREATEPROC, 4, 10, 0 },
    { "hold CreateNewProc last", { "hold", "4" }, F_CREATEPROC, 6, 10, 0 },
    { "hold toolflags CreateNewProc", { "hold", "4", "TOOLFLAGS" }, F_CREATEPROC, 4, 10, 0 },
    { "hold child stalled", { "hold", "4" }, F_STALL, 2, 10, 1 },
    { "hold child stalled in the run", { "hold", "4" }, F_STALL, 5, 10, 1 },
    { "holdbare CreateNewProc warm-up", { "holdbare", "4" }, F_CREATEPROC, 2, 10, 0 },
    { "holdbare CreateNewProc loop", { "holdbare", "4" }, F_CREATEPROC, 5, 10, 0 },
    { "hold more than the platform has", { "hold", "9" }, F_NONE, 0, 10, 0 },
    /* wrong arguments */
    { "usage", { "churn" }, F_NONE, 0, 20, 0 },
    { "unknown mode", { "frobnicate", "3" }, F_NONE, 0, 20, 0 },
};

static int is_hold(const struct tcase *c)
{
    return strncmp(c->args[0], "hold", 4) == 0;
}

static void test_table(void)
{
    size_t i;

    cur_test = "table";
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct tcase *c = &cases[i];
        char *argv[10];
        struct run_result r;
        int argc = 0, a, want_alive = c->stalled ? 1 : 0;

        argv[argc++] = (char *)"aarleak";
        argv[argc++] = (char *)LIB;
        for (a = 0; a < 4 && c->args[a]; a++)
            argv[argc++] = (char *)c->args[a];
        reset(c->kind, c->at);
        r = run_probe(argc, argv, want_alive);
        if (!(r.rc == c->rc && !r.hung && r.alive_at_return == want_alive && (c->stalled || r.delays < 500UL)))
            printf("  case \"%s\": rc %d (want %d), hung %d, alive %d, delays %lu\n", c->name, r.rc,
                   c->rc, r.hung, r.alive_at_return, r.delays);
        CHECK(!r.hung);
        CHECK(r.rc == c->rc);
        /* Every process that started is joined, but a stalled one, which has
         * been told to go (hold). */
        CHECK(r.alive_at_return == want_alive);
        if (is_hold(c) && c->rc != 20)
            CHECK(r.released);
        CHECK(libs_open == 0 && handles_open == 0 && ports_open == 0 && files_open == 0);
        /* No wait for a process that does not exist: a run that fails for another
         * reason does not spend its time-out. */
        if (!c->stalled)
            CHECK(r.delays < 500UL);
    }
}

int main(void)
{
    test_table();
    printf("aarleak_test: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
