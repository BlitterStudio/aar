/*
 * What aar-run does for debugging and profiling: the options CRASHLOG,
 * PROFILE and PROFILEFILE, the register lines it prints for a fault or a stall,
 * the crash report file, and the profile sample file. No AmigaOS calls, so a
 * host test (client/tests/run_debug_test.c) can exercise all of it. The file
 * formats are described in client/run/README.md; tools/aar-symbolize and
 * tools/aar-profile read them.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_RUN_DEBUG_H
#define AAR_RUN_DEBUG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aar_run_policy.h"

/* ---- Options ----------------------------------------------------------------- */

#define AAR_RUN_DEBUG_PATH_MAX 256u

/* What the command line or the tool types asked for. All zero: nothing. */
struct aar_run_debug {
    uint32_t profile_hz;                         /* PROFILE=<hz>, 0: off */
    char crashlog[AAR_RUN_DEBUG_PATH_MAX];       /* CRASHLOG=<path>, "": none */
    char profilefile[AAR_RUN_DEBUG_PATH_MAX];    /* PROFILEFILE=<path>, "": none */
};

/* One option by name (PROFILE, CRASHLOG or PROFILEFILE, upper case, no '=') and
 * its value: a command-line word and a tool type use the same rules.
 * AAR_RUN_ARG_OTHER for another name; AAR_RUN_ARG_BAD for a value that does not
 * parse: a rate that is not 1 to AAR_PROFILE_HZ_MAX, an empty path or one that
 * does not fit. */
static inline int aar_run_debug_set(struct aar_run_debug *d, const char *name,
                                    const char *value)
{
    char *path;

    if (strcmp(name, "PROFILE") == 0) {
        uint32_t hz;

        if (!aar_run_parse_positive(value, &hz) || hz > AAR_PROFILE_HZ_MAX)
            return AAR_RUN_ARG_BAD;
        d->profile_hz = hz;
        return AAR_RUN_ARG_TAKEN;
    }
    if (strcmp(name, "CRASHLOG") == 0)
        path = d->crashlog;
    else if (strcmp(name, "PROFILEFILE") == 0)
        path = d->profilefile;
    else
        return AAR_RUN_ARG_OTHER;
    if (!value || !value[0] || strlen(value) >= AAR_RUN_DEBUG_PATH_MAX)
        return AAR_RUN_ARG_BAD;
    memcpy(path, value, strlen(value) + 1u);
    return AAR_RUN_ARG_TAKEN;
}

/* One command-line word: NAME=<value>, the name matched without regard to
 * case. */
static inline int aar_run_debug_arg(const char *arg, struct aar_run_debug *d)
{
    static const char *const names[] = { "PROFILE", "CRASHLOG", "PROFILEFILE" };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char key[16];
        const char *value;

        snprintf(key, sizeof(key), "%s=", names[i]);
        value = aar_run_key_value(arg, key);
        if (value)
            return aar_run_debug_set(d, names[i], value);
    }
    return AAR_RUN_ARG_OTHER;
}

/* The same three options from the ToolTypes of a Workbench start, in the order of
 * aar_run_tt_get (the project icon, then aar-run's own). A key that is set with a
 * value that cannot be used (an empty path, a rate that is not 1 to
 * AAR_PROFILE_HZ_MAX) returns 0 and names the key in *bad_key. */
static inline int aar_run_debug_tooltypes(const struct aar_run_tt_chain *chain,
                                          struct aar_run_debug *d, const char **bad_key)
{
    static const char *const names[] = { "PROFILE", "CRASHLOG", "PROFILEFILE" };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *value = aar_run_tt_get(chain, names[i]);

        if (value && aar_run_debug_set(d, names[i], value) == AAR_RUN_ARG_BAD) {
            *bad_key = names[i];
            return 0;
        }
    }
    return 1;
}

/* PROFILE and PROFILEFILE go together: a rate with nowhere to write the samples
 * and a file with no rate to sample at are both a mistake of the command. */
enum aar_run_debug_verdict {
    AAR_RUN_DEBUG_OK = 0,
    AAR_RUN_DEBUG_NO_FILE,  /* PROFILE without PROFILEFILE */
    AAR_RUN_DEBUG_NO_RATE   /* PROFILEFILE without PROFILE */
};

static inline enum aar_run_debug_verdict
aar_run_debug_check(const struct aar_run_debug *d)
{
    if (d->profile_hz != 0u && d->profilefile[0] == '\0')
        return AAR_RUN_DEBUG_NO_FILE;
    if (d->profile_hz == 0u && d->profilefile[0] != '\0')
        return AAR_RUN_DEBUG_NO_RATE;
    return AAR_RUN_DEBUG_OK;
}

static inline void aar_run_debug_line(char *buf, size_t size,
                                      enum aar_run_debug_verdict v)
{
    switch (v) {
    case AAR_RUN_DEBUG_NO_FILE:
        snprintf(buf, size, "aar-run: PROFILE needs PROFILEFILE=<path>.");
        break;
    case AAR_RUN_DEBUG_NO_RATE:
        snprintf(buf, size, "aar-run: PROFILEFILE needs PROFILE=<hz>.");
        break;
    default:
        if (size)
            buf[0] = '\0';
        break;
    }
}

/* AAR_TAG_PROFILE_HZ appended to a tag list that holds n tags and ends in
 * TAG_DONE at tags[n]. The array needs room for one more entry. Returns the
 * number of tags. */
static inline unsigned aar_run_debug_tags(struct TagItem *tags, unsigned n,
                                          uint32_t profile_hz)
{
    if (profile_hz) {
        tags[n].ti_Tag = AAR_TAG_PROFILE_HZ;
        tags[n++].ti_Data = profile_hz;
    }
    tags[n].ti_Tag = TAG_DONE;
    tags[n].ti_Data = 0u;
    return n;
}

/* ---- Fault and stall ----------------------------------------------------------- */

/* The report of a fault or a stall: the end of an app that left registers and a
 * stack behind. A fault class, a faulted state and the two end reasons that fill
 * the fault record (AAR_END_USER_FAULT, AAR_END_STALL) all count. */
static inline int aar_run_crash_wanted(uint32_t end_reason, uint32_t fault_class,
                                       int faulted)
{
    return fault_class != AAR_FAULT_NONE || faulted ||
           end_reason == AAR_END_USER_FAULT || end_reason == AAR_END_STALL;
}

/* The line under a fault or a stall: where the app was. */
static inline void aar_run_reg_line(char *buf, size_t size, uint32_t pc,
                                    uint32_t sp, uint32_t lr)
{
    snprintf(buf, size, "  pc=%08lx sp=%08lx lr=%08lx", (unsigned long)pc,
             (unsigned long)sp, (unsigned long)lr);
}

/* ---- The crash report (CRASHLOG) ------------------------------------------------ */

/* A text file, "key: value" lines, then the stack words. Numbers that are
 * addresses are 0x and 16 digits; the other registers are 0x and 8. The first
 * word of end_reason, fault_class and profile is the number. Stack words are
 * the snapshot's bytes read as little-endian 32-bit app words, four to a line,
 * each line headed by the app address of its first byte; a last word of fewer
 * than 4 bytes has 2 digits per byte.
 *
 *   aar-crash 1
 *   end_reason: 3 user fault
 *   fault_class: 1 data abort
 *   syndrome: 0x00000805
 *   pstate: 0x60000010
 *   pc: 0x0000000000012345
 *   lr: 0x0000000000012300
 *   sp: 0x00000000bffff000
 *   fault_addr: 0x0000000000000004
 *   load_base: 0x0000000000010000
 *   profile: 1 A32
 *   exit_code: -1
 *   elf: Work:Apps/game.elf
 *   stack_sp: 0x00000000bffff000
 *   stack_bytes: 64
 *   stack:
 *   0x00000000bffff000: 00000001 00000002 00000003 00000004
 */
#define AAR_RUN_CRASH_MAGIC "aar-crash 1"
#define AAR_RUN_CRASH_BUF 4096u

struct aar_run_text {
    char *buf;
    size_t size;
    size_t len;
};

static inline void aar_run_text_add(struct aar_run_text *t, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static inline void aar_run_text_add(struct aar_run_text *t, const char *format, ...)
{
    va_list args;
    int n;

    if (t->len + 1u >= t->size)
        return;
    va_start(args, format);
    n = vsnprintf(t->buf + t->len, t->size - t->len, format, args);
    va_end(args);
    if (n < 0)
        return;
    t->len += (size_t)n < t->size - t->len ? (size_t)n : t->size - t->len - 1u;
}

static inline void aar_run_text_u64(struct aar_run_text *t, const char *key,
                                    AARU64 v)
{
    aar_run_text_add(t, "%s: 0x%08lx%08lx\n", key, (unsigned long)v.hi,
                     (unsigned long)v.lo);
}

/* Fills buf (NUL-ended, cut when it is too small) and returns the length. */
static inline size_t aar_run_crash_report(char *buf, size_t size, const char *elf,
                                          const AARStatusInfo *st,
                                          const uint8_t *stack,
                                          uint32_t stack_bytes)
{
    struct aar_run_text t;
    uint32_t i, j;

    t.buf = buf;
    t.size = size;
    t.len = 0;
    if (size)
        buf[0] = '\0';
    aar_run_text_add(&t, "%s\n", AAR_RUN_CRASH_MAGIC);
    aar_run_text_add(&t, "end_reason: %lu %s\n", (unsigned long)st->end_reason,
                     aar_run_end_reason_name(st->end_reason));
    aar_run_text_add(&t, "fault_class: %lu %s\n", (unsigned long)st->fault_class,
                     aar_run_fault_class_name(st->fault_class));
    aar_run_text_add(&t, "syndrome: 0x%08lx\n", (unsigned long)st->fault_syndrome);
    aar_run_text_add(&t, "pstate: 0x%08lx\n", (unsigned long)st->fault_pstate);
    aar_run_text_u64(&t, "pc", st->fault_pc);
    aar_run_text_u64(&t, "lr", st->fault_lr);
    aar_run_text_u64(&t, "sp", st->fault_sp);
    aar_run_text_u64(&t, "fault_addr", st->fault_addr);
    aar_run_text_u64(&t, "load_base", st->load_base);
    aar_run_text_add(&t, "profile: %lu %s\n", (unsigned long)st->profile,
                     aar_run_profile_name(st->profile));
    aar_run_text_add(&t, "exit_code: %ld\n", (long)st->exit_code);
    aar_run_text_add(&t, "elf: %s\n", elf ? elf : "");
    aar_run_text_u64(&t, "stack_sp", st->stack_sp);
    aar_run_text_add(&t, "stack_bytes: %lu\n", (unsigned long)stack_bytes);
    aar_run_text_add(&t, "stack:\n");
    for (i = 0; i < stack_bytes; i += 16u) {
        uint32_t lo = st->stack_sp.lo + i;
        uint32_t hi = st->stack_sp.hi + (lo < st->stack_sp.lo ? 1u : 0u);

        aar_run_text_add(&t, "0x%08lx%08lx:", (unsigned long)hi, (unsigned long)lo);
        for (j = i; j < i + 16u && j < stack_bytes; j += 4u) {
            uint32_t n = stack_bytes - j < 4u ? stack_bytes - j : 4u;
            uint32_t word = 0u, k;

            for (k = 0; k < n; k++)
                word |= (uint32_t)stack[j + k] << (8u * k);
            aar_run_text_add(&t, " %0*lx", (int)(2u * n), (unsigned long)word);
        }
        aar_run_text_add(&t, "\n");
    }
    return t.len;
}

/* ---- The profile file (PROFILEFILE) --------------------------------------------- */

/*
 * Binary, big-endian like the samples the library hands over:
 *
 *    0  "AARP"
 *    4  version, 1
 *    8  the rate the platform granted, Hz
 *   12  samples the ring lost before aar-run took them (written at the end)
 *   16  load base, high word then low word
 *   24  0 (reserved)
 *   28  length of the ELF path in bytes
 *   32  the ELF path, zero-padded to a multiple of 8
 *   then 8 bytes per sample: the app address, high word then low word
 *
 * The number of samples is the rest of the file divided by 8.
 */
#define AAR_RUN_PROFILE_MAGIC "AARP"
#define AAR_RUN_PROFILE_VERSION 1u
#define AAR_RUN_PROFILE_FIXED 32u
#define AAR_RUN_PROFILE_LOST_OFFSET 12u
#define AAR_RUN_PROFILE_HEADER_MAX 1024u
/* aar-run takes samples from the library at most this many at a time, and looks
 * for more once in this many passes of the serving loop (about one per
 * millisecond at the default heartbeat), and once more at the end. */
#define AAR_RUN_PROFILE_CHUNK 256u
#define AAR_RUN_PROFILE_EVERY 32u
/* A turn of the loop drains at most this many chunks, so that a platform that writes
 * samples as fast as aar-run stores them cannot keep the loop from the app's
 * requests and from Ctrl-C. The last drain, after the app has ended, may take more:
 * 1024 chunks are more than any ring holds. */
#define AAR_RUN_PROFILE_PASS_CHUNKS 4u
#define AAR_RUN_PROFILE_END_CHUNKS 1024u

/* Whether to take another chunk: the ones taken so far (chunks_done, this one
 * included) are under the budget, and the last one was full, so more may wait. */
static inline int aar_run_profile_drain_more(uint32_t chunks_done, uint32_t max_chunks,
                                             uint32_t got)
{
    return got >= AAR_RUN_PROFILE_CHUNK && chunks_done < max_chunks;
}

/* A big-endian word stored a byte at a time: the buffers have no alignment. */
static inline void aar_run_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Writes the header into buf and returns its length, 0 when buf is too small. */
static inline size_t aar_run_profile_header(uint8_t *buf, size_t size, uint32_t hz,
                                            AARU64 load_base, const char *elf)
{
    size_t n = strlen(elf);
    size_t padded = (n + 7u) & ~(size_t)7u;

    if (AAR_RUN_PROFILE_FIXED + padded > size)
        return 0;
    memset(buf, 0, AAR_RUN_PROFILE_FIXED + padded);
    memcpy(buf, AAR_RUN_PROFILE_MAGIC, 4);
    aar_run_be32(buf + 4, AAR_RUN_PROFILE_VERSION);
    aar_run_be32(buf + 8, hz);
    aar_run_be32(buf + 16, load_base.hi);
    aar_run_be32(buf + 20, load_base.lo);
    aar_run_be32(buf + 28, (uint32_t)n);
    memcpy(buf + AAR_RUN_PROFILE_FIXED, elf, n);
    return AAR_RUN_PROFILE_FIXED + padded;
}

/* The word the lost count is written into at the end. */
static inline void aar_run_profile_lost_word(uint8_t out[4], uint32_t lost)
{
    aar_run_be32(out, lost);
}

/* 1 once in AAR_RUN_PROFILE_EVERY calls: the loop's turn to drain the ring. */
static inline int aar_run_profile_due(uint32_t *passes)
{
    if (++*passes < AAR_RUN_PROFILE_EVERY)
        return 0;
    *passes = 0u;
    return 1;
}

static inline void aar_run_profile_summary_line(char *buf, size_t size,
                                                uint32_t hz, unsigned long samples,
                                                unsigned long lost,
                                                const char *path)
{
    snprintf(buf, size, "aar-run: profile: %lu samples at %lu Hz, %lu lost, in %s",
             samples, (unsigned long)hz, lost, path);
}

#endif /* AAR_RUN_DEBUG_H */
