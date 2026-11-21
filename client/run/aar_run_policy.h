/*
 * Decisions the reference launcher aar-run makes from its own state: the
 * arguments and tool types, the limits it takes from AARQuery, the check of the
 * image against the platform before a claim is taken, the library options (tag
 * list), the texts for a refused claim, load or start, the check of what
 * AARQuery said, the exit code, and the helpers of STATUS and RELEASE. No
 * AmigaOS calls, so a host test can exercise them without DOS or Exec. What the
 * serving loop decides is in aar_serve_core.h.
 *
 * aar-run runs on any platform behind aar.library. What the platform takes is
 * read from AARInfo (aar_run_limits_from_info), never assumed; the only ZZ9000
 * words are the ones STATUS and RELEASE need and the note of
 * aar_run_zz9000_lacks_z3.
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
#include <libraries/aar_serve.h>
#include <libraries/aar_zz9000.h>

#define AAR_RUN_VERSION "0.1"

#define AAR_RUN_MIB_BYTES (1024u * 1024u)

/* The heartbeat of aar-run's loop, in microseconds. It keeps the latency the
 * launcher always had; POLL=<us> changes it for a measurement. */
#define AAR_RUN_DEFAULT_POLL_US 1000UL

/* The timer's microsecond field stays below one second. */
#define AAR_RUN_POLL_US_LIMIT 1000000UL

/* The decimal digits of text, nothing else, within 32 bits. */
static inline int aar_run_parse_uint(const char *text, uint32_t *out)
{
    uint32_t value = 0;
    const unsigned char *p = (const unsigned char *)text;

    if (!p || !*p || !out)
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
    *out = value;
    return 1;
}

/* MAXMEM, HOSTMEM and WEIGHT are decimal numbers, at least 1. How large each
 * may be is the platform's say (AARInfo), checked by aar_run_limits_check once
 * AARQuery has answered. */
static inline int aar_run_parse_positive(const char *text, uint32_t *out)
{
    uint32_t value;

    if (!aar_run_parse_uint(text, &value) || value == 0 || !out)
        return 0;
    *out = value;
    return 1;
}

/* ---- Limits: what the platform takes -------------------------------------- */

/* The bound AAR_TAG_SHARED_MIB has on every platform. */
#define AAR_RUN_TAG_MIB_MAX 4095u

/* The limits of the platform that matter to a launch, read from AARInfo. */
struct aar_run_limits {
    uint32_t max_args;        /* strings AARStart takes, argv[0] included; 0: no limit */
    uint32_t max_arg_bytes;   /* bytes of one string, NUL included; 0: no limit */
    uint32_t weight_max;      /* largest WEIGHT; 0: the platform does not weight */
    uint32_t hostmem_max_mib; /* largest HOSTMEM, the rule of AAR_TAG_SHARED_MIB */
    uint32_t max_image_bytes; /* largest image AARLoadFile takes */
};

static inline struct aar_run_limits aar_run_limits_from_info(const AARInfo *info)
{
    struct aar_run_limits lim;

    lim.max_args = info->max_args;
    lim.max_arg_bytes = info->max_arg_bytes;
    lim.weight_max = info->weight_max;
    lim.hostmem_max_mib = (info->shared_max_mib != 0u &&
                           info->shared_max_mib < AAR_RUN_TAG_MIB_MAX)
                              ? info->shared_max_mib : AAR_RUN_TAG_MIB_MAX;
    lim.max_image_bytes = info->max_image_bytes;
    return lim;
}

enum aar_run_limit_verdict {
    AAR_RUN_LIMIT_OK = 0,
    AAR_RUN_LIMIT_ARGS,       /* more arguments than AARStart takes */
    AAR_RUN_LIMIT_ARG_BYTES,  /* an argument longer than the platform takes */
    AAR_RUN_LIMIT_WEIGHT,     /* WEIGHT above weight_max */
    AAR_RUN_LIMIT_NO_WEIGHT,  /* WEIGHT on a platform that does not weight */
    AAR_RUN_LIMIT_HOSTMEM     /* HOSTMEM above the platform's largest */
};

/* The launch against the limits, before anything is claimed: app_argc
 * arguments for the app (argv[0] comes on top of them), the longest of them in
 * bytes without its NUL, and the WEIGHT and HOSTMEM given (0: not given). The
 * library refuses the same values later; this says so with the launcher's own
 * words and without a claim. */
static inline enum aar_run_limit_verdict
aar_run_limits_check(const struct aar_run_limits *lim, uint32_t app_argc,
                     uint32_t longest_arg, uint32_t weight,
                     uint32_t hostmem_mib)
{
    if (lim->max_args != 0u && app_argc + 1u > lim->max_args)
        return AAR_RUN_LIMIT_ARGS;
    if (lim->max_arg_bytes != 0u && longest_arg + 1u > lim->max_arg_bytes)
        return AAR_RUN_LIMIT_ARG_BYTES;
    if (weight != 0u && lim->weight_max == 0u)
        return AAR_RUN_LIMIT_NO_WEIGHT;
    if (weight > lim->weight_max)
        return AAR_RUN_LIMIT_WEIGHT;
    if (hostmem_mib > lim->hostmem_max_mib)
        return AAR_RUN_LIMIT_HOSTMEM;
    return AAR_RUN_LIMIT_OK;
}

static inline void aar_run_limit_line(char *buf, size_t size,
                                      enum aar_run_limit_verdict v,
                                      const struct aar_run_limits *lim)
{
    switch (v) {
    case AAR_RUN_LIMIT_ARGS:
        if (lim->max_args <= 1u)
            snprintf(buf, size, "aar-run: this platform takes no arguments for "
                     "the app.");
        else
            snprintf(buf, size, "aar-run: this platform takes at most %lu "
                     "arguments for the app.", (unsigned long)(lim->max_args - 1u));
        break;
    case AAR_RUN_LIMIT_ARG_BYTES:
        snprintf(buf, size, "aar-run: an argument is longer than %lu bytes, "
                 "the most this platform takes.",
                 (unsigned long)(lim->max_arg_bytes - 1u));
        break;
    case AAR_RUN_LIMIT_WEIGHT:
        snprintf(buf, size, "aar-run: WEIGHT must be 1 to %lu on this platform.",
                 (unsigned long)lim->weight_max);
        break;
    case AAR_RUN_LIMIT_NO_WEIGHT:
        snprintf(buf, size, "aar-run: WEIGHT is not available: this platform "
                 "does not weight apps.");
        break;
    case AAR_RUN_LIMIT_HOSTMEM:
        snprintf(buf, size, "aar-run: HOSTMEM must be 1 to %lu MiB on this "
                 "platform.", (unsigned long)lim->hostmem_max_mib);
        break;
    default:
        if (size)
            buf[0] = '\0';
        break;
    }
}

/* ---- Words of the command line that aar-run takes itself ------------------- */

/* Result of looking at one command-line word for KEY=<value>. */
#define AAR_RUN_ARG_OTHER 0  /* not this key: the caller keeps looking */
#define AAR_RUN_ARG_TAKEN 1  /* KEY=<value>: stored, not for the app */
#define AAR_RUN_ARG_BAD (-1) /* the key with a value that does not parse */

static inline char aar_run_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* The text after `key` (which includes its '=') when arg starts with it, else
 * NULL. The key is matched without regard to case, like a ToolType name. */
static inline const char *aar_run_key_value(const char *arg, const char *key)
{
    size_t i;

    if (!arg)
        return NULL;
    for (i = 0; key[i]; i++)
        if (aar_run_upper(arg[i]) != key[i])
            return NULL;
    return arg + i;
}

/* WEIGHT is the app's CPU share. It goes to the library as AAR_TAG_WEIGHT
 * before the load; the platform keeps it for the claim. The tag takes 1 up
 * to AARInfo.weight_max; a launch without WEIGHT leaves it out, which the
 * platform reads as its default. aar-run refuses a value above the bound
 * (aar_run_limits_check) before it claims anything. */
static inline int aar_run_weight_arg(const char *arg, uint32_t *weight_out)
{
    const char *value = aar_run_key_value(arg, "WEIGHT=");

    if (!value)
        return AAR_RUN_ARG_OTHER;
    return aar_run_parse_positive(value, weight_out) ? AAR_RUN_ARG_TAKEN
                                                     : AAR_RUN_ARG_BAD;
}

/* POLL=<us>: the heartbeat of the loop in microseconds, 0 for the library
 * signal alone. A diagnostic argument (README.md, Diagnostics). */
static inline int aar_run_poll_arg(const char *arg, uint32_t *us_out)
{
    const char *value = aar_run_key_value(arg, "POLL=");
    uint32_t us;

    if (!value)
        return AAR_RUN_ARG_OTHER;
    if (!aar_run_parse_uint(value, &us) || us >= AAR_RUN_POLL_US_LIMIT)
        return AAR_RUN_ARG_BAD;
    *us_out = us;
    return AAR_RUN_ARG_TAKEN;
}

static inline int aar_run_streq_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (aar_run_upper(*a) != aar_run_upper(*b))
            return 0;
    return *a == *b;
}

/* DISPLAY=<BUFFERS|PAN3|PAN2|SINGLE>: the display strategy to start with
 * (AAR_SERVE_DISPLAY_*). A diagnostic argument. */
static inline int aar_run_display_arg(const char *arg, uint32_t *strategy_out)
{
    static const struct {
        const char *name;
        uint32_t strategy;
    } names[] = {
        {"BUFFERS", AAR_SERVE_DISPLAY_BUFFERS},
        {"PAN3", AAR_SERVE_DISPLAY_PAN3},
        {"PAN2", AAR_SERVE_DISPLAY_PAN2},
        {"SINGLE", AAR_SERVE_DISPLAY_SINGLE},
    };
    const char *value = aar_run_key_value(arg, "DISPLAY=");
    size_t i;

    if (!value)
        return AAR_RUN_ARG_OTHER;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (aar_run_streq_nocase(value, names[i].name)) {
            *strategy_out = names[i].strategy;
            return AAR_RUN_ARG_TAKEN;
        }
    }
    return AAR_RUN_ARG_BAD;
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

/* AAROpen. With no platform, or without a library the platform needs (the
 * ZZ9000 build needs mmu.library), the library answers AAR_E_NOSYS. */
static inline void aar_run_claim_failure_line(char *buf, size_t size, int rc,
                                              uint32_t reason)
{
    if (rc == AAR_E_NOSYS)
        snprintf(buf, size, "aar-run: claim failed: the platform cannot admit "
                 "an app (a board or a library it needs, such as "
                 "mmu.library, is missing)");
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

/* What AARQuery said about the platform, against what aar-run needs. Any
 * platform is fine; what it takes is read as limits (aar_run_limits_from_info)
 * and the image is checked against it by aar_run_probe_check. */
enum aar_run_query_verdict {
    AAR_RUN_QUERY_OK = 0,
    AAR_RUN_QUERY_API         /* another API major than aar-run was built for */
};

static inline enum aar_run_query_verdict aar_run_query_check(const AARInfo *info)
{
    if (info->api_major != AAR_API_MAJOR)
        return AAR_RUN_QUERY_API;
    return AAR_RUN_QUERY_OK;
}

static inline const char *aar_run_query_verdict_text(enum aar_run_query_verdict v)
{
    if (v == AAR_RUN_QUERY_OK)
        return "";
    return "aar-run: the platform's AAR API major differs from the one "
           "aar-run was built for.";
}

/* The one ZZ9000 specific rule aar-run keeps: a ZZ9000 that is not a Zorro III
 * board is named in a note (the library then refuses the claim itself). The
 * platform words mean nothing on another platform. */
static inline int aar_run_zz9000_lacks_z3(const AARInfo *info)
{
    return info->platform_id == AAR_PLATFORM_ZZ9000 &&
           !(info->platform[AAR_ZZ9000_INFO_CAPS] & AAR_ZZ9000_CAP_Z3);
}

/* AARQuery itself failed: there is no platform behind the library (for the
 * ZZ9000: no board, a zz9k.library older than 36, or firmware without the app
 * runtime). */
static inline void aar_run_query_failure_line(char *buf, size_t size, int rc)
{
    if (rc == AAR_E_NOSYS)
        snprintf(buf, size, "aar-run: no AAR platform found (the library has "
                 "none to ask).");
    else
        snprintf(buf, size, "aar-run: AARQuery failed (%s).",
                 aar_run_error_name(rc));
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

/* Exit code after a failed ELF load: 5 (RETURN_WARN) when the user stopped
 * it with Ctrl-C, 20 for any other load failure. */
static inline int aar_run_load_failure_exit(int ctrl_c_pending, int user_stop)
{
    return (ctrl_c_pending != 0 || user_stop != 0) ? 5 : 20;
}

/* A Ctrl-C the launcher has seen stays seen: a later read of the signal (a
 * libnix stdio call, a DOS packet wait, the library) may have consumed it, and
 * the exit code must still be the user's 5, not a launcher error. */
static inline int aar_run_break_latch(int latched, int ctrl_c_pending,
                                      int user_stop)
{
    return (latched != 0 || ctrl_c_pending != 0 || user_stop != 0) ? 1 : 0;
}

/* The stack a command really runs on. A Shell command gets the CLI's default
 * stack (cli_DefaultStack, in longwords: what the Stack command sets); the
 * task bounds alone can describe a larger stack than the command was given.
 * A Workbench start has no CLI: the task bounds are the icon's stack. */
static inline int aar_run_stack_ok(uint32_t task_bytes, int has_cli,
                                   uint32_t cli_default_longs)
{
    uint32_t bytes = task_bytes;

    if (has_cli) {
        uint32_t cli_bytes = cli_default_longs > UINT32_MAX / 4u ?
                                 UINT32_MAX : cli_default_longs * 4u;

        if (cli_bytes < bytes)
            bytes = cli_bytes;
    }
    return bytes >= AAR_SERVE_MIN_STACK;
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

/* ---- The image against the platform, before a claim (D5) -------------------- */

/* What AARProbeFile and the platform's AARInfo say about an image, before
 * AAROpen: nothing is claimed for an image that cannot run. */
enum aar_run_probe_verdict {
    AAR_RUN_PROBE_OK = 0,
    AAR_RUN_PROBE_NOENT,   /* the file cannot be opened */
    AAR_RUN_PROBE_IO,      /* the file cannot be read */
    AAR_RUN_PROBE_NOT_AAR, /* not an AAR image (the probe's AAR_E_INVAL) */
    AAR_RUN_PROBE_FAILED,  /* any other answer of the probe */
    AAR_RUN_PROBE_PROFILE, /* an image of a profile the platform does not run */
    AAR_RUN_PROBE_API,     /* the image needs a newer API than the platform has */
    AAR_RUN_PROBE_GROUP    /* the image requires a group the platform lacks */
};

/* probe_rc is what AARProbeFile returned and img what it filled. The probe
 * judges the note against the API of the library; the platform may be older,
 * so the image is compared with AARInfo here: profile first, then the API,
 * then the groups the image requires (the optional ones are a hint). */
static inline enum aar_run_probe_verdict
aar_run_probe_check(int probe_rc, const AARImageInfo *img,
                    const AARInfo *platform)
{
    if (probe_rc == AAR_E_NOENT)
        return AAR_RUN_PROBE_NOENT;
    if (probe_rc == AAR_E_IO)
        return AAR_RUN_PROBE_IO;
    if (probe_rc == AAR_E_INVAL)
        return AAR_RUN_PROBE_NOT_AAR;
    if (probe_rc != AAR_OK)
        return AAR_RUN_PROBE_FAILED;
    /* A profile is a number; the platform's mask has a bit for each. The
     * range keeps the shift defined. */
    if (img->profile < 1u || img->profile > 32u ||
        !(platform->profiles & (1u << (img->profile - 1u))))
        return AAR_RUN_PROBE_PROFILE;
    if (img->api_major != platform->api_major ||
        img->api_minor_required > platform->api_minor)
        return AAR_RUN_PROBE_API;
    if (img->groups_required & ~platform->groups)
        return AAR_RUN_PROBE_GROUP;
    return AAR_RUN_PROBE_OK;
}

/* A mask of AAR_GROUP_* as words: "display, input", "none" for 0. A bit that
 * is not defined is written in hex. */
static inline void aar_run_group_names(char *buf, size_t size, uint32_t mask)
{
    static const char *const names[] = {"display", "input", "audio", "gl",
                                        "services"};
    uint32_t known = 0;
    size_t n = 0;
    unsigned i;

    if (!size)
        return;
    buf[0] = '\0';
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (mask & (1u << i)) {
            snprintf(buf + n, size - n, "%s%s", n ? ", " : "", names[i]);
            n = strlen(buf);
            known |= 1u << i;
        }
    }
    if (mask & ~known) {
        snprintf(buf + n, size - n, "%s0x%lx", n ? ", " : "",
                 (unsigned long)(mask & ~known));
        n = strlen(buf);
    }
    if (!n)
        snprintf(buf, size, "none");
}

static inline const char *aar_run_profile_name(uint32_t profile)
{
    return profile == AAR_PROFILE_A32 ? "A32" :
           profile == AAR_PROFILE_A64 ? "A64" : "unknown";
}

/* The profiles of a platform's mask, for a refusal line. */
static inline void aar_run_profiles_text(char *buf, size_t size, uint32_t mask)
{
    const int a32 = (mask & AAR_PROFILEF_A32) != 0;
    const int a64 = (mask & AAR_PROFILEF_A64) != 0;

    snprintf(buf, size, "%s", a32 && a64 ? "A32 and A64" : a32 ? "A32" :
             a64 ? "A64" : "no");
}

/* The refusal of an image, with the cause. `path` is the name the user gave. */
static inline void aar_run_probe_line(char *buf, size_t size,
                                      enum aar_run_probe_verdict v,
                                      const char *path, const AARImageInfo *img,
                                      const AARInfo *platform)
{
    char list[48];

    switch (v) {
    case AAR_RUN_PROBE_NOENT:
        snprintf(buf, size, "aar-run: cannot open %s", path);
        break;
    case AAR_RUN_PROBE_IO:
        snprintf(buf, size, "aar-run: cannot read %s", path);
        break;
    case AAR_RUN_PROBE_NOT_AAR:
        snprintf(buf, size, "aar-run: %s is not an AAR image (no valid AAR "
                 "ELF note, or one built for another API than this library)",
                 path);
        break;
    case AAR_RUN_PROBE_PROFILE:
        aar_run_profiles_text(list, sizeof(list), platform->profiles);
        if (img->profile == AAR_PROFILE_A32 || img->profile == AAR_PROFILE_A64)
            snprintf(buf, size, "aar-run: %s is an %s app; this platform runs "
                     "%s apps only.", path, aar_run_profile_name(img->profile),
                     list);
        else
            snprintf(buf, size, "aar-run: %s declares unknown profile %lu; "
                     "this platform runs %s apps.", path,
                     (unsigned long)img->profile, list);
        break;
    case AAR_RUN_PROBE_API:
        snprintf(buf, size, "aar-run: %s needs AAR API %lu.%lu, this platform "
                 "has %lu.%lu.", path, (unsigned long)img->api_major,
                 (unsigned long)img->api_minor_required,
                 (unsigned long)platform->api_major,
                 (unsigned long)platform->api_minor);
        break;
    case AAR_RUN_PROBE_GROUP:
        aar_run_group_names(list, sizeof(list),
                            img->groups_required & ~platform->groups);
        snprintf(buf, size, "aar-run: %s needs service groups this platform "
                 "lacks: %s.", path, list);
        break;
    case AAR_RUN_PROBE_FAILED:
        snprintf(buf, size, "aar-run: cannot probe %s", path);
        break;
    default:
        if (size)
            buf[0] = '\0';
        break;
    }
}

/* The exit code of a refused image: 20, a launcher error like a refused load.
 * A Ctrl-C that stopped the probe's own open or read is the user's: 5. */
static inline int aar_run_probe_exit(enum aar_run_probe_verdict v,
                                     int ctrl_c_pending, int user_stop)
{
    if (v == AAR_RUN_PROBE_NOENT || v == AAR_RUN_PROBE_IO ||
        v == AAR_RUN_PROBE_FAILED)
        return aar_run_load_failure_exit(ctrl_c_pending, user_stop);
    return 20;
}

/* A name with a volume or assign colon may be a multi-directory assign: the
 * library's own open tries one directory, and aar-run finds the file with its
 * DOS packets (aar_serve_locate), which needs the serving loop and so a claim.
 * Only that case waits to probe the full name; every other verdict, and a
 * missing plain path, is final at once. */
static inline int aar_run_probe_may_wait(enum aar_run_probe_verdict v,
                                         const char *path)
{
    return v == AAR_RUN_PROBE_NOENT && path && strchr(path, ':') != NULL;
}

/* ---- STATUS and RELEASE through AARExt -------------------------------------- */

/* AARExt returned this table, or NULL on a platform that has none. Both
 * commands need both calls. */
static inline int aar_run_admin_available(const aar_client_ext_zz9000 *ext)
{
    return AAR_CLIENT_EXT_ZZ9000_COVERS(ext, list) && ext->list != NULL &&
           AAR_CLIENT_EXT_ZZ9000_COVERS(ext, release_handle) &&
           ext->release_handle != NULL;
}

static inline void aar_run_admin_unsupported_line(char *buf, size_t size,
                                                  const char *command)
{
    snprintf(buf, size, "aar-run: %s is not supported on this platform.",
             command);
}

#endif /* AAR_RUN_POLICY_H */
