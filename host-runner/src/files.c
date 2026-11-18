/*
 * File proxy. Relative paths stay under --root. A component that is ".." or
 * empty (a leading slash, or two slashes) is the parent, as in an AmigaDOS
 * path. A parent step at the root is AAR_E_ACCES. A path containing ':' is
 * AAR_E_ACCES unless --abs, in which case the root check does not apply and
 * the string is opened as a host path.
 *
 * File ids are 16 integers starting at AAR_FD_BASE, and the list ids are a
 * separate table of 16 from the same range. The file flags go to the host's
 * open() as they are (CREATE, TRUNCATE, EXCL), so each is atomic there.
 * map_file reads at most 16 MiB into a runtime-owned shared
 * block, so the file counts against the shared limit as it does on the
 * ZZ9000. A larger file is AAR_E_NOSPC. A zero-length file is AAR_E_NOMEM,
 * because a shared buffer cannot be empty.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 700
#endif

#include "host_internal.h"

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_IFREG
#define S_IFREG _S_IFREG
#endif
#ifndef S_IFMT
#define S_IFMT (_S_IFDIR | _S_IFREG)
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

struct host_fd {
    int used;
    FILE *fp;
};

struct host_list {
    int used;
    char path[4096];
#ifdef _WIN32
    HANDLE find;
    WIN32_FIND_DATAA data;
    int have;
    int done;
#else
    DIR *dir;
#endif
};

static struct host_fd fds[HOST_FD_COUNT];
static struct host_list lists[HOST_FD_COUNT];
static char root_abs[4096];
static int abs_paths;

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

static void to_slash(char *p)
{
    for (; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
}

static void strip_trailing_sep(char *p)
{
    size_t n = strlen(p);

    while (n > 1u && is_sep(p[n - 1u])) {
        if (n == 3u && p[1] == ':')
            break;
        p[--n] = 0;
    }
}

static int make_abs(const char *in, char *out, size_t n)
{
#ifdef _WIN32
    DWORD got = GetFullPathNameA(in, (DWORD)n, out, NULL);
    if (got == 0 || got >= n)
        return -1;
#else
    (void)n;
    if (!realpath(in, out))
        return -1;
#endif
    to_slash(out);
    strip_trailing_sep(out);
    return 0;
}

static int slot_of(int id)
{
    if (id < HOST_FD_BASE || id >= HOST_FD_BASE + HOST_FD_COUNT)
        return -1;
    return id - HOST_FD_BASE;
}

static int at_root(const char *cur)
{
    return strcmp(cur, root_abs) == 0;
}

static int step_parent(char *cur)
{
    char *slash;

    if (at_root(cur))
        return AAR_E_ACCES;
    slash = strrchr(cur, '/');
    if (!slash)
        return AAR_E_ACCES;
    if (slash == cur)
        slash[1] = 0;
    else if (slash - cur == 2 && cur[1] == ':')
        slash[1] = 0;
    else
        *slash = 0;
    if (strlen(cur) < strlen(root_abs) ||
        strncmp(cur, root_abs, strlen(root_abs)) != 0)
        return AAR_E_ACCES;
    return AAR_OK;
}

static int join_comp(char *cur, size_t n, const char *comp)
{
    size_t len = strlen(cur);
    int need = !(len > 0u && cur[len - 1u] == '/');
    size_t add = strlen(comp) + (need ? 1u : 0u);

    if (len + add + 1u > n)
        return AAR_E_INVAL;
    if (need)
        cur[len++] = '/';
    memcpy(cur + len, comp, strlen(comp) + 1u);
    return AAR_OK;
}

static int path_stat(const char *path, struct stat *st)
{
#ifdef _WIN32
    struct _stat64 wide;
    if (_stat64(path, &wide) != 0)
        return -1;
    memset(st, 0, sizeof *st);
    st->st_mode = (wide.st_mode & _S_IFDIR) ? S_IFDIR : S_IFREG;
    st->st_size = (off_t)wide.st_size;
    st->st_mtime = (time_t)wide.st_mtime;
    return 0;
#else
    return stat(path, st);
#endif
}

static int is_dir_path(const char *path)
{
    struct stat st;

    if (path_stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
}

/*
 * Walk path the way an AmigaDOS path reads. On success, out is the host path
 * of the parent directory and leaf is the final component. A missing
 * intermediate directory is AAR_E_NOENT. The leaf itself may be absent when
 * creating.
 */
static int resolve_rel(const char *path, char *out, size_t out_n, char *leaf,
                       size_t leaf_n, int creating)
{
    const char *p = path;
    char cur[4096];
    char comp[256];
    int have_hold = 0;
    char hold[256];

    if (strlen(root_abs) + 1u > out_n)
        return AAR_E_INVAL;
    memcpy(cur, root_abs, strlen(root_abs) + 1u);
    hold[0] = 0;
    leaf[0] = 0;
    for (;;) {
        size_t i = 0;
        int parent = 0;
        int end = 0;

        if (*p == 0) {
            end = 1;
        } else if (is_sep(*p)) {
            parent = 1;
            p++;
        } else {
            while (*p && !is_sep(*p)) {
                if (i + 1u >= sizeof comp)
                    return AAR_E_INVAL;
                comp[i++] = *p++;
            }
            comp[i] = 0;
            if (is_sep(*p))
                p++;
        }
        if (end)
            break;
        if (parent || strcmp(comp, "..") == 0) {
            if (have_hold) {
                int rc = join_comp(cur, sizeof cur, hold);
                if (rc != AAR_OK)
                    return rc;
                if (!is_dir_path(cur))
                    return AAR_E_NOENT;
                have_hold = 0;
            }
            {
                int rc = step_parent(cur);
                if (rc != AAR_OK)
                    return rc;
            }
            continue;
        }
        if (strcmp(comp, ".") == 0)
            continue;
        if (have_hold) {
            int rc = join_comp(cur, sizeof cur, hold);
            if (rc != AAR_OK)
                return rc;
            if (!is_dir_path(cur))
                return AAR_E_NOENT;
        }
        if (strlen(comp) >= sizeof hold)
            return AAR_E_INVAL;
        memcpy(hold, comp, strlen(comp) + 1u);
        have_hold = 1;
    }
    if (!have_hold)
        return AAR_E_IO;
    if (strlen(hold) + 1u > leaf_n)
        return AAR_E_INVAL;
    memcpy(leaf, hold, strlen(hold) + 1u);
    if (strlen(cur) + 1u > out_n)
        return AAR_E_INVAL;
    memcpy(out, cur, strlen(cur) + 1u);
    (void)creating;
    return AAR_OK;
}

static int compose(char *dst, size_t n, const char *dir, const char *leaf)
{
    size_t ld = strlen(dir);
    size_t ll = strlen(leaf);
    int need = !(ld > 0u && dir[ld - 1u] == '/');

    if (ld + ll + (need ? 1u : 0u) + 1u > n)
        return AAR_E_INVAL;
    memcpy(dst, dir, ld);
    if (need)
        dst[ld++] = '/';
    memcpy(dst + ld, leaf, ll + 1u);
    return AAR_OK;
}

static int resolve_open(const char *path, char *full, size_t n, int creating)
{
    char dir[4096];
    char leaf[256];
    int rc;

    if (!path || path[0] == 0 || strlen(path) > HOST_PATH_MAX)
        return AAR_E_INVAL;
    if (strchr(path, ':')) {
        if (!abs_paths)
            return AAR_E_ACCES;
        if (strlen(path) + 1u > n)
            return AAR_E_INVAL;
        memcpy(full, path, strlen(path) + 1u);
        return AAR_OK;
    }
    rc = resolve_rel(path, dir, sizeof dir, leaf, sizeof leaf, creating);
    if (rc != AAR_OK)
        return rc;
    return compose(full, n, dir, leaf);
}

static int io_err(void)
{
    if (errno == ENOENT)
        return AAR_E_NOENT;
    if (errno == EACCES || errno == EPERM)
        return AAR_E_ACCES;
    if (errno == ENOSPC)
        return AAR_E_NOSPC;
    if (errno == EEXIST)
        return AAR_E_EXIST;
    if (errno == ENOMEM)
        return AAR_E_NOMEM;
    if (errno == EBUSY)
        return AAR_E_BUSY;
    return AAR_E_IO;
}

static int host_seek(FILE *fp, int64_t off, int whence)
{
#ifdef _WIN32
    return _fseeki64(fp, (__int64)off, whence);
#else
    return fseeko(fp, (off_t)off, whence);
#endif
}

static int64_t host_tell(FILE *fp)
{
#ifdef _WIN32
    return (int64_t)_ftelli64(fp);
#else
    return (int64_t)ftello(fp);
#endif
}

/* open() and fdopen() under the names the runner uses. The Windows C runtime
 * spells them with an underscore. */
#ifdef _WIN32
#define HOST_O_RDONLY _O_RDONLY
#define HOST_O_RDWR   _O_RDWR
#define HOST_O_CREAT  _O_CREAT
#define HOST_O_TRUNC  _O_TRUNC
#define HOST_O_EXCL   _O_EXCL
#define HOST_O_BINARY _O_BINARY
#define host_open(path, oflags) _open((path), (oflags), _S_IREAD | _S_IWRITE)
#define host_fdopen _fdopen
#define host_close _close
#else
#define HOST_O_RDONLY O_RDONLY
#define HOST_O_RDWR   O_RDWR
#define HOST_O_CREAT  O_CREAT
#define HOST_O_TRUNC  O_TRUNC
#define HOST_O_EXCL   O_EXCL
#define HOST_O_BINARY 0
#define host_open(path, oflags) open((path), (oflags), 0666)
#define host_fdopen fdopen
#define host_close close
#endif

static int api_file_open(const char *path, uint32_t flags)
{
    static const uint32_t known = AAR_FILE_READ | AAR_FILE_WRITE |
        AAR_FILE_CREATE | AAR_FILE_TRUNCATE | AAR_FILE_EXCL;
    char full[4096];
    const char *mode;
    FILE *fp;
    int oflags;
    int fd;
    int slot;
    int rc;

    if ((flags & ~known) != 0u)
        return AAR_E_INVAL;
    if ((flags & (AAR_FILE_READ | AAR_FILE_WRITE)) == 0u)
        return AAR_E_INVAL;
    /* CREATE and TRUNCATE write; EXCL only qualifies CREATE. */
    if ((flags & (AAR_FILE_CREATE | AAR_FILE_TRUNCATE)) != 0u &&
        (flags & AAR_FILE_WRITE) == 0u)
        return AAR_E_INVAL;
    if ((flags & AAR_FILE_EXCL) != 0u && (flags & AAR_FILE_CREATE) == 0u)
        return AAR_E_INVAL;
    rc = resolve_open(path, full, sizeof full,
                      (flags & AAR_FILE_CREATE) != 0u);
    if (rc != AAR_OK)
        return rc;
    /* The flags go to open() as they are, so create, exclusive create and
     * truncate are one atomic operation of the host. The stdio stream is
     * read and write for any write access, as the old "r+b" and "wb+"
     * modes were. */
    if (flags & AAR_FILE_WRITE) {
        oflags = HOST_O_RDWR;
        mode = "r+b";
    } else {
        oflags = HOST_O_RDONLY;
        mode = "rb";
    }
    if (flags & AAR_FILE_CREATE)
        oflags |= HOST_O_CREAT;
    if (flags & AAR_FILE_TRUNCATE)
        oflags |= HOST_O_TRUNC;
    if (flags & AAR_FILE_EXCL)
        oflags |= HOST_O_EXCL;
    /* Reserve the slot before the open: a truncation or creation followed by
     * AAR_E_NOMEM would destroy or leave behind the caller's file. */
    for (slot = 0; slot < HOST_FD_COUNT; slot++) {
        if (!fds[slot].used)
            break;
    }
    if (slot == HOST_FD_COUNT)
        return AAR_E_NOMEM;
    fd = host_open(full, oflags | HOST_O_BINARY);
    if (fd < 0)
        return io_err();
    fp = host_fdopen(fd, mode);
    if (!fp) {
        rc = io_err();
        (void)host_close(fd);
        return rc;
    }
    fds[slot].used = 1;
    fds[slot].fp = fp;
    return HOST_FD_BASE + slot;
}

static int api_file_close(int fd)
{
    int ix = slot_of(fd);
    int rc;

    if (ix < 0 || !fds[ix].used)
        return AAR_E_BADF;
    /* fclose flushes; a failed final write (ENOSPC) must not read as
     * success. The descriptor is released either way. */
    rc = fclose(fds[ix].fp) == 0 ? AAR_OK : io_err();
    fds[ix].fp = 0;
    fds[ix].used = 0;
    return rc;
}

static int api_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n)
{
    int ix = slot_of(fd);
    size_t n;

    if (ix < 0 || !fds[ix].used)
        return AAR_E_BADF;
    if (!buf || !out_n)
        return AAR_E_INVAL;
    n = fread(buf, 1, (size_t)len, fds[ix].fp);
    *out_n = (uint32_t)n;
    if (n < len && ferror(fds[ix].fp))
        return AAR_E_IO;
    return AAR_OK;
}

static int api_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n)
{
    int ix = slot_of(fd);
    size_t n;

    if (ix < 0 || !fds[ix].used)
        return AAR_E_BADF;
    if (!buf || !out_n)
        return AAR_E_INVAL;
    n = fwrite(buf, 1, (size_t)len, fds[ix].fp);
    *out_n = (uint32_t)n;
    if (n < len) {
        if (fflush(fds[ix].fp) != 0 && errno == ENOSPC)
            return AAR_E_NOSPC;
        return AAR_E_IO;
    }
    return AAR_OK;
}

static int api_file_seek(int fd, int64_t off, int whence, int64_t *out_pos)
{
    int ix = slot_of(fd);
    int w;

    if (ix < 0 || !fds[ix].used)
        return AAR_E_BADF;
    if (whence == AAR_SEEK_SET)
        w = SEEK_SET;
    else if (whence == AAR_SEEK_CUR)
        w = SEEK_CUR;
    else if (whence == AAR_SEEK_END)
        w = SEEK_END;
    else
        return AAR_E_INVAL;
    if (host_seek(fds[ix].fp, off, w) != 0)
        return AAR_E_INVAL;
    if (out_pos)
        *out_pos = host_tell(fds[ix].fp);
    return AAR_OK;
}

static int fill_stat(const char *full, aar_stat *out)
{
    struct stat st;

    if (path_stat(full, &st) != 0)
        return io_err();
    memset(out, 0, sizeof *out);
    out->size = (uint64_t)st.st_size;
    out->mtime = st.st_mtime > 0 ? (int64_t)st.st_mtime : 0;
    out->mode = S_ISDIR(st.st_mode) ? AAR_MODE_DIR : AAR_MODE_FILE;
    return AAR_OK;
}

static int api_file_stat(const char *path, aar_stat *out)
{
    char full[4096];
    int rc;

    if (!out)
        return AAR_E_INVAL;
    rc = resolve_open(path, full, sizeof full, 0);
    if (rc != AAR_OK)
        return rc;
    return fill_stat(full, out);
}

static int api_file_list_open(const char *path)
{
    char full[4096];
    int rc;
    int id;
    int ix;

    if (!path || path[0] == 0 || strlen(path) > HOST_PATH_MAX)
        return AAR_E_INVAL;
    if (strchr(path, ':')) {
        if (!abs_paths)
            return AAR_E_ACCES;
        if (strlen(path) + 1u > sizeof full)
            return AAR_E_INVAL;
        memcpy(full, path, strlen(path) + 1u);
    } else if (strcmp(path, ".") == 0) {
        memcpy(full, root_abs, strlen(root_abs) + 1u);
    } else {
        char dir[4096];
        char leaf[256];
        rc = resolve_rel(path, dir, sizeof dir, leaf, sizeof leaf, 0);
        if (rc != AAR_OK)
            return rc;
        rc = compose(full, sizeof full, dir, leaf);
        if (rc != AAR_OK)
            return rc;
    }
    if (!is_dir_path(full))
        return AAR_E_NOENT;
    for (ix = 0; ix < HOST_FD_COUNT; ix++) {
        if (!lists[ix].used)
            break;
    }
    if (ix == HOST_FD_COUNT)
        return AAR_E_NOMEM;
    memset(&lists[ix], 0, sizeof lists[ix]);
    memcpy(lists[ix].path, full, strlen(full) + 1u);
#ifdef _WIN32
    {
        char pattern[4096];
        if (strlen(full) + 3u > sizeof pattern)
            return AAR_E_INVAL;
        snprintf(pattern, sizeof pattern, "%s/*", full);
        lists[ix].find = FindFirstFileA(pattern, &lists[ix].data);
        if (lists[ix].find == INVALID_HANDLE_VALUE)
            return AAR_E_IO;
        lists[ix].have = 1;
    }
#else
    lists[ix].dir = opendir(full);
    if (!lists[ix].dir)
        return io_err();
#endif
    lists[ix].used = 1;
    id = HOST_FD_BASE + ix;
    return id;
}

static int skip_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int api_file_list_next(int list, aar_dirent *out)
{
    int ix = slot_of(list);

    if (ix < 0 || !lists[ix].used)
        return AAR_E_BADF;
    if (!out)
        return AAR_E_INVAL;
#ifdef _WIN32
    for (;;) {
        const char *name;
        if (lists[ix].done)
            return AAR_E_NOENT;
        if (!lists[ix].have) {
            if (!FindNextFileA(lists[ix].find, &lists[ix].data)) {
                lists[ix].done = 1;
                return AAR_E_NOENT;
            }
        }
        lists[ix].have = 0;
        name = lists[ix].data.cFileName;
        if (skip_dot(name))
            continue;
        memset(out, 0, sizeof *out);
        strncpy(out->name, name, AAR_NAME_MAX);
        out->mode = (lists[ix].data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        ? AAR_MODE_DIR
                        : AAR_MODE_FILE;
        return AAR_OK;
    }
#else
    for (;;) {
        struct dirent *ent = readdir(lists[ix].dir);
        if (!ent)
            return AAR_E_NOENT;
        if (skip_dot(ent->d_name))
            continue;
        memset(out, 0, sizeof *out);
        strncpy(out->name, ent->d_name, AAR_NAME_MAX);
        out->mode = AAR_MODE_FILE;
        {
            char child[4096];
            struct stat st;

            if (compose(child, sizeof child, lists[ix].path, ent->d_name) ==
                    AAR_OK &&
                path_stat(child, &st) == 0 && S_ISDIR(st.st_mode))
                out->mode = AAR_MODE_DIR;
        }
        return AAR_OK;
    }
#endif
}

static int api_file_list_close(int list)
{
    int ix = slot_of(list);

    if (ix < 0 || !lists[ix].used)
        return AAR_E_BADF;
#ifdef _WIN32
    if (lists[ix].find != INVALID_HANDLE_VALUE)
        FindClose(lists[ix].find);
#else
    if (lists[ix].dir)
        closedir(lists[ix].dir);
#endif
    memset(&lists[ix], 0, sizeof lists[ix]);
    return AAR_OK;
}

static int api_map_file(const char *path, const void **data, uint64_t *bytes)
{
    aar_stat st;
    aar_shared block;
    uint8_t *dst;
    int fd;
    uint64_t done = 0;
    int rc;

    if (!path || !data || !bytes)
        return AAR_E_INVAL;
    *data = 0;
    *bytes = 0;
    rc = api_file_stat(path, &st);
    if (rc != AAR_OK)
        return rc;
    if (st.mode != AAR_MODE_FILE)
        return AAR_E_IO;
    if (st.size > HOST_MAP_LIMIT)
        return AAR_E_NOSPC;
    if (st.size == 0u)
        return AAR_E_NOMEM;
    rc = host_shared_alloc(st.size, 1, &block);
    if (rc != AAR_OK)
        return rc;
    dst = (uint8_t *)block.ptr;
    fd = api_file_open(path, AAR_FILE_READ);
    if (fd < 0) {
        host_shared_free_runtime(dst);
        return fd;
    }
    while (done < st.size) {
        uint32_t got = 0;
        uint32_t want = (uint32_t)(st.size - done);
        rc = api_file_read(fd, dst + done, want, &got);
        if (rc != AAR_OK || got == 0u) {
            api_file_close(fd);
            host_shared_free_runtime(dst);
            return rc != AAR_OK ? rc : AAR_E_IO;
        }
        done += got;
    }
    api_file_close(fd);
    *data = dst;
    *bytes = done;
    return AAR_OK;
}

static void api_unmap_file(const void *data)
{
    host_shared_free_runtime(data);
}

int host_files_init(const char *root, int allow_abs)
{
    struct stat st;

    abs_paths = allow_abs;
    if (!root)
        root = ".";
    if (make_abs(root, root_abs, sizeof root_abs) != 0) {
        fprintf(stderr, "aar-host: --root %s is not a directory\n", root);
        return -1;
    }
    if (path_stat(root_abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "aar-host: --root %s is not a directory\n", root);
        return -1;
    }
    memset(fds, 0, sizeof fds);
    memset(lists, 0, sizeof lists);
    return 0;
}

void host_files_shutdown(void)
{
    int i;

    for (i = 0; i < HOST_FD_COUNT; i++) {
        if (fds[i].used && fds[i].fp)
            fclose(fds[i].fp);
        fds[i].used = 0;
        fds[i].fp = 0;
        if (lists[i].used)
            api_file_list_close(HOST_FD_BASE + i);
    }
}

void host_files_bind(aar_api *api)
{
    api->file_open = api_file_open;
    api->file_close = api_file_close;
    api->file_read = api_file_read;
    api->file_write = api_file_write;
    api->file_seek = api_file_seek;
    api->file_stat = api_file_stat;
    api->file_list_open = api_file_list_open;
    api->file_list_next = api_file_list_next;
    api->file_list_close = api_file_list_close;
    api->map_file = api_map_file;
    api->unmap_file = api_unmap_file;
}
