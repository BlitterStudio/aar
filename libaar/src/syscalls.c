/*
 * Layer 2 of libaar: glue for a PIC newlib. Not linked into a freestanding
 * image.
 *
 * Heap. The private heap starts as the range the runtime reports through
 * heap_range. _sbrk then grows and shrinks it at its end in whole heap
 * granules through heap_grow and heap_shrink. This is not a firmware _sbrk.
 *
 * Descriptors 0, 1 and 2 are the console: writes go to console_write, which
 * writes every byte or fails, and reads return 0 at the end of input. They are
 * separate from aar_log. Descriptors 3 and above are the ids that
 * aar_file_open returns (AAR_FD_BASE is 3), so fopen, fread, fwrite, fseek
 * and fclose work over aar_file_*. _isatty reports 0 everywhere, so stdio
 * buffers fully and exit flushes it.
 *
 * Files and directories. `_unlink` is aar_file_delete and removes a file or an
 * empty directory, which is what `remove` needs. `rename` and `_rename` are
 * aar_file_rename with AAR_RENAME_REPLACE, so an existing target is replaced
 * as POSIX says; AmigaDOS has no atomic replace, so the client deletes the
 * target and then renames, and a failure in between loses it. `mkdir`,
 * `_mkdir`, `rmdir`, `ftruncate`, `truncate`, `chmod`, `utime` and `utimes`
 * are aar_dir_create, aar_file_delete, aar_file_truncate, aar_file_set_protect
 * and aar_file_set_date. `chmod` maps the owner bits of the mode onto the
 * AmigaDOS bits (see mode_to_protect) and keeps the archive, pure, script and
 * hold bits; `_stat` maps them back. `_fstat` reads the same bits through the
 * path the descriptor was opened with.
 *
 * Time. `_gettimeofday` (and so `time`) is aar_time_unix_us. `_times` (and so
 * `clock`) counts aar_time_us from the start of the program, in
 * CLOCKS_PER_SEC ticks, all of it as user time.
 *
 * Environment. `getenv`, `_getenv_r`, `setenv`, `unsetenv` and `putenv` use a
 * private table. A name is read from the host with aar_env_get the first time
 * it is asked for and then stays as it was found; setenv, unsetenv and putenv
 * change only this table, never the host's. A pointer that getenv returned
 * stays valid until that name is set or unset. newlib's `environ` is not
 * used and stays empty.
 *
 * Not provided, and so a link error when used: _link (so `link`), fchmod,
 * futimes, symlink, readlink, fsync. The table has no such calls.
 *
 * aar_alloc and aar_free sit on newlib's malloc.
 *
 * SPDX-License-Identifier: MIT
 */
#define _DEFAULT_SOURCE 1
#include "aar_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>
#include <reent.h>

/* ---- Errors ------------------------------------------------------------- */

static int errno_of(int rc)
{
    switch (rc) {
    case AAR_E_INVAL:    return EINVAL;
    case AAR_E_NOMEM:    return ENOMEM;
    case AAR_E_BUSY:     return EBUSY;
    case AAR_E_NOENT:    return ENOENT;
    case AAR_E_ACCES:    return EACCES;
    case AAR_E_NOSYS:    return ENOSYS;
    case AAR_E_TIMEDOUT: return ETIMEDOUT;
    case AAR_E_FAULT:    return EFAULT;
    case AAR_E_NOSPC:    return ENOSPC;
    case AAR_E_EXIST:    return EEXIST;
    case AAR_E_BADF:     return EBADF;
    default:             return EIO;
    }
}

static int fail(int rc)
{
    errno = errno_of(rc);
    return -1;
}

/* ---- Heap --------------------------------------------------------------- */

static uint8_t *heap_base;
static uint8_t *heap_brk;
static uint8_t *heap_end;
static uint32_t heap_step;
static int heap_ready;

static int heap_init(void)
{
    void *base = 0;
    uint64_t bytes = 0;
    aar_info info;

    if (heap_ready)
        return 1;
    aar_heap_range(&base, &bytes);
    if (!base || bytes > (uint64_t)((uintptr_t)-1 - (uintptr_t)base))
        return 0;
    heap_step = AAR_HEAP_GRANULE;
    info.size = (uint32_t)sizeof(info);
    info.heap_granule = 0;
    if (aar_get_info(&info) == AAR_OK && info.heap_granule != 0u &&
        (info.heap_granule & (info.heap_granule - 1u)) == 0u)
        heap_step = info.heap_granule;
    heap_base = (uint8_t *)base;
    heap_end = heap_base + (size_t)bytes;
    heap_brk = heap_base;
    heap_ready = 1;
    return 1;
}

/* Commit at least `need` more bytes at the heap end. */
static int heap_extend(size_t need)
{
    uint64_t mask = (uint64_t)heap_step - 1u;
    uint64_t bytes;
    void *old_break = 0;

    if ((uint64_t)need > UINT64_MAX - mask)
        return 0;
    bytes = ((uint64_t)need + mask) & ~mask;
    if (bytes > (uint64_t)((uintptr_t)-1 - (uintptr_t)heap_end))
        return 0;
    if (aar_heap_grow(bytes, &old_break) != AAR_OK ||
        (uint8_t *)old_break != heap_end)
        return 0;
    heap_end += (size_t)bytes;
    return 1;
}

/* Return whole granules above the break, keeping one granule of slack. */
static void heap_trim(void)
{
    size_t spare = (size_t)(heap_end - heap_brk);
    size_t bytes;
    void *new_break = 0;

    if (spare < 2u * (size_t)heap_step)
        return;
    bytes = (spare - heap_step) & ~((size_t)heap_step - 1u);
    if (aar_heap_shrink(bytes, &new_break) == AAR_OK)
        heap_end = (uint8_t *)new_break;
}

void *_sbrk(ptrdiff_t increment)
{
    uint8_t *previous;
    size_t step;

    if (!heap_init()) {
        errno = ENOMEM;
        return (void *)-1;
    }
    previous = heap_brk;
    if (increment >= 0) {
        size_t room = (size_t)(heap_end - heap_brk);

        step = (size_t)increment;
        if (step > room && !heap_extend(step - room)) {
            errno = ENOMEM;
            return (void *)-1;
        }
    } else {
        step = (size_t)(-(increment + 1)) + 1u;
        if (step > (size_t)(heap_brk - heap_base)) {
            errno = ENOMEM;
            return (void *)-1;
        }
    }
    heap_brk = previous + increment;
    if (increment < 0)
        heap_trim();
    return previous;
}

/* ---- Process ------------------------------------------------------------ */

void _exit(int code)
{
    aar_exit(code);
    for (;;)
        ;
}

int _isatty(int fd)
{
    (void)fd;
    return 0;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = ENOSYS;
    return -1;
}

int _getpid(void)
{
    return 1;
}

/* ---- Files and console -------------------------------------------------- */

static int is_console(int fd)
{
    return fd >= 0 && fd < AAR_FD_BASE;
}

/* Descriptors opened with O_APPEND. Every write on them seeks to the end
 * first, so another writer extending the file, or an lseek, cannot make a
 * write land before the end. File ids are small integers; an append open
 * that returns an id this map cannot hold fails with EMFILE. */
#define APPEND_FD_LIMIT 256
static uint32_t append_fds[APPEND_FD_LIMIT / 32];

static int fd_appends(int fd)
{
    return fd >= 0 && fd < APPEND_FD_LIMIT &&
           (append_fds[fd / 32] & (1u << (fd % 32))) != 0;
}

static void fd_set_append(int fd, int on)
{
    if (fd < 0 || fd >= APPEND_FD_LIMIT)
        return;
    if (on)
        append_fds[fd / 32] |= 1u << (fd % 32);
    else
        append_fds[fd / 32] &= ~(1u << (fd % 32));
}

/* The path each descriptor was opened with, for _fstat, which reads the
 * protection bits and the date through aar_file_stat because the table has no
 * stat on an id. A descriptor whose path could not be kept (no memory, or an
 * id past the map) is reported with default bits. */
static char *fd_paths[APPEND_FD_LIMIT];

static void fd_set_path(int fd, const char *path)
{
    if (fd < 0 || fd >= APPEND_FD_LIMIT)
        return;
    free(fd_paths[fd]);
    fd_paths[fd] = NULL;
    if (path) {
        size_t n = strlen(path) + 1;

        fd_paths[fd] = malloc(n);
        if (fd_paths[fd])
            memcpy(fd_paths[fd], path, n);
    }
}

int _open(const char *path, int flags, ...)
{
    int access = flags & O_ACCMODE;
    uint32_t aflags;
    int fd;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (access == O_RDONLY)
        aflags = AAR_FILE_READ;
    else if (access == O_WRONLY)
        aflags = AAR_FILE_WRITE;
    else
        aflags = AAR_FILE_READ | AAR_FILE_WRITE;

    /* O_CREAT, O_TRUNC and O_EXCL map one to one onto the flags of
     * aar_file_open, which the runtime applies as a single operation. No
     * existence check comes first: the client's open decides, so a file that
     * appears in between cannot be replaced by mistake. O_EXCL means
     * something only together with O_CREAT. */
    if ((flags & (O_CREAT | O_TRUNC)) != 0 && access == O_RDONLY) {
        errno = EINVAL;
        return -1;
    }
    if ((flags & O_CREAT) != 0) {
        aflags |= AAR_FILE_CREATE;
        if ((flags & O_EXCL) != 0)
            aflags |= AAR_FILE_EXCL;
    }
    if ((flags & O_TRUNC) != 0)
        aflags |= AAR_FILE_TRUNCATE;

    fd = aar_file_open(path, aflags);
    if (fd < 0)
        return fail(fd);
    fd_set_append(fd, 0);
    if ((flags & O_APPEND) != 0) {
        int64_t pos = 0;
        int rc;

        if (fd >= APPEND_FD_LIMIT) {
            (void)aar_file_close(fd);
            errno = EMFILE;
            return -1;
        }
        rc = aar_file_seek(fd, 0, AAR_SEEK_END, &pos);
        if (rc != AAR_OK) {
            (void)aar_file_close(fd);
            return fail(rc);
        }
        fd_set_append(fd, 1);
    }
    fd_set_path(fd, path);
    return fd;
}

int _close(int fd)
{
    int rc;

    if (is_console(fd))
        return 0;
    rc = aar_file_close(fd);
    if (rc == AAR_OK) {
        fd_set_append(fd, 0);
        fd_set_path(fd, NULL);
    }
    return rc == AAR_OK ? 0 : fail(rc);
}

ssize_t _write(int fd, const void *buf, size_t count)
{
    const uint8_t *bytes = (const uint8_t *)buf;
    size_t done = 0;

    if (fd < 0 || (fd < AAR_FD_BASE && fd != 1 && fd != 2)) {
        errno = EBADF;
        return -1;
    }
    if (count != 0 && !bytes) {
        errno = EFAULT;
        return -1;
    }
    if (count != 0 && fd_appends(fd)) {
        int64_t pos = 0;
        int rc = aar_file_seek(fd, 0, AAR_SEEK_END, &pos);

        if (rc != AAR_OK)
            return fail(rc);
    }
    while (done < count) {
        uint32_t wrote = 0;
        uint32_t chunk = count - done > UINT32_MAX ? UINT32_MAX :
                         (uint32_t)(count - done);
        int rc = is_console(fd) ?
                 aar_console_write(bytes + done, chunk, &wrote) :
                 aar_file_write(fd, bytes + done, chunk, &wrote);

        if (rc != AAR_OK || wrote > chunk) {
            errno = rc != AAR_OK ? errno_of(rc) : EIO;
            return done ? (ssize_t)done : -1;
        }
        done += wrote;
        if (wrote != chunk) {
            errno = EIO;
            return (ssize_t)done;
        }
    }
    return (ssize_t)done;
}

ssize_t _read(int fd, void *buf, size_t count)
{
    uint32_t got = 0;
    uint32_t chunk;
    int rc;

    if (fd < 0 || (fd < AAR_FD_BASE && fd != 0)) {
        errno = EBADF;
        return -1;
    }
    if (count != 0 && !buf) {
        errno = EFAULT;
        return -1;
    }
    if (count == 0)
        return 0;
    chunk = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
    rc = is_console(fd) ? aar_console_read(buf, chunk, &got) :
                          aar_file_read(fd, buf, chunk, &got);
    if (rc != AAR_OK) {
        errno = errno_of(rc);
        return -1;
    }
    if (got > chunk) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)got;
}

/* True when v is a non-negative value the target's off_t holds: 31 bits on
 * A32, where off_t is a 32-bit long, 63 bits on A64. */
static int off_fits(uint64_t v)
{
    return v <= (uint64_t)((off_t)(~((uint64_t)1 << (8 * sizeof(off_t) - 1))));
}

off_t _lseek(int fd, off_t offset, int whence)
{
    int64_t pos = 0;
    int rc;

    if (is_console(fd)) {
        errno = ESPIPE;
        return -1;
    }
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    rc = aar_file_seek(fd, (int64_t)offset, whence, &pos);
    if (rc != AAR_OK)
        return fail(rc);
    if (pos < 0 || !off_fits((uint64_t)pos)) {
        errno = EOVERFLOW;
        return -1;
    }
    return (off_t)pos;
}

/* The owner bits of a POSIX mode and the AmigaDOS protection bits (spec 7.1;
 * bits 0 to 3 mean DENY). Read, write and execute map to AAR_PROT_READ,
 * AAR_PROT_WRITE and AAR_PROT_EXECUTE; a mode without write also sets
 * AAR_PROT_DELETE, as AmigaDOS read-only protection does. Group and other bits
 * of a mode, and the set-id and sticky bits, are ignored: the table has no
 * such concept. */
#define PROT_PERM_MASK (AAR_PROT_DELETE | AAR_PROT_EXECUTE | AAR_PROT_WRITE | \
                        AAR_PROT_READ)

static uint32_t mode_to_protect(mode_t mode)
{
    uint32_t p = 0;

    if (!(mode & S_IRUSR))
        p |= AAR_PROT_READ;
    if (!(mode & S_IWUSR))
        p |= AAR_PROT_WRITE | AAR_PROT_DELETE;
    if (!(mode & S_IXUSR))
        p |= AAR_PROT_EXECUTE;
    return p;
}

/* The permission bits stat reports. The owner gets what the protection bits
 * allow; group and other get the same read and execute, and no write. */
static mode_t protect_to_perm(uint32_t protect)
{
    mode_t u = 0;

    if (!(protect & AAR_PROT_READ))
        u |= 4;
    if (!(protect & AAR_PROT_WRITE))
        u |= 2;
    if (!(protect & AAR_PROT_EXECUTE))
        u |= 1;
    return (mode_t)((u << 6) | ((u & 5u) << 3) | (u & 5u));
}

static void fill_stat(struct stat *st, const aar_stat *as)
{
    st->st_mode = (as->mode == AAR_MODE_DIR ? S_IFDIR : S_IFREG) |
                  protect_to_perm(as->protect);
    st->st_mtime = (time_t)as->mtime;
    st->st_blksize = 4096;
}

/* The table has no fstat: the size is found by seeking, then the position is
 * put back. The mode and the date come from a stat of the path the descriptor
 * was opened with, when that still names a file. */
int _fstat(int fd, struct stat *st)
{
    int64_t pos = 0;
    int64_t end = 0;
    int rc;

    memset(st, 0, sizeof(*st));
    if (is_console(fd)) {
        st->st_mode = S_IFCHR | 0600;
        return 0;
    }
    rc = aar_file_seek(fd, 0, AAR_SEEK_CUR, &pos);
    if (rc == AAR_OK)
        rc = aar_file_seek(fd, 0, AAR_SEEK_END, &end);
    if (rc == AAR_OK)
        rc = aar_file_seek(fd, pos, AAR_SEEK_SET, &pos);
    if (rc != AAR_OK)
        return fail(rc);
    if (end < 0 || !off_fits((uint64_t)end)) {
        errno = EOVERFLOW;
        return -1;
    }
    st->st_mode = S_IFREG | 0644;
    st->st_blksize = 4096;
    if (fd < APPEND_FD_LIMIT && fd_paths[fd]) {
        aar_stat as;

        if (aar_file_stat(fd_paths[fd], &as) == AAR_OK && as.mode == AAR_MODE_FILE)
            fill_stat(st, &as);
    }
    st->st_size = (off_t)end;
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    aar_stat as;
    int rc;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    rc = aar_file_stat(path, &as);
    if (rc != AAR_OK)
        return fail(rc);
    /* off_t is 32 bits on A32: a size it cannot hold is an error, not a
     * wrapped value. */
    if (!off_fits(as.size)) {
        errno = EOVERFLOW;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    fill_stat(st, &as);
    st->st_size = (off_t)as.size;
    return 0;
}

/* ---- Changing files and directories ------------------------------------- */

/* A delete that fails on a directory that still has entries reports
 * AAR_E_EXIST (spec 7.1); POSIX calls that ENOTEMPTY. */
static int fail_delete(int rc)
{
    if (rc == AAR_E_EXIST) {
        errno = ENOTEMPTY;
        return -1;
    }
    return fail(rc);
}

int _unlink(const char *path)
{
    int rc;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    rc = aar_file_delete(path);
    return rc == AAR_OK ? 0 : fail_delete(rc);
}

int rmdir(const char *path)
{
    aar_stat as;
    int rc;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    /* file_delete removes a file as well, and rmdir on a file must not. */
    rc = aar_file_stat(path, &as);
    if (rc != AAR_OK)
        return fail(rc);
    if (as.mode != AAR_MODE_DIR) {
        errno = ENOTDIR;
        return -1;
    }
    rc = aar_file_delete(path);
    return rc == AAR_OK ? 0 : fail_delete(rc);
}

/* newlib calls _rename only where it has no _link; here rename is defined
 * directly, because its link and unlink fallback cannot replace a target. */
int _rename(const char *from, const char *to)
{
    int rc;

    if (!from || !to) {
        errno = EFAULT;
        return -1;
    }
    if (strcmp(from, to) == 0) {
        /* A rename onto itself does nothing, and must not delete the target
         * that is also the source. */
        aar_stat as;

        rc = aar_file_stat(from, &as);
        return rc == AAR_OK ? 0 : fail(rc);
    }
    rc = aar_file_rename(from, to, AAR_RENAME_REPLACE);
    return rc == AAR_OK ? 0 : fail_delete(rc);
}

int rename(const char *from, const char *to)
{
    return _rename(from, to);
}

int _mkdir(const char *path, mode_t mode)
{
    int rc;

    (void)mode;
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    rc = aar_dir_create(path);
    return rc == AAR_OK ? 0 : fail(rc);
}

int mkdir(const char *path, mode_t mode)
{
    return _mkdir(path, mode);
}

int ftruncate(int fd, off_t length)
{
    int rc;

    if (is_console(fd) || length < 0) {
        errno = EINVAL;
        return -1;
    }
    rc = aar_file_truncate(fd, (uint64_t)length);
    return rc == AAR_OK ? 0 : fail(rc);
}

int truncate(const char *path, off_t length)
{
    int fd;
    int rc;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    fd = aar_file_open(path, AAR_FILE_WRITE);
    if (fd < 0)
        return fail(fd);
    rc = aar_file_truncate(fd, (uint64_t)length);
    if (aar_file_close(fd) != AAR_OK && rc == AAR_OK)
        rc = AAR_E_IO;
    return rc == AAR_OK ? 0 : fail(rc);
}

int chmod(const char *path, mode_t mode)
{
    aar_stat as;
    int rc;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    /* The permission bits are replaced; the archive, pure, script and hold
     * bits stay as they are, so the current ones are read first. */
    rc = aar_file_stat(path, &as);
    if (rc != AAR_OK)
        return fail(rc);
    rc = aar_file_set_protect(path, (as.protect & ~(uint32_t)PROT_PERM_MASK & 0xffu) |
                                    mode_to_protect(mode));
    return rc == AAR_OK ? 0 : fail(rc);
}

/* ---- Time --------------------------------------------------------------- */

/* Unix seconds now, rounded down. */
static int unix_now(int64_t *seconds)
{
    int64_t us = 0;
    int rc = aar_time_unix_us(&us);

    if (rc != AAR_OK)
        return rc;
    *seconds = us / 1000000 - (us % 1000000 < 0 ? 1 : 0);
    return AAR_OK;
}

int utime(const char *path, const struct utimbuf *times)
{
    int64_t mtime = 0;
    int rc = AAR_OK;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (times)
        mtime = (int64_t)times->modtime;
    else
        rc = unix_now(&mtime);
    if (rc == AAR_OK)
        rc = aar_file_set_date(path, mtime);
    return rc == AAR_OK ? 0 : fail(rc);
}

/* Only the modification time (tv[1]) is kept: the table has no access time. */
int utimes(const char *path, const struct timeval tv[2])
{
    int64_t mtime = 0;
    int rc = AAR_OK;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (tv)
        mtime = (int64_t)tv[1].tv_sec;
    else
        rc = unix_now(&mtime);
    if (rc == AAR_OK)
        rc = aar_file_set_date(path, mtime);
    return rc == AAR_OK ? 0 : fail(rc);
}

/* tz is not filled in: the offset the host knows is in aar_locale_info, and
 * newlib's localtime reads TZ. */
int _gettimeofday(struct timeval *tv, void *tz)
{
    int64_t us = 0;
    int rc;

    (void)tz;
    rc = aar_time_unix_us(&us);
    if (rc != AAR_OK)
        return fail(rc);
    if (tv) {
        int64_t sec = us / 1000000;
        int64_t frac = us % 1000000;

        if (frac < 0) {
            frac += 1000000;
            sec--;
        }
        tv->tv_sec = (time_t)sec;
        tv->tv_usec = (suseconds_t)frac;
    }
    return 0;
}

/* The program's start on aar_time_us. times and clock count from here. */
static uint64_t start_us;

__attribute__((constructor(101))) static void times_start(void)
{
    start_us = aar_time_us();
}

clock_t _times(struct tms *buf)
{
    uint64_t us = aar_time_us() - start_us;
    clock_t ticks = (clock_t)(us / (1000000u / CLOCKS_PER_SEC));

    if (buf) {
        buf->tms_utime = ticks;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return ticks;
}

/* ---- Environment -------------------------------------------------------- */

struct env_var {
    struct env_var *next;
    char *value;            /* NULL: the name is unset */
    char name[];
};

static struct env_var *env_vars;

static struct env_var *env_find(const char *name, size_t len)
{
    struct env_var *v;

    for (v = env_vars; v; v = v->next)
        if (strncmp(v->name, name, len) == 0 && v->name[len] == 0)
            return v;
    return NULL;
}

static struct env_var *env_add(const char *name, size_t len)
{
    struct env_var *v = malloc(sizeof(*v) + len + 1);

    if (!v)
        return NULL;
    v->value = NULL;
    memcpy(v->name, name, len);
    v->name[len] = 0;
    v->next = env_vars;
    env_vars = v;
    return v;
}

/* A name aar_env_get accepts and POSIX allows in the environment. */
static int env_name_ok(const char *name, size_t *len)
{
    size_t n = name ? strlen(name) : 0;

    if (n == 0 || n > 255 || strchr(name, '='))
        return 0;
    *len = n;
    return 1;
}

/* The value of `name` from the host, as a malloc'd string, or NULL. *known is
 * 0 when the host did not answer (policy, no such call, a stop), and then
 * nothing is remembered, so a later call can ask again. */
static char *env_fetch(const char *name, int *known)
{
    char small[256];
    char *buf = small;
    uint32_t n = 0;
    char *value = NULL;
    int rc = aar_env_get(name, small, sizeof small, &n);

    *known = 1;
    if (rc == AAR_E_NOSPC) {
        buf = malloc((size_t)n + 1);
        if (!buf) {
            *known = 0;
            return NULL;
        }
        rc = aar_env_get(name, buf, n + 1, &n);
    }
    if (rc == AAR_OK) {
        value = malloc((size_t)n + 1);
        if (value) {
            memcpy(value, buf, n);
            value[n] = 0;
        } else {
            *known = 0;
        }
    } else if (rc != AAR_E_NOENT) {
        *known = 0;
    }
    if (buf != small)
        free(buf);
    return value;
}

/* The entry for `name`, asking the host the first time. NULL when the table
 * has none and the host gave no answer. */
static struct env_var *env_lookup(const char *name, size_t len)
{
    struct env_var *v = env_find(name, len);
    int known;
    char *value;

    if (v)
        return v;
    value = env_fetch(name, &known);
    if (!known)
        return NULL;
    v = env_add(name, len);
    if (!v) {
        free(value);
        return NULL;
    }
    v->value = value;
    return v;
}

char *getenv(const char *name)
{
    size_t len;
    struct env_var *v;

    if (!env_name_ok(name, &len))
        return NULL;
    v = env_lookup(name, len);
    return v ? v->value : NULL;
}

char *_getenv_r(struct _reent *reent, const char *name)
{
    (void)reent;
    return getenv(name);
}

/* Gives `name` the value `value` (NULL unsets it). */
static int env_store(const char *name, size_t len, const char *value)
{
    struct env_var *v = env_find(name, len);
    char *copy = NULL;

    if (value) {
        size_t n = strlen(value) + 1;

        copy = malloc(n);
        if (!copy) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(copy, value, n);
    }
    if (!v)
        v = env_add(name, len);
    if (!v) {
        free(copy);
        errno = ENOMEM;
        return -1;
    }
    free(v->value);
    v->value = copy;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t len;

    if (!env_name_ok(name, &len) || !value) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite) {
        struct env_var *v = env_lookup(name, len);

        if (v && v->value)
            return 0;
    }
    return env_store(name, len, value);
}

int unsetenv(const char *name)
{
    size_t len;

    if (!env_name_ok(name, &len)) {
        errno = EINVAL;
        return -1;
    }
    return env_store(name, len, NULL);
}

/* The string is copied, not kept: a later change to it does not change the
 * variable. "NAME" without '=' unsets the name, as glibc does. */
int putenv(char *string)
{
    const char *eq = string ? strchr(string, '=') : NULL;
    size_t len;
    char name[256];

    if (!string) {
        errno = EINVAL;
        return -1;
    }
    if (!eq)
        return unsetenv(string);
    len = (size_t)(eq - string);
    if (len == 0 || len > 255) {
        errno = EINVAL;
        return -1;
    }
    memcpy(name, string, len);
    name[len] = 0;
    return setenv(name, eq + 1, 1);
}

/* ---- Private allocator (libaar layer 2) --------------------------------- */

/* align 0 asks for the default alignment. Any other value must be a power of
 * two; one the allocator already guarantees costs nothing extra. */
void *aar_alloc(uint64_t bytes, uint32_t align)
{
    size_t size;

    if (bytes > SIZE_MAX)
        return 0;
    size = (size_t)bytes;
    if (align == 0u)
        align = (uint32_t)_Alignof(max_align_t);
    if ((align & (align - 1u)) != 0u)
        return 0;
    if (align <= (uint32_t)_Alignof(max_align_t))
        return malloc(size ? size : 1u);
    if (size > SIZE_MAX - (align - 1u))
        return 0;
    size = (size + align - 1u) & ~((size_t)align - 1u);
    return aligned_alloc(align, size);
}

void aar_free(void *p)
{
    free(p);
}
