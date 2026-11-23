/*
 * Runner side of C-ELF (conformance/TESTS.md): image refusal and acceptance.
 * This file belongs to the C-ELF author.
 *
 * The images are the files of conformance/negative (made by gen_negative.py,
 * copied to <DIR>/negative); its MANIFEST says, per image, which rows it counts
 * for, which codes AARLoadFile may answer and what AARProbeFile does. The first
 * C-ELF test that runs goes through the whole corpus of the profile under test
 * once and keeps the outcome per row; every test then reports its row.
 *
 * For every image: AARProbeFile; a claim, AARLoadFile and (accepted images) the
 * start and the wait for the exit code; AARClose, which has to give AAR_OK; then
 * a second claim for AARLoadMemory of the same bytes, which has to give the same
 * answer (C-ELF.42). The next AAROpen proves that a refusal left nothing behind.
 * Every refusal is collected for C-ELF.41.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf_util.h"

#define ELF_IDS 44
#define IMAGE_TIMEOUT_TICKS (10UL * CONF_TICKS_PER_SECOND)

/* The codes a load may answer with (spec 5.2), as a mask. */
#define LC_PROFILE (1UL << 0)
#define LC_INVAL   (1UL << 1)
#define LC_NOGROUP (1UL << 2)
#define LC_NOMEM   (1UL << 3)
#define LC_OK      (1UL << 4)
#define LC_ANY     (1UL << 5)
#define LC_REFUSALS (LC_PROFILE | LC_INVAL | LC_NOGROUP | LC_NOMEM)

struct acc {
    UWORD pass, fail;
    char first_fail[CONF_DETAIL_MAX];
    char last_pass[CONF_DETAIL_MAX];
};

static struct acc accs[ELF_IDS + 1];
static int corpus_done;
static int images_seen;       /* images that applied to this run */
static char corpus_problem[CONF_DETAIL_MAX]; /* the corpus could not be read */

static void note(int id, int ok, const char *detail)
{
    struct acc *a;

    if (id < 1 || id > ELF_IDS)
        return;
    a = &accs[id];
    if (ok) {
        a->pass++;
        snprintf(a->last_pass, sizeof a->last_pass, "%s", detail);
    } else {
        if (!a->fail)
            snprintf(a->first_fail, sizeof a->first_fail, "%s", detail);
        a->fail++;
    }
}

static int is_refusal_code(LONG rc)
{
    return rc == AAR_E_PROFILE || rc == AAR_E_INVAL || rc == AAR_E_NOGROUP ||
           rc == AAR_E_NOMEM;
}

static int is_library_code(LONG rc)
{
    return rc <= AAR_OK && rc >= AAR_E_OWNER;
}

static ULONG code_mask(LONG rc)
{
    switch (rc) {
    case AAR_E_PROFILE: return LC_PROFILE;
    case AAR_E_INVAL: return LC_INVAL;
    case AAR_E_NOGROUP: return LC_NOGROUP;
    case AAR_E_NOMEM: return LC_NOMEM;
    default: return 0;
    }
}

/* "PROFILE|INVAL", "OK" or "ANY" as a mask. */
static ULONG parse_load(const char *s)
{
    ULONG m = 0;

    while (*s) {
        if (strncmp(s, "PROFILE", 7) == 0) {
            m |= LC_PROFILE;
            s += 7;
        } else if (strncmp(s, "INVAL", 5) == 0) {
            m |= LC_INVAL;
            s += 5;
        } else if (strncmp(s, "NOGROUP", 7) == 0) {
            m |= LC_NOGROUP;
            s += 7;
        } else if (strncmp(s, "NOMEM", 5) == 0) {
            m |= LC_NOMEM;
            s += 5;
        } else if (strncmp(s, "OK", 2) == 0) {
            m |= LC_OK;
            s += 2;
        } else if (strncmp(s, "ANY", 3) == 0) {
            m |= LC_ANY;
            s += 3;
        } else {
            s++;
        }
    }
    return m;
}

static const char *mask_text(ULONG m, char *buf, ULONG size)
{
    buf[0] = '\0';
    if (m & LC_PROFILE)
        snprintf(buf + strlen(buf), size - strlen(buf), "PROFILE ");
    if (m & LC_INVAL)
        snprintf(buf + strlen(buf), size - strlen(buf), "INVAL ");
    if (m & LC_NOGROUP)
        snprintf(buf + strlen(buf), size - strlen(buf), "NOGROUP ");
    if (m & LC_NOMEM)
        snprintf(buf + strlen(buf), size - strlen(buf), "NOMEM ");
    if (m & LC_OK)
        snprintf(buf + strlen(buf), size - strlen(buf), "OK ");
    return buf;
}

/* ---- One attempt to load an image -------------------------------------------- */

struct attempt {
    LONG open_rc;
    LONG load_rc;
    LONG close_rc;
    int ran;                 /* started and waited for */
    int ended;
    LONG exit_code;
    ULONG end_reason, state;
    LONG start_rc;
};

static const char *const run_argv[] = { "conf_elf" };

/* A claim, a load from the file (path) or from memory (data), and, when the image
 * loaded and run is set, the start and the wait for its end. */
static void attempt(const char *path, const UBYTE *data, ULONG len, int run,
                    struct attempt *a)
{
    AARHandle *h = NULL;
    ULONG reason = 0;

    memset(a, 0, sizeof *a);
    a->open_rc = AAROpen(&h, NULL, &reason);
    if (a->open_rc != AAR_OK || !h)
        return;
    a->load_rc = path ? AARLoadFile(h, path) : AARLoadMemory(h, data, len);
    if (a->load_rc == AAR_OK && run) {
        a->start_rc = AARStart(h, run_argv, 1);
        if (a->start_rc == AAR_OK) {
            struct conf_util_end e;

            conf_util_wait_end(h, IMAGE_TIMEOUT_TICKS, &e);
            a->ran = 1;
            a->ended = e.ended;
            a->exit_code = e.exit_code;
            a->end_reason = e.end_reason;
            a->state = e.state;
            if (!e.ended)
                (void)AARStop(h);
        }
    }
    a->close_rc = AARClose(h);
}

/* ---- One line of the manifest ------------------------------------------------- */

static void judge_ids(const char *ids, int ok, const char *detail)
{
    const char *p = ids;

    while ((p = strstr(p, "C-ELF.")) != NULL) {
        p += 6;
        note((int)strtoul(p, NULL, 10), ok, detail);
    }
}

static int ids_has(const char *ids, int n)
{
    const char *p = ids;

    while ((p = strstr(p, "C-ELF.")) != NULL) {
        p += 6;
        if ((int)strtoul(p, NULL, 10) == n)
            return 1;
    }
    return 0;
}

static ULONG profile_value_of(const char *name)
{
    return strcmp(name, "a64") == 0 ? AAR_PROFILE_A64 : AAR_PROFILE_A32;
}

static ULONG profile_bit_of(const char *name)
{
    return 1UL << (profile_value_of(name) - 1UL);
}

static void do_image(const char *name, const char *prof, const char *ids,
                     const char *loadspec, const char *probespec)
{
    char path[256];
    char d[CONF_DETAIL_MAX];
    char want[48];
    ULONG want_mask = parse_load(loadspec);
    AARImageInfo ii;
    struct attempt af, am;
    UBYTE *data = NULL;
    ULONG len = 0;
    LONG probe_rc;
    LONG rd;
    int ok = 1;
    int unlisted = ids_has(ids, 31);

    if (want_mask & LC_ANY)
        return; /* an image that other areas use */
    if (unlisted) {
        /* C-ELF.31 is judged with the image of a profile the platform lacks. */
        if (conf_g.info.profiles & profile_bit_of(prof))
            return;
    } else if (strcmp(prof, conf_g.profile_name) != 0) {
        return;
    }
    if (!conf_util_image_path(path, sizeof path, prof, name)) {
        judge_ids(ids, 0, "image path too long");
        return;
    }
    images_seen++;
    d[0] = '\0';

    /* AARProbeFile */
    memset(&ii, 0, sizeof ii);
    ii.size = sizeof ii;
    probe_rc = AARProbeFile(path, &ii);
    if (probe_rc == AAR_E_NOENT) {
        conf_detailf(d, "%.30s: the file is missing, copy conformance/negative to DIR", name);
        ok = 0;
    } else if (strcmp(probespec, "ok") == 0 && probe_rc != AAR_OK) {
        conf_detailf(d, "%.30s: AARProbeFile refused it: %ld", name, (long)probe_rc);
        ok = 0;
    } else if (strcmp(probespec, "ok") == 0 && ii.profile != profile_value_of(prof)) {
        conf_detailf(d, "%.30s: AARProbeFile says profile %lu", name, (unsigned long)ii.profile);
        ok = 0;
    } else if (strcmp(probespec, "refuse") == 0 &&
               (probe_rc == AAR_OK || !is_library_code(probe_rc))) {
        conf_detailf(d, "%.30s: AARProbeFile returned %ld, a negative code of 3.5 is due",
                     name, (long)probe_rc);
        ok = 0;
    } else if (strcmp(probespec, "any") == 0 && !is_library_code(probe_rc)) {
        conf_detailf(d, "%.30s: AARProbeFile returned %ld", name, (long)probe_rc);
        ok = 0;
    }

    /* AARLoadFile, and the start of an accepted image. */
    attempt(path, NULL, 0, (want_mask & LC_OK) != 0, &af);
    if (af.open_rc != AAR_OK) {
        if (ok)
            conf_detailf(d, "%.30s: AAROpen returned %ld", name, (long)af.open_rc);
        ok = 0;
    } else if (want_mask & LC_OK) {
        if (af.load_rc != AAR_OK) {
            if (ok)
                conf_detailf(d, "%.30s: AARLoadFile refused it: %ld", name, (long)af.load_rc);
            ok = 0;
        } else if (af.start_rc != AAR_OK) {
            if (ok)
                conf_detailf(d, "%.30s: AARStart returned %ld", name, (long)af.start_rc);
            ok = 0;
        } else if (!af.ended || af.end_reason != AAR_END_EXIT ||
                   af.exit_code != CONF_IMAGE_EXIT) {
            if (ok)
                conf_detailf(d, "%.30s: ended %s, reason %lu, exit %ld (want %ld)", name,
                             af.ended ? "yes" : "never", (unsigned long)af.end_reason,
                             (long)af.exit_code, (long)CONF_IMAGE_EXIT);
            ok = 0;
        }
    } else if (af.load_rc == AAR_OK) {
        if (ok)
            conf_detailf(d, "%.30s: AARLoadFile accepted it", name);
        ok = 0;
    } else if (!(want_mask & code_mask(af.load_rc))) {
        if (ok)
            conf_detailf(d, "%.30s: AARLoadFile returned %ld, want %s", name,
                         (long)af.load_rc, mask_text(want_mask, want, sizeof want));
        ok = 0;
    }
    if (af.open_rc == AAR_OK && af.close_rc != AAR_OK) {
        if (ok)
            conf_detailf(d, "%.30s: AARClose returned %ld", name, (long)af.close_rc);
        ok = 0;
    }
    judge_ids(ids, ok, ok ? name : d);

    /* C-ELF.41: a refusal is one of the four codes of spec 5.2. */
    if (af.open_rc == AAR_OK && af.load_rc != AAR_OK) {
        char e[CONF_DETAIL_MAX];

        conf_detailf(e, "%.30s refused with %ld", name, (long)af.load_rc);
        note(41, is_refusal_code(af.load_rc), e);
    }

    /* C-ELF.42: AARLoadMemory gives the same answer. */
    rd = conf_util_read_file(path, &data, &len);
    if (rd != 0) {
        conf_detailf(d, "%.30s: the file cannot be read for the memory load", name);
        note(42, 0, d);
        return;
    }
    attempt(NULL, data, len, (want_mask & LC_OK) != 0, &am);
    FreeVec(data);
    if (am.open_rc != AAR_OK) {
        conf_detailf(d, "%.30s: AAROpen returned %ld", name, (long)am.open_rc);
        note(42, 0, d);
    } else if (am.load_rc != af.load_rc) {
        conf_detailf(d, "%.30s: AARLoadMemory %ld, AARLoadFile %ld", name,
                     (long)am.load_rc, (long)af.load_rc);
        note(42, 0, d);
    } else if (am.load_rc == AAR_OK && (am.exit_code != af.exit_code ||
                                       am.end_reason != af.end_reason)) {
        conf_detailf(d, "%.30s: memory load ends %lu/%ld, file load %lu/%ld", name,
                     (unsigned long)am.end_reason, (long)am.exit_code,
                     (unsigned long)af.end_reason, (long)af.exit_code);
        note(42, 0, d);
    } else if (am.close_rc != AAR_OK) {
        conf_detailf(d, "%.30s: AARClose returned %ld", name, (long)am.close_rc);
        note(42, 0, d);
    } else {
        note(42, 1, name);
    }
}

static void run_corpus(void)
{
    char path[256];
    UBYTE *buf = NULL;
    ULONG len = 0;
    char *p;
    char *end;

    if (corpus_done)
        return;
    corpus_done = 1;
    if (!conf_util_negative_path(path, sizeof path, "MANIFEST")) {
        conf_detailf(corpus_problem, "the corpus path is too long");
        return;
    }
    if (conf_util_read_file(path, &buf, &len) != 0) {
        conf_detailf(corpus_problem, "cannot read %.60s: copy conformance/negative to DIR",
                     path);
        return;
    }
    p = (char *)buf;
    end = p + len;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *f[5];
        int n = 0;
        char *q;

        if (nl)
            *nl = '\0';
        else
            nl = end;
        if (nl > p && nl[-1] == '\r')
            nl[-1] = '\0';
        if (*p != '#' && *p != '\0') {
            q = p;
            while (n < 5 && *q) {
                f[n++] = q;
                while (*q && *q != ' ')
                    q++;
                if (*q)
                    *q++ = '\0';
            }
            if (n == 5)
                do_image(f[0], f[1], f[2], f[3], f[4]);
        }
        p = nl + 1;
    }
    FreeVec(buf);
    if (!images_seen)
        conf_detailf(corpus_problem, "the manifest lists no image for %s",
                     conf_g.profile_name);
}

/* ---- The tests ------------------------------------------------------------------- */

/* C-ELF.1 to C-ELF.38, C-ELF.40, C-ELF.41, C-ELF.42: the outcome of the corpus. */
static void t_row(int idx)
{
    const char *id = conf_tests[idx].id;
    int n = (int)strtoul(strchr(id, '.') + 1, NULL, 10);
    const struct acc *a;
    char d[CONF_DETAIL_MAX];

    run_corpus();
    if (corpus_problem[0]) {
        conf_set_result(idx, CONF_R_FAIL, corpus_problem);
        return;
    }
    a = &accs[n];
    if (a->fail) {
        conf_detailf(d, "%u of %u images failed: %.80s", (unsigned)a->fail,
                     (unsigned)(a->fail + a->pass), a->first_fail);
        conf_set_result(idx, CONF_R_FAIL, d);
    } else if (a->pass) {
        if (a->pass == 1) {
            conf_set_result(idx, CONF_R_PASS, a->last_pass);
        } else {
            conf_detailf(d, "%u images, e.g. %.60s", (unsigned)a->pass, a->last_pass);
            conf_set_result(idx, CONF_R_PASS, d);
        }
    } else if (n == 31 && (conf_g.info.profiles & 3UL) == 3UL) {
        conf_set_result(idx, CONF_R_PASS, "all profiles supported");
    } else {
        conf_set_result(idx, CONF_R_FAIL, "no image of the corpus applied to this row");
    }
}

/* C-ELF.39: the platform's image size limit. */
static void t_max_image(int idx)
{
    char d[CONF_DETAIL_MAX];
    ULONG max = conf_g.info.max_image_bytes;

    conf_detailf(d, "max_image_bytes %lu", (unsigned long)max);
    conf_set_result(idx, max >= 16UL * 1024UL * 1024UL ? CONF_R_PASS : CONF_R_FAIL, d);
}

static const struct conf_r_test tests[] = {
    { "C-ELF.1", t_row },   { "C-ELF.2", t_row },   { "C-ELF.3", t_row },
    { "C-ELF.4", t_row },   { "C-ELF.5", t_row },   { "C-ELF.6", t_row },
    { "C-ELF.7", t_row },   { "C-ELF.8", t_row },   { "C-ELF.9", t_row },
    { "C-ELF.10", t_row },  { "C-ELF.11", t_row },  { "C-ELF.12", t_row },
    { "C-ELF.13", t_row },  { "C-ELF.14", t_row },  { "C-ELF.15", t_row },
    { "C-ELF.16", t_row },  { "C-ELF.17", t_row },  { "C-ELF.18", t_row },
    { "C-ELF.19", t_row },  { "C-ELF.20", t_row },  { "C-ELF.21", t_row },
    { "C-ELF.22", t_row },  { "C-ELF.23", t_row },  { "C-ELF.24", t_row },
    { "C-ELF.25", t_row },  { "C-ELF.26", t_row },  { "C-ELF.27", t_row },
    { "C-ELF.28", t_row },  { "C-ELF.29", t_row },  { "C-ELF.30", t_row },
    { "C-ELF.31", t_row },  { "C-ELF.32", t_row },  { "C-ELF.33", t_row },
    { "C-ELF.34", t_row },  { "C-ELF.35", t_row },  { "C-ELF.36", t_row },
    { "C-ELF.37", t_row },  { "C-ELF.38", t_row },  { "C-ELF.39", t_max_image },
    { "C-ELF.40", t_row },  { "C-ELF.41", t_row },  { "C-ELF.42", t_row },
};

const struct conf_area conf_area_elf = {
    .name = "C-ELF",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
};
