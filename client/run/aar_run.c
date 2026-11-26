/*
 * aar-run: the reference launcher of the AAR standard (spec section 9.6). It
 * opens a claim on aar.library, gives it its options, loads and starts one app,
 * serves the app with aar_serve until it ends, and returns the app's exit code.
 * It runs on any platform behind aar.library: what the platform takes comes
 * from AARQuery, and the image is checked against it with AARProbeFile before a
 * claim is taken.
 *
 *   aar-run <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [WEIGHT=<n>] [AUDIO=AHI] [args]
 *   aar-run STATUS
 *   aar-run RELEASE <handle>
 *
 * This file is the arguments, the Workbench tool types, the library calls of the
 * launch sequence and the exit policy. The serving is client/serve (aar_serve.h);
 * STATUS and RELEASE go through AARExt and are aar_run_zz9000.c, which this file
 * includes; they say "not supported on this platform" where AARExt has no table.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <graphics/gfxbase.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/icon.h>
#include <proto/aar.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libraries/aar_serve.h>

#include "aar_run_policy.h"
#include "aar_run_workbench.h"
#include "aar_run_debug.h"

struct Library *AARBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library *IconBase;

extern struct WBStartup *_WBenchMsg;

static const char version[] __attribute__((used)) =
    "$VER: aar-run " AAR_RUN_VERSION " (20.11.2026)\r\n";

/* The console of a start without a Shell: one window titled aar-run, opened at
 * the start of the run. aar-run's own lines and the app's console both go to
 * it (the serving loop writes the app's output to this handle), and WAIT keeps
 * it open until the user closes it. If it cannot be opened, the loop opens the
 * same spec at the app's first console use. */
#define AAR_RUN_WB_CONSOLE "CON:0/0/640/160/aar-run/CLOSE/WAIT"

static BPTR wb_console;

/* aar-run never writes through libnix's stdout: for a Workbench start that would
 * open a second, untitled window (the m68k check refuses a reference to the C
 * library's printf and puts). */
static void out_text(const char *text)
{
    if (wb_console)
        (void)Write(wb_console, (APTR)text, (LONG)strlen(text));
    else
        fputs(text, stdout);
}

static int out_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int out_printf(const char *format, ...)
{
    char text[512];
    va_list args;
    int n;

    va_start(args, format);
    n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    out_text(text);
    return n;
}

static int out_puts(const char *text)
{
    out_text(text);
    out_text("\n");
    return 0;
}

static int out_putchar(int c)
{
    char text[2];

    text[0] = (char)c;
    text[1] = '\0';
    out_text(text);
    return c;
}

#define printf out_printf
#define puts out_puts
#define putchar out_putchar

/* What the launch gathered from the arguments and the tool types. */
static AARHandle *aar_handle;
static AARInfo platform_info;
static AARImageInfo image_note;
static int claim_open;
static aar_serve *g_serve;
static char elf_full[512];
static char elf_path[256];
static struct aar_run_args app_args;
static struct aar_run_settings settings;
static int doc_first = 1; /* the first WBArg that is a document */
static BPTR root_lock;
static int abs_paths;
static int joy_port0;
static int audio_ahi;
static uint32_t maxmem_mib;
static uint32_t hostmem_mib;
static uint32_t weight_value;
static struct aar_run_debug debug_opts;
static BPTR profile_file;
static int profile_wrote_header;
static uint32_t profile_granted;
static uint32_t profile_passes;
static unsigned long profile_samples;
static unsigned long profile_lost;
static uint8_t profile_header[AAR_RUN_PROFILE_HEADER_MAX];
static ULONG profile_buf[AAR_RUN_PROFILE_CHUNK * 2u];
static uint8_t crash_stack[AAR_CB_STACK_MAX];
static char crash_text[AAR_RUN_CRASH_BUF];
static uint32_t poll_us = AAR_RUN_DEFAULT_POLL_US;
static uint32_t display_strategy = AAR_SERVE_DISPLAY_BUFFERS;
static int launcher_output_active;
static unsigned long dropped_logs;
static unsigned long dropped_messages;
static unsigned long runtime_dropped_logs;

static void cleanup(void);

static int stack_ok(void)
{
    struct Process *process = (struct Process *)FindTask(NULL);
    uint32_t task_bytes = 0;
    uint32_t cli_longs = 0;
    int has_cli = 0;

    if (!process)
        return 0;
    if ((uintptr_t)process->pr_Task.tc_SPUpper > (uintptr_t)process->pr_Task.tc_SPLower)
        task_bytes = (uint32_t)((uintptr_t)process->pr_Task.tc_SPUpper -
                                (uintptr_t)process->pr_Task.tc_SPLower);
    if (process->pr_Task.tc_Node.ln_Type == NT_PROCESS && process->pr_CLI) {
        struct CommandLineInterface *cli =
            (struct CommandLineInterface *)BADDR(process->pr_CLI);

        has_cli = 1;
        cli_longs = (uint32_t)cli->cli_DefaultStack;
    }
    return aar_run_stack_ok(task_bytes, has_cli, cli_longs);
}

static int ctrl_c_set(void)
{
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

/* The claim's status, the smallest read: still ours? */
static int claim_check(void)
{
    AARStatusInfo st;

    memset(&st, 0, sizeof(st));
    st.size = offsetof(AARStatusInfo, generation);
    return aar_handle && AARStatus(aar_handle, &st) == AAR_OK;
}

/* The AAR_REASON_* of the last refused load or start. */
static ULONG claim_reason(void)
{
    AARStatusInfo st;

    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (aar_handle && AARStatus(aar_handle, &st) == AAR_OK)
        return st.reason;
    return AAR_REASON_NONE;
}

/* Did a Ctrl-C of the user stop the launch? aar_serve records the ones its
 * DOS packet waits consume. */
static int user_stop_seen(void)
{
    struct aar_serve_result res;

    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    if (g_serve)
        (void)aar_serve_result(g_serve, &res);
    return (res.flags & AAR_SERVE_R_USER_STOP) != 0;
}

/* The break latch of the launch (aar_run_break_latch). */
static int break_latched;

static int break_seen(void)
{
    break_latched = aar_run_break_latch(break_latched, ctrl_c_set(),
                                        user_stop_seen());
    return break_latched;
}

/* libnix's default __chkabort() ends the program with exit code 20 at the first
 * stdio call that finds Ctrl-C set. aar-run handles the break itself (the
 * load, the packet waits and the serving loop), and exits with 5 for it. */
void __chkabort(void)
{
}

static void usage(void)
{
    puts("aar-run <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [WEIGHT=<n>] [AUDIO=AHI] [args]\n"
         "aar-run STATUS\n"
         "aar-run RELEASE <handle>\n"
         "Diagnostics: POLL=<us> DISPLAY=<BUFFERS|PAN3|PAN2|SINGLE>\n"
         "Debugging: CRASHLOG=<path> PROFILE=<hz> PROFILEFILE=<path>\n"
         "Run Stack 32768 in this Shell first.\n"
         "Workbench ToolTypes: APP, ROOT, ABS, MODE, ARGS, JOYPORT0, AUDIO, MAXMEM, HOSTMEM, WEIGHT,\n"
         "CRASHLOG, PROFILE, PROFILEFILE.\n"
         "ROOT defaults to the current directory, or the icon directory.");
}

/* One launcher line: through the serving loop's console once it runs (the
 * text goes out in order with the app's), else on the Shell's output. */
static void say(const char *msg)
{
    if (launcher_output_active && g_serve)
        aar_serve_status(g_serve, msg);
    else
        puts(msg);
}

/* The requester shows msg as an argument of a fixed "%s" format, never as the
 * format: a '%' in a file name stays one. */
static void requester(const char *msg)
{
    if (_WBenchMsg && IntuitionBase) {
        struct EasyStruct es;
        const char *args[1]; /* a 32-bit pointer on AmigaOS */

        args[0] = msg;
        memset(&es, 0, sizeof(es));
        es.es_StructSize = sizeof(es);
        es.es_Title = (UBYTE *)"aar-run";
        es.es_TextFormat = (UBYTE *)"%s";
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(NULL, &es, NULL, args);
    }
}

static void tell(const char *msg)
{
    say(msg);
    requester(msg);
}

static const char *fault_name(uint32_t code)
{
    return aar_run_fault_class_name(code);
}

static const char *state_name(uint32_t state)
{
    return aar_run_slot_state_name(state);
}

/* An argument for the app. */
static int add_arg(const char *text)
{
    if (aar_run_args_add(&app_args, text) != AAR_RUN_ARGS_OK) {
        tell("aar-run: out of memory.");
        return 0;
    }
    return 1;
}

static int add_args_split(const char *text)
{
    int r = aar_run_args_add_words(&app_args, text);

    if (r == AAR_RUN_ARGS_QUOTE) {
        tell("aar-run: ARGS has a double quote that is never closed.");
        return 0;
    }
    if (r != AAR_RUN_ARGS_OK) {
        tell("aar-run: out of memory.");
        return 0;
    }
    return 1;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
static const char *icon_tool_type(void *icon, const char *key)
{
    struct DiskObject *object = (struct DiskObject *)icon;

    if (!object->do_ToolTypes)
        return NULL;
    return (const char *)FindToolType((STRPTR *)object->do_ToolTypes,
                                      (STRPTR)key);
}
#pragma GCC diagnostic pop

/* The ToolTypes of a Workbench start: the icon of the project first (the ELF's
 * own icon, or a document's), then aar-run's own for the keys the project does
 * not set. A Shell start has no ToolTypes. */
static int load_tooltypes(void)
{
    struct DiskObject *tool_icon = NULL;
    struct DiskObject *project_icon = NULL;
    struct aar_run_tt_chain chain;
    const char *bad_key = NULL;
    int ok = 1;

    memset(&settings, 0, sizeof(settings));
    if (!IconBase || !_WBenchMsg || _WBenchMsg->sm_NumArgs < 1)
        return 1;
    {
        const struct WBArg *tool = &_WBenchMsg->sm_ArgList[0];
        BPTR old = CurrentDir(tool->wa_Lock);

        tool_icon = GetDiskObject(tool->wa_Name);
        if (_WBenchMsg->sm_NumArgs >= 2) {
            const struct WBArg *project = &_WBenchMsg->sm_ArgList[1];

            if (project->wa_Name && project->wa_Name[0]) {
                CurrentDir(project->wa_Lock);
                project_icon = GetDiskObject(project->wa_Name);
            } else if (project->wa_Lock) {
                /* A drawer is its own lock with no name: its icon is found
                 * by the drawer's path (a volume's by its name). */
                char directory[512];
                char path[512];

                if (NameFromLock(project->wa_Lock, (STRPTR)directory,
                                 (LONG)sizeof(directory)) &&
                    aar_run_drawer_icon_path(directory, path, sizeof(path)))
                    project_icon = GetDiskObject((STRPTR)path);
            }
        }
        CurrentDir(old);
    }
    chain.lookup = icon_tool_type;
    chain.project = project_icon;
    chain.tool = tool_icon;
    if (!aar_run_tt_settings(&chain, &settings, &bad_key) ||
        !aar_run_debug_tooltypes(&chain, &debug_opts, &bad_key)) {
        printf("aar-run: bad %s ToolType\n", bad_key);
        ok = 0;
    } else {
        abs_paths = settings.abs_paths;
        joy_port0 = settings.joy_port0;
        audio_ahi = settings.audio_ahi;
        maxmem_mib = settings.maxmem_mib;
        hostmem_mib = settings.hostmem_mib;
        weight_value = settings.weight;
        ok = !settings.args || add_args_split(settings.args);
    }
    settings.args = NULL; /* it belonged to an icon that is freed now */
    if (tool_icon)
        FreeDiskObject(tool_icon);
    if (project_icon)
        FreeDiskObject(project_icon);
    return ok;
}

/* ---- The launch ------------------------------------------------------------------------------ */

static int open_root(const char *root_text)
{
    if (root_text && root_text[0]) {
        root_lock = Lock((STRPTR)root_text, ACCESS_READ);
        if (!root_lock) {
            printf("aar-run: cannot lock ROOT %s\n", root_text);
            return 0;
        }
        return 1;
    }
    if (_WBenchMsg && _WBenchMsg->sm_NumArgs >= 1) {
        root_lock = DupLock(_WBenchMsg->sm_ArgList[0].wa_Lock);
    } else {
        BPTR cur = CurrentDir(0);
        CurrentDir(cur);
        root_lock = DupLock(cur);
    }
    if (!root_lock) {
        puts("aar-run: cannot lock the root directory");
        return 0;
    }
    return 1;
}

static int open_library(void)
{
    AARBase = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, AAR_LIB_VERSION);
    if (!AARBase) {
        tell("aar.library not found. Install Libs:aar.library for this "
             "platform.");
        return 0;
    }
    if (!aar_run_library_ok(AARBase->lib_Version, AARBase->lib_Revision)) {
        char line[256];

        aar_run_library_line(line, sizeof(line), AARBase->lib_Version,
                             AARBase->lib_Revision);
        tell(line);
        CloseLibrary(AARBase);
        AARBase = NULL;
        return 0;
    }
    return 1;
}

static int path_is_directory(const char *path)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    int directory = 0;

    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock)
        return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lock, fib))
            directory = fib->fib_DirEntryType > 0;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return directory;
}

/* Whether the first project of a Workbench start is the app (an AAR image) or
 * a document. Needs aar.library open. */
static int project_is_app(const char *path)
{
    AARImageInfo note;
    LONG rc;

    memset(&note, 0, sizeof(note));
    note.size = sizeof(note);
    rc = AARProbeFile(path, &note);
    return aar_run_project_is_app(path_is_directory(path), (int)rc);
}

/* The documents of a Workbench start follow the ARGS words in the app's argv,
 * as paths relative to ROOT where they lie under it, else in full. */
static int add_documents(void)
{
    char root_full[512];
    char path[512];
    char text[512];
    LONG i;

    if (!_WBenchMsg)
        return 1;
    root_full[0] = 0;
    if (root_lock && !NameFromLock(root_lock, (STRPTR)root_full,
                                   (LONG)sizeof(root_full)))
        root_full[0] = 0;
    for (i = doc_first; i < _WBenchMsg->sm_NumArgs; i++) {
        if (!aar_run_project_path(&_WBenchMsg->sm_ArgList[i], path,
                                  sizeof(path)) ||
            aar_run_doc_arg(root_full, path, text, sizeof(text)) ==
                AAR_RUN_DOC_NOFIT) {
            tell("aar-run: the path of a document is too long.");
            return 0;
        }
        if (!add_arg(text))
            return 0;
    }
    return 1;
}

/* argv[0] is the app's file name, as C programs expect; the Shell or ARGS
 * tokens follow it. */
static int start_app(void)
{
    const char **argv_list;
    char line[160];
    int i;
    LONG rc;

    argv_list = (const char **)malloc(((size_t)app_args.n + 1u) * sizeof(char *));
    if (!argv_list) {
        puts("aar-run: out of memory.");
        return 0;
    }
    argv_list[0] = (const char *)FilePart((STRPTR)elf_path);
    for (i = 0; i < app_args.n; i++)
        argv_list[i + 1] = app_args.v[i];
    rc = AARStart(aar_handle, argv_list, (ULONG)app_args.n + 1u);
    free(argv_list);
    if (rc != AAR_OK) {
        aar_run_failure_line(line, sizeof(line), "start", rc,
                             rc == AAR_E_NORESOURCE ? claim_reason() :
                                                      AAR_REASON_NONE);
        puts(line);
        return 0;
    }
    return 1;
}

static void report_fault(uint32_t code, int32_t exit_code, uint32_t pc,
                         uint32_t sp, uint32_t lr, uint32_t far, uint32_t fsr,
                         uint32_t cpsr, uint32_t generation)
{
    char line[160];

    snprintf(line, sizeof(line), "aar-run: %s", fault_name(code));
    say(line);
    aar_run_reg_line(line, sizeof(line), pc, sp, lr);
    say(line);
    snprintf(line, sizeof(line), "  far=%08lx fsr=%08lx cpsr=%08lx",
             (unsigned long)far, (unsigned long)fsr, (unsigned long)cpsr);
    say(line);
    snprintf(line, sizeof(line), "  exit=%ld generation=%08lx",
             (long)exit_code, (unsigned long)generation);
    say(line);
    if (_WBenchMsg && IntuitionBase) {
        snprintf(line, sizeof(line),
                 "%s\\npc=%08lx far=%08lx\\nexit=%ld",
                 fault_name(code), (unsigned long)pc, (unsigned long)far,
                 (long)exit_code);
        requester(line);
    }
}

/* A stall: the watchdog ended the app, and the fault record holds where it was. */
static void report_stall(const AARStatusInfo *st)
{
    char line[160];

    snprintf(line, sizeof(line), "aar-run: %s", aar_run_end_reason_name(st->end_reason));
    say(line);
    aar_run_reg_line(line, sizeof(line), st->fault_pc.lo, st->fault_sp.lo, st->fault_lr.lo);
    say(line);
    snprintf(line, sizeof(line), "  exit=%ld generation=%08lx", (long)st->exit_code,
             (unsigned long)st->generation);
    say(line);
}

/* CRASHLOG: the registers and the stack snapshot of a fault or a stall, in a
 * text file for tools/aar-symbolize. */
static void write_crash_log(const AARStatusInfo *st)
{
    char line[320];
    ULONG got = 0;
    BPTR file;
    size_t n;

    if (!debug_opts.crashlog[0])
        return;
    if (AARReadStack(aar_handle, crash_stack, sizeof(crash_stack), &got) != AAR_OK)
        got = 0;
    n = aar_run_crash_report(crash_text, sizeof(crash_text), elf_full, st, crash_stack, got);
    file = Open((CONST_STRPTR)debug_opts.crashlog, MODE_NEWFILE);
    if (file && Write(file, crash_text, (LONG)n) == (LONG)n)
        snprintf(line, sizeof(line), "aar-run: crash report written to %s", debug_opts.crashlog);
    else
        snprintf(line, sizeof(line), "aar-run: cannot write the crash report to %s",
                 debug_opts.crashlog);
    say(line);
    if (file)
        Close(file);
}

/* PROFILEFILE: the file is created before the load, so that a path that cannot
 * be written stops the launch and not the end of a long run. */
static int profile_open(void)
{
    if (!debug_opts.profilefile[0])
        return 1;
    profile_file = Open((CONST_STRPTR)debug_opts.profilefile, MODE_NEWFILE);
    if (!profile_file) {
        printf("aar-run: cannot create %s\n", debug_opts.profilefile);
        return 0;
    }
    return 1;
}

/* No more samples: the file is closed, and removed when it has no header. */
static void profile_drop(void)
{
    if (profile_file) {
        Close(profile_file);
        profile_file = 0;
        if (!profile_wrote_header)
            (void)DeleteFile((CONST_STRPTR)debug_opts.profilefile);
    }
}

/* After the start: what the platform granted, and the header of the file. */
static void profile_begin(void)
{
    char line[320];
    AARStatusInfo st;
    size_t n;

    if (!profile_file)
        return;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (AARStatus(aar_handle, &st) != AAR_OK || st.profile_hz == 0) {
        say("aar-run: this platform grants no profile, so PROFILEFILE is not written.");
        profile_drop();
        return;
    }
    n = aar_run_profile_header(profile_header, sizeof(profile_header), st.profile_hz,
                               st.load_base, elf_full);
    if (!n || Write(profile_file, profile_header, (LONG)n) != (LONG)n) {
        say("aar-run: cannot write the profile file.");
        profile_drop();
        return;
    }
    profile_wrote_header = 1;
    profile_granted = st.profile_hz;
    snprintf(line, sizeof(line), "aar-run: sampling the pc at %lu Hz into %s",
             (unsigned long)st.profile_hz, debug_opts.profilefile);
    say(line);
}

/* Takes what the ring holds, at most max_chunks of AAR_RUN_PROFILE_CHUNK samples,
 * so that a platform that writes as fast as the file can take them does not keep
 * the loop from serving the app. A failure of the library or of the file ends the
 * profile; what was written stays. */
static void profile_drain(uint32_t max_chunks)
{
    ULONG got, lost;
    uint32_t chunks = 0;

    while (profile_file) {
        got = 0;
        lost = 0;
        if (AARReadProfile(aar_handle, profile_buf, AAR_RUN_PROFILE_CHUNK, &got, &lost) !=
            AAR_OK)
            return;
        profile_lost += lost;
        if (got) {
            LONG bytes = (LONG)(got * AAR_PROFILE_SAMPLE_BYTES);

            if (Write(profile_file, profile_buf, bytes) != bytes) {
                say("aar-run: cannot write the profile file.");
                profile_drop();
                return;
            }
            profile_samples += got;
        }
        if (!aar_run_profile_drain_more(++chunks, max_chunks, got))
            return;
    }
}

/* The last samples, the lost count into the header, the file closed. */
static void profile_end(void)
{
    char line[320];
    uint8_t word[4];

    if (!profile_file)
        return;
    profile_drain(AAR_RUN_PROFILE_END_CHUNKS);
    if (!profile_file)
        return;
    aar_run_profile_lost_word(word, (uint32_t)profile_lost);
    if (Seek(profile_file, AAR_RUN_PROFILE_LOST_OFFSET, OFFSET_BEGINNING) >= 0)
        (void)Write(profile_file, word, 4);
    aar_run_profile_summary_line(line, sizeof(line), profile_granted, profile_samples,
                                 profile_lost, debug_opts.profilefile);
    say(line);
    profile_drop();
}

/* aar_serve_run, with a turn for the profile ring between the passes. */
static LONG serve_loop(void)
{
    LONG rc;

    if (!profile_file)
        return aar_serve_run(g_serve);
    do {
        rc = aar_serve_step(g_serve, Wait(aar_serve_sigmask(g_serve)));
        if (aar_run_profile_due(&profile_passes))
            profile_drain(AAR_RUN_PROFILE_PASS_CHUNKS);
    } while (rc == AAR_SERVE_CONTINUE);
    return rc;
}

/* The report after the loop and the process exit code. */
static int finish_run(LONG ended)
{
    struct aar_serve_result res;
    char line[160];
    const AARStatusInfo *st;
    LONG rc;

    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    rc = aar_serve_result(g_serve, &res);
    dropped_logs = (unsigned long)res.dropped_logs;
    dropped_messages = (unsigned long)res.dropped_messages;
    runtime_dropped_logs = (unsigned long)res.runtime_dropped_logs;
    if (audio_ahi)
        printf("aar-run: AHI underruns %lu, ran dry %lu, requests %lu (frames min %lu avg %lu), "
               "heartbeat %lu us (required 1..20000 us).\n",
               (unsigned long)res.ahi_underruns, (unsigned long)res.ahi_gaps,
               (unsigned long)res.ahi_requests, (unsigned long)res.ahi_min_frames,
               (unsigned long)res.ahi_avg_frames, (unsigned long)poll_us);
    if (ended == AAR_SERVE_LOST) {
        snprintf(line, sizeof(line), "aar-run: claim lost (%s).",
                 aar_run_claim_loss_text(1, AAR_END_NONE));
        say(line);
        return 20;
    }
    if (rc != AAR_OK) {
        say("aar-run: the app's final status could not be read.");
        return 20;
    }
    st = &res.status;
    if (st->fault_class || (res.flags & AAR_SERVE_R_FAULTED))
        report_fault(st->fault_class, st->exit_code, st->fault_pc.lo,
                     st->fault_sp.lo, st->fault_lr.lo, st->fault_addr.lo,
                     st->fault_syndrome, st->fault_pstate, st->generation);
    else if (st->end_reason == AAR_END_STALL)
        report_stall(st);
    else if (aar_run_claim_loss_text(0, st->end_reason)) {
        snprintf(line, sizeof(line), "aar-run: claim lost (%s).",
                 aar_run_claim_loss_text(0, st->end_reason));
        say(line);
    } else {
        snprintf(line, sizeof(line), "aar-run: exit %ld (%s)",
                 (long)st->exit_code, state_name(st->state));
        say(line);
    }
    if (aar_run_crash_wanted(st->end_reason, st->fault_class,
                             (res.flags & AAR_SERVE_R_FAULTED) != 0))
        write_crash_log(st);
    return aar_run_process_exit(
        aar_run_app_exit_code(st->end_reason, st->exit_code,
                              (res.flags & AAR_SERVE_R_STOP_ASKED) != 0),
        (res.flags & AAR_SERVE_R_DISPLAY_FAILED) != 0,
        (res.flags & AAR_SERVE_R_CONSOLE_FAILED) != 0,
        (res.flags & AAR_SERVE_R_AUDIO_FAILED) != 0,
        (res.flags & AAR_SERVE_R_QUEUE_CORRUPT) != 0);
}

static void cleanup(void)
{
    if (g_serve) {
        aar_serve_destroy(g_serve);
        g_serve = NULL;
    }
    if (profile_file) {
        Close(profile_file);
        profile_file = 0;
    }
    if (root_lock) {
        UnLock(root_lock);
        root_lock = 0;
    }
    aar_run_args_free(&app_args);
    /* The library returns the control block, the queues, every service buffer
     * and every map at the close, so nothing above may touch them afterwards. */
    if (claim_open) {
        (void)AARClose(aar_handle);
        aar_handle = NULL;
        claim_open = 0;
    }
    if (AARBase) {
        CloseLibrary(AARBase);
        AARBase = NULL;
    }
    if (IconBase) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (launcher_output_active) {
        launcher_output_active = 0;
        if (dropped_logs || runtime_dropped_logs)
            printf("aar-run: log records not shown: %lu dropped by the runtime "
                   "(its queue was full), %lu by the launcher's console queue.\n",
                   runtime_dropped_logs, dropped_logs);
        if (dropped_messages)
            printf("aar-run: dropped %lu app messages.\n", dropped_messages);
    }
    if (wb_console) {
        Close(wb_console);
        wb_console = 0;
    }
}

/* The claim gate before each load step: a claim the platform has already
 * revoked is not worth another step. The library renews the lease, so this
 * only asks whether the claim is still ours. */
static int claim_gone(void)
{
    return !claim_check();
}

/* The library's options, before the load (the old limits write and client
 * capabilities of LOAD_BEGIN). */
static int apply_options(void)
{
    struct TagItem tags[AAR_RUN_TAG_MAX + 1u];
    char line[160];
    LONG rc;
    unsigned n;

    n = aar_run_build_tags(tags, maxmem_mib, hostmem_mib, weight_value, audio_ahi);
    (void)aar_run_debug_tags(tags, n, debug_opts.profile_hz);
    rc = AARSetTagList(aar_handle, tags);
    if (rc == AAR_OK)
        return 1;
    aar_run_tags_failure_line(line, sizeof(line), rc);
    puts(line);
    return 0;
}

static void load_failed(LONG rc)
{
    char line[200];
    /* Decided before any output: a stdio call may consume the break. */
    int broke = break_seen();

    aar_run_load_failure_line(line, sizeof(line), rc,
                              rc == AAR_E_NORESOURCE ? claim_reason() :
                                                       AAR_REASON_NONE,
                              broke, platform_info.max_image_bytes);
    puts(line);
}

/* The image against the platform, before any claim (D5): AARProbeFile needs no
 * handle. */
static enum aar_run_probe_verdict probe_image(const char *path)
{
    LONG rc;

    memset(&image_note, 0, sizeof(image_note));
    image_note.size = sizeof(image_note);
    rc = AARProbeFile(path, &image_note);
    return aar_run_probe_check((int)rc, &image_note, &platform_info);
}

/* Say why the image cannot run, and return the exit code. */
static int refuse_image(enum aar_run_probe_verdict v, const char *path)
{
    char line[320];
    /* Decided before any output: a stdio call may consume the break. */
    int broke = break_seen();

    aar_run_probe_line(line, sizeof(line), v, path, &image_note, &platform_info);
    tell(line);
    return aar_run_probe_exit(v, broke, 0);
}

/* One command-line word of the options aar-run takes itself: the diagnostics
 * and WEIGHT, matched without regard to case. AAR_RUN_ARG_OTHER: not one. */
static int take_option(const char *arg)
{
    int r = aar_run_weight_arg(arg, &weight_value);

    if (r != AAR_RUN_ARG_OTHER)
        return r;
    r = aar_run_poll_arg(arg, &poll_us);
    if (r != AAR_RUN_ARG_OTHER)
        return r;
    r = aar_run_display_arg(arg, &display_strategy);
    if (r != AAR_RUN_ARG_OTHER)
        return r;
    return aar_run_debug_arg(arg, &debug_opts);
}

/* The image files of the app: the one asked for, then its profile files
 * (aar_run_image_candidates). */
static char cand[AAR_RUN_CAND_MAX][sizeof(elf_path)];
static char cand_shown[AAR_RUN_CAND_MAX][sizeof(elf_full)];
static enum aar_run_probe_verdict cand_verdict[AAR_RUN_CAND_MAX];
static size_t cand_count;

/* Probes the candidates in order until one runs, without a claim. Returns its
 * index. -1: none does; *pending is then the first candidate whose name may be
 * a multi-directory assign (it, and the ones after it, wait for the locate
 * after the claim), or cand_count when every candidate has been judged. */
static int probe_candidates(size_t *pending)
{
    size_t i;

    cand_count = aar_run_image_candidates(elf_path, platform_info.profiles,
                                          &cand[0][0], sizeof(cand[0]));
    *pending = cand_count;
    for (i = 0; i < AAR_RUN_CAND_MAX; i++)
        cand_verdict[i] = AAR_RUN_PROBE_NOENT;
    for (i = 0; i < cand_count; i++) {
        memcpy(cand_shown[i], cand[i], strlen(cand[i]) + 1);
        cand_verdict[i] = probe_image(cand[i]);
        if (cand_verdict[i] == AAR_RUN_PROBE_OK)
            return (int)i;
        if (aar_run_probe_may_wait(cand_verdict[i], cand[i])) {
            *pending = i;
            return -1;
        }
    }
    return -1;
}

/* No candidate runs: say why, with the exit code. */
static int refuse_candidates(void)
{
    size_t r = aar_run_report_candidate(cand_verdict, cand_count);
    enum aar_run_probe_verdict v = cand_verdict[r];

    if (v != AAR_RUN_PROBE_NOENT)
        (void)probe_image(cand_shown[r]); /* the note the line describes */
    return refuse_image(v, cand_shown[r]);
}

static LONG locate_image(const char *name)
{
    BPTR base_dir = 0;

    if (!strchr(name, ':')) {
        base_dir = CurrentDir(0);
        CurrentDir(base_dir);
    }
    return aar_serve_locate(g_serve, base_dir, name, elf_full,
                            sizeof(elf_full));
}

/* After the claim: finds the chosen candidate on disk (elf_full) and judges
 * the ones that waited for the locate, in order. Returns 1 with elf_path the
 * name of the image to load; 0 with the exit code in *rc. */
static int locate_and_choose(int chosen, size_t pending, int *rc)
{
    size_t i = chosen >= 0 ? (size_t)chosen : pending;

    for (; i < cand_count; i++) {
        LONG status = locate_image(cand[i]);

        if (status == AAR_E_NOENT) {
            cand_verdict[i] = AAR_RUN_PROBE_NOENT;
            chosen = -1;
            continue;
        }
        if (status != AAR_OK) {
            printf("aar-run: cannot resolve the path of %s\n", cand[i]);
            *rc = 20;
            return 0;
        }
        if (chosen < 0) {
            memcpy(cand_shown[i], elf_full, strlen(elf_full) + 1);
            cand_verdict[i] = probe_image(elf_full);
            if (cand_verdict[i] != AAR_RUN_PROBE_OK)
                continue;
        }
        memcpy(elf_path, cand[i], strlen(cand[i]) + 1);
        return 1;
    }
    *rc = refuse_candidates();
    return 0;
}

/* The default ROOT of a Workbench start is the directory that holds the image
 * that runs. Locks that directory (the image as a name that Lock finds) and
 * makes it the root. Returns 1 when the root changed; 0 when it is that
 * directory already or cannot be found, which leaves the root as it was. */
static int retarget_root(const char *image)
{
    char full[512];
    char parent[512];
    BPTR lock;
    BPTR dir;

    lock = Lock((STRPTR)image, ACCESS_READ);
    if (!lock)
        return 0;
    if (!NameFromLock(lock, (STRPTR)full, (LONG)sizeof(full))) {
        UnLock(lock);
        return 0;
    }
    UnLock(lock);
    if (!aar_run_path_parent(full, parent, sizeof(parent)))
        return 0;
    dir = Lock((STRPTR)parent, ACCESS_READ);
    if (!dir)
        return 0;
    if (root_lock && SameLock(root_lock, dir) == LOCK_SAME) {
        UnLock(dir);
        return 0;
    }
    if (root_lock)
        UnLock(root_lock);
    root_lock = dir;
    return 1;
}

/* The arguments, WEIGHT and HOSTMEM against what the platform takes. Prints
 * the refusal. Returns 1 when the launch is within the limits. */
static int check_limits(void)
{
    struct aar_run_limits limits = aar_run_limits_from_info(&platform_info);
    enum aar_run_limit_verdict verdict = aar_run_limits_check(
        &limits, (uint32_t)app_args.n, app_args.longest, weight_value,
        hostmem_mib);
    char line[160];

    if (verdict == AAR_RUN_LIMIT_OK)
        return 1;
    aar_run_limit_line(line, sizeof(line), verdict, &limits);
    tell(line);
    return 0;
}

/* The claim, the serving state on the root, and the library's options. Returns
 * 0 with the exit code in *rc; cleanup closes what was opened. */
static int begin_claim(int *rc)
{
    aar_serve_config cfg;
    ULONG reason = 0;
    LONG status;

    status = AAROpen(&aar_handle, NULL, &reason);
    if (status != AAR_OK) {
        char line[200];

        aar_handle = NULL;
        aar_run_claim_failure_line(line, sizeof(line), status, reason);
        puts(line);
        *rc = 20;
        return 0;
    }
    claim_open = 1;
    aar_serve_config_default(&cfg);
    cfg.flags = abs_paths ? AAR_SERVEF_ABS_PATHS : 0UL;
    cfg.root = root_lock;
    cfg.screen_mode = settings.have_mode ? settings.mode : 0;
    cfg.screen_title = (CONST_STRPTR)"aar-run";
    cfg.console_in = wb_console;
    cfg.console_out = wb_console;
    cfg.console_spec = _WBenchMsg && !wb_console ? (CONST_STRPTR)AAR_RUN_WB_CONSOLE : NULL;
    cfg.audio_route = audio_ahi ? AAR_AUDIO_ROUTE_AHI : AAR_AUDIO_ROUTE_PLATFORM;
    cfg.joy_ports = joy_port0 ? 3UL : 2UL;
    cfg.poll_us = poll_us;
    if (!poll_us)
        cfg.flags |= AAR_SERVEF_USE_SIGNAL;
    cfg.display_strategy = display_strategy;
    status = aar_serve_create(aar_handle, &cfg, &g_serve);
    if (status != AAR_OK) {
        char line[160];

        if (status == AAR_E_NOSYS) {
            tell("timer.device is required.");
        } else {
            aar_run_failure_line(line, sizeof(line), "serve", status,
                                 AAR_REASON_NONE);
            tell(line);
        }
        *rc = 20;
        return 0;
    }
    /* The options go to the library before the load. */
    if (!apply_options()) {
        *rc = aar_run_load_failure_exit(break_seen(), 0);
        return 0;
    }
    if (claim_gone()) {
        *rc = 20;
        return 0;
    }
    return 1;
}

/* The claim and its serving state, closed again before the load. */
static void end_claim(void)
{
    if (g_serve) {
        aar_serve_destroy(g_serve);
        g_serve = NULL;
    }
    if (claim_open) {
        (void)AARClose(aar_handle);
        aar_handle = NULL;
        claim_open = 0;
    }
}

#include "aar_run_zz9000.c"

int main(int argc, char **argv)
{
    int chosen;
    size_t pending;
    LONG status;
    enum aar_run_query_verdict verdict;
    int root_follows_image;
    int root_final;
    int rc = 20;
    int show_status = 0;
    uint32_t release_handle = 0;

    if (_WBenchMsg)
        wb_console = Open((CONST_STRPTR)AAR_RUN_WB_CONSOLE, MODE_OLDFILE);
    elf_path[0] = 0;
    if (!stack_ok()) {
        tell("Run Stack 32768 in this Shell before aar-run.");
        return 10;
    }
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 37);
    if (!GfxBase || !IntuitionBase) {
        tell("AmigaOS 3 graphics and Intuition are required.");
        cleanup();
        return 20;
    }
    if (!load_tooltypes()) {
        usage();
        cleanup();
        return 10;
    }
    if (_WBenchMsg) {
        if (_WBenchMsg->sm_NumArgs >= 2) {
            char project[256];

            if (!aar_run_project_path(&_WBenchMsg->sm_ArgList[1], project,
                                      sizeof(project))) {
                tell("aar-run: the path of the project is too long.");
                cleanup();
                return 10;
            }
            if (!AARBase && !open_library()) {
                cleanup();
                return 20;
            }
            if (project_is_app(project)) {
                memcpy(elf_path, project, strlen(project) + 1);
                doc_first = 2;
            }
        }
        if (!elf_path[0] && settings.app[0])
            memcpy(elf_path, settings.app, strlen(settings.app) + 1);
        if (!elf_path[0]) {
            tell("Set the APP tooltype, or start aar-run from a project icon.");
            cleanup();
            return 10;
        }
    } else {
        int i;

        if (argc == 3 && strcmp(argv[1], "RELEASE") == 0) {
            if (!aar_run_parse_handle(argv[2], &release_handle)) {
                usage();
                cleanup();
                return 10;
            }
        } else if (argc == 2 && aar_run_is_status_keyword(argv[1])) {
            show_status = 1;
        } else {
            if (argc < 2) {
                usage();
                cleanup();
                return 10;
            }
            if (strlen(argv[1]) >= sizeof(elf_path)) {
                tell("ELF path is too long.");
                cleanup();
                return 10;
            }
            memcpy(elf_path, argv[1], strlen(argv[1]) + 1);
            for (i = 2; i < argc; i++) {
                int opt;

                if (strncmp(argv[i], "AUDIO=", 6) == 0) {
                    if (!aar_run_tool_is_ahi(argv[i] + 6)) {
                        tell("AUDIO must be AHI.");
                        cleanup();
                        return 10;
                    }
                    audio_ahi = 1;
                    continue;
                }
                opt = take_option(argv[i]);
                if (opt == AAR_RUN_ARG_BAD) {
                    printf("aar-run: bad value in %s\n", argv[i]);
                    usage();
                    cleanup();
                    return 10;
                }
                if (opt == AAR_RUN_ARG_TAKEN)
                    continue;
                if (strncmp(argv[i], "HOSTMEM=", 8) == 0) {
                    if (!aar_run_parse_positive(argv[i] + 8, &hostmem_mib)) {
                        usage();
                        cleanup();
                        return 10;
                    }
                    continue;
                }
                if (strncmp(argv[i], "MAXMEM=", 7) == 0) {
                    if (!aar_run_parse_positive(argv[i] + 7, &maxmem_mib)) {
                        usage();
                        cleanup();
                        return 10;
                    }
                    continue;
                }
                if (!add_arg(argv[i])) {
                    cleanup();
                    return 10;
                }
            }
        }
    }
    if (!show_status && !release_handle) {
        enum aar_run_debug_verdict dv = aar_run_debug_check(&debug_opts);

        if (dv != AAR_RUN_DEBUG_OK) {
            char line[96];

            aar_run_debug_line(line, sizeof(line), dv);
            tell(line);
            usage();
            cleanup();
            return 10;
        }
    }
    if (!show_status && !release_handle && !open_root(settings.root)) {
        cleanup();
        return 20;
    }
    if (!AARBase && !open_library()) {
        cleanup();
        return 20;
    }
    memset(&platform_info, 0, sizeof(platform_info));
    platform_info.size = sizeof(platform_info);
    status = AARQuery(&platform_info);
    if (status != AAR_OK) {
        char line[160];

        aar_run_query_failure_line(line, sizeof(line), status);
        tell(line);
        cleanup();
        return 20;
    }
    if (!show_status && !release_handle)
        printf("aar-run %s\n", AAR_RUN_VERSION);
    if (!show_status) {
        printf("runtime api %lu.%lu",
               (unsigned long)platform_info.api_major,
               (unsigned long)platform_info.api_minor);
        zz9000_print_caps(&platform_info);
        putchar('\n');
        zz9000_print_note(&platform_info);
    }
    verdict = aar_run_query_check(&platform_info);
    if (verdict != AAR_RUN_QUERY_OK) {
        tell(aar_run_query_verdict_text(verdict));
        cleanup();
        return 20;
    }
    if (show_status || release_handle) {
        if (!zz9000_init()) {
            char line[80];

            aar_run_admin_unsupported_line(line, sizeof(line),
                                           show_status ? "STATUS" : "RELEASE");
            tell(line);
            cleanup();
            return 20;
        }
        rc = show_status ? zz9000_print_status()
                         : zz9000_release_claim(release_handle);
        cleanup();
        return rc;
    }
    /* The image next, still without a claim: the file asked for, then its
     * profile files. A name that may be a multi-directory assign is probed
     * after the locate below. */
    chosen = probe_candidates(&pending);
    /* Without a ROOT ToolType the root of a Workbench start is the directory of
     * the image that runs. The probe has found it, unless the name waits for
     * the locate after the claim. The documents are paths relative to the root,
     * so they are added once the root is known. */
    root_follows_image = _WBenchMsg && !settings.root[0];
    root_final = !root_follows_image || chosen >= 0;
    if (root_follows_image && chosen >= 0)
        (void)retarget_root(cand[chosen]);
    if (root_final && !add_documents()) {
        cleanup();
        return 10;
    }
    /* What the platform takes decides what a launch may ask for; nothing is
     * claimed for a launch it would refuse. */
    if (!check_limits()) {
        cleanup();
        return 10;
    }
    if (chosen < 0 && pending == cand_count) {
        rc = refuse_candidates();
        cleanup();
        return rc;
    }
    if (!begin_claim(&rc)) {
        cleanup();
        return rc;
    }
    if (!locate_and_choose(chosen, pending, &rc)) {
        cleanup();
        return rc;
    }
    if (strcmp(elf_path, cand[0]) != 0) {
        char line[2 * sizeof(elf_path) + 48];

        aar_run_choice_line(line, sizeof(line), cand[0], elf_path);
        puts(line);
    }
    if (!root_final) {
        /* The locate found the image: its directory is the root. The serving
         * state has the old root, so the claim is opened again on the new one. */
        if (retarget_root(elf_full)) {
            end_claim();
            if (!begin_claim(&rc)) {
                cleanup();
                return rc;
            }
        }
        if (!add_documents() || !check_limits()) {
            cleanup();
            return 10;
        }
    }
    if (claim_gone()) {
        cleanup();
        return 20;
    }
    if (!profile_open()) {
        cleanup();
        return 20;
    }
    printf("loading %s\n", elf_full);
    status = AARLoadFile(aar_handle, elf_full);
    if (status != AAR_OK) {
        load_failed(status);
        rc = aar_run_load_failure_exit(break_seen(), 0);
        cleanup();
        return rc;
    }
    if (!start_app()) {
        (void)AARStop(aar_handle);
        cleanup();
        return 20;
    }
    profile_begin();
    fflush(stdout);
    launcher_output_active = 1;
    {
        LONG ended = serve_loop();

        profile_end();
        rc = finish_run(ended);
    }
    cleanup();
    return rc;
}
