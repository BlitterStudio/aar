/*
 * Decisions the reference launcher aar-run makes from its own state: the
 * arguments and tool types, the library options (tag list), the texts for a refused
 * claim, load or start, the checks of what AARQuery said, the exit code, and the
 * ZZ9000 list helpers (STATUS and RELEASE). No AmigaOS calls, so a host test can
 * exercise them without DOS or Exec. What the serving loop decides is in
 * aar_serve_core.h.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_RUN_POLICY_H
#define AAR_RUN_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libraries/aar.h>
#include <libraries/aar_zz9000.h>

#define AAR_RUN_MIB_BYTES (1024u * 1024u)

/* The most strings aar-run hands AARStart (argv[0] and 15 arguments) and the
 * most bytes of one string with its NUL. AARInfo says what the platform
 * takes; aar_run_args_fit() refuses a platform that takes less. */
#define AAR_RUN_ARGV_MAX 16u
#define AAR_RUN_ARGV_STR_MAX 128u

/* MAXMEM and HOSTMEM are decimal MiB, at least 1. */
static inline int aar_run_parse_maxmem(const char *text, uint32_t *mib_out)
{
    uint32_t value = 0;
    const unsigned char *p = (const unsigned char *)text;

    if (!p || !*p || !mib_out)
        return 0;
    for (; *p; ++p) {
        uint32_t digit;

        if (*p < '0' || *p > '9')
            return 0;
        digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10u)
            return 0;
        value = value * 10u + digit;
    }
    if (value == 0)
        return 0;
    *mib_out = value;
    return 1;
}

/* HOSTMEM is the shared (host-visible) memory limit in MiB. It goes to the
 * library as AAR_TAG_SHARED_MIB before the load; the platform checks it at
 * load begin. The tag takes 0..4095; a launch without HOSTMEM leaves it out
 * and keeps the platform default. */
#define AAR_RUN_HOSTMEM_MAX_MIB 4095u

static inline int aar_run_parse_hostmem(const char *text, uint32_t *mib_out)
{
    uint32_t value;

    if (!aar_run_parse_maxmem(text, &value) || value > AAR_RUN_HOSTMEM_MAX_MIB ||
        !mib_out)
        return 0;
    *mib_out = value;
    return 1;
}

/* WEIGHT is the app's CPU share. It goes to the library as AAR_TAG_WEIGHT
 * before the load; the platform keeps it for the claim. The tag takes 1 up
 * to AARInfo.weight_max (256 on the ZZ9000); a launch without WEIGHT leaves
 * it out, which the card reads as weight 1. aar-run refuses a value above the
 * bound below before it claims anything. */
#define AAR_RUN_WEIGHT_MAX 256u

static inline int aar_run_parse_weight(const char *text, uint32_t *weight_out)
{
    uint32_t value;

    if (!aar_run_parse_maxmem(text, &value) || value > AAR_RUN_WEIGHT_MAX ||
        !weight_out)
        return 0;
    *weight_out = value;
    return 1;
}

/* Result of looking at one command-line word for WEIGHT=<n>. */
#define AAR_RUN_ARG_OTHER 0 /* not WEIGHT=: the caller keeps looking */
#define AAR_RUN_ARG_TAKEN 1 /* WEIGHT=<n>: stored, not for the app */
#define AAR_RUN_ARG_BAD (-1) /* WEIGHT= with a value outside 1..256 */

/* The key is matched without regard to case, like a ToolType name. */
static inline int aar_run_weight_arg(const char *arg, uint32_t *weight_out)
{
    static const char key[] = "WEIGHT=";
    size_t i;

    if (!arg)
        return AAR_RUN_ARG_OTHER;
    for (i = 0; i + 1 < sizeof(key); i++) {
        char c = arg[i];

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c != key[i])
            return AAR_RUN_ARG_OTHER;
    }
    return aar_run_parse_weight(arg + i, weight_out) ? AAR_RUN_ARG_TAKEN
                                                   : AAR_RUN_ARG_BAD;
}

/* What aar-run can serve, for AAR_TAG_CLIENT_CAPS: CLUT8 screens, pan-flip
 * acknowledgement by completion flag and platform-owned flips. The lease
 * is not here: the library renews it, not the launcher. */
static inline uint32_t aar_run_client_ccaps(void)
{
    return AAR_CCAP_DISPLAY_CLUT8 | AAR_CCAP_DISPLAY_PAN_ACK |
           AAR_CCAP_DISPLAY_LATEST;
}

/* The options aar-run gives AARSetTagList before the load: a tag for each
 * limit the launch set (a zero keeps the platform default and is left
 * out), the serving capabilities, the extended lease, and the AHI audio
 * route when AUDIO=AHI was asked. Ended by TAG_DONE. Returns the number of
 * tags before TAG_DONE. The launcher's own policy (ROOT, ABS, the console,
 * the screen mode) is not a library tag. */
#define AAR_RUN_TAG_MAX 8u

static inline unsigned aar_run_build_tags(struct TagItem *tags,
                                          uint32_t maxmem_mib,
                                          uint32_t hostmem_mib,
                                          uint32_t weight, int audio_ahi)
{
    unsigned n = 0;

    if (maxmem_mib) {
        tags[n].ti_Tag = AAR_TAG_PRIVATE_MIB;
        tags[n++].ti_Data = maxmem_mib;
    }
    if (hostmem_mib) {
        tags[n].ti_Tag = AAR_TAG_SHARED_MIB;
        tags[n++].ti_Data = hostmem_mib;
    }
    if (weight) {
        tags[n].ti_Tag = AAR_TAG_WEIGHT;
        tags[n++].ti_Data = weight;
    }
    tags[n].ti_Tag = AAR_TAG_CLIENT_CAPS;
    tags[n++].ti_Data = aar_run_client_ccaps();
    tags[n].ti_Tag = AAR_TAG_LEASE_EXTENDED;
    tags[n++].ti_Data = 1u;
    if (audio_ahi) {
        tags[n].ti_Tag = AAR_TAG_AUDIO_ROUTE;
        tags[n++].ti_Data = AAR_AUDIO_ROUTE_AHI;
    }
    tags[n].ti_Tag = TAG_DONE;
    tags[n].ti_Data = 0u;
    return n;
}

/* A claim handle named on the command line: decimal, $hex or 0x hex, never
 * zero. */
static inline int aar_run_parse_handle(const char *text, uint32_t *handle_out)
{
    const unsigned char *p = (const unsigned char *)text;
    uint32_t base = 10u;
    uint32_t value = 0;
    int digits = 0;

    if (!p || !handle_out)
        return 0;
    if (p[0] == '$') {
        base = 16u;
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16u;
        p += 2;
    }
    for (; *p; ++p) {
        uint32_t digit;

        if (*p >= '0' && *p <= '9')
            digit = (uint32_t)(*p - '0');
        else if (base == 16u && *p >= 'a' && *p <= 'f')
            digit = (uint32_t)(*p - 'a') + 10u;
        else if (base == 16u && *p >= 'A' && *p <= 'F')
            digit = (uint32_t)(*p - 'A') + 10u;
        else
            return 0;
        if (value > (UINT32_MAX - digit) / base)
            return 0;
        value = value * base + digit;
        digits++;
    }
    if (!digits || value == 0)
        return 0;
    *handle_out = value;
    return 1;
}

/* A refused claim, load or start carries an AAR_REASON_* code: AAROpen gives
 * it in *reason, a load or start in AARStatusInfo.reason. */
static inline const char *aar_run_refusal_text(uint32_t reason)
{
    switch (reason) {
    case AAR_REASON_MEMORY:
        return "not enough card memory for the app record and stack";
    case AAR_REASON_TABLES:
        return "no address-space translation tables left";
    case AAR_REASON_HOSTHEAP:
        return "not enough shared card memory for the queues and buffers";
    case AAR_REASON_LIMIT:
        return "a platform limit was reached (HOSTMEM or MAXMEM too large?)";
    default:
        return "refused by the platform";
    }
}

/* The name of an AAR_E_* code as aar-run prints it. */
static inline const char *aar_run_error_name(int status)
{
    switch (status) {
    case AAR_OK: return "ok";
    case AAR_E_IO: return "io error";
    case AAR_E_INVAL: return "bad request";
    case AAR_E_NOMEM: return "no memory";
    case AAR_E_BUSY: return "busy";
    case AAR_E_NOENT: return "not found";
    case AAR_E_ACCES: return "refused";
    case AAR_E_NOSYS: return "not implemented";
    case AAR_E_TIMEDOUT: return "timeout";
    case AAR_E_PROFILE: return "profile not supported";
    case AAR_E_NOGROUP: return "service group not available";
    case AAR_E_NORESOURCE: return "no resources";
    case AAR_E_STALE: return "claim revoked";
    case AAR_E_OWNER: return "handle belongs to another task";
    default: return "error";
    }
}

/* One launcher line for a refused or failed library call. `what` names the
 * step ("claim", "load", "start", "options"). A refusal for lack of a
 * resource says why, from the reason the library kept. */
static inline void aar_run_failure_line(char *buf, size_t size, const char *what,
                                        int rc, uint32_t reason)
{
    if (rc == AAR_E_NORESOURCE)
        snprintf(buf, size, "aar-run: %s refused: %s", what,
                 aar_run_refusal_text(reason));
    else
        snprintf(buf, size, "aar-run: %s failed (%s)", what,
                 aar_run_error_name(rc));
}

/* AAROpen. With no platform, or without mmu.library, the library answers
 * AAR_E_NOSYS. */
static inline void aar_run_claim_failure_line(char *buf, size_t size, int rc,
                                              uint32_t reason)
{
    if (rc == AAR_E_NOSYS)
        snprintf(buf, size, "aar-run: claim failed: the ZZ9000 board, "
                 "zz9k.library 36 or mmu.library 41 is missing");
    else
        aar_run_failure_line(buf, size, "claim", rc, reason);
}

/* AARSetTagList: a value out of range is the launch's MAXMEM, HOSTMEM or
 * WEIGHT; BUSY is a program error (the options came after the load). */
static inline void aar_run_tags_failure_line(char *buf, size_t size, int rc)
{
    if (rc == AAR_E_INVAL)
        snprintf(buf, size,
                 "aar-run: MAXMEM, HOSTMEM or WEIGHT is out of range (%s)",
                 aar_run_error_name(rc));
    else if (rc == AAR_E_NOSYS)
        snprintf(buf, size,
                 "aar-run: the platform does not take WEIGHT or the AUDIO "
                 "route (%s)", aar_run_error_name(rc));
    else
        aar_run_failure_line(buf, size, "options", rc, AAR_REASON_NONE);
}

/* AARLoadFile. A break (Ctrl-C) comes back as AAR_E_IO with the signal
 * left set. The library checks the image size against its limit and the
 * platform's loader checks the image, so a bad request is either. */
static inline void aar_run_load_failure_line(char *buf, size_t size, int rc,
                                             uint32_t reason, int broke,
                                             uint32_t max_image_bytes)
{
    if (rc == AAR_E_IO && broke)
        snprintf(buf, size, "aar-run: stopped during load");
    else if (rc == AAR_E_NOENT)
        snprintf(buf, size, "aar-run: cannot open the ELF (%s)",
                 aar_run_error_name(rc));
    else if (rc == AAR_E_INVAL)
        snprintf(buf, size,
                 "aar-run: the ELF is empty, larger than the %lu MiB image "
                 "limit, or refused by the loader",
                 (unsigned long)(max_image_bytes / AAR_RUN_MIB_BYTES));
    else
        aar_run_failure_line(buf, size, "load", rc, reason);
}

/* What AARQuery said about the platform, against what aar-run needs. */
enum aar_run_query_verdict {
    AAR_RUN_QUERY_OK = 0,
    AAR_RUN_QUERY_NOT_ZZ9000, /* another platform */
    AAR_RUN_QUERY_NO_Z3,      /* a ZZ9000 that is not a Zorro III board */
    AAR_RUN_QUERY_API,        /* another API major than aar-run was built for */
    AAR_RUN_QUERY_ARGS        /* takes fewer or shorter arguments than aar-run */
};

/* The strings of argv aar-run hands AARStart must fit what the platform
 * takes. */
static inline int aar_run_args_fit(const AARInfo *info)
{
    return info->max_args >= AAR_RUN_ARGV_MAX &&
           info->max_arg_bytes >= AAR_RUN_ARGV_STR_MAX;
}

static inline enum aar_run_query_verdict aar_run_query_check(const AARInfo *info)
{
    if (info->platform_id != AAR_PLATFORM_ZZ9000)
        return AAR_RUN_QUERY_NOT_ZZ9000;
    if (!(info->platform[AAR_ZZ9000_INFO_CAPS] & AAR_ZZ9000_CAP_Z3))
        return AAR_RUN_QUERY_NO_Z3;
    if (info->api_major != AAR_API_MAJOR)
        return AAR_RUN_QUERY_API;
    if (!aar_run_args_fit(info))
        return AAR_RUN_QUERY_ARGS;
    return AAR_RUN_QUERY_OK;
}

static inline const char *aar_run_query_verdict_text(enum aar_run_query_verdict v)
{
    switch (v) {
    case AAR_RUN_QUERY_OK:
        return "";
    case AAR_RUN_QUERY_ARGS:
        return "aar-run: the platform takes fewer or shorter app arguments "
               "than aar-run passes.";
    default:
        return "aar-run requires a Zorro III board and app-runtime firmware.";
    }
}

/* AARQuery itself failed: there is no platform (no board, a zz9k.library
 * older than 36, or firmware without the app runtime). */
static inline void aar_run_query_failure_line(char *buf, size_t size, int rc)
{
    if (rc == AAR_E_NOSYS)
        snprintf(buf, size, "ZZ9000 board, zz9k.library 36 or the app-runtime "
                 "firmware not found.");
    else
        snprintf(buf, size, "aar-run: AARQuery failed (%s). Zorro III app "
                 "firmware is required.", aar_run_error_name(rc));
}

static inline const char *aar_run_slot_state_name(uint32_t state)
{
    switch (state) {
    case AAR_STATE_IDLE: return "idle";
    case AAR_STATE_CLAIMED: return "claimed";
    case AAR_STATE_LOADING: return "loading";
    case AAR_STATE_STARTING: return "starting";
    case AAR_STATE_RUNNING: return "running";
    case AAR_STATE_STOPPING: return "stopping";
    case AAR_STATE_FAULTED: return "faulted";
    default: return "unknown";
    }
}

static inline const char *aar_run_end_reason_name(uint32_t reason)
{
    switch (reason) {
    case AAR_END_NONE: return "none";
    case AAR_END_EXIT: return "exit";
    case AAR_END_FORCED: return "forced stop";
    case AAR_END_USER_FAULT: return "user fault";
    case AAR_END_STALL: return "stall watchdog";
    case AAR_END_STUCK_CALL: return "stuck-call watchdog";
    case AAR_END_LEASE: return "lease loss";
    case AAR_END_PLATFORM_RESET: return "Amiga reset";
    case AAR_END_RUNTIME_RESTART: return "core restart";
    case AAR_END_QUARANTINED: return "quarantined";
    default: return "unknown";
    }
}

/* STALE alone does not identify a lease expiry. Only name a cause when the
 * final control block supplied it. */
static inline const char *aar_run_claim_loss_text(int stale, uint32_t end_reason)
{
    if (stale)
        return "stale handle; final status unavailable";
    if (end_reason == AAR_END_LEASE)
        return "lease expired";
    if (end_reason == AAR_END_QUARANTINED)
        return "quarantined";
    return NULL;
}

static inline const char *aar_run_fault_class_name(uint32_t fault)
{
    switch (fault) {
    case AAR_FAULT_NONE: return "none";
    case AAR_FAULT_DATA: return "data abort";
    case AAR_FAULT_FETCH: return "prefetch abort";
    case AAR_FAULT_ILLEGAL: return "undefined instruction";
    case AAR_FAULT_ALIGN: return "alignment fault";
    case AAR_FAULT_OTHER: return "fault";
    default: return "unknown";
    }
}

/* What the claim list reports an app is blocked on (AARZZ9000List.block_reason).
 * The library passes the card's code through and the public header names
 * none of them, so the values are aar-run's reading of the ZZ9000 list. The
 * firmware reports both display-open and flip waits as vsync. */
#define AAR_RUN_BLOCK_NONE    0u
#define AAR_RUN_BLOCK_FILE    1u
#define AAR_RUN_BLOCK_CONSOLE 2u
#define AAR_RUN_BLOCK_EVENT   3u
#define AAR_RUN_BLOCK_SLEEP   4u
#define AAR_RUN_BLOCK_VSYNC   5u
#define AAR_RUN_BLOCK_AUDIO   6u

static inline const char *aar_run_block_reason_name(uint32_t reason)
{
    switch (reason) {
    case AAR_RUN_BLOCK_NONE: return "none";
    case AAR_RUN_BLOCK_FILE: return "file";
    case AAR_RUN_BLOCK_CONSOLE: return "console";
    case AAR_RUN_BLOCK_EVENT: return "event";
    case AAR_RUN_BLOCK_SLEEP: return "sleep";
    case AAR_RUN_BLOCK_VSYNC: return "vsync";
    case AAR_RUN_BLOCK_AUDIO: return "audio";
    default: return "unknown";
    }
}

/* README returns 20 for a launcher error even when the app exits 0.
 * An AHI open failure on the AUDIO=AHI route is a launcher error, as are
 * a display failure, a console that cannot be opened or written, and a
 * request queue found corrupt (the app was stopped for it, which is not a
 * stop the user asked for). */
static inline int aar_run_process_exit(int app_exit, int display_failed,
                                       int console_failed, int ahi_open_failed,
                                       int queue_corrupt)
{
    if (display_failed || console_failed || ahi_open_failed || queue_corrupt)
        return 20;
    return app_exit;
}

/* The process exit code for an app that ended with end_reason and exit_code
 * (AAR_CB_END_REASON, AAR_CB_EXIT_CODE). Every platform-ended case is a
 * launcher error, a forced end is one unless the user asked for it, and an
 * exit code outside 0..255 is the app's mistake unless the user stopped it. */
static inline int aar_run_app_exit_code(uint32_t end_reason, int32_t exit_code,
                                        int user_stop)
{
    switch (end_reason) {
    case AAR_END_USER_FAULT:
    case AAR_END_STALL:
    case AAR_END_STUCK_CALL:
    case AAR_END_LEASE:
    case AAR_END_PLATFORM_RESET:
    case AAR_END_RUNTIME_RESTART:
    case AAR_END_QUARANTINED:
        return 20;
    case AAR_END_FORCED:
        if (!user_stop)
            return 20;
        break;
    default:
        break;
    }
    if (exit_code < 0 || exit_code > 255)
        return user_stop ? 0 : 20;
    return (int)exit_code;
}

/* argv accepts an empty NUL-terminated string; only oversized strings are
 * invalid. */
static inline int aar_run_arg_length_allowed(uint32_t length)
{
    return length <= 127u;
}

/* Exit code after a failed ELF load: 5 (RETURN_WARN) when the user stopped
 * it with Ctrl-C, 20 for any other load failure. */
static inline int aar_run_load_failure_exit(int ctrl_c_pending, int user_stop)
{
    return (ctrl_c_pending != 0 || user_stop != 0) ? 5 : 20;
}

/* AmigaDOS keywords are case-insensitive: "aar-run status" lists claims
 * instead of trying to load a file called "status". */
static inline int aar_run_is_status_keyword(const char *arg)
{
    static const char keyword[] = "STATUS";
    unsigned i;

    if (!arg)
        return 0;
    for (i = 0; keyword[i]; i++) {
        char c = arg[i];

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c != keyword[i])
            return 0;
    }
    return arg[i] == '\0';
}

/* RELEASE <handle> ends a claim an administrator names. It is for claims that
 * are finished: an app that ended or faulted, a claim the card revoked, or one
 * whose launcher lost its lease. A claim whose app still runs, or is being
 * claimed or loaded by a live launcher, is refused. */
static inline int aar_run_release_allowed(uint32_t state, uint32_t end_reason)
{
    if (end_reason == AAR_END_QUARANTINED || end_reason == AAR_END_LEASE)
        return 1;
    return state == AAR_STATE_IDLE || state == AAR_STATE_FAULTED;
}

#endif /* AAR_RUN_POLICY_H */
