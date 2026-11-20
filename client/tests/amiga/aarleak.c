/*
 * Memory leak probe for aar.library on a real AmigaOS, over the reference
 * platform (no board needed): the library is built by build_leak.sh with
 * client/src/refplat.c in place of the ZZ9000 transport.
 *
 *   aarleak LIBRARY_PATH churn N          N times AAROpen + AARClose in this task
 *   aarleak LIBRARY_PATH tasks N          the same, one task after the other
 *   aarleak LIBRARY_PATH probe FILE N     N times AARProbeFile of FILE
 *   aarleak LIBRARY_PATH dos FILE N       N times Open, Seek, Read, Close of FILE
 *                                         (the baseline of the probe)
 *   aarleak LIBRARY_PATH notify N         N times AAROpen with a notify port + AARClose
 *
 * Each mode does one warm-up pass, then prints AvailMem(MEMF_ANY) before and
 * after, and the bytes lost per pass.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <proto/aar.h>
#include <libraries/aar_zz9000.h>

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

static int one_pair(struct MsgPort *port)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    LONG rc = AAROpen(&h, port, &reason);

    if (rc != AAR_OK) {
        printf("AAROpen failed: %ld\n", (long)rc);
        return 0;
    }
    if (AARClose(h) != AAR_OK) {
        printf("AARClose failed\n");
        return 0;
    }
    return 1;
}

static void dos_once(const char *file)
{
    BPTR f = Open((STRPTR)file, MODE_OLDFILE);
    char buf[64];

    if (!f)
        return;
    (void)Seek(f, 0L, OFFSET_END);
    (void)Seek(f, 0L, OFFSET_BEGINNING);
    (void)Read(f, buf, sizeof(buf));
    Close(f);
}

static void probe_once(const char *file)
{
    AARImageInfo info;

    info.size = sizeof(info);
    (void)AARProbeFile(file, &info);
}

#include <dos/dostags.h>

static volatile int child_done;
static volatile ULONG child_ready, child_exited;
static volatile int child_go;
static int child_use_library, child_hold, child_toolflags;

/* A child process that opens the library, opens and closes one handle, closes
 * the library and ends: one task of the card test T3. With child_use_library 0
 * it does nothing: the baseline of the process itself. With child_hold it arms
 * the handle (AARMessageSignal) and waits for child_go while holding it, as the
 * card test does with all its tasks at once. */
static void child_main(void)
{
    struct Library *base = NULL;
    AARHandle *h = NULL;
    ULONG reason = 0;

    if (child_use_library) {
        base = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, 0);
        if (base && AAROpen(&h, NULL, &reason) != AAR_OK)
            h = NULL;
        if (h && child_hold)
            (void)AARMessageSignal(h);
    }
    if (child_hold) {
        child_ready++;
        while (!child_go)
            Delay(2);
    }
    if (h)
        (void)AARClose(h);
    if (base)
        CloseLibrary(base);
    child_exited++;
    child_done = 1;
}

static void run_children(ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++) {
        child_done = 0;
        if (!CreateNewProcTags(NP_Entry, (ULONG)child_main, NP_Name, (ULONG)"aarleak child",
                               NP_StackSize, 8192, TAG_DONE)) {
            printf("CreateNewProc failed\n");
            return;
        }
        while (!child_done)
            Delay(1);
        Delay(3); /* the child's exit */
    }
}

/* All children at once, each holding its handle until every one is open. */
static void run_hold(ULONG n)
{
    ULONG i;

    child_ready = child_exited = 0;
    child_go = 0;
    child_hold = 1;
    for (i = 0; i < n; i++) {
        struct Process *p;

        /* TOOLFLAGS: the tag set of aarlibtest's spawn(), which asks not to close
         * the NIL: handles that CreateNewProc opens for the child, so each
         * child leaves two file handles behind. */
        if (child_toolflags)
            p = CreateNewProcTags(NP_Entry, (ULONG)child_main, NP_Name, (ULONG)"aarleak child",
                                  NP_StackSize, 16384, NP_CloseInput, FALSE,
                                  NP_CloseOutput, FALSE, TAG_DONE);
        else
            p = CreateNewProcTags(NP_Entry, (ULONG)child_main, NP_Name, (ULONG)"aarleak child",
                                  NP_StackSize, 16384, TAG_DONE);
        if (!p) {
            printf("CreateNewProc failed\n");
            break;
        }
    }
    while (child_ready < n)
        Delay(5);
    child_go = 1;
    while (child_exited < n)
        Delay(5);
    Delay(25);
    child_hold = 0;
}

int main(int argc, char **argv)
{
    ULONG n, i, before, after;
    const char *mode;
    int rc = 0;

    if (argc < 4) {
        printf("usage: aarleak LIBRARY churn|tasks|notify N | probe|dos FILE N\n");
        return 20;
    }
    AARBase = OpenLibrary((CONST_STRPTR)argv[1], 0);
    if (!AARBase) {
        printf("cannot open %s\n", argv[1]);
        return 20;
    }
    mode = argv[2];
    if (strcmp(mode, "churn") == 0 || strcmp(mode, "notify") == 0) {
        struct MsgPort *port = strcmp(mode, "notify") == 0 ? CreateMsgPort() : NULL;

        n = (ULONG)strtoul(argv[3], NULL, 0);
        (void)one_pair(port);
        before = avail();
        for (i = 0; i < n; i++)
            rc |= !one_pair(port);
        after = avail();
        report(mode, before, after, n);
        if (port)
            DeleteMsgPort(port);
    } else if (strcmp(mode, "list") == 0) {
        /* Holds N handles and walks the platform's claim list the way a client
         * does: the next cursor is the index plus one. */
        const aar_client_ext_zz9000 *ext = (const aar_client_ext_zz9000 *)
            AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
        AARHandle *hs[16];
        ULONG cursor = 0, k;
        int steps = 0;

        n = (ULONG)strtoul(argv[3], NULL, 0);
        if (n > 16)
            n = 16;
        for (i = 0; i < n; i++) {
            ULONG reason = 0;

            hs[i] = NULL;
            (void)AAROpen(&hs[i], NULL, &reason);
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
            if (lr != 1)
                break;
            cursor = li.index + 1;
            steps++;
        }
        printf("%d claims listed, %lu held\n", steps, (unsigned long)n);
        for (i = 0; i < n; i++)
            if (hs[i])
                (void)AARClose(hs[i]);
    } else if (strcmp(mode, "hold") == 0 || strcmp(mode, "holdbare") == 0) {
        n = (ULONG)strtoul(argv[3], NULL, 0);
        child_use_library = mode[4] == '\0';
        child_toolflags = argc > 4 && strcmp(argv[4], "TOOLFLAGS") == 0;
        run_hold(2);
        before = avail();
        run_hold(n);
        after = avail();
        report(mode, before, after, n);
    } else if (strcmp(mode, "tasks") == 0 || strcmp(mode, "bare") == 0) {
        n = (ULONG)strtoul(argv[3], NULL, 0);
        child_use_library = mode[0] == 't';
        run_children(2);
        before = avail();
        run_children(n);
        after = avail();
        report(mode, before, after, n);
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
        rc = 20;
    }
    CloseLibrary(AARBase);
    return rc ? 10 : 0;
}
