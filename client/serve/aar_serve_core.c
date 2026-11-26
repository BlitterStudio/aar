/*
 * The OS-free core of aar_serve: the request handlers behind the operation tables.
 * The Amiga backends, the loop and the public calls are in aar_serve.c. The host
 * tests compile this file alone, against fake tables.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "aar_serve_core.h"

int32_t aar_serve_errno_from_io(int32_t io_error)
{
    switch (io_error) {
    case 0:
        return AAR_OK;
    case AAR_SERVE_IOERR_OBJECT_NOT_FOUND:
    case AAR_SERVE_IOERR_DIR_NOT_FOUND:
    case AAR_SERVE_IOERR_DEVICE_NOT_MOUNTED:
    case AAR_SERVE_IOERR_NO_MORE_ENTRIES:
        return AAR_E_NOENT;
    case AAR_SERVE_IOERR_DISK_FULL:
        return AAR_E_NOSPC;
    case AAR_SERVE_IOERR_WRITE_PROTECTED:
    case AAR_SERVE_IOERR_READ_PROTECTED:
    case AAR_SERVE_IOERR_DELETE_PROTECTED:
        return AAR_E_ACCES;
    case AAR_SERVE_IOERR_OBJECT_EXISTS:
        return AAR_E_EXIST;
    case AAR_SERVE_IOERR_NO_FREE_STORE:
        return AAR_E_NOMEM;
    case AAR_SERVE_IOERR_OBJECT_IN_USE:
        return AAR_E_BUSY;
    case AAR_SERVE_IOERR_DIR_NOT_EMPTY:
        return AAR_E_EXIST;
    case AAR_SERVE_IOERR_RENAME_ACROSS_DEVICES:
        return AAR_E_INVAL;
    case AAR_SERVE_IOERR_SEEK_ERROR:
        return AAR_E_INVAL;
    default:
        return AAR_E_IO;
    }
}

uint32_t aar_serve_amiga_unix(uint32_t days, uint32_t minutes, uint32_t ticks)
{
    if (days == 0 && minutes == 0 && ticks == 0)
        return 0;
    return (uint32_t)AAR_SERVE_UNIX_DELTA + days * 86400u + minutes * 60u +
           ticks / 50u;
}

int64_t aar_serve_utc_from_local(uint32_t local_unix, int32_t east_min)
{
    return (int64_t)local_unix - (int64_t)(east_min * 60);
}

int aar_serve_amiga_from_utc(int64_t utc, int32_t east_min, uint32_t *days,
                             uint32_t *minutes, uint32_t *ticks)
{
    int64_t local = utc + (int64_t)(east_min * 60) - (int64_t)AAR_SERVE_UNIX_DELTA;
    uint32_t secs;

    /* A 32-bit count of seconds, as the Unix times of this file: until 2106. */
    if (local < 0 || local > 0xffffffffLL)
        return 0;
    secs = (uint32_t)local;
    *days = secs / 86400u;
    *minutes = secs % 86400u / 60u;
    *ticks = secs % 60u * 50u;
    return 1;
}

static void done_clear(struct aar_serve_done *done)
{
    memset(done, 0, sizeof(*done));
}

static void done_result(struct aar_serve_done *done, int32_t result)
{
    done_clear(done);
    done->result = result;
}

/* ---- Files -------------------------------------------------------------- */

void aar_serve_files_init(struct aar_serve_files *files, const struct aar_serve_dos_ops *dos,
                          uintptr_t root, int abs_paths)
{
    memset(files, 0, sizeof(*files));
    files->dos = dos;
    files->root = root;
    files->abs_paths = abs_paths;
}

void aar_serve_files_close_all(struct aar_serve_files *f)
{
    unsigned i;

    for (i = 0; i < AAR_SERVE_FD_COUNT; i++) {
        if (f->fh[i]) {
            f->dos->fh_end_or_close(f->dos->ctx, f->fh[i]);
            f->fh[i] = 0;
            f->fh_write[i] = 0;
        }
        if (f->list[i]) {
            f->dos->list_close(f->dos->ctx, f->list[i]);
            f->list[i] = 0;
        }
    }
}

static int slot_index(int32_t id, int32_t *err)
{
    if (id < AAR_FD_BASE || id >= AAR_FD_BASE + (int32_t)AAR_SERVE_FD_COUNT) {
        *err = AAR_E_BADF;
        return -1;
    }
    return (int)(id - AAR_FD_BASE);
}

/* The path of a request: NUL terminated inside the payload. */
static int path_from_req(const struct aar_serve_req *req, char *out, int32_t *err)
{
    uint32_t i;

    if (!req->buf || req->buf_len == 0) {
        *err = AAR_E_INVAL;
        return 0;
    }
    for (i = 0; i < AAR_SERVE_PATH_BYTES && i < req->buf_len; i++) {
        out[i] = (char)req->buf[i];
        if (out[i] == 0)
            return 1;
    }
    *err = AAR_E_INVAL;
    return 0;
}

static int under_root(const struct aar_serve_files *f, uintptr_t lock)
{
    const struct aar_serve_dos_ops *d = f->dos;
    uintptr_t cur = d->dup_lock(d->ctx, lock);

    while (cur) {
        uintptr_t parent;

        if (d->same_lock(d->ctx, cur, f->root)) {
            d->unlock(d->ctx, cur);
            return 1;
        }
        parent = d->parent_dir(d->ctx, cur);
        d->unlock(d->ctx, cur);
        cur = parent;
    }
    return 0;
}

static int step_parent(const struct aar_serve_files *f, uintptr_t *cur,
                       int32_t *err)
{
    const struct aar_serve_dos_ops *d = f->dos;
    uintptr_t parent = d->parent_dir(d->ctx, *cur);

    if (!parent || !under_root(f, parent)) {
        if (parent)
            d->unlock(d->ctx, parent);
        *err = AAR_E_ACCES;
        return 0;
    }
    d->unlock(d->ctx, *cur);
    *cur = parent;
    return 1;
}

static int enter_dir(const struct aar_serve_files *f, uintptr_t *cur,
                     const char *name, int32_t *err)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int transport_ok = 0;
    int32_t io = 0;
    uintptr_t child = d->lock_rel(d->ctx, *cur, name, &transport_ok, &io);

    if (!transport_ok) {
        *err = AAR_E_IO;
        return 0;
    }
    if (!child) {
        *err = io == AAR_SERVE_IOERR_OBJECT_NOT_FOUND ||
               io == AAR_SERVE_IOERR_DIR_NOT_FOUND ? AAR_E_NOENT : AAR_E_IO;
        return 0;
    }
    if (!under_root(f, child)) {
        d->unlock(d->ctx, child);
        *err = AAR_E_ACCES;
        return 0;
    }
    d->unlock(d->ctx, *cur);
    *cur = child;
    return 1;
}

/* A slash after a name separates the next component. A slash with no name (a
 * leading slash, or two slashes) is the parent. `..` is parent. A single
 * trailing slash is only the separator. */
static int next_comp(const char **pp, char *out, size_t n, int *parent,
                     int *end)
{
    const char *p = *pp;
    size_t i = 0;

    *parent = 0;
    *end = 0;
    if (*p == 0) {
        *end = 1;
        return 1;
    }
    if (*p == '/') {
        *parent = 1;
        *pp = p + 1;
        return 1;
    }
    while (*p && *p != '/') {
        if (i + 1 >= n)
            return 0;
        out[i++] = *p++;
    }
    out[i] = 0;
    if (*p == '/')
        p++;
    *pp = p;
    return 1;
}

/* Walks path from the ROOT, never above it. Returns the lock of the leaf's
 * parent directory and the leaf name, or 0 with *err. */
static uintptr_t resolve_rel(const struct aar_serve_files *f, const char *path,
                             char *leaf, size_t leaf_n, int32_t *err)
{
    const struct aar_serve_dos_ops *d = f->dos;
    const char *p = path;
    uintptr_t cur = d->dup_lock(d->ctx, f->root);
    char comp[108];
    char hold[108];
    int have_hold = 0;

    hold[0] = 0;
    leaf[0] = 0;
    if (!cur) {
        *err = AAR_E_IO;
        return 0;
    }
    for (;;) {
        int parent = 0, end = 0;

        comp[0] = 0;
        if (!next_comp(&p, comp, sizeof(comp), &parent, &end)) {
            *err = AAR_E_INVAL;
            d->unlock(d->ctx, cur);
            return 0;
        }
        if (end)
            break;
        if (parent || strcmp(comp, "..") == 0) {
            if (have_hold) {
                if (!enter_dir(f, &cur, hold, err)) {
                    d->unlock(d->ctx, cur);
                    return 0;
                }
                have_hold = 0;
            }
            if (!step_parent(f, &cur, err)) {
                d->unlock(d->ctx, cur);
                return 0;
            }
            continue;
        }
        if (strcmp(comp, ".") == 0)
            continue;
        if (have_hold) {
            if (!enter_dir(f, &cur, hold, err)) {
                d->unlock(d->ctx, cur);
                return 0;
            }
        }
        memcpy(hold, comp, strlen(comp) + 1);
        have_hold = 1;
    }
    if (have_hold) {
        if (strlen(hold) >= leaf_n) {
            *err = AAR_E_INVAL;
            d->unlock(d->ctx, cur);
            return 0;
        }
        memcpy(leaf, hold, strlen(hold) + 1);
    }
    return cur;
}

/* An error for a failed lock: the DOS error, or ENOENT when there is none. */
static int32_t lock_errno(int32_t io)
{
    int32_t err = aar_serve_errno_from_io(io);

    return err == AAR_OK ? AAR_E_NOENT : err;
}

static int lock_target(const struct aar_serve_files *f, uintptr_t dir,
                       const char *leaf, uintptr_t *out, int32_t *err)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int transport_ok = 0;
    int32_t io = 0;
    uintptr_t locked;

    if (!leaf[0]) {
        *out = d->dup_lock(d->ctx, dir);
        if (!*out) {
            *err = AAR_E_IO;
            return 0;
        }
        return 1;
    }
    locked = d->lock_rel(d->ctx, dir, leaf, &transport_ok, &io);
    if (!transport_ok) {
        *err = AAR_E_IO;
        return 0;
    }
    if (!locked) {
        *err = lock_errno(io);
        return 0;
    }
    if (!under_root(f, locked)) {
        d->unlock(d->ctx, locked);
        *err = AAR_E_ACCES;
        return 0;
    }
    *out = locked;
    return 1;
}

static int final_path_under_root(const struct aar_serve_files *f, uintptr_t dir,
                                 const char *leaf, enum aar_serve_open_plan plan,
                                 int32_t *err)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int transport_ok = 0;
    int32_t io = 0;
    uintptr_t locked = d->lock_rel(d->ctx, dir, leaf, &transport_ok, &io);

    if (!transport_ok) {
        *err = AAR_E_IO;
        return 0;
    }
    if (!locked) {
        switch (aar_serve_leaf_lock_failure(plan, io, AAR_SERVE_IOERR_OBJECT_NOT_FOUND,
                                            AAR_SERVE_IOERR_OBJECT_IN_USE)) {
        case AAR_SERVE_LEAF_PROCEED:
            return 1;
        case AAR_SERVE_LEAF_EXISTS:
            *err = AAR_E_EXIST;
            return 0;
        default:
            break;
        }
        *err = lock_errno(io);
        return 0;
    }
    if (!under_root(f, locked)) {
        d->unlock(d->ctx, locked);
        *err = AAR_E_ACCES;
        return 0;
    }
    d->unlock(d->ctx, locked);
    return 1;
}

/* A step that creates, truncates, replaces or writes a file runs only while
 * its request is live and no stop is under way. */
static int open_step_allowed(const struct aar_serve_dos_ops *d)
{
    return aar_serve_mutation_allowed(d->live(d->ctx), d->stopping(d->ctx));
}

/* Every open that can create or replace a file (anything but MODE_OLDFILE)
 * checks that the request is still live before its packet goes out. */
static uintptr_t open_target(const struct aar_serve_dos_ops *d, enum aar_serve_mode mode,
                             int *transport_ok, int32_t *io)
{
    if (mode != AAR_SERVE_MODE_OLD && !open_step_allowed(d)) {
        *transport_ok = 0;
        *io = AAR_SERVE_IOERR_BREAK;
        return 0;
    }
    return d->target_open(d->ctx, mode, transport_ok, io);
}

/* Opens one file for the flags word's plan. Returns 1 with *fh_out set, or 0
 * with *err set (still AAR_OK means the DOS error *io says why).
 *
 * AmigaDOS has no atomic exclusive create. EXCL is a Lock probe followed by
 * MODE_NEWFILE, so it is exclusive against the other apps aar_serve serves (one
 * serving loop handles every file request) but not against other Amiga
 * programs that create the same name between the probe and the open. */
static int open_with_plan(const struct aar_serve_files *f,
                          enum aar_serve_open_plan plan, uintptr_t dir,
                          const char *name, int absolute, uintptr_t *fh_out,
                          int32_t *err, int32_t *io)
{
    const struct aar_serve_dos_ops *d = f->dos;
    uintptr_t fh = 0;
    int transport_ok = 0;
    int exists = 0;
    int ok = 0;

    *fh_out = 0;
    *io = 0;
    d->target_begin(d->ctx, absolute ? 0 : dir, name);
    if (aar_serve_plan_probes_target(plan, absolute)) {
        if (!d->target_probe(d->ctx, &exists, &transport_ok, io)) {
            if (!transport_ok) {
                *err = AAR_E_IO;
            } else {
                *err = lock_errno(*io);
            }
            goto out;
        }
    }
    switch (plan) {
    case AAR_SERVE_OPEN_READ_EXISTING:
        fh = open_target(d, AAR_SERVE_MODE_OLD, &transport_ok, io);
        break;
    case AAR_SERVE_OPEN_UPDATE_EXISTING:
    case AAR_SERVE_OPEN_TRUNCATE_EXISTING:
        if (!exists) {
            *err = AAR_E_NOENT;
            goto out;
        }
        fh = open_target(d, AAR_SERVE_MODE_READWRITE, &transport_ok, io);
        if (fh && plan == AAR_SERVE_OPEN_TRUNCATE_EXISTING) {
            int32_t size;
            int size_ok = 0;
            int32_t size_io = 0;

            if (!open_step_allowed(d)) {
                d->fh_end_or_close(d->ctx, fh);
                *err = AAR_E_IO;
                goto out;
            }
            size = d->fh_set_size(d->ctx, fh, 0, &size_ok, &size_io);
            if (!size_ok) {
                d->fh_end_or_close(d->ctx, fh);
                *err = AAR_E_IO;
                goto out;
            }
            if (size > 0) {
                /* Another handle's position kept the file from being emptied
                 * (SetFileSize answers the size it left): not a truncation. */
                d->fh_end_or_close(d->ctx, fh);
                *err = AAR_E_BUSY;
                goto out;
            }
            if (size < 0) {
                d->fh_end_or_close(d->ctx, fh);
                fh = 0;
                if (size_io != AAR_SERVE_IOERR_ACTION_NOT_KNOWN) {
                    *err = size_io == AAR_SERVE_IOERR_DISK_FULL ? AAR_E_NOSPC :
                           size_io == AAR_SERVE_IOERR_WRITE_PROTECTED ?
                           AAR_E_ACCES : AAR_E_IO;
                    goto out;
                }
                /* No ACTION_SET_FILE_SIZE: the probe found the file, so
                 * MODE_NEWFILE replaces it, on the probe's candidate. */
                fh = open_target(d, AAR_SERVE_MODE_NEW, &transport_ok, io);
            }
        }
        break;
    case AAR_SERVE_OPEN_CREATE_KEEP:
        fh = open_target(d, AAR_SERVE_MODE_READWRITE, &transport_ok, io);
        if (!fh && transport_ok && *io == AAR_SERVE_IOERR_OBJECT_NOT_FOUND)
            fh = open_target(d, AAR_SERVE_MODE_NEW, &transport_ok, io);
        break;
    case AAR_SERVE_OPEN_CREATE_REPLACE:
        fh = open_target(d, AAR_SERVE_MODE_NEW, &transport_ok, io);
        break;
    case AAR_SERVE_OPEN_CREATE_EXCLUSIVE:
        if (exists) {
            *err = AAR_E_EXIST;
            goto out;
        }
        fh = open_target(d, AAR_SERVE_MODE_NEW, &transport_ok, io);
        break;
    default:
        *err = AAR_E_INVAL;
        goto out;
    }
    if (!transport_ok) {
        *err = AAR_E_IO;
        goto out;
    }
    *fh_out = fh;
    ok = fh != 0;
out:
    d->target_end(d->ctx);
    return ok;
}

static int32_t serve_open(struct aar_serve_files *f, const struct aar_serve_req *req)
{
    const struct aar_serve_dos_ops *d = f->dos;
    char path[AAR_SERVE_PATH_BYTES];
    char leaf[108];
    uintptr_t dir = 0;
    uintptr_t fh = 0;
    enum aar_serve_open_plan plan;
    int32_t err = AAR_OK;
    int32_t io = 0;
    int index;
    int absolute = 0;

    index = slot_index((int32_t)req->arg[AAR_A_FILE_OPEN_FD], &err);
    if (index < 0)
        return err;
    if (f->fh[index])
        return AAR_E_BUSY;
    plan = aar_serve_open_plan_for_flags(req->arg[AAR_A_FILE_OPEN_FLAGS]);
    if (plan == AAR_SERVE_OPEN_INVALID)
        return AAR_E_INVAL;
    if (!path_from_req(req, path, &err))
        return err;
    if (path[0] == 0)
        return AAR_E_INVAL;
    if (strchr(path, ':')) {
        if (!f->abs_paths)
            return AAR_E_ACCES;
        absolute = 1;
    }
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!absolute) {
        dir = resolve_rel(f, path, leaf, sizeof(leaf), &err);
        if (!dir)
            return err;
        if (!leaf[0]) {
            err = AAR_E_IO;
            goto done;
        }
        if (!final_path_under_root(f, dir, leaf, plan, &err))
            goto done;
    }
    if (!open_with_plan(f, plan, dir, absolute ? path : leaf, absolute, &fh,
                        &err, &io) &&
        err == AAR_OK)
        err = lock_errno(io);
    if (!fh)
        goto done;
    if (!d->live(d->ctx)) {
        err = AAR_E_IO;
        goto done;
    }
    f->fh[index] = fh;
    f->fh_write[index] = (req->arg[AAR_A_FILE_OPEN_FLAGS] & AAR_FILE_WRITE) != 0;
    fh = 0;
done:
    if (fh)
        d->fh_end_or_close(d->ctx, fh);
    if (dir)
        d->unlock(d->ctx, dir);
    return err;
}

static int32_t serve_close(struct aar_serve_files *f, const struct aar_serve_req *req)
{
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_FD], &err);
    int delivered = 0;
    int32_t io = 0;

    if (index < 0 || !f->fh[index])
        return AAR_E_BADF;
    if (!f->dos->fh_close(f->dos->ctx, f->fh[index], &delivered, &io))
        err = delivered ? lock_errno(io) : AAR_E_IO;
    if (delivered) {
        f->fh[index] = 0;
        f->fh_write[index] = 0;
    }
    return err;
}

static int32_t serve_read(struct aar_serve_files *f, const struct aar_serve_req *req,
                          struct aar_serve_done *done)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_FD], &err);
    uint32_t left = req->arg[AAR_A_FILE_LEN];
    uint32_t got = 0;

    if (index < 0 || !f->fh[index])
        return AAR_E_BADF;
    if (left > req->buf_len || (left && !req->buf))
        return AAR_E_INVAL;
    while (left) {
        uint32_t chunk = left > AAR_SERVE_IO_CHUNK ? AAR_SERVE_IO_CHUNK : left;
        int transport_ok = 0;
        int32_t io = 0;
        int32_t n;

        if (!d->live(d->ctx)) {
            err = AAR_E_IO;
            break;
        }
        if (d->user_break(d->ctx))
            break;
        n = d->fh_read(d->ctx, f->fh[index], f->io, chunk, &transport_ok, &io);
        if (!transport_ok) {
            err = AAR_E_IO;
            break;
        }
        if (n < 0) {
            err = lock_errno(io);
            break;
        }
        if (n == 0)
            break;
        memcpy(req->buf + got, f->io, (uint32_t)n);
        got += (uint32_t)n;
        left -= (uint32_t)n;
        if ((uint32_t)n < chunk)
            break;
    }
    done->out[AAR_R_FILE_COUNT] = got;
    return err;
}

static int32_t serve_write(struct aar_serve_files *f, const struct aar_serve_req *req,
                           struct aar_serve_done *done)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_FD], &err);
    uint32_t left = req->arg[AAR_A_FILE_LEN];
    uint32_t put = 0;

    if (index < 0 || !f->fh[index])
        return AAR_E_BADF;
    if (left > req->buf_len || (left && !req->buf))
        return AAR_E_INVAL;
    while (left) {
        uint32_t chunk = left > AAR_SERVE_IO_CHUNK ? AAR_SERVE_IO_CHUNK : left;
        int transport_ok = 0;
        int32_t io = 0;
        int32_t n;

        if (!open_step_allowed(d)) {
            err = AAR_E_IO;
            break;
        }
        memcpy(f->io, req->buf + put, chunk);
        n = d->fh_write(d->ctx, f->fh[index], f->io, chunk, &transport_ok,
                        &io);
        if (!transport_ok) {
            err = AAR_E_IO;
            break;
        }
        if (n < 0) {
            err = lock_errno(io);
            break;
        }
        if (n == 0) {
            err = AAR_E_NOSPC;
            break;
        }
        put += (uint32_t)n;
        left -= (uint32_t)n;
        if ((uint32_t)n < chunk)
            break;
    }
    done->out[AAR_R_FILE_COUNT] = put;
    return err;
}

/* The current position and the size of an open file. ACTION_SEEK answers with
 * the old position, so the position is seek(0, CUR) and the size is
 * seek(0, END) followed by seek(cur, BEGINNING), which answers with the end
 * and leaves the position where it was. *size is read only when want_size. */
static int32_t seek_bounds(struct aar_serve_files *f, uintptr_t fh,
                           int want_size, int32_t *cur, int32_t *size,
                           int32_t *io)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int transport_ok = 0;

    *cur = d->fh_seek(d->ctx, fh, 0, 1, &transport_ok, io);
    if (!transport_ok)
        return AAR_E_IO;
    if (*cur < 0)
        return aar_serve_errno_from_io(*io) == AAR_OK ?
               AAR_E_IO : aar_serve_errno_from_io(*io);
    if (!want_size)
        return AAR_OK;
    if (d->fh_seek(d->ctx, fh, 0, 2, &transport_ok, io) < 0)
        return AAR_E_IO;
    *size = d->fh_seek(d->ctx, fh, *cur, 0, &transport_ok, io);
    if (!transport_ok || *size < 0)
        return AAR_E_IO;
    return AAR_OK;
}

/* A position before the start is AAR_E_INVAL (spec 7.1) and leaves the
 * position unchanged. Handlers do not agree on it: FFS refuses, but
 * ram-handler accepts the seek and leaves the handle at a negative position,
 * which no later call can tell from an error. So a seek that goes backwards
 * from the current position or the end is checked here, before it reaches the
 * handler. Returns AAR_OK when the seek may go on. */
static int32_t seek_precheck(struct aar_serve_files *f, uintptr_t fh, int mode,
                             int32_t offset)
{
    int32_t cur = 0;
    int32_t size = 0;
    int32_t io = 0;
    int32_t err;

    if (offset >= 0 || mode == 0)
        return AAR_OK;
    err = seek_bounds(f, fh, mode == 2, &cur, &size, &io);
    if (err != AAR_OK)
        return err;
    if ((int64_t)(mode == 1 ? cur : size) + offset < 0)
        return AAR_E_INVAL;
    return AAR_OK;
}

/* Why a handler refused a seek whose target is not before the start. Past the
 * end is AAR_E_INVAL whatever Res2 the handler leaves (FFS and ram-handler give
 * ERROR_SEEK_ERROR, others none). A refusal for a target inside the file is a
 * real error: the handler's own code, or AAR_E_IO when it gave none. */
static int32_t seek_refused(struct aar_serve_files *f, uintptr_t fh, int mode,
                            int64_t offset, int32_t io)
{
    int32_t refused = aar_serve_errno_from_io(io);
    int32_t cur = 0;
    int32_t size = 0;
    int32_t err;
    int64_t base;

    if (refused == AAR_OK)
        refused = AAR_E_IO;
    err = seek_bounds(f, fh, 1, &cur, &size, &io);
    if (err != AAR_OK)
        return err;
    base = mode == 0 ? 0 : (mode == 1 ? (int64_t)cur : (int64_t)size);
    if (base + offset < 0 || base + offset > (int64_t)size)
        return AAR_E_INVAL;
    return refused;
}

static int32_t serve_seek(struct aar_serve_files *f, const struct aar_serve_req *req,
                          struct aar_serve_done *done)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_FD], &err);
    uint32_t whence = req->arg[AAR_A_FILE_WHENCE];
    uint64_t offset = ((uint64_t)req->arg[AAR_A_FILE_OFF_HI] << 32) |
                      req->arg[AAR_A_FILE_OFF_LO];
    int64_t target = (int64_t)offset;
    int mode;
    int transport_ok = 0;
    int32_t io = 0;
    int32_t pos;
    int32_t current;

    if (index < 0 || !f->fh[index])
        return AAR_E_BADF;
    if (whence == (uint32_t)AAR_SEEK_SET) {
        if (offset > 0x7fffffffULL)
            return AAR_E_INVAL;
        mode = 0;
    } else if (whence == (uint32_t)AAR_SEEK_CUR) {
        mode = 1;
    } else if (whence == (uint32_t)AAR_SEEK_END) {
        mode = 2;
    } else {
        return AAR_E_INVAL;
    }
    if (target > 2147483647LL || target < -2147483647LL)
        return AAR_E_INVAL;
    err = seek_precheck(f, f->fh[index], mode, (int32_t)target);
    if (err != AAR_OK)
        return err;
    pos = d->fh_seek(d->ctx, f->fh[index], (int32_t)target, mode,
                     &transport_ok, &io);
    if (!transport_ok)
        return AAR_E_IO;
    if (pos < 0)
        return seek_refused(f, f->fh[index], mode, target, io);
    current = d->fh_seek(d->ctx, f->fh[index], 0, 1, &transport_ok, &io);
    if (!transport_ok)
        return AAR_E_IO;
    if (current < 0) {
        err = aar_serve_errno_from_io(io);
        return err == AAR_OK ? AAR_E_IO : err;
    }
    done->out[AAR_R_FILE_POS_HI] = 0;
    done->out[AAR_R_FILE_POS_LO] = (uint32_t)current;
    return AAR_OK;
}

/* The lock a path names for stat and list: an absolute name through the
 * resolver, a relative one confined to the ROOT. *dir is the parent lock
 * the caller unlocks. */
static int lock_named(struct aar_serve_files *f, const char *path,
                      int allow_empty_root, uintptr_t *dir, uintptr_t *locked,
                      int32_t *err)
{
    const struct aar_serve_dos_ops *d = f->dos;
    char leaf[108];
    int transport_ok = 0;
    int32_t io = 0;

    *dir = 0;
    *locked = 0;
    if (strchr(path, ':')) {
        if (!f->abs_paths) {
            *err = AAR_E_ACCES;
            return 0;
        }
        *locked = d->lock_abs(d->ctx, path, &transport_ok, &io);
        if (!transport_ok) {
            *err = AAR_E_IO;
            return 0;
        }
        if (!*locked) {
            *err = aar_serve_errno_from_io(io);
            if (*err == AAR_OK)
                *err = AAR_E_IO;
            return 0;
        }
        return 1;
    }
    if (path[0] == 0 && allow_empty_root) {
        *locked = d->dup_lock(d->ctx, f->root);
        if (!*locked) {
            *err = AAR_E_IO;
            return 0;
        }
        return 1;
    }
    *dir = resolve_rel(f, path, leaf, sizeof(leaf), err);
    if (!*dir)
        return 0;
    if (!lock_target(f, *dir, leaf, locked, err))
        return 0;
    return 1;
}

static int files_utc_offset(const struct aar_serve_files *f, int32_t *east_min);

static int32_t serve_stat(struct aar_serve_files *f, const struct aar_serve_req *req,
                          struct aar_serve_done *done)
{
    const struct aar_serve_dos_ops *d = f->dos;
    char path[AAR_SERVE_PATH_BYTES];
    uintptr_t dir = 0;
    uintptr_t locked = 0;
    struct aar_serve_info info;
    int transport_ok = 0;
    int32_t io = 0;
    int32_t err = AAR_OK;

    if (!path_from_req(req, path, &err))
        return err;
    if (path[0] == 0)
        return AAR_E_INVAL;
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!lock_named(f, path, 0, &dir, &locked, &err))
        goto done;
    if (!d->lock_examine(d->ctx, locked, &info, &transport_ok, &io)) {
        err = transport_ok ? lock_errno(io) : AAR_E_IO;
        goto done;
    }
    {
        int32_t east = 0;
        int64_t utc = 0;

        if (info.mtime) {
            (void)files_utc_offset(f, &east);
            utc = aar_serve_utc_from_local(info.mtime, east);
        }
        done->out[AAR_R_STAT_SIZE_HI] = 0;
        done->out[AAR_R_STAT_SIZE_LO] = info.size;
        done->out[AAR_R_STAT_MTIME_HI] = (uint32_t)((uint64_t)utc >> 32);
        done->out[AAR_R_STAT_MTIME_LO] = (uint32_t)utc;
    }
    done->out[AAR_R_STAT_MODE] = info.is_dir ? AAR_MODE_DIR : AAR_MODE_FILE;
    done->out[AAR_R_STAT_PROTECT] = info.protect & 0xffu;
done:
    if (locked)
        d->unlock(d->ctx, locked);
    if (dir)
        d->unlock(d->ctx, dir);
    return err;
}

static int32_t serve_list_open(struct aar_serve_files *f,
                               const struct aar_serve_req *req)
{
    const struct aar_serve_dos_ops *d = f->dos;
    char path[AAR_SERVE_PATH_BYTES];
    uintptr_t dir = 0;
    uintptr_t locked = 0;
    uintptr_t list = 0;
    int transport_ok = 0;
    int32_t io = 0;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_LIST_ID], &err);

    if (index < 0)
        return err;
    if (f->list[index])
        return AAR_E_BUSY;
    if (!path_from_req(req, path, &err))
        return err;
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!lock_named(f, path, 1, &dir, &locked, &err))
        goto done;
    if (!d->list_open(d->ctx, locked, &list, &transport_ok, &io)) {
        err = transport_ok ? lock_errno(io) : AAR_E_IO;
        goto done;
    }
    /* The list owns the lock now. */
    locked = 0;
    if (!d->live(d->ctx)) {
        d->list_close(d->ctx, list);
        err = AAR_E_IO;
        goto done;
    }
    f->list[index] = list;
done:
    if (locked)
        d->unlock(d->ctx, locked);
    if (dir)
        d->unlock(d->ctx, dir);
    return err;
}

static int32_t serve_list_next(struct aar_serve_files *f,
                               const struct aar_serve_req *req,
                               struct aar_serve_done *done)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_LIST_ID], &err);
    struct aar_serve_info info;
    int transport_ok = 0;
    int32_t io = 0;
    size_t len;

    if (index < 0 || !f->list[index])
        return AAR_E_BADF;
    if (!req->buf || req->buf_len < 2u)
        return AAR_E_INVAL;
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!d->list_next(d->ctx, f->list[index], &info, &transport_ok, &io))
        return transport_ok ? lock_errno(io) : AAR_E_IO;
    len = strlen(info.name);
    if (len > AAR_NAME_MAX)
        len = AAR_NAME_MAX;
    if (len > req->buf_len - 1u)
        len = req->buf_len - 1u;
    memcpy(req->buf, info.name, len);
    req->buf[len] = 0;
    done->out[AAR_R_LIST_MODE] = info.is_dir ? AAR_MODE_DIR : AAR_MODE_FILE;
    return AAR_OK;
}

static int32_t serve_list_close(struct aar_serve_files *f,
                                const struct aar_serve_req *req)
{
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_LIST_ID], &err);

    if (index < 0 || !f->list[index])
        return AAR_E_BADF;
    f->dos->list_close(f->dos->ctx, f->list[index]);
    f->list[index] = 0;
    return AAR_OK;
}

/* ---- Names, dates, sizes and volumes --------------------------------------- */

/* The user's UTC offset in minutes east of UTC; 1 when it is known. */
static int files_utc_offset(const struct aar_serve_files *f, int32_t *east_min)
{
    *east_min = 0;
    return f->dos->utc_offset ? f->dos->utc_offset(f->dos->ctx, east_min) : 0;
}

/* The object a request's path names: a relative path is a parent directory
 * under the ROOT and a leaf name; a path with a volume or assign prefix (with
 * ABS) is resolved by GetDeviceProc. A path that names the ROOT, a directory
 * reached with `..`, or only a volume has no leaf to act on: AAR_E_ACCES. */
struct aar_serve_loc {
    char path[AAR_SERVE_PATH_BYTES];
    char leaf[108];
    uintptr_t dir;
    int absolute;
};

static int loc_resolve(const struct aar_serve_files *f, const char *path,
                       struct aar_serve_loc *l, int32_t *err)
{
    memset(l, 0, sizeof(*l));
    if (path[0] == 0) {
        *err = AAR_E_INVAL;
        return 0;
    }
    memcpy(l->path, path, strlen(path) + 1);
    if (strchr(path, ':')) {
        if (!f->abs_paths || aar_serve_dos_packet_path(path)[0] == 0) {
            *err = AAR_E_ACCES;
            return 0;
        }
        l->absolute = 1;
        return 1;
    }
    l->dir = resolve_rel(f, path, l->leaf, sizeof(l->leaf), err);
    if (!l->dir)
        return 0;
    if (!l->leaf[0]) {
        f->dos->unlock(f->dos->ctx, l->dir);
        l->dir = 0;
        *err = AAR_E_ACCES;
        return 0;
    }
    return 1;
}

static void loc_release(const struct aar_serve_files *f, struct aar_serve_loc *l)
{
    if (l->dir) {
        f->dos->unlock(f->dos->ctx, l->dir);
        l->dir = 0;
    }
}

static const char *loc_name(const struct aar_serve_loc *l)
{
    return l->absolute ? l->path : l->leaf;
}

/* A relative leaf that exists is under the ROOT (a link may lead out of it).
 * What a missing leaf means depends on the plan, as for an open. */
static int loc_confined(const struct aar_serve_files *f, const struct aar_serve_loc *l,
                        enum aar_serve_open_plan plan, int32_t *err)
{
    return l->absolute || final_path_under_root(f, l->dir, l->leaf, plan, err);
}

static uintptr_t loc_lock(const struct aar_serve_files *f, const struct aar_serve_loc *l,
                          int *transport_ok, int32_t *io)
{
    const struct aar_serve_dos_ops *d = f->dos;

    return l->absolute ? d->lock_abs(d->ctx, l->path, transport_ok, io) :
                         d->lock_rel(d->ctx, l->dir, l->leaf, transport_ok, io);
}

/* One packet that names the object, refused like every other change once a stop
 * is under way. */
static int32_t loc_named(const struct aar_serve_files *f, const struct aar_serve_loc *l,
                         enum aar_serve_named kind,
                         const struct aar_serve_named_args *args)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int transport_ok = 0;
    int ok = 0;
    int32_t io = 0;

    d->target_begin(d->ctx, l->absolute ? 0 : l->dir, loc_name(l));
    if (open_step_allowed(d))
        ok = d->target_named(d->ctx, kind, args, &transport_ok, &io);
    d->target_end(d->ctx);
    if (ok)
        return AAR_OK;
    return transport_ok ? lock_errno(io) : AAR_E_IO;
}

/* FILE_DELETE, DIR_CREATE, FILE_SET_DATE and FILE_SET_PROTECT: the path in buf. */
static int32_t serve_named(struct aar_serve_files *f, const struct aar_serve_req *req,
                           enum aar_serve_named kind,
                           const struct aar_serve_named_args *args)
{
    char path[AAR_SERVE_PATH_BYTES];
    struct aar_serve_loc loc;
    struct aar_serve_named_args none;
    int32_t err = AAR_OK;

    if (!args) {
        memset(&none, 0, sizeof(none));
        args = &none;
    }

    if (!f->dos->target_named)
        return AAR_E_NOSYS;
    if (!path_from_req(req, path, &err))
        return err;
    if (!f->dos->live(f->dos->ctx))
        return AAR_E_IO;
    if (!loc_resolve(f, path, &loc, &err))
        return err;
    if (loc_confined(f, &loc,
                     kind == AAR_SERVE_NAMED_CREATE_DIR ? AAR_SERVE_OPEN_CREATE_EXCLUSIVE :
                                                          AAR_SERVE_OPEN_READ_EXISTING,
                     &err))
        err = loc_named(f, &loc, kind, args);
    loc_release(f, &loc);
    return err;
}

static int32_t serve_set_date(struct aar_serve_files *f, const struct aar_serve_req *req)
{
    int64_t mtime = (int64_t)(((uint64_t)req->arg[AAR_A_FILE_DATE_HI] << 32) |
                              req->arg[AAR_A_FILE_DATE_LO]);
    struct aar_serve_named_args args;
    int32_t east = 0;

    memset(&args, 0, sizeof(args));
    (void)files_utc_offset(f, &east);
    if (!aar_serve_amiga_from_utc(mtime, east, &args.days, &args.minutes, &args.ticks))
        return AAR_E_INVAL;
    return serve_named(f, req, AAR_SERVE_NAMED_SET_DATE, &args);
}

static int32_t serve_set_protect(struct aar_serve_files *f, const struct aar_serve_req *req)
{
    struct aar_serve_named_args args;

    memset(&args, 0, sizeof(args));
    args.protect = req->arg[AAR_A_FILE_PROTECT];
    if (args.protect & ~0xffu)
        return AAR_E_INVAL;
    return serve_named(f, req, AAR_SERVE_NAMED_SET_PROTECT, &args);
}

/* `from` NUL `to` NUL in buf. */
static int two_paths_from_req(const struct aar_serve_req *req, char *from, char *to,
                              int32_t *err)
{
    uint32_t i = 0;
    uint32_t j;

    if (!req->buf || req->buf_len == 0)
        goto bad;
    for (;; i++) {
        if (i >= req->buf_len || i >= AAR_SERVE_PATH_BYTES)
            goto bad;
        from[i] = (char)req->buf[i];
        if (from[i] == 0)
            break;
    }
    i++;
    for (j = 0;; j++) {
        if (i + j >= req->buf_len || j >= AAR_SERVE_PATH_BYTES)
            goto bad;
        to[j] = (char)req->buf[i + j];
        if (to[j] == 0)
            break;
    }
    return 1;
bad:
    *err = AAR_E_INVAL;
    return 0;
}

/* Before the rename: a target that is the source itself (the same object, found
 * by comparing the two locks, not the two names) leaves nothing to do, with or
 * without AAR_RENAME_REPLACE (*same). With AAR_RENAME_REPLACE any other
 * existing target goes first. A target that is a directory with entries stays,
 * and the call fails with AAR_E_EXIST. */
static int32_t rename_prepare(const struct aar_serve_files *f,
                              const struct aar_serve_loc *from,
                              const struct aar_serve_loc *to, int replace, int *same)
{
    const struct aar_serve_dos_ops *d = f->dos;
    struct aar_serve_named_args none;
    int transport_ok = 0;
    int32_t io = 0;
    uintptr_t target;
    uintptr_t source;
    int32_t replace_ok = AAR_OK;

    memset(&none, 0, sizeof(none));
    *same = 0;
    target = loc_lock(f, to, &transport_ok, &io);
    if (!target) {
        /* There is no target to compare or delete. Whatever else kept the
         * lock from us is the rename's to report, unless it must delete. */
        if (!replace || (transport_ok && (io == AAR_SERVE_IOERR_OBJECT_NOT_FOUND ||
                                          io == AAR_SERVE_IOERR_DIR_NOT_FOUND)))
            return AAR_OK;
        return transport_ok ? lock_errno(io) : AAR_E_IO;
    }
    source = loc_lock(f, from, &transport_ok, &io);
    if (!source) {
        d->unlock(d->ctx, target);
        return transport_ok ? lock_errno(io) : AAR_E_IO;
    }
    *same = d->same_lock(d->ctx, source, target);
    if (replace && !*same)
        replace_ok = d->check_replace ?
            d->check_replace(d->ctx, source, target,
                             from->absolute ? loc_name(from) : NULL,
                             to->absolute ? loc_name(to) : NULL) : AAR_E_NOSYS;
    d->unlock(d->ctx, source);
    d->unlock(d->ctx, target);
    if (*same || !replace)
        return AAR_OK;
    if (replace_ok != AAR_OK)
        return replace_ok;
    return loc_named(f, to, AAR_SERVE_NAMED_DELETE, &none);
}

static int32_t serve_rename(struct aar_serve_files *f, const struct aar_serve_req *req)
{
    const struct aar_serve_dos_ops *d = f->dos;
    char from_path[AAR_SERVE_PATH_BYTES];
    char to_path[AAR_SERVE_PATH_BYTES];
    struct aar_serve_loc from;
    struct aar_serve_loc to;
    uint32_t flags = req->arg[AAR_A_FILE_RENAME_FLAGS];
    int32_t err = AAR_OK;
    int32_t io = 0;
    int transport_ok = 0;
    int same = 0;
    int ok;

    if (!d->rename || !d->target_named)
        return AAR_E_NOSYS;
    if (flags & ~(uint32_t)AAR_RENAME_REPLACE)
        return AAR_E_INVAL;
    if (!two_paths_from_req(req, from_path, to_path, &err))
        return err;
    if (!d->live(d->ctx))
        return AAR_E_IO;
    memset(&from, 0, sizeof(from));
    memset(&to, 0, sizeof(to));
    if (!loc_resolve(f, from_path, &from, &err) || !loc_resolve(f, to_path, &to, &err))
        goto done;
    if (!loc_confined(f, &from, AAR_SERVE_OPEN_READ_EXISTING, &err) ||
        !loc_confined(f, &to,
                      (flags & AAR_RENAME_REPLACE) ? AAR_SERVE_OPEN_CREATE_KEEP :
                                                     AAR_SERVE_OPEN_CREATE_EXCLUSIVE,
                      &err))
        goto done;
    /* An early handler check also covers a rename to a missing target.
     * Matching ports alone do not authorise a replacement delete. */
    if (d->same_volume && d->same_volume(d->ctx, from.absolute ? 0 : from.dir,
                                         loc_name(&from), to.absolute ? 0 : to.dir,
                                         loc_name(&to)) == 0) {
        err = AAR_E_INVAL;
        goto done;
    }
    err = rename_prepare(f, &from, &to, (flags & AAR_RENAME_REPLACE) != 0, &same);
    if (err != AAR_OK || same)
        goto done;
    if (!open_step_allowed(d)) {
        err = AAR_E_IO;
        goto done;
    }
    ok = d->rename(d->ctx, from.absolute ? 0 : from.dir, loc_name(&from),
                   to.absolute ? 0 : to.dir, loc_name(&to), &transport_ok, &io);
    err = ok ? AAR_OK : (transport_ok ? lock_errno(io) : AAR_E_IO);
done:
    loc_release(f, &from);
    loc_release(f, &to);
    return err;
}

/* A file's length is set to size. Shorter: ACTION_SET_FILE_SIZE. Longer: the
 * new bytes are written as zeros, because handlers differ on what a longer
 * SET_FILE_SIZE leaves in them and the spec says zeros. The position stays
 * where it was, as with ftruncate, unless the file now ends before it. */
static int32_t serve_truncate(struct aar_serve_files *f, const struct aar_serve_req *req)
{
    const struct aar_serve_dos_ops *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_TRUNC_FD], &err);
    uint64_t size = ((uint64_t)req->arg[AAR_A_FILE_TRUNC_SIZE_HI] << 32) |
                    req->arg[AAR_A_FILE_TRUNC_SIZE_LO];
    int32_t cur = 0;
    int32_t length = 0;
    int32_t io = 0;
    uintptr_t fh;

    if (index < 0 || !f->fh[index] || !f->fh_write[index])
        return AAR_E_BADF;
    if (size > 0x7fffffffULL)
        return AAR_E_INVAL;
    fh = f->fh[index];
    if (!d->live(d->ctx) || !open_step_allowed(d))
        return AAR_E_IO;
    err = seek_bounds(f, fh, 1, &cur, &length, &io);
    if (err != AAR_OK)
        return err;
    if (size < (uint64_t)length) {
        int transport_ok = 0;
        int32_t result = d->fh_set_size(d->ctx, fh, (uint32_t)size, &transport_ok, &io);

        if (!transport_ok)
            return AAR_E_IO;
        if (result < 0)
            return lock_errno(io);
        /* SetFileSize cannot cut below the position of another handle on the
         * file: it answers a larger size, not an error. The file stays longer
         * than asked, so the truncation did not happen. */
        if ((uint64_t)result != size)
            return AAR_E_BUSY;
    } else if (size > (uint64_t)length) {
        uint64_t left = size - (uint64_t)length;
        int transport_ok = 0;

        if (d->fh_seek(d->ctx, fh, 0, 2, &transport_ok, &io) < 0)
            return transport_ok ? lock_errno(io) : AAR_E_IO;
        memset(f->io, 0, sizeof(f->io));
        while (left) {
            uint32_t chunk = left > AAR_SERVE_IO_CHUNK ? AAR_SERVE_IO_CHUNK : (uint32_t)left;
            int32_t n;

            if (!open_step_allowed(d)) {
                err = AAR_E_IO;
                break;
            }
            n = d->fh_write(d->ctx, fh, f->io, chunk, &transport_ok, &io);
            if (!transport_ok) {
                err = AAR_E_IO;
                break;
            }
            if (n < 0) {
                err = lock_errno(io);
                break;
            }
            if (n == 0) {
                err = AAR_E_NOSPC;
                break;
            }
            left -= (uint32_t)n;
        }
        if (err != AAR_OK) {
            /* The file is longer by what was written, but the descriptor stays
             * where it was: the offset is put back, as far as that works, and
             * the extension's own error is the answer. */
            int back_ok = 0;
            int32_t back_io = 0;

            (void)d->fh_seek(d->ctx, fh, cur, 0, &back_ok, &back_io);
            return err;
        }
    }
    if ((uint64_t)cur <= size) {
        int transport_ok = 0;

        if (d->fh_seek(d->ctx, fh, cur, 0, &transport_ok, &io) < 0)
            return transport_ok ? lock_errno(io) : AAR_E_IO;
    }
    return AAR_OK;
}

static int32_t serve_volume_info(struct aar_serve_files *f, const struct aar_serve_req *req,
                                 struct aar_serve_done *done)
{
    const struct aar_serve_dos_ops *d = f->dos;
    char path[AAR_SERVE_PATH_BYTES];
    struct aar_serve_volume vol;
    uintptr_t dir = 0;
    uintptr_t locked = 0;
    int transport_ok = 0;
    int32_t io = 0;
    int32_t err = AAR_OK;

    if (!d->volume_info)
        return AAR_E_NOSYS;
    if (!path_from_req(req, path, &err))
        return err;
    if (path[0] == 0)
        return AAR_E_INVAL;
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!lock_named(f, path, 0, &dir, &locked, &err))
        goto done;
    memset(&vol, 0, sizeof(vol));
    if (!d->volume_info(d->ctx, locked, &vol, &transport_ok, &io)) {
        err = transport_ok ? lock_errno(io) : AAR_E_IO;
        goto done;
    }
    done->out[AAR_R_VOLUME_BLOCKS_TOTAL] = vol.blocks_total;
    done->out[AAR_R_VOLUME_BLOCKS_USED] = vol.blocks_used;
    done->out[AAR_R_VOLUME_BLOCK_BYTES] = vol.block_bytes;
    done->out[AAR_R_VOLUME_FLAGS] = vol.read_only ? AAR_VOLF_READONLY : 0u;
done:
    if (locked)
        d->unlock(d->ctx, locked);
    if (dir)
        d->unlock(d->ctx, dir);
    return err;
}

void aar_serve_files_serve(struct aar_serve_files *f, const struct aar_serve_req *req,
                           struct aar_serve_done *done)
{
    int32_t result;

    done_clear(done);
    switch (req->op) {
    case AAR_OP_FILE_OPEN: result = serve_open(f, req); break;
    case AAR_OP_FILE_CLOSE: result = serve_close(f, req); break;
    case AAR_OP_FILE_READ: result = serve_read(f, req, done); break;
    case AAR_OP_FILE_WRITE: result = serve_write(f, req, done); break;
    case AAR_OP_FILE_SEEK: result = serve_seek(f, req, done); break;
    case AAR_OP_FILE_STAT: result = serve_stat(f, req, done); break;
    case AAR_OP_LIST_OPEN: result = serve_list_open(f, req); break;
    case AAR_OP_LIST_NEXT: result = serve_list_next(f, req, done); break;
    case AAR_OP_LIST_CLOSE: result = serve_list_close(f, req); break;
    case AAR_OP_FILE_DELETE:
        result = serve_named(f, req, AAR_SERVE_NAMED_DELETE, NULL);
        break;
    case AAR_OP_FILE_RENAME: result = serve_rename(f, req); break;
    case AAR_OP_DIR_CREATE:
        result = serve_named(f, req, AAR_SERVE_NAMED_CREATE_DIR, NULL);
        break;
    case AAR_OP_FILE_SET_DATE: result = serve_set_date(f, req); break;
    case AAR_OP_FILE_SET_PROTECT: result = serve_set_protect(f, req); break;
    case AAR_OP_FILE_TRUNCATE: result = serve_truncate(f, req); break;
    case AAR_OP_VOLUME_INFO: result = serve_volume_info(f, req, done); break;
    default: result = AAR_E_NOSYS; break;
    }
    done->result = result;
}

/* ---- Locale, time, environment and clipboard ------------------------------ */

uint32_t aar_serve_latin1_to_utf8(const uint8_t *src, uint32_t n, uint8_t *dst,
                                  uint32_t cap)
{
    uint32_t i;
    uint32_t o = 0;

    for (i = 0; i < n; i++) {
        uint8_t c = src[i];

        if (c < 0x80u) {
            if (o + 1u > cap)
                break;
            dst[o++] = c;
        } else {
            if (o + 2u > cap)
                break;
            dst[o++] = (uint8_t)(0xC0u | (c >> 6));
            dst[o++] = (uint8_t)(0x80u | (c & 0x3Fu));
        }
    }
    return o;
}

uint32_t aar_serve_utf8_to_latin1(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    uint32_t i = 0;
    uint32_t o = 0;

    while (i < n) {
        uint8_t c = src[i];
        uint32_t cp = c;
        uint32_t len = 1;
        int ok = 1;

        if (c >= 0xC2u && c <= 0xDFu) {
            len = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            len = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            len = 4;
            cp = c & 0x07u;
        } else if (c >= 0x80u) {
            ok = 0;
        }
        if (ok && len > 1) {
            uint32_t k;

            if (i + len > n) {
                ok = 0;
            } else {
                for (k = 1; k < len; k++) {
                    if ((src[i + k] & 0xC0u) != 0x80u) {
                        ok = 0;
                        break;
                    }
                    cp = (cp << 6) | (src[i + k] & 0x3Fu);
                }
            }
            if (ok && ((len == 3 && cp < 0x800u) || (len == 4 && cp < 0x10000u) ||
                       (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu))
                ok = 0;
        }
        if (!ok) {
            /* A malformed sequence is one '?', with the continuation bytes it
             * carries. */
            uint32_t skipped = 0;

            dst[o++] = '?';
            i++;
            if (c >= 0xC0u)
                while (i < n && skipped < 3u && (src[i] & 0xC0u) == 0x80u) {
                    i++;
                    skipped++;
                }
            continue;
        }
        dst[o++] = cp <= 0xFFu ? (uint8_t)cp : (uint8_t)'?';
        i += len;
    }
    return o;
}

void aar_serve_host_init(struct aar_serve_host *h, const struct aar_serve_host_ops *ops,
                         uint32_t policy)
{
    memset(h, 0, sizeof(*h));
    h->ops = *ops;
    h->policy = policy;
}

static void host_drop_read(struct aar_serve_host *h)
{
    if (h->rd)
        h->ops.mem_free(h->ops.ctx, h->rd);
    h->rd = NULL;
    h->rd_bytes = 0;
}

static void host_drop_write(struct aar_serve_host *h)
{
    if (h->wr)
        h->ops.mem_free(h->ops.ctx, h->wr);
    h->wr = NULL;
    h->wr_bytes = 0;
    h->wr_total = 0;
    h->wr_active = 0;
}

void aar_serve_host_close(struct aar_serve_host *h)
{
    if (!h->ops.mem_free)
        return;
    host_drop_read(h);
    host_drop_write(h);
}

/* TIME_UNIX: the clock is the user's local time; the offset of locale.library
 * (minutes west) brings it to UTC. Without locale.library the value can be off
 * by the user's offset, and the flag says so. */
static int32_t host_time(struct aar_serve_host *h, struct aar_serve_done *done)
{
    struct aar_serve_locale loc;
    uint32_t days = 0;
    uint32_t minutes = 0;
    uint32_t ticks = 0;
    int known;
    uint32_t local;
    int64_t utc;
    uint64_t us;

    if (!h->ops.live(h->ops.ctx))
        return AAR_E_IO;
    memset(&loc, 0, sizeof(loc));
    h->ops.datestamp(h->ops.ctx, &days, &minutes, &ticks);
    known = h->ops.locale(h->ops.ctx, &loc);
    /* 32-bit arithmetic: the 68000 family has no 64-bit multiply or divide
     * without a library call, and the clock is a 32-bit count of seconds
     * until 2106. */
    local = days * 86400u + minutes * 60u + ticks / 50u;
    utc = (int64_t)AAR_SERVE_UNIX_DELTA + (int64_t)local;
    if (known)
        utc += (int64_t)(loc.gmt_west_min * 60);
    us = (uint64_t)(uint32_t)utc * 1000000u + (uint64_t)((ticks % 50u) * 20000u);
    done->out[AAR_R_TIME_UNIX_US_HI] = (uint32_t)(us >> 32);
    done->out[AAR_R_TIME_UNIX_US_LO] = (uint32_t)us;
    done->out[AAR_R_TIME_UNIX_FLAGS] = known ? AAR_LOCALEF_OFFSET_KNOWN : 0u;
    return AAR_OK;
}

static uint32_t cstr_len(const char *s, uint32_t cap)
{
    uint32_t n = 0;

    while (n < cap && s[n])
        n++;
    return n;
}

/* A string of locale.library as UTF-8 in a field of cap bytes that is
 * NUL-terminated: only whole characters. */
static void locale_text(uint8_t *field, uint32_t cap, const char *latin1, uint32_t max)
{
    uint32_t n = aar_serve_latin1_to_utf8((const uint8_t *)latin1,
                                          cstr_len(latin1, max), field, cap - 1u);

    field[n] = 0;
}

static int32_t host_locale_info(struct aar_serve_host *h, const struct aar_serve_req *req)
{
    static const char suffix[] = ".language";
    struct aar_serve_locale loc;
    uint8_t *rec = req->buf;
    uint32_t n;
    uint32_t shift;
    uint32_t put = 0;
    int known;

    if (!rec || req->buf_len < AAR_LOCALE_REC_BYTES)
        return AAR_E_INVAL;
    if (!h->ops.live(h->ops.ctx))
        return AAR_E_IO;
    memset(&loc, 0, sizeof(loc));
    known = h->ops.locale(h->ops.ctx, &loc);
    memset(rec, 0, AAR_LOCALE_REC_BYTES);
    aar_serve_put_be32(rec + AAR_LOCALE_REC_UTC_OFFSET_MIN,
                       (uint32_t)(-(int64_t)loc.gmt_west_min));
    aar_serve_put_be32(rec + AAR_LOCALE_REC_FLAGS, known ? AAR_LOCALEF_OFFSET_KNOWN : 0u);
    /* "english.language" is "english". */
    n = cstr_len(loc.language, sizeof(loc.language));
    if (n >= sizeof(suffix) - 1u &&
        memcmp(loc.language + n - (sizeof(suffix) - 1u), suffix, sizeof(suffix) - 1u) == 0)
        n -= (uint32_t)sizeof(suffix) - 1u;
    if (n > 31u)
        n = 31u;
    memcpy(rec + AAR_LOCALE_REC_LANGUAGE, loc.language, n);
    /* The country code is up to four characters in the bytes of a ULONG. */
    for (shift = 24; ; shift -= 8) {
        uint8_t c = (uint8_t)(loc.country >> shift);

        if (c && put < 7u)
            rec[AAR_LOCALE_REC_COUNTRY + put++] = c;
        if (shift == 0)
            break;
    }
    locale_text(rec + AAR_LOCALE_REC_DECIMAL_POINT, 4u, loc.decimal_point,
                sizeof(loc.decimal_point));
    locale_text(rec + AAR_LOCALE_REC_GROUP_SEPARATOR, 4u, loc.group_separator,
                sizeof(loc.group_separator));
    return AAR_OK;
}

/* A name that would leave ENV: is not a variable of ENV:. AmigaDOS reads
 * a name by components: a volume (a colon), a leading slash and an empty
 * component (two slashes in a row: each one past the first is a parent) all
 * climb out of the directory, so every component must be a non-empty name, and
 * not ".." (not special to AmigaDOS, but never a variable's name). */
static int env_name_confined(const char *name)
{
    const char *comp = name;

    if (strchr(name, ':'))
        return 0;
    for (;;) {
        const char *end = comp;

        while (*end && *end != '/')
            end++;
        if (end == comp || (end - comp == 2 && comp[0] == '.' && comp[1] == '.'))
            return 0;
        if (*end == 0)
            return 1;
        comp = end + 1;
    }
}

static int32_t host_env_get(struct aar_serve_host *h, const struct aar_serve_req *req,
                            struct aar_serve_done *done)
{
    char name[AAR_SERVE_ENV_NAME_MAX + 1u];
    uint32_t i;
    uint32_t full = 0;
    int32_t rc;

    if (h->policy & AAR_SERVE_HOST_NO_ENV)
        return AAR_E_ACCES;
    if (!req->buf)
        return AAR_E_INVAL;
    for (i = 0; i <= AAR_SERVE_ENV_NAME_MAX && i < req->buf_len; i++) {
        name[i] = (char)req->buf[i];
        if (name[i] == 0)
            break;
    }
    /* No NUL in 256 bytes, no NUL in the buffer, or an empty name. */
    if (i > AAR_SERVE_ENV_NAME_MAX || i >= req->buf_len || i == 0)
        return AAR_E_INVAL;
    if (!env_name_confined(name))
        return AAR_E_ACCES;
    if (!h->ops.live(h->ops.ctx))
        return AAR_E_IO;
    rc = h->ops.env_get(h->ops.ctx, name, req->buf, req->buf_len, &full);
    if (rc != AAR_OK)
        return rc;
    /* The NUL follows the value when it fits; the runtime sees the full
     * length and decides AAR_E_NOSPC from it. */
    if (full < req->buf_len)
        req->buf[full] = 0;
    done->out[AAR_R_ENV_LEN] = full;
    return AAR_OK;
}

#define IFF_ID(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                            ((uint32_t)(c) << 8) | (uint32_t)(d))
#define IFF_FORM IFF_ID('F', 'O', 'R', 'M')
#define IFF_FTXT IFF_ID('F', 'T', 'X', 'T')
#define IFF_CHRS IFF_ID('C', 'H', 'R', 'S')
#define IFF_CSET IFF_ID('C', 'S', 'E', 'T')
/* CSET code sets are IANA MIBenum values: 106 is UTF-8; 0 (the IFF default),
 * 3 (ASCII), 4 (ISO-8859-1) and every code set not known here read as
 * ISO-8859-1. */
#define CSET_UTF8 106u

/* The next chunk of the body of a form that ends at end: *pos is the offset of
 * the chunk's header and moves to the next one. The data is clipped to the
 * form. 0 at the end. */
static int form_next(const uint8_t *raw, uint32_t end, uint32_t *pos, uint32_t *id,
                     const uint8_t **data, uint32_t *avail)
{
    uint32_t size;
    uint64_t next;

    if ((uint64_t)*pos + 8u > end)
        return 0;
    *id = aar_serve_be32(raw + *pos);
    size = aar_serve_be32(raw + *pos + 4u);
    *data = raw + *pos + 8u;
    *avail = size;
    if (*avail > end - (*pos + 8u))
        *avail = end - (*pos + 8u);
    next = (uint64_t)*pos + 8u + size + (size & 1u);
    *pos = next > end ? end : (uint32_t)next;
    return 1;
}

/* The extent of the form at the start of raw: 8 + its size and the pad byte
 * that follows an odd size, but not beyond the data there is. 0: it is not a
 * form. */
static uint32_t form_extent(const uint8_t *raw, uint32_t rawlen)
{
    uint32_t size;
    uint32_t extent;

    if (rawlen < 12u || aar_serve_be32(raw) != IFF_FORM)
        return 0;
    size = aar_serve_be32(raw + 4);
    if (size < 4u)
        return 0;
    if (size > rawlen - 8u)
        return rawlen;
    extent = 8u + size + (size & 1u);
    return extent > rawlen ? rawlen : extent;
}

/* The text of an FTXT form: its CHRS chunks in order, as UTF-8. */
static int32_t clip_text_from_form(struct aar_serve_host *h, const uint8_t *raw,
                                   uint32_t rawlen, uint8_t **out, uint32_t *out_bytes)
{
    uint32_t end = form_extent(raw, rawlen);
    uint32_t pos = 12;
    uint32_t id;
    uint32_t avail;
    uint64_t total = 0;
    uint64_t need;
    uint32_t cset = 0;
    uint32_t n = 0;
    uint32_t i;
    int have_cset = 0;
    int utf8;
    const uint8_t *data;
    uint8_t *text;

    if (!end || aar_serve_be32(raw + 8) != IFF_FTXT)
        return AAR_E_NOENT;
    while (form_next(raw, end, &pos, &id, &data, &avail)) {
        if (id == IFF_CSET && avail >= 4u && !have_cset) {
            cset = aar_serve_be32(data);
            have_cset = 1;
        } else if (id == IFF_CHRS) {
            total += avail;
        }
    }
    if (total == 0)
        return AAR_E_NOENT;
    utf8 = have_cset && cset == CSET_UTF8;
    /* The text limit is on the UTF-8 text, not on the stream it came in (that
     * has the wrapper's chunk headers too): its exact length is counted, then
     * the text is made in a buffer of that size. */
    need = total;
    if (!utf8) {
        pos = 12;
        while (form_next(raw, end, &pos, &id, &data, &avail)) {
            if (id != IFF_CHRS)
                continue;
            for (i = 0; i < avail; i++)
                if (data[i] >= 0x80u)
                    need++;
        }
    }
    if (need > AAR_SERVE_CLIP_MAX)
        return AAR_E_NOMEM;
    text = h->ops.mem_alloc(h->ops.ctx, (uint32_t)need);
    if (!text)
        return AAR_E_NOMEM;
    pos = 12;
    while (form_next(raw, end, &pos, &id, &data, &avail)) {
        if (id != IFF_CHRS)
            continue;
        if (utf8) {
            memcpy(text + n, data, avail);
            n += avail;
        } else {
            n += aar_serve_latin1_to_utf8(data, avail, text + n, (uint32_t)need - n);
        }
    }
    *out = text;
    *out_bytes = n;
    return AAR_OK;
}

/* The clip of unit in type, held for the read sequence. */
static int32_t clip_load(struct aar_serve_host *h, uint32_t unit, uint32_t type)
{
    uint8_t *raw = NULL;
    uint32_t rawlen = 0;
    int32_t rc;

    host_drop_read(h);
    rc = h->ops.clip_read(h->ops.ctx, unit, &raw, &rawlen);
    if (rc != AAR_OK)
        return rc;
    if (type == AAR_CLIP_TEXT) {
        uint8_t *text = NULL;
        uint32_t bytes = 0;

        rc = clip_text_from_form(h, raw, rawlen, &text, &bytes);
        h->ops.mem_free(h->ops.ctx, raw);
        if (rc != AAR_OK)
            return rc;
        h->rd = text;
        h->rd_bytes = bytes;
    } else {
        uint32_t extent = form_extent(raw, rawlen);

        if (!extent) {
            h->ops.mem_free(h->ops.ctx, raw);
            return AAR_E_NOENT;
        }
        /* The raw data was taken with room for a text's wrapper: an IFF clip
         * has the limit of the clip itself. */
        if (extent > AAR_SERVE_CLIP_MAX) {
            h->ops.mem_free(h->ops.ctx, raw);
            return AAR_E_NOMEM;
        }
        h->rd = raw;
        h->rd_bytes = extent;
    }
    h->rd_unit = unit;
    h->rd_type = type;
    return AAR_OK;
}

static int clip_args(const struct aar_serve_req *req, uint32_t *unit, uint32_t *type)
{
    *unit = req->arg[AAR_A_CLIP_UNIT];
    *type = req->arg[AAR_A_CLIP_TYPE];
    return *unit < AAR_CLIP_UNITS && (*type == AAR_CLIP_TEXT || *type == AAR_CLIP_IFF);
}

static int32_t host_clip_read(struct aar_serve_host *h, const struct aar_serve_req *req,
                              struct aar_serve_done *done)
{
    uint32_t unit;
    uint32_t type;
    uint32_t offset = req->arg[AAR_A_CLIP_OFFSET];
    uint32_t n;
    int32_t rc;

    if (h->policy & AAR_SERVE_HOST_NO_CLIP)
        return AAR_E_ACCES;
    if (!clip_args(req, &unit, &type) || (req->buf_len && !req->buf))
        return AAR_E_INVAL;
    if (!h->ops.live(h->ops.ctx))
        return AAR_E_IO;
    /* One read sequence reads one clip: it is taken at offset 0 and kept. */
    if (offset == 0 || !h->rd || h->rd_unit != unit || h->rd_type != type) {
        rc = clip_load(h, unit, type);
        if (rc != AAR_OK)
            return rc;
    }
    if (offset > h->rd_bytes)
        return AAR_E_INVAL;
    n = h->rd_bytes - offset;
    if (n > req->buf_len)
        n = req->buf_len;
    if (n)
        memcpy(req->buf, h->rd + offset, n);
    done->out[AAR_R_CLIP_READ_SIZE] = h->rd_bytes;
    done->out[AAR_R_CLIP_READ_COUNT] = n;
    if (offset + n >= h->rd_bytes && n)
        host_drop_read(h);
    return AAR_OK;
}

/* The last chunk is in: the clip of the unit is replaced. */
static int32_t clip_commit(struct aar_serve_host *h)
{
    uint32_t total = h->wr_total;
    uint32_t size;
    int32_t rc;

    if (h->wr_bytes != total)
        return AAR_E_INVAL;
    if (h->wr_type == AAR_CLIP_TEXT) {
        /* The Latin-1 text is never longer than the UTF-8 it came from, so it
         * is made in place. */
        uint32_t m = aar_serve_utf8_to_latin1(h->wr, total, h->wr);
        uint8_t *form = h->ops.mem_alloc(h->ops.ctx, m + 24u);

        if (!form)
            return AAR_E_NOMEM;
        size = 4u + 8u + m + (m & 1u);
        memcpy(form, "FORM", 4);
        aar_serve_put_be32(form + 4, size);
        memcpy(form + 8, "FTXT", 4);
        memcpy(form + 12, "CHRS", 4);
        aar_serve_put_be32(form + 16, m);
        memcpy(form + 20, h->wr, m);
        if (m & 1u)
            form[20 + m] = 0;
        rc = h->ops.clip_write(h->ops.ctx, h->wr_unit, form, 8u + size);
        h->ops.mem_free(h->ops.ctx, form);
        return rc;
    }
    /* One FORM, as it came; the pad byte is added when the sender left it out. */
    if (total < 12u || aar_serve_be32(h->wr) != IFF_FORM)
        return AAR_E_INVAL;
    size = aar_serve_be32(h->wr + 4);
    if (size < 4u || size > total - 8u)
        return AAR_E_INVAL;
    if (size & 1u)
        h->wr[8u + size] = 0; /* the pad byte; the buffer holds one byte more than the clip */
    return h->ops.clip_write(h->ops.ctx, h->wr_unit, h->wr, 8u + size + (size & 1u));
}

static int32_t host_clip_write(struct aar_serve_host *h, const struct aar_serve_req *req,
                               struct aar_serve_done *done)
{
    uint32_t unit;
    uint32_t type;
    uint32_t offset = req->arg[AAR_A_CLIP_OFFSET];
    uint32_t total = req->arg[AAR_A_CLIP_TOTAL];
    uint32_t count = req->buf_len;
    int32_t rc = AAR_OK;

    if (h->policy & AAR_SERVE_HOST_NO_CLIP)
        return AAR_E_ACCES;
    if (!clip_args(req, &unit, &type) || (count && !req->buf))
        return AAR_E_INVAL;
    if (!h->ops.live(h->ops.ctx))
        return AAR_E_IO;
    if (offset == 0) {
        /* A sequence that was never finished is dropped: it changed nothing. */
        host_drop_write(h);
        if (total > AAR_SERVE_CLIP_MAX)
            return AAR_E_NOMEM;
        h->wr = h->ops.mem_alloc(h->ops.ctx, total + 1u);
        if (!h->wr)
            return AAR_E_NOMEM;
        h->wr_active = 1;
        h->wr_unit = unit;
        h->wr_type = type;
        h->wr_total = total;
    } else if (!h->wr_active || h->wr_unit != unit || h->wr_type != type ||
               h->wr_total != total || offset != h->wr_bytes) {
        host_drop_write(h);
        return AAR_E_INVAL;
    }
    if (count > h->wr_total - h->wr_bytes) {
        host_drop_write(h);
        return AAR_E_INVAL;
    }
    if (count)
        memcpy(h->wr + h->wr_bytes, req->buf, count);
    h->wr_bytes += count;
    if (req->flags & AAR_CLIPF_LAST) {
        rc = clip_commit(h);
        host_drop_write(h);
        if (rc != AAR_OK)
            return rc;
    }
    done->out[AAR_R_CLIP_WRITE_COUNT] = count;
    return AAR_OK;
}

void aar_serve_host_serve(struct aar_serve_host *h, const struct aar_serve_req *req,
                          struct aar_serve_done *done)
{
    int32_t result;

    done_clear(done);
    switch (req->op) {
    case AAR_OP_TIME_UNIX: result = host_time(h, done); break;
    case AAR_OP_LOCALE_INFO: result = host_locale_info(h, req); break;
    case AAR_OP_ENV_GET: result = host_env_get(h, req, done); break;
    case AAR_OP_CLIP_READ: result = host_clip_read(h, req, done); break;
    case AAR_OP_CLIP_WRITE: result = host_clip_write(h, req, done); break;
    default: result = AAR_E_NOSYS; break;
    }
    done->result = result;
}

/* ---- Console ------------------------------------------------------------ */

/* A console table needs all its members; aar_serve wraps a program's table so
 * that the optional ones may be NULL there (aar_serve.c, repl_con_*). */
void aar_serve_console_init(struct aar_serve_console *c,
                            const struct aar_serve_console_ops *ops,
                            const struct aar_serve_sink *sink)
{
    memset(c, 0, sizeof(*c));
    c->ops = *ops;
    c->sink = sink;
}

static void launcher_put_byte(struct aar_serve_console *c, uint8_t value)
{
    c->launcher[c->launcher_head] = value;
    c->launcher_head = (c->launcher_head + 1u) % AAR_SERVE_LAUNCHER_OUTPUT_BYTES;
    c->launcher_used++;
}

void aar_serve_console_launcher_put(struct aar_serve_console *c, const uint8_t *data,
                                    uint32_t bytes, int diagnostic)
{
    uint32_t i;

    if (!aar_serve_launcher_output_accept(c->launcher_used, bytes, diagnostic)) {
        if (diagnostic)
            c->launcher_dropped++;
        return;
    }
    for (i = 0; i < bytes; i++)
        launcher_put_byte(c, data[i]);
}

void aar_serve_console_launcher_status(struct aar_serve_console *c, const char *msg)
{
    uint32_t bytes = (uint32_t)strlen(msg);
    uint32_t i;

    if (!aar_serve_launcher_output_accept(c->launcher_used, bytes + 1u, 0))
        return;
    for (i = 0; i < bytes; i++)
        launcher_put_byte(c, (uint8_t)msg[i]);
    launcher_put_byte(c, '\n');
}

void aar_serve_console_launcher_log(struct aar_serve_console *c, const uint8_t *data,
                                    uint32_t bytes)
{
    uint32_t i;
    int newline = bytes == 0 || data[bytes - 1u] != '\n';

    if (!aar_serve_launcher_output_accept(c->launcher_used,
                                          bytes + (uint32_t)newline, 1)) {
        c->launcher_dropped++;
        return;
    }
    for (i = 0; i < bytes; i++)
        launcher_put_byte(c, data[i]);
    if (newline)
        launcher_put_byte(c, '\n');
}

static void app_write_finish(struct aar_serve_console *c, int32_t result)
{
    struct aar_serve_done done;

    done_result(&done, result);
    done.out[AAR_R_CONSOLE_COUNT] = c->app_write_done;
    c->app_write_active = 0;
    c->sink->complete(c->sink->ctx, &c->app_write, &done);
}

static void read_finish(struct aar_serve_console *c, int32_t result,
                        uint32_t count)
{
    struct aar_serve_done done;

    done_result(&done, result);
    done.out[AAR_R_CONSOLE_COUNT] = count;
    c->read_active = 0;
    c->read_pending = 0;
    c->sink->complete(c->sink->ctx, &c->read_req, &done);
}

static void console_fail(struct aar_serve_console *c)
{
    if (c->failed)
        return;
    c->failed = 1;
    c->ops.output_error(c->ops.ctx);
}

static uint32_t launcher_contiguous(const struct aar_serve_console *c)
{
    uint32_t bytes = AAR_SERVE_LAUNCHER_OUTPUT_BYTES - c->launcher_tail;

    return c->launcher_used < bytes ? c->launcher_used : bytes;
}

void aar_serve_console_pump(struct aar_serve_console *c)
{
    if (!c->ops.live(c->ops.ctx)) {
        /* The claim was lost: the requests' buffers are not ours any more.
         * Their completions go nowhere, and a DOS packet that is still out
         * finds nothing waiting for it. The launcher's own output (the
         * diagnostics that say so) is still written. */
        if (c->app_write_active)
            app_write_finish(c, AAR_E_IO);
        if (c->read_active)
            read_finish(c, AAR_E_IO, 0);
    }
    if (c->failed && c->app_write_active)
        app_write_finish(c, AAR_E_IO);
    if (!c->failed && !c->write_pending) {
        uint32_t app_left = c->app_write_active ?
            c->app_write.buf_len - c->app_write_done : 0u;
        int source = aar_serve_output_next_source(c->launcher_used, app_left);

        /* An app write opens its destination even when queued launcher
         * bytes are written first: they may be the first output there is, and
         * on a Workbench launch only the app's console request opens CON:. */
        if (app_left != 0u && !c->ops.open(c->ops.ctx)) {
            console_fail(c);
            app_write_finish(c, AAR_E_IO);
        } else if (source == AAR_SERVE_OUTPUT_LAUNCHER) {
            uint32_t chunk = launcher_contiguous(c);

            if (chunk > AAR_SERVE_CONSOLE_WRITE_BYTES)
                chunk = AAR_SERVE_CONSOLE_WRITE_BYTES;
            c->write_source = source;
            c->write_bytes = chunk;
            c->write_pending = 1;
            if (!c->ops.write_start(c->ops.ctx, c->launcher + c->launcher_tail,
                                    chunk)) {
                c->write_pending = 0;
                console_fail(c);
            }
        } else if (source == AAR_SERVE_OUTPUT_APP) {
            uint32_t chunk = app_left;

            if (chunk > AAR_SERVE_CONSOLE_WRITE_BYTES)
                chunk = AAR_SERVE_CONSOLE_WRITE_BYTES;
            c->write_source = source;
            c->write_bytes = chunk;
            c->write_pending = 1;
            if (!c->ops.write_start(c->ops.ctx,
                                    c->app_write.buf + c->app_write_done,
                                    chunk)) {
                c->write_pending = 0;
                console_fail(c);
                app_write_finish(c, AAR_E_IO);
            }
        }
    }
    if (c->read_active && !c->read_pending && !c->eof &&
        c->ops.read_allowed(c->ops.ctx)) {
        if (!c->ops.open(c->ops.ctx)) {
            /* The console the app asked for cannot be opened: a launcher
             * error, as for a write. */
            console_fail(c);
            read_finish(c, AAR_E_IO, 0);
        } else if (c->ops.read_start(c->ops.ctx, c->read_max)) {
            c->read_pending = 1;
        } else {
            /* A read that cannot start is the end of the input. */
            c->eof = 1;
            read_finish(c, AAR_OK, 0);
        }
    }
}

void aar_serve_console_write_request(struct aar_serve_console *c,
                                     const struct aar_serve_req *req,
                                     struct aar_serve_done *done, int *deferred)
{
    *deferred = 0;
    done_clear(done);
    if (c->failed) {
        done->result = AAR_E_IO;
        return;
    }
    if (req->buf_len == 0) {
        done->result = AAR_OK;
        return;
    }
    if (!req->buf) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (c->app_write_active) {
        done->result = AAR_E_BUSY;
        return;
    }
    c->app_write = *req;
    c->app_write_done = 0;
    c->app_write_active = 1;
    *deferred = 1;
    aar_serve_console_pump(c);
}

void aar_serve_console_write_done(struct aar_serve_console *c, int32_t written)
{
    uint32_t advanced = aar_serve_console_write_advance(c->write_bytes, written);
    int source = c->write_source;
    int failed = aar_serve_console_write_failed(c->write_bytes, written);

    if (!c->write_pending)
        return; /* nothing was started: a completion without a write is ignored */
    c->write_pending = 0;
    c->write_source = AAR_SERVE_OUTPUT_NONE;
    if (source == AAR_SERVE_OUTPUT_LAUNCHER) {
        c->launcher_tail = (c->launcher_tail + advanced) %
                           AAR_SERVE_LAUNCHER_OUTPUT_BYTES;
        c->launcher_used -= advanced;
    } else if (c->app_write_active) {
        c->app_write_done += advanced;
    }
    if (failed) {
        console_fail(c);
        if (c->app_write_active)
            app_write_finish(c, AAR_E_IO);
        return;
    }
    if (source == AAR_SERVE_OUTPUT_APP && c->app_write_active &&
        c->app_write_done >= c->app_write.buf_len)
        app_write_finish(c, AAR_OK);
    aar_serve_console_pump(c);
}

void aar_serve_console_read_request(struct aar_serve_console *c,
                                    const struct aar_serve_req *req,
                                    struct aar_serve_done *done, int *deferred)
{
    *deferred = 0;
    done_clear(done);
    if (!req->buf || req->buf_len == 0) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (c->eof) {
        done->result = AAR_OK;
        done->out[AAR_R_CONSOLE_COUNT] = 0;
        return;
    }
    if (c->read_active) {
        done->result = AAR_E_BUSY;
        return;
    }
    c->read_req = *req;
    c->read_active = 1;
    c->read_pending = 0;
    c->read_max = req->buf_len < AAR_SERVE_CONSOLE_READ_BYTES ? req->buf_len :
                                                          AAR_SERVE_CONSOLE_READ_BYTES;
    *deferred = 1;
    aar_serve_console_pump(c);
}

void aar_serve_console_read_done(struct aar_serve_console *c, int32_t got,
                                 const uint8_t *data)
{
    uint32_t count;

    if (!c->read_active) {
        c->read_pending = 0;
        return;
    }
    if (!c->ops.live(c->ops.ctx)) {
        read_finish(c, AAR_E_IO, 0);
        return;
    }
    if (got <= 0) {
        c->eof = 1;
        read_finish(c, AAR_OK, 0);
        return;
    }
    count = (uint32_t)got < c->read_req.buf_len ? (uint32_t)got :
                                                  c->read_req.buf_len;
    memcpy(c->read_req.buf, data, count);
    read_finish(c, AAR_OK, count);
}

int aar_serve_console_output_pending(const struct aar_serve_console *c)
{
    return c->write_pending || c->launcher_used || c->app_write_active;
}

/* ---- Display ------------------------------------------------------------ */

void aar_serve_display_init(struct aar_serve_display *d,
                            const struct aar_serve_display_ops *ops,
                            const struct aar_serve_sink *sink)
{
    memset(d, 0, sizeof(*d));
    d->ops = *ops;
    d->sink = sink;
}

void aar_serve_display_reset(struct aar_serve_display *d)
{
    d->open = 0;
    d->have_two = 0;
    d->have_pan = 0;
    d->pan_count = 0;
    d->showing = 0;
    d->features = 0;
    d->caps = 0;
    d->flip_active = 0;
    d->flip_queued = 0;
}

int32_t aar_serve_display_open_check(const struct aar_serve_req *req)
{
    uint32_t w = req->arg[AAR_A_DISPLAY_OPEN_W];
    uint32_t h = req->arg[AAR_A_DISPLAY_OPEN_H];
    uint32_t format = req->arg[AAR_A_DISPLAY_OPEN_FORMAT];

    if (w == 0 || h == 0 || w > AAR_SERVE_SCREEN_MAX || h > AAR_SERVE_SCREEN_MAX)
        return AAR_E_INVAL;
    if (format != AAR_FMT_RGB565 && format != AAR_FMT_BGRA8888 &&
        format != AAR_FMT_CLUT8)
        return AAR_E_INVAL;
    return AAR_OK;
}

uint32_t aar_serve_display_depth(uint32_t format)
{
    return format == AAR_FMT_BGRA8888 ? 32u :
           (format == AAR_FMT_CLUT8 ? 8u : 16u);
}

uint32_t aar_serve_display_layout(uint32_t format)
{
    return format == AAR_FMT_BGRA8888 ? AAR_LAYOUT_BGRA8888 :
           (format == AAR_FMT_CLUT8 ? AAR_LAYOUT_CLUT8 : AAR_LAYOUT_RGB565_BE);
}

uint16_t aar_serve_display_caps(uint32_t open_flags, uint32_t features)
{
    uint32_t caps = AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8;

    if (open_flags & AAR_DISPLAY_LATEST)
        caps |= AAR_DISPCAP_LATEST;
    if ((open_flags & AAR_DISPLAY_WINDOW) && (features & AAR_DISPCAP_WINDOW)) {
        caps |= AAR_DISPCAP_WINDOW;
        /* A resizable window is a window: without one there is nothing to size. */
        if ((open_flags & AAR_DISPLAY_RESIZABLE) && (features & AAR_DISPCAP_RESIZE))
            caps |= AAR_DISPCAP_RESIZE;
        /* The close gadget belongs to the window. */
        if ((open_flags & AAR_DISPLAY_CLOSE_REQUEST) &&
            (features & AAR_DISPCAP_CLOSE_REQUEST))
            caps |= AAR_DISPCAP_CLOSE_REQUEST;
    }
    if ((open_flags & AAR_DISPLAY_POINTER_ABS) && (features & AAR_DISPCAP_POINTER_ABS))
        caps |= AAR_DISPCAP_POINTER_ABS;
    if ((open_flags & AAR_DISPLAY_TEXT_INPUT) && (features & AAR_DISPCAP_TEXT_INPUT))
        caps |= AAR_DISPCAP_TEXT_INPUT;
    if ((open_flags & AAR_DISPLAY_SCALE) && (features & AAR_DISPCAP_SCALED))
        caps |= AAR_DISPCAP_SCALED;
    return (uint16_t)caps;
}

void aar_serve_display_opened(struct aar_serve_display *d, const struct aar_serve_req *req,
                              uint32_t pitch, uint32_t front, uint32_t back,
                              uint32_t spare, int have_two, int have_pan,
                              uint32_t pan_count, uint32_t features,
                              struct aar_serve_done *done)
{
    aar_serve_display_reset(d);
    d->open = 1;
    d->id = AAR_SERVE_DISPLAY_ID;
    d->width = req->arg[AAR_A_DISPLAY_OPEN_W];
    d->height = req->arg[AAR_A_DISPLAY_OPEN_H];
    d->format = req->arg[AAR_A_DISPLAY_OPEN_FORMAT];
    d->flags = req->arg[AAR_A_DISPLAY_OPEN_FLAGS];
    d->layout = aar_serve_display_layout(d->format);
    d->pitch = pitch;
    d->front = front;
    d->back = back;
    d->spare = spare;
    d->have_two = have_two;
    d->have_pan = have_pan;
    d->pan_count = have_pan ? pan_count : 0u;
    d->features = features;
    d->caps = aar_serve_display_caps(d->flags, features);
    done_result(done, AAR_OK);
    done->flags = (uint16_t)d->caps;
    done->out[AAR_R_DISPLAY_OPEN_ID] = d->id;
    done->out[AAR_R_DISPLAY_OPEN_PITCH] = pitch;
    done->out[AAR_R_DISPLAY_OPEN_LAYOUT] = d->layout;
    done->out[AAR_R_DISPLAY_OPEN_FRONT] = front;
    done->out[AAR_R_DISPLAY_OPEN_BACK] = back;
    done->out[AAR_R_DISPLAY_OPEN_SPARE] = spare;
}

static uint32_t display_pages(const struct aar_serve_display *d)
{
    if (d->have_pan)
        return d->pan_count ? d->pan_count : 1u;
    return d->have_two ? 2u : 1u;
}

static uint32_t display_page_token(const struct aar_serve_display *d, uint32_t page)
{
    if (d->have_pan)
        return d->front + page * d->pitch * d->height;
    return page == 0 ? d->front : (page == 1 ? d->back : d->spare);
}

/* After a flip: the page now scanned out is the front, the next page the app
 * draws into is the back. */
static void display_flip_outs(const struct aar_serve_display *d,
                              struct aar_serve_done *done)
{
    uint32_t pages = display_pages(d);

    done->out[AAR_R_DISPLAY_FLIP_FRONT] = display_page_token(d, d->showing);
    done->out[AAR_R_DISPLAY_FLIP_BACK] =
        display_page_token(d, (d->showing + 1u) % pages);
    done->out[AAR_R_DISPLAY_FLIP_PITCH] = d->pitch;
    done->out[AAR_R_DISPLAY_FLIP_LAYOUT] = d->layout;
    done->out[AAR_R_DISPLAY_FLIP_DROPPED] = 0;
}

static int display_flip_two(struct aar_serve_display *d, const struct aar_serve_req *req)
{
    uint32_t next = d->showing ? 0u : 1u;

    if (!d->ops.change_buffer(d->ops.ctx, (int)next))
        return 0;
    d->showing = next;
    d->flip_req = *req;
    d->flip_active = 1;
    return 1;
}

void aar_serve_display_flip_request(struct aar_serve_display *d,
                                    const struct aar_serve_req *req,
                                    struct aar_serve_done *done, int *deferred)
{
    *deferred = 0;
    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (d->have_two) {
        if (d->flip_active) {
            if (d->flip_queued) {
                done->result = AAR_E_BUSY;
                return;
            }
            d->flip_next = *req;
            d->flip_queued = 1;
            *deferred = 1;
            return;
        }
        if (!display_flip_two(d, req)) {
            done->result = AAR_E_IO;
            return;
        }
        *deferred = 1;
        return;
    }
    if (d->have_pan) {
        uint32_t next = (d->showing + 1u) % display_pages(d);

        d->ops.pan(d->ops.ctx, next);
        d->showing = next;
        done->flags = AAR_CFLAG_FLIP_SUBMITTED;
        display_flip_outs(d, done);
        return;
    }
    /* A single buffer: the firmware copies the frame. A window or a scaled
     * screen shows it first. */
    if (d->ops.present)
        d->ops.present(d->ops.ctx);
    if ((d->flags & AAR_DISPLAY_VSYNC) && d->ops.wait_tof)
        d->ops.wait_tof(d->ops.ctx);
    display_flip_outs(d, done);
}

/* A replacement table's open succeeded and left the screen unrecorded (the
 * built-in table records it itself): record it from the completion. The table's
 * ops decide the mode: with change_buffer there are two screen buffers (a flip
 * completes at aar_serve_flip_shown), else with pan the pages are one tall
 * bitmap (a third page when a spare token is given), else there is one buffer.
 * A table that gives no front token or pitch has opened nothing. */
static void display_adopt(struct aar_serve_display *d, const struct aar_serve_req *req,
                          struct aar_serve_done *done)
{
    uint32_t pitch = done->out[AAR_R_DISPLAY_OPEN_PITCH];
    uint32_t front = done->out[AAR_R_DISPLAY_OPEN_FRONT];
    uint32_t back = done->out[AAR_R_DISPLAY_OPEN_BACK];
    uint32_t spare = done->out[AAR_R_DISPLAY_OPEN_SPARE];
    int two = d->ops.change_buffer != NULL && back != 0u;
    int pan = !two && d->ops.pan != NULL && back != 0u;

    if (front == 0u || pitch == 0u) {
        if (d->ops.close)
            d->ops.close(d->ops.ctx);
        done_result(done, AAR_E_IO);
        return;
    }
    aar_serve_display_opened(d, req, pitch, front, back, spare, two, pan,
                             pan ? (spare ? 3u : 2u) : 0u, 0u, done);
}

static void flip_complete(struct aar_serve_display *d, const struct aar_serve_req *req,
                          int32_t result)
{
    struct aar_serve_done done;

    done_result(&done, result);
    if (result == AAR_OK)
        display_flip_outs(d, &done);
    d->sink->complete(d->sink->ctx, req, &done);
}

/* A parked DISPLAY_VBLANK ends with result: the watch stops and the app is
 * answered. */
static void vblank_finish(struct aar_serve_display *d, int32_t result)
{
    struct aar_serve_req req;
    struct aar_serve_done done;

    if (!d->vb_active)
        return;
    req = d->vb_req;
    d->vb_active = 0;
    if (d->ops.vblank)
        d->ops.vblank(d->ops.ctx, 0);
    done_result(&done, result);
    d->sink->complete(d->sink->ctx, &req, &done);
}

void aar_serve_display_flip_safe(struct aar_serve_display *d)
{
    struct aar_serve_req finished;

    if (!d->flip_active)
        return;
    finished = d->flip_req;
    d->flip_active = 0;
    flip_complete(d, &finished, AAR_OK);
    if (d->flip_queued) {
        struct aar_serve_req next = d->flip_next;

        d->flip_queued = 0;
        if (display_flip_two(d, &next))
            return;
        flip_complete(d, &next, AAR_E_IO);
    }
}

void aar_serve_display_palette_request(struct aar_serve_display *d,
                                       const struct aar_serve_req *req,
                                       struct aar_serve_done *done)
{
    uint32_t colors[2u + 256u * 3u];
    uint32_t first = req->arg[AAR_A_DISPLAY_PALETTE_FIRST];
    uint32_t count = req->arg[AAR_A_DISPLAY_PALETTE_COUNT];
    uint32_t i;

    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id ||
        d->format != AAR_FMT_CLUT8 || count == 0 || first >= 256u ||
        count > 256u - first || !req->buf || req->buf_len < count * 4u) {
        done->result = AAR_E_INVAL;
        return;
    }
    /* LoadRGB32: (count << 16) | first, then R, G, B per colour, each
     * left-justified in 32 bits, then a 0 terminator. */
    colors[0] = (count << 16) | first;
    for (i = 0; i < count; i++) {
        uint32_t rgb = aar_serve_be32(req->buf + i * 4u);

        colors[1u + i * 3u] = ((rgb >> 16) & 0xffu) * 0x01010101u;
        colors[2u + i * 3u] = ((rgb >> 8) & 0xffu) * 0x01010101u;
        colors[3u + i * 3u] = (rgb & 0xffu) * 0x01010101u;
    }
    colors[1u + count * 3u] = 0;
    if (!d->ops.load_rgb32) {
        done->result = AAR_E_NOSYS;
        return;
    }
    d->ops.load_rgb32(d->ops.ctx, colors);
    done->result = AAR_OK;
}

void aar_serve_display_close_request(struct aar_serve_display *d,
                                     const struct aar_serve_req *req,
                                     struct aar_serve_done *done)
{
    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id) {
        done->result = AAR_E_INVAL;
        return;
    }
    /* The close waits for the SafeMessages of flips still in flight. */
    if (d->ops.close)
        d->ops.close(d->ops.ctx);
    if (d->flip_active) {
        struct aar_serve_req finished = d->flip_req;

        d->flip_active = 0;
        flip_complete(d, &finished, AAR_E_IO);
    }
    if (d->flip_queued) {
        struct aar_serve_req next = d->flip_next;

        d->flip_queued = 0;
        flip_complete(d, &next, AAR_E_IO);
    }
    vblank_finish(d, AAR_E_IO);
    aar_serve_display_reset(d);
    done->result = AAR_OK;
}

/* ---- Display: info, modes, resize, title, pointer, vertical blank ------- */

void aar_serve_display_clock(struct aar_serve_display *d, uint32_t now_ms)
{
    d->now_ms = now_ms;
}

void aar_serve_display_info_request(struct aar_serve_display *d,
                                    const struct aar_serve_req *req,
                                    struct aar_serve_done *done)
{
    aar_display_info info;

    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id) {
        done->result = AAR_E_INVAL;
        return;
    }
    memset(&info, 0, sizeof(info));
    info.screen_width = d->width;
    info.screen_height = d->height;
    if (d->ops.info)
        d->ops.info(d->ops.ctx, &info);
    /* WINDOW and SCALED are what was granted, whatever the table says. */
    info.flags &= ~(uint32_t)(AAR_DISPINFO_WINDOW | AAR_DISPINFO_SCALED);
    if (d->caps & AAR_DISPCAP_WINDOW)
        info.flags |= AAR_DISPINFO_WINDOW;
    if (d->caps & AAR_DISPCAP_SCALED)
        info.flags |= AAR_DISPINFO_SCALED;
    done->result = AAR_OK;
    done->out[AAR_R_DISPLAY_INFO_WIDTH] = d->width;
    done->out[AAR_R_DISPLAY_INFO_HEIGHT] = d->height;
    done->out[AAR_R_DISPLAY_INFO_REFRESH_MHZ] = info.refresh_mhz;
    done->out[AAR_R_DISPLAY_INFO_SCREEN_W] = info.screen_width;
    done->out[AAR_R_DISPLAY_INFO_SCREEN_H] = info.screen_height;
    done->out[AAR_R_DISPLAY_INFO_FLAGS] = info.flags;
}

void aar_serve_display_modes_request(struct aar_serve_display *d,
                                     const struct aar_serve_req *req,
                                     struct aar_serve_done *done)
{
    uint32_t room = req->buf ? req->buf_len / AAR_DISPLAY_MODE_REC_BYTES : 0u;
    uint32_t max = room < AAR_SERVE_MODES_MAX ? room : AAR_SERVE_MODES_MAX;
    uint32_t total = 0, n, i;

    done_clear(done);
    if (d->ops.modes) {
        memset(d->mode_list, 0, max * sizeof(d->mode_list[0]));
        total = d->ops.modes(d->ops.ctx, d->mode_list, max);
    }
    n = total < max ? total : max;
    for (i = 0; i < n; i++)
        aar_serve_mode_encode(&d->mode_list[i],
                              req->buf + i * AAR_DISPLAY_MODE_REC_BYTES);
    done->result = AAR_OK;
    done->out[AAR_R_DISPLAY_MODES_COUNT] = n;
    done->out[AAR_R_DISPLAY_MODES_TOTAL] = total;
}

void aar_serve_display_resize_request(struct aar_serve_display *d,
                                      const struct aar_serve_req *req,
                                      struct aar_serve_done *done)
{
    struct aar_serve_req open_req;
    uint32_t w = req->arg[AAR_A_DISPLAY_RESIZE_W];
    uint32_t h = req->arg[AAR_A_DISPLAY_RESIZE_H];

    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (!(d->caps & AAR_DISPCAP_RESIZE) || !d->ops.resize) {
        done->result = AAR_E_NOSYS;
        return;
    }
    if (w == 0 || h == 0 || w > AAR_DISPLAY_MAX_DIM || h > AAR_DISPLAY_MAX_DIM) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (d->flip_active || d->flip_queued) {
        done->result = AAR_E_BUSY;
        return;
    }
    /* The table sees the request as the open it replaces. */
    open_req = *req;
    open_req.arg[AAR_A_DISPLAY_OPEN_W] = w;
    open_req.arg[AAR_A_DISPLAY_OPEN_H] = h;
    open_req.arg[AAR_A_DISPLAY_OPEN_FORMAT] = d->format;
    open_req.arg[AAR_A_DISPLAY_OPEN_FLAGS] = d->flags;
    d->ops.resize(d->ops.ctx, &open_req, done);
    if (done->result != AAR_OK)
        return;
    if (done->out[AAR_R_DISPLAY_OPEN_FRONT] == 0u ||
        done->out[AAR_R_DISPLAY_OPEN_PITCH] == 0u) {
        done_result(done, AAR_E_IO);
        return;
    }
    aar_serve_display_opened(d, &open_req, done->out[AAR_R_DISPLAY_OPEN_PITCH],
                             done->out[AAR_R_DISPLAY_OPEN_FRONT],
                             done->out[AAR_R_DISPLAY_OPEN_BACK],
                             done->out[AAR_R_DISPLAY_OPEN_SPARE], d->have_two,
                             d->have_pan, d->pan_count, d->features, done);
}

void aar_serve_display_title_request(struct aar_serve_display *d,
                                     const struct aar_serve_req *req,
                                     struct aar_serve_done *done)
{
    char title[256];
    uint32_t n;

    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id || !req->buf) {
        done->result = AAR_E_INVAL;
        return;
    }
    /* At most 255 bytes, and the NUL inside the buffer. */
    for (n = 0; n < req->buf_len && n < 256u; n++) {
        if (req->buf[n] == 0)
            break;
    }
    if (n >= req->buf_len || n >= 256u) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (!d->ops.set_title) {
        done->result = AAR_E_NOSYS;
        return;
    }
    title[aar_serve_utf8_to_latin1(req->buf, n, (uint8_t *)title)] = '\0';
    done->result = d->ops.set_title(d->ops.ctx, title);
}

void aar_serve_display_pointer_request(struct aar_serve_display *d,
                                       const struct aar_serve_req *req,
                                       struct aar_serve_done *done)
{
    uint32_t mode = req->arg[AAR_A_DISPLAY_POINTER_MODE];
    uint32_t w = req->arg[AAR_A_DISPLAY_POINTER_W];
    uint32_t h = req->arg[AAR_A_DISPLAY_POINTER_H];
    int32_t hot_x = (int32_t)req->arg[AAR_A_DISPLAY_POINTER_HOT_X];
    int32_t hot_y = (int32_t)req->arg[AAR_A_DISPLAY_POINTER_HOT_Y];

    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id || mode > AAR_POINTER_CUSTOM) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (mode == AAR_POINTER_CUSTOM &&
        (w == 0 || h == 0 || w > AAR_POINTER_MAX_DIM || h > AAR_POINTER_MAX_DIM ||
         hot_x < 0 || hot_y < 0 || hot_x >= (int32_t)w || hot_y >= (int32_t)h ||
         !req->buf || req->buf_len < w * h * 4u)) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (!d->ops.set_pointer) {
        done->result = AAR_E_NOSYS;
        return;
    }
    done->result = d->ops.set_pointer(d->ops.ctx, mode, w, h, hot_x, hot_y,
                                      mode == AAR_POINTER_CUSTOM ? req->buf : NULL);
}

void aar_serve_display_vblank_request(struct aar_serve_display *d,
                                      const struct aar_serve_req *req,
                                      struct aar_serve_done *done, int *deferred)
{
    uint32_t us = req->arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US];
    uint32_t ms;

    *deferred = 0;
    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id || us == 0) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (d->vb_active) {
        done->result = AAR_E_BUSY;
        return;
    }
    if (!d->ops.vblank || !d->ops.vblank(d->ops.ctx, 1)) {
        done->result = AAR_E_NOSYS;
        return;
    }
    ms = us / 1000u + ((us % 1000u) ? 1u : 0u);
    d->vb_req = *req;
    d->vb_active = 1;
    d->vb_deadline_ms = d->now_ms + ms;
    *deferred = 1;
}

void aar_serve_display_vblank_seen(struct aar_serve_display *d)
{
    vblank_finish(d, AAR_OK);
}

void aar_serve_display_vblank_check(struct aar_serve_display *d, uint32_t now_ms)
{
    if (d->vb_active && (int32_t)(now_ms - d->vb_deadline_ms) >= 0)
        vblank_finish(d, AAR_E_TIMEDOUT);
}

uint32_t aar_serve_display_vblank_wait_us(const struct aar_serve_display *d,
                                          uint32_t now_ms)
{
    int32_t left;

    if (!d->vb_active)
        return 0xFFFFFFFFu;
    left = (int32_t)(d->vb_deadline_ms - now_ms);
    if (left <= 0)
        return 1u;
    if (left > 4000000)
        left = 4000000;
    return (uint32_t)left * 1000u;
}

/* ---- Display geometry, pointer and text (pure) -------------------------- */

void aar_serve_fit_rect(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                        uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    uint32_t fw, fh;

    if (!sw || !sh || !dw || !dh) {
        *x = *y = *w = *h = 0;
        return;
    }
    if (dw * sh <= dh * sw) {
        fw = dw;
        fh = sh * dw / sw;
    } else {
        fh = dh;
        fw = sw * dh / sh;
    }
    if (fw == 0)
        fw = 1;
    if (fh == 0)
        fh = 1;
    *x = (dw - fw) / 2u;
    *y = (dh - fh) / 2u;
    *w = fw;
    *h = fh;
}

static int32_t floor_div(int32_t a, int32_t b)
{
    int32_t q = a / b;

    if (a % b < 0)
        q--;
    return q;
}

void aar_serve_surface_pos(int32_t mx, int32_t my, uint32_t sw, uint32_t sh,
                           uint32_t fit_x, uint32_t fit_y, uint32_t fit_w,
                           uint32_t fit_h, int32_t *sx, int32_t *sy)
{
    if (fit_w == 0 || fit_h == 0) {
        *sx = mx;
        *sy = my;
        return;
    }
    *sx = floor_div((mx - (int32_t)fit_x) * (int32_t)sw, (int32_t)fit_w);
    *sy = floor_div((my - (int32_t)fit_y) * (int32_t)sh, (int32_t)fit_h);
}

/* The source index of destination index i is (2i + 1) * n / (2m) rounded down
 * (the pixel centres). Each axis keeps acc = (2i + 1) * n - 2m * index and
 * steps it by 2n, so a row needs no multiplication or division. The rows y0 to
 * y0 + rows - 1 of the dw x dh result are written to dst, from its first row
 * on; dw and dh up to 16384. */
void aar_serve_scale_nn(const uint8_t *src, uint32_t src_pitch, uint32_t sw,
                        uint32_t sh, uint8_t *dst, uint32_t dst_pitch,
                        uint32_t dw, uint32_t dh, uint32_t bpp, uint32_t y0,
                        uint32_t rows)
{
    uint32_t x, y, t = (2u * y0 + 1u) * sh;
    uint32_t sy = t / (2u * dh);
    uint32_t ay = t - sy * 2u * dh;

    for (y = 0; y < rows; y++) {
        const uint8_t *srow;
        uint32_t sx = 0, ax = sw;

        while (ay >= 2u * dh) {
            ay -= 2u * dh;
            sy++;
        }
        ay += 2u * sh;
        srow = src + sy * src_pitch;
        if (bpp == 4u) {
            const uint32_t *s32 = (const uint32_t *)(const void *)srow;
            uint32_t *d32 = (uint32_t *)(void *)(dst + y * dst_pitch);

            for (x = 0; x < dw; x++) {
                while (ax >= 2u * dw) {
                    ax -= 2u * dw;
                    sx++;
                }
                ax += 2u * sw;
                d32[x] = s32[sx];
            }
        } else if (bpp == 2u) {
            const uint16_t *s16 = (const uint16_t *)(const void *)srow;
            uint16_t *d16 = (uint16_t *)(void *)(dst + y * dst_pitch);

            for (x = 0; x < dw; x++) {
                while (ax >= 2u * dw) {
                    ax -= 2u * dw;
                    sx++;
                }
                ax += 2u * sw;
                d16[x] = s16[sx];
            }
        } else {
            uint8_t *d8 = dst + y * dst_pitch;

            for (x = 0; x < dw; x++) {
                while (ax >= 2u * dw) {
                    ax -= 2u * dw;
                    sx++;
                }
                ax += 2u * sw;
                d8[x] = srow[sx];
            }
        }
    }
}

int aar_serve_wheel_key(uint16_t raw, int16_t *dx, int16_t *dy)
{
    *dx = 0;
    *dy = 0;
    switch (raw) {
    case 0x7Au: *dy = -1; return 1;
    case 0x7Bu: *dy = 1; return 1;
    case 0x7Cu: *dx = -1; return 1;
    case 0x7Du: *dx = 1; return 1;
    default: return 0;
    }
}

uint32_t aar_serve_text_chars(const uint8_t *mapped, int32_t n, uint16_t qualifiers,
                              uint16_t command_mask, uint16_t *out, uint32_t max)
{
    uint32_t count = 0;
    int32_t i = 0;

    if (qualifiers & command_mask)
        return 0;
    while (i < n) {
        uint8_t c = mapped[i++];

        if (c == 0x9Bu) {
            /* A CSI sequence: parameters, intermediates, a final byte. */
            while (i < n && mapped[i] >= 0x20u && mapped[i] <= 0x3Fu)
                i++;
            if (i < n && mapped[i] >= 0x40u && mapped[i] <= 0x7Eu)
                i++;
            continue;
        }
        if (c < 0x20u || (c >= 0x7Fu && c <= 0x9Fu))
            continue;
        if (count < max)
            out[count++] = c;
    }
    return count;
}


uint32_t aar_serve_pointer_pixel(uint32_t argb, const uint32_t palette[3])
{
    int32_t r = (int32_t)((argb >> 16) & 0xFFu);
    int32_t g = (int32_t)((argb >> 8) & 0xFFu);
    int32_t b = (int32_t)(argb & 0xFFu);
    uint32_t best = 1, best_d = 0xFFFFFFFFu, k;

    if ((argb >> 24) < 0x80u)
        return 0;
    for (k = 0; k < 3; k++) {
        int32_t dr = r - (int32_t)((palette[k] >> 16) & 0xFFu);
        int32_t dg = g - (int32_t)((palette[k] >> 8) & 0xFFu);
        int32_t db = b - (int32_t)(palette[k] & 0xFFu);
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);

        if (dist < best_d) {
            best_d = dist;
            best = k + 1u;
        }
    }
    return best;
}

void aar_serve_pointer_planes(const uint8_t *argb_be, uint32_t w, uint32_t h,
                              const uint32_t palette[3], uint8_t *plane0,
                              uint8_t *plane1, uint32_t bytes_per_row)
{
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        uint8_t *row0 = plane0 + y * bytes_per_row;
        uint8_t *row1 = plane1 + y * bytes_per_row;

        memset(row0, 0, bytes_per_row);
        memset(row1, 0, bytes_per_row);
        for (x = 0; x < w; x++) {
            uint32_t p = aar_serve_pointer_pixel(
                aar_serve_be32(argb_be + (y * w + x) * 4u), palette);
            uint8_t bit = (uint8_t)(0x80u >> (x & 7u));

            if (p & 1u)
                row0[x >> 3] |= bit;
            if (p & 2u)
                row1[x >> 3] |= bit;
        }
    }
}

uint32_t aar_serve_refresh_mhz(uint32_t total_rows, uint32_t total_clocks,
                               uint32_t clock_hz)
{
    uint32_t div, mhz;

    if (!total_rows || !total_clocks || !clock_hz || total_rows > 4096u ||
        total_clocks > 4096u || clock_hz > 4000000u)
        return 0;
    div = total_rows * total_clocks;
    mhz = (clock_hz * 1000u + div / 2u) / div;
    return (mhz < 20000u || mhz > 300000u) ? 0u : mhz;
}

void aar_serve_mode_encode(const aar_display_mode *m, uint8_t *rec)
{
    aar_serve_put_be32(rec, m->width);
    aar_serve_put_be32(rec + 4, m->height);
    aar_serve_put_be32(rec + 8, m->refresh_mhz);
    aar_serve_put_be32(rec + 12, m->formats);
    aar_serve_put_be32(rec + 16, m->flags);
    aar_serve_put_be32(rec + 20, 0u);
}

void aar_serve_modes_prefer(aar_display_mode *modes, uint32_t n, uint32_t wb_w,
                            uint32_t wb_h)
{
    uint32_t i;
    int found = 0;

    for (i = 0; i < n; i++) {
        if (!found && modes[i].width == wb_w && modes[i].height == wb_h) {
            modes[i].flags |= AAR_MODEF_PREFERRED;
            found = 1;
        } else {
            modes[i].flags &= ~(uint32_t)AAR_MODEF_PREFERRED;
        }
    }
}

int aar_serve_cd32_valid(uint32_t jp)
{
    uint32_t type = jp & AAR_SERVE_JP_TYPE_MASK;

    return type == AAR_SERVE_JP_TYPE_GAMECTLR || type == AAR_SERVE_JP_TYPE_JOYSTK;
}

uint16_t aar_serve_cd32_buttons(uint32_t jp)
{
    uint16_t b = 0;

    if (jp & AAR_SERVE_JPF_RED)
        b |= AAR_JOY_BUTTON1;
    if (jp & AAR_SERVE_JPF_BLUE)
        b |= AAR_JOY_BUTTON2;
    if (jp & AAR_SERVE_JPF_GREEN)
        b |= AAR_JOY_BUTTON3;
    if (jp & AAR_SERVE_JPF_YELLOW)
        b |= AAR_JOY_BUTTON4;
    if (jp & AAR_SERVE_JPF_PLAY)
        b |= AAR_JOY_PLAY;
    if (jp & AAR_SERVE_JPF_FORWARD)
        b |= AAR_JOY_FORWARD;
    if (jp & AAR_SERVE_JPF_REVERSE)
        b |= AAR_JOY_REVERSE;
    return b;
}

void aar_serve_cd32_axes(uint32_t jp, int16_t *dx, int16_t *dy)
{
    *dx = (jp & AAR_SERVE_JPF_LEFT) ? -1 : ((jp & AAR_SERVE_JPF_RIGHT) ? 1 : 0);
    *dy = (jp & AAR_SERVE_JPF_UP) ? -1 : ((jp & AAR_SERVE_JPF_DOWN) ? 1 : 0);
}

/* ---- Audio -------------------------------------------------------------- */

void aar_serve_audio_init(struct aar_serve_audio *a, const struct aar_serve_audio_ops *ops)
{
    memset(a, 0, sizeof(*a));
    a->ops = *ops;
}

void aar_serve_audio_open_request(struct aar_serve_audio *a, const struct aar_serve_req *req,
                                  struct aar_serve_done *done)
{
    uint32_t rate = req->arg[AAR_A_AUDIO_OPEN_RATE];
    uint32_t channels = req->arg[AAR_A_AUDIO_OPEN_CHANNELS];
    uint32_t route = req->arg[AAR_A_AUDIO_OPEN_ROUTE];
    uint32_t frames = 0;
    uint32_t room;

    done_clear(done);
    /* The platform route never reaches the client. */
    if (route != AAR_AUDIO_ROUTE_AHI || rate == 0 || channels != 2u ||
        req->buf_token == 0 || req->buf_off != 0 ||
        req->buf_len <= AAR_AUDIO_RING_HDR_BYTES) {
        done->result = AAR_E_INVAL;
        return;
    }
    /* The ring holds a power of two of frames after the header. */
    room = (req->buf_len - AAR_AUDIO_RING_HDR_BYTES) / (channels * 2u);
    for (frames = 1u; frames <= room / 2u; frames <<= 1)
        ;
    if (room == 0) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (a->open) {
        a->ops.close(a->ops.ctx);
        a->open = 0;
    }
    if (!a->ops.open(a->ops.ctx, rate, channels, req->buf_token, req->buf_len,
                     frames)) {
        a->open_failed = 1;
        done->result = AAR_E_IO;
        return;
    }
    a->open = 1;
    a->rate = rate;
    a->channels = channels;
    a->frames = frames;
    done->result = AAR_OK;
    done->out[AAR_R_AUDIO_OPEN_RATE] = rate;
    done->out[AAR_R_AUDIO_OPEN_CHANNELS] = channels;
    done->out[AAR_R_AUDIO_OPEN_RING] = req->buf_token;
    done->out[AAR_R_AUDIO_OPEN_RING_BYTES] = req->buf_len;
    done->out[AAR_R_AUDIO_OPEN_FRAMES] = frames;
}

void aar_serve_audio_close_request(struct aar_serve_audio *a,
                                   const struct aar_serve_req *req,
                                   struct aar_serve_done *done)
{
    (void)req;
    done_clear(done);
    if (a->open) {
        a->ops.close(a->ops.ctx);
        a->open = 0;
    }
    done->result = AAR_OK;
}

void aar_serve_audio_refused(struct aar_serve_audio *a)
{
    if (a->open) {
        a->ops.close(a->ops.ctx);
        a->open = 0;
    }
}

uint32_t aar_serve_audio_chunk_frames(uint32_t available_frames, uint32_t rate,
                                      uint32_t channels, uint32_t buffer_bytes)
{
    uint32_t frames = available_frames;
    uint32_t tick = rate / 50u * 3u; /* 60 ms */
    uint32_t fit = channels ? buffer_bytes / (channels * 2u) : 0u;

    if (tick < frames)
        frames = tick;
    if (fit < frames)
        frames = fit;
    return frames;
}

/* ---- Events ------------------------------------------------------------- */

void aar_serve_events_init(struct aar_serve_events *e, const struct aar_serve_events_ops *ops)
{
    memset(e, 0, sizeof(*e));
    e->ops = *ops;
}

int aar_serve_events_push(struct aar_serve_events *e, uint16_t type, uint16_t code,
                          int16_t dx, int16_t dy, uint16_t buttons,
                          uint32_t time_ms)
{
    aar_event ev;
    int rc;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    ev.time_ms = time_ms;
    if (type == AAR_EV_STOP) {
        if (e->stop_pending)
            return AAR_OK;
        rc = e->ops.post(e->ops.ctx, &ev);
        if (rc == AAR_E_BUSY) {
            e->stop_pending = 1;
            e->stop = ev;
            return AAR_OK;
        }
        return rc;
    }
    return e->ops.post(e->ops.ctx, &ev);
}

void aar_serve_events_flush(struct aar_serve_events *e)
{
    if (e->stop_pending && e->ops.post(e->ops.ctx, &e->stop) != AAR_E_BUSY)
        e->stop_pending = 0;
}

int aar_serve_events_pending(const struct aar_serve_events *e)
{
    return e->stop_pending;
}

/* ---- Dispatch ----------------------------------------------------------- */

void aar_serve_core_init(struct aar_serve_core *s, const struct aar_serve_sink *sink)
{
    memset(s, 0, sizeof(*s));
    s->sink = *sink;
}

/* Every request a handler still holds is completed with AAR_E_IO, once: the
 * serving state is going away and the app must not stay parked on them. */
void aar_serve_core_abort(struct aar_serve_core *s)
{
    struct aar_serve_console *c = &s->console;
    struct aar_serve_display *d = &s->display;

    if (c->app_write_active)
        app_write_finish(c, AAR_E_IO);
    if (c->read_active)
        read_finish(c, AAR_E_IO, 0);
    if (d->flip_active) {
        struct aar_serve_req finished = d->flip_req;

        d->flip_active = 0;
        flip_complete(d, &finished, AAR_E_IO);
    }
    if (d->flip_queued) {
        struct aar_serve_req next = d->flip_next;

        d->flip_queued = 0;
        flip_complete(d, &next, AAR_E_IO);
    }
    vblank_finish(d, AAR_E_IO);
}

int aar_serve_core_request(struct aar_serve_core *s, const struct aar_serve_req *req,
                           struct aar_serve_done *done)
{
    int deferred = 0;

    switch (req->op) {
    case AAR_OP_FILE_OPEN:
    case AAR_OP_FILE_CLOSE:
    case AAR_OP_FILE_READ:
    case AAR_OP_FILE_WRITE:
    case AAR_OP_FILE_SEEK:
    case AAR_OP_FILE_STAT:
    case AAR_OP_LIST_OPEN:
    case AAR_OP_LIST_NEXT:
    case AAR_OP_LIST_CLOSE:
    case AAR_OP_FILE_DELETE:
    case AAR_OP_FILE_RENAME:
    case AAR_OP_DIR_CREATE:
    case AAR_OP_FILE_SET_DATE:
    case AAR_OP_FILE_SET_PROTECT:
    case AAR_OP_FILE_TRUNCATE:
    case AAR_OP_VOLUME_INFO:
        aar_serve_files_serve(&s->files, req, done);
        break;
    case AAR_OP_TIME_UNIX:
    case AAR_OP_LOCALE_INFO:
    case AAR_OP_ENV_GET:
    case AAR_OP_CLIP_READ:
    case AAR_OP_CLIP_WRITE:
        aar_serve_host_serve(&s->host, req, done);
        break;
    case AAR_OP_CONSOLE_WRITE:
        aar_serve_console_write_request(&s->console, req, done, &deferred);
        break;
    case AAR_OP_CONSOLE_READ:
        aar_serve_console_read_request(&s->console, req, done, &deferred);
        break;
    case AAR_OP_DISPLAY_OPEN:
        done_clear(done);
        if (aar_serve_display_open_check(req) != AAR_OK)
            done->result = AAR_E_INVAL;
        else if (s->display.open)
            done->result = AAR_E_BUSY;
        else {
            s->display.ops.open(s->display.ops.ctx, req, done);
            if (done->result == AAR_OK && !s->display.open)
                display_adopt(&s->display, req, done);
        }
        break;
    case AAR_OP_DISPLAY_FLIP:
        aar_serve_display_flip_request(&s->display, req, done, &deferred);
        break;
    case AAR_OP_DISPLAY_PALETTE:
        aar_serve_display_palette_request(&s->display, req, done);
        break;
    case AAR_OP_DISPLAY_CLOSE:
        aar_serve_display_close_request(&s->display, req, done);
        break;
    case AAR_OP_DISPLAY_INFO:
        aar_serve_display_info_request(&s->display, req, done);
        break;
    case AAR_OP_DISPLAY_MODES:
        aar_serve_display_modes_request(&s->display, req, done);
        break;
    case AAR_OP_DISPLAY_RESIZE:
        aar_serve_display_resize_request(&s->display, req, done);
        break;
    case AAR_OP_DISPLAY_TITLE:
        aar_serve_display_title_request(&s->display, req, done);
        break;
    case AAR_OP_DISPLAY_POINTER:
        aar_serve_display_pointer_request(&s->display, req, done);
        break;
    case AAR_OP_DISPLAY_VBLANK:
        aar_serve_display_vblank_request(&s->display, req, done, &deferred);
        break;
    case AAR_OP_AUDIO_OPEN:
        aar_serve_audio_open_request(&s->audio, req, done);
        break;
    case AAR_OP_AUDIO_CLOSE:
        aar_serve_audio_close_request(&s->audio, req, done);
        break;
    default:
        done_result(done, AAR_E_NOSYS);
        break;
    }
    return deferred ? AAR_SERVE_REQ_DEFERRED : AAR_SERVE_REQ_DONE;
}

/* ---- DOS packet release helpers ----------------------------------------- */

static int aar_serve_dos_release(const struct aar_serve_dos_release_ops *ops,
                                 int32_t *result)
{
    if (!ops || !ops->send_release)
        return 0;
    return ops->send_release(ops->context, result);
}

int aar_serve_dos_close_slot(const struct aar_serve_dos_release_ops *ops,
                             int *delivered_out)
{
    int32_t result = 0;
    int delivered = aar_serve_dos_release(ops, &result);

    if (delivered_out)
        *delivered_out = delivered;
    if (!delivered)
        return 0;
    if (ops && ops->free_local)
        ops->free_local(ops->context);
    if (ops && ops->forget_slot)
        ops->forget_slot(ops->context);
    return result != 0;
}

void aar_serve_dos_end_or_close(const struct aar_serve_dos_release_ops *ops)
{
    int32_t result;

    if (!aar_serve_dos_release(ops, &result)) {
        if (ops && ops->fallback_release)
            ops->fallback_release(ops->context);
        return;
    }
    if (ops && ops->free_local)
        ops->free_local(ops->context);
}

void aar_serve_dos_release_abandoned(const struct aar_serve_dos_release_ops *ops,
                                     int acquired)
{
    if (acquired)
        aar_serve_dos_end_or_close(ops);
}
