/*
 * aar-run: the reference launcher of the AAR standard (spec section 9.6). It opens a
 * claim on aar.library, gives it its options, loads and starts one app, serves
 * the app with aar_serve until it ends, and returns the app's exit code.
 *
 *   aar-run <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [WEIGHT=<1-256>] [AUDIO=AHI] [args]
 *   aar-run STATUS
 *   aar-run RELEASE <handle>
 *
 * This file is the arguments, the Workbench tool types, the library calls of the
 * launch sequence and the exit policy. The serving is client/serve (aar_serve.h);
 * STATUS and RELEASE are aar_run_zz9000.c, which this file includes.
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

#include <libraries/aar_serve.h>

#include "aar_run_policy.h"

struct Library *AARBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library *IconBase;

extern struct WBStartup *_WBenchMsg;

#define AAR_RUN_VERSION "0.1"
static const char version[] __attribute__((used)) =
    "$VER: aar-run " AAR_RUN_VERSION " (20.11.2026)\r\n";

/* The Workbench console of a start without a Shell, opened at the app's first
 * console use. */
#define AAR_RUN_WB_CONSOLE "CON:0/0/640/160/aar-run/CLOSE/WAIT"

/* What the launch gathered from the arguments and the tool types. */
static AARHandle *aar_handle;
static AARInfo platform_info;
static int claim_open;
static aar_serve *g_serve;
static char elf_full[512];
static char elf_path[256];
static char arg_store[AAR_RUN_ARGV_MAX][AAR_RUN_ARGV_STR_MAX];
static int app_argc;
static BPTR root_lock;
static int abs_paths;
static int joy_port0;
static int audio_ahi;
static uint32_t maxmem_mib;
static uint32_t hostmem_mib;
static uint32_t weight_value;
static int launcher_output_active;
static unsigned long dropped_logs;

static void cleanup(void);
static int tool_is_ahi(const char *value);

static int stack_ok(void)
{
    struct Task *task = FindTask(NULL);
    if (!task || (uintptr_t)task->tc_SPUpper <= (uintptr_t)task->tc_SPLower ||
        (uintptr_t)task->tc_SPUpper - (uintptr_t)task->tc_SPLower <
            AAR_SERVE_MIN_STACK)
        return 0;
    return 1;
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

static void usage(void)
{
    puts("aar-run <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [WEIGHT=<1-256>] [AUDIO=AHI] [args]\n"
         "aar-run STATUS\n"
         "aar-run RELEASE <handle>\n"
         "Run Stack 32768 in this Shell first.\n"
         "Workbench ToolTypes: APP, ROOT, ABS, MODE, ARGS, JOYPORT0, AUDIO, MAXMEM, HOSTMEM, WEIGHT.\n"
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

static void requester(const char *msg)
{
    if (_WBenchMsg && IntuitionBase) {
        struct EasyStruct es;
        memset(&es, 0, sizeof(es));
        es.es_StructSize = sizeof(es);
        es.es_Title = (UBYTE *)"aar-run";
        es.es_TextFormat = (UBYTE *)msg;
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(NULL, &es, NULL, NULL);
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

static int tool_true(const char *value)
{
    if (!value || !value[0])
        return 0;
    if (value[0] == '1' && value[1] == 0)
        return 1;
    if ((value[0] == 'y' || value[0] == 'Y') &&
        (value[1] == 0 || (value[1] == 'e' || value[1] == 'E')))
        return 1;
    if (value[0] == 't' || value[0] == 'T')
        return 1;
    return 0;
}

static int parse_mode(const char *text, ULONG *out)
{
    char *end = NULL;
    unsigned long value;
    if (!text || !text[0])
        return 0;
    if (text[0] == '$')
        value = strtoul(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        value = strtoul(text, &end, 16);
    else
        value = strtoul(text, &end, 10);
    if (!end || *end)
        return 0;
    *out = (ULONG)value;
    return 1;
}

static int add_arg(const char *text)
{
    size_t n;
    if (!text)
        return 1;
    n = strlen(text);
    if (!aar_run_arg_length_allowed((uint32_t)n) ||
        app_argc >= (int)AAR_RUN_ARGV_MAX - 1) {
        tell("Too many arguments, or an argument is longer than 127 bytes.");
        return 0;
    }
    memcpy(arg_store[app_argc], text, n + 1);
    app_argc++;
    return 1;
}

static int add_args_split(const char *text)
{
    char buf[256];
    char *p;
    if (!text || !text[0])
        return 1;
    if (strlen(text) >= sizeof(buf)) {
        tell("ARGS is longer than 255 bytes.");
        return 0;
    }
    memcpy(buf, text, strlen(text) + 1);
    p = buf;
    while (*p) {
        char *start;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = 0;
        if (!add_arg(start))
            return 0;
    }
    return 1;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
static char *tool_value(struct DiskObject *icon, const char *name)
{
    return (char *)FindToolType((STRPTR *)icon->do_ToolTypes, (STRPTR)name);
}
#pragma GCC diagnostic pop

static int read_tooltypes(struct DiskObject *icon, char *app_out, size_t app_n,
                          char *root_out, size_t root_n, ULONG *mode,
                          int *have_mode, uint32_t *maxmem,
                          uint32_t *hostmem, uint32_t *weight)
{
    char *value;
    if (!icon || !icon->do_ToolTypes)
        return 1;
    value = tool_value(icon, "APP");
    if (value && value[0] && app_out && strlen(value) < app_n)
        memcpy(app_out, value, strlen(value) + 1);
    value = tool_value(icon, "ROOT");
    if (value && value[0] && root_out && strlen(value) < root_n)
        memcpy(root_out, value, strlen(value) + 1);
    value = tool_value(icon, "ABS");
    if (value && (value[0] == 0 || tool_true(value)))
        abs_paths = 1;
    value = tool_value(icon, "MODE");
    if (value && value[0] && mode && parse_mode(value, mode))
        *have_mode = 1;
    value = tool_value(icon, "JOYPORT0");
    if (value && (value[0] == 0 || tool_true(value)))
        joy_port0 = 1;
    value = tool_value(icon, "AUDIO");
    if (tool_is_ahi(value))
        audio_ahi = 1;
    value = tool_value(icon, "MAXMEM");
    if (value && !aar_run_parse_maxmem(value, maxmem))
        return 0;
    value = tool_value(icon, "HOSTMEM");
    if (value && !aar_run_parse_hostmem(value, hostmem))
        return 0;
    value = tool_value(icon, "WEIGHT");
    if (value && !aar_run_parse_weight(value, weight))
        return 0;
    value = tool_value(icon, "ARGS");
    return !value || add_args_split(value);
}

static int tool_is_ahi(const char *value)
{
    return value && (tolower((unsigned char)value[0]) == 'a') &&
           value[1] && (tolower((unsigned char)value[1]) == 'h') &&
           value[2] && (tolower((unsigned char)value[2]) == 'i') &&
           value[3] == '\0';
}

static int load_program_icon(char *app_out, size_t app_n, char *root_out,
                             size_t root_n, ULONG *mode, int *have_mode,
                             uint32_t *maxmem, uint32_t *hostmem,
                             uint32_t *weight)
{
    struct DiskObject *icon;
    BPTR old = 0;
    int have_old = 0;
    if (!IconBase)
        return 1;
    if (_WBenchMsg && _WBenchMsg->sm_NumArgs >= 1) {
        old = CurrentDir(_WBenchMsg->sm_ArgList[0].wa_Lock);
        have_old = 1;
        icon = GetDiskObject(_WBenchMsg->sm_ArgList[0].wa_Name);
    } else {
        icon = GetDiskObject((STRPTR)"PROGDIR:aar-run");
    }
    if (icon) {
        int ok = read_tooltypes(icon, app_out, app_n, root_out, root_n, mode,
                                have_mode, maxmem, hostmem, weight);
        FreeDiskObject(icon);
        if (!ok) {
            if (have_old)
                CurrentDir(old);
            return 0;
        }
    }
    if (have_old)
        CurrentDir(old);
    return 1;
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

/* argv[0] is the app's file name, as C programs expect; the Shell or ARGS
 * tokens follow it. */
static int start_app(void)
{
    const char *argv_list[AAR_RUN_ARGV_MAX];
    char line[160];
    int i;
    LONG rc;

    argv_list[0] = (const char *)FilePart((STRPTR)elf_path);
    for (i = 0; i < app_argc; i++)
        argv_list[i + 1] = arg_store[i];
    rc = AARStart(aar_handle, argv_list, (ULONG)app_argc + 1u);
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
    snprintf(line, sizeof(line), "  pc=%08lx sp=%08lx lr=%08lx",
             (unsigned long)pc, (unsigned long)sp, (unsigned long)lr);
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
    else if (aar_run_claim_loss_text(0, st->end_reason)) {
        snprintf(line, sizeof(line), "aar-run: claim lost (%s).",
                 aar_run_claim_loss_text(0, st->end_reason));
        say(line);
    } else {
        snprintf(line, sizeof(line), "aar-run: exit %ld (%s)",
                 (long)st->exit_code, state_name(st->state));
        say(line);
    }
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
    if (root_lock) {
        UnLock(root_lock);
        root_lock = 0;
    }
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
        if (dropped_logs)
            printf("aar-run: dropped %lu diagnostic log records.\n",
                   dropped_logs);
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
    struct TagItem tags[AAR_RUN_TAG_MAX];
    char line[160];
    LONG rc;

    (void)aar_run_build_tags(tags, maxmem_mib, hostmem_mib, weight_value,
                             audio_ahi);
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

    aar_run_load_failure_line(line, sizeof(line), rc,
                              rc == AAR_E_NORESOURCE ? claim_reason() :
                                                       AAR_REASON_NONE,
                              ctrl_c_set(), platform_info.max_image_bytes);
    puts(line);
}

#include "aar_run_zz9000.c"

int main(int argc, char **argv)
{
    char app_from_icon[256];
    char root_text[256];
    aar_serve_config cfg;
    ULONG forced_mode = 0;
    int have_mode = 0;
    ULONG reason = 0;
    LONG status;
    enum aar_run_query_verdict verdict;
    int rc = 20;
    int show_status = 0;
    uint32_t release_handle = 0;

    app_from_icon[0] = 0;
    root_text[0] = 0;
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
    if (!load_program_icon(app_from_icon, sizeof(app_from_icon),
                           root_text, sizeof(root_text),
                           &forced_mode, &have_mode, &maxmem_mib,
                           &hostmem_mib, &weight_value)) {
        usage();
        cleanup();
        return 10;
    }
    if (_WBenchMsg) {
        if (_WBenchMsg->sm_NumArgs >= 2) {
            char name[256];
            if (NameFromLock(_WBenchMsg->sm_ArgList[1].wa_Lock, (STRPTR)name,
                             (LONG)sizeof(name)) > 0) {
                size_t n = strlen(name);
                size_t m = strlen((char *)_WBenchMsg->sm_ArgList[1].wa_Name);
                if (n + 1 + m < sizeof(elf_path)) {
                    memcpy(elf_path, name, n);
                    if (n && elf_path[n - 1] != ':')
                        elf_path[n++] = '/';
                    memcpy(elf_path + n, _WBenchMsg->sm_ArgList[1].wa_Name, m + 1);
                }
            }
        } else if (app_from_icon[0]) {
            memcpy(elf_path, app_from_icon, strlen(app_from_icon) + 1);
        }
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
                if (strncmp(argv[i], "AUDIO=", 6) == 0) {
                    if (!tool_is_ahi(argv[i] + 6)) {
                        tell("AUDIO must be AHI.");
                        cleanup();
                        return 10;
                    }
                    audio_ahi = 1;
                    continue;
                }
                {
                    int w = aar_run_weight_arg(argv[i], &weight_value);

                    if (w == AAR_RUN_ARG_BAD) {
                        usage();
                        cleanup();
                        return 10;
                    }
                    if (w == AAR_RUN_ARG_TAKEN)
                        continue;
                }
                if (strncmp(argv[i], "HOSTMEM=", 8) == 0) {
                    if (!aar_run_parse_hostmem(argv[i] + 8, &hostmem_mib)) {
                        usage();
                        cleanup();
                        return 10;
                    }
                    continue;
                }
                if (strncmp(argv[i], "MAXMEM=", 7) == 0) {
                    if (!aar_run_parse_maxmem(argv[i] + 7, &maxmem_mib)) {
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
    if (!show_status && !release_handle && !open_root(root_text)) {
        cleanup();
        return 20;
    }
    AARBase = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, AAR_LIB_VERSION);
    if (!AARBase) {
        tell("aar.library not found. Install Libs:aar.library from the "
             "ZZ9000 package.");
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
    }
    verdict = aar_run_query_check(&platform_info);
    if (verdict != AAR_RUN_QUERY_OK) {
        tell(aar_run_query_verdict_text(verdict));
        cleanup();
        return 20;
    }
    if (!zz9000_init()) {
        tell("aar.library has no ZZ9000 client extension.");
        cleanup();
        return 20;
    }
    if (show_status) {
        rc = zz9000_print_status();
        cleanup();
        return rc;
    }
    if (release_handle) {
        rc = zz9000_release_claim(release_handle);
        cleanup();
        return rc;
    }
    status = AAROpen(&aar_handle, NULL, &reason);
    if (status != AAR_OK) {
        char line[200];

        aar_handle = NULL;
        aar_run_claim_failure_line(line, sizeof(line), status, reason);
        puts(line);
        cleanup();
        return 20;
    }
    claim_open = 1;
    aar_serve_config_default(&cfg);
    cfg.flags = abs_paths ? AAR_SERVEF_ABS_PATHS : 0UL;
    cfg.root = root_lock;
    cfg.screen_mode = have_mode ? forced_mode : 0;
    cfg.screen_title = (CONST_STRPTR)"aar-run";
    cfg.console_spec = _WBenchMsg ? (CONST_STRPTR)AAR_RUN_WB_CONSOLE : NULL;
    cfg.audio_route = audio_ahi ? AAR_AUDIO_ROUTE_AHI : AAR_AUDIO_ROUTE_PLATFORM;
    cfg.joy_ports = joy_port0 ? 3UL : 2UL;
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
        cleanup();
        return 20;
    }
    /* The options go to the library before the load. */
    if (!apply_options()) {
        rc = aar_run_load_failure_exit(ctrl_c_set(), user_stop_seen());
        cleanup();
        return rc;
    }
    if (claim_gone()) {
        cleanup();
        return 20;
    }
    {
        BPTR base_dir = 0;

        if (!strchr(elf_path, ':')) {
            base_dir = CurrentDir(0);
            CurrentDir(base_dir);
        }
        status = aar_serve_locate(g_serve, base_dir, elf_path, elf_full,
                                  sizeof(elf_full));
        if (status == AAR_E_NOENT) {
            printf("aar-run: cannot open %s\n", elf_path);
            rc = aar_run_load_failure_exit(ctrl_c_set(), user_stop_seen());
            cleanup();
            return rc;
        }
        if (status != AAR_OK) {
            printf("aar-run: cannot resolve the path of %s\n", elf_path);
            cleanup();
            return 20;
        }
    }
    if (claim_gone()) {
        cleanup();
        return 20;
    }
    printf("loading %s\n", elf_full);
    status = AARLoadFile(aar_handle, elf_full);
    if (status != AAR_OK) {
        load_failed(status);
        rc = aar_run_load_failure_exit(ctrl_c_set(), user_stop_seen());
        cleanup();
        return rc;
    }
    if (!start_app()) {
        (void)AARStop(aar_handle);
        cleanup();
        return 20;
    }
    fflush(stdout);
    launcher_output_active = 1;
    rc = finish_run(aar_serve_run(g_serve));
    cleanup();
    return rc;
}
