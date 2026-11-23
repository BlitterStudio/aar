/*
 * Runner side of C-FREE (conformance/TESTS.md): what a platform gets back. The
 * payload conf_free judges the freestanding build from inside (C-FREE.1, .2, run
 * by the core as variant main); everything else is here, as loops over claims
 * and payload runs, with the payload's variants (conf_free.h):
 *
 *   C-FREE.4   20 runs of `hog` (largest private heap, never freed): the last run
 *              gets as much as the first, within one granule
 *   C-FREE.5   the same with `sharedhog` (console and file buffers in use, every
 *              shared block taken, never freed), within 1 MiB
 *   C-FREE.6   200 rounds of open, load, start, exit, close with `quick`: none is
 *              refused with AAR_E_NORESOURCE or AAR_E_NOMEM, or at all
 *   C-FREE.7   after load refusals on every path (images made from conf_free.elf by
 *              damaging its header, note and segments, plus every file under
 *              <DIR>/negative if there is one), the number of claims that can be
 *              open, the heap and the shared memory a payload can get are the same
 *   C-FREE.8   20 runs of `services` (four decode sessions of each kind open at
 *              exit): every run opens four of each kind that has a codec
 *   C-FREE.9   20 runs of `display` (open at exit): every run opens it
 *   C-FREE.10  20 runs of `audio` (open at exit): every run opens it
 *
 * Assumptions for the card run: a payload run takes seconds (it serves the
 * console and one message); AAROpen can be called 32 times by a task (the claim
 * count stops there); AARLoadMemory takes the damaged images; the tolerance of one
 * granule (1 MiB) in .4 and .5 is for rounding, a leak loses the whole amount.
 * This file belongs to the C-FREE author.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "../payloads/conf_free.h"

#define RUNS 20UL
#define ROUNDS 200UL
#define CLAIM_CAP 32UL
#define MAX_CORPUS_FILES 500UL

static void pass_fail(int idx, int ok, const char *detail)
{
    conf_set_result(idx, ok ? CONF_R_PASS : CONF_R_FAIL, detail);
}

/* ---- Running a variant and reading its message ------------------------------------------------- */

struct reply {
    ULONG have;
    ULONG w[4];
};

static void on_reply(struct conf_run *run, const AARMessage *msg)
{
    struct reply *r = (struct reply *)run->user;
    unsigned i;

    if (!r)
        return;
    r->have++;
    for (i = 0; i < 4; i++)
        r->w[i] = conf_free_word(msg->bytes, i);
}

/* Runs the variant to its end. Returns 1 when it ran and sent its message. */
static int run_variant(const char *variant, struct reply *r, char *why)
{
    struct conf_run run;
    struct conf_outcome out;

    memset(r, 0, sizeof *r);
    conf_run_init(&run, "C-FREE");
    run.variant = variant;
    run.norelay = 1;
    run.user = r;
    conf_run_payload(&run, &out);
    conf_run_free(&run);
    if (!out.ran) {
        conf_detailf(why, "%.100s", out.why[0] ? out.why : "did not run");
        return 0;
    }
    if (out.timed_out) {
        conf_detailf(why, "did not finish in time");
        return 0;
    }
    if (out.end_reason != AAR_END_EXIT) {
        conf_detailf(why, "ended with end reason %lu, fault %lu", (unsigned long)out.end_reason,
                     (unsigned long)out.fault_class);
        return 0;
    }
    if (r->have == 0) {
        conf_detailf(why, "sent no message");
        return 0;
    }
    return 1;
}

/* ---- C-FREE.4 and C-FREE.5: the same amount after 20 runs ------------------------------------------ */

/* unit: what w[0] counts; slack: how much less than the first run still passes. */
static void same_amount(int idx, const char *variant, const char *unit, ULONG slack)
{
    struct reply r;
    char why[CONF_DETAIL_MAX];
    char d[CONF_DETAIL_MAX];
    ULONG first = 0;
    ULONG last = 0;
    ULONG i;

    for (i = 0; i < RUNS; i++) {
        if (!run_variant(variant, &r, why)) {
            conf_detailf(d, "run %lu of %lu: %.80s", (unsigned long)(i + 1), (unsigned long)RUNS, why);
            pass_fail(idx, 0, d);
            return;
        }
        if (i == 0) {
            first = r.w[0];
            if (first == 0) {
                pass_fail(idx, 0, "the first run got nothing");
                return;
            }
        }
        last = r.w[0];
        if (last + slack < first) {
            conf_detailf(d, "run %lu got %lu %s, the first run got %lu", (unsigned long)(i + 1),
                         (unsigned long)last, unit, (unsigned long)first);
            pass_fail(idx, 0, d);
            return;
        }
    }
    conf_detailf(d, "%lu runs: first %lu %s, last %lu %s", (unsigned long)RUNS, (unsigned long)first,
                 unit, (unsigned long)last, unit);
    pass_fail(idx, 1, d);
}

/* C-FREE.4 */
static void t_heap(int idx)
{
    same_amount(idx, "hog", "MiB", 1UL);
}

/* C-FREE.5 */
static void t_shared(int idx)
{
    same_amount(idx, "sharedhog", "KiB", 1024UL);
}

/* ---- C-FREE.6: 200 claims -------------------------------------------------------------------------------- */

/* The path of a file of the payload directory of the profile. */
static int payload_path(char *path, ULONG size, const char *file)
{
    snprintf(path, size, "%s", conf_g.dir);
    return AddPart((STRPTR)path, (CONST_STRPTR)conf_g.profile_name, size) &&
           AddPart((STRPTR)path, (CONST_STRPTR)file, size);
}

/* One claim that does nothing: open, load conf_free.elf, start it as `quick`
 * (it returns at once), wait for the end, close. Returns AAR_OK or the code of
 * the call that refused; *step names it. */
static LONG one_claim(const char *path, const char **step)
{
    static const char *const argv[] = { "conf_free.elf", "conf:variant=quick" };
    AARHandle *h = NULL;
    ULONG reason = 0;
    AARStatusInfo st;
    ULONG waited = 0;
    LONG rc;

    *step = "AAROpen";
    rc = AAROpen(&h, NULL, &reason);
    if (rc != AAR_OK)
        return rc;
    *step = "AARLoadFile";
    rc = AARLoadFile(h, path);
    if (rc == AAR_OK) {
        *step = "AARStart";
        rc = AARStart(h, argv, 2UL);
    }
    if (rc == AAR_OK) {
        *step = "the app did not end";
        for (;;) {
            memset(&st, 0, sizeof st);
            st.size = sizeof st;
            if (AARStatus(h, &st) == AAR_OK &&
                (st.state == AAR_STATE_IDLE || st.state == AAR_STATE_FAULTED ||
                 st.end_reason != AAR_END_NONE))
                break;
            if (++waited > 500UL) { /* 10 s */
                rc = AAR_E_IO;
                (void)AARStop(h);
                break;
            }
            Delay(1);
        }
    }
    (void)AARClose(h);
    return rc;
}

/* C-FREE.6 */
static void t_claims(int idx)
{
    char path[256];
    char d[CONF_DETAIL_MAX];
    const char *step = "";
    ULONG i;
    LONG rc;

    if (!payload_path(path, sizeof path, "conf_free.elf")) {
        pass_fail(idx, 0, "payload path too long");
        return;
    }
    for (i = 0; i < ROUNDS; i++) {
        rc = one_claim(path, &step);
        if (rc != AAR_OK) {
            conf_detailf(d, "round %lu of %lu: %s returned %ld", (unsigned long)(i + 1),
                         (unsigned long)ROUNDS, step, (long)rc);
            pass_fail(idx, 0, d);
            return;
        }
    }
    conf_detailf(d, "%lu rounds of open, load, start, exit, close", (unsigned long)ROUNDS);
    pass_fail(idx, 1, d);
}

/* ---- C-FREE.7: refusals leave nothing behind ------------------------------------------------------------- */

struct corpus {
    ULONG tried;
    ULONG refused;
    ULONG accepted;
    ULONG open_failed;
    ULONG files;
};

/* One load that is expected to be refused: counts what happened. The image is
 * data of `len` bytes, or the file `path` when data is NULL. */
static void try_load(struct corpus *c, const void *data, ULONG len, const char *path)
{
    AARHandle *h = NULL;
    ULONG reason = 0;
    LONG rc = AAROpen(&h, NULL, &reason);

    c->tried++;
    if (rc != AAR_OK) {
        c->open_failed++;
        return;
    }
    rc = data ? AARLoadMemory(h, data, len) : AARLoadFile(h, path);
    if (rc < 0)
        c->refused++;
    else
        c->accepted++;
    (void)AARClose(h);
}

/* How many claims can be open at once, up to CLAIM_CAP (or max_apps). */
static ULONG claim_capacity(void)
{
    AARHandle *hs[CLAIM_CAP];
    ULONG reason = 0;
    ULONG cap = CLAIM_CAP;
    ULONG n = 0;
    ULONG i;

    if (conf_g.info.max_apps != 0 && conf_g.info.max_apps < cap)
        cap = conf_g.info.max_apps;
    while (n < cap && AAROpen(&hs[n], NULL, &reason) == AAR_OK)
        n++;
    for (i = 0; i < n; i++)
        (void)AARClose(hs[i]);
    return n;
}

static UBYTE *read_whole(const char *path, ULONG *len)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    UBYTE *buf = NULL;
    LONG size;

    if (!fh)
        return NULL;
    (void)Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size > 0) {
        buf = (UBYTE *)AllocVec((ULONG)size + 4096UL, MEMF_ANY | MEMF_CLEAR);
        if (buf && Read(fh, buf, size) != size) {
            FreeVec(buf);
            buf = NULL;
        }
        if (buf)
            *len = (ULONG)size;
    }
    Close(fh);
    return buf;
}

static ULONG rd16(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8);
}

static ULONG rd32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static void wr16(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)v;
    p[1] = (UBYTE)(v >> 8);
}

static void wr32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)v;
    p[1] = (UBYTE)(v >> 8);
    p[2] = (UBYTE)(v >> 16);
    p[3] = (UBYTE)(v >> 24);
}

/* The program headers of the original image. */
struct elfi {
    int c64;
    ULONG phoff, phentsize, phnum;
    LONG load0, load1, dyn, note;
    LONG magic; /* offset of the note's "AARN", or -1 */
};

static int parse_elf(const UBYTE *img, ULONG len, struct elfi *e)
{
    ULONG i;

    memset(e, 0, sizeof *e);
    e->load0 = e->load1 = e->dyn = e->note = e->magic = -1;
    if (len < 64 || img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F')
        return 0;
    e->c64 = img[4] == 2;
    e->phoff = e->c64 ? rd32(img + 32) : rd32(img + 28);
    e->phentsize = rd16(img + (e->c64 ? 54 : 42));
    e->phnum = rd16(img + (e->c64 ? 56 : 44));
    if (e->phoff + e->phnum * e->phentsize > len)
        return 0;
    for (i = 0; i < e->phnum; i++) {
        ULONG type = rd32(img + e->phoff + i * e->phentsize);

        if (type == 1 && e->load0 < 0)
            e->load0 = (LONG)i;
        else if (type == 1 && e->load1 < 0)
            e->load1 = (LONG)i;
        else if (type == 2)
            e->dyn = (LONG)i;
        else if (type == 4)
            e->note = (LONG)i;
    }
    for (i = 0; i + 4 <= len; i++) {
        if (img[i] == 'A' && img[i + 1] == 'A' && img[i + 2] == 'R' && img[i + 3] == 'N') {
            e->magic = (LONG)i;
            break;
        }
    }
    return e->load0 >= 0 && e->load1 >= 0 && e->dyn >= 0 && e->note >= 0 && e->magic >= 0;
}

static UBYTE *phdr(UBYTE *img, const struct elfi *e, LONG i)
{
    return img + e->phoff + (ULONG)i * e->phentsize;
}

/* Damages a copy of the image in one way. n counts the ways: returns 1 when
 * applied, 0 past the last, -1 when the way does not apply to this profile. */
static int mutate(UBYTE *img, ULONG len, const struct elfi *e, ULONG n, ULONG *newlen)
{
    ULONG m = (ULONG)e->magic;
    UBYTE *l0 = phdr(img, e, e->load0);
    UBYTE *l1 = phdr(img, e, e->load1);

    *newlen = len;
    switch (n) {
    case 0: img[0] = 0; return 1;                                  /* bad magic */
    case 1: img[4] = e->c64 ? 1 : 2; return 1;                     /* the other class */
    case 2: img[5] = 2; return 1;                                  /* big-endian */
    case 3: wr16(img + 16, 2); return 1;                           /* ET_EXEC */
    case 4: wr16(img + 18, 3); return 1;                           /* a machine that is not Arm */
    case 5: wr16(img + (e->c64 ? 56 : 44), 0); return 1;           /* no program headers */
    case 6: wr32(img + (e->c64 ? 32 : 28), 0x7FFFFFF0UL); return 1; /* headers beyond the file */
    case 7: wr16(img + (e->c64 ? 54 : 42), 7); return 1;           /* a bad header size */
    case 8:                                                        /* flags of the other float ABI */
        if (e->c64)
            wr32(img + 48, 1);
        else
            wr32(img + 36, 0x05000200UL);
        return 1;
    case 9:                                                        /* EABI version 4 */
        if (e->c64)
            return -1;
        wr32(img + 36, 0x04000400UL);
        return 1;
    case 10: wr32(phdr(img, e, e->note), 3); return 1;             /* PT_INTERP */
    case 11: wr32(phdr(img, e, e->dyn), 0); return 1;              /* no PT_DYNAMIC */
    case 12: wr32(phdr(img, e, e->note), 0); return 1;             /* no note */
    case 13: wr32(l0 + (e->c64 ? 4 : 24), 7); return 1;            /* a segment that is RWX */
    case 14:                                                       /* memsz below filesz */
        wr32(l0 + (e->c64 ? 40 : 20), rd32(l0 + (e->c64 ? 32 : 16)) - 1UL);
        return 1;
    case 15:                                                       /* overlapping segments */
        wr32(l1 + (e->c64 ? 16 : 8), rd32(l0 + (e->c64 ? 16 : 8)));
        return 1;
    case 16: wr32(l0 + (e->c64 ? 40 : 20), 0x7FFFFF00UL); return 1; /* a segment of 2 GiB */
    case 17: wr32(l0 + (e->c64 ? 32 : 16), len + 4096UL); return 1; /* filesz past the file */
    case 18: img[m] = 0; return 1;                                 /* note magic */
    case 19: wr32(img + m + 8, rd32(img + m + 8) == 1 ? 2UL : 1UL); return 1; /* the other profile */
    case 20: wr32(img + m + 12, 99UL); return 1;                   /* api_major */
    case 21: wr32(img + m + 28, 0x40000000UL); return 1;           /* a group no platform has */
    case 22: wr32(img + m + 24, 0xFFFFUL); return 1;               /* a heap of 64 GiB */
    case 23: wr32(img + m + 20, 3UL); return 1;                    /* a stack of 3 bytes */
    case 24: wr32(img + m + 4, 99UL); return 1;                    /* note version */
    case 25: *newlen = 52; return 1;                               /* the ELF header only */
    case 26: *newlen = len / 2; return 1;                          /* half the file */
    case 27: *newlen = len - 1; return 1;                          /* the last byte missing */
    case 28: *newlen = 0; return 1;                                /* nothing */
    case 29: memset(img, 0, 4096); *newlen = 4096; return 1;       /* zeros */
    default: return 0;
    }
}

static void scan_dir(const char *dir, int depth, struct corpus *c)
{
    BPTR lock = Lock((CONST_STRPTR)dir, SHARED_LOCK);
    struct FileInfoBlock *fib;

    if (!lock)
        return;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib)) {
        while (ExNext(lock, fib) && c->files < MAX_CORPUS_FILES) {
            char path[256];

            snprintf(path, sizeof path, "%s", dir);
            if (!AddPart((STRPTR)path, (CONST_STRPTR)fib->fib_FileName, sizeof path))
                continue;
            if (fib->fib_DirEntryType > 0) {
                if (depth > 0)
                    scan_dir(path, depth - 1, c);
            } else {
                c->files++;
                try_load(c, NULL, 0, path);
            }
        }
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
}

struct snapshot {
    ULONG claims;
    ULONG heap_mib;
    ULONG shared_kib;
};

static int take_snapshot(struct snapshot *s, char *why)
{
    struct reply r;

    s->claims = claim_capacity();
    if (!run_variant("hog", &r, why))
        return 0;
    s->heap_mib = r.w[0];
    if (!run_variant("sharedhog", &r, why))
        return 0;
    s->shared_kib = r.w[0];
    return 1;
}

/* C-FREE.7 */
static void t_corpus(int idx)
{
    char path[256];
    char dir[256];
    char d[CONF_DETAIL_MAX];
    char why[CONF_DETAIL_MAX];
    struct snapshot before;
    struct snapshot after;
    struct corpus c;
    struct elfi e;
    UBYTE *orig;
    UBYTE *work;
    ULONG len = 0;
    ULONG newlen;
    ULONG n;
    int applicable;

    if (!payload_path(path, sizeof path, "conf_free.elf")) {
        pass_fail(idx, 0, "payload path too long");
        return;
    }
    orig = read_whole(path, &len);
    if (!orig) {
        conf_detailf(d, "cannot read %.60s", path);
        pass_fail(idx, 0, d);
        return;
    }
    work = (UBYTE *)AllocVec(len + 4096UL, MEMF_ANY | MEMF_CLEAR);
    if (!work || !parse_elf(orig, len, &e)) {
        if (work)
            FreeVec(work);
        FreeVec(orig);
        pass_fail(idx, 0, "the payload image is not one this test can damage");
        return;
    }
    if (!take_snapshot(&before, why)) {
        FreeVec(work);
        FreeVec(orig);
        conf_detailf(d, "before: %.90s", why);
        pass_fail(idx, 0, d);
        return;
    }
    memset(&c, 0, sizeof c);
    for (n = 0;; n++) {
        memcpy(work, orig, len);
        applicable = mutate(work, len, &e, n, &newlen);
        if (applicable == 0)
            break;
        if (applicable > 0)
            try_load(&c, work, newlen, NULL);
    }
    snprintf(dir, sizeof dir, "%s", conf_g.dir);
    if (AddPart((STRPTR)dir, (CONST_STRPTR)"negative", sizeof dir))
        scan_dir(dir, 1, &c);
    FreeVec(work);
    FreeVec(orig);
    if (!take_snapshot(&after, why)) {
        conf_detailf(d, "after: %.90s", why);
        pass_fail(idx, 0, d);
        return;
    }
    if (c.open_failed) {
        conf_detailf(d, "%lu of %lu refusals could not even open a claim", (unsigned long)c.open_failed,
                     (unsigned long)c.tried);
        pass_fail(idx, 0, d);
    } else if (after.claims < before.claims) {
        conf_detailf(d, "claims that can be open fell from %lu to %lu after %lu refusals",
                     (unsigned long)before.claims, (unsigned long)after.claims, (unsigned long)c.refused);
        pass_fail(idx, 0, d);
    } else if (after.heap_mib + 1UL < before.heap_mib) {
        conf_detailf(d, "the heap a payload can get fell from %lu to %lu MiB after %lu refusals",
                     (unsigned long)before.heap_mib, (unsigned long)after.heap_mib, (unsigned long)c.refused);
        pass_fail(idx, 0, d);
    } else if (after.shared_kib + 1024UL < before.shared_kib) {
        conf_detailf(d, "the shared memory a payload can get fell from %lu to %lu KiB after %lu refusals",
                     (unsigned long)before.shared_kib, (unsigned long)after.shared_kib,
                     (unsigned long)c.refused);
        pass_fail(idx, 0, d);
    } else if (c.refused == 0) {
        pass_fail(idx, 0, "no damaged image was refused: nothing was tested");
    } else {
        conf_detailf(d, "%lu loads (%lu refused, %lu from files): claims %lu, heap %lu MiB, shared %lu KiB as before",
                     (unsigned long)c.tried, (unsigned long)c.refused, (unsigned long)c.files,
                     (unsigned long)after.claims, (unsigned long)after.heap_mib,
                     (unsigned long)after.shared_kib);
        pass_fail(idx, 1, d);
    }
}

/* ---- C-FREE.8 to C-FREE.10: groups ------------------------------------------------------------------------- */

/* C-FREE.8 */
static void t_services(int idx)
{
    struct reply r;
    char why[CONF_DETAIL_MAX];
    char d[CONF_DETAIL_MAX];
    ULONG i;
    ULONG images = 0;
    ULONG audios = 0;

    for (i = 0; i < RUNS; i++) {
        if (!run_variant("services", &r, why)) {
            conf_detailf(d, "run %lu of %lu: %.80s", (unsigned long)(i + 1), (unsigned long)RUNS, why);
            pass_fail(idx, 0, d);
            return;
        }
        images = r.w[1] ? 0 : r.w[0];
        audios = r.w[3] ? 0 : r.w[2];
        if ((!r.w[1] && r.w[0] < CONF_FREE_SESSIONS) || (!r.w[3] && r.w[2] < CONF_FREE_SESSIONS)) {
            conf_detailf(d, "run %lu opened %lu image and %lu audio decode sessions, not %lu of each",
                         (unsigned long)(i + 1), (unsigned long)r.w[0], (unsigned long)r.w[2],
                         (unsigned long)CONF_FREE_SESSIONS);
            pass_fail(idx, 0, d);
            return;
        }
    }
    conf_detailf(d, "%lu runs: the last opened %lu image and %lu audio sessions%s", (unsigned long)RUNS,
                 (unsigned long)images, (unsigned long)audios,
                 (r.w[1] || r.w[3]) ? " (a kind without a codec is not counted)" : "");
    pass_fail(idx, 1, d);
}

/* 20 runs of a variant whose message is the result of an open call. */
static void open_each_time(int idx, const char *variant, const char *what)
{
    struct reply r;
    char why[CONF_DETAIL_MAX];
    char d[CONF_DETAIL_MAX];
    ULONG i;

    for (i = 0; i < RUNS; i++) {
        if (!run_variant(variant, &r, why)) {
            conf_detailf(d, "run %lu of %lu: %.80s", (unsigned long)(i + 1), (unsigned long)RUNS, why);
            pass_fail(idx, 0, d);
            return;
        }
        if ((LONG)r.w[0] != AAR_OK) {
            conf_detailf(d, "run %lu: %s returned %ld", (unsigned long)(i + 1), what, (long)(LONG)r.w[0]);
            pass_fail(idx, 0, d);
            return;
        }
    }
    conf_detailf(d, "%lu runs, each opened it and exited without closing", (unsigned long)RUNS);
    pass_fail(idx, 1, d);
}

/* C-FREE.9 */
static void t_display(int idx)
{
    open_each_time(idx, "display", "aar_display_open");
}

/* C-FREE.10 */
static void t_audio(int idx)
{
    open_each_time(idx, "audio", "aar_audio_open");
}

static const struct conf_r_test tests[] = {
    { "C-FREE.4", t_heap },
    { "C-FREE.5", t_shared },
    { "C-FREE.6", t_claims },
    { "C-FREE.7", t_corpus },
    { "C-FREE.8", t_services },
    { "C-FREE.9", t_display },
    { "C-FREE.10", t_audio },
};

static const struct conf_peer peers[] = {
    { CONF_MSG_FREE_HEAP, on_reply },
    { CONF_MSG_FREE_SHARED, on_reply },
    { CONF_MSG_FREE_SVC, on_reply },
    { CONF_MSG_FREE_DISPLAY, on_reply },
    { CONF_MSG_FREE_AUDIO, on_reply },
};

/* The payload's own tests (C-FREE.1, C-FREE.2) are the run of variant main. */
static const struct conf_variant variants[] = {
    { "main", NULL },
};

const struct conf_area conf_area_free = {
    .name = "C-FREE",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
};
