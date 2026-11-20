/*
 * Host test of the process handling of the leak probe
 * (client/tests/amiga/aarleak.c): it compiles the probe against a model of the
 * calls it makes (AARLEAK_HOST, client/tests/aarleak_host.h). The model has
 * processes that become ready and exit as the parent waits (Delay), a
 * CreateNewProcTags that can fail, and a limit on the waiting: a probe that
 * waits for a process that does not exist would sleep for ever on the Amiga,
 * and the model stops it and reports a hang.
 *
 * The rule: the probe waits for, releases and joins the processes that
 * started, whatever happened to the others, and reports a run that could not
 * start them all as failed.
 *
 * SPDX-License-Identifier: MIT
 */
#define AARLEAK_HOST 1
#include <setjmp.h>

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

/* ---- The model ----------------------------------------------------------------- */

#define MAXPROC 16
#define DELAY_LIMIT 2000UL

static int fail_at;            /* the CreateNewProcTags call with this number fails (1-based; 0: none) */
static int create_calls;
static int started;            /* processes created */
static int proc_ready[MAXPROC];
static int proc_exited[MAXPROC];
static int proc_released;      /* processes that saw child_go before they exited */
static unsigned long delays;
static jmp_buf *hang_env;

ULONG AvailMem(ULONG attributes)
{
    (void)attributes;
    return 1000000UL;
}

BPTR Open(STRPTR name, LONG mode)
{
    (void)name;
    (void)mode;
    return 0;
}

LONG Seek(BPTR file, LONG position, LONG mode)
{
    (void)file;
    (void)position;
    (void)mode;
    return 0;
}

LONG Read(BPTR file, void *buffer, LONG length)
{
    (void)file;
    (void)buffer;
    (void)length;
    return 0;
}

LONG Close(BPTR file)
{
    (void)file;
    return 0;
}

struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    (void)name;
    (void)version;
    return NULL;
}

void CloseLibrary(struct Library *library)
{
    (void)library;
}

struct MsgPort *CreateMsgPort(void)
{
    return NULL;
}

void DeleteMsgPort(struct MsgPort *port)
{
    (void)port;
}

LONG AAROpen(AARHandle **handlep, struct MsgPort *port, ULONG *reason)
{
    (void)handlep;
    (void)port;
    (void)reason;
    return AAR_E_NOSYS;
}

LONG AARClose(AARHandle *handle)
{
    (void)handle;
    return AAR_OK;
}

ULONG AARMessageSignal(AARHandle *handle)
{
    (void)handle;
    return 0;
}

LONG AARProbeFile(const char *path, AARImageInfo *info)
{
    (void)path;
    (void)info;
    return AAR_OK;
}

APTR AARExt(ULONG platform_id, ULONG ext_id)
{
    (void)platform_id;
    (void)ext_id;
    return 0;
}

struct Process *CreateNewProcTags(ULONG tag, ...)
{
    create_calls++;
    (void)tag;
    if (fail_at != 0 && create_calls == fail_at)
        return NULL;
    if (started >= MAXPROC)
        abort();
    return (struct Process *)(uintptr_t)(0x1000u + (unsigned)started++);
}

/*
 * Time passes: a child of a hold run becomes ready on its first slice and, once
 * it is released (child_go), exits on its next; a child of a plain run is done
 * on its first slice. What the child does is what child_main does.
 */
void Delay(ULONG ticks)
{
    int i;

    (void)ticks;
    if (++delays > DELAY_LIMIT && hang_env)
        longjmp(*hang_env, 1);
    for (i = 0; i < started; i++) {
        if (proc_exited[i])
            continue;
        if (!child_hold) {
            proc_exited[i] = 1;
            child_exited++;
            child_done = 1;
        } else if (!proc_ready[i]) {
            proc_ready[i] = 1;
            child_ready++;
        } else if (child_go) {
            proc_exited[i] = 1;
            proc_released++;
            child_exited++;
        }
    }
}

/* ---- The tests ------------------------------------------------------------------- */

static void reset(void)
{
    memset(proc_ready, 0, sizeof(proc_ready));
    memset(proc_exited, 0, sizeof(proc_exited));
    fail_at = 0;
    create_calls = 0;
    started = 0;
    proc_released = 0;
    delays = 0;
    child_done = 0;
    child_go = 0;
    child_hold = 0;
    child_ready = child_exited = 0;
}

/* Runs one of the probe's loops; *hung says whether it would never have ended. */
static int guarded(int (*run)(ULONG), ULONG n, int *hung)
{
    jmp_buf env;
    int rc;

    hang_env = &env;
    delays = 0;
    *hung = 0;
    if (setjmp(env) != 0) {
        *hung = 1;
        hang_env = NULL;
        return -1;
    }
    rc = run(n);
    hang_env = NULL;
    return rc;
}

static void test_hold(void)
{
    int hung, rc, i;

    cur_test = "hold";
    /* Every child starts: all are released and joined, the run is a success. */
    reset();
    rc = guarded(run_hold, 4, &hung);
    CHECK(!hung && rc == 1 && started == 4 && child_exited == 4 && proc_released == 4);
    CHECK(child_hold == 0);
    /* The third process cannot be created: the two that started are released
     * and joined, nothing hangs, and the run is a failure. */
    reset();
    fail_at = 3;
    rc = guarded(run_hold, 5, &hung);
    CHECK(!hung);
    CHECK(rc == 0);
    CHECK(started == 2 && child_ready == 2 && child_exited == 2 && proc_released == 2);
    for (i = 0; i < started; i++)
        CHECK(proc_exited[i]); /* none is left blocked */
    CHECK(child_hold == 0);
    /* The first cannot be created: nothing to join. */
    reset();
    fail_at = 1;
    rc = guarded(run_hold, 3, &hung);
    CHECK(!hung && rc == 0 && started == 0 && child_exited == 0);
    /* The last cannot be created. */
    reset();
    fail_at = 4;
    rc = guarded(run_hold, 4, &hung);
    CHECK(!hung && rc == 0 && started == 3 && child_exited == 3 && proc_released == 3);
    /* The tool-flags variant spawns the same way. */
    reset();
    child_toolflags = 1;
    fail_at = 2;
    rc = guarded(run_hold, 3, &hung);
    CHECK(!hung && rc == 0 && started == 1 && child_exited == 1);
    child_toolflags = 0;
}

static void test_children(void)
{
    int hung, rc;

    cur_test = "children";
    /* One after the other: all run. */
    reset();
    rc = guarded(run_children, 3, &hung);
    CHECK(!hung && rc == 1 && started == 3 && child_exited == 3);
    /* The second cannot be created: the first was joined, the run fails. */
    reset();
    fail_at = 2;
    rc = guarded(run_children, 4, &hung);
    CHECK(!hung && rc == 0 && started == 1 && child_exited == 1);
}

int main(void)
{
    test_hold();
    test_children();
    printf("aarleak_test: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
