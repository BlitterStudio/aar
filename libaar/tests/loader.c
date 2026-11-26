/*
 * Test loader for libaar. Loads an AAR PIE ELF into anonymous memory with the
 * reference loader of the standard (loader/aar_load.h: every rule of spec 2.2
 * and 2.3, the segments, the bss and the RELATIVE relocations of PT_DYNAMIC),
 * builds an aar_api table over POSIX calls and enters the image. The same code
 * loads A32 and A64 images; the build decides which profile it runs. Build it
 * for Linux user mode with the loader sources (arm-linux-gnueabihf-gcc -static
 * -marm, or aarch64-linux-gnu-gcc -static, with loader/aar_load.c and
 * loader/aar_image.c) and run it under qemu-arm or qemu-aarch64. An image the
 * loader refuses ends the run with status 2 and the code and rule on stderr:
 *
 *   loader app.elf
 *
 * Environment:
 *   AAR_ROOT         directory that file paths resolve in (default .)
 *   HEAP_INIT_MIB    private heap committed at start (default 0)
 *   CORE_ONLY=1      hand the app a table that ends before the optional slots
 *   COHERENT=1       report AAR_INFOF_SHARED_COHERENT, so publish and acquire
 *                    must not reach the table
 *   NO_RELOC=1       apply no relocation (a test of the conformance corpus: an
 *                    image whose code checks its relocated pointer then fails;
 *                    the loader's plan loses its relocation tables)
 *   UNIX_US_BASE=<n> time_unix_us answers n microseconds since 1970 plus the
 *                    time elapsed since its first call (default: the host's
 *                    clock)
 *
 * env_get serves host variables whose names start with AAR_, and
 * AAR_ENV_CALLS answers with the number of other env_get calls so far. The
 * clipboard is kept in memory. The protection bits of a file are the host's
 * owner bits (read, write, execute; write also stands for delete); the
 * archive, pure, script and hold bits are kept in a small table.
 *
 * Two paths have a meaning of their own: huge.bin opens as a file of 3 GiB
 * (no data, seeks tracked), and stat-calls.magic stats as a file whose size is
 * the number of stat calls made so far.
 *
 * The exit report on stderr counts heap_grow, heap_shrink and
 * publish/acquire calls. The exit status is the app's.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "aar/app.h"
#include "../../loader/aar_load.h"

#define HEAP_LIMIT (64u << 20)
#define MAX_FILES 64

static uint8_t *heap_base;
static uint64_t heap_committed;
static unsigned grow_calls;
static unsigned shrink_calls;
static unsigned publish_calls;
static int coherent;
static int files[MAX_FILES];
static int huge[MAX_FILES];
static int64_t huge_pos[MAX_FILES];
static unsigned stat_calls;
static uint8_t file_write[MAX_FILES];
static unsigned env_calls;

static void app_exit(int code)
{
    fprintf(stderr, "[loader] exit(%d) heap_grow=%u heap_shrink=%u publish=%u\n",
            code, grow_calls, shrink_calls, publish_calls);
    fflush(NULL);
    _exit(code & 255);
}

static void app_yield(void)
{
}

static int app_should_stop(void)
{
    return 0;
}

static uint64_t app_time_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void app_sleep_us(uint32_t us)
{
    usleep(us);
}

static int app_info(aar_info *out)
{
    uint32_t size = out->size;

    memset(out, 0, size < sizeof *out ? size : sizeof *out);
    out->size = size;
    out->heap_granule = AAR_HEAP_GRANULE;
    out->flags = coherent ? AAR_INFOF_SHARED_COHERENT : 0u;
    return AAR_OK;
}

static void app_heap_range(void **base, uint64_t *bytes)
{
    *base = heap_base;
    *bytes = heap_committed;
}

static int app_heap_grow(uint64_t bytes, void **old_break)
{
    grow_calls++;
    if (bytes == 0 || bytes % AAR_HEAP_GRANULE)
        return AAR_E_INVAL;
    if (heap_committed + bytes > HEAP_LIMIT)
        return AAR_E_NOMEM;
    *old_break = heap_base + heap_committed;
    heap_committed += bytes;
    return AAR_OK;
}

static int app_heap_shrink(uint64_t bytes, void **new_break)
{
    shrink_calls++;
    if (bytes == 0 || bytes % AAR_HEAP_GRANULE || bytes > heap_committed)
        return AAR_E_INVAL;
    heap_committed -= bytes;
    memset(heap_base + heap_committed, 0, bytes);
    *new_break = heap_base + heap_committed;
    return AAR_OK;
}

static int app_heap_info(aar_heap_info *out)
{
    out->committed = heap_committed;
    out->limit = HEAP_LIMIT;
    out->pool_free = HEAP_LIMIT - heap_committed;
    return AAR_OK;
}

static int app_publish(const void *p, uint64_t bytes)
{
    (void)p;
    (void)bytes;
    publish_calls++;
    return AAR_OK;
}

static void app_log(const char *msg, uint32_t len)
{
    fprintf(stderr, "[log] %.*s", (int)len, msg);
}

static int aar_error(void)
{
    switch (errno) {
    case ENOENT:    return AAR_E_NOENT;
    case EEXIST:
    case ENOTEMPTY: return AAR_E_EXIST;
    case EACCES:
    case EPERM:
    case EROFS:     return AAR_E_ACCES;
    case EBUSY:     return AAR_E_BUSY;
    case EINVAL:
    case EXDEV:
    case EISDIR:
    case ENOTDIR:   return AAR_E_INVAL;
    case ENOSPC:    return AAR_E_NOSPC;
    default:        return AAR_E_IO;
    }
}

static void host_path(char *out, size_t size, const char *path)
{
    const char *root = getenv("AAR_ROOT");

    snprintf(out, size, "%s/%s", root ? root : ".", path);
}

static int host_fd(int id)
{
    if (id < AAR_FD_BASE || id >= AAR_FD_BASE + MAX_FILES)
        return -1;
    return files[id - AAR_FD_BASE];
}

/* Paths with a meaning of their own, for runtime_test. MAGIC_HUGE opens as a
 * file whose size is 3 GiB (reads return nothing, seeks are tracked), so the
 * 32-bit off_t of A32 cannot hold it. MAGIC_STAT_CALLS stats as a file whose
 * size is the number of ordinary stat calls so far. */
#define MAGIC_HUGE       "huge.bin"
#define MAGIC_STAT_CALLS "stat-calls.magic"
#define HUGE_SIZE        (3ull << 30)

static int app_file_open(const char *path, uint32_t flags)
{
    char full[300];
    int mode = O_RDONLY;
    int fd;
    int i;
    int is_huge;

    if ((flags & AAR_FILE_READ) && (flags & AAR_FILE_WRITE))
        mode = O_RDWR;
    else if (flags & AAR_FILE_WRITE)
        mode = O_WRONLY;
    /* The three flags map to open(2) one to one, so the loader's open is as
     * atomic as the host's. */
    if (flags & AAR_FILE_CREATE)
        mode |= O_CREAT;
    if (flags & AAR_FILE_TRUNCATE)
        mode |= O_TRUNC;
    if (flags & AAR_FILE_EXCL)
        mode |= O_EXCL;
    is_huge = strcmp(path, MAGIC_HUGE) == 0;
    if (is_huge)
        strcpy(full, "/dev/null");
    else
        host_path(full, sizeof full, path);
    fd = open(full, mode, 0644);
    if (fd < 0)
        return aar_error();
    for (i = 0; i < MAX_FILES; i++) {
        if (!files[i]) {
            files[i] = fd;
            file_write[i] = (flags & AAR_FILE_WRITE) != 0;
            huge[i] = is_huge;
            huge_pos[i] = 0;
            return i + AAR_FD_BASE;
        }
    }
    close(fd);
    return AAR_E_NOMEM;
}

static int app_file_close(int id)
{
    int fd = host_fd(id);

    if (fd <= 0)
        return AAR_E_BADF;
    close(fd);
    files[id - AAR_FD_BASE] = 0;
    huge[id - AAR_FD_BASE] = 0;
    return AAR_OK;
}

static int app_file_read(int id, void *buf, uint32_t len, uint32_t *out_n)
{
    int fd = host_fd(id);
    ssize_t n;

    if (fd <= 0)
        return AAR_E_BADF;
    n = read(fd, buf, len);
    if (n < 0)
        return AAR_E_IO;
    *out_n = (uint32_t)n;
    return AAR_OK;
}

static int app_file_write(int id, const void *buf, uint32_t len,
                          uint32_t *out_n)
{
    int fd = host_fd(id);
    ssize_t n;

    if (fd <= 0)
        return AAR_E_BADF;
    n = write(fd, buf, len);
    if (n < 0)
        return AAR_E_IO;
    *out_n = (uint32_t)n;
    return AAR_OK;
}

static int app_file_seek(int id, int64_t off, int whence, int64_t *out_pos)
{
    int fd = host_fd(id);
    off_t pos;

    if (fd <= 0)
        return AAR_E_BADF;
    if (huge[id - AAR_FD_BASE]) {
        int64_t *at = &huge_pos[id - AAR_FD_BASE];
        int64_t base = whence == AAR_SEEK_SET ? 0 :
                       whence == AAR_SEEK_CUR ? *at : (int64_t)HUGE_SIZE;

        if (base + off < 0)
            return AAR_E_INVAL;
        *at = base + off;
        *out_pos = *at;
        return AAR_OK;
    }
    pos = lseek(fd, (off_t)off, whence);
    if (pos < 0)
        return AAR_E_IO;
    *out_pos = pos;
    return AAR_OK;
}

/* Protection bits that the host's mode cannot hold (delete, archive, pure,
 * script, hold), kept per path. */
#define EXTRA_MAX 16
static struct { char path[300]; uint32_t bits; } extras[EXTRA_MAX];

static uint32_t extra_get(const char *path)
{
    int i;

    for (i = 0; i < EXTRA_MAX; i++)
        if (extras[i].bits && strcmp(extras[i].path, path) == 0)
            return extras[i].bits;
    return 0;
}

static void extra_set(const char *path, uint32_t bits)
{
    int i, free_slot = -1;

    for (i = 0; i < EXTRA_MAX; i++) {
        if (extras[i].bits && strcmp(extras[i].path, path) == 0) {
            extras[i].bits = bits;
            return;
        }
        if (!extras[i].bits && free_slot < 0)
            free_slot = i;
    }
    if (bits && free_slot >= 0) {
        snprintf(extras[free_slot].path, sizeof extras[free_slot].path, "%s", path);
        extras[free_slot].bits = bits;
    }
}

static uint32_t stat_protect(const char *path, const struct stat *st)
{
    uint32_t p = extra_get(path);

    if (!(st->st_mode & S_IRUSR))
        p |= AAR_PROT_READ;
    if (!(st->st_mode & S_IWUSR))
        p |= AAR_PROT_WRITE | AAR_PROT_DELETE;
    if (!(st->st_mode & S_IXUSR))
        p |= AAR_PROT_EXECUTE;
    return p;
}

static int app_file_stat(const char *path, aar_stat *out)
{
    char full[300];
    struct stat st;

    memset(out, 0, sizeof *out);
    if (strcmp(path, MAGIC_STAT_CALLS) == 0) {
        /* Reports how many real stat calls the app made so far, so a test
         * can see that an operation did not issue one. */
        out->size = stat_calls;
        out->mode = AAR_MODE_FILE;
        return AAR_OK;
    }
    stat_calls++;
    if (strcmp(path, MAGIC_HUGE) == 0) {
        out->size = HUGE_SIZE;
        out->mode = AAR_MODE_FILE;
        return AAR_OK;
    }
    host_path(full, sizeof full, path);
    if (stat(full, &st) != 0)
        return aar_error();
    out->size = (uint64_t)st.st_size;
    out->mtime = st.st_mtime;
    out->mode = S_ISDIR(st.st_mode) ? AAR_MODE_DIR : AAR_MODE_FILE;
    out->protect = stat_protect(path, &st);
    return AAR_OK;
}

static int bad_path(const char *path)
{
    return path[0] == 0 || strlen(path) > 255;
}

static int app_file_delete(const char *path)
{
    char full[300];
    struct stat st;
    int rc;

    if (bad_path(path))
        return AAR_E_INVAL;
    host_path(full, sizeof full, path);
    if (stat(full, &st) != 0)
        return aar_error();
    if (stat_protect(path, &st) & AAR_PROT_DELETE)
        return AAR_E_ACCES;
    rc = S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
    if (rc != 0)
        return aar_error();
    extra_set(path, 0);
    return AAR_OK;
}

static int app_file_rename(const char *from, const char *to, uint32_t flags)
{
    char a[300], b[300];
    struct stat st;

    if (bad_path(from) || bad_path(to) || (flags & ~(uint32_t)AAR_RENAME_REPLACE))
        return AAR_E_INVAL;
    host_path(a, sizeof a, from);
    host_path(b, sizeof b, to);
    if (stat(a, &st) != 0)
        return aar_error();
    if (!(flags & AAR_RENAME_REPLACE) && stat(b, &st) == 0)
        return AAR_E_EXIST;
    if (rename(a, b) != 0)
        return aar_error();
    extra_set(to, extra_get(from));
    extra_set(from, 0);
    return AAR_OK;
}

static int app_dir_create(const char *path)
{
    char full[300];

    if (bad_path(path))
        return AAR_E_INVAL;
    host_path(full, sizeof full, path);
    if (mkdir(full, 0755) != 0)
        return aar_error();
    return AAR_OK;
}

static int app_file_set_date(const char *path, int64_t mtime)
{
    char full[300];
    struct timespec ts[2];

    if (bad_path(path))
        return AAR_E_INVAL;
    host_path(full, sizeof full, path);
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = (time_t)mtime;
    ts[1].tv_nsec = 0;
    if (utimensat(AT_FDCWD, full, ts, 0) != 0)
        return aar_error();
    return AAR_OK;
}

static int app_file_set_protect(const char *path, uint32_t protect)
{
    char full[300];
    struct stat st;
    mode_t u = 0;

    if (bad_path(path) || (protect & ~0xffu))
        return AAR_E_INVAL;
    host_path(full, sizeof full, path);
    if (stat(full, &st) != 0)
        return aar_error();
    if (!(protect & AAR_PROT_READ))
        u |= 4;
    if (!(protect & AAR_PROT_WRITE))
        u |= 2;
    if (!(protect & AAR_PROT_EXECUTE))
        u |= 1;
    if (chmod(full, (u << 6) | ((u & 5u) << 3) | (u & 5u)) != 0)
        return aar_error();
    extra_set(path, protect & (0xf0u | AAR_PROT_DELETE));
    return AAR_OK;
}

static int app_file_truncate(int id, uint64_t size)
{
    int fd = host_fd(id);

    if (fd <= 0)
        return AAR_E_BADF;
    if (!file_write[id - AAR_FD_BASE])
        return AAR_E_BADF;
    if (huge[id - AAR_FD_BASE])
        return AAR_OK;
    if (ftruncate(fd, (off_t)size) != 0)
        return aar_error();
    return AAR_OK;
}

static int app_volume_info(const char *path, aar_volume_info *out)
{
    char full[300];
    struct statvfs sv;
    aar_volume_info v;
    uint32_t size = out->size;

    if (bad_path(path))
        return AAR_E_INVAL;
    host_path(full, sizeof full, path);
    if (statvfs(full, &sv) != 0)
        return aar_error();
    memset(&v, 0, sizeof v);
    v.size = size;
    v.block_bytes = (uint32_t)sv.f_frsize;
    v.total_bytes = (uint64_t)sv.f_blocks * sv.f_frsize;
    v.free_bytes = (uint64_t)sv.f_bavail * sv.f_frsize;
    v.flags = (sv.f_flag & ST_RDONLY) ? AAR_VOLF_READONLY : 0u;
    memcpy(out, &v, size < sizeof v ? size : sizeof v);
    if (size > sizeof v)
        memset((uint8_t *)out + sizeof v, 0, size - sizeof v);
    return AAR_OK;
}

static int app_time_unix_us(int64_t *out_us)
{
    static int64_t base_us, first_us;
    const char *fixed = getenv("UNIX_US_BASE");

    if (fixed) {
        int64_t now = (int64_t)app_time_us();

        if (!first_us) {
            first_us = now;
            base_us = strtoll(fixed, NULL, 0);
        }
        *out_us = base_us + (now - first_us);
    } else {
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);
        *out_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    }
    return AAR_OK;
}

static int app_locale_info(aar_locale_info *out)
{
    aar_locale_info l;
    uint32_t size = out->size;

    memset(&l, 0, sizeof l);
    l.size = size;
    l.utc_offset_min = 60;
    l.flags = AAR_LOCALEF_OFFSET_KNOWN;
    strcpy(l.language, "english");
    strcpy(l.country, "GB");
    strcpy(l.decimal_point, ".");
    strcpy(l.group_separator, ",");
    memcpy(out, &l, size < sizeof l ? size : sizeof l);
    if (size > sizeof l)
        memset((uint8_t *)out + sizeof l, 0, size - sizeof l);
    return AAR_OK;
}

static int app_env_get(const char *name, char *buf, uint32_t bytes, uint32_t *out_n)
{
    size_t len = strlen(name);
    const char *value;
    char count[16];

    if (len == 0 || len > 255)
        return AAR_E_INVAL;
    if (strcmp(name, "AAR_ENV_CALLS") == 0) {
        snprintf(count, sizeof count, "%u", env_calls);
        value = count;
    } else {
        env_calls++;
        value = strncmp(name, "AAR_", 4) == 0 ? getenv(name) : NULL;
    }
    if (!value)
        return AAR_E_NOENT;
    *out_n = (uint32_t)strlen(value);
    if (bytes <= *out_n)
        return AAR_E_NOSPC;
    memcpy(buf, value, *out_n + 1u);
    return AAR_OK;
}

/* The clipboard: a few units in memory, each holding one clip of one type. */
#define CLIP_SLOTS 4
static struct { int used; uint32_t unit, type; uint8_t *data; uint32_t size; } clips[CLIP_SLOTS];

static int clip_args(uint32_t unit, uint32_t type)
{
    return unit < AAR_CLIP_UNITS && (type == AAR_CLIP_TEXT || type == AAR_CLIP_IFF);
}

static int app_clip_read(uint32_t unit, uint32_t type, void *buf, uint32_t bytes,
                         uint32_t *out_n)
{
    int i;

    if (!clip_args(unit, type))
        return AAR_E_INVAL;
    for (i = 0; i < CLIP_SLOTS; i++) {
        if (clips[i].used && clips[i].unit == unit) {
            if (clips[i].type != type)
                return AAR_E_NOENT;
            *out_n = clips[i].size;
            if (clips[i].size == 0)
                return AAR_E_NOENT;
            memcpy(buf, clips[i].data, bytes < clips[i].size ? bytes : clips[i].size);
            return bytes < clips[i].size ? AAR_E_NOSPC : AAR_OK;
        }
    }
    return AAR_E_NOENT;
}

static int app_clip_write(uint32_t unit, uint32_t type, const void *data, uint32_t bytes)
{
    int i, slot = -1;
    uint8_t *copy;

    if (!clip_args(unit, type))
        return AAR_E_INVAL;
    for (i = 0; i < CLIP_SLOTS; i++) {
        if (clips[i].used && clips[i].unit == unit) {
            slot = i;
            break;
        }
        if (!clips[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return AAR_E_NOMEM;
    copy = malloc(bytes ? bytes : 1u);
    if (!copy)
        return AAR_E_NOMEM;
    memcpy(copy, data, bytes);
    free(clips[slot].data);
    clips[slot].used = 1;
    clips[slot].unit = unit;
    clips[slot].type = type;
    clips[slot].data = copy;
    clips[slot].size = bytes;
    return AAR_OK;
}

static int app_console_write(const void *buf, uint32_t len, uint32_t *out_n)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    *out_n = len;
    return AAR_OK;
}

static int app_console_read(void *buf, uint32_t len, uint32_t *out_n)
{
    ssize_t n = read(0, buf, len);

    *out_n = n < 0 ? 0u : (uint32_t)n;
    return n < 0 ? AAR_E_IO : AAR_OK;
}

/* The loader implements the API 0.6 core calls later. Until then it answers
 * each with AAR_E_NOSYS, which a client that does not serve an op may do. The
 * display group is not in this table, so its minor 6 slots stay NULL. */
static int app_nosys_path(const char *path)
{
    (void)path;
    return AAR_E_NOSYS;
}

static int app_file_rename(const char *from, const char *to, uint32_t flags)
{
    (void)from;
    (void)to;
    (void)flags;
    return AAR_E_NOSYS;
}

static int app_file_set_date(const char *path, int64_t mtime)
{
    (void)path;
    (void)mtime;
    return AAR_E_NOSYS;
}

static int app_file_set_protect(const char *path, uint32_t protect)
{
    (void)path;
    (void)protect;
    return AAR_E_NOSYS;
}

static int app_file_truncate(int fd, uint64_t size)
{
    (void)fd;
    (void)size;
    return AAR_E_NOSYS;
}

static int app_volume_info(const char *path, aar_volume_info *out)
{
    (void)path;
    (void)out;
    return AAR_E_NOSYS;
}

static int app_time_unix_us(int64_t *out_us)
{
    (void)out_us;
    return AAR_E_NOSYS;
}

static int app_locale_info(aar_locale_info *out)
{
    (void)out;
    return AAR_E_NOSYS;
}

static int app_env_get(const char *name, char *buf, uint32_t bytes,
                       uint32_t *out_n)
{
    (void)name;
    (void)buf;
    (void)bytes;
    (void)out_n;
    return AAR_E_NOSYS;
}

static int app_clip_read(uint32_t unit, uint32_t type, void *buf,
                         uint32_t bytes, uint32_t *out_n)
{
    (void)unit;
    (void)type;
    (void)buf;
    (void)bytes;
    (void)out_n;
    return AAR_E_NOSYS;
}

static int app_clip_write(uint32_t unit, uint32_t type, const void *data,
                          uint32_t bytes)
{
    (void)unit;
    (void)type;
    (void)data;
    (void)bytes;
    return AAR_E_NOSYS;
}

static void build_table(aar_api *api)
{
    memset(api, 0, sizeof *api);
    api->size = getenv("CORE_ONLY") ? (uint32_t)offsetof(aar_api, ext)
                                    : (uint32_t)sizeof *api;
    api->version = AAR_API_VERSION;
    api->exit = app_exit;
    api->yield = app_yield;
    api->should_stop = app_should_stop;
    api->time_us = app_time_us;
    api->sleep_us = app_sleep_us;
    api->info = app_info;
    api->heap_range = app_heap_range;
    api->heap_grow = app_heap_grow;
    api->heap_shrink = app_heap_shrink;
    api->heap_info = app_heap_info;
    api->publish = app_publish;
    api->acquire = app_publish;
    api->log = app_log;
    api->file_delete = app_nosys_path;
    api->file_rename = app_file_rename;
    api->dir_create = app_nosys_path;
    api->file_set_date = app_file_set_date;
    api->file_set_protect = app_file_set_protect;
    api->file_truncate = app_file_truncate;
    api->volume_info = app_volume_info;
    api->time_unix_us = app_time_unix_us;
    api->locale_info = app_locale_info;
    api->env_get = app_env_get;
    api->clip_read = app_clip_read;
    api->clip_write = app_clip_write;
    api->file_open = app_file_open;
    api->file_close = app_file_close;
    api->file_read = app_file_read;
    api->file_write = app_file_write;
    api->file_seek = app_file_seek;
    api->file_stat = app_file_stat;
    api->file_delete = app_file_delete;
    api->file_rename = app_file_rename;
    api->dir_create = app_dir_create;
    api->file_set_date = app_file_set_date;
    api->file_set_protect = app_file_set_protect;
    api->file_truncate = app_file_truncate;
    api->volume_info = app_volume_info;
    api->time_unix_us = app_time_unix_us;
    api->locale_info = app_locale_info;
    api->env_get = app_env_get;
    api->clip_read = app_clip_read;
    api->clip_write = app_clip_write;
    api->console_write = app_console_write;
    api->console_read = app_console_read;
}

static uint8_t *read_file(const char *path, long *size)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *data;

    if (!fp) {
        perror(path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    *size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = malloc((size_t)*size);
    if (!data || fread(data, 1, (size_t)*size, fp) != (size_t)*size) {
        fprintf(stderr, "%s: read failed\n", path);
        free(data);
        data = NULL;
    }
    fclose(fp);
    return data;
}

/* ---- Loading, through the reference loader (loader/aar_load.h) --------------------
 *
 * The loader reads the whole file from memory. aar_load_plan applies every rule of
 * spec 2.2 and 2.3 for the profile this build runs (a platform that ran both
 * would pass both bits); a refusal is reported with its code and rule and ends the
 * run with status 2. The memory is anonymous and readable, writable and
 * executable (the test needs no more), the app runs where it is written, so
 * write_base and app_base are the same address. aar_load_place copies the
 * segments, zeroes the bss and applies the relocations; NO_RELOC=1 drops the
 * relocation tables from the plan first.
 */
#if defined(__aarch64__)
#define LOADER_PROFILE AAR_PROFILE_A64
#else
#define LOADER_PROFILE AAR_PROFILE_A32
#endif

#define PAGE_BYTES 4096u

struct file_source {
    const uint8_t *data;
    uint64_t size;
};

static int file_read(void *user, uint64_t offset, void *dst, uint32_t count)
{
    const struct file_source *f = (const struct file_source *)user;

    if (offset > f->size || count > f->size - offset)
        return AAR_E_IO;
    memcpy(dst, f->data + offset, count);
    return AAR_OK;
}

/* Plans and places the image. Returns the base the app runs at (the memory
 * mapped, `*mapped` bytes of the span rounded to pages, then one spare page for
 * the A64 entry stub of ENTRY_CONTRACT), or NULL. *entry is the address of the
 * entry point. */
static uint8_t *load_image(const uint8_t *img, size_t size, const char *path,
                           size_t *mapped, int *relocs, uint64_t *entry)
{
    struct file_source src;
    struct aar_load_plan plan;
    const char *why = NULL;
    uint64_t align, top;
    uint8_t *raw, *base;
    uint32_t i;
    int rc;

    src.data = img;
    src.size = size;
    rc = aar_load_plan(file_read, &src, size, LOADER_PROFILE, &plan, &why);
    if (rc != AAR_OK) {
        fprintf(stderr, "%s: refused (code %d): %s\n", path, rc, why ? why : "?");
        return NULL;
    }
    /* NO_RELOC=1 applies no relocation: a test of the tests, to show that an
     * image whose code checks its relocated pointer exits with another code. */
    if (getenv("NO_RELOC"))
        plan.nrelocs = 0;
    /* The base is a multiple of the image's alignment (at least a page). */
    align = plan.align > PAGE_BYTES ? plan.align : PAGE_BYTES;
    if (plan.span > (1ull << 30) || align > (1u << 24)) {
        fprintf(stderr, "%s: the image needs too much memory (span %llu, alignment %llu)\n",
                path, (unsigned long long)plan.span, (unsigned long long)align);
        return NULL;
    }
    top = (plan.span + PAGE_BYTES - 1u) & ~(uint64_t)(PAGE_BYTES - 1u);
    raw = mmap(NULL, (size_t)(top + PAGE_BYTES + align), PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    base = (uint8_t *)(((uintptr_t)raw + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u));
    rc = aar_load_place(&plan, file_read, &src, base, (uint64_t)(uintptr_t)base, top,
                        entry, &why);
    if (rc != AAR_OK) {
        fprintf(stderr, "%s: could not be placed (code %d): %s\n", path, rc, why ? why : "?");
        return NULL;
    }
    *relocs = 0;
    for (i = 0; i < plan.nrelocs; i++)
        *relocs += (int)plan.reloc[i].count;
    __builtin___clear_cache((char *)base, (char *)base + top);
    *mapped = (size_t)top;
    return base;
}

/* ---- ENTRY_CONTRACT=1: enter the image as spec 3.2 says ----------------------
 *
 * The plain entry above is a C call: link register, scratch registers and
 * floating-point state are whatever the loader had. With ENTRY_CONTRACT set the
 * image is entered with r0/x0 the table, r1/w1 argc, r2/x2 argv, a fresh 1 MiB
 * stack (8-byte aligned on A32, 16 on A64), every other general and vector
 * register 0, lr 0 (and x29 0), FPSCR 0 (FPCR and FPSR 0). argv is the file name
 * of the image (last path component), "alpha" and "beta gamma", the first
 * words of the runner's arguments (conformance/payloads/conf.h), then the words
 * of ENTRY_ARGS. The conformance payloads conf_entry, conf_table and conf_mem
 * judge this state and the table below.
 *
 * A32 jumps through the stack (ldr pc), A64 through a copy of the stub in a page
 * next to the image whose last instruction is a branch patched to the entry, so
 * that no register carries the entry address.
 */
#define APP_STACK_BYTES (1u << 20)

#if defined(__aarch64__)
__asm__(
    ".text\n"
    ".balign 4\n"
    "loader_enter_start:\n"
    "    hint #34\n"
    "    mov  sp, x3\n"
    "    mov  w1, w1\n"
    "    .irp r, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30\n"
    "    mov  x\\r, xzr\n"
    "    .endr\n"
    "    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
    "    movi v\\r\\().2d, #0\n"
    "    .endr\n"
    "    msr  fpcr, xzr\n"
    "    msr  fpsr, xzr\n"
    "    b    .\n"
    "loader_enter_end:\n"
    ".pushsection .rodata\n"
    ".balign 4\n"
    "loader_enter_bytes:\n"
    ".word loader_enter_end - loader_enter_start\n"
    ".popsection\n");
extern const uint32_t loader_enter_start[], loader_enter_bytes;

static void enter_contract(uintptr_t entry, uint8_t *near_img, size_t near_bytes,
                           const aar_api *api, int argc, char **argv,
                           uint8_t *stack_top)
{
    size_t bytes = loader_enter_bytes;
    uint32_t *stub = (uint32_t *)(void *)(near_img + near_bytes);
    uint32_t *last;
    int64_t delta;

    memcpy(stub, loader_enter_start, bytes);
    last = stub + bytes / 4 - 1;
    delta = ((int64_t)entry - (int64_t)(uintptr_t)last) / 4;
    if (delta < -(1 << 25) || delta >= (1 << 25)) {
        fprintf(stderr, "[loader] the entry is out of branch range of the stub\n");
        _exit(2);
    }
    *last = 0x14000000u | ((uint32_t)delta & 0x03ffffffu);
    __builtin___clear_cache((char *)stub, (char *)stub + bytes);
    ((void (*)(const aar_api *, int, char **, uint8_t *))(void *)stub)(
        api, argc, argv, stack_top);
    _exit(99);
}
#else
__asm__(
    ".text\n"
    ".arm\n"
    ".balign 4\n"
    ".fpu neon\n"
    ".global loader_enter_a32\n"
    "loader_enter_a32:\n"
    "    ldr  r12, [sp]\n"
    "    mov  sp, r3\n"
    "    sub  sp, sp, #8\n"
    "    str  r12, [sp]\n"
    "    .irp r, 3,4,5,6,7,8,9,10,11,12\n"
    "    mov  r\\r, #0\n"
    "    .endr\n"
    "    mov  lr, #0\n"
    "    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
    "    vmov.i32 q\\r, #0\n"
    "    .endr\n"
    "    vmsr fpscr, r3\n"
    "    ldr  pc, [sp], #8\n");
extern void loader_enter_a32(const aar_api *api, int argc, char **argv,
                             uint8_t *stack_top, uintptr_t entry);

static void enter_contract(uintptr_t entry, uint8_t *near_img, size_t near_bytes,
                           const aar_api *api, int argc, char **argv,
                           uint8_t *stack_top)
{
    (void)near_img;
    (void)near_bytes;
    loader_enter_a32(api, argc, argv, stack_top, entry);
    _exit(99);
}
#endif

/* ---- The table a contract run sees -------------------------------------------------
 *
 * Enough of a platform for the conformance payloads (C-TABLE, C-MEM) to judge
 * it: every core slot is non-NULL (a slot the loader does not implement is
 * filled with a stub that refuses), aar_info is complete, and
 *   HEAP_CAP_MIB=<n>   caps the private heap at n MiB (default and most: 64),
 *                      as AAR_TAG_PRIVATE_MIB would
 *   CPU_FEATURES=<n>   aar_info.cpu_features (default 0), the bits of spec 8.7
 * The private heap raises AAR_EV_LOW_MEMORY once when less than a tenth of the
 * cap is free and again after more than half is free. No group is declared.
 */
static uint64_t contract_cap;
static int low_armed = 1;
static int low_pending;

static void low_check(void)
{
    uint64_t free_bytes = contract_cap - heap_committed;

    if (low_armed && free_bytes < contract_cap / 10u) {
        low_pending = 1;
        low_armed = 0;
    } else if (!low_armed && free_bytes >= contract_cap / 2u) {
        low_armed = 1;
    }
}

static int contract_heap_grow(uint64_t bytes, void **old_break)
{
    int rc;

    if (bytes == 0 || bytes % AAR_HEAP_GRANULE)
        return AAR_E_INVAL;
    if (heap_committed + bytes > contract_cap)
        return AAR_E_NOMEM;
    rc = app_heap_grow(bytes, old_break);
    low_check();
    return rc;
}

static int contract_heap_shrink(uint64_t bytes, void **new_break)
{
    int rc = app_heap_shrink(bytes, new_break);

    low_check();
    return rc;
}

static int contract_heap_info(aar_heap_info *out)
{
    int rc = app_heap_info(out);

    out->limit = contract_cap;
    out->pool_free = contract_cap - heap_committed;
    return rc;
}

static int contract_event_poll(aar_event *out, uint32_t max, uint32_t *out_n)
{
    *out_n = 0;
    if (max && low_pending) {
        memset(out, 0, sizeof *out);
        out->type = AAR_EV_LOW_MEMORY;
        low_pending = 0;
        *out_n = 1;
    }
    return AAR_OK;
}

static int contract_info(aar_info *out)
{
    aar_info full;
    uint32_t size = out->size;
    const char *features = getenv("CPU_FEATURES");

    memset(&full, 0, sizeof full);
    full.size = size;
#if defined(__aarch64__)
    full.profile = AAR_PROFILE_A64;
#else
    full.profile = AAR_PROFILE_A32;
#endif
    full.flags = coherent ? AAR_INFOF_SHARED_COHERENT : 0u;
    full.heap_granule = AAR_HEAP_GRANULE;
    full.shared_granule = 4096u;
    full.msg_capacity = 63u;
    full.sleep_granule_us = 1000u;
    full.stall_limit_ms = AAR_STALL_LIMIT_MIN_MS;
    full.stop_grace_ms = 500u;
    full.cpu_features = features ? (uint32_t)strtoul(features, NULL, 0) : 0u;
    full.shared_limit = (uint64_t)AAR_SHARED_DEFAULT_MIB << 20;
    full.max_files = AAR_MIN_FILES;
    memcpy(out, &full, size < sizeof full ? size : sizeof full);
    if (size > sizeof full)
        memset((uint8_t *)out + sizeof full, 0, size - sizeof full);
    return AAR_OK;
}

static int stub_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                             aar_shared *out)
{
    (void)bytes;
    (void)align;
    (void)flags;
    (void)out;
    return AAR_E_NOMEM;
}

static int stub_shared_free(void *p)
{
    (void)p;
    return AAR_E_INVAL;
}

/* The loader has no client: a message is printed (the keyword, then the four
 * little-endian words from byte 16) and taken. */
static int stub_msg_send(const void *msg)
{
    const uint8_t *b = (const uint8_t *)msg;
    uint32_t w[4];
    int i;

    for (i = 0; i < 4; i++)
        w[i] = (uint32_t)b[16 + 4 * i] | ((uint32_t)b[17 + 4 * i] << 8) |
               ((uint32_t)b[18 + 4 * i] << 16) | ((uint32_t)b[19 + 4 * i] << 24);
    fprintf(stderr, "[loader] message %.16s %u %u %u %u\n", (const char *)msg, w[0], w[1],
            w[2], w[3]);
    return AAR_OK;
}

static int stub_msg_poll(void *msg)
{
    (void)msg;
    return 0;
}

static int stub_msg_wait(uint32_t timeout_us)
{
    (void)timeout_us;
    return AAR_E_TIMEDOUT;
}

static int stub_list_open(const char *path)
{
    (void)path;
    return AAR_E_NOENT;
}

static int stub_list_next(int list, aar_dirent *out)
{
    (void)list;
    (void)out;
    return AAR_E_BADF;
}

static int stub_list_close(int list)
{
    (void)list;
    return AAR_E_BADF;
}

static void contract_table(aar_api *api)
{
    const char *cap = getenv("HEAP_CAP_MIB");

    contract_cap = (uint64_t)HEAP_LIMIT;
    if (cap && (uint64_t)strtoul(cap, NULL, 0) * AAR_HEAP_GRANULE < contract_cap)
        contract_cap = (uint64_t)strtoul(cap, NULL, 0) * AAR_HEAP_GRANULE;
    api->heap_grow = contract_heap_grow;
    api->heap_shrink = contract_heap_shrink;
    api->heap_info = contract_heap_info;
    api->event_poll = contract_event_poll;
    api->info = contract_info;
    if (!api->shared_alloc)
        api->shared_alloc = stub_shared_alloc;
    if (!api->shared_free)
        api->shared_free = stub_shared_free;
    if (!api->msg_send)
        api->msg_send = stub_msg_send;
    if (!api->msg_poll)
        api->msg_poll = stub_msg_poll;
    if (!api->msg_wait)
        api->msg_wait = stub_msg_wait;
    if (!api->file_list_open)
        api->file_list_open = stub_list_open;
    if (!api->file_list_next)
        api->file_list_next = stub_list_next;
    if (!api->file_list_close)
        api->file_list_close = stub_list_close;
}

/* argv: the image path's last component, "alpha", "beta gamma", then the
 * space-separated words of ENTRY_ARGS (for example conf:variant=caps). */
#define CONTRACT_ARGV_MAX 32

static void run_contract(uintptr_t entry, uint8_t *base, size_t mapped,
                         aar_api *api, const char *elf)
{
    static char *contract_argv[CONTRACT_ARGV_MAX + 1] = { NULL, "alpha", "beta gamma" };
    static char words[512];
    const char *extra = getenv("ENTRY_ARGS");
    const char *slash = strrchr(elf, '/');
    uint8_t *stack = mmap(NULL, APP_STACK_BYTES, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int n = 3;

    if (stack == MAP_FAILED) {
        perror("mmap stack");
        _exit(2);
    }
    contract_argv[0] = (char *)(slash ? slash + 1 : elf);
    if (extra) {
        char *w;

        snprintf(words, sizeof words, "%s", extra);
        for (w = strtok(words, " "); w && n < CONTRACT_ARGV_MAX; w = strtok(NULL, " "))
            contract_argv[n++] = w;
    }
    contract_table(api);
    enter_contract(entry, base, mapped, api, n, contract_argv,
                   stack + APP_STACK_BYTES);
}

int main(int argc, char **argv)
{
    static aar_api api;
    char *app_argv[] = { "app", "one", NULL };
    const char *init = getenv("HEAP_INIT_MIB");
    long size = 0;
    size_t mapped = 0;
    int relocs = 0;
    uint8_t *img;
    uint8_t *base;
    uint64_t entry_addr = 0;
    int (*entry)(const aar_api *, int, char **);
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: %s app.elf\n", argv[0]);
        return 2;
    }
    coherent = getenv("COHERENT") != NULL;
    img = read_file(argv[1], &size);
    if (!img)
        return 2;
    base = load_image(img, (size_t)size, argv[1], &mapped, &relocs, &entry_addr);
    if (!base)
        return 2;
    heap_base = mmap(NULL, HEAP_LIMIT, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap_base == MAP_FAILED)
        return 2;
    heap_committed = (init ? (uint64_t)atoi(init) : 0u) * AAR_HEAP_GRANULE;
    build_table(&api);
    entry = (int (*)(const aar_api *, int, char **))(uintptr_t)entry_addr;
    fprintf(stderr, "[loader] %d relocations applied, entry offset %#lx\n",
            relocs, (unsigned long)(entry_addr - (uint64_t)(uintptr_t)base));
    if (getenv("ENTRY_CONTRACT"))
        run_contract((uintptr_t)entry, base, mapped, &api, argv[1]);
    rc = entry(&api, 2, app_argv);
    fprintf(stderr, "[loader] entry returned %d\n", rc);
    return 99;
}
