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

    return aar_serve_user_break(ctrl_c_set(), s->user_stop);
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

static int32_t be_fh_set_size0(void *ctx, uintptr_t fh, int *transport_ok,
                               int32_t *io)
{
    struct aar_serve *s = ctx;
    LONG result;

    result = file_packet_async(s, (BPTR)fh, ACTION_SET_FILE_SIZE, 0,
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
    .fh_set_size0 = be_fh_set_size0,
    .fh_read = be_fh_read,
    .fh_write = be_fh_write,
    .fh_seek = be_fh_seek,
    .fh_end_or_close = be_fh_end_or_close,
    .fh_close = be_fh_close,
    .list_open = be_list_open,
    .list_next = be_list_next,
    .list_close = be_list_close,
};

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
    ctrl_c = ctrl_c_set();
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
    mask = (1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C;
    if (s->tm.port)
        mask |= 1UL << s->tm.port->mp_SigBit;
    for (;;) {
        signals = X_WAIT(mask);
        if ((signals & SIGBREAKF_CTRL_C) || ctrl_c_set()) {
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
    if (s->cfg.poll_us)
        timer_arm(s);
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
