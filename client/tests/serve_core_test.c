/*
 * Host tests of the aar_serve request handlers: a fake request stream goes into
 * aar_serve_core_request(), a fake DOS (an in-memory volume with locks, file
 * handles and lists), fake console, display and audio backends stand under it,
 * and the completions are checked.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar_serve_core.h"

/* ---- The fake volume ---------------------------------------------------- */

#define MAX_NODES 40
#define MAX_LOCKS 128
#define MAX_FH 40
#define MAX_LISTS 40
#define FILE_CAP 65536u

struct fnode {
    int used;
    int parent;
    char name[24];
    int is_dir;
    int link;           /* entry resolves to this node (a soft link), or -1 */
    int held;           /* another program holds it: locks fail IN_USE */
    uint8_t *data;
    uint32_t size;
    uint32_t mtime_days;
};

struct flock {
    int used;
    int node;
};

struct ffh {
    int used;
    int node;
    uint32_t pos;
};

struct flist {
    int used;
    int lock_node;
    int next_child; /* node index scan position */
};

struct fake {
    struct fnode nodes[MAX_NODES];
    struct flock locks[MAX_LOCKS];
    struct ffh fhs[MAX_FH];
    struct flist lists[MAX_LISTS];
    int live_locks;
    int live_fhs;
    int live_lists;

    int live;            /* the request is still wanted */
    int broken;          /* a Ctrl-C is pending */
    int stop;            /* a stop is under way (aar_serve_stop, the window, the platform) */
    int break_after_open; /* set `broken` once an open succeeded */
    int lose_after_open;  /* the request is lost the moment an open succeeded */
    int write_zero;       /* a write that stores nothing and reports no error */
    int set_size_lost;    /* SetFileSize loses its packet */
    int set_size_unsupported;
    int seek_model;       /* 0 FFS: refuses with ERROR_SEEK_ERROR; 1 refuses with Res2 0;
                           * 2 ram-handler: accepts a target before the start and moves
                           * the position negative, refuses past the end */
    int seek_refusals;
    int close_undelivered;
    int fail_transport_lock; /* lock_rel of this leaf name loses its packet */
    char fail_transport_name[24];

    /* a log of what the handlers asked of DOS */
    int opens[3];        /* per aar_serve_mode */
    int probes;
    int set_sizes;
    int end_or_close;
    int reads;
    int writes;
    int max_read;
    int lock_rel_calls;

    /* target state */
    uintptr_t t_dir;
    char t_name[256];
};

static struct fake fk;

static int node_new(int parent, const char *name, int is_dir)
{
    int i;

    for (i = 0; i < MAX_NODES; i++) {
        if (!fk.nodes[i].used) {
            struct fnode *n = &fk.nodes[i];

            memset(n, 0, sizeof(*n));
            n->used = 1;
            n->parent = parent;
            n->link = -1;
            n->is_dir = is_dir;
            n->mtime_days = 1000u + (uint32_t)i;
            strncpy(n->name, name, sizeof(n->name) - 1u);
            if (!is_dir)
                n->data = calloc(1, FILE_CAP);
            return i;
        }
    }
    assert(!"out of nodes");
    return -1;
}

static int file_new(int parent, const char *name, const char *text)
{
    int n = node_new(parent, name, 0);

    fk.nodes[n].size = (uint32_t)strlen(text);
    memcpy(fk.nodes[n].data, text, fk.nodes[n].size);
    return n;
}

static int child_of(int dir, const char *name)
{
    int i;

    for (i = 0; i < MAX_NODES; i++) {
        struct fnode *n = &fk.nodes[i];

        if (n->used && n->parent == dir && strcmp(n->name, name) == 0)
            return n->link >= 0 ? n->link : i;
    }
    return -1;
}

static uintptr_t lock_new(int node)
{
    int i;

    for (i = 0; i < MAX_LOCKS; i++) {
        if (!fk.locks[i].used) {
            fk.locks[i].used = 1;
            fk.locks[i].node = node;
            fk.live_locks++;
            return (uintptr_t)(i + 1);
        }
    }
    assert(!"out of locks");
    return 0;
}

static int lock_node(uintptr_t lock)
{
    assert(lock >= 1 && lock <= MAX_LOCKS && fk.locks[lock - 1].used);
    return fk.locks[lock - 1].node;
}

static void data_free_all(void)
{
    int i;

    for (i = 0; i < MAX_NODES; i++)
        free(fk.nodes[i].data);
}

/* ---- aar_serve_dos_ops over the fake volume ------------------------------------- */

static int f_live(void *ctx)
{
    (void)ctx;
    return fk.live;
}

static int f_user_break(void *ctx)
{
    (void)ctx;
    return fk.broken;
}

static int f_stopping(void *ctx)
{
    (void)ctx;
    return fk.stop || fk.broken;
}

static uintptr_t f_dup_lock(void *ctx, uintptr_t lock)
{
    (void)ctx;
    return lock_new(lock_node(lock));
}

static void f_unlock(void *ctx, uintptr_t lock)
{
    (void)ctx;
    (void)lock_node(lock);
    fk.locks[lock - 1].used = 0;
    fk.live_locks--;
}

static uintptr_t f_parent_dir(void *ctx, uintptr_t lock)
{
    int parent = fk.nodes[lock_node(lock)].parent;

    (void)ctx;
    return parent < 0 ? 0 : lock_new(parent);
}

static int f_same_lock(void *ctx, uintptr_t a, uintptr_t b)
{
    (void)ctx;
    return lock_node(a) == lock_node(b);
}

static uintptr_t f_lock_rel(void *ctx, uintptr_t dir, const char *name,
                            int *transport_ok, int32_t *io)
{
    int n;

    (void)ctx;
    fk.lock_rel_calls++;
    *io = 0;
    if (fk.fail_transport_lock && strcmp(name, fk.fail_transport_name) == 0) {
        *transport_ok = 0;
        return 0;
    }
    *transport_ok = 1;
    n = child_of(lock_node(dir), name);
    if (n < 0) {
        *io = AAR_SERVE_IOERR_OBJECT_NOT_FOUND;
        return 0;
    }
    if (fk.nodes[n].held) {
        *io = AAR_SERVE_IOERR_OBJECT_IN_USE;
        return 0;
    }
    return lock_new(n);
}

/* "Vol:a/b": walks from node 0. */
static int abs_find(const char *name)
{
    const char *colon = strchr(name, ':');
    char comp[24];
    int cur = 0;
    const char *p;

    if (!colon || strncmp(name, fk.nodes[0].name, (size_t)(colon - name)) != 0)
        return -1;
    p = colon + 1;
    while (*p) {
        size_t n = 0;

        while (*p && *p != '/')
            comp[n++] = *p++;
        comp[n] = 0;
        if (*p == '/')
            p++;
        cur = child_of(cur, comp);
        if (cur < 0)
            return -1;
    }
    return cur;
}

static uintptr_t f_lock_abs(void *ctx, const char *name, int *transport_ok,
                            int32_t *io)
{
    int n = abs_find(name);

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (n < 0) {
        *io = AAR_SERVE_IOERR_OBJECT_NOT_FOUND;
        return 0;
    }
    return lock_new(n);
}

static void info_from(int n, struct aar_serve_info *info)
{
    struct fnode *node = &fk.nodes[n];

    memset(info, 0, sizeof(*info));
    info->is_dir = node->is_dir;
    info->size = node->size;
    info->mtime = aar_serve_amiga_unix(node->mtime_days, 0u, 0u);
    strcpy(info->name, node->name);
}

static int f_lock_examine(void *ctx, uintptr_t lock, struct aar_serve_info *info,
                          int *transport_ok, int32_t *io)
{
    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    info_from(lock_node(lock), info);
    return 1;
}

static int target_node(int *held)
{
    int n;

    if (fk.t_dir)
        n = child_of(lock_node(fk.t_dir), fk.t_name);
    else
        n = abs_find(fk.t_name);
    *held = n >= 0 && fk.nodes[n].held;
    return n;
}

static void f_target_begin(void *ctx, uintptr_t dir, const char *name)
{
    (void)ctx;
    fk.t_dir = dir;
    strncpy(fk.t_name, name, sizeof(fk.t_name) - 1u);
}

static int f_target_probe(void *ctx, int *exists, int *transport_ok,
                          int32_t *io)
{
    int held;

    (void)ctx;
    fk.probes++;
    *transport_ok = 1;
    *io = 0;
    *exists = target_node(&held) >= 0;
    return 1;
}

static int fh_new(int node)
{
    int i;

    for (i = 0; i < MAX_FH; i++) {
        if (!fk.fhs[i].used) {
            fk.fhs[i].used = 1;
            fk.fhs[i].node = node;
            fk.fhs[i].pos = 0;
            fk.live_fhs++;
            return i + 1;
        }
    }
    assert(!"out of handles");
    return 0;
}

static uintptr_t f_target_open(void *ctx, enum aar_serve_mode mode,
                               int *transport_ok, int32_t *io)
{
    int held;
    int n;

    (void)ctx;
    fk.opens[mode]++;
    *transport_ok = 1;
    *io = 0;
    n = target_node(&held);
    if (n >= 0 && held) {
        *io = AAR_SERVE_IOERR_OBJECT_IN_USE;
        return 0;
    }
    if (mode == AAR_SERVE_MODE_OLD && n < 0) {
        *io = AAR_SERVE_IOERR_OBJECT_NOT_FOUND;
        return 0;
    }
    if (n < 0) {
        /* MODE_READWRITE and MODE_NEWFILE create a missing file. */
        int parent = fk.t_dir ? lock_node(fk.t_dir) : 0;

        if (mode == AAR_SERVE_MODE_READWRITE) {
            *io = AAR_SERVE_IOERR_OBJECT_NOT_FOUND;
            return 0;
        }
        n = node_new(parent, fk.t_name, 0);
    } else if (mode == AAR_SERVE_MODE_NEW) {
        fk.nodes[n].size = 0;
    }
    if (fk.lose_after_open)
        fk.live = 0;
    if (fk.break_after_open)
        fk.broken = 1;
    return (uintptr_t)fh_new(n);
}

static void f_target_end(void *ctx)
{
    (void)ctx;
    fk.t_dir = 0;
}

static struct ffh *fh_get(uintptr_t fh)
{
    assert(fh >= 1 && fh <= MAX_FH && fk.fhs[fh - 1].used);
    return &fk.fhs[fh - 1];
}

static int32_t f_fh_set_size0(void *ctx, uintptr_t fh, int *transport_ok,
                              int32_t *io)
{
    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    fk.set_sizes++;
    if (fk.set_size_lost) {
        *transport_ok = 0;
        return 0;
    }
    if (fk.set_size_unsupported) {
        *io = AAR_SERVE_IOERR_ACTION_NOT_KNOWN;
        return -1;
    }
    fk.nodes[fh_get(fh)->node].size = 0;
    return 0;
}

static int32_t f_fh_read(void *ctx, uintptr_t fh, uint8_t *dst, uint32_t bytes,
                         int *transport_ok, int32_t *io)
{
    struct ffh *h = fh_get(fh);
    struct fnode *n = &fk.nodes[h->node];
    uint32_t left = h->pos < n->size ? n->size - h->pos : 0u;

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    fk.reads++;
    if (bytes > (uint32_t)fk.max_read)
        fk.max_read = (int)bytes;
    if (left > bytes)
        left = bytes;
    memcpy(dst, n->data + h->pos, left);
    h->pos += left;
    return (int32_t)left;
}

static int32_t f_fh_write(void *ctx, uintptr_t fh, const uint8_t *src,
                          uint32_t bytes, int *transport_ok, int32_t *io)
{
    struct ffh *h = fh_get(fh);
    struct fnode *n = &fk.nodes[h->node];

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (fk.write_zero)
        return 0;
    fk.writes++;
    if (h->pos + bytes > FILE_CAP) {
        *io = AAR_SERVE_IOERR_DISK_FULL;
        return -1;
    }
    memcpy(n->data + h->pos, src, bytes);
    h->pos += bytes;
    if (h->pos > n->size)
        n->size = h->pos;
    return (int32_t)bytes;
}

static int32_t f_fh_seek(void *ctx, uintptr_t fh, int32_t offset, int whence,
                         int *transport_ok, int32_t *io)
{
    struct ffh *h = fh_get(fh);
    struct fnode *n = &fk.nodes[h->node];
    int64_t base = whence == 0 ? 0 : (whence == 1 ? (int64_t)(int32_t)h->pos : (int64_t)n->size);
    int64_t target = base + offset;
    int32_t old = (int32_t)h->pos;

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (fk.seek_model == 2 && target < 0) {
        h->pos = (uint32_t)(int32_t)target;
        return old;
    }
    if (target < 0 || target > (int64_t)n->size) {
        fk.seek_refusals++;
        *io = fk.seek_model == 1 ? 0 : AAR_SERVE_IOERR_SEEK_ERROR;
        return -1;
    }
    h->pos = (uint32_t)target;
    return old;
}

static void f_fh_end_or_close(void *ctx, uintptr_t fh)
{
    (void)ctx;
    (void)fh_get(fh);
    fk.fhs[fh - 1].used = 0;
    fk.live_fhs--;
    fk.end_or_close++;
}

static int f_fh_close(void *ctx, uintptr_t fh, int *delivered, int32_t *io)
{
    (void)ctx;
    *io = 0;
    if (fk.close_undelivered) {
        *delivered = 0;
        return 0;
    }
    *delivered = 1;
    fk.fhs[fh_get(fh) - fk.fhs].used = 0;
    fk.live_fhs--;
    return 1;
}

static int f_list_open(void *ctx, uintptr_t lock, uintptr_t *list,
                       int *transport_ok, int32_t *io)
{
    int i;
    int n = lock_node(lock);

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (!fk.nodes[n].is_dir) {
        *io = AAR_SERVE_IOERR_OBJECT_WRONG_TYPE;
        return 0;
    }
    for (i = 0; i < MAX_LISTS; i++) {
        if (!fk.lists[i].used) {
            fk.lists[i].used = 1;
            fk.lists[i].lock_node = n;
            fk.lists[i].next_child = 0;
            fk.live_lists++;
            /* the list owns the lock */
            f_unlock(NULL, lock);
            *list = (uintptr_t)(i + 1);
            return 1;
        }
    }
    assert(!"out of lists");
    return 0;
}

static int f_list_next(void *ctx, uintptr_t list, struct aar_serve_info *info,
                       int *transport_ok, int32_t *io)
{
    struct flist *l;

    (void)ctx;
    assert(list >= 1 && list <= MAX_LISTS && fk.lists[list - 1].used);
    l = &fk.lists[list - 1];
    *transport_ok = 1;
    *io = 0;
    while (l->next_child < MAX_NODES) {
        int i = l->next_child++;

        if (fk.nodes[i].used && fk.nodes[i].parent == l->lock_node) {
            info_from(i, info);
            {
                /* A real ACTION_EXAMINE_NEXT result: counted name, with no
                 * NUL and unrelated bytes after its counted end. */
                uint8_t fib_name[108];
                size_t n = strlen(fk.nodes[i].name);

                assert(n < sizeof(fib_name));
                memset(fib_name, 0xa5, sizeof(fib_name));
                fib_name[0] = (uint8_t)n;
                memcpy(fib_name + 1, fk.nodes[i].name, n);
                aar_serve_bstr_decode(fib_name, sizeof(fib_name),
                                      info->name, sizeof(info->name));
            }
            return 1;
        }
    }
    *io = AAR_SERVE_IOERR_NO_MORE_ENTRIES;
    return 0;
}

static void f_list_close(void *ctx, uintptr_t list)
{
    (void)ctx;
    assert(list >= 1 && list <= MAX_LISTS && fk.lists[list - 1].used);
    fk.lists[list - 1].used = 0;
    fk.live_lists--;
}

static const struct aar_serve_dos_ops fake_dos = {
    NULL, f_live, f_user_break, f_stopping, f_dup_lock, f_unlock, f_parent_dir,
    f_same_lock, f_lock_rel, f_lock_abs, f_lock_examine, f_target_begin,
    f_target_probe, f_target_open, f_target_end, f_fh_set_size0, f_fh_read,
    f_fh_write, f_fh_seek, f_fh_end_or_close, f_fh_close, f_list_open,
    f_list_next, f_list_close
};

/* ---- Completions the sink collects -------------------------------------- */

#define MAX_SINK 16

struct sunk {
    struct aar_serve_req req;
    struct aar_serve_done done;
};

static struct sunk sunk[MAX_SINK];
static int sunk_n;

static void sink_complete(void *ctx, const struct aar_serve_req *req,
                          const struct aar_serve_done *done)
{
    (void)ctx;
    assert(sunk_n < MAX_SINK);
    sunk[sunk_n].req = *req;
    sunk[sunk_n].done = *done;
    sunk_n++;
}

static const struct aar_serve_sink sink = { NULL, sink_complete };

/* ---- Fixture: ROOT is Vol:root, with a sibling outside it --------------- */

enum { N_VOL = 0, N_ROOT, N_DIR, N_HELLO, N_OUTSIDE, N_SECRET };

static struct aar_serve_core serve;
static uint32_t next_id;

static void fixture(int abs_paths)
{
    int escape;

    if (fk.nodes[0].used)
        data_free_all();
    memset(&fk, 0, sizeof(fk));
    fk.live = 1;
    node_new(-1, "Vol", 1);                       /* 0 */
    node_new(0, "root", 1);                       /* 1 */
    node_new(1, "dir", 1);                        /* 2 */
    file_new(2, "hello.txt", "hello world");      /* 3 */
    node_new(0, "outside", 1);                    /* 4 */
    file_new(4, "secret.txt", "top secret");      /* 5 */
    file_new(1, "top.txt", "top");                /* 6 */
    escape = node_new(1, "escape", 0);            /* 7: a link out of ROOT */
    fk.nodes[escape].link = N_SECRET;
    aar_serve_core_init(&serve, &sink);
    aar_serve_files_init(&serve.files, &fake_dos, 0, abs_paths);
    serve.files.root = lock_new(N_ROOT);
    sunk_n = 0;
    next_id = 100u;
}

static void fixture_free(void)
{
    aar_serve_files_close_all(&serve.files);
    f_unlock(NULL, serve.files.root);
    assert(fk.live_locks == 0);
    assert(fk.live_fhs == 0);
    assert(fk.live_lists == 0);
}

/* ---- Request helpers ---------------------------------------------------- */

static struct aar_serve_req mkreq(uint16_t op)
{
    struct aar_serve_req r;

    memset(&r, 0, sizeof(r));
    r.op = op;
    r.id = next_id++;
    return r;
}

static int32_t serve_one(const struct aar_serve_req *req, struct aar_serve_done *done)
{
    int rc = aar_serve_core_request(&serve, req, done);

    assert(rc == AAR_SERVE_REQ_DONE);
    return done->result;
}

static int32_t do_open(uint32_t fd, uint32_t flags, const char *path)
{
    struct aar_serve_req r = mkreq(AAR_OP_FILE_OPEN);
    struct aar_serve_done d;
    uint8_t buf[AAR_SERVE_PATH_BYTES + 8u];

    memset(buf, 0, sizeof(buf));
    strcpy((char *)buf, path);
    r.arg[AAR_A_FILE_OPEN_FD] = fd;
    r.arg[AAR_A_FILE_OPEN_FLAGS] = flags;
    r.buf = buf;
    r.buf_len = (uint32_t)strlen(path) + 1u;
    return serve_one(&r, &d);
}

static int32_t do_close(uint32_t fd)
{
    struct aar_serve_req r = mkreq(AAR_OP_FILE_CLOSE);
    struct aar_serve_done d;

    r.arg[AAR_A_FILE_FD] = fd;
    return serve_one(&r, &d);
}

static int32_t do_read(uint32_t fd, uint32_t len, uint8_t *buf,
                       uint32_t buf_len, uint32_t *count)
{
    struct aar_serve_req r = mkreq(AAR_OP_FILE_READ);
    struct aar_serve_done d;
    int32_t rc;

    r.arg[AAR_A_FILE_FD] = fd;
    r.arg[AAR_A_FILE_LEN] = len;
    r.buf = buf;
    r.buf_len = buf_len;
    rc = serve_one(&r, &d);
    if (count)
        *count = d.out[AAR_R_FILE_COUNT];
    return rc;
}

static int32_t do_write(uint32_t fd, const uint8_t *data, uint32_t len,
                        uint32_t *count)
{
    struct aar_serve_req r = mkreq(AAR_OP_FILE_WRITE);
    struct aar_serve_done d;
    int32_t rc;

    r.arg[AAR_A_FILE_FD] = fd;
    r.arg[AAR_A_FILE_LEN] = len;
    r.buf = (uint8_t *)(uintptr_t)data;
    r.buf_len = len;
    rc = serve_one(&r, &d);
    if (count)
        *count = d.out[AAR_R_FILE_COUNT];
    return rc;
}

static int32_t do_seek(uint32_t fd, int64_t offset, uint32_t whence,
                       uint32_t *pos)
{
    struct aar_serve_req r = mkreq(AAR_OP_FILE_SEEK);
    struct aar_serve_done d;
    int32_t rc;

    r.arg[AAR_A_FILE_FD] = fd;
    r.arg[AAR_A_FILE_OFF_HI] = (uint32_t)((uint64_t)offset >> 32);
    r.arg[AAR_A_FILE_OFF_LO] = (uint32_t)offset;
    r.arg[AAR_A_FILE_WHENCE] = whence;
    rc = serve_one(&r, &d);
    if (pos)
        *pos = d.out[AAR_R_FILE_POS_LO];
    return rc;
}

static int32_t do_stat(const char *path, struct aar_serve_done *d)
{
    struct aar_serve_req r = mkreq(AAR_OP_FILE_STAT);
    uint8_t buf[AAR_SERVE_PATH_BYTES + 8u];

    memset(buf, 0, sizeof(buf));
    strcpy((char *)buf, path);
    r.buf = buf;
    r.buf_len = (uint32_t)strlen(path) + 1u;
    return serve_one(&r, d);
}

static int32_t do_list_open(uint32_t id, const char *path)
{
    struct aar_serve_req r = mkreq(AAR_OP_LIST_OPEN);
    struct aar_serve_done d;
    uint8_t buf[AAR_SERVE_PATH_BYTES + 8u];

    memset(buf, 0, sizeof(buf));
    strcpy((char *)buf, path);
    r.arg[AAR_A_LIST_ID] = id;
    r.buf = buf;
    r.buf_len = (uint32_t)strlen(path) + 1u;
    return serve_one(&r, &d);
}

static int32_t do_list_next(uint32_t id, char *name, uint32_t name_len,
                            uint32_t *mode)
{
    struct aar_serve_req r = mkreq(AAR_OP_LIST_NEXT);
    struct aar_serve_done d;
    uint8_t buf[64];
    int32_t rc;

    memset(buf, 0xee, sizeof(buf));
    r.arg[AAR_A_LIST_ID] = id;
    r.buf = buf;
    r.buf_len = name_len;
    rc = serve_one(&r, &d);
    if (rc == AAR_OK) {
        strcpy(name, (const char *)buf);
        *mode = d.out[AAR_R_LIST_MODE];
    }
    return rc;
}

static int32_t do_list_close(uint32_t id)
{
    struct aar_serve_req r = mkreq(AAR_OP_LIST_CLOSE);
    struct aar_serve_done d;

    r.arg[AAR_A_LIST_ID] = id;
    return serve_one(&r, &d);
}

#define R AAR_FILE_READ
#define W AAR_FILE_WRITE
#define C AAR_FILE_CREATE
#define T AAR_FILE_TRUNCATE
#define X AAR_FILE_EXCL

/* ---- File tests --------------------------------------------------------- */

static void test_read_in_chunks_and_past_the_end(void)
{
    uint8_t out[32];
    uint32_t count = 0;
    int node;
    uint32_t i;
    uint8_t *big;

    fixture(0);
    node = N_HELLO;
    for (i = 0; i < 20000u; i++)
        fk.nodes[node].data[i] = (uint8_t)(i * 7u);
    fk.nodes[node].size = 20000u;
    assert(do_open(3, R, "dir/hello.txt") == AAR_OK);

    big = malloc(40000u);
    assert(do_read(3, 20000u, big, 40000u, &count) == AAR_OK);
    assert(count == 20000u);
    for (i = 0; i < 20000u; i++)
        assert(big[i] == (uint8_t)(i * 7u));
    /* one DOS read per 8 KiB chunk, none larger */
    assert(fk.reads == 3 && fk.max_read == (int)AAR_SERVE_IO_CHUNK);
    /* at the end: a short read, then nothing */
    assert(do_read(3, 100u, out, sizeof(out), &count) == AAR_E_INVAL);
    assert(do_read(3, 16u, out, sizeof(out), &count) == AAR_OK && count == 0u);
    /* a length past the payload is refused */
    assert(do_seek(3, 0, AAR_SEEK_SET, NULL) == AAR_OK);
    assert(do_read(3, 4097u, out, 4096u, &count) == AAR_E_INVAL);
    assert(do_read(3, 11u, out, sizeof(out), &count) == AAR_OK && count == 11u);
    free(big);
    /* a bad descriptor */
    assert(do_read(4, 1u, out, sizeof(out), &count) == AAR_E_BADF);
    assert(do_read(2, 1u, out, sizeof(out), &count) == AAR_E_BADF);
    assert(do_read(3u + AAR_SERVE_FD_COUNT, 1u, out, sizeof(out), &count) ==
           AAR_E_BADF);
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_read_stops_at_a_break_with_the_bytes_so_far(void)
{
    uint8_t big[20000];
    uint32_t count = 77u;

    fixture(0);
    fk.nodes[N_HELLO].size = 20000u;
    assert(do_open(3, R, "dir/hello.txt") == AAR_OK);
    fk.broken = 1;
    assert(do_read(3, 20000u, big, sizeof(big), &count) == AAR_OK);
    assert(count == 0u && fk.reads == 0);
    fk.broken = 0;
    fk.live = 0;
    assert(do_read(3, 100u, big, sizeof(big), &count) == AAR_E_IO);
    fk.live = 1;
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_write_seek_and_stat(void)
{
    static const uint8_t text[] = "0123456789";
    uint8_t back[16];
    uint32_t count = 0;
    uint32_t pos = 0;
    struct aar_serve_done d;

    fixture(0);
    assert(do_open(5, W | C | T, "new.bin") == AAR_OK);
    assert(do_write(5, text, 10u, &count) == AAR_OK && count == 10u);
    assert(fk.nodes[child_of(N_ROOT, "new.bin")].size == 10u);
    assert(do_seek(5, 4, AAR_SEEK_SET, &pos) == AAR_OK && pos == 4u);
    assert(do_seek(5, 2, AAR_SEEK_CUR, &pos) == AAR_OK && pos == 6u);
    assert(do_seek(5, -3, AAR_SEEK_END, &pos) == AAR_OK && pos == 7u);
    /* past either end is the handler's refusal */
    assert(do_seek(5, 100, AAR_SEEK_SET, &pos) == AAR_E_INVAL);
    assert(do_seek(5, -100, AAR_SEEK_CUR, &pos) == AAR_E_INVAL);
    /* a SET offset above 2 GiB, a bad whence and a huge END offset */
    assert(do_seek(5, 0x80000000LL, AAR_SEEK_SET, &pos) == AAR_E_INVAL);
    assert(do_seek(5, -1, AAR_SEEK_SET, &pos) == AAR_E_INVAL);
    assert(do_seek(5, 0, 3u, &pos) == AAR_E_INVAL);
    assert(do_seek(5, 0x100000000LL, AAR_SEEK_END, &pos) == AAR_E_INVAL);
    assert(do_seek(6, 0, AAR_SEEK_SET, &pos) == AAR_E_BADF);
    assert(do_seek(5, 0, AAR_SEEK_SET, &pos) == AAR_OK && pos == 0u);
    assert(do_read(5, 10u, back, sizeof(back), &count) == AAR_OK && count == 10u);
    assert(memcmp(back, text, 10u) == 0);
    assert(do_close(5) == AAR_OK);
    assert(do_close(5) == AAR_E_BADF);

    assert(do_stat("new.bin", &d) == AAR_OK);
    assert(d.out[AAR_R_STAT_SIZE_LO] == 10u && d.out[AAR_R_STAT_SIZE_HI] == 0u);
    assert(d.out[AAR_R_STAT_MODE] == AAR_MODE_FILE);
    assert(d.out[AAR_R_STAT_MTIME_LO] ==
           aar_serve_amiga_unix(fk.nodes[child_of(N_ROOT, "new.bin")].mtime_days,
                                0u, 0u));
    assert(do_stat("dir", &d) == AAR_OK);
    assert(d.out[AAR_R_STAT_MODE] == AAR_MODE_DIR);
    assert(do_stat("dir/nothing", &d) == AAR_E_NOENT);
    assert(do_stat("", &d) == AAR_E_INVAL);
    fixture_free();
}

/* A seek before the start is AAR_E_INVAL whatever Res2 the handler leaves, and
 * the position stays put (C-FILE.19 on ram-handler). */
static void test_seek_refusal_is_inval_whatever_res2(void)
{
    static const uint8_t text[] = "0123456789";
    uint8_t one = 0xffu;
    uint32_t count = 0;
    uint32_t pos = 99u;
    int model;

    for (model = 0; model <= 2; model++) {
        fixture(0);
        fk.seek_model = model;
        assert(do_open(5, W | C | T, "seek.bin") == AAR_OK);
        assert(do_write(5, text, 10u, &count) == AAR_OK && count == 10u);
        assert(do_seek(5, 0, AAR_SEEK_SET, &pos) == AAR_OK && pos == 0u);
        /* CUR before the start from position 0 */
        assert(do_seek(5, -1, AAR_SEEK_CUR, &pos) == AAR_E_INVAL);
        assert(do_read(5, 1u, &one, 1u, &count) == AAR_OK && count == 1u &&
               one == '0');
        /* CUR before the start from the middle: the position stays at 1 */
        assert(do_seek(5, -5, AAR_SEEK_CUR, &pos) == AAR_E_INVAL);
        assert(do_read(5, 1u, &one, 1u, &count) == AAR_OK && count == 1u &&
               one == '1');
        /* END with a negative offset larger than the size */
        assert(do_seek(5, -11, AAR_SEEK_END, &pos) == AAR_E_INVAL);
        assert(do_read(5, 1u, &one, 1u, &count) == AAR_OK && count == 1u &&
               one == '2');
        /* past the end is a refusal too, and also leaves the position */
        assert(do_seek(5, 1, AAR_SEEK_END, &pos) == AAR_E_INVAL);
        assert(do_seek(5, 11, AAR_SEEK_SET, &pos) == AAR_E_INVAL);
        assert(do_read(5, 1u, &one, 1u, &count) == AAR_OK && count == 1u &&
               one == '3');
        /* only the two past-the-end seeks reach the handler as refusals */
        assert(fk.seek_refusals == 2);
        assert(do_close(5) == AAR_OK);
        fixture_free();
    }
}

static void test_each_open_plan_acts_on_the_file_it_should(void)
{
    uint8_t b[16];
    uint32_t count;
    int n;

    /* READ: the file must exist, MODE_OLDFILE, no probe. */
    fixture(0);
    assert(do_open(3, R, "top.txt") == AAR_OK);
    assert(fk.opens[AAR_SERVE_MODE_OLD] == 1 && fk.probes == 0);
    assert(do_open(4, R, "missing") == AAR_E_NOENT);
    assert(do_close(3) == AAR_OK);

    /* WRITE: probes, never creates. */
    assert(do_open(3, W, "missing") == AAR_E_NOENT);
    assert(child_of(N_ROOT, "missing") < 0 && fk.opens[AAR_SERVE_MODE_READWRITE] == 0);
    assert(do_open(3, W, "top.txt") == AAR_OK);
    assert(fk.opens[AAR_SERVE_MODE_READWRITE] == 1);
    assert(do_read(3, 3u, b, sizeof(b), &count) == AAR_OK && count == 3u);
    assert(do_close(3) == AAR_OK);

    /* TRUNCATE without CREATE: SetFileSize(0) on an existing file only. */
    assert(do_open(3, W | T, "missing") == AAR_E_NOENT);
    assert(do_open(3, W | T, "top.txt") == AAR_OK);
    assert(fk.set_sizes == 1 && fk.nodes[child_of(N_ROOT, "top.txt")].size == 0u);
    assert(do_close(3) == AAR_OK);

    /* ... and MODE_NEWFILE where the handler has no SetFileSize. */
    fixture_free();
    fixture(0);
    fk.set_size_unsupported = 1;
    assert(do_open(3, W | T, "top.txt") == AAR_OK);
    assert(fk.set_sizes == 1 && fk.opens[AAR_SERVE_MODE_NEW] == 1);
    assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 0u);
    assert(do_close(3) == AAR_OK);

    /* CREATE alone keeps an existing file's contents, creates a missing one. */
    fixture_free();
    fixture(0);
    assert(do_open(3, W | C, "top.txt") == AAR_OK);
    assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 3u);
    assert(fk.opens[AAR_SERVE_MODE_NEW] == 0);
    assert(do_close(3) == AAR_OK);
    assert(do_open(3, W | C, "made.txt") == AAR_OK);
    n = child_of(N_ROOT, "made.txt");
    assert(n >= 0 && fk.nodes[n].size == 0u && fk.opens[AAR_SERVE_MODE_NEW] == 1);
    assert(do_close(3) == AAR_OK);

    /* CREATE|TRUNCATE replaces. */
    assert(do_open(3, W | C | T, "top.txt") == AAR_OK);
    assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 0u);
    assert(do_close(3) == AAR_OK);

    /* The flags themselves: refused before DOS is touched. */
    fixture_free();
    fixture(0);
    assert(do_open(3, 0u, "top.txt") == AAR_E_INVAL);
    assert(do_open(3, R | C, "top.txt") == AAR_E_INVAL);
    assert(do_open(3, W | X, "top.txt") == AAR_E_INVAL);
    assert(do_open(3, R | 64u, "top.txt") == AAR_E_INVAL);
    assert(fk.lock_rel_calls == 0 && fk.probes == 0);
    fixture_free();
}

static void test_exclusive_create_refuses_an_existing_file(void)
{
    int n;

    fixture(0);
    assert(do_open(3, W | C | X, "top.txt") == AAR_E_EXIST);
    assert(fk.opens[AAR_SERVE_MODE_NEW] == 0);
    /* held by another program: the lock is refused IN_USE, still EXIST */
    fk.nodes[child_of(N_ROOT, "top.txt")].held = 1;
    assert(do_open(3, W | C | X, "top.txt") == AAR_E_EXIST);
    assert(fk.opens[AAR_SERVE_MODE_NEW] == 0);
    /* and one that is not there is created */
    assert(do_open(3, W | C | X, "fresh.txt") == AAR_OK);
    n = child_of(N_ROOT, "fresh.txt");
    assert(n >= 0 && fk.opens[AAR_SERVE_MODE_NEW] == 1);
    assert(do_open(4, W | C | X, "fresh.txt") == AAR_E_EXIST);
    assert(do_close(3) == AAR_OK);
    /* EXCL with TRUNCATE is still exclusive */
    assert(do_open(3, W | C | T | X, "fresh.txt") == AAR_E_EXIST);
    fixture_free();
}

static void test_root_confinement(void)
{
    uint32_t count;
    uint8_t b[16];

    fixture(0);
    /* inside, with dot and parent components that stay inside */
    assert(do_open(3, R, "dir/../top.txt") == AAR_OK);
    assert(do_close(3) == AAR_OK);
    assert(do_open(3, R, "./dir/./hello.txt") == AAR_OK);
    assert(do_close(3) == AAR_OK);
    assert(do_open(3, R, "dir//hello.txt") == AAR_E_NOENT);
    assert(do_open(3, R, "dir//top.txt") == AAR_OK);
    assert(do_close(3) == AAR_OK);
    /* out of it */
    assert(do_open(4, R, "../outside/secret.txt") == AAR_E_ACCES);
    assert(do_open(4, R, "dir/../../outside/secret.txt") == AAR_E_ACCES);
    assert(do_open(4, R, "/outside/secret.txt") == AAR_E_ACCES);
    assert(do_open(4, R, "..") == AAR_E_ACCES);
    assert(do_open(4, W | C, "../new.txt") == AAR_E_ACCES);
    assert(child_of(N_VOL, "new.txt") < 0);
    /* a soft link whose target is outside ROOT */
    assert(do_open(4, R, "escape") == AAR_E_ACCES);
    assert(do_open(4, W | C | X, "escape") == AAR_E_ACCES);
    assert(do_stat("escape", &(struct aar_serve_done){ 0 }) == AAR_E_ACCES);
    assert(do_stat("../outside", &(struct aar_serve_done){ 0 }) == AAR_E_ACCES);
    assert(do_list_open(3, "../outside") == AAR_E_ACCES);
    /* an absolute name needs ABS */
    assert(do_open(4, R, "Vol:outside/secret.txt") == AAR_E_ACCES);
    assert(do_stat("Vol:outside/secret.txt", &(struct aar_serve_done){ 0 }) ==
           AAR_E_ACCES);
    assert(do_list_open(3, "Vol:outside") == AAR_E_ACCES);
    (void)count;
    (void)b;
    assert(fk.opens[AAR_SERVE_MODE_OLD] == 3);
    /* the doubled slash is the parent, so only a name of the parent resolves */
    fixture_free();

    /* with ABS an absolute name goes to the volume, outside ROOT or not */
    fixture(1);
    assert(do_open(3, R, "Vol:outside/secret.txt") == AAR_OK);
    assert(do_read(3, 10u, b, sizeof(b), &count) == AAR_OK && count == 10u);
    assert(memcmp(b, "top secret", 10u) == 0);
    assert(do_close(3) == AAR_OK);
    assert(do_open(3, R, "Vol:nothing") == AAR_E_NOENT);
    assert(do_stat("Vol:outside/secret.txt", &(struct aar_serve_done){ 0 }) ==
           AAR_OK);
    /* relative names stay confined even with ABS */
    assert(do_open(3, R, "../outside/secret.txt") == AAR_E_ACCES);
    /* a link out of ROOT is refused by stat, list and open with ABS on as well */
    assert(do_open(3, R, "escape") == AAR_E_ACCES);
    assert(do_stat("escape", &(struct aar_serve_done){ 0 }) == AAR_E_ACCES);
    assert(do_list_open(3, "escape") == AAR_E_ACCES);
    assert(do_stat("../outside", &(struct aar_serve_done){ 0 }) == AAR_E_ACCES);
    assert(do_list_open(3, "../outside") == AAR_E_ACCES);
    fixture_free();
}

/* A stop of any kind (aar_serve_stop, the input window, the platform ending the
 * app, a break) cancels every step that changes the file system: no open that
 * creates, truncates, replaces or updates, and no write. Reads, seeks and
 * closes still run. */
static void test_a_stop_cancels_every_mutating_step(void)
{
    static const uint8_t data[4] = { 'a', 'b', 'c', 'd' };
    uint32_t count = 99u;
    uint8_t b[8];
    int stop_kind;

    for (stop_kind = 0; stop_kind < 2; stop_kind++) {
        fixture(0);
        assert(do_open(5, W | C, "keep.txt") == AAR_OK); /* open before the stop */
        fk.stop = 1;
        fk.broken = stop_kind; /* a stop asked, or a break still pending */
        assert(do_write(5, data, sizeof(data), &count) == AAR_E_IO && count == 0u);
        assert(fk.writes == 0);
        assert(do_open(3, W | C, "new.txt") == AAR_E_IO);
        assert(child_of(N_ROOT, "new.txt") < 0);
        assert(do_open(3, W | T, "top.txt") == AAR_E_IO);
        assert(do_open(3, W | C | T, "top.txt") == AAR_E_IO);
        assert(do_open(3, W | C | X, "other.txt") == AAR_E_IO);
        assert(do_open(3, W, "top.txt") == AAR_E_IO);
        assert(do_open(3, W | C, "top.txt") == AAR_E_IO);
        assert(fk.set_sizes == 0 && fk.opens[AAR_SERVE_MODE_NEW] == 1);
        assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 3u);
        /* a read, a seek and a close still run */
        assert(do_open(3, R, "top.txt") == AAR_OK);
        assert(do_read(3, 3u, b, sizeof(b), &count) == AAR_OK || fk.broken);
        assert(do_close(3) == AAR_OK);
        assert(do_close(5) == AAR_OK);
        fixture_free();
    }
}

static void test_transport_failures_are_io_errors_not_dos_results(void)
{
    fixture(0);
    fk.fail_transport_lock = 1;
    strcpy(fk.fail_transport_name, "dir");
    assert(do_open(3, R, "dir/hello.txt") == AAR_E_IO);
    strcpy(fk.fail_transport_name, "top.txt");
    assert(do_open(3, R, "top.txt") == AAR_E_IO);
    assert(do_open(3, W | C | X, "top.txt") == AAR_E_IO);
    assert(fk.opens[AAR_SERVE_MODE_NEW] == 0);
    fk.fail_transport_lock = 0;
    assert(do_open(3, R, "top.txt") == AAR_OK);
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_a_lost_request_does_not_create_or_truncate(void)
{
    fixture(0);
    fk.live = 0;
    assert(do_open(3, W | C, "gone.txt") == AAR_E_IO);
    assert(child_of(N_ROOT, "gone.txt") < 0);
    fk.live = 1;
    /* a Ctrl-C an earlier wait consumed stops the destructive step after the
     * READWRITE open of a truncating open: nothing is truncated */
    fk.break_after_open = 1;
    assert(do_open(3, W | T, "top.txt") == AAR_E_IO);
    assert(fk.set_sizes == 0 && fk.nodes[child_of(N_ROOT, "top.txt")].size == 3u);
    assert(fk.live_fhs == 0);
    fk.break_after_open = 0;
    fk.broken = 0;
    /* CREATE|TRUNCATE (MODE_NEWFILE) does not even send its packet */
    fk.broken = 1;
    assert(do_open(3, W | C | T, "top.txt") == AAR_E_IO);
    assert(fk.opens[AAR_SERVE_MODE_NEW] == 0);
    assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 3u);
    /* a read-only open needs no such check */
    assert(do_open(3, R, "top.txt") == AAR_OK);
    fk.broken = 0;
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

/* Guards that a second guard or DOS itself would usually answer the same way:
 * each is observed by what the handler did not ask of DOS. */
static void test_each_guard_of_the_file_handlers_has_its_own_check(void)
{
    uint8_t b[32];
    uint32_t count = 0;

    /* an open of a request that is gone is not sent at all */
    fixture(0);
    fk.live = 0;
    assert(do_open(3, R, "top.txt") == AAR_E_IO);
    assert(fk.opens[AAR_SERVE_MODE_OLD] == 0);
    fk.live = 1;

    /* the request is lost while the open is out: the handle is closed, the
     * descriptor stays free */
    fk.lose_after_open = 1;
    assert(do_open(3, R, "top.txt") == AAR_E_IO);
    assert(fk.live_fhs == 0);
    fk.lose_after_open = 0;
    fk.live = 1;
    assert(do_close(3) == AAR_E_BADF);

    /* a write of a request that is gone does not reach the file */
    assert(do_open(3, W, "top.txt") == AAR_OK);
    fk.live = 0;
    assert(do_write(3, (const uint8_t *)"xyz", 3u, &count) == AAR_E_IO);
    assert(fk.writes == 0 && fk.nodes[child_of(N_ROOT, "top.txt")].size == 3u);
    fk.live = 1;
    /* a write that stores nothing is a full volume */
    fk.write_zero = 1;
    assert(do_write(3, (const uint8_t *)"xyz", 3u, &count) == AAR_E_NOSPC);
    fk.write_zero = 0;
    assert(do_close(3) == AAR_OK);

    /* a short read ends the transfer: one packet, not one more to see zero */
    assert(do_open(3, R, "dir/hello.txt") == AAR_OK);
    fk.reads = 0;
    assert(do_read(3, 20u, b, sizeof(b), &count) == AAR_OK && count == 11u);
    assert(fk.reads == 1);
    assert(do_close(3) == AAR_OK);

    /* a SetFileSize that lost its packet is an I/O error, not a truncation */
    fk.set_size_lost = 1;
    assert(do_open(3, W | T, "top.txt") == AAR_E_IO);
    assert(fk.live_fhs == 0);
    fk.set_size_lost = 0;
    fixture_free();

    /* an update of a file that is not there is refused before the open, with
     * an absolute name too */
    fixture(1);
    assert(do_open(3, W, "Vol:nothing") == AAR_E_NOENT);
    assert(fk.opens[AAR_SERVE_MODE_READWRITE] == 0);
    fixture_free();
}

static void test_the_descriptor_table(void)
{
    uint32_t fd;

    fixture(0);
    assert(do_open(2, R, "top.txt") == AAR_E_BADF);
    assert(do_open(3u + AAR_SERVE_FD_COUNT, R, "top.txt") == AAR_E_BADF);
    for (fd = 3u; fd < 3u + AAR_SERVE_FD_COUNT; fd++)
        assert(do_open(fd, R, "top.txt") == AAR_OK);
    assert(fk.live_fhs == (int)AAR_SERVE_FD_COUNT);
    assert(do_open(3, R, "top.txt") == AAR_E_BUSY);
    assert(fk.live_fhs == (int)AAR_SERVE_FD_COUNT);
    /* a close whose packet was never delivered keeps the slot */
    fk.close_undelivered = 1;
    assert(do_close(4) == AAR_E_IO);
    assert(do_open(4, R, "top.txt") == AAR_E_BUSY);
    fk.close_undelivered = 0;
    assert(do_close(4) == AAR_OK);
    assert(do_open(4, R, "top.txt") == AAR_OK);
    /* open with an unterminated or empty path */
    assert(do_open(3, R, "") == AAR_E_BUSY);
    fixture_free();

    fixture(0);
    assert(do_open(3, R, "") == AAR_E_INVAL);
    {
        struct aar_serve_req r = mkreq(AAR_OP_FILE_OPEN);
        struct aar_serve_done d;
        uint8_t full[AAR_SERVE_PATH_BYTES + 1u];

        memset(full, 'a', sizeof(full));
        r.arg[AAR_A_FILE_OPEN_FD] = 3u;
        r.arg[AAR_A_FILE_OPEN_FLAGS] = R;
        r.buf = full;
        r.buf_len = sizeof(full);
        assert(serve_one(&r, &d) == AAR_E_INVAL);
        r.buf = NULL;
        r.buf_len = 0;
        assert(serve_one(&r, &d) == AAR_E_INVAL);
    }
    fixture_free();
}

static void test_list_walks_the_directory(void)
{
    char name[40];
    uint32_t mode;

    fixture(0);
    /* the ROOT itself, by the empty name */
    assert(do_list_open(3, "") == AAR_OK);
    assert(do_list_open(3, "") == AAR_E_BUSY);
    assert(do_list_next(3, name, 40u, &mode) == AAR_OK);
    assert(strcmp(name, "dir") == 0 && mode == AAR_MODE_DIR);
    assert(do_list_next(3, name, 40u, &mode) == AAR_OK);
    assert(strcmp(name, "top.txt") == 0 && mode == AAR_MODE_FILE);
    assert(do_list_next(3, name, 40u, &mode) == AAR_OK);
    assert(strcmp(name, "escape") == 0);
    assert(do_list_next(3, name, 40u, &mode) == AAR_E_NOENT);
    assert(do_list_close(3) == AAR_OK);
    assert(do_list_close(3) == AAR_E_BADF);
    assert(do_list_next(3, name, 40u, &mode) == AAR_E_BADF);

    /* a subdirectory, and a name cut to the room the payload has */
    assert(do_list_open(4, "dir") == AAR_OK);
    assert(do_list_next(4, name, 4u, &mode) == AAR_OK);
    assert(strcmp(name, "hel") == 0 && mode == AAR_MODE_FILE);
    assert(do_list_next(4, name, 1u, &mode) == AAR_E_INVAL);
    assert(do_list_close(4) == AAR_OK);

    /* not a directory, not there */
    assert(do_list_open(3, "top.txt") == AAR_E_IO);
    assert(do_list_open(3, "nothing") == AAR_E_NOENT);
    assert(do_list_open(2, "") == AAR_E_BADF);
    /* files and lists have their own tables */
    assert(do_open(3, R, "top.txt") == AAR_OK);
    assert(do_list_open(3, "") == AAR_OK);
    assert(do_list_close(3) == AAR_OK);
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_unknown_ops_are_not_implemented(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;

    fixture(0);
    r = mkreq(AAR_OP_NONE);
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    r = mkreq(0x7fu);
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    r = mkreq(AAR_OP_MSG);
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    fixture_free();
}

static void test_dos_errors_map_to_aar_errors(void)
{
    assert(aar_serve_errno_from_io(0) == AAR_OK);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_OBJECT_NOT_FOUND) == AAR_E_NOENT);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_DIR_NOT_FOUND) == AAR_E_NOENT);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_NO_MORE_ENTRIES) == AAR_E_NOENT);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_DISK_FULL) == AAR_E_NOSPC);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_WRITE_PROTECTED) == AAR_E_ACCES);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_READ_PROTECTED) == AAR_E_ACCES);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_DELETE_PROTECTED) == AAR_E_ACCES);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_OBJECT_EXISTS) == AAR_E_EXIST);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_NO_FREE_STORE) == AAR_E_NOMEM);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_OBJECT_IN_USE) == AAR_E_BUSY);
    assert(aar_serve_errno_from_io(AAR_SERVE_IOERR_SEEK_ERROR) == AAR_E_INVAL);
    assert(aar_serve_errno_from_io(12345) == AAR_E_IO);
    assert(aar_serve_amiga_unix(0u, 0u, 0u) == 0u);
    assert(aar_serve_amiga_unix(1u, 2u, 100u) ==
           (uint32_t)AAR_SERVE_UNIX_DELTA + 86400u + 120u + 2u);
}

/* ---- Console ------------------------------------------------------------ */

struct fcon {
    int open_ok;
    int write_ok;
    int read_ok;
    int allowed;
    int live;
    int errors;
    int opens;
    int read_starts;
    uint32_t read_max;
    int writes;
    uint8_t written[8192];
    uint32_t written_n;
    uint32_t last_write;
};

static struct fcon fc;

static int c_open(void *ctx)
{
    (void)ctx;
    fc.opens++;
    return fc.open_ok;
}

static int c_write_start(void *ctx, const uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    if (!fc.write_ok)
        return 0;
    assert(bytes > 0 && bytes <= AAR_SERVE_CONSOLE_WRITE_BYTES);
    memcpy(fc.written + fc.written_n, data, bytes);
    fc.last_write = bytes;
    fc.writes++;
    return 1;
}

static int c_read_start(void *ctx, uint32_t max)
{
    (void)ctx;
    fc.read_starts++;
    fc.read_max = max;
    return fc.read_ok;
}

static int c_read_allowed(void *ctx)
{
    (void)ctx;
    return fc.allowed;
}

static int c_live(void *ctx)
{
    (void)ctx;
    return fc.live;
}

static void c_output_error(void *ctx)
{
    (void)ctx;
    fc.errors++;
}

static void console_fixture(void)
{
    struct aar_serve_console_ops ops;

    memset(&fc, 0, sizeof(fc));
    fc.open_ok = fc.write_ok = fc.read_ok = fc.allowed = fc.live = 1;
    memset(&ops, 0, sizeof(ops));
    ops.open = c_open;
    ops.write_start = c_write_start;
    ops.read_start = c_read_start;
    ops.read_allowed = c_read_allowed;
    ops.live = c_live;
    ops.output_error = c_output_error;
    aar_serve_core_init(&serve, &sink);
    aar_serve_console_init(&serve.console, &ops, &serve.sink);
    sunk_n = 0;
    next_id = 200u;
}

/* The write finished: what the DOS packet reported for the bytes it was
 * given (the whole span by default). */
static void write_finishes(int32_t written)
{
    fc.written_n += fc.last_write;
    aar_serve_console_write_done(&serve.console, written);
}

static void test_console_write_completes_when_the_dos_write_does(void)
{
    uint8_t text[3000];
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct aar_serve_done d;
    uint32_t i;

    console_fixture();
    for (i = 0; i < sizeof(text); i++)
        text[i] = (uint8_t)(i % 251u);
    r.buf = text;
    r.buf_len = sizeof(text);
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    /* one packet is in flight, nothing is completed */
    assert(fc.writes == 1 && fc.last_write == AAR_SERVE_CONSOLE_WRITE_BYTES);
    assert(sunk_n == 0 && aar_serve_console_output_pending(&serve.console));
    /* a second request meanwhile is busy: the firmware keeps one */
    {
        struct aar_serve_req r2 = mkreq(AAR_OP_CONSOLE_WRITE);
        struct aar_serve_done d2;

        r2.buf = text;
        r2.buf_len = 4u;
        assert(aar_serve_core_request(&serve, &r2, &d2) == AAR_SERVE_REQ_DONE);
        assert(d2.result == AAR_E_BUSY);
    }
    write_finishes((int32_t)fc.last_write);
    assert(fc.writes == 2 && sunk_n == 0);
    write_finishes((int32_t)fc.last_write);
    assert(fc.writes == 3 && fc.last_write == 3000u - 2048u && sunk_n == 0);
    write_finishes((int32_t)fc.last_write);
    assert(sunk_n == 1 && sunk[0].req.id == r.id);
    assert(sunk[0].done.result == AAR_OK);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 3000u);
    assert(fc.written_n == 3000u && memcmp(fc.written, text, 3000u) == 0);
    assert(!aar_serve_console_output_pending(&serve.console));
    assert(fc.errors == 0 && fc.opens >= 1);
}

static void test_console_write_edges(void)
{
    static const uint8_t text_init[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    uint8_t text[8];
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct aar_serve_done d;

    console_fixture();
    memcpy(text, text_init, sizeof(text));
    /* nothing to write completes at once */
    r.buf = text;
    r.buf_len = 0;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DONE);
    assert(d.result == AAR_OK && d.out[AAR_R_CONSOLE_COUNT] == 0u);
    /* bytes with no payload are refused */
    r.buf = NULL;
    r.buf_len = 8u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DONE);
    assert(d.result == AAR_E_INVAL);
    assert(fc.writes == 0);

    /* the console cannot be opened */
    console_fixture();
    fc.open_ok = 0;
    r.buf = text;
    r.buf_len = 8u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(fc.errors == 1 && fc.writes == 0);
    /* and stays failed */
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DONE);
    assert(d.result == AAR_E_IO && fc.errors == 1);

    /* the packet cannot start */
    console_fixture();
    fc.write_ok = 0;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(fc.errors == 1);
}

static void test_a_short_write_fails_the_output_after_its_partial_advance(void)
{
    uint8_t text[100];
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct aar_serve_done d;

    console_fixture();
    memset(text, 'x', sizeof(text));
    r.buf = text;
    r.buf_len = sizeof(text);
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    aar_serve_console_write_done(&serve.console, 40);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 40u);
    assert(fc.errors == 1);
    assert(fc.writes == 1);

    /* a failed write (0 or negative) advances nothing */
    console_fixture();
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    aar_serve_console_write_done(&serve.console, -1);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 0u && fc.errors == 1);
}

static void test_launcher_text_goes_before_app_text(void)
{
    static const uint8_t text_init[4] = { 'a', 'p', 'p', '!' };
    uint8_t text[4];
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct aar_serve_done d;

    console_fixture();
    memcpy(text, text_init, sizeof(text));
    aar_serve_console_launcher_status(&serve.console, "hello");
    assert(aar_serve_console_output_pending(&serve.console));
    aar_serve_console_pump(&serve.console);
    assert(fc.writes == 1 && memcmp(fc.written, "hello\n", 6u) == 0);
    /* the app asks while the launcher packet is in flight */
    r.buf = text;
    r.buf_len = 4u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(fc.writes == 1 && sunk_n == 0);
    aar_serve_console_launcher_status(&serve.console, "again");
    write_finishes(6);
    /* a launcher message queued before the app bytes goes first */
    assert(fc.writes == 2 && memcmp(fc.written + 6, "again\n", 6u) == 0);
    write_finishes(6);
    assert(fc.writes == 3 && memcmp(fc.written + 12, "app!", 4u) == 0);
    write_finishes(4);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_OK &&
           sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 4u);
    assert(!aar_serve_console_output_pending(&serve.console));

    /* a partial launcher write resumes where it left off */
    console_fixture();
    aar_serve_console_launcher_status(&serve.console, "abcdef");
    aar_serve_console_pump(&serve.console);
    fc.written_n += 0;
    aar_serve_console_write_done(&serve.console, 3);
    assert(fc.errors == 1);
}

static void test_launcher_output_keeps_status_room_for_non_diagnostics(void)
{
    uint8_t line[1000];
    uint32_t i;

    console_fixture();
    memset(line, 'd', sizeof(line));
    for (i = 0; i < 20u; i++)
        aar_serve_console_launcher_put(&serve.console, line, sizeof(line), 1);
    /* diagnostics stop at the limit and are counted; status text still fits */
    assert(serve.console.launcher_used <= AAR_SERVE_LAUNCHER_OUTPUT_BYTES -
                                          AAR_SERVE_LAUNCHER_STATUS_RESERVE_BYTES);
    assert(serve.console.launcher_dropped > 0u);
    {
        uint32_t before = serve.console.launcher_used;

        aar_serve_console_launcher_status(&serve.console, "still fits");
        assert(serve.console.launcher_used == before + 11u);
    }
}

static void test_log_records_become_lines(void)
{
    static const uint8_t plain[] = "boot ok";
    static const uint8_t with_nl[] = "line\n";
    uint8_t big[1000];
    uint32_t i;

    console_fixture();
    /* a record without a newline gets one; one with it is left alone */
    aar_serve_console_launcher_log(&serve.console, plain, 7u);
    assert(serve.console.launcher_used == 8u);
    aar_serve_console_launcher_log(&serve.console, with_nl, 5u);
    assert(serve.console.launcher_used == 13u);
    aar_serve_console_launcher_log(&serve.console, plain, 0u); /* empty: a blank line */
    assert(serve.console.launcher_used == 14u);
    aar_serve_console_pump(&serve.console);
    assert(fc.writes == 1 && fc.last_write == 14u);
    assert(memcmp(fc.written, "boot ok\nline\n\n", 14u) == 0);
    write_finishes(14);
    assert(!aar_serve_console_output_pending(&serve.console));

    /* logs are diagnostics: they stop at the limit and are counted */
    console_fixture();
    memset(big, 'l', sizeof(big));
    for (i = 0; i < 20u; i++)
        aar_serve_console_launcher_log(&serve.console, big, sizeof(big));
    assert(serve.console.launcher_dropped > 0u);
    assert(serve.console.launcher_used <= AAR_SERVE_LAUNCHER_OUTPUT_BYTES -
                                          AAR_SERVE_LAUNCHER_STATUS_RESERVE_BYTES);
}

static void test_app_write_opens_the_console_before_queued_diagnostics(void)
{
    static const uint8_t diag[] = "diag";
    static const uint8_t text[] = "hello";
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct aar_serve_done d;

    /* Launcher bytes are queued when the app's first CONSOLE_WRITE arrives
     * and go out first; the destination must be opened for the app anyway,
     * or on a Workbench launch their write has nowhere to go. */
    console_fixture();
    aar_serve_console_launcher_log(&serve.console, diag, 4u);
    assert(fc.opens == 0);
    r.buf = (uint8_t *)(uintptr_t)text;
    r.buf_len = 5u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(fc.opens >= 1);
    assert(fc.writes == 1 && memcmp(fc.written, "diag\n", 5u) == 0);
    write_finishes((int32_t)fc.last_write);
    assert(fc.writes == 2 && memcmp(fc.written + 5u, "hello", 5u) == 0);
    write_finishes((int32_t)fc.last_write);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_OK && fc.errors == 0);

    /* The destination cannot be opened: the request fails and the output is
     * a launcher error, as when the app's bytes go first. */
    console_fixture();
    fc.open_ok = 0;
    aar_serve_console_launcher_log(&serve.console, diag, 4u);
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED ||
           sunk_n == 1);
    aar_serve_console_pump(&serve.console);
    assert(fc.errors == 1);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(fc.writes == 0);
}

static void test_console_read_that_cannot_open_the_console_is_a_launcher_error(void)
{
    uint8_t buf[64];
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_READ);
    struct aar_serve_done d;

    console_fixture();
    fc.open_ok = 0;
    r.buf = buf;
    r.buf_len = sizeof(buf);
    if (aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED)
        aar_serve_console_pump(&serve.console);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(fc.errors == 1 && fc.read_starts == 0);
}

static void test_console_buffers_are_left_alone_after_the_claim_is_lost(void)
{
    uint8_t buf[64];
    static const uint8_t typed[] = "typed\n";
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_READ);
    struct aar_serve_req w = mkreq(AAR_OP_CONSOLE_WRITE);
    struct aar_serve_done d;

    /* A DOS read is out when the claim is revoked. Its bytes arrive later:
     * they must not be copied into the request's buffer, which is card
     * memory that another claim may own by then. */
    console_fixture();
    memset(buf, 0xee, sizeof(buf));
    r.buf = buf;
    r.buf_len = sizeof(buf);
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(fc.read_starts == 1);
    fc.live = 0;
    aar_serve_console_read_done(&serve.console, 6, typed);
    assert(buf[0] == 0xee && buf[5] == 0xee);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);

    /* A write that was waiting for the next chunk is not started: its
     * buffer is gone too. */
    console_fixture();
    w.buf_len = 3000u;                 /* more than one packet */
    {
        static uint8_t big[3000];

        w.buf = big;
        assert(aar_serve_core_request(&serve, &w, &d) == AAR_SERVE_REQ_DEFERRED);
    }
    assert(fc.writes == 1);
    fc.live = 0;
    write_finishes((int32_t)fc.last_write);
    assert(fc.writes == 1);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);

    /* The launcher still says the claim was lost: its queue drains, or the
     * cleanup that waits for it never ends. */
    aar_serve_console_launcher_status(&serve.console, "claim lost");
    aar_serve_console_pump(&serve.console);
    assert(fc.writes == 2 && fc.last_write == 11u);
    write_finishes((int32_t)fc.last_write);
    assert(!aar_serve_console_output_pending(&serve.console));
}

static void test_console_read_waits_for_demand_and_returns_what_was_read(void)
{
    uint8_t buf[512];
    uint8_t line[] = "typed\n";
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_READ);
    struct aar_serve_done d;

    console_fixture();
    fc.allowed = 0;
    r.buf = buf;
    r.buf_len = sizeof(buf);
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    /* the app is not RUNNING: no DOS read is started */
    assert(fc.read_starts == 0);
    aar_serve_console_pump(&serve.console);
    assert(fc.read_starts == 0);
    fc.allowed = 1;
    aar_serve_console_pump(&serve.console);
    assert(fc.read_starts == 1 && fc.read_max == AAR_SERVE_CONSOLE_READ_BYTES);
    aar_serve_console_pump(&serve.console);
    assert(fc.read_starts == 1); /* one read per request */
    /* a second outstanding read is refused */
    {
        struct aar_serve_req r2 = mkreq(AAR_OP_CONSOLE_READ);
        struct aar_serve_done d2;

        r2.buf = buf;
        r2.buf_len = sizeof(buf);
        assert(aar_serve_core_request(&serve, &r2, &d2) == AAR_SERVE_REQ_DONE);
        assert(d2.result == AAR_E_BUSY);
    }
    aar_serve_console_read_done(&serve.console, 6, line);
    assert(sunk_n == 1 && sunk[0].req.id == r.id);
    assert(sunk[0].done.result == AAR_OK);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 6u);
    assert(memcmp(buf, "typed\n", 6u) == 0);
    /* a room smaller than a line reads at most the room */
    r = mkreq(AAR_OP_CONSOLE_READ);
    r.buf = buf;
    r.buf_len = 3u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(fc.read_starts == 2 && fc.read_max == 3u);
    /* a longer line than the room never overruns it */
    aar_serve_console_read_done(&serve.console, 6, line);
    assert(sunk_n == 2 && sunk[1].done.out[AAR_R_CONSOLE_COUNT] == 3u);
}

static void test_console_read_end_of_input_is_sticky(void)
{
    uint8_t buf[16];
    struct aar_serve_req r = mkreq(AAR_OP_CONSOLE_READ);
    struct aar_serve_done d;

    console_fixture();
    r.buf = buf;
    r.buf_len = sizeof(buf);
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    aar_serve_console_read_done(&serve.console, 0, NULL);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_OK &&
           sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 0u);
    /* every later read answers 0 without touching DOS */
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DONE);
    assert(d.result == AAR_OK && d.out[AAR_R_CONSOLE_COUNT] == 0u);
    assert(fc.read_starts == 1);

    /* a failed read is the end of the input too */
    console_fixture();
    fc.read_ok = 0;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_OK &&
           sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 0u);
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DONE);
    assert(d.out[AAR_R_CONSOLE_COUNT] == 0u && fc.read_starts == 1);

    /* a negative DOS result is the end too; a console that will not open
     * fails the request */
    console_fixture();
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    aar_serve_console_read_done(&serve.console, -1, NULL);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 0u);
    console_fixture();
    fc.open_ok = 0;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);

    /* no payload, no room */
    console_fixture();
    r.buf = NULL;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DONE);
    assert(d.result == AAR_E_INVAL);
}

/* ---- Display ------------------------------------------------------------ */

struct fdisp {
    int open_calls;
    int changes;
    int change_ok;
    int last_to_back;
    int pans;
    uint32_t last_page;
    int tofs;
    int loads;
    uint32_t table[2 + 256 * 3];
    int closes;
    int have_two;
    int have_pan;
    uint32_t pan_count;
};

static uint32_t fd_features;    /* the AAR_DISPCAP_* bits the fake screen could provide */
static struct fdisp fd_;

static void d_open(void *ctx, const struct aar_serve_req *req, struct aar_serve_done *done)
{
    (void)ctx;
    fd_.open_calls++;
    /* 640x480 at 16 bit: pitch 1280, tokens at 0x00400000 */
    aar_serve_display_opened(&serve.display, req, 1280u, 0x00400000u,
                             fd_.have_two ? 0x00600000u :
                             (fd_.have_pan ? 0x00400000u + 1280u * 480u : 0u),
                             fd_.pan_count == 3u ? 0x00400000u + 2u * 1280u * 480u : 0u,
                             fd_.have_two, fd_.have_pan, fd_.pan_count, fd_features, done);
}

static int d_change(void *ctx, int to_back)
{
    (void)ctx;
    fd_.changes++;
    fd_.last_to_back = to_back;
    return fd_.change_ok;
}

static void d_pan(void *ctx, uint32_t page)
{
    (void)ctx;
    fd_.pans++;
    fd_.last_page = page;
}

static void d_tof(void *ctx)
{
    (void)ctx;
    fd_.tofs++;
}

static void d_load(void *ctx, const uint32_t *table)
{
    uint32_t count = table[0] >> 16;

    (void)ctx;
    fd_.loads++;
    memcpy(fd_.table, table, (2u + count * 3u) * 4u);
}

static void d_close(void *ctx)
{
    (void)ctx;
    fd_.closes++;
}

static void display_fixture(int two, int pan, uint32_t pan_count)
{
    struct aar_serve_display_ops ops;

    memset(&fd_, 0, sizeof(fd_));
    fd_.change_ok = 1;
    fd_.have_two = two;
    fd_.have_pan = pan;
    fd_.pan_count = pan_count;
    memset(&ops, 0, sizeof(ops));
    ops.open = d_open;
    ops.change_buffer = d_change;
    ops.pan = d_pan;
    ops.wait_tof = d_tof;
    ops.load_rgb32 = d_load;
    ops.close = d_close;
    aar_serve_core_init(&serve, &sink);
    aar_serve_display_init(&serve.display, &ops, &serve.sink);
    sunk_n = 0;
    next_id = 300u;
}

static struct aar_serve_done display_open(uint32_t w, uint32_t h, uint32_t fmt,
                                          uint32_t flags, int32_t expect)
{
    struct aar_serve_req r = mkreq(AAR_OP_DISPLAY_OPEN);
    struct aar_serve_done d;

    r.arg[AAR_A_DISPLAY_OPEN_W] = w;
    r.arg[AAR_A_DISPLAY_OPEN_H] = h;
    r.arg[AAR_A_DISPLAY_OPEN_FORMAT] = fmt;
    r.arg[AAR_A_DISPLAY_OPEN_FLAGS] = flags;
    assert(serve_one(&r, &d) == expect);
    return d;
}

static struct aar_serve_req flip_req(void)
{
    struct aar_serve_req r = mkreq(AAR_OP_DISPLAY_FLIP);

    r.arg[AAR_A_DISPLAY_ID] = AAR_SERVE_DISPLAY_ID;
    return r;
}

static void test_display_open_validates_and_reports_what_was_made(void)
{
    struct aar_serve_done d;

    display_fixture(0, 1, 3u);
    display_open(0u, 480u, AAR_FMT_RGB565, 0u, AAR_E_INVAL);
    display_open(640u, 0u, AAR_FMT_RGB565, 0u, AAR_E_INVAL);
    display_open(4097u, 480u, AAR_FMT_RGB565, 0u, AAR_E_INVAL);
    display_open(640u, 4097u, AAR_FMT_RGB565, 0u, AAR_E_INVAL);
    display_open(640u, 480u, 0u, 0u, AAR_E_INVAL);
    display_open(640u, 480u, 4u, 0u, AAR_E_INVAL);
    assert(fd_.open_calls == 0);

    d = display_open(640u, 480u, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, AAR_OK);
    assert(fd_.open_calls == 1);
    assert(d.out[AAR_R_DISPLAY_OPEN_ID] == AAR_SERVE_DISPLAY_ID);
    assert(d.out[AAR_R_DISPLAY_OPEN_PITCH] == 1280u);
    assert(d.out[AAR_R_DISPLAY_OPEN_LAYOUT] == AAR_LAYOUT_RGB565_BE);
    assert(d.out[AAR_R_DISPLAY_OPEN_FRONT] == 0x00400000u);
    assert(d.out[AAR_R_DISPLAY_OPEN_BACK] == 0x00400000u + 1280u * 480u);
    assert(d.out[AAR_R_DISPLAY_OPEN_SPARE] == 0x00400000u + 2u * 1280u * 480u);
    /* async and CLUT8 are always granted, latest only when asked for */
    assert(d.flags == (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8));
    /* a second open while one is open is refused */
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_E_BUSY);
    assert(fd_.open_calls == 1);

    assert(aar_serve_display_layout(AAR_FMT_BGRA8888) == AAR_LAYOUT_BGRA8888);
    assert(aar_serve_display_layout(AAR_FMT_CLUT8) == AAR_LAYOUT_CLUT8);
    assert(aar_serve_display_layout(AAR_FMT_RGB565) == AAR_LAYOUT_RGB565_BE);
    assert(aar_serve_display_depth(AAR_FMT_BGRA8888) == 32u);
    assert(aar_serve_display_depth(AAR_FMT_CLUT8) == 8u);
    assert(aar_serve_display_depth(AAR_FMT_RGB565) == 16u);
    assert(aar_serve_display_caps(AAR_DISPLAY_LATEST, 0u) ==
           (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8 | AAR_DISPCAP_LATEST));
    assert(aar_serve_display_caps(AAR_DISPLAY_VSYNC, 0u) ==
           (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8));

    display_fixture(0, 1, 2u);
    d = display_open(640u, 480u, AAR_FMT_CLUT8,
                     AAR_DISPLAY_LATEST | AAR_DISPLAY_VSYNC, AAR_OK);
    assert(d.flags == (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8 |
                       AAR_DISPCAP_LATEST));
    assert(d.out[AAR_R_DISPLAY_OPEN_LAYOUT] == AAR_LAYOUT_CLUT8);
    assert(d.out[AAR_R_DISPLAY_OPEN_SPARE] == 0u);
}

static void test_pan_flips_rotate_the_pages_and_rely_on_the_card_vblank(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;
    const uint32_t page = 1280u * 480u;
    const uint32_t base = 0x00400000u;

    display_fixture(0, 1, 3u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK);
    /* pan to page 1: it is the front, page 2 is the next to draw into */
    assert(fd_.pans == 1 && fd_.last_page == 1u && fd_.tofs == 0);
    assert(d.flags == AAR_CFLAG_FLIP_SUBMITTED);
    assert(d.out[AAR_R_DISPLAY_FLIP_FRONT] == base + page);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base + 2u * page);
    assert(d.out[AAR_R_DISPLAY_FLIP_PITCH] == 1280u);
    assert(d.out[AAR_R_DISPLAY_FLIP_LAYOUT] == AAR_LAYOUT_RGB565_BE);
    assert(d.out[AAR_R_DISPLAY_FLIP_DROPPED] == 0u);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.last_page == 2u);
    assert(d.out[AAR_R_DISPLAY_FLIP_FRONT] == base + 2u * page);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.last_page == 0u);
    assert(d.out[AAR_R_DISPLAY_FLIP_FRONT] == base);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base + page);
    assert(fd_.pans == 3 && sunk_n == 0);

    /* double buffering by panning: two pages */
    display_fixture(0, 1, 2u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.last_page == 1u);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.last_page == 0u);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base + page);
}

static void test_screen_buffer_flips_complete_on_the_safe_message(void)
{
    struct aar_serve_req r1;
    struct aar_serve_req r2;
    struct aar_serve_req r3;
    struct aar_serve_done d;

    display_fixture(1, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r1 = flip_req();
    assert(aar_serve_core_request(&serve, &r1, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(fd_.changes == 1 && fd_.last_to_back == 1 && sunk_n == 0);
    /* a second flip meanwhile waits its turn, a third is refused */
    r2 = flip_req();
    assert(aar_serve_core_request(&serve, &r2, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(fd_.changes == 1);
    r3 = flip_req();
    assert(aar_serve_core_request(&serve, &r3, &d) == AAR_SERVE_REQ_DONE);
    assert(d.result == AAR_E_BUSY);

    aar_serve_display_flip_safe(&serve.display);
    /* the first completes with the buffers as they were left (back showing);
     * the second flip started back to the front buffer */
    assert(sunk_n == 1 && sunk[0].req.id == r1.id && sunk[0].done.result == AAR_OK);
    assert(sunk[0].done.flags == 0u);
    assert(fd_.changes == 2 && fd_.last_to_back == 0);
    aar_serve_display_flip_safe(&serve.display);
    assert(sunk_n == 2 && sunk[1].req.id == r2.id && sunk[1].done.result == AAR_OK);
    /* a stray SafeMessage completes nothing */
    aar_serve_display_flip_safe(&serve.display);
    assert(sunk_n == 2);

    /* ChangeScreenBuffer fails */
    display_fixture(1, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    fd_.change_ok = 0;
    r1 = flip_req();
    assert(aar_serve_core_request(&serve, &r1, &d) == AAR_SERVE_REQ_DONE);
    assert(d.result == AAR_E_IO);
    /* the failed flip left nothing waiting for a message */
    aar_serve_display_flip_safe(&serve.display);
    assert(sunk_n == 0);

    /* a queued flip that cannot start completes with the error */
    display_fixture(1, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r1 = flip_req();
    r2 = flip_req();
    assert(aar_serve_core_request(&serve, &r1, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(aar_serve_core_request(&serve, &r2, &d) == AAR_SERVE_REQ_DEFERRED);
    fd_.change_ok = 0;
    aar_serve_display_flip_safe(&serve.display);
    assert(sunk_n == 2 && sunk[0].done.result == AAR_OK &&
           sunk[1].done.result == AAR_E_IO);
}

static void test_a_single_buffer_flip_waits_for_the_frame_only_when_asked(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;

    display_fixture(0, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fd_.tofs == 0 && fd_.pans == 0 && fd_.changes == 0);
    assert(d.flags == 0u);
    assert(d.out[AAR_R_DISPLAY_FLIP_FRONT] == 0x00400000u &&
           d.out[AAR_R_DISPLAY_FLIP_BACK] == 0x00400000u);

    display_fixture(0, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, AAR_OK);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.tofs == 1);
}

static void test_flips_name_the_open_display(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;

    display_fixture(0, 1, 2u);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_E_INVAL); /* nothing open */
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = flip_req();
    r.arg[AAR_A_DISPLAY_ID] = 9u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    assert(fd_.pans == 0);
}

static struct aar_serve_req palette_req(uint32_t first, uint32_t count,
                                        uint8_t *words, uint32_t bytes)
{
    struct aar_serve_req r = mkreq(AAR_OP_DISPLAY_PALETTE);

    r.arg[AAR_A_DISPLAY_ID] = AAR_SERVE_DISPLAY_ID;
    r.arg[AAR_A_DISPLAY_PALETTE_FIRST] = first;
    r.arg[AAR_A_DISPLAY_PALETTE_COUNT] = count;
    r.buf = words;
    r.buf_len = bytes;
    return r;
}

static void test_palette_becomes_a_loadrgb32_table(void)
{
    uint8_t words[16 * 4];
    struct aar_serve_req r;
    struct aar_serve_done d;
    uint32_t i;

    display_fixture(0, 1, 2u);
    display_open(320u, 200u, AAR_FMT_CLUT8, 0u, AAR_OK);
    for (i = 0; i < 16u; i++)
        aar_serve_put_be32(words + i * 4u, (i << 16) | ((i + 1u) << 8) | (i + 2u));
    r = palette_req(10u, 16u, words, sizeof(words));
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fd_.loads == 1);
    assert(fd_.table[0] == ((16u << 16) | 10u));
    for (i = 0; i < 16u; i++) {
        assert(fd_.table[1 + i * 3u] == i * 0x01010101u);
        assert(fd_.table[2 + i * 3u] == (i + 1u) * 0x01010101u);
        assert(fd_.table[3 + i * 3u] == (i + 2u) * 0x01010101u);
    }
    assert(fd_.table[1 + 16u * 3u] == 0u);
    /* the high byte of a word is ignored */
    aar_serve_put_be32(words, 0xff112233u);
    r = palette_req(0u, 1u, words, 4u);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fd_.table[1] == 0x11u * 0x01010101u &&
           fd_.table[2] == 0x22u * 0x01010101u &&
           fd_.table[3] == 0x33u * 0x01010101u);
    /* the last colour fits, one more does not */
    r = palette_req(255u, 1u, words, 4u);
    assert(serve_one(&r, &d) == AAR_OK);
    r = palette_req(255u, 2u, words, 8u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(256u, 1u, words, 4u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(300u, 1u, words, 4u); /* 256 - first wraps */
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(0u, 0u, words, 4u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* fewer bytes than colours */
    r = palette_req(0u, 2u, words, 7u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(0u, 2u, NULL, 0u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(0u, 1u, words, 4u);
    r.arg[AAR_A_DISPLAY_ID] = 5u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    assert(fd_.loads == 3);

    /* a full table fits the buffer the handler builds */
    {
        uint8_t all[256 * 4];

        memset(all, 0xff, sizeof(all));
        r = palette_req(0u, 256u, all, sizeof(all));
        assert(serve_one(&r, &d) == AAR_OK);
        assert(fd_.table[0] == (256u << 16) &&
               fd_.table[1 + 255u * 3u] == 0xffffffffu &&
               fd_.table[1 + 256u * 3u] == 0u);
    }

    /* only a CLUT8 screen has a palette */
    display_fixture(0, 1, 2u);
    display_open(320u, 200u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = palette_req(0u, 1u, words, 4u);
    assert(serve_one(&r, &d) == AAR_E_INVAL && fd_.loads == 0);
}

static void test_display_close_ends_the_screen_and_pending_flips(void)
{
    struct aar_serve_req r;
    struct aar_serve_req c;
    struct aar_serve_done d;

    display_fixture(1, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    c = mkreq(AAR_OP_DISPLAY_CLOSE);
    c.arg[AAR_A_DISPLAY_ID] = 7u;
    assert(serve_one(&c, &d) == AAR_E_INVAL && fd_.closes == 0);
    r = flip_req();
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    c.arg[AAR_A_DISPLAY_ID] = AAR_SERVE_DISPLAY_ID;
    assert(serve_one(&c, &d) == AAR_OK && fd_.closes == 1);
    /* the flip that was waiting for its SafeMessage is answered */
    assert(sunk_n == 1 && sunk[0].req.id == r.id &&
           sunk[0].done.result == AAR_E_IO);
    /* closed: a flip names nothing, and the screen can open again */
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    assert(serve_one(&c, &d) == AAR_E_INVAL && fd_.closes == 1);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    assert(fd_.open_calls == 2);
}

/* ---- Audio -------------------------------------------------------------- */

struct faud {
    int opens;
    int open_ok;
    int closes;
    uint32_t rate, channels, token, bytes, frames;
};

static struct faud fa;

static int a_open(void *ctx, uint32_t rate, uint32_t channels, uint32_t token,
                  uint32_t bytes, uint32_t frames)
{
    (void)ctx;
    fa.opens++;
    fa.rate = rate;
    fa.channels = channels;
    fa.token = token;
    fa.bytes = bytes;
    fa.frames = frames;
    return fa.open_ok;
}

static void a_close(void *ctx)
{
    (void)ctx;
    fa.closes++;
}

static void audio_fixture(void)
{
    struct aar_serve_audio_ops ops;

    memset(&fa, 0, sizeof(fa));
    fa.open_ok = 1;
    ops.ctx = NULL;
    ops.open = a_open;
    ops.close = a_close;
    aar_serve_core_init(&serve, &sink);
    aar_serve_audio_init(&serve.audio, &ops);
    next_id = 400u;
}

static struct aar_serve_req audio_req(uint32_t rate, uint32_t channels,
                                      uint32_t route, uint32_t token,
                                      uint32_t bytes)
{
    struct aar_serve_req r = mkreq(AAR_OP_AUDIO_OPEN);

    r.arg[AAR_A_AUDIO_OPEN_RATE] = rate;
    r.arg[AAR_A_AUDIO_OPEN_CHANNELS] = channels;
    r.arg[AAR_A_AUDIO_OPEN_ROUTE] = route;
    r.buf_token = token;
    r.buf_len = bytes;
    return r;
}

static void test_audio_open_binds_the_runtimes_ring(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;

    audio_fixture();
    /* 0x80 header + 8192 frames of 16 bit stereo */
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0x00120000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fa.opens == 1 && fa.rate == 44100u && fa.channels == 2u &&
           fa.token == 0x00120000u && fa.bytes == 0x8080u && fa.frames == 8192u);
    assert(d.out[AAR_R_AUDIO_OPEN_RATE] == 44100u);
    assert(d.out[AAR_R_AUDIO_OPEN_CHANNELS] == 2u);
    assert(d.out[AAR_R_AUDIO_OPEN_RING] == 0x00120000u);
    assert(d.out[AAR_R_AUDIO_OPEN_RING_BYTES] == 0x8080u);
    assert(d.out[AAR_R_AUDIO_OPEN_FRAMES] == 8192u);
    /* a block rounded up to 40 KiB still holds 8192 frames, no more */
    r = audio_req(22050u, 2u, AAR_AUDIO_ROUTE_AHI, 0x00130000u, 0xa000u);
    assert(serve_one(&r, &d) == AAR_OK && fa.frames == 8192u);
    assert(fa.closes == 1); /* reopening closes the old stream first */
    assert(d.out[AAR_R_AUDIO_OPEN_FRAMES] == 8192u);

    r = mkreq(AAR_OP_AUDIO_CLOSE);
    assert(serve_one(&r, &d) == AAR_OK && fa.closes == 2);
    assert(serve_one(&r, &d) == AAR_OK && fa.closes == 2); /* idempotent */
}

static void test_audio_open_refusals(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;

    audio_fixture();
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_PLATFORM, 0x1000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(0u, 2u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(44100u, 1u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x80u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x8080u);
    r.buf_off = 16u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    assert(fa.opens == 0);

    /* AHI cannot be opened */
    fa.open_ok = 0;
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_IO);
    assert(serve.audio.open_failed && !serve.audio.open);
}

static void test_audio_chunk_is_a_fiftieth_of_a_second_at_most(void)
{
    /* 44100 Hz: 2646 frames per 3/50 s */
    assert(aar_serve_audio_chunk_frames(5000u, 44100u, 2u, 24576u) == 2646u);
    assert(aar_serve_audio_chunk_frames(100u, 44100u, 2u, 24576u) == 100u);
    assert(aar_serve_audio_chunk_frames(0u, 44100u, 2u, 24576u) == 0u);
    /* 48 kHz: 2880 frames; the buffer holds 6144 frames of stereo */
    assert(aar_serve_audio_chunk_frames(5000u, 48000u, 2u, 24576u) == 2880u);
    assert(aar_serve_audio_chunk_frames(20000u, 192000u, 2u, 24576u) == 6144u);
    assert(aar_serve_audio_chunk_frames(5000u, 44100u, 0u, 24576u) == 0u);
}

/* ---- Events ------------------------------------------------------------- */

struct fev {
    int full;
    int posted;
    aar_event last;
    int calls;
};

static struct fev fe;

static int e_post(void *ctx, const aar_event *ev)
{
    (void)ctx;
    fe.calls++;
    if (fe.full)
        return AAR_E_BUSY;
    fe.posted++;
    fe.last = *ev;
    return AAR_OK;
}

static void test_a_stop_that_found_the_queue_full_is_retried(void)
{
    struct aar_serve_events e;
    struct aar_serve_events_ops ops;

    ops.ctx = NULL;
    ops.post = e_post;
    memset(&fe, 0, sizeof(fe));
    aar_serve_events_init(&e, &ops);

    /* an ordinary event goes through, with its fields */
    assert(aar_serve_events_push(&e, AAR_EV_KEY_DOWN, 0x45u, 0, 0, 0x8u, 1234u) ==
           AAR_OK);
    assert(fe.posted == 1 && fe.last.type == AAR_EV_KEY_DOWN &&
           fe.last.code == 0x45u && fe.last.buttons == 0x8u &&
           fe.last.time_ms == 1234u);
    assert(aar_serve_events_push(&e, AAR_EV_MOUSE, 0u, -3, 5, 1u, 5u) == AAR_OK);
    assert(fe.last.dx == -3 && fe.last.dy == 5);

    /* a full queue drops an ordinary event (the module counts it) ... */
    fe.full = 1;
    assert(aar_serve_events_push(&e, AAR_EV_KEY_UP, 0x45u, 0, 0, 0u, 1u) ==
           AAR_E_BUSY);
    assert(!aar_serve_events_pending(&e));
    /* ... but a stop is kept and retried until it is delivered */
    assert(aar_serve_events_push(&e, AAR_EV_STOP, 0u, 0, 0, 0u, 77u) == AAR_OK);
    assert(aar_serve_events_pending(&e));
    assert(aar_serve_events_push(&e, AAR_EV_STOP, 0u, 0, 0, 0u, 78u) == AAR_OK);
    aar_serve_events_flush(&e);
    assert(aar_serve_events_pending(&e));
    fe.full = 0;
    fe.posted = 0;
    aar_serve_events_flush(&e);
    assert(!aar_serve_events_pending(&e) && fe.posted == 1);
    assert(fe.last.type == AAR_EV_STOP && fe.last.time_ms == 77u);
    aar_serve_events_flush(&e);
    assert(fe.posted == 1);
    /* a stop that gets through at once is posted once */
    fe.posted = 0;
    assert(aar_serve_events_push(&e, AAR_EV_STOP, 0u, 0, 0, 0u, 1u) == AAR_OK);
    assert(fe.posted == 1 && !aar_serve_events_pending(&e));
}

/* ---- Display, API minor 6 ----------------------------------------------- */

#define ALL_FEATURES (AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE | AAR_DISPCAP_POINTER_ABS | \
                      AAR_DISPCAP_TEXT_INPUT | AAR_DISPCAP_CLOSE_REQUEST | AAR_DISPCAP_SCALED)
#define BASE_CAPS (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8)

static void test_display_caps_follow_the_flags_and_what_the_screen_could_do(void)
{
    struct aar_serve_done d;

    /* nothing asked: nothing granted, whatever the screen could do */
    assert(aar_serve_display_caps(0u, ALL_FEATURES) == BASE_CAPS);
    assert(aar_serve_display_caps(AAR_DISPLAY_WINDOW, ALL_FEATURES) ==
           (BASE_CAPS | AAR_DISPCAP_WINDOW));
    /* the window fell back to a screen: no window cap, and none that needs a window */
    assert(aar_serve_display_caps(AAR_DISPLAY_WINDOW, 0u) == BASE_CAPS);
    assert(aar_serve_display_caps(AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE |
                                      AAR_DISPLAY_CLOSE_REQUEST,
                                  AAR_DISPCAP_POINTER_ABS) == BASE_CAPS);
    /* resizable and the close request need the window */
    assert(aar_serve_display_caps(AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE, ALL_FEATURES) ==
           (BASE_CAPS | AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE));
    assert(aar_serve_display_caps(AAR_DISPLAY_RESIZABLE, ALL_FEATURES) == BASE_CAPS);
    assert(aar_serve_display_caps(AAR_DISPLAY_CLOSE_REQUEST, ALL_FEATURES) == BASE_CAPS);
    assert(aar_serve_display_caps(AAR_DISPLAY_WINDOW | AAR_DISPLAY_CLOSE_REQUEST, ALL_FEATURES) ==
           (BASE_CAPS | AAR_DISPCAP_WINDOW | AAR_DISPCAP_CLOSE_REQUEST));
    /* a window that cannot be sized grants no resize */
    assert(aar_serve_display_caps(AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE,
                                  AAR_DISPCAP_WINDOW) == (BASE_CAPS | AAR_DISPCAP_WINDOW));
    /* the others are granted when asked for and possible */
    assert(aar_serve_display_caps(AAR_DISPLAY_POINTER_ABS, ALL_FEATURES) ==
           (BASE_CAPS | AAR_DISPCAP_POINTER_ABS));
    assert(aar_serve_display_caps(AAR_DISPLAY_POINTER_ABS, 0u) == BASE_CAPS);
    assert(aar_serve_display_caps(AAR_DISPLAY_TEXT_INPUT, ALL_FEATURES) ==
           (BASE_CAPS | AAR_DISPCAP_TEXT_INPUT));
    assert(aar_serve_display_caps(AAR_DISPLAY_TEXT_INPUT, AAR_DISPCAP_POINTER_ABS) == BASE_CAPS);
    assert(aar_serve_display_caps(AAR_DISPLAY_SCALE, ALL_FEATURES) ==
           (BASE_CAPS | AAR_DISPCAP_SCALED));
    assert(aar_serve_display_caps(AAR_DISPLAY_SCALE, 0u) == BASE_CAPS);
    assert(aar_serve_display_caps(AAR_DISPLAY_LATEST | AAR_DISPLAY_VSYNC, 0u) ==
           (BASE_CAPS | AAR_DISPCAP_LATEST));
    assert(aar_serve_display_caps(0xFFFFFFFFu, ALL_FEATURES) ==
           (BASE_CAPS | AAR_DISPCAP_LATEST | ALL_FEATURES));
    /* the open reply carries them */
    fd_features = AAR_DISPCAP_WINDOW | AAR_DISPCAP_SCALED;
    display_fixture(0, 0, 0u);
    d = display_open(320u, 200u, AAR_FMT_RGB565,
                     AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE | AAR_DISPLAY_TEXT_INPUT, AAR_OK);
    assert(d.flags == (BASE_CAPS | AAR_DISPCAP_WINDOW | AAR_DISPCAP_SCALED));
    assert(serve.display.caps == d.flags);
    fd_features = 0;
}

struct fdx {
    int info_calls;
    aar_display_info info_out;
    uint32_t mode_total;
    uint32_t mode_max_seen;
    int resize_calls;
    int resize_result;
    uint32_t resize_front;
    uint32_t resize_w, resize_h, resize_format, resize_flags;
    char title[300];
    int title_calls;
    int title_result;
    int pointer_calls;
    uint32_t pointer_mode, pointer_w, pointer_h;
    int32_t pointer_hot_x, pointer_hot_y;
    const uint8_t *pointer_argb;
    int vb_watch_on, vb_watch_off, vb_can;
};
static struct fdx fx;

static void x_info(void *ctx, aar_display_info *out)
{
    (void)ctx;
    fx.info_calls++;
    out->refresh_mhz = fx.info_out.refresh_mhz;
    out->screen_width = fx.info_out.screen_width;
    out->screen_height = fx.info_out.screen_height;
    out->flags = fx.info_out.flags | AAR_DISPINFO_WINDOW; /* the core overrides WINDOW */
}

static uint32_t x_modes(void *ctx, aar_display_mode *out, uint32_t max)
{
    uint32_t i;

    (void)ctx;
    fx.mode_max_seen = max;
    for (i = 0; i < fx.mode_total && i < max; i++) {
        out[i].width = 640u + i;
        out[i].height = 480u + i;
        out[i].refresh_mhz = 50000u + i;
        out[i].formats = (1u << AAR_FMT_RGB565) | (1u << AAR_FMT_BGRA8888);
        out[i].flags = i == 1 ? AAR_MODEF_PREFERRED : 0u;
    }
    return fx.mode_total;
}

static void x_resize(void *ctx, const struct aar_serve_req *req, struct aar_serve_done *done)
{
    (void)ctx;
    fx.resize_calls++;
    fx.resize_w = req->arg[AAR_A_DISPLAY_OPEN_W];
    fx.resize_h = req->arg[AAR_A_DISPLAY_OPEN_H];
    fx.resize_format = req->arg[AAR_A_DISPLAY_OPEN_FORMAT];
    fx.resize_flags = req->arg[AAR_A_DISPLAY_OPEN_FLAGS];
    memset(done, 0, sizeof(*done));
    done->result = fx.resize_result;
    done->out[AAR_R_DISPLAY_OPEN_PITCH] = fx.resize_w * 2u;
    done->out[AAR_R_DISPLAY_OPEN_FRONT] = fx.resize_front;
}

static int x_title(void *ctx, const char *title)
{
    (void)ctx;
    fx.title_calls++;
    snprintf(fx.title, sizeof(fx.title), "%s", title);
    return fx.title_result;
}

static int x_pointer(void *ctx, uint32_t mode, uint32_t w, uint32_t h, int32_t hx, int32_t hy,
                     const uint8_t *argb)
{
    (void)ctx;
    fx.pointer_calls++;
    fx.pointer_mode = mode;
    fx.pointer_w = w;
    fx.pointer_h = h;
    fx.pointer_hot_x = hx;
    fx.pointer_hot_y = hy;
    fx.pointer_argb = argb;
    return AAR_OK;
}

static int x_vblank(void *ctx, int watch)
{
    (void)ctx;
    if (watch) {
        fx.vb_watch_on++;
        return fx.vb_can;
    }
    fx.vb_watch_off++;
    return 1;
}

/* A fixture with every optional call, and a display opened with flags. */
static void x_fixture(uint32_t flags)
{
    struct aar_serve_display_ops ops;

    memset(&fx, 0, sizeof(fx));
    fx.vb_can = 1;
    fx.resize_result = AAR_OK;
    fx.resize_front = 0x00500000u;
    fd_features = ALL_FEATURES;
    display_fixture(0, 0, 0u);
    ops = serve.display.ops;
    ops.info = x_info;
    ops.modes = x_modes;
    ops.resize = x_resize;
    ops.set_title = x_title;
    ops.set_pointer = x_pointer;
    ops.vblank = x_vblank;
    aar_serve_display_init(&serve.display, &ops, &serve.sink);
    display_open(320u, 200u, AAR_FMT_RGB565, flags, AAR_OK);
    sunk_n = 0;
}

static struct aar_serve_req dreq(uint16_t op)
{
    struct aar_serve_req r = mkreq(op);

    r.arg[AAR_A_DISPLAY_ID] = AAR_SERVE_DISPLAY_ID;
    return r;
}

static void test_display_info_reports_the_surface_and_what_the_screen_adds(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;

    x_fixture(AAR_DISPLAY_WINDOW | AAR_DISPLAY_SCALE);
    fx.info_out.refresh_mhz = 59940u;
    fx.info_out.screen_width = 1920u;
    fx.info_out.screen_height = 1080u;
    fx.info_out.flags = AAR_DISPINFO_ACTIVE;
    r = dreq(AAR_OP_DISPLAY_INFO);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(d.out[AAR_R_DISPLAY_INFO_WIDTH] == 320u && d.out[AAR_R_DISPLAY_INFO_HEIGHT] == 200u);
    assert(d.out[AAR_R_DISPLAY_INFO_REFRESH_MHZ] == 59940u);
    assert(d.out[AAR_R_DISPLAY_INFO_SCREEN_W] == 1920u && d.out[AAR_R_DISPLAY_INFO_SCREEN_H] == 1080u);
    assert(d.out[AAR_R_DISPLAY_INFO_FLAGS] ==
           (AAR_DISPINFO_WINDOW | AAR_DISPINFO_SCALED | AAR_DISPINFO_ACTIVE));
    /* WINDOW and SCALED are what was granted: a screen display says neither */
    x_fixture(0u);
    fx.info_out.flags = 0u;
    r = dreq(AAR_OP_DISPLAY_INFO);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(d.out[AAR_R_DISPLAY_INFO_FLAGS] == 0u);
    /* another id */
    r = dreq(AAR_OP_DISPLAY_INFO);
    r.arg[AAR_A_DISPLAY_ID] = 9u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* no display open */
    display_fixture(0, 0, 0u);
    r = dreq(AAR_OP_DISPLAY_INFO);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* a table without info: the refresh rate is unknown and the screen is the surface */
    display_open(320u, 200u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = dreq(AAR_OP_DISPLAY_INFO);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(d.out[AAR_R_DISPLAY_INFO_REFRESH_MHZ] == 0u);
    assert(d.out[AAR_R_DISPLAY_INFO_SCREEN_W] == 320u && d.out[AAR_R_DISPLAY_INFO_SCREEN_H] == 200u);
    assert(d.out[AAR_R_DISPLAY_INFO_FLAGS] == 0u);
    fd_features = 0;
}

static void test_display_modes_are_encoded_and_counted(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;
    uint8_t buf[AAR_DISPLAY_MODE_REC_BYTES * 4u + 5u];

    x_fixture(0u);
    fx.mode_total = 3u;
    memset(buf, 0xEE, sizeof(buf));
    r = mkreq(AAR_OP_DISPLAY_MODES);
    r.buf = buf;
    r.buf_len = sizeof(buf);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(d.out[AAR_R_DISPLAY_MODES_COUNT] == 3u && d.out[AAR_R_DISPLAY_MODES_TOTAL] == 3u);
    assert(fx.mode_max_seen == 4u); /* the room: whole records only */
    /* second record: 641 x 481, 50001 mHz, RGB565 | BGRA8888, preferred */
    assert(aar_serve_be32(buf + 24) == 641u && aar_serve_be32(buf + 28) == 481u);
    assert(aar_serve_be32(buf + 32) == 50001u);
    assert(aar_serve_be32(buf + 36) == ((1u << AAR_FMT_RGB565) | (1u << AAR_FMT_BGRA8888)));
    assert(aar_serve_be32(buf + 40) == AAR_MODEF_PREFERRED && aar_serve_be32(buf + 44) == 0u);
    assert(aar_serve_be32(buf + 0) == 640u && aar_serve_be32(buf + 20) == 0u);
    assert(buf[72] == 0xEE); /* nothing past the three records */
    /* room for two of three: two written, the total says three */
    r = mkreq(AAR_OP_DISPLAY_MODES);
    r.buf = buf;
    r.buf_len = AAR_DISPLAY_MODE_REC_BYTES * 2u + 23u;
    assert(serve_one(&r, &d) == AAR_OK);
    assert(d.out[AAR_R_DISPLAY_MODES_COUNT] == 2u && d.out[AAR_R_DISPLAY_MODES_TOTAL] == 3u);
    /* no room at all finds the count */
    r = mkreq(AAR_OP_DISPLAY_MODES);
    r.buf = NULL;
    r.buf_len = 0;
    assert(serve_one(&r, &d) == AAR_OK);
    assert(d.out[AAR_R_DISPLAY_MODES_COUNT] == 0u && d.out[AAR_R_DISPLAY_MODES_TOTAL] == 3u);
    /* it needs no open display, and a table without modes has none */
    display_fixture(0, 0, 0u);
    r = mkreq(AAR_OP_DISPLAY_MODES);
    r.buf = buf;
    r.buf_len = sizeof(buf);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(d.out[AAR_R_DISPLAY_MODES_COUNT] == 0u && d.out[AAR_R_DISPLAY_MODES_TOTAL] == 0u);
    fd_features = 0;
}

static void test_display_resize_reallocates_and_answers_like_open(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;

    x_fixture(AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE | AAR_DISPLAY_VSYNC);
    r = dreq(AAR_OP_DISPLAY_RESIZE);
    r.arg[AAR_A_DISPLAY_RESIZE_W] = 400u;
    r.arg[AAR_A_DISPLAY_RESIZE_H] = 300u;
    assert(serve_one(&r, &d) == AAR_OK);
    /* the table saw the open it replaces: the new size, the old format and flags */
    assert(fx.resize_calls == 1 && fx.resize_w == 400u && fx.resize_h == 300u);
    assert(fx.resize_format == AAR_FMT_RGB565);
    assert(fx.resize_flags == (AAR_DISPLAY_WINDOW | AAR_DISPLAY_RESIZABLE | AAR_DISPLAY_VSYNC));
    /* the reply is an open reply with the caps of the open */
    assert(d.out[AAR_R_DISPLAY_OPEN_ID] == AAR_SERVE_DISPLAY_ID);
    assert(d.out[AAR_R_DISPLAY_OPEN_PITCH] == 800u);
    assert(d.out[AAR_R_DISPLAY_OPEN_FRONT] == 0x00500000u);
    assert(d.out[AAR_R_DISPLAY_OPEN_LAYOUT] == AAR_LAYOUT_RGB565_BE);
    assert(d.flags == (BASE_CAPS | AAR_DISPCAP_WINDOW | AAR_DISPCAP_RESIZE));
    /* and the display is recorded at the new size */
    assert(serve.display.width == 400u && serve.display.height == 300u);
    assert(serve.display.front == 0x00500000u && serve.display.pitch == 800u);
    assert(serve.display.caps == d.flags && serve.display.open);
    /* sizes outside 1 to 4096 */
    r = dreq(AAR_OP_DISPLAY_RESIZE);
    r.arg[AAR_A_DISPLAY_RESIZE_W] = 0u;
    r.arg[AAR_A_DISPLAY_RESIZE_H] = 300u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.arg[AAR_A_DISPLAY_RESIZE_W] = 300u;
    r.arg[AAR_A_DISPLAY_RESIZE_H] = 4097u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.arg[AAR_A_DISPLAY_RESIZE_H] = 4096u;
    assert(serve_one(&r, &d) == AAR_OK);
    /* a failure of the table passes through and leaves the display as it was */
    fx.resize_result = AAR_E_NOMEM;
    r = dreq(AAR_OP_DISPLAY_RESIZE);
    r.arg[AAR_A_DISPLAY_RESIZE_W] = 10u;
    r.arg[AAR_A_DISPLAY_RESIZE_H] = 10u;
    assert(serve_one(&r, &d) == AAR_E_NOMEM);
    assert(serve.display.width == 300u && serve.display.height == 4096u);
    /* a table that leaves no token has not resized */
    fx.resize_result = AAR_OK;
    fx.resize_front = 0u;
    assert(serve_one(&r, &d) == AAR_E_IO);
    fx.resize_front = 0x00500000u;
    /* a flip that is pending: busy */
    serve.display.flip_active = 1;
    assert(serve_one(&r, &d) == AAR_E_BUSY);
    serve.display.flip_active = 0;
    serve.display.flip_queued = 1;
    assert(serve_one(&r, &d) == AAR_E_BUSY);
    serve.display.flip_queued = 0;
    /* the wrong id */
    r.arg[AAR_A_DISPLAY_ID] = 7u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* without the resize cap: not implemented, and the table is not asked */
    x_fixture(AAR_DISPLAY_WINDOW);
    r = dreq(AAR_OP_DISPLAY_RESIZE);
    r.arg[AAR_A_DISPLAY_RESIZE_W] = 10u;
    r.arg[AAR_A_DISPLAY_RESIZE_H] = 10u;
    assert(serve_one(&r, &d) == AAR_E_NOSYS && fx.resize_calls == 0);
    fd_features = 0;
}

static void test_display_title_is_converted_to_latin1(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;
    uint8_t buf[300];

    x_fixture(0u);
    r = dreq(AAR_OP_DISPLAY_TITLE);
    memset(buf, 0, sizeof(buf));
    strcpy((char *)buf, "Caf\xC3\xA9 \xE2\x82\xAC");
    r.buf = buf;
    r.buf_len = 12u;
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fx.title_calls == 1 && strcmp(fx.title, "Caf\xE9 ?") == 0);
    /* 255 bytes fit, 256 do not, and the NUL must be inside the buffer */
    memset(buf, 'a', sizeof(buf));
    buf[255] = 0;
    r.buf_len = 256u;
    assert(serve_one(&r, &d) == AAR_OK && strlen(fx.title) == 255u);
    buf[255] = 'a';
    buf[256] = 0;
    r.buf_len = 257u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    memset(buf, 'b', 10u);
    r.buf_len = 10u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.buf = NULL;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* the table's result is the call's */
    fx.title_result = AAR_E_NOSYS;
    r.buf = buf;
    buf[3] = 0;
    r.buf_len = 4u;
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    r.arg[AAR_A_DISPLAY_ID] = 3u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* a table without a title call */
    display_fixture(0, 0, 0u);
    display_open(320u, 200u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = dreq(AAR_OP_DISPLAY_TITLE);
    r.buf = buf;
    r.buf_len = 4u;
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    fd_features = 0;
}

static void test_display_pointer_is_checked_and_passed_on(void)
{
    struct aar_serve_req r;
    struct aar_serve_done d;
    static uint8_t argb[64u * 64u * 4u];

    x_fixture(0u);
    r = dreq(AAR_OP_DISPLAY_POINTER);
    r.arg[AAR_A_DISPLAY_POINTER_MODE] = AAR_POINTER_HIDDEN;
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fx.pointer_calls == 1 && fx.pointer_mode == AAR_POINTER_HIDDEN && !fx.pointer_argb);
    r.arg[AAR_A_DISPLAY_POINTER_MODE] = AAR_POINTER_BUSY;
    assert(serve_one(&r, &d) == AAR_OK && fx.pointer_mode == AAR_POINTER_BUSY);
    r.arg[AAR_A_DISPLAY_POINTER_MODE] = AAR_POINTER_DEFAULT;
    assert(serve_one(&r, &d) == AAR_OK && fx.pointer_mode == AAR_POINTER_DEFAULT);
    r.arg[AAR_A_DISPLAY_POINTER_MODE] = AAR_POINTER_CUSTOM + 1u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* a custom pointer needs a good image */
    r.arg[AAR_A_DISPLAY_POINTER_MODE] = AAR_POINTER_CUSTOM;
    r.arg[AAR_A_DISPLAY_POINTER_W] = 16u;
    r.arg[AAR_A_DISPLAY_POINTER_H] = 12u;
    r.arg[AAR_A_DISPLAY_POINTER_HOT_X] = 3u;
    r.arg[AAR_A_DISPLAY_POINTER_HOT_Y] = 11u;
    r.buf = argb;
    r.buf_len = 16u * 12u * 4u;
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fx.pointer_mode == AAR_POINTER_CUSTOM && fx.pointer_w == 16u && fx.pointer_h == 12u);
    assert(fx.pointer_hot_x == 3 && fx.pointer_hot_y == 11 && fx.pointer_argb == argb);
    r.buf_len = 16u * 12u * 4u - 1u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.buf_len = 16u * 12u * 4u;
    r.buf = NULL;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.buf = argb;
    r.arg[AAR_A_DISPLAY_POINTER_HOT_Y] = 12u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.arg[AAR_A_DISPLAY_POINTER_HOT_Y] = (uint32_t)(int32_t)-1;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.arg[AAR_A_DISPLAY_POINTER_HOT_Y] = 0u;
    r.arg[AAR_A_DISPLAY_POINTER_W] = 0u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.arg[AAR_A_DISPLAY_POINTER_W] = 65u;
    r.buf_len = 65u * 12u * 4u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.arg[AAR_A_DISPLAY_POINTER_W] = 64u;
    r.arg[AAR_A_DISPLAY_POINTER_H] = 64u;
    r.buf_len = sizeof(argb);
    assert(serve_one(&r, &d) == AAR_OK && fx.pointer_w == 64u && fx.pointer_h == 64u);
    r.arg[AAR_A_DISPLAY_ID] = 2u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* a table without a pointer call */
    display_fixture(0, 0, 0u);
    display_open(320u, 200u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = dreq(AAR_OP_DISPLAY_POINTER);
    r.arg[AAR_A_DISPLAY_POINTER_MODE] = AAR_POINTER_HIDDEN;
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    fd_features = 0;
}

static void test_display_vblank_is_parked_until_the_blank_or_the_timeout(void)
{
    struct aar_serve_req r, r2, c;
    struct aar_serve_done d;

    x_fixture(0u);
    aar_serve_display_clock(&serve.display, 1000u);
    r = dreq(AAR_OP_DISPLAY_VBLANK);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 0u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 20000u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    assert(fx.vb_watch_on == 1 && sunk_n == 0);
    assert(aar_serve_display_vblank_wait_us(&serve.display, 1000u) == 20000u);
    assert(aar_serve_display_vblank_wait_us(&serve.display, 1015u) == 5000u);
    /* one at a time */
    r2 = dreq(AAR_OP_DISPLAY_VBLANK);
    r2.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 5u;
    assert(serve_one(&r2, &d) == AAR_E_BUSY);
    /* nothing happens before the timeout */
    aar_serve_display_vblank_check(&serve.display, 1019u);
    assert(sunk_n == 0);
    /* the blank completes it with AAR_OK, once, and stops the watch */
    aar_serve_display_vblank_seen(&serve.display);
    assert(sunk_n == 1 && sunk[0].req.id == r.id && sunk[0].done.result == AAR_OK);
    assert(fx.vb_watch_off == 1);
    aar_serve_display_vblank_seen(&serve.display);
    aar_serve_display_vblank_check(&serve.display, 99999u);
    assert(sunk_n == 1);
    assert(aar_serve_display_vblank_wait_us(&serve.display, 1000u) == 0xFFFFFFFFu);
    /* the timeout: 1500 us is two milliseconds */
    sunk_n = 0;
    aar_serve_display_clock(&serve.display, 5000u);
    r = dreq(AAR_OP_DISPLAY_VBLANK);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 1500u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    aar_serve_display_vblank_check(&serve.display, 5001u);
    assert(sunk_n == 0);
    aar_serve_display_vblank_check(&serve.display, 5002u);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_TIMEDOUT && fx.vb_watch_off == 2);
    /* the clock wraps */
    sunk_n = 0;
    aar_serve_display_clock(&serve.display, 0xFFFFFFFFu - 1u);
    r = dreq(AAR_OP_DISPLAY_VBLANK);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 5000u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    aar_serve_display_vblank_check(&serve.display, 0xFFFFFFFFu);
    assert(sunk_n == 0);
    aar_serve_display_vblank_check(&serve.display, 3u);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_TIMEDOUT);
    /* a close, and an abort, end a parked request with AAR_E_IO */
    sunk_n = 0;
    aar_serve_display_clock(&serve.display, 100u);
    r = dreq(AAR_OP_DISPLAY_VBLANK);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 5000u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    c = dreq(AAR_OP_DISPLAY_CLOSE);
    assert(serve_one(&c, &d) == AAR_OK);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    x_fixture(0u);
    r = dreq(AAR_OP_DISPLAY_VBLANK);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 5000u;
    assert(aar_serve_core_request(&serve, &r, &d) == AAR_SERVE_REQ_DEFERRED);
    aar_serve_core_abort(&serve);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    /* a platform that cannot see the blank, and a table that has no call */
    x_fixture(0u);
    fx.vb_can = 0;
    r = dreq(AAR_OP_DISPLAY_VBLANK);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 5000u;
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    display_fixture(0, 0, 0u);
    display_open(320u, 200u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = dreq(AAR_OP_DISPLAY_VBLANK);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 5000u;
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    /* no display */
    display_fixture(0, 0, 0u);
    r = dreq(AAR_OP_DISPLAY_VBLANK);
    r.arg[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = 5000u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    fd_features = 0;
}

static void test_the_fit_rectangle_and_the_pointer_position_on_the_surface(void)
{
    uint32_t x, y, w, h;
    int32_t sx, sy;

    /* 320 x 200 in 640 x 480: twice as wide, bars above and below */
    aar_serve_fit_rect(320u, 200u, 640u, 480u, &x, &y, &w, &h);
    assert(x == 0u && y == 40u && w == 640u && h == 400u);
    /* in 1000 x 400: height-bound, bars at the sides */
    aar_serve_fit_rect(320u, 200u, 1000u, 400u, &x, &y, &w, &h);
    assert(x == 180u && y == 0u && w == 640u && h == 400u);
    /* the same aspect fills it, a smaller area scales down */
    aar_serve_fit_rect(320u, 200u, 160u, 100u, &x, &y, &w, &h);
    assert(x == 0u && y == 0u && w == 160u && h == 100u);
    aar_serve_fit_rect(1920u, 1080u, 100u, 100u, &x, &y, &w, &h);
    assert(x == 0u && y == 22u && w == 100u && h == 56u);
    /* never empty, never larger than the area */
    aar_serve_fit_rect(4096u, 1u, 10u, 10u, &x, &y, &w, &h);
    assert(w == 10u && h == 1u && y == 4u);
    aar_serve_fit_rect(0u, 5u, 10u, 10u, &x, &y, &w, &h);
    assert(w == 0u && h == 0u);
    /* shown 1:1 at the top-left: the position is the window's */
    aar_serve_surface_pos(17, -3, 320u, 200u, 0u, 0u, 0u, 0u, &sx, &sy);
    assert(sx == 17 && sy == -3);
    /* scaled: back through the rectangle, rounded down, outside values stay outside */
    aar_serve_surface_pos(320, 240, 320u, 200u, 0u, 40u, 640u, 400u, &sx, &sy);
    assert(sx == 160 && sy == 100);
    aar_serve_surface_pos(0, 40, 320u, 200u, 0u, 40u, 640u, 400u, &sx, &sy);
    assert(sx == 0 && sy == 0);
    aar_serve_surface_pos(639, 439, 320u, 200u, 0u, 40u, 640u, 400u, &sx, &sy);
    assert(sx == 319 && sy == 199);
    aar_serve_surface_pos(-1, 39, 320u, 200u, 0u, 40u, 640u, 400u, &sx, &sy);
    assert(sx == -1 && sy == -1);
    aar_serve_surface_pos(700, 500, 320u, 200u, 0u, 40u, 640u, 400u, &sx, &sy);
    assert(sx == 350 && sy == 230);
}

static void scale_check(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, uint32_t bpp,
                        uint32_t strip)
{
    uint8_t *src = malloc((size_t)sw * sh * bpp), *dst = malloc((size_t)dw * dh * bpp);
    uint8_t *tmp = malloc((size_t)dw * strip * bpp);
    uint32_t x, y, k, y0;

    assert(src && dst && tmp);
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++)
            for (k = 0; k < bpp; k++)
                src[(y * sw + x) * bpp + k] = (uint8_t)(y * 31u + x * 7u + k * 101u + 3u);
    for (y0 = 0; y0 < dh; y0 += strip) {
        uint32_t n = dh - y0 < strip ? dh - y0 : strip;

        aar_serve_scale_nn(src, sw * bpp, sw, sh, tmp, dw * bpp, dw, dh, bpp, y0, n);
        memcpy(dst + (size_t)y0 * dw * bpp, tmp, (size_t)n * dw * bpp);
    }
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            uint32_t sx = (uint32_t)(((uint64_t)(2u * x + 1u) * sw) / (2u * dw));
            uint32_t sy = (uint32_t)(((uint64_t)(2u * y + 1u) * sh) / (2u * dh));

            assert(memcmp(dst + ((size_t)y * dw + x) * bpp, src + ((size_t)sy * sw + sx) * bpp,
                          bpp) == 0);
        }
    }
    free(src);
    free(dst);
    free(tmp);
}

static void test_nearest_neighbour_scaling_matches_the_pixel_centre_formula(void)
{
    static const uint32_t sizes[][4] = {
        {4, 3, 8, 6}, {4, 3, 9, 7}, {8, 6, 3, 2}, {7, 5, 7, 5}, {1, 1, 5, 5},
        {5, 5, 1, 1}, {320, 200, 400, 250}, {100, 60, 31, 17}, {16, 16, 64, 48},
    };
    unsigned i, bpp;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (bpp = 1; bpp <= 4; bpp *= 2) {
            scale_check(sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3], bpp, 16u);
            scale_check(sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3], bpp, 1u);
            scale_check(sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3], bpp, 5u);
        }
}

static void test_wheel_keys_and_text_characters(void)
{
    int16_t dx, dy;
    uint16_t out[8];
    uint8_t m1[] = { 'a' };
    uint8_t m2[] = { 'a', 0xE9, ' ', '~' };
    uint8_t csi[] = { 0x9B, 'A', 'b', 0x9B, '1', ';', '2', '~', 'c' };
    uint8_t ctl[] = { 0x0D, 0x08, 0x1B, 0x7F, 0x85, 0x9F, 0xA0, 0xFF };

    assert(aar_serve_wheel_key(0x7Au, &dx, &dy) && dx == 0 && dy == -1);
    assert(aar_serve_wheel_key(0x7Bu, &dx, &dy) && dx == 0 && dy == 1);
    assert(aar_serve_wheel_key(0x7Cu, &dx, &dy) && dx == -1 && dy == 0);
    assert(aar_serve_wheel_key(0x7Du, &dx, &dy) && dx == 1 && dy == 0);
    assert(!aar_serve_wheel_key(0x79u, &dx, &dy) && dx == 0 && dy == 0);
    assert(!aar_serve_wheel_key(0x7Eu, &dx, &dy));
    assert(!aar_serve_wheel_key(0xFAu, &dx, &dy)); /* the caller removes the up bit first */

    assert(aar_serve_text_chars(m1, 1, 0u, 0xC0u, out, 8u) == 1u && out[0] == 'a');
    assert(aar_serve_text_chars(m2, 4, 1u, 0xC0u, out, 8u) == 4u);
    assert(out[0] == 'a' && out[1] == 0xE9u && out[2] == ' ' && out[3] == '~');
    /* the cursor and function keys are escape sequences, not text */
    assert(aar_serve_text_chars(csi, 9, 0u, 0xC0u, out, 8u) == 2u);
    assert(out[0] == 'b' && out[1] == 'c');
    /* control characters and the C1 range make none; 0xA0 and above are Latin-1 */
    assert(aar_serve_text_chars(ctl, 8, 0u, 0xC0u, out, 8u) == 2u);
    assert(out[0] == 0xA0u && out[1] == 0xFFu);
    /* an Amiga key held: a shortcut, not text */
    assert(aar_serve_text_chars(m1, 1, 0x40u, 0xC0u, out, 8u) == 0u);
    assert(aar_serve_text_chars(m1, 1, 0x80u, 0xC0u, out, 8u) == 0u);
    assert(aar_serve_text_chars(m1, 1, 0x08u, 0xC0u, out, 8u) == 1u);
    /* the output is limited, and nothing mapped is nothing */
    assert(aar_serve_text_chars(m2, 4, 0u, 0xC0u, out, 2u) == 2u);
    assert(aar_serve_text_chars(m2, 0, 0u, 0xC0u, out, 8u) == 0u);
    assert(aar_serve_text_chars(m2, -1, 0u, 0xC0u, out, 8u) == 0u);
    /* a CSI that the text ends in */
    assert(aar_serve_text_chars(csi, 1, 0u, 0xC0u, out, 8u) == 0u);
}

static void test_utf8_titles_become_latin1(void)
{
    char out[16];

    assert(aar_serve_utf8_to_latin1((const uint8_t *)"plain", 5u, out, sizeof(out)) == 5u);
    assert(strcmp(out, "plain") == 0);
    /* two byte sequences up to U+00FF are the same Latin-1 character */
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"\xC3\xA4\xC3\xB6\xC3\xBC\xC2\xA0", 8u, out,
                                    sizeof(out)) == 4u);
    assert(memcmp(out, "\xE4\xF6\xFC\xA0", 5) == 0);
    /* above U+00FF, and the three and four byte forms, are '?' */
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"\xC4\x80\xE2\x82\xAC\xF0\x9F\x98\x80z", 10u,
                                    out, sizeof(out)) == 4u);
    assert(strcmp(out, "???z") == 0);
    /* malformed: a lone continuation, a lead without its continuation, an overlong form, a
     * surrogate, a code point past U+10FFFF, a truncated end */
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"\x80" "a", 2u, out, sizeof(out)) == 2u);
    assert(strcmp(out, "?a") == 0);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"\xC3" "a", 2u, out, sizeof(out)) == 2u);
    assert(strcmp(out, "?a") == 0);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"\xC0\x80", 2u, out, sizeof(out)) == 2u);
    assert(strcmp(out, "??") == 0);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"\xE0\x80\x80", 3u, out, sizeof(out)) == 3u);
    assert(strcmp(out, "???") == 0);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"\xED\xA0\x80", 3u, out, sizeof(out)) == 3u);
    assert(strcmp(out, "???") == 0);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"\xF4\x90\x80\x80", 4u, out, sizeof(out)) == 4u);
    assert(strcmp(out, "????") == 0);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"ab\xE2\x82", 4u, out, sizeof(out)) == 4u);
    assert(strcmp(out, "ab??") == 0);
    /* a NUL ends it, n limits it, cap limits the output */
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"ab\0cd", 5u, out, sizeof(out)) == 2u);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"abcdef", 3u, out, sizeof(out)) == 3u);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"abcdef", 6u, out, 4u) == 3u);
    assert(strcmp(out, "abc") == 0);
    assert(aar_serve_utf8_to_latin1((const uint8_t *)"abc", 3u, out, 0u) == 0u);
}

static void test_pointer_images_are_quantised_to_the_pointer_colours(void)
{
    /* the Preferences colours: black, white, orange */
    static const uint32_t pal[3] = { 0x000000u, 0xFFFFFFu, 0xFF8800u };
    uint8_t argb[16 * 2 * 4];
    uint8_t p0[12], p1[12];
    uint32_t x;

    assert(aar_serve_pointer_pixel(0x00FFFFFFu, pal) == 0u);  /* transparent */
    assert(aar_serve_pointer_pixel(0x7FFFFFFFu, pal) == 0u);  /* just under half */
    assert(aar_serve_pointer_pixel(0x80FFFFFFu, pal) == 2u);  /* just over: white */
    assert(aar_serve_pointer_pixel(0xFF000000u, pal) == 1u);
    assert(aar_serve_pointer_pixel(0xFFFF8000u, pal) == 3u);
    assert(aar_serve_pointer_pixel(0xFFE0E0E0u, pal) == 2u);
    assert(aar_serve_pointer_pixel(0xFF202020u, pal) == 1u);
    assert(aar_serve_pointer_pixel(0xFFFF0000u, pal) == 3u);  /* red is nearer orange than black */
    /* row 0: pixels 0..3 are transparent, black, white, orange, the rest transparent; row 1: white */
    memset(argb, 0, sizeof(argb));
    for (x = 0; x < 16; x++)
        aar_serve_put_be32(argb + (16u + x) * 4u, 0xFFFFFFFFu);
    aar_serve_put_be32(argb + 1u * 4u, 0xFF000000u);
    aar_serve_put_be32(argb + 2u * 4u, 0xFFFFFFFFu);
    aar_serve_put_be32(argb + 3u * 4u, 0xFFFF8800u);
    memset(p0, 0xAA, sizeof(p0));
    memset(p1, 0xAA, sizeof(p1));
    /* rows of 4 bytes, so that the planes of 16 pixels have room to spare */
    aar_serve_pointer_planes(argb, 16u, 2u, pal, p0, p1, 4u);
    /* pixel 1 is value 1 (plane 0), pixel 2 value 2 (plane 1), pixel 3 value 3 (both) */
    assert(p0[0] == 0x50u && p0[1] == 0u && p0[2] == 0u && p0[3] == 0u);
    assert(p1[0] == 0x30u && p1[1] == 0u && p1[2] == 0u && p1[3] == 0u);
    /* white is 2: plane 1 only; the rows are cleared whole */
    assert(p0[4] == 0u && p0[5] == 0u && p0[6] == 0u && p0[7] == 0u);
    assert(p1[4] == 0xFFu && p1[5] == 0xFFu && p1[6] == 0u && p1[7] == 0u);
    /* nothing past the rows is touched */
    assert(p0[8] == 0xAAu && p1[8] == 0xAAu && p0[11] == 0xAAu && p1[11] == 0xAAu);
}

static void test_refresh_rates_and_mode_records(void)
{
    aar_display_mode m[3];
    uint8_t rec[AAR_DISPLAY_MODE_REC_BYTES];

    /* PAL: 312 lines of 227 colour clocks of 280 ns */
    assert(aar_serve_refresh_mhz(312u, 227u, 3571429u) == 50427u);
    assert(aar_serve_refresh_mhz(525u, 113u, 3571429u) == 60201u);
    assert(aar_serve_refresh_mhz(0u, 227u, 3571429u) == 0u);
    assert(aar_serve_refresh_mhz(312u, 0u, 3571429u) == 0u);
    assert(aar_serve_refresh_mhz(312u, 227u, 0u) == 0u);
    assert(aar_serve_refresh_mhz(5000u, 227u, 3571429u) == 0u);
    assert(aar_serve_refresh_mhz(2000u, 500u, 3571429u) == 0u);  /* 3.5 Hz */
    assert(aar_serve_refresh_mhz(10u, 100u, 3571429u) == 0u);    /* 3.5 kHz */

    m[0].width = 0x01020304u;
    m[0].height = 480u;
    m[0].refresh_mhz = 60000u;
    m[0].formats = 6u;
    m[0].flags = 1u;
    m[0].reserved = 0xFFFFFFFFu;
    memset(rec, 0xEE, sizeof(rec));
    aar_serve_mode_encode(&m[0], rec);
    assert(rec[0] == 1u && rec[1] == 2u && rec[2] == 3u && rec[3] == 4u);
    assert(aar_serve_be32(rec + 4) == 480u && aar_serve_be32(rec + 8) == 60000u);
    assert(aar_serve_be32(rec + 12) == 6u && aar_serve_be32(rec + 16) == 1u);
    assert(aar_serve_be32(rec + 20) == 0u); /* the reserved word is 0 */

    memset(m, 0, sizeof(m));
    m[0].width = 640u; m[0].height = 480u;
    m[1].width = 1280u; m[1].height = 720u; m[1].flags = AAR_MODEF_PREFERRED;
    m[2].width = 1280u; m[2].height = 720u;
    aar_serve_modes_prefer(m, 3u, 1280u, 720u);
    assert(m[0].flags == 0u && m[1].flags == AAR_MODEF_PREFERRED && m[2].flags == 0u);
    aar_serve_modes_prefer(m, 3u, 640u, 480u);
    assert(m[0].flags == AAR_MODEF_PREFERRED && m[1].flags == 0u);
    aar_serve_modes_prefer(m, 3u, 800u, 600u);
    assert(m[0].flags == 0u && m[1].flags == 0u && m[2].flags == 0u);
}

static void test_cd32_pad_values_map_to_joy_bits(void)
{
    int16_t dx, dy;

    assert(aar_serve_cd32_valid(AAR_SERVE_JP_TYPE_GAMECTLR));
    assert(aar_serve_cd32_valid(AAR_SERVE_JP_TYPE_JOYSTK));
    assert(!aar_serve_cd32_valid(0x00000000UL));        /* not available */
    assert(!aar_serve_cd32_valid(0x20000000UL));        /* a mouse */
    assert(!aar_serve_cd32_valid(0x40000000UL));        /* unknown */
    assert(aar_serve_cd32_buttons(0u) == 0u);
    assert(aar_serve_cd32_buttons(AAR_SERVE_JPF_RED) == AAR_JOY_BUTTON1);
    assert(aar_serve_cd32_buttons(AAR_SERVE_JPF_BLUE) == AAR_JOY_BUTTON2);
    assert(aar_serve_cd32_buttons(AAR_SERVE_JPF_GREEN) == AAR_JOY_BUTTON3);
    assert(aar_serve_cd32_buttons(AAR_SERVE_JPF_YELLOW) == AAR_JOY_BUTTON4);
    assert(aar_serve_cd32_buttons(AAR_SERVE_JPF_PLAY) == AAR_JOY_PLAY);
    assert(aar_serve_cd32_buttons(AAR_SERVE_JPF_FORWARD) == AAR_JOY_FORWARD);
    assert(aar_serve_cd32_buttons(AAR_SERVE_JPF_REVERSE) == AAR_JOY_REVERSE);
    assert(aar_serve_cd32_buttons(AAR_SERVE_JPF_RED | AAR_SERVE_JPF_PLAY | AAR_SERVE_JPF_REVERSE) ==
           (AAR_JOY_BUTTON1 | AAR_JOY_PLAY | AAR_JOY_REVERSE));
    /* the type and the directions are not buttons */
    assert(aar_serve_cd32_buttons(AAR_SERVE_JP_TYPE_GAMECTLR | 0xFu) == 0u);
    aar_serve_cd32_axes(0u, &dx, &dy);
    assert(dx == 0 && dy == 0);
    aar_serve_cd32_axes(AAR_SERVE_JPF_LEFT | AAR_SERVE_JPF_UP, &dx, &dy);
    assert(dx == -1 && dy == -1);
    aar_serve_cd32_axes(AAR_SERVE_JPF_RIGHT | AAR_SERVE_JPF_DOWN, &dx, &dy);
    assert(dx == 1 && dy == 1);
    aar_serve_cd32_axes(AAR_SERVE_JPF_RIGHT | AAR_SERVE_JPF_LEFT | AAR_SERVE_JPF_RED, &dx, &dy);
    assert(dx == -1); /* both: the left one wins, as the register reading does */
}

int main(void)
{
    test_dos_errors_map_to_aar_errors();
    test_read_in_chunks_and_past_the_end();
    test_read_stops_at_a_break_with_the_bytes_so_far();
    test_write_seek_and_stat();
    test_seek_refusal_is_inval_whatever_res2();
    test_each_open_plan_acts_on_the_file_it_should();
    test_exclusive_create_refuses_an_existing_file();
    test_root_confinement();
    test_transport_failures_are_io_errors_not_dos_results();
    test_a_lost_request_does_not_create_or_truncate();
    test_a_stop_cancels_every_mutating_step();
    test_each_guard_of_the_file_handlers_has_its_own_check();
    test_the_descriptor_table();
    test_list_walks_the_directory();
    test_unknown_ops_are_not_implemented();
    test_console_write_completes_when_the_dos_write_does();
    test_console_write_edges();
    test_a_short_write_fails_the_output_after_its_partial_advance();
    test_launcher_text_goes_before_app_text();
    test_launcher_output_keeps_status_room_for_non_diagnostics();
    test_log_records_become_lines();
    test_app_write_opens_the_console_before_queued_diagnostics();
    test_console_read_that_cannot_open_the_console_is_a_launcher_error();
    test_console_buffers_are_left_alone_after_the_claim_is_lost();
    test_console_read_waits_for_demand_and_returns_what_was_read();
    test_console_read_end_of_input_is_sticky();
    test_display_open_validates_and_reports_what_was_made();
    test_display_caps_follow_the_flags_and_what_the_screen_could_do();
    test_display_info_reports_the_surface_and_what_the_screen_adds();
    test_display_modes_are_encoded_and_counted();
    test_display_resize_reallocates_and_answers_like_open();
    test_display_title_is_converted_to_latin1();
    test_display_pointer_is_checked_and_passed_on();
    test_display_vblank_is_parked_until_the_blank_or_the_timeout();
    test_the_fit_rectangle_and_the_pointer_position_on_the_surface();
    test_nearest_neighbour_scaling_matches_the_pixel_centre_formula();
    test_wheel_keys_and_text_characters();
    test_utf8_titles_become_latin1();
    test_pointer_images_are_quantised_to_the_pointer_colours();
    test_refresh_rates_and_mode_records();
    test_cd32_pad_values_map_to_joy_bits();
    test_pan_flips_rotate_the_pages_and_rely_on_the_card_vblank();
    test_screen_buffer_flips_complete_on_the_safe_message();
    test_a_single_buffer_flip_waits_for_the_frame_only_when_asked();
    test_flips_name_the_open_display();
    test_palette_becomes_a_loadrgb32_table();
    test_display_close_ends_the_screen_and_pending_flips();
    test_audio_open_binds_the_runtimes_ring();
    test_audio_open_refusals();
    test_audio_chunk_is_a_fiftieth_of_a_second_at_most();
    test_a_stop_that_found_the_queue_full_is_retried();
    puts("serve_core_test: ok");
    return 0;
}
