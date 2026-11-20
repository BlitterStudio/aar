/*
 * Memory leak probe for aar.library on a real AmigaOS, over the reference
 * platform (no board needed): the library is built by build_leak.sh with
 * client/src/refplat.c in place of the ZZ9000 transport.
 *
 *   aarleak LIBRARY_PATH churn N          N times AAROpen + AARClose in this task
 *   aarleak LIBRARY_PATH tasks N          the same, one task after the other
 *   aarleak LIBRARY_PATH bare N           N tasks that open nothing: the baseline
 *   aarleak LIBRARY_PATH hold N [TOOLFLAGS]   N tasks at once, each holding a handle
 *   aarleak LIBRARY_PATH holdbare N [TOOLFLAGS]   the same without the library
 *   aarleak LIBRARY_PATH list N           N handles, then the platform's claim list
 *   aarleak LIBRARY_PATH probe FILE N     N times AARProbeFile of FILE
 *   aarleak LIBRARY_PATH dos FILE N       N times Open, Seek, Read, Close of FILE
 *                                         (the baseline of the probe)
 *   aarleak LIBRARY_PATH notify N         N times AAROpen with a notify port + AARClose
 *
 * Each mode does one warm-up pass, then prints AvailMem(MEMF_ANY) before and
 * after, and the bytes lost per pass.
 *
 * The exit status says whether the measurement can be trusted. Exit code 0: every
 * call of the run did what it was meant to. Exit code 10: some call failed (an
 * OpenLibrary, AAROpen, AARClose, AARMessageSignal or AARProbeFile in this task
 * or in a child, a file call, the claim list, a process that could not be
 * created, a child that never became ready or never ended), and each failure is
 * printed; the run goes on to its end, so that every process that started is
 * released and joined and every handle and file is closed. Exit code 20: the
 * arguments are wrong or the library cannot be opened. FILE must be an image
 * AARProbeFile accepts, and N must not exceed the claims the platform has
 * (8 for the reference platform) in the modes that hold handles at once.
 *
 * The probe is tested on the host (client/tests/aarleak_test.c), which compiles
 * this file against a model of the calls it makes (AARLEAK_HOST), fails each of
 * them in turn and requires a failing exit status with nothing left behind.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AARLEAK_HOST
#include "../aarleak_host.h"
#else
#include <dos/dostags.h>
#include <proto/dos.h>
#include <proto/exec.h>
#define AARLEAK_FN(f) ((ULONG)(f))
#define AARLEAK_STR(s) ((ULONG)(s))
#define AARLEAK_PTR(p) ((const void *)(p))
#endif

#include <proto/aar.h>
#include <libraries/aar_zz9000.h>

/* The longest a wait for the children may last, in ticks (50 a second): two
 * minutes. A child that never gets there fails the run instead of hanging it. */
#ifndef AARLEAK_WAIT_TICKS
#define AARLEAK_WAIT_TICKS 6000UL
#endif

struct Library *AARBase;

static ULONG avail(void)
{
    return AvailMem(MEMF_ANY);
}

static void report(const char *what, ULONG before, ULONG after, ULONG n)
{
    long lost = (long)before - (long)after;

    printf("%s: %lu passes, AvailMem %lu -> %lu, lost %ld bytes (%ld per pass)\n", what,
           (unsigned long)n, (unsigned long)before, (unsigned long)after, lost,
           n ? lost / (long)n : 0L);
}

/* Failures of this task. Each is counted; the first few are printed. */
static ULONG fails;

static void fail(const char *what, long code)
{
    if (fails < 5)
        printf("FAILED: %s (%ld)\n", what, code);
    fails++;
}

static int one_pair(struct MsgPort *port)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    LONG rc = AAROpen(&h, port, &reason);

    if (rc != AAR_OK) {
        fail("AAROpen", (long)rc);
        return 0;
    }
    rc = AARClose(h);
    if (rc != AAR_OK) {
        fail("AARClose", (long)rc);
        return 0;
    }
    return 1;
}

static void dos_once(const char *file)
{
    BPTR f = Open((STRPTR)file, MODE_OLDFILE);
    char buf[64];

    if (!f) {
        fail("Open", 0);
        return;
    }
    if (Seek(f, 0L, OFFSET_END) < 0L)
        fail("Seek to the end", 0);
    if (Seek(f, 0L, OFFSET_BEGINNING) < 0L)
        fail("Seek to the start", 0);
    if (Read(f, buf, sizeof(buf)) < 0L)
        fail("Read", 0);
    if (!Close(f))
        fail("Close", 0);
}

static void probe_once(const char *file)
{
    AARImageInfo info;
    LONG rc;

    info.size = sizeof(info);
    rc = AARProbeFile(file, &info);
    if (rc != AAR_OK)
        fail("AARProbeFile", (long)rc);
}

static volatile ULONG child_ready, child_exited;
static volatile int child_go;
static int child_use_library, child_hold, child_toolflags;

/* What a child reports, as bits: it does not print (a child has no console of its
 * own), the parent does after the join. */
#define CF_OPENLIBRARY 1UL
#define CF_AAROPEN 2UL
#define CF_SIGNAL 4UL
#define CF_AARCLOSE 8UL
static volatile ULONG child_failed;

/* The counters are shared between tasks that run at the same time. */
static void bump(volatile ULONG *counter)
{
    Forbid();
    (*counter)++;
    Permit();
}

static void child_fail(ULONG bit)
{
    Forbid();
    child_failed |= bit;
    Permit();
}

/* A child process that opens the library, opens and closes one handle, closes
 * the library and ends: one task of the card test T3. With child_use_library 0
 * it does nothing: the baseline of the process itself. With child_hold it arms
 * the handle (AARMessageSignal) and waits for child_go while holding it, as the
 * card test does with all its tasks at once. A call that fails is recorded and
 * the child still takes its part in the handshake, so the parent never waits
 * for it in vain. */
static void child_main(void)
{
    struct Library *base = NULL;
    AARHandle *h = NULL;
    ULONG reason = 0;

    if (child_use_library) {
        base = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, 0);
        if (!base)
            child_fail(CF_OPENLIBRARY);
        else if (AAROpen(&h, NULL, &reason) != AAR_OK) {
            h = NULL;
            child_fail(CF_AAROPEN);
        } else if (child_hold && AARMessageSignal(h) == 0UL)
            child_fail(CF_SIGNAL);
    }
    if (child_hold) {
        bump(&child_ready);
        while (!child_go)
            Delay(2);
    }
    if (h && AARClose(h) != AAR_OK)
        child_fail(CF_AARCLOSE);
    if (base)
        CloseLibrary(base);
    bump(&child_exited);
}

/* Waits until *counter reaches target. A wait that would be for ever fails. */
static int wait_count(volatile ULONG *counter, ULONG target, const char *what)
{
    ULONG waited = 0;

    while (*counter < target) {
        if (waited >= AARLEAK_WAIT_TICKS) {
            printf("FAILED: timed out waiting for %s (%lu of %lu)\n", what,
                   (unsigned long)*counter, (unsigned long)target);
            fails++;
            return 0;
        }
        Delay(5);
        waited += 5;
    }
    return 1;
}

/* Prints what the children reported. 1 when they all did their work. */
static int children_ok(void)
{
    ULONG bits = child_failed;

    if (bits & CF_OPENLIBRARY)
        fail("a child's OpenLibrary", 0);
    if (bits & CF_AAROPEN)
        fail("a child's AAROpen", 0);
    if (bits & CF_SIGNAL)
        fail("a child's AARMessageSignal", 0);
    if (bits & CF_AARCLOSE)
        fail("a child's AARClose", 0);
    return bits == 0;
}

/* One child after the other, each joined before the next starts: a process that
 * cannot be created ends the run with nothing left to join. 1 when all ran and
 * did their work. */
static int run_children(ULONG n)
{
    ULONG i;
    int ok = 1;

    child_exited = 0;
    child_failed = 0;
    child_hold = 0;
    for (i = 0; i < n; i++) {
        if (!CreateNewProcTags(NP_Entry, AARLEAK_FN(child_main), NP_Name,
                               AARLEAK_STR("aarleak child"), NP_StackSize, 8192, TAG_DONE)) {
            printf("CreateNewProc failed after %lu children\n", (unsigned long)i);
            fails++;
            ok = 0;
            break;
        }
        if (!wait_count(&child_exited, i + 1, "a child to end")) {
            ok = 0;
            break; /* it may still run: the next one would not be one after the other */
        }
        Delay(3); /* the child's exit */
    }
    return children_ok() && ok;
}

/*
 * All children at once, each holding its handle until every one is open. The
 * waits are for the children that were started, not for the n that were asked
 * for: a process that cannot be created stops the spawning, and the ones that
 * did start are released and joined, so the probe neither hangs nor leaves
 * them blocked. The release is given even when a wait timed out, so that a
 * child that was only slow ends when it gets there. 1 when all n ran and did
 * their work.
 */
static int run_hold(ULONG n)
{
    ULONG i, started = 0;
    int ok = 1;

    child_ready = child_exited = 0;
    child_failed = 0;
    child_go = 0;
    child_hold = 1;
    for (i = 0; i < n; i++) {
        struct Process *p;

        /* TOOLFLAGS: the tag set of aarlibtest's spawn(), which asks not to close
         * the NIL: handles that CreateNewProc opens for the child, so each
         * child leaves two file handles behind. */
        if (child_toolflags)
            p = CreateNewProcTags(NP_Entry, AARLEAK_FN(child_main), NP_Name,
                                  AARLEAK_STR("aarleak child"), NP_StackSize, 16384,
                                  NP_CloseInput, FALSE, NP_CloseOutput, FALSE, TAG_DONE);
        else
            p = CreateNewProcTags(NP_Entry, AARLEAK_FN(child_main), NP_Name,
                                  AARLEAK_STR("aarleak child"), NP_StackSize, 16384, TAG_DONE);
        if (!p) {
            printf("CreateNewProc failed after %lu of %lu children\n", (unsigned long)started,
                   (unsigned long)n);
            fails++;
            ok = 0;
            break;
        }
        started++;
    }
    if (!wait_count(&child_ready, started, "the children to be ready"))
        ok = 0;
    child_go = 1;
    if (!wait_count(&child_exited, started, "the children to end"))
        ok = 0;
    Delay(25);
    child_hold = 0;
    return children_ok() && ok;
}

static int leak_main(int argc, char **argv)
{
    ULONG n, i, before, after;
    const char *mode;
    int ok;

    fails = 0;
    child_failed = 0;
    child_toolflags = 0;
    if (argc < 4) {
        printf("usage: aarleak LIBRARY churn|tasks|bare|notify|list N | hold|holdbare N [TOOLFLAGS]"
               " | probe|dos FILE N\n");
        return 20;
    }
    AARBase = OpenLibrary((CONST_STRPTR)argv[1], 0);
    if (!AARBase) {
        printf("cannot open %s\n", argv[1]);
        return 20;
    }
    mode = argv[2];
    if (strcmp(mode, "churn") == 0 || strcmp(mode, "notify") == 0) {
        struct MsgPort *port = NULL;

        if (mode[0] == 'n') {
            port = CreateMsgPort();
            if (!port)
                fail("CreateMsgPort", 0);
        }
        if (mode[0] != 'n' || port) {
            n = (ULONG)strtoul(argv[3], NULL, 0);
            (void)one_pair(port);
            before = avail();
            for (i = 0; i < n; i++)
                (void)one_pair(port);
            after = avail();
            report(mode, before, after, n);
        }
        if (port)
            DeleteMsgPort(port);
    } else if (strcmp(mode, "list") == 0) {
        /* Holds N handles and walks the platform's claim list the way a client
         * does: the next cursor is the index plus one. */
        const aar_client_ext_zz9000 *ext = (const aar_client_ext_zz9000 *)
            AARLEAK_PTR(AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT));
        AARHandle *hs[16];
        ULONG cursor = 0, k, opened = 0;
        int steps = 0;

        n = (ULONG)strtoul(argv[3], NULL, 0);
        if (n > 16)
            n = 16;
        if (!ext)
            fail("AARExt", 0);
        for (i = 0; i < n; i++) {
            ULONG reason = 0;
            LONG rc;

            hs[i] = NULL;
            rc = AAROpen(&hs[i], NULL, &reason);
            if (rc != AAR_OK) {
                hs[i] = NULL;
                fail("AAROpen", (long)rc);
            } else {
                opened++;
            }
        }
        for (k = 0; ext && k < 40; k++) {
            AARZZ9000List li;
            LONG lr;

            memset(&li, 0, sizeof(li));
            li.size = sizeof(li);
            lr = ext->list(cursor, &li);
            printf("list(cursor %lu) = %ld handle 0x%08lx index %lu state %lu\n",
                   (unsigned long)cursor, (long)lr, (unsigned long)li.handle,
                   (unsigned long)li.index, (unsigned long)li.state);
            if (lr < 0)
                fail("the claim list", (long)lr);
            if (lr != 1)
                break;
            cursor = li.index + 1;
            steps++;
        }
        printf("%d claims listed, %lu held\n", steps, (unsigned long)opened);
        if (ext && (ULONG)steps < opened)
            fail("the claim list is shorter than the handles held", (long)steps);
        for (i = 0; i < n; i++)
            if (hs[i]) {
                LONG rc = AARClose(hs[i]);

                if (rc != AAR_OK)
                    fail("AARClose", (long)rc);
            }
    } else if (strcmp(mode, "hold") == 0 || strcmp(mode, "holdbare") == 0) {
        n = (ULONG)strtoul(argv[3], NULL, 0);
        child_use_library = mode[4] == '\0';
        child_toolflags = argc > 4 && strcmp(argv[4], "TOOLFLAGS") == 0;
        if (!run_hold(2)) {
            printf("FAILED: the warm-up pass did not run clean\n");
            fails++;
        } else {
            before = avail();
            ok = run_hold(n);
            after = avail();
            report(mode, before, after, n);
            if (!ok) {
                printf("FAILED: the run did not run clean\n");
                fails++;
            }
        }
    } else if (strcmp(mode, "tasks") == 0 || strcmp(mode, "bare") == 0) {
        n = (ULONG)strtoul(argv[3], NULL, 0);
        child_use_library = mode[0] == 't';
        if (!run_children(2)) {
            printf("FAILED: the warm-up pass did not run clean\n");
            fails++;
        } else {
            before = avail();
            ok = run_children(n);
            after = avail();
            report(mode, before, after, n);
            if (!ok) {
                printf("FAILED: the run did not run clean\n");
                fails++;
            }
        }
    } else if ((strcmp(mode, "probe") == 0 || strcmp(mode, "dos") == 0) && argc >= 5) {
        n = (ULONG)strtoul(argv[4], NULL, 0);
        if (mode[0] == 'p')
            probe_once(argv[3]);
        else
            dos_once(argv[3]);
        before = avail();
        for (i = 0; i < n; i++) {
            if (mode[0] == 'p')
                probe_once(argv[3]);
            else
                dos_once(argv[3]);
        }
        after = avail();
        report(mode, before, after, n);
    } else {
        printf("unknown mode\n");
        CloseLibrary(AARBase);
        return 20;
    }
    CloseLibrary(AARBase);
    if (fails > 0)
        printf("FAILED: %lu failures; the measurement is not valid\n", (unsigned long)fails);
    return fails ? 10 : 0;
}

#ifndef AARLEAK_HOST
int main(int argc, char **argv)
{
    return leak_main(argc, argv);
}
#endif /* !AARLEAK_HOST */
