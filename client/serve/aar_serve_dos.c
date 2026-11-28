/*
 * aar_serve: the DOS backend of the file handlers and the asynchronous packet engine
 * under it (every DOS call is a packet, so a slow handler never stops the loop's
 * Ctrl-C).
 *
 * This file is part of aar_serve.c, which includes it after the services of the
 * loop and before the public calls; it is not compiled alone. State lives in the
 * instance (struct aar_serve in aar_serve_priv.h).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

static int dos_packet_async(struct aar_serve *s, struct MsgPort *handler,
                            LONG action, LONG arg1, LONG arg2, LONG arg3,
                            LONG arg4, LONG arg5, int check_live,
                            LONG *result_out, int *ownership_lost,
                            int *abandoned_out);
static LONG file_packet_async(struct aar_serve *s, BPTR fh, LONG action,
                              LONG arg2, LONG arg3, LONG arg4, LONG arg5,
                              int *transport_ok);
static int examine_lock_async(struct aar_serve *s, BPTR lock,
                              struct FileInfoBlock *fib, int *transport_ok);
static int examine_next_async(struct aar_serve *s, BPTR lock,
                              struct FileInfoBlock *fib, int *transport_ok);
static BPTR lock_async(struct aar_serve *s, BPTR dir, const char *name,
                       LONG access, int *transport_ok);
static BPTR lock_abs_async(struct aar_serve *s, const char *name,
                           int *transport_ok);

static void release_dos_path_resolver(struct aar_serve_resolver *resolver);
static int dos_path_resolver_start(struct aar_serve_resolver *resolver,
                                   BPTR dir, const char *name);
static BPTR lock_resolved_async(struct aar_serve *s,
                                struct aar_serve_resolver *resolver,
                                const char *name,
                                struct aar_serve_dos_path_candidate *selected,
                                int *transport_ok);
static BPTR open_resolved_async(struct aar_serve *s,
                                struct aar_serve_resolver *resolver,
                                const char *name, LONG mode, int *transport_ok);
static int end_async(struct aar_serve *s, BPTR fh, int *transport_ok);
static void end_or_close(struct aar_serve *s, BPTR fh);
static int be_target_named(void *ctx, enum aar_serve_named kind,
                           const struct aar_serve_named_args *args,
                           int *transport_ok, int32_t *io);
static int be_rename(void *ctx, uintptr_t from_dir, const char *from_name,
                     uintptr_t to_dir, const char *to_name, int *transport_ok,
                     int32_t *io);
static int be_volume_info(void *ctx, uintptr_t lock, struct aar_serve_volume *out,
                          int *transport_ok, int32_t *io);
static int be_same_volume(void *ctx, uintptr_t from_dir, const char *from_name,
                          uintptr_t to_dir, const char *to_name);
static int32_t be_check_replace(void *ctx, uintptr_t source, uintptr_t target,
                                const char *from_abs, const char *to_abs);
static int be_utc_offset(void *ctx, int32_t *east_min);

/* ---- The DOS backend of the file handlers --------------------------------
 *
 * aar_serve_core.c decides what each request means (open plans, ROOT
 * confinement, descriptors); these functions are the AmigaDOS calls under it.
 * Every DOS call is an asynchronous packet (dos_packet_async), so a slow
 * handler never stops the main loop's Ctrl-C. */

struct dos_list {
    BPTR lock;
    struct FileInfoBlock *fib;
};

/* Ready the resolver for the next packet. A pinned or relative resolver
 * (handler set) replays its one candidate; an unresolved absolute name
 * starts a new walk and drops the previous walk's DevProc. */
static int open_target_arm(struct aar_serve_target *target)
{
    if (target->resolver.handler) {
        target->resolver.fixed_used = 0;
        return 1;
    }
    release_dos_path_resolver(&target->resolver);
    return dos_path_resolver_start(&target->resolver, target->dir,
                                   target->name);
}

static int be_live(void *ctx)
{
    struct aar_serve *s = ctx;

    return claim_live(s);
}

static int be_user_break(void *ctx)
{
    struct aar_serve *s = ctx;

    return aar_serve_user_break(ctrl_c_set(s), s->user_stop);
}

/* The state is read now, not taken from the end of the last pass: a request
 * served in this pass must see an end the platform has just decided. */
static int be_stopping(void *ctx)
{
    struct aar_serve *s = ctx;
    uint32_t state, end_reason;

    if (claim_state(s, &state, &end_reason) == AAR_OK)
        s->app_state = state;
    return aar_serve_stopping(ctrl_c_set(s), s->user_stop, s->stop_asked,
                              s->app_state);
}

static uintptr_t be_dup_lock(void *ctx, uintptr_t lock)
{
    (void)ctx;
    return (uintptr_t)X_DUP_LOCK((BPTR)lock);
}

static void be_unlock(void *ctx, uintptr_t lock)
{
    (void)ctx;
    X_UNLOCK((BPTR)lock);
}

static uintptr_t be_parent_dir(void *ctx, uintptr_t lock)
{
    (void)ctx;
    return (uintptr_t)X_PARENT_DIR((BPTR)lock);
}

static int be_same_lock(void *ctx, uintptr_t a, uintptr_t b)
{
    (void)ctx;
    return X_SAME_LOCK((BPTR)a, (BPTR)b) == LOCK_SAME;
}

static uintptr_t be_lock_rel(void *ctx, uintptr_t dir, const char *name,
                             int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    BPTR lock;

    lock = lock_async(s, (BPTR)dir, name, ACCESS_READ, transport_ok);
    *io = (int32_t)X_IO_ERR();
    return (uintptr_t)lock;
}

static uintptr_t be_lock_abs(void *ctx, const char *name, int *transport_ok,
                             int32_t *io)
{
    struct aar_serve *s = ctx;
    BPTR lock;

    lock = lock_abs_async(s, name, transport_ok);
    *io = (int32_t)X_IO_ERR();
    return (uintptr_t)lock;
}

static void info_from_fib(const struct FileInfoBlock *fib,
                          struct aar_serve_info *info)
{
    memset(info, 0, sizeof(*info));
    info->is_dir = fib->fib_DirEntryType > 0;
    info->size = (uint32_t)fib->fib_Size;
    info->protect = (uint32_t)fib->fib_Protection & 0xffu;
    info->mtime = aar_serve_amiga_unix((uint32_t)fib->fib_Date.ds_Days,
                                       (uint32_t)fib->fib_Date.ds_Minute,
                                       (uint32_t)fib->fib_Date.ds_Tick);
    aar_serve_bstr_decode((const uint8_t *)fib->fib_FileName,
                          sizeof(fib->fib_FileName), info->name, sizeof(info->name));
}

static int be_lock_examine(void *ctx, uintptr_t lock, struct aar_serve_info *info,
                           int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    struct FileInfoBlock *fib = X_ALLOC_DOS_OBJECT(DOS_FIB, NULL);
    int ok;

    *transport_ok = 1;
    *io = 0;
    if (!fib) {
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    ok = examine_lock_async(s, (BPTR)lock, fib, transport_ok);
    *io = (int32_t)X_IO_ERR();
    if (ok)
        info_from_fib(fib, info);
    X_FREE_DOS_OBJECT(DOS_FIB, fib);
    return ok;
}

/* Only the comment of the object: fib_Comment as the handler returns it, a
 * BSTR of at most 79 characters. The FIB lives for this call alone. */
static int be_lock_comment(void *ctx, uintptr_t lock, uint8_t *dst, uint32_t cap,
                           uint32_t *len, int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    struct FileInfoBlock *fib = X_ALLOC_DOS_OBJECT(DOS_FIB, NULL);
    uint32_t n;
    int ok;

    *transport_ok = 1;
    *io = 0;
    *len = 0;
    if (!fib) {
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    fib->fib_Comment[0] = 0;
    ok = examine_lock_async(s, (BPTR)lock, fib, transport_ok);
    *io = (int32_t)X_IO_ERR();
    if (ok) {
        n = (uint8_t)fib->fib_Comment[0];
        if (n > AAR_FILE_COMMENT_MAX) {
            ok = 0;
            *io = 0;
        } else {
            uint32_t copy = n < cap ? n : cap;

            if (copy)
                memcpy(dst, fib->fib_Comment + 1, copy);
            *len = n;
        }
    }
    X_FREE_DOS_OBJECT(DOS_FIB, fib);
    return ok;
}

static void be_target_begin(void *ctx, uintptr_t dir, const char *name)
{
    struct aar_serve *s = ctx;

    memset(&s->dos.target, 0, sizeof(s->dos.target));
    s->dos.target.name = name;
    s->dos.target.dir = (BPTR)dir;
}

/* Does the object exist? ERROR_OBJECT_IN_USE counts as existing: the handler
 * refused the lock because someone holds the object. Any other lock failure
 * is the caller's error. An absolute name that exists pins the resolver to
 * the candidate that answered; one that does not exist stays unresolved. */
static int be_target_probe(void *ctx, int *exists, int *transport_ok,
                           int32_t *io)
{
    struct aar_serve *s = ctx;
    struct aar_serve_dos_path_candidate selected;
    enum aar_serve_probe_result found;
    BPTR locked;

    memset(&selected, 0, sizeof(selected));
    *io = 0;
    *transport_ok = 0;
    if (s->dos.target.dir) {
        locked = lock_async(s, s->dos.target.dir, s->dos.target.name, ACCESS_READ,
                            transport_ok);
    } else {
        if (!open_target_arm(&s->dos.target))
            return 0;
        locked = lock_resolved_async(s, &s->dos.target.resolver, s->dos.target.name,
                                     &selected, transport_ok);
    }
    if (!*transport_ok)
        return 0;
    *io = (int32_t)(locked ? 0 : X_IO_ERR());
    found = aar_serve_probe_result_for_lock(locked != 0, *io,
                                            ERROR_OBJECT_NOT_FOUND,
                                            ERROR_OBJECT_IN_USE);
    if (found == AAR_SERVE_PROBE_ERROR)
        return 0;
    if (locked)
        X_UNLOCK(locked);
    *exists = found == AAR_SERVE_PROBE_EXISTS;
    if (!s->dos.target.dir && *exists) {
        s->dos.target.resolver.handler = (struct MsgPort *)selected.handler;
        s->dos.target.resolver.directory = (BPTR)selected.directory;
        s->dos.target.resolver.fixed_used = 0;
    }
    return 1;
}

static uintptr_t be_target_open(void *ctx, enum aar_serve_mode mode,
                                int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    LONG dos_mode = mode == AAR_SERVE_MODE_NEW ? MODE_NEWFILE :
                    (mode == AAR_SERVE_MODE_READWRITE ? MODE_READWRITE :
                     MODE_OLDFILE);
    BPTR fh;

    *transport_ok = 0;
    *io = 0;
    if (!open_target_arm(&s->dos.target))
        return 0;
    fh = open_resolved_async(s, &s->dos.target.resolver, s->dos.target.name, dos_mode,
                             transport_ok);
    *io = (int32_t)X_IO_ERR();
    return (uintptr_t)fh;
}

static void be_target_end(void *ctx)
{
    struct aar_serve *s = ctx;

    release_dos_path_resolver(&s->dos.target.resolver);
    memset(&s->dos.target, 0, sizeof(s->dos.target));
}

static int32_t be_fh_set_size(void *ctx, uintptr_t fh, uint32_t size,
                              int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    LONG result;

    result = file_packet_async(s, (BPTR)fh, ACTION_SET_FILE_SIZE, (LONG)size,
                               OFFSET_BEGINNING, 0, 0, transport_ok);
    *io = (int32_t)X_IO_ERR();
    return (int32_t)result;
}

static int32_t be_fh_read(void *ctx, uintptr_t fh, uint8_t *dst,
                          uint32_t bytes, int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    LONG result;

    result = file_packet_async(s, (BPTR)fh, ACTION_READ, (LONG)dst, (LONG)bytes,
                               0, 0, transport_ok);
    *io = (int32_t)X_IO_ERR();
    return (int32_t)result;
}

static int32_t be_fh_write(void *ctx, uintptr_t fh, const uint8_t *src,
                           uint32_t bytes, int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    LONG result;

    result = file_packet_async(s, (BPTR)fh, ACTION_WRITE, (LONG)src, (LONG)bytes,
                               0, 0, transport_ok);
    *io = (int32_t)X_IO_ERR();
    return (int32_t)result;
}

static int32_t be_fh_seek(void *ctx, uintptr_t fh, int32_t offset, int whence,
                          int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    LONG mode = whence == 0 ? OFFSET_BEGINNING :
                (whence == 1 ? OFFSET_CURRENT : OFFSET_END);
    LONG result;

    result = file_packet_async(s, (BPTR)fh, ACTION_SEEK, (LONG)offset, mode, 0, 0,
                               transport_ok);
    *io = (int32_t)X_IO_ERR();
    return (int32_t)result;
}

static void be_fh_end_or_close(void *ctx, uintptr_t fh)
{
    struct aar_serve *s = ctx;

    end_or_close(s, (BPTR)fh);
}

static int be_fh_close(void *ctx, uintptr_t fh, int *delivered, int32_t *io)
{
    struct aar_serve *s = ctx;
    int closed;

    closed = end_async(s, (BPTR)fh, delivered);
    *io = (int32_t)X_IO_ERR();
    return closed;
}

static int be_list_open(void *ctx, uintptr_t lock, uintptr_t *list,
                        int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    struct dos_list *item = X_ALLOC_VEC(sizeof(*item), MEMF_PUBLIC | MEMF_CLEAR);

    *transport_ok = 1;
    *io = 0;
    if (!item) {
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    item->fib = X_ALLOC_DOS_OBJECT(DOS_FIB, NULL);
    if (!item->fib) {
        X_FREE_VEC(item);
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    if (!examine_lock_async(s, (BPTR)lock, item->fib, transport_ok)) {
        *io = (int32_t)X_IO_ERR();
        X_FREE_DOS_OBJECT(DOS_FIB, item->fib);
        X_FREE_VEC(item);
        return 0;
    }
    if (item->fib->fib_DirEntryType < 0) {
        *io = ERROR_OBJECT_WRONG_TYPE;
        X_FREE_DOS_OBJECT(DOS_FIB, item->fib);
        X_FREE_VEC(item);
        return 0;
    }
    item->lock = (BPTR)lock;
    *list = (uintptr_t)item;
    return 1;
}

static int be_list_next(void *ctx, uintptr_t list, struct aar_serve_info *info,
                        int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    struct dos_list *item = (struct dos_list *)list;

    if (!examine_next_async(s, item->lock, item->fib, transport_ok)) {
        *io = (int32_t)X_IO_ERR();
        return 0;
    }
    info_from_fib(item->fib, info);
    return 1;
}

static void be_list_close(void *ctx, uintptr_t list)
{
    struct dos_list *item = (struct dos_list *)list;

    (void)ctx;
    if (item->fib)
        X_FREE_DOS_OBJECT(DOS_FIB, item->fib);
    if (item->lock)
        X_UNLOCK(item->lock);
    X_FREE_VEC(item);
}

/* The DOS table of the file handlers; the instance copies it and sets ctx. */
static const struct aar_serve_dos_ops aar_serve_dos_backend = {
    .ctx = NULL,
    .live = be_live,
    .user_break = be_user_break,
    .stopping = be_stopping,
    .dup_lock = be_dup_lock,
    .unlock = be_unlock,
    .parent_dir = be_parent_dir,
    .same_lock = be_same_lock,
    .lock_rel = be_lock_rel,
    .lock_abs = be_lock_abs,
    .lock_examine = be_lock_examine,
    .target_begin = be_target_begin,
    .target_probe = be_target_probe,
    .target_open = be_target_open,
    .target_end = be_target_end,
    .fh_set_size = be_fh_set_size,
    .fh_read = be_fh_read,
    .fh_write = be_fh_write,
    .fh_seek = be_fh_seek,
    .fh_end_or_close = be_fh_end_or_close,
    .fh_close = be_fh_close,
    .list_open = be_list_open,
    .list_next = be_list_next,
    .list_close = be_list_close,
    .utc_offset = be_utc_offset,
    .target_named = be_target_named,
    .rename = be_rename,
    .volume_info = be_volume_info,
    .same_volume = be_same_volume,
    .check_replace = be_check_replace,
    .lock_comment = be_lock_comment,
};

/* ---- File requester paths --------------------------------------------------- */

/* A path relative to ROOT fits the ABI even when its native spelling does not. */
#define REQUESTER_DRAWER_BYTES (2u * AAR_SERVE_PATH_BYTES)

static int requester_basename(const char *file)
{
    return file[0] && strcmp(file, ".") != 0 && strcmp(file, "..") != 0 &&
           !strpbrk(file, "/:");
}

static int requester_lock_absolute(struct aar_serve *s, const char *name,
                                   int allow_missing, uintptr_t *lock)
{
    int transport_ok;
    int32_t io;

    *lock = be_lock_abs(s, name, &transport_ok, &io);
    if (!transport_ok)
        return AAR_E_IO;
    if (*lock || (allow_missing && io == ERROR_OBJECT_NOT_FOUND))
        return AAR_OK;
    return io ? aar_serve_errno_from_io(io) : AAR_E_NOENT;
}

static int requester_examine(struct aar_serve *s, uintptr_t lock,
                             struct aar_serve_info *info)
{
    int transport_ok;
    int32_t io;

    if (be_lock_examine(s, lock, info, &transport_ok, &io))
        return AAR_OK;
    return transport_ok && io ? aar_serve_errno_from_io(io) : AAR_E_IO;
}

static int requester_initial_path(struct aar_serve *s, const char *initial,
                                  char drawer[REQUESTER_DRAWER_BYTES],
                                  char file[AAR_PATH_MAX + 1u])
{
    const struct aar_serve_files *f = &s->core.files;
    const char *colon = strchr(initial, ':');
    struct aar_serve_info info;
    uintptr_t parent = 0, target = 0;
    int rc;

    if (strlen(initial) > AAR_PATH_MAX)
        return AAR_E_INVAL;
    file[0] = 0;
    if (colon) {
        if (!f->abs_paths)
            return AAR_E_ACCES;
        rc = requester_lock_absolute(s, initial, 1, &target);
    } else {
        rc = aar_serve_requester_resolve_rel(f, initial, 1, file,
                                            &parent, &target);
    }
    if (rc != AAR_OK)
        goto done;
    if (target) {
        rc = requester_examine(s, target, &info);
        if (rc != AAR_OK)
            goto done;
        if (info.is_dir) {
            file[0] = 0;
            if (!X_NAME_FROM_LOCK((BPTR)target, (STRPTR)drawer, REQUESTER_DRAWER_BYTES))
                rc = AAR_E_IO;
            goto done;
        }
    }
    if (colon) {
        if (target) {
            /* The actual parent may be a later directory of a multi-assign. */
            parent = be_parent_dir(s, target);
            if (!parent) {
                rc = AAR_E_IO;
                goto done;
            }
            memcpy(file, info.name, strlen(info.name) + 1u);
        } else {
            const char *slash = strrchr(initial, '/');
            const char *leaf = (slash && slash > colon ? slash : colon) + 1;
            size_t prefix = (size_t)(leaf - initial);

            /* Keep the separator; a run of slashes can include parent steps. */
            memcpy(drawer, initial, prefix);
            drawer[prefix] = 0;
            memcpy(file, leaf, strlen(leaf) + 1u);
            rc = requester_lock_absolute(s, drawer, 0, &parent);
            if (rc != AAR_OK)
                goto done;
        }
    }
    if (!requester_basename(file)) {
        rc = AAR_E_INVAL;
        goto done;
    }
    rc = requester_examine(s, parent, &info);
    if (rc == AAR_OK &&
        (!info.is_dir ||
         !X_NAME_FROM_LOCK((BPTR)parent, (STRPTR)drawer, REQUESTER_DRAWER_BYTES)))
        rc = AAR_E_IO;
done:
    if (target)
        be_unlock(s, target);
    if (parent)
        be_unlock(s, parent);
    return rc;
}

static int requester_selected_path(struct aar_serve *s, const char *drawer,
                                   const char *file, int save,
                                   char path[AAR_PATH_MAX + 1u])
{
    const struct aar_serve_files *f = &s->core.files;
    char physical[REQUESTER_DRAWER_BYTES];
    char name[REQUESTER_DRAWER_BYTES];
    const char *base = physical;
    struct aar_serve_info info;
    uintptr_t lock = 0, parent = 0, target = 0;
    size_t base_len, file_len, sep;
    int rc;

    if (!requester_basename(file))
        return AAR_E_INVAL;
    file_len = strlen(file);
    if (file_len > AAR_PATH_MAX)
        return AAR_E_NOSPC;
    if (strchr(drawer, ':')) {
        /* ASL returns a native drawer, which can exceed the packet's BSTR. */
        lock = X_LOCK((CONST_STRPTR)drawer, ACCESS_READ);
        rc = lock ? AAR_OK : aar_serve_errno_from_io((int32_t)X_IO_ERR());
        if (!lock && rc == AAR_OK)
            rc = AAR_E_IO;
    } else {
        rc = aar_serve_requester_resolve_rel(f, drawer, 0, name, &parent, &lock);
        if (parent) {
            be_unlock(s, parent);
            parent = 0;
        }
    }
    if (rc != AAR_OK)
        goto done;
    rc = requester_examine(s, lock, &info);
    if (rc != AAR_OK)
        goto done;
    if (!info.is_dir ||
        !X_NAME_FROM_LOCK((BPTR)lock, (STRPTR)physical, sizeof(physical))) {
        rc = AAR_E_IO;
        goto done;
    }
    if (!f->abs_paths) {
        size_t root_len;

        if (!X_NAME_FROM_LOCK((BPTR)f->root, (STRPTR)name, sizeof(name))) {
            rc = AAR_E_IO;
            goto done;
        }
        root_len = strlen(name);
        if (!root_len || strncmp(physical, name, root_len) != 0) {
            rc = AAR_E_ACCES;
            goto done;
        }
        base = physical + root_len;
        if (*base && name[root_len - 1u] != ':') {
            if (*base != '/') {
                rc = AAR_E_ACCES;
                goto done;
            }
            base++;
        }
    }
    base_len = strlen(base);
    sep = base_len && base[base_len - 1u] != ':' && base[base_len - 1u] != '/';
    if (base_len + sep + file_len > AAR_PATH_MAX) {
        rc = AAR_E_NOSPC;
        goto done;
    }
    memcpy(path, base, base_len);
    if (sep)
        path[base_len] = '/';
    memcpy(path + base_len + sep, file, file_len + 1u);
    if (f->abs_paths) {
        rc = requester_lock_absolute(s, path, save, &target);
    } else {
        /* The strings only format the answer. The ordinary file resolver must
         * reach the same physical drawer and never follow a leaf outside ROOT. */
        rc = aar_serve_requester_resolve_rel(f, path, save, name, &parent, &target);
        if (rc == AAR_OK && X_SAME_LOCK((BPTR)parent, (BPTR)lock) != LOCK_SAME)
            rc = AAR_E_ACCES;
    }
    if (rc == AAR_OK && target) {
        rc = requester_examine(s, target, &info);
        if (rc == AAR_OK && info.is_dir)
            rc = AAR_E_INVAL;
    }
done:
    if (target)
        be_unlock(s, target);
    if (parent)
        be_unlock(s, parent);
    if (lock)
        be_unlock(s, lock);
    return rc;
}

/*
 * A DOS packet may block on slow media. aar_serve waits on a reply port it owns
 * together with its poll timer and Ctrl-C, asking the library on each tick
 * whether the claim is still live (the library renews the lease itself).
 * Only the reply ends the wait: packet and buffers stay alive until then,
 * even after Ctrl-C or a lost claim.
 */
static int dos_packet_async(struct aar_serve *s, struct MsgPort *handler,
                            LONG action, LONG arg1, LONG arg2, LONG arg3,
                            LONG arg4, LONG arg5, int check_live,
                            LONG *result_out, int *ownership_lost,
                            int *abandoned_out)
{
    struct MsgPort *port;
    struct StandardPacket *packet;
    struct Message *message;
    ULONG mask;
    ULONG signals;
    int abandoned = 0;
    int ctrl_c;
    int owned;

    if (result_out)
        *result_out = -1;
    if (ownership_lost)
        *ownership_lost = 0;
    if (abandoned_out)
        *abandoned_out = 0;
    if (!handler || !result_out)
        return 0;
    /* A packet whose operation is already lost is not sent at all: the
     * handler would act on it (create, truncate, write) before the result
     * is discarded as abandoned. Releases always go out. */
    ctrl_c = ctrl_c_set(s);
    owned = !check_live || claim_live(s);
    if (!aar_serve_packet_may_send(action == ACTION_END ||
                                   action == ACTION_FREE_LOCK, owned, ctrl_c)) {
        if (ctrl_c) {
            X_SET_SIGNAL(0, SIGBREAKF_CTRL_C);
            s->user_stop = 1;
            if (aar_serve_ctrl_c_must_request_stop(1))
                want_stop(s, AAR_SERVE_WHY_BREAK);
        }
        if (!owned && ownership_lost)
            *ownership_lost = 1;
        *result_out = 0;
        if (abandoned_out)
            *abandoned_out = 1;
        X_SET_IO_ERR(ERROR_BREAK);
        return 1;
    }
    port = X_CREATE_PORT();
    packet = X_ALLOC_MEM(sizeof(*packet), MEMF_PUBLIC | MEMF_CLEAR);
    if (!port || !packet) {
        if (packet)
            X_FREE_MEM(packet, sizeof(*packet));
        if (port)
            X_DELETE_PORT(port);
        X_SET_IO_ERR(aar_serve_dos_error_code(0, 0, ERROR_NO_FREE_STORE));
        return 0;
    }
    packet->sp_Msg.mn_Node.ln_Name = (char *)&packet->sp_Pkt;
    packet->sp_Pkt.dp_Link = &packet->sp_Msg;
    packet->sp_Pkt.dp_Port = port;
    packet->sp_Pkt.dp_Type = action;
    packet->sp_Pkt.dp_Arg1 = arg1;
    packet->sp_Pkt.dp_Arg2 = arg2;
    packet->sp_Pkt.dp_Arg3 = arg3;
    packet->sp_Pkt.dp_Arg4 = arg4;
    packet->sp_Pkt.dp_Arg5 = arg5;
    X_PUT_MSG(handler, &packet->sp_Msg);
    timer_reap(s);
    timer_arm(s);
    mask = (1UL << port->mp_SigBit) | (s->cfg.break_mask & SIGBREAKF_CTRL_C);
    if (s->tm.port)
        mask |= 1UL << s->tm.port->mp_SigBit;
    for (;;) {
        signals = X_WAIT(mask);
        if ((signals & SIGBREAKF_CTRL_C) || ctrl_c_set(s)) {
            X_SET_SIGNAL(0, SIGBREAKF_CTRL_C);
            s->user_stop = 1;
            if (aar_serve_ctrl_c_must_request_stop(1))
                want_stop(s, AAR_SERVE_WHY_BREAK);
            abandoned = aar_serve_packet_abandoned(1, 1);
        }
        if (s->tm.port && (signals & (1UL << s->tm.port->mp_SigBit))) {
            timer_reap(s);
            if (aar_serve_packet_abandoned(0, claim_check(s)) && check_live) {
                abandoned = 1;
                if (ownership_lost)
                    *ownership_lost = 1;
            }
            if (!abandoned)
                timer_arm(s);
        }
        message = X_GET_MSG(port);
        if (!aar_serve_packet_keep_waiting(message != NULL))
            break;
    }
    timer_reap(s);
    /* The heartbeat is armed whenever it is in aar_serve_sigmask. This wait
     * may have run outside the loop (aar_serve_locate, between create and the
     * first Wait): the end of a step would not re-arm it, and the loop would
     * sleep until some other signal came. */
    if (!s->closing && !s->in_step)
        timer_arm_for(s, timed_wake_us(s));
    if (check_live && !claim_live(s)) {
        abandoned = 1;
        if (ownership_lost)
            *ownership_lost = 1;
    }
    X_SET_IO_ERR(aar_serve_dos_error_code(1, packet->sp_Pkt.dp_Res2,
                                  ERROR_NO_FREE_STORE));
    *result_out = packet->sp_Pkt.dp_Res1;
    if (abandoned_out)
        *abandoned_out = abandoned;
    X_FREE_MEM(packet, sizeof(*packet));
    X_DELETE_PORT(port);
    return 1;
}

static LONG file_packet_async(struct aar_serve *s, BPTR fh, LONG action,
                              LONG arg2, LONG arg3, LONG arg4, LONG arg5,
                              int *transport_ok)
{
    struct FileHandle *handle = (struct FileHandle *)BADDR(fh);
    LONG result;
    int abandoned;

    if (transport_ok)
        *transport_ok = 0;
    if (!handle || !handle->fh_Type || !transport_ok)
        return -1;
    if (!dos_packet_async(s, handle->fh_Type, action, handle->fh_Arg1, arg2,
                          arg3, arg4, arg5, 1, &result, NULL,
                          &abandoned) ||
        !aar_serve_packet_result_usable(1, abandoned))
        return -1;
    *transport_ok = 1;
    return result;
}

/* The flagged transfer has its own reply port and staging: neither DOS nor
 * other file requests may use the mapped request buffer as their scratch. */
static int file_transfer_flagged(const struct aar_serve_req *req)
{
    return (req->op == AAR_OP_FILE_READ || req->op == AAR_OP_FILE_WRITE) &&
           (req->flags & AAR_FILEF_ASYNC) != 0;
}

static BPTR file_transfer_handle(struct aar_serve *s, uint32_t fd)
{
    uint32_t slot = fd - AAR_FD_BASE;

    return slot < AAR_SERVE_FD_COUNT ? (BPTR)s->core.files.fh[slot] : 0;
}

static int file_transfer_busy(struct aar_serve *s, const struct aar_serve_req *req)
{
    BPTR pending = s->dos.file.fh;

    if (!pending)
        return 0;
    switch (req->op) {
    case AAR_OP_FILE_READ:
    case AAR_OP_FILE_WRITE:
    case AAR_OP_FILE_SEEK:
    case AAR_OP_FILE_CLOSE:
        return file_transfer_handle(s, req->arg[AAR_A_FILE_FD]) == pending;
    case AAR_OP_FILE_TRUNCATE:
        return file_transfer_handle(s, req->arg[AAR_A_FILE_TRUNC_FD]) == pending;
    default:
        return 0;
    }
}

/* Check before touching a shared buffer or starting another native chunk.
 * Unlike an ordinary read, a parked read may outlive the running app. */
static int file_transfer_allowed(struct aar_serve *s)
{
    uint32_t state, end;

    return !s->closing && !s->stop_asked && !be_user_break(s) &&
           claim_state(s, &state, &end) == AAR_OK &&
           state == AAR_STATE_RUNNING && end == AAR_END_NONE;
}

static void file_transfer_send(struct aar_serve_file_transfer *t)
{
    struct StandardPacket *packet = &t->storage->packet;
    struct FileHandle *handle = (struct FileHandle *)BADDR(t->fh);
    uint32_t left = t->req.arg[AAR_A_FILE_LEN] - t->count;
    int writing = t->req.op == AAR_OP_FILE_WRITE;

    t->chunk = left > AAR_SERVE_IO_CHUNK ? AAR_SERVE_IO_CHUNK : left;
    if (writing)
        memcpy(t->storage->buffer, t->req.buf + t->count, t->chunk);
    memset(packet, 0, sizeof(*packet)); /* DOS may change dp_Port and the message links. */
    packet->sp_Msg.mn_Node.ln_Name = (char *)&packet->sp_Pkt;
    packet->sp_Pkt.dp_Link = &packet->sp_Msg;
    packet->sp_Pkt.dp_Port = t->port;
    packet->sp_Pkt.dp_Type = writing ? ACTION_WRITE : ACTION_READ;
    packet->sp_Pkt.dp_Arg1 = handle->fh_Arg1;
    packet->sp_Pkt.dp_Arg2 = (LONG)t->storage->buffer;
    packet->sp_Pkt.dp_Arg3 = (LONG)t->chunk;
    X_PUT_MSG(handle->fh_Type, &packet->sp_Msg);
}

static int file_transfer_begin(struct aar_serve *s, const struct aar_serve_req *req,
                               struct aar_serve_done *done)
{
    struct aar_serve_file_transfer *t = &s->dos.file;
    BPTR fh = file_transfer_handle(s, req->arg[AAR_A_FILE_FD]);
    uint32_t len = req->arg[AAR_A_FILE_LEN];

    if (t->fh)
        done->result = AAR_E_BUSY;
    else if (!fh)
        done->result = AAR_E_BADF;
    else if (len > req->buf_len || (len && !req->buf))
        done->result = AAR_E_INVAL;
    else if (!len)
        done->result = AAR_OK;
    else if (!((struct FileHandle *)BADDR(fh))->fh_Type)
        done->result = AAR_E_IO;
    else {
        if (!t->port)
            t->port = X_CREATE_PORT();
        if (t->port && !t->storage)
            t->storage = X_ALLOC_MEM(sizeof(*t->storage), MEMF_PUBLIC);
        if (!t->storage)
            done->result = AAR_E_NOMEM;
        else if (!file_transfer_allowed(s))
            done->result = AAR_E_IO;
        else {
            t->req = *req;
            t->fh = fh;
            t->count = 0;
            file_transfer_send(t);
            return AAR_SERVE_REQ_DEFERRED;
        }
    }
    return AAR_SERVE_REQ_DONE;
}

static void service_file_transfer(struct aar_serve *s)
{
    struct aar_serve_file_transfer *t = &s->dos.file;
    struct aar_serve_done done = {0};
    LONG n;
    int allowed, writing;

    if (!t->fh || X_GET_MSG(t->port) != &t->storage->packet.sp_Msg)
        return;
    allowed = file_transfer_allowed(s);
    writing = t->req.op == AAR_OP_FILE_WRITE;
    n = t->storage->packet.sp_Pkt.dp_Res1;
    if (n > 0 && (uint32_t)n <= t->chunk && (writing || allowed)) {
        if (!writing)
            memcpy(t->req.buf + t->count, t->storage->buffer, (uint32_t)n);
        t->count += (uint32_t)n;
    }
    if (!allowed || (n > 0 && (uint32_t)n > t->chunk))
        done.result = AAR_E_IO;
    else if (n < 0) {
        done.result = aar_serve_errno_from_io(t->storage->packet.sp_Pkt.dp_Res2);
        if (done.result == AAR_OK)
            done.result = AAR_E_NOENT; /* As in the synchronous file handlers. */
    } else if (writing && n == 0)
        done.result = AAR_E_NOSPC;
    else if ((uint32_t)n == t->chunk &&
             t->count < t->req.arg[AAR_A_FILE_LEN]) {
        file_transfer_send(t);
        return;
    }
    done.out[AAR_R_FILE_COUNT] = t->count;
    t->fh = 0;
    t->req.buf = NULL; /* The CQ backlog needs the request identity, not its buffer. */
    if (claim_live(s))
        s->core.sink.complete(s->core.sink.ctx, &t->req, &done);
}

static void file_transfer_destroy(struct aar_serve *s)
{
    struct aar_serve_file_transfer *t = &s->dos.file;

    /* The loop never waits here until teardown. In particular, ACTION_END
     * must not reach this handle while DOS still owns its read/write packet. */
    while (t->fh) {
        X_WAIT_PORT(t->port);
        service_file_transfer(s);
    }
    if (t->storage)
        X_FREE_MEM(t->storage, sizeof(*t->storage));
    if (t->port)
        X_DELETE_PORT(t->port);
}

static int examine_lock_async(struct aar_serve *s, BPTR lock,
                              struct FileInfoBlock *fib, int *transport_ok)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(lock);
    LONG result;
    int abandoned;

    if (transport_ok)
        *transport_ok = 0;
    if (!file_lock || !file_lock->fl_Task || !fib || !transport_ok)
        return 0;
    if (!dos_packet_async(s, file_lock->fl_Task, ACTION_EXAMINE_OBJECT,
                          (LONG)lock, (LONG)MKBADDR(fib), 0, 0, 0,
                          1, &result, NULL, &abandoned) ||
        !aar_serve_packet_result_usable(1, abandoned))
        return 0;
    *transport_ok = 1;
    return aar_serve_dos_boolean_result(1, result);
}

static int examine_next_async(struct aar_serve *s, BPTR lock,
                              struct FileInfoBlock *fib, int *transport_ok)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(lock);
    LONG result;
    int abandoned;

    if (transport_ok)
        *transport_ok = 0;
    if (!file_lock || !file_lock->fl_Task || !fib || !transport_ok)
        return 0;
    if (!dos_packet_async(s, file_lock->fl_Task, ACTION_EXAMINE_NEXT,
                          (LONG)lock, (LONG)MKBADDR(fib), 0, 0, 0,
                          1, &result, NULL, &abandoned) ||
        !aar_serve_packet_result_usable(1, abandoned))
        return 0;
    *transport_ok = 1;
    return aar_serve_dos_boolean_result(1, result);
}

/* DOS packet names must remain in public, longword-aligned storage until the
 * handler replies. */
static uint8_t *bstr_storage_alloc(void)
{
    return X_ALLOC_VEC(258u, MEMF_PUBLIC);
}

static uint8_t *bstr_alloc(const char *name)
{
    uint8_t *buf;

    if (!name)
        return NULL;
    buf = bstr_storage_alloc();
    if (!buf)
        return NULL;
    if (aar_serve_bstr_encode(name, buf, 258u) == 0u) {
        X_FREE_VEC(buf);
        X_SET_IO_ERR(ERROR_OBJECT_TOO_LARGE);
        return NULL;
    }
    return buf;
}

struct lock_release {
    struct aar_serve *s;
    struct MsgPort *handler;
    BPTR lock;
};

static int send_free_lock(void *context, int32_t *result)
{
    struct lock_release *release = context;
    struct aar_serve *s = release->s;
    LONG packet_result = 0;
    int delivered;

    if (!release->handler || !release->lock)
        return 0;
    delivered = dos_packet_async(s, release->handler, ACTION_FREE_LOCK,
                                 (LONG)release->lock, 0, 0, 0, 0, 0,
                                 &packet_result, NULL, NULL);
    if (result)
        *result = packet_result;
    return delivered;
}

static void fallback_unlock(void *context)
{
    struct lock_release *release = context;

    X_UNLOCK(release->lock);
}

static void release_abandoned_lock(struct aar_serve *s,
                                   struct MsgPort *handler, BPTR lock)
{
    struct lock_release release;
    struct aar_serve_dos_release_ops ops;

    release.s = s;
    release.handler = handler;
    release.lock = lock;
    ops.context = &release;
    ops.send_release = send_free_lock;
    ops.free_local = NULL;
    ops.fallback_release = fallback_unlock;
    ops.forget_slot = NULL;
    aar_serve_dos_release_abandoned(&ops, lock != 0);
}

static BPTR lock_on_handler_async(struct aar_serve *s, struct MsgPort *handler,
                                  BPTR dir, BPTR name_bstr, LONG access,
                                  int *completed_out)
{
    LONG result;
    int abandoned;

    if (completed_out)
        *completed_out = 0;
    if (!handler || !name_bstr || !completed_out)
        return 0;
    if (!dos_packet_async(s, handler, ACTION_LOCATE_OBJECT, (LONG)dir,
                          (LONG)name_bstr, access, 0, 0,
                          1, &result, NULL, &abandoned))
        return 0;
    if (abandoned) {
        release_abandoned_lock(s, handler, (BPTR)result);
        return 0;
    }
    if (!aar_serve_packet_result_usable(1, abandoned))
        return 0;
    *completed_out = 1;
    return (BPTR)result;
}

static BPTR lock_async(struct aar_serve *s, BPTR dir, const char *name,
                       LONG access, int *transport_ok)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(dir);
    uint8_t *storage;
    BPTR lock;
    int completed;

    if (transport_ok)
        *transport_ok = 0;
    if (!file_lock || !file_lock->fl_Task || !transport_ok)
        return 0;
    storage = bstr_alloc(name);
    if (!storage) {
        X_SET_IO_ERR(aar_serve_dos_error_code(0, 0, ERROR_OBJECT_TOO_LARGE));
        return 0;
    }
    lock = lock_on_handler_async(s, file_lock->fl_Task, dir, MKBADDR(storage),
                                 access, &completed);
    X_FREE_VEC(storage);
    if (completed)
        *transport_ok = 1;
    return lock;
}

static int resolve_dos_path(void *context, int advance,
                            struct aar_serve_dos_path_candidate *candidate)
{
    struct aar_serve_resolver *resolver = context;

    if (resolver->handler) {
        if (advance || resolver->fixed_used)
            return 0;
        resolver->fixed_used = 1;
        candidate->handler = (uintptr_t)resolver->handler;
        candidate->directory = (uint32_t)resolver->directory;
        return 1;
    }
    resolver->devproc = X_GET_DEVICE_PROC((STRPTR)resolver->name,
                                      advance ? resolver->devproc : NULL);
    if (!resolver->devproc)
        return 0;
    candidate->handler = (uintptr_t)resolver->devproc->dvp_Port;
    candidate->directory = (uint32_t)resolver->devproc->dvp_Lock;
    return 1;
}

static void release_dos_path_resolver(struct aar_serve_resolver *resolver)
{
    if (resolver->devproc)
        X_FREE_DEVICE_PROC(resolver->devproc);
}

struct lock_path_request {
    struct aar_serve *s;
    LONG access;
    BPTR lock;
    LONG error;
};

static void send_lock_path_packet(
    void *context, const struct aar_serve_dos_path_candidate *candidate,
    const uint8_t *name_bstr, uint32_t name_bstr_bytes,
    uint32_t name_bstr_bptr, struct aar_serve_dos_path_reply *reply)
{
    struct lock_path_request *request = context;
    struct aar_serve *s = request->s;
    int completed;

    (void)name_bstr;
    (void)name_bstr_bytes;
    request->lock = lock_on_handler_async(s, (struct MsgPort *)candidate->handler,
                                          (BPTR)candidate->directory,
                                          (BPTR)name_bstr_bptr,
                                          request->access, &completed);
    reply->completed = completed;
    reply->abandoned = !completed;
    reply->result = (int32_t)request->lock;
    request->error = X_IO_ERR();
    reply->error = request->error;
}

/* Locks name through resolver: every GetDeviceProc candidate, including
 * late-binding, nonbinding, and multi-directory assigns, or the one fixed
 * candidate of a pinned resolver. selected (optional) receives the candidate
 * that decided the answer. The caller releases the resolver. */
static BPTR lock_resolved_async(struct aar_serve *s,
                                struct aar_serve_resolver *resolver,
                                const char *name,
                                struct aar_serve_dos_path_candidate *selected,
                                int *transport_ok)
{
    struct lock_path_request request;
    uint8_t *name_bstr;
    int usable;

    if (transport_ok)
        *transport_ok = 0;
    if (!name || !transport_ok)
        return 0;
    name_bstr = bstr_storage_alloc();
    if (!name_bstr) {
        X_SET_IO_ERR(ERROR_NO_FREE_STORE);
        return 0;
    }
    memset(&request, 0, sizeof(request));
    request.s = s;
    request.access = ACCESS_READ;
    (void)aar_serve_dos_dispatch_path(name, name_bstr, 258u,
                                      ERROR_OBJECT_NOT_FOUND, resolve_dos_path,
                                      resolver, send_lock_path_packet, &request,
                                      NULL, &usable, selected);
    X_FREE_VEC(name_bstr);
    if (usable)
        *transport_ok = 1;
    if (!request.lock && usable)
        X_SET_IO_ERR(request.error);
    return request.lock;
}

/* Absolute (ABS tooltype) paths use every GetDeviceProc candidate. */
static BPTR lock_abs_async(struct aar_serve *s, const char *name,
                           int *transport_ok)
{
    struct aar_serve_resolver resolver;
    BPTR lock;
    LONG io;

    memset(&resolver, 0, sizeof(resolver));
    resolver.name = name;
    lock = lock_resolved_async(s, &resolver, name, NULL, transport_ok);
    io = X_IO_ERR();
    release_dos_path_resolver(&resolver);
    X_SET_IO_ERR(io);
    return lock;
}

static BPTR open_on_handler_async(struct aar_serve *s, struct MsgPort *handler,
                                  BPTR dir, BPTR name_bstr, LONG action,
                                  int *completed_out)
{
    struct FileHandle *handle;
    LONG result;
    int abandoned;

    if (completed_out)
        *completed_out = 0;
    if (!handler || !name_bstr || !completed_out)
        return 0;
    handle = X_ALLOC_DOS_OBJECT(DOS_FILEHANDLE, NULL);
    if (!handle) {
        X_SET_IO_ERR(ERROR_NO_FREE_STORE);
        return 0;
    }
    handle->fh_Type = handler;
    handle->fh_Arg1 = 0;
    if (!dos_packet_async(s, handler, action, MKBADDR(handle), (LONG)dir,
                          (LONG)name_bstr, 0, 0,
                          1, &result, NULL, &abandoned)) {
        X_FREE_DOS_OBJECT(DOS_FILEHANDLE, handle);
        return 0;
    }
    if (abandoned) {
        if (result != 0)
            end_or_close(s, MKBADDR(handle));
        else
            X_FREE_DOS_OBJECT(DOS_FILEHANDLE, handle);
        return 0;
    }
    if (!aar_serve_packet_result_usable(1, abandoned)) {
        X_FREE_DOS_OBJECT(DOS_FILEHANDLE, handle);
        return 0;
    }
    *completed_out = 1;
    if (result == 0) {
        X_FREE_DOS_OBJECT(DOS_FILEHANDLE, handle);
        return 0;
    }
    return MKBADDR(handle);
}

struct open_path_request {
    struct aar_serve *s;
    LONG action;
    BPTR fh;
    LONG error;
};

static void send_open_path_packet(
    void *context, const struct aar_serve_dos_path_candidate *candidate,
    const uint8_t *name_bstr, uint32_t name_bstr_bytes,
    uint32_t name_bstr_bptr, struct aar_serve_dos_path_reply *reply)
{
    struct open_path_request *request = context;
    struct aar_serve *s = request->s;
    int completed;

    (void)name_bstr;
    (void)name_bstr_bytes;
    request->fh = open_on_handler_async(s, (struct MsgPort *)candidate->handler,
                                        (BPTR)candidate->directory,
                                        (BPTR)name_bstr_bptr,
                                        request->action, &completed);
    reply->completed = completed;
    reply->abandoned = !completed;
    reply->result = (int32_t)request->fh;
    request->error = X_IO_ERR();
    reply->error = request->error;
}

/* A resolver for name: one fixed candidate on dir's handler when dir is a
 * lock, GetDeviceProc candidates when dir is 0. Returns 0 for a lock that
 * has no handler. */
static int dos_path_resolver_start(struct aar_serve_resolver *resolver,
                                   BPTR dir, const char *name)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(dir);

    memset(resolver, 0, sizeof(*resolver));
    if (dir) {
        if (!file_lock || !file_lock->fl_Task)
            return 0;
        resolver->handler = file_lock->fl_Task;
        resolver->directory = dir;
    } else {
        resolver->name = name;
    }
    return 1;
}

static BPTR open_resolved_async(struct aar_serve *s,
                                struct aar_serve_resolver *resolver,
                                const char *name, LONG mode, int *transport_ok)
{
    struct open_path_request request;
    uint8_t *name_bstr;
    int usable;

    if (transport_ok)
        *transport_ok = 0;
    if (!name || !transport_ok)
        return 0;
    name_bstr = bstr_storage_alloc();
    if (!name_bstr) {
        X_SET_IO_ERR(ERROR_NO_FREE_STORE);
        return 0;
    }
    memset(&request, 0, sizeof(request));
    request.s = s;
    if (mode == MODE_NEWFILE)
        request.action = ACTION_FINDOUTPUT;
    else if (mode == MODE_READWRITE)
        request.action = ACTION_FINDUPDATE;
    else
        request.action = ACTION_FINDINPUT;
    (void)aar_serve_dos_dispatch_path(name, name_bstr, 258u,
                                      ERROR_OBJECT_NOT_FOUND, resolve_dos_path,
                                      resolver, send_open_path_packet, &request,
                                      NULL, &usable, NULL);
    X_FREE_VEC(name_bstr);
    if (usable)
        *transport_ok = 1;
    if (!request.fh && usable)
        X_SET_IO_ERR(request.error);
    return request.fh;
}

/* Finds the ELF: a lock on the file, searched from dir (a lock) or through
 * every GetDeviceProc candidate of a volume or assign name. */
static BPTR lock_find_async(struct aar_serve *s, BPTR dir, const char *name,
                            int *transport_ok)
{
    struct aar_serve_resolver resolver;
    BPTR lock;
    LONG io;

    if (transport_ok)
        *transport_ok = 0;
    if (!name || !transport_ok)
        return 0;
    if (!dos_path_resolver_start(&resolver, dir, name))
        return 0;
    lock = lock_resolved_async(s, &resolver, name, NULL, transport_ok);
    io = X_IO_ERR();
    release_dos_path_resolver(&resolver);
    X_SET_IO_ERR(io);
    return lock;
}

struct file_handle_release {
    struct aar_serve *s;
    BPTR fh;
};

static int send_file_end(void *context, int32_t *result)
{
    struct file_handle_release *release = context;
    struct aar_serve *s = release->s;
    struct FileHandle *handle = (struct FileHandle *)BADDR(release->fh);
    LONG packet_result = 0;
    int abandoned;
    int delivered;

    if (!handle || !handle->fh_Type)
        return 0;
    delivered = dos_packet_async(s, handle->fh_Type, ACTION_END, handle->fh_Arg1,
                                 0, 0, 0, 0, 1, &packet_result,
                                 NULL, &abandoned);
    if (result)
        *result = aar_serve_packet_result_usable(delivered, abandoned) ?
                  packet_result : 0;
    return delivered;
}

static void free_file_handle(void *context)
{
    struct file_handle_release *release = context;

    X_FREE_DOS_OBJECT(DOS_FILEHANDLE, BADDR(release->fh));
}

static void close_file_handle(void *context)
{
    struct file_handle_release *release = context;

    X_CLOSE(release->fh);
}

static struct aar_serve_dos_release_ops
file_handle_release_ops(struct file_handle_release *release)
{
    struct aar_serve_dos_release_ops ops;

    ops.context = release;
    ops.send_release = send_file_end;
    ops.free_local = free_file_handle;
    ops.fallback_release = close_file_handle;
    ops.forget_slot = NULL;
    return ops;
}

static int end_async(struct aar_serve *s, BPTR fh, int *delivered_out)
{
    struct file_handle_release release;
    struct aar_serve_dos_release_ops ops;

    release.s = s;
    release.fh = fh;
    ops = file_handle_release_ops(&release);
    return aar_serve_dos_close_slot(&ops, delivered_out);
}

static void end_or_close(struct aar_serve *s, BPTR fh)
{
    struct file_handle_release release;
    struct aar_serve_dos_release_ops ops;

    release.s = s;
    release.fh = fh;
    ops = file_handle_release_ops(&release);
    aar_serve_dos_end_or_close(&ops);
}

/* ---- Names, dates and volumes ------------------------------------------------ */

static int be_utc_offset(void *ctx, int32_t *east_min)
{
    struct aar_serve_locale loc;
    int known = host_locale(ctx, &loc);

    *east_min = known ? -loc.gmt_west_min : 0;
    return known;
}

/* One packet that names an object: delete, create a directory, set the date,
 * the protection or the comment. It runs through the target's resolver, so a name with a
 * volume or assign goes to every GetDeviceProc candidate that does not have it. */
struct named_path_request {
    struct aar_serve *s;
    LONG action;
    LONG protect;
    struct DateStamp *date;
    uint8_t *comment_bstr; /* public, longword aligned, until the reply */
    LONG result;
    LONG error;
};

static void send_named_path_packet(
    void *context, const struct aar_serve_dos_path_candidate *candidate,
    const uint8_t *name_bstr, uint32_t name_bstr_bytes,
    uint32_t name_bstr_bptr, struct aar_serve_dos_path_reply *reply)
{
    struct named_path_request *request = context;
    struct MsgPort *handler = (struct MsgPort *)candidate->handler;
    LONG dir = (LONG)candidate->directory;
    LONG result = 0;
    int abandoned = 0;
    int delivered;

    (void)name_bstr;
    (void)name_bstr_bytes;
    if (request->action == ACTION_SET_DATE)
        delivered = dos_packet_async(request->s, handler, ACTION_SET_DATE, 0, dir,
                                     (LONG)name_bstr_bptr, (LONG)request->date, 0,
                                     1, &result, NULL, &abandoned);
    else if (request->action == ACTION_SET_PROTECT)
        delivered = dos_packet_async(request->s, handler, ACTION_SET_PROTECT, 0, dir,
                                     (LONG)name_bstr_bptr, request->protect, 0,
                                     1, &result, NULL, &abandoned);
    else if (request->action == ACTION_SET_COMMENT)
        delivered = dos_packet_async(request->s, handler, ACTION_SET_COMMENT, 0, dir,
                                     (LONG)name_bstr_bptr,
                                     (LONG)((uintptr_t)request->comment_bstr >> 2), 0,
                                     1, &result, NULL, &abandoned);
    else
        delivered = dos_packet_async(request->s, handler, request->action, dir,
                                     (LONG)name_bstr_bptr, 0, 0, 0,
                                     1, &result, NULL, &abandoned);
    request->error = X_IO_ERR();
    /* A directory made by a packet whose answer is thrown away is not kept
     * locked. */
    if (delivered && abandoned && request->action == ACTION_CREATE_DIR && result != 0)
        release_abandoned_lock(request->s, handler, (BPTR)result);
    reply->completed = delivered && !abandoned;
    reply->abandoned = !reply->completed;
    request->result = reply->completed ? result : 0;
    reply->result = (int32_t)request->result;
    reply->error = (int32_t)request->error;
}

static int be_target_named(void *ctx, enum aar_serve_named kind,
                           const struct aar_serve_named_args *args,
                           int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    struct named_path_request request;
    uint8_t *name_bstr;
    int usable = 0;
    int ok;

    *transport_ok = 0;
    *io = 0;
    memset(&request, 0, sizeof(request));
    request.s = s;
    switch (kind) {
    case AAR_SERVE_NAMED_DELETE: request.action = ACTION_DELETE_OBJECT; break;
    case AAR_SERVE_NAMED_CREATE_DIR: request.action = ACTION_CREATE_DIR; break;
    case AAR_SERVE_NAMED_SET_DATE: request.action = ACTION_SET_DATE; break;
    case AAR_SERVE_NAMED_SET_PROTECT:
        request.action = ACTION_SET_PROTECT;
        request.protect = (LONG)args->protect;
        break;
    case AAR_SERVE_NAMED_SET_COMMENT:
        request.action = ACTION_SET_COMMENT;
        break;
    default:
        *transport_ok = 1;
        *io = ERROR_ACTION_NOT_KNOWN;
        return 0;
    }
    if (!open_target_arm(&s->dos.target))
        return 0;
    if (kind == AAR_SERVE_NAMED_SET_COMMENT) {
        /* Round the 258-byte name BSTR buffer up to a BPTR boundary. Both
         * strings are public and stay alive until the packet is answered. */
        const uint32_t comment_off = 260u;

        name_bstr = X_ALLOC_VEC(comment_off + args->comment_len + 2u, MEMF_PUBLIC);
        if (name_bstr)
            request.comment_bstr = name_bstr + comment_off;
    } else {
        name_bstr = bstr_storage_alloc();
    }
    if (kind == AAR_SERVE_NAMED_SET_DATE)
        request.date = X_ALLOC_VEC(sizeof(*request.date), MEMF_PUBLIC | MEMF_CLEAR);
    if (!name_bstr || (kind == AAR_SERVE_NAMED_SET_DATE && !request.date)) {
        if (name_bstr)
            X_FREE_VEC(name_bstr);
        if (request.date)
            X_FREE_VEC(request.date);
        *transport_ok = 1;
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    if (request.comment_bstr) {
        request.comment_bstr[0] = (uint8_t)args->comment_len;
        if (args->comment_len)
            memcpy(request.comment_bstr + 1, args->comment, args->comment_len);
        request.comment_bstr[args->comment_len + 1u] = 0;
    }
    if (request.date) {
        request.date->ds_Days = (LONG)args->days;
        request.date->ds_Minute = (LONG)args->minutes;
        request.date->ds_Tick = (LONG)args->ticks;
    }
    (void)aar_serve_dos_dispatch_path(s->dos.target.name, name_bstr, 258u,
                                      ERROR_OBJECT_NOT_FOUND, resolve_dos_path,
                                      &s->dos.target.resolver, send_named_path_packet,
                                      &request, NULL, &usable, NULL);
    X_FREE_VEC(name_bstr);
    if (request.date)
        X_FREE_VEC(request.date);
    if (!usable)
        return 0;
    *transport_ok = 1;
    *io = (int32_t)request.error;
    ok = request.result != 0;
    if (ok && request.action == ACTION_CREATE_DIR)
        X_UNLOCK((BPTR)request.result);
    return ok;
}

/* One side of a rename: the handler, the directory lock the name is relative
 * to, and the name as a BSTR in public memory. A name with a volume or assign
 * starts at the first GetDeviceProc candidate: a rename inside the second
 * directory of a multi-directory assign is not found. */
struct rename_side {
    struct MsgPort *handler;
    BPTR lock;
    struct DevProc *devproc;
    uint8_t *bstr;
};

static int rename_side_start(struct rename_side *side, BPTR dir, const char *name)
{
    memset(side, 0, sizeof(*side));
    if (dir) {
        struct FileLock *file_lock = (struct FileLock *)BADDR(dir);

        if (!file_lock || !file_lock->fl_Task)
            return 0;
        side->handler = file_lock->fl_Task;
        side->lock = dir;
    } else {
        side->devproc = X_GET_DEVICE_PROC((STRPTR)name, NULL);
        if (!side->devproc)
            return 0;
        side->handler = side->devproc->dvp_Port;
        side->lock = side->devproc->dvp_Lock;
    }
    side->bstr = bstr_alloc(aar_serve_dos_packet_path(name));
    return side->bstr != NULL;
}

static void rename_side_end(struct rename_side *side)
{
    if (side->bstr)
        X_FREE_VEC(side->bstr);
    if (side->devproc)
        X_FREE_DEVICE_PROC(side->devproc);
}

static int be_rename(void *ctx, uintptr_t from_dir, const char *from_name,
                     uintptr_t to_dir, const char *to_name, int *transport_ok,
                     int32_t *io)
{
    struct aar_serve *s = ctx;
    struct rename_side from;
    struct rename_side to;
    LONG result = 0;
    int abandoned = 0;
    int delivered = 0;

    *transport_ok = 0;
    *io = 0;
    memset(&from, 0, sizeof(from));
    memset(&to, 0, sizeof(to));
    if (!rename_side_start(&from, (BPTR)from_dir, from_name) ||
        !rename_side_start(&to, (BPTR)to_dir, to_name)) {
        LONG err = X_IO_ERR();

        rename_side_end(&from);
        rename_side_end(&to);
        *transport_ok = 1;
        *io = (int32_t)(err ? err : ERROR_OBJECT_NOT_FOUND);
        return 0;
    }
    if (from.handler != to.handler) {
        rename_side_end(&from);
        rename_side_end(&to);
        *transport_ok = 1;
        *io = ERROR_RENAME_ACROSS_DEVICES;
        return 0;
    }
    delivered = dos_packet_async(s, from.handler, ACTION_RENAME_OBJECT,
                                 (LONG)from.lock, (LONG)MKBADDR(from.bstr),
                                 (LONG)to.lock, (LONG)MKBADDR(to.bstr), 0, 1,
                                 &result, NULL, &abandoned);
    *io = (int32_t)X_IO_ERR();
    rename_side_end(&from);
    rename_side_end(&to);
    if (!delivered || abandoned)
        return 0;
    *transport_ok = 1;
    return result != 0;
}

/* Reject different packet destinations early. A handler match does not
 * prove that the existing objects are on the same volume. */
static int be_same_volume(void *ctx, uintptr_t from_dir, const char *from_name,
                          uintptr_t to_dir, const char *to_name)
{
    struct rename_side from;
    struct rename_side to;
    int same;

    (void)ctx;
    memset(&from, 0, sizeof(from));
    memset(&to, 0, sizeof(to));
    if (!rename_side_start(&from, (BPTR)from_dir, from_name)) {
        rename_side_end(&from);
        return -1;
    }
    if (!rename_side_start(&to, (BPTR)to_dir, to_name)) {
        rename_side_end(&from);
        rename_side_end(&to);
        return -1;
    }
    same = from.handler == to.handler;
    rename_side_end(&from);
    rename_side_end(&to);
    return same;
}

/* The named delete walks a multi-directory assign, but the rename only uses
 * its first candidate. Refuse destructive replacement through such names. */
static int32_t unambiguous_replace_name(const char *name)
{
    struct DevProc *dp;
    int ambiguous;

    if (!name)
        return AAR_OK;
    dp = X_GET_DEVICE_PROC((STRPTR)name, NULL);
    if (!dp) {
        int32_t err = aar_serve_errno_from_io((int32_t)X_IO_ERR());

        return err == AAR_OK ? AAR_E_IO : err;
    }
    ambiguous = (dp->dvp_Flags & DVPF_ASSIGN) != 0;
    X_FREE_DEVICE_PROC(dp);
    return ambiguous ? AAR_E_INVAL : AAR_OK;
}

static int32_t be_check_replace(void *ctx, uintptr_t source, uintptr_t target,
                                const char *from_abs, const char *to_abs)
{
    int32_t rc;

    (void)ctx;
    if (X_SAME_LOCK((BPTR)source, (BPTR)target) != LOCK_SAME_VOLUME)
        return AAR_E_INVAL;
    rc = unambiguous_replace_name(from_abs);
    return rc == AAR_OK ? unambiguous_replace_name(to_abs) : rc;
}

static int be_volume_info(void *ctx, uintptr_t lock, struct aar_serve_volume *out,
                          int *transport_ok, int32_t *io)
{
    struct aar_serve *s = ctx;
    struct FileLock *file_lock = (struct FileLock *)BADDR((BPTR)lock);
    struct InfoData *info;
    LONG result = 0;
    int abandoned = 0;
    int delivered;

    *transport_ok = 0;
    *io = 0;
    if (!file_lock || !file_lock->fl_Task)
        return 0;
    info = X_ALLOC_VEC(sizeof(*info), MEMF_PUBLIC | MEMF_CLEAR);
    if (!info) {
        *transport_ok = 1;
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    delivered = dos_packet_async(s, file_lock->fl_Task, ACTION_INFO, (LONG)lock,
                                 (LONG)MKBADDR(info), 0, 0, 0, 1, &result, NULL,
                                 &abandoned);
    *io = (int32_t)X_IO_ERR();
    if (delivered && !abandoned) {
        *transport_ok = 1;
        if (result != 0) {
            out->blocks_total = (uint32_t)info->id_NumBlocks;
            out->blocks_used = (uint32_t)info->id_NumBlocksUsed;
            out->block_bytes = (uint32_t)info->id_BytesPerBlock;
            out->read_only = info->id_DiskState == ID_WRITE_PROTECTED;
        }
    }
    X_FREE_VEC(info);
    return *transport_ok && result != 0;
}
