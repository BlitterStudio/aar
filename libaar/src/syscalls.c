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
 * Not provided, and so a link error when used: _unlink, _link, _rename,
 * _times, _gettimeofday. The table has no such calls.
 *
 * aar_alloc and aar_free sit on newlib's malloc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

    /* AAR_FILE_CREATE creates or replaces. Open an existing file that is
     * neither exclusive nor truncated without it. */
    if ((flags & (O_CREAT | O_TRUNC)) != 0 && access == O_RDONLY) {
        errno = EINVAL;
        return -1;
    }
    if ((flags & O_CREAT) != 0 && (flags & O_TRUNC) != 0 &&
        (flags & O_EXCL) == 0) {
        aflags |= AAR_FILE_CREATE;
    } else if ((flags & (O_CREAT | O_TRUNC | O_EXCL)) != 0) {
        aar_stat st;
        int rc = aar_file_stat(path, &st);

        if (rc != AAR_OK && rc != AAR_E_NOENT)
            return fail(rc);
        if (rc == AAR_OK) {
            if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
                errno = EEXIST;
                return -1;
            }
            if ((flags & O_TRUNC) != 0)
                aflags |= AAR_FILE_CREATE;
        } else if ((flags & O_CREAT) != 0) {
            aflags |= AAR_FILE_CREATE;
        } else {
            errno = ENOENT;
            return -1;
        }
    }

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
    return fd;
}

int _close(int fd)
{
    int rc;

    if (is_console(fd))
        return 0;
    rc = aar_file_close(fd);
    if (rc == AAR_OK)
        fd_set_append(fd, 0);
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
    if (pos < 0 || (uint64_t)pos > (uint64_t)((off_t)(~((uint64_t)1 << (8 * sizeof(off_t) - 1))))) {
        errno = EOVERFLOW;
        return -1;
    }
    return (off_t)pos;
}

/* The table has no fstat: the size is found by seeking, then the position is
 * put back. */
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
    st->st_mode = S_IFREG | 0644;
    st->st_size = (off_t)end;
    st->st_blksize = 4096;
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
    memset(st, 0, sizeof(*st));
    st->st_mode = (as.mode == AAR_MODE_DIR ? S_IFDIR | 0755 : S_IFREG | 0644);
    st->st_size = (off_t)as.size;
    st->st_mtime = (time_t)as.mtime;
    st->st_blksize = 4096;
    return 0;
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
