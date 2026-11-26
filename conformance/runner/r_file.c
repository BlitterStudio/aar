/*
 * Runner side of C-FILE (conformance/TESTS.md): the client of the payload
 * conf_file. It prepares the fixture tree under ROOT (conf_file.h lists it),
 * checks the files the app leaves behind, and judges the tests that need a
 * client that serves by hand: C-FILE.26 to C-FILE.28 (a withheld reply, the 30
 * second timeout, the retired file id, a stop that ends a file wait) and
 * C-FILE.31 (the locks and handles an exiting app left open). This file belongs
 * to the C-FILE author.
 *
 * Assumptions the card run confirms: a path of 255 bytes relative to ROOT is
 * served (the fixture directories are made one component at a time, so the
 * runner itself never builds a long path); AARServiceNext, AARGetMessage and
 * AARServiceReply work from a polling loop with no notify port and no signal;
 * a reply to a request the app has given up (a timeout) is the late completion
 * the library accepts; AARStop ends a file wait with AAR_E_IO.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "../payloads/conf_file.h"

/* ---- Fixtures ------------------------------------------------------------------------ */

static BPTR root_lock(void)
{
    BPTR lock = Lock((STRPTR)conf_g.root, SHARED_LOCK);

    if (!lock) {
        lock = CreateDir((STRPTR)conf_g.root);
        if (lock) {
            UnLock(lock);
            lock = Lock((STRPTR)conf_g.root, SHARED_LOCK);
        }
    }
    return lock;
}

/* The directory `name` in `parent`, made when missing. Returns a shared lock or 0. */
static BPTR child_dir(BPTR parent, const char *name)
{
    BPTR old = CurrentDir(parent);
    BPTR lock = Lock((STRPTR)name, SHARED_LOCK);

    if (!lock) {
        lock = CreateDir((STRPTR)name);
        if (lock) {
            UnLock(lock);
            lock = Lock((STRPTR)name, SHARED_LOCK);
        }
    }
    (void)CurrentDir(old);
    return lock;
}

/* A file of len bytes in dir. kind: 0 the bytes of data, 1 the 1 MiB pattern. */
static int make_file(BPTR dir, const char *name, const char *data, ULONG len, int kind)
{
    BPTR old = CurrentDir(dir);
    BPTR fh = Open((STRPTR)name, MODE_NEWFILE);
    int ok = 1;

    if (!fh) {
        (void)CurrentDir(old);
        return 0;
    }
    if (kind == 0) {
        ok = len == 0 || Write(fh, (APTR)data, (LONG)len) == (LONG)len;
    } else {
        UBYTE *buf = AllocVec(32768UL, MEMF_ANY);
        ULONG done = 0;
        ULONG i;

        if (!buf) {
            ok = 0;
        } else {
            while (ok && done < CONF_FILE_PATTERN_BYTES) {
                for (i = 0; i < 32768UL; i++)
                    buf[i] = conf_file_pat((uint32_t)(done + i));
                ok = Write(fh, buf, 32768L) == 32768L;
                done += 32768UL;
            }
            FreeVec(buf);
        }
    }
    Close(fh);
    (void)CurrentDir(old);
    return ok;
}

/* Deletes the *.tmp files of the directory: what the payloads left behind. */
static void clear_tmp(BPTR dir)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR old;
    char names[32][32];
    ULONG count = 0;
    ULONG i;

    if (!fib)
        return;
    if (Examine(dir, fib)) {
        while (ExNext(dir, fib) && count < 32UL) {
            size_t n = strlen((const char *)fib->fib_FileName);

            if (fib->fib_DirEntryType < 0 && n > 4 && n < 32 &&
                strcmp((const char *)fib->fib_FileName + n - 4, ".tmp") == 0)
                strcpy(names[count++], (const char *)fib->fib_FileName);
        }
    }
    FreeDosObject(DOS_FIB, fib);
    old = CurrentDir(dir);
    for (i = 0; i < count; i++)
        (void)DeleteFile((STRPTR)names[i]);
    (void)CurrentDir(old);
}

static int have_big(BPTR dir)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR old = CurrentDir(dir);
    BPTR lock = Lock((STRPTR)CONF_FILE_BIG_NAME, SHARED_LOCK);
    int ok = 0;

    if (fib && lock && Examine(lock, fib))
        ok = (ULONG)fib->fib_Size == CONF_FILE_BIG_BYTES;
    if (lock)
        UnLock(lock);
    (void)CurrentDir(old);
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    return ok;
}

/* Prepares conf-fixtures/ under ROOT. Returns 1, or 0 with a diagnostic. */
static int prepare_fixtures(void)
{
    static const char *const dir5[CONF_FILE_DIR5_COUNT] = CONF_FILE_DIR5_NAMES;
    static const char *const chain[CONF_FILE_LONG_DEPTH] = {
        CONF_FILE_LD1, CONF_FILE_LD2, CONF_FILE_LD3, CONF_FILE_LD4, CONF_FILE_LD5,
        CONF_FILE_LD6, CONF_FILE_LD7
    };
    BPTR root = root_lock();
    BPTR fx = 0;
    BPTR d5 = 0;
    BPTR cur = 0;
    BPTR old;
    struct DateStamp ds;
    ULONG i;
    int ok = 1;

    if (!root) {
        conf_emitf("# C-FILE: cannot open or create ROOT %s", conf_g.root);
        return 0;
    }
    fx = child_dir(root, CONF_FILE_DIR);
    if (!fx) {
        conf_emitf("# C-FILE: cannot create %s/%s", conf_g.root, CONF_FILE_DIR);
        UnLock(root);
        return 0;
    }
    clear_tmp(fx);
    ok &= make_file(fx, CONF_FILE_TEXT_NAME, CONF_FILE_TEXT, (ULONG)strlen(CONF_FILE_TEXT), 0);
    ok &= make_file(fx, CONF_FILE_PATTERN_NAME, NULL, 0, 1);
    ok &= make_file(fx, CONF_FILE_DATED_NAME, CONF_FILE_DATED_TEXT,
                    (ULONG)strlen(CONF_FILE_DATED_TEXT), 0);
    ds.ds_Days = (LONG)CONF_FILE_DATE_DAYS;
    ds.ds_Minute = (LONG)CONF_FILE_DATE_MINS;
    ds.ds_Tick = (LONG)CONF_FILE_DATE_TICKS;
    old = CurrentDir(fx);
    ok &= SetFileDate((STRPTR)CONF_FILE_DATED_NAME, &ds) != 0;
    (void)CurrentDir(old);

    d5 = child_dir(fx, CONF_FILE_DIR5_NAME);
    if (d5) {
        for (i = 0; i < CONF_FILE_DIR5_COUNT; i++) {
            char text[16];

            snprintf(text, sizeof text, "%s\n", dir5[i]);
            ok &= make_file(d5, dir5[i], text, (ULONG)strlen(text), 0);
        }
        UnLock(d5);
    } else {
        ok = 0;
    }
    cur = child_dir(fx, CONF_FILE_LEAKDIR_NAME);
    if (cur)
        UnLock(cur);
    else
        ok = 0;

    /* The chain of seven directories, one component at a time. */
    cur = DupLock(fx);
    for (i = 0; cur && i < CONF_FILE_LONG_DEPTH; i++) {
        BPTR next = child_dir(cur, chain[i]);

        UnLock(cur);
        cur = next;
    }
    if (cur) {
        ok &= make_file(cur, CONF_FILE_LEAF24, CONF_FILE_LONG_TEXT,
                        (ULONG)strlen(CONF_FILE_LONG_TEXT), 0);
        ok &= make_file(cur, CONF_FILE_LEAF25, CONF_FILE_LONG_TEXT,
                        (ULONG)strlen(CONF_FILE_LONG_TEXT), 0);
        UnLock(cur);
    } else {
        ok = 0;
    }

    if (conf_g.info.platform_id == AAR_PLATFORM_ZZ9000 && !have_big(fx)) {
        BPTR o = CurrentDir(fx);
        BPTR fh = Open((STRPTR)CONF_FILE_BIG_NAME, MODE_NEWFILE);

        if (fh) {
            ok &= SetFileSize(fh, (LONG)CONF_FILE_BIG_BYTES, OFFSET_BEGINNING) ==
                  (LONG)CONF_FILE_BIG_BYTES;
            Close(fh);
        } else {
            ok = 0;
        }
        (void)CurrentDir(o);
    }
    UnLock(fx);
    UnLock(root);
    if (!ok)
        conf_emitf("# C-FILE: the fixture tree under %s is incomplete (dos error %ld)",
                   conf_g.root, (long)IoErr());
    return ok;
}

/* The full name of a file in the fixture directory. */
static int fx_path(char *dst, ULONG size, const char *name)
{
    snprintf(dst, size, "%s", conf_g.root);
    return AddPart((STRPTR)dst, (CONST_STRPTR)CONF_FILE_DIR, size) &&
           AddPart((STRPTR)dst, (CONST_STRPTR)name, size);
}

/* ---- The peer: the runner reads what the app wrote ------------------------------------ */

static void reply(struct conf_run *run, ULONG w0, ULONG w1)
{
    AARMessage m;

    conf_file_build(m.bytes, CONF_FILE_RES, (uint32_t)w0, (uint32_t)w1, 0, 0);
    (void)AARSendMessage(run->h, &m);
}

/* CONF_FILE_CHECK */
static void h_check(struct conf_run *run, const AARMessage *msg)
{
    ULONG which = conf_file_get32(msg->bytes, 0);
    char path[256];
    BPTR fh;
    ULONG want;
    ULONG done = 0;

    if (!fx_path(path, sizeof path, which == 1 ? "created.tmp" : "out1m.tmp")) {
        reply(run, 1, 0);
        return;
    }
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        reply(run, 1, (ULONG)IoErr());
        return;
    }
    if (which == 1) {
        char buf[64];
        LONG n = Read(fh, buf, sizeof buf);

        Close(fh);
        want = (ULONG)strlen(CONF_FILE_CREATED);
        if (n != (LONG)want || memcmp(buf, CONF_FILE_CREATED, want) != 0)
            reply(run, 2, n < 0 ? 0UL : (ULONG)n);
        else
            reply(run, 0, want);
        return;
    }
    {
        UBYTE *buf = AllocVec(32768UL, MEMF_ANY);
        ULONG code = 0;
        ULONG detail = 0;

        if (!buf) {
            Close(fh);
            reply(run, 3, 0);
            return;
        }
        while (done < CONF_FILE_PATTERN_BYTES) {
            LONG n = Read(fh, buf, 32768L);
            ULONG i;

            if (n <= 0) {
                code = 2;
                detail = done;
                break;
            }
            for (i = 0; i < (ULONG)n; i++) {
                if (buf[i] != conf_file_pat2((uint32_t)(done + i))) {
                    code = 2;
                    detail = done + i;
                    break;
                }
            }
            if (code)
                break;
            done += (ULONG)n;
        }
        if (!code && Read(fh, buf, 1L) > 0) {
            code = 2;
            detail = CONF_FILE_PATTERN_BYTES + 1UL;
        }
        FreeVec(buf);
        Close(fh);
        reply(run, code, detail);
    }
}

/* ---- C-FILE.6: what the client did with the requests after the stop ------------------------- */

/* The size of a file in the fixture directory, -1 when it is missing. */
static LONG fx_size(const char *name)
{
    char path[256];
    struct FileInfoBlock *fib;
    BPTR lock;
    LONG size = -1;

    if (!fx_path(path, sizeof path, name))
        return -1;
    lock = Lock((STRPTR)path, SHARED_LOCK);
    if (!lock)
        return -1;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib))
        size = (LONG)fib->fib_Size;
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return size;
}

/* Date (days since 1978-01-01) and protection bits of a file in the fixture directory. Returns 0, or -1 when
 * the file is missing. */
static int fx_stamp(const char *name, LONG *days, LONG *prot)
{
    char path[256];
    struct FileInfoBlock *fib;
    BPTR lock;
    int rc = -1;

    if (!fx_path(path, sizeof path, name))
        return -1;
    lock = Lock((STRPTR)path, SHARED_LOCK);
    if (!lock)
        return -1;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib)) {
        *days = (LONG)fib->fib_Date.ds_Days;
        *prot = (LONG)fib->fib_Protection;
        rc = 0;
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return rc;
}

/* The payload runs create, truncate and write after the stop request. A stop
 * ends a parked call with AAR_E_IO whatever the client then does with the
 * request, so the payload's codes prove nothing: the files do. The payload prints
 * the id; this adds a failure when the client let a request through. The id is
 * built at run time because the row is judged by the payload. */
static void on_end(const struct conf_run *run, const struct conf_outcome *out,
                   const struct conf_parsed *pr)
{
    char id[16];

    (void)pr;
    if (strcmp(run->area, "C-FILE") != 0 || !run->variant ||
        strcmp(run->variant, CONF_FILE_VARIANT_MAIN) != 0 || !out->ran)
        return;
    if (fx_size("stopkeep.tmp") < 0)
        return; /* the payload did not get to the stop test */
    snprintf(id, sizeof id, "C-FILE.%d", 6);
    if (fx_size("afterstop.tmp") >= 0)
        conf_record(id, 0, "a create after the stop request made a file");
    else if (fx_size("stopkeep.tmp") != 4)
        conf_record(id, 0, "a truncate after the stop request emptied a file");
    else if (fx_size("stopw.tmp") != 0)
        conf_record(id, 0, "a write after the stop request put bytes in a file");
    /* The calls of API 0.6 after the stop request (the payload's second stop row): the objects they were
     * aimed at must be as the payload left them. */
    snprintf(id, sizeof id, "C-FILE.%d", 49);
    if (fx_size("stopold.tmp") >= 0 || fx_size("stopnew.tmp") >= 0) {
        LONG days = 0, prot = 0;

        if (fx_size("stopdel.tmp") != 5)
            conf_record(id, 0, "a delete after the stop request removed a file");
        else if (fx_size("stopold.tmp") != 5 || fx_size("stopnew.tmp") >= 0)
            conf_record(id, 0, "a rename after the stop request moved a file");
        else if (fx_size("stopdir.tmp") >= 0)
            conf_record(id, 0, "a dir_create after the stop request made a directory");
        else if (fx_stamp("stopdate.tmp", &days, &prot) != 0 || days == (LONG)CONF_FILE_DATE_DAYS)
            conf_record(id, 0, "a set_date after the stop request changed the date of a file");
        else if (fx_stamp("stopprot.tmp", &days, &prot) != 0 || (prot & 0x0FL) != 0)
            conf_record(id, 0, "a set_protect after the stop request changed the protection of a file");
        else if (fx_size("stoptr.tmp") != 5)
            conf_record(id, 0, "a truncate after the stop request changed the length of a file");
    }
}

static const struct conf_peer peers[] = {
    { CONF_FILE_CHECK, h_check },
};

/* ---- Variants ------------------------------------------------------------------------------ */

static char abs_arg[200];

static void prepare_main(struct conf_run *run)
{
    (void)run;
    (void)prepare_fixtures();
}

/* The second half of C-FILE.5: the path of the text fixture with the volume of
 * the root, and the serving loop allowed to open it. */
static void prepare_abs(struct conf_run *run)
{
    char full[160];
    BPTR root;

    (void)prepare_fixtures();
    full[0] = '\0';
    if (strchr(conf_g.root, ':')) {
        snprintf(full, sizeof full, "%s", conf_g.root);
    } else {
        root = root_lock();
        if (root) {
            (void)NameFromLock(root, (STRPTR)full, (LONG)sizeof full);
            UnLock(root);
        }
    }
    if (!full[0] || !AddPart((STRPTR)full, (CONST_STRPTR)CONF_FILE_DIR, sizeof full) ||
        !AddPart((STRPTR)full, (CONST_STRPTR)CONF_FILE_TEXT_NAME, sizeof full)) {
        conf_emitf("# C-FILE: no full path for the ABS_PATHS run");
        return;
    }
    snprintf(abs_arg, sizeof abs_arg, "conf:abs=%s", full);
    if (strlen(abs_arg) + 1 > conf_g.info.max_arg_bytes || strlen(abs_arg) > 127) {
        conf_emitf("# C-FILE: the path %s is too long for an argument: the second half "
                   "of the path policy test is not run", full);
        return;
    }
    run->serve_flags |= AAR_SERVEF_ABS_PATHS;
    run->extra_args[run->extra_count++] = abs_arg;
}

static const struct conf_variant variants[] = {
    { CONF_FILE_VARIANT_MAIN, prepare_main },
    { CONF_FILE_VARIANT_ABS, prepare_abs },
};

/* ---- C-FILE.26 to C-FILE.28: a client that serves by hand ------------------------------------ */

struct stall {
    AARHandle *h;
    AARServiceReq withheld;     /* the open of CONF_FILE_STALL_A */
    int got_a;
    ULONG fd_a;                 /* the file id the runtime chose for it */
    int stat_seen;              /* the stat of CONF_FILE_STALL_C is withheld */
    ULONG stat_tick;
    int stopped;
    int collision;              /* a later open was given fd_a */
    ULONG opens;                /* opens of CONF_FILE_STALL_B served */
    int step1, step2, step4;
    LONG rc1, bad2, rc4;
    ULONG ms1, ms4, rounds;
};

/* The request's path is `name` (the buffer may or may not count the NUL). */
static int path_is(const AARServiceReq *req, const char *name)
{
    size_t n = strlen(name);
    const char *p = (const char *)req->ptr;

    if (!p || req->buf_len < n || strncmp(p, name, n) != 0)
        return 0;
    return req->buf_len == n || p[n] == '\0';
}

static void complete(struct stall *s, AARServiceReq *req, LONG result)
{
    memset(req->out, 0, sizeof req->out);
    req->reply_flags = 0;
    req->result = result;
    (void)AARServiceReply(s->h, req);
}

static void stall_request(struct stall *s, AARServiceReq *req)
{
    if (req->op == AAR_OP_FILE_OPEN && path_is(req, CONF_FILE_STALL_A) && !s->got_a) {
        s->withheld = *req;
        s->got_a = 1;
        s->fd_a = req->arg[AAR_A_FILE_OPEN_FD];
        return;
    }
    if (req->op == AAR_OP_FILE_STAT && path_is(req, CONF_FILE_STALL_C) && !s->stat_seen) {
        s->stat_seen = 1;
        s->stat_tick = conf_ticks();
        return;
    }
    if (req->op == AAR_OP_FILE_OPEN) {
        s->opens++;
        if (s->got_a && req->arg[AAR_A_FILE_OPEN_FD] == s->fd_a)
            s->collision = 1;
        complete(s, req, AAR_OK);
        return;
    }
    if (req->op == AAR_OP_FILE_CLOSE) {
        complete(s, req, AAR_OK);
        return;
    }
    complete(s, req, AAR_E_NOSYS);
}

static void stall_message(struct stall *s, const AARMessage *msg)
{
    AARMessage go;
    ULONG step;

    if (memcmp(msg->bytes, CONF_FILE_STALL, strlen(CONF_FILE_STALL)) != 0)
        return;
    step = conf_file_get32(msg->bytes, 0);
    switch (step) {
    case 1:
        s->step1 = 1;
        s->rc1 = (LONG)conf_file_get32(msg->bytes, 1);
        s->ms1 = conf_file_get32(msg->bytes, 2);
        /* The app has given up the open: complete it late, so that its storage is
         * released, and let the app go on. */
        if (s->got_a)
            complete(s, &s->withheld, AAR_E_IO);
        conf_file_build(go.bytes, CONF_FILE_GO, 0, 0, 0, 0);
        (void)AARSendMessage(s->h, &go);
        break;
    case 2:
        s->step2 = 1;
        s->bad2 = (LONG)conf_file_get32(msg->bytes, 1);
        s->rounds = conf_file_get32(msg->bytes, 3);
        break;
    case 4:
        s->step4 = 1;
        s->rc4 = (LONG)conf_file_get32(msg->bytes, 1);
        s->ms4 = conf_file_get32(msg->bytes, 2);
        break;
    default:
        break;
    }
}

static void stall_fail_all(const char *why)
{
    conf_record("C-FILE.26", 0, why);
    conf_record("C-FILE.27", 0, why);
    conf_record("C-FILE.28", 0, why);
}

static void stall_judge(const struct stall *s)
{
    char d[CONF_DETAIL_MAX];

    if (!s->step1) {
        conf_record("C-FILE.26", 0, "the app never reported the withheld open");
    } else if (s->rc1 != AAR_E_TIMEDOUT) {
        conf_detailf(d, "the withheld open returned %ld after %lu ms, not AAR_E_TIMEDOUT",
                     (long)s->rc1, (unsigned long)s->ms1);
        conf_record("C-FILE.26", 0, d);
    } else if (s->ms1 < CONF_FILE_TIMEOUT_MIN_MS || s->ms1 > CONF_FILE_TIMEOUT_MAX_MS) {
        conf_detailf(d, "AAR_E_TIMEDOUT after %lu ms, not about 30 s", (unsigned long)s->ms1);
        conf_record("C-FILE.26", 0, d);
    } else {
        conf_detailf(d, "AAR_E_TIMEDOUT after %lu ms", (unsigned long)s->ms1);
        conf_record("C-FILE.26", 1, d);
    }

    if (!s->got_a || !s->step1) {
        conf_record("C-FILE.27", 0, "no timed-out open to check the file id of");
    } else if (!s->step2) {
        conf_record("C-FILE.27", 0, "the app never reported the opens after the timeout");
    } else if (s->bad2 != 0 || s->rounds != CONF_FILE_STALL_ROUNDS) {
        conf_detailf(d, "after the timeout and its late completion an open gave %ld "
                     "(%lu of %lu rounds)", (long)s->bad2, (unsigned long)s->rounds,
                     (unsigned long)CONF_FILE_STALL_ROUNDS);
        conf_record("C-FILE.27", 0, d);
    } else if (s->collision) {
        conf_detailf(d, "file id %lu timed out and was handed out again", (unsigned long)s->fd_a);
        conf_record("C-FILE.27", 0, d);
    } else {
        conf_detailf(d, "id %lu timed out and was not handed out in %lu later opens",
                     (unsigned long)s->fd_a, (unsigned long)s->opens);
        conf_record("C-FILE.27", 1, d);
    }

    if (!s->stat_seen) {
        conf_record("C-FILE.28", 0, "the request to withhold never arrived");
    } else if (!s->step4) {
        conf_record("C-FILE.28", 0, "the app never reported the result of the call the stop ended");
    } else if (s->rc4 != AAR_E_IO) {
        conf_detailf(d, "the file wait ended with %ld after %lu ms, not AAR_E_IO",
                     (long)s->rc4, (unsigned long)s->ms4);
        conf_record("C-FILE.28", 0, d);
    } else if (s->ms4 < 500UL) {
        conf_detailf(d, "AAR_E_IO after %lu ms: the call did not wait for the client",
                     (unsigned long)s->ms4);
        conf_record("C-FILE.28", 0, d);
    } else {
        conf_detailf(d, "the stop ended a file wait with AAR_E_IO after %lu ms",
                     (unsigned long)s->ms4);
        conf_record("C-FILE.28", 1, d);
    }
}

static void stall_run(void)
{
    static struct stall s;
    char path[256];
    const char *argv[3];
    AARHandle *h = NULL;
    AARServiceReq req;
    AARMessage msg;
    AARStatusInfo st;
    ULONG reason = 0;
    ULONG start;
    LONG rc;
    int ended = 0;

    memset(&s, 0, sizeof s);
    snprintf(path, sizeof path, "%s", conf_g.dir);
    if (!AddPart((STRPTR)path, (CONST_STRPTR)conf_g.profile_name, sizeof path) ||
        !AddPart((STRPTR)path, (CONST_STRPTR)"conf_file.elf", sizeof path)) {
        stall_fail_all("payload path too long");
        return;
    }
    argv[0] = "conf_file.elf";
    argv[1] = "conf:peer=1";
    argv[2] = "conf:variant=" CONF_FILE_VARIANT_STALL;
    rc = AAROpen(&h, NULL, &reason);
    if (rc != AAR_OK) {
        char d[CONF_DETAIL_MAX];

        conf_detailf(d, "AAROpen returned %ld (reason %lu)", (long)rc, (unsigned long)reason);
        stall_fail_all(d);
        return;
    }
    s.h = h;
    rc = AARLoadFile(h, path);
    if (rc == AAR_OK)
        rc = AARStart(h, (const char *const *)argv, 3UL);
    if (rc != AAR_OK) {
        char d[CONF_DETAIL_MAX];

        conf_detailf(d, "load or start of the stall payload refused: %ld", (long)rc);
        stall_fail_all(d);
        (void)AARClose(h);
        return;
    }
    conf_emitf("# running conf_file variant stall (takes about 35 s)");
    start = conf_ticks();
    for (;;) {
        int progress = 0;
        ULONG now = conf_ticks();

        memset(&req, 0, sizeof req);
        req.size = sizeof req;
        if (AARServiceNext(h, &req) == AAR_OK) {
            stall_request(&s, &req);
            progress = 1;
        }
        if (AARGetMessage(h, &msg) == AAR_OK) {
            stall_message(&s, &msg);
            progress = 1;
        }
        if (s.stat_seen && !s.stopped && (LONG)(now - s.stat_tick) >= (LONG)CONF_TICKS_PER_SECOND) {
            (void)AARStop(h);
            s.stopped = 1;
        }
        memset(&st, 0, sizeof st);
        st.size = 16;
        rc = AARStatus(h, &st);
        if (rc != AAR_OK || st.end_reason != AAR_END_NONE) {
            ended = 1;
            break;
        }
        if ((LONG)(now - start) > (LONG)(100UL * CONF_TICKS_PER_SECOND)) {
            (void)AARStop(h);
            break;
        }
        if (CheckSignal(SIGBREAKF_CTRL_C)) {
            conf_g.aborted = 1;
            (void)AARStop(h);
            break;
        }
        if (!progress)
            Delay(1);
    }
    /* What the app sent before it ended stays readable. */
    while (AARGetMessage(h, &msg) == AAR_OK)
        stall_message(&s, &msg);
    (void)AARClose(h);
    if (!ended) {
        stall_fail_all("the stall payload did not end in 100 s");
        return;
    }
    stall_judge(&s);
}

/* One run judges the three ids; the later calls find their results set. */
static void t_stall(int idx)
{
    (void)idx;
    stall_run();
}

/* ---- C-FILE.31: what an exiting app left open ------------------------------------------------- */

static int rename_ok(const char *from, const char *to)
{
    return Rename((STRPTR)from, (STRPTR)to) != 0;
}

static void t_leak(int idx)
{
    struct conf_run run;
    struct conf_outcome out;
    char a[256], a2[256], b[256], l[256], l2[256], d5[256], d5x[256];
    char why[CONF_DETAIL_MAX];

    if (!prepare_fixtures()) {
        conf_set_result(idx, CONF_R_FAIL, "the fixture tree could not be prepared");
        return;
    }
    conf_run_init(&run, "C-FILE");
    run.variant = CONF_FILE_VARIANT_LEAK;
    run.norelay = 1;
    run.timeout_s = 60;
    conf_run_payload(&run, &out);
    conf_run_free(&run);
    if (!out.ran) {
        conf_detailf(why, "the leak payload did not run: %.80s", out.why);
        conf_set_result(idx, CONF_R_FAIL, why);
        return;
    }
    if (!fx_path(a, sizeof a, "leak-a.tmp") || !fx_path(a2, sizeof a2, "leak-a2.tmp") ||
        !fx_path(b, sizeof b, "leak-b.tmp") || !fx_path(l, sizeof l, CONF_FILE_LEAKDIR_NAME) ||
        !fx_path(l2, sizeof l2, "leakdir2") || !fx_path(d5, sizeof d5, CONF_FILE_DIR5_NAME) ||
        !fx_path(d5x, sizeof d5x, "dir5x")) {
        conf_set_result(idx, CONF_R_FAIL, "a path is too long");
        return;
    }
    if (!rename_ok(a, a2)) {
        conf_detailf(why, "rename of a file the app left open failed (dos error %ld)",
                     (long)IoErr());
    } else if (!DeleteFile((STRPTR)a2)) {
        conf_detailf(why, "delete of a file the app left open failed (dos error %ld)",
                     (long)IoErr());
    } else if (!DeleteFile((STRPTR)b)) {
        conf_detailf(why, "delete of the second file the app left open failed (dos error %ld)",
                     (long)IoErr());
    } else if (!rename_ok(l, l2)) {
        conf_detailf(why, "rename of a directory the app left a list open on failed (dos error %ld)",
                     (long)IoErr());
    } else if (!DeleteFile((STRPTR)l2)) {
        conf_detailf(why, "delete of that directory failed (dos error %ld)", (long)IoErr());
    } else if (!rename_ok(d5, d5x)) {
        conf_detailf(why, "rename of the second directory with an open list failed (dos error %ld)",
                     (long)IoErr());
    } else {
        (void)rename_ok(d5x, d5);
        conf_set_result(idx, CONF_R_PASS,
                        "two open files and two open lists left, all deleted or renamed after the exit");
        return;
    }
    (void)rename_ok(d5x, d5);
    conf_set_result(idx, CONF_R_FAIL, why);
}

static const struct conf_r_test tests[] = {
    { "C-FILE.26", t_stall },
    { "C-FILE.27", t_stall },
    { "C-FILE.28", t_stall },
    { "C-FILE.31", t_leak },
};

const struct conf_area conf_area_file = {
    .name = "C-FILE",
    .tests = tests,
    .test_count = CONF_COUNT(tests),
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
    .on_end = on_end,
};
