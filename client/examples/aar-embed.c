/*
 * aar-embed: a program with its own window and its own event loop that runs an
 * AAR app with aar_serve (libraries/aar_serve.h). It is the embedding example of
 * docs/aar-serve-api.md: where aar-run (client/run) lets aar_serve_run own the
 * task, this program waits on its window and on aar_serve_sigmask together and
 * steps the loop itself.
 *
 *   aar-embed <app.elf> [args]
 *
 * The window shows the app's last message and has a close gadget that stops the
 * app. The app's console output goes to the Shell. The program asks for what an
 * embedder usually wants: a heartbeat of 20 ms and the library's signal (so it
 * sleeps between requests), no backdrop input window (the program has its own
 * window), and the app's messages left for AARGetMessage.
 *
 * Build: m68k-amigaos-gcc -noixemul -m68030 -Iclient/include -Iinclude \
 *     client/examples/aar-embed.c client/serve/aar_serve.c \
 *     client/serve/aar_serve_core.c -lamiga
 * (or against libaarserve.a, see client/tests/m68k_check.sh).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/aar.h>

#include <stdio.h>
#include <string.h>

#include <libraries/aar_serve.h>

/* The program's own libraries: aar_serve uses them through these. */
struct Library *AARBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

static char status_line[96];

/* The window's title line: the app's last message, as hex. */
static void show_messages(AARHandle *h, struct Window *win)
{
    AARMessage m;
    int shown = 0;

    while (AARGetMessage(h, &m) == AAR_OK) {
        snprintf(status_line, sizeof(status_line),
                 "aar-embed: message %02x%02x%02x%02x%02x%02x%02x%02x",
                 m.bytes[0], m.bytes[1], m.bytes[2], m.bytes[3],
                 m.bytes[4], m.bytes[5], m.bytes[6], m.bytes[7]);
        shown = 1;
    }
    if (shown)
        SetWindowTitles(win, (CONST_STRPTR)status_line, (CONST_STRPTR)-1);
}

/* The window's messages: the close gadget stops the app. */
static void handle_window(aar_serve *s, struct Window *win)
{
    struct IntuiMessage *im;

    while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
        ULONG class = im->Class;

        ReplyMsg((struct Message *)im);
        if (class == IDCMP_CLOSEWINDOW)
            aar_serve_stop(s);
    }
}

static int stack_ok(void)
{
    struct Task *task = FindTask(NULL);

    return (ULONG)task->tc_SPUpper - (ULONG)task->tc_SPLower >=
           AAR_SERVE_MIN_STACK;
}

int main(int argc, char **argv)
{
    AARHandle *h = NULL;
    aar_serve *s = NULL;
    aar_serve_config cfg;
    struct aar_serve_result res;
    struct Window *win = NULL;
    ULONG reason = 0;
    LONG rc;
    int code = 20;

    if (argc < 2) {
        puts("aar-embed <app.elf> [args]");
        return 10;
    }
    if (!stack_ok()) {
        puts("aar-embed: run Stack 32768 first.");
        return 10;
    }
    AARBase = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, AAR_LIB_VERSION);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    if (!AARBase || !GfxBase || !IntuitionBase) {
        puts("aar-embed: aar.library and Intuition are required.");
        goto done;
    }
    rc = AAROpen(&h, NULL, &reason);
    if (rc != AAR_OK) {
        printf("aar-embed: no claim (%ld)\n", (long)rc);
        goto done;
    }
    rc = AARLoadFile(h, argv[1]);
    if (rc == AAR_OK)
        rc = AARStart(h, (const char *const *)&argv[1], (ULONG)argc - 1UL);
    if (rc != AAR_OK) {
        printf("aar-embed: cannot start %s (%ld)\n", argv[1], (long)rc);
        goto done;
    }
    aar_serve_config_default(&cfg);
    cfg.flags = AAR_SERVEF_USE_SIGNAL | AAR_SERVEF_NO_INPUT_WINDOW |
                AAR_SERVEF_KEEP_MESSAGES;
    cfg.poll_us = 20000UL;
    cfg.screen_title = (CONST_STRPTR)"aar-embed";
    rc = aar_serve_create(h, &cfg, &s);
    if (rc != AAR_OK) {
        printf("aar-embed: cannot serve (%ld)\n", (long)rc);
        (void)AARStop(h);
        goto done;
    }
    win = OpenWindowTags(NULL,
                         WA_Title, (ULONG)"aar-embed",
                         WA_Width, 360, WA_Height, 40,
                         WA_CloseGadget, TRUE, WA_DragBar, TRUE,
                         WA_Activate, TRUE,
                         WA_IDCMP, IDCMP_CLOSEWINDOW,
                         TAG_DONE);
    if (!win) {
        puts("aar-embed: cannot open the window.");
        aar_serve_stop(s);
    }
    for (;;) {
        ULONG mine = win ? 1UL << win->UserPort->mp_SigBit : 0UL;
        /* Ask again each time: the loop's ports appear while the app runs. */
        ULONG got = Wait(mine | aar_serve_sigmask(s));

        if (win && (got & mine))
            handle_window(s, win);
        rc = aar_serve_step(s, got);
        if (win)
            show_messages(h, win);
        if (rc != AAR_SERVE_CONTINUE)
            break;
    }
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    if (aar_serve_result(s, &res) == AAR_OK) {
        printf("aar-embed: exit %ld\n", (long)res.exit_code);
        code = res.exit_code >= 0 && res.exit_code < 256 ? (int)res.exit_code : 20;
    }
done:
    if (win)
        CloseWindow(win);
    if (s)
        aar_serve_destroy(s);
    if (h)
        (void)AARClose(h);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (AARBase)
        CloseLibrary(AARBase);
    return code;
}
