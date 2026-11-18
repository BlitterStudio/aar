/*
 * Test loader for libaar. Loads an AAR PIE ELF into anonymous memory, applies
 * the RELATIVE relocations listed in PT_DYNAMIC the way a runtime must (spec
 * 2.2), builds an aar_api table over POSIX calls and enters the image. Build
 * it for Linux user mode (arm-linux-gnueabihf-gcc -static -marm, or
 * aarch64-linux-gnu-gcc -static) and run it under qemu-arm or qemu-aarch64:
 *
 *   loader app.elf
 *
 * Environment:
 *   AAR_ROOT         directory that file paths resolve in (default .)
 *   HEAP_INIT_MIB    private heap committed at start (default 0)
 *   CORE_ONLY=1      hand the app a table that ends before the optional slots
 *   COHERENT=1       report AAR_INFOF_SHARED_COHERENT, so publish and acquire
 *                    must not reach the table
 *
 * One path has a meaning of its own: stat-calls.magic stats as a file whose
 * size is the number of stat calls made so far.
 *
 * The exit report on stderr counts heap_grow, heap_shrink and
 * publish/acquire calls. The exit status is the app's.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aar/app.h"

#if defined(__aarch64__)
typedef Elf64_Ehdr Ehdr;
typedef Elf64_Phdr Phdr;
typedef Elf64_Dyn Dyn;
typedef Elf64_Rela Reloc;
#define DT_RELOC DT_RELA
#define DT_RELOCSZ DT_RELASZ
#define RELOC_RELATIVE R_AARCH64_RELATIVE
#define RELOC_TYPE(info) ELF64_R_TYPE(info)
#else
typedef Elf32_Ehdr Ehdr;
typedef Elf32_Phdr Phdr;
typedef Elf32_Dyn Dyn;
typedef Elf32_Rel Reloc;
#define DT_RELOC DT_REL
#define DT_RELOCSZ DT_RELSZ
#define RELOC_RELATIVE R_ARM_RELATIVE
#define RELOC_TYPE(info) ELF32_R_TYPE(info)
#endif

#define HEAP_LIMIT (64u << 20)
#define MAX_FILES 64

static uint8_t *heap_base;
static uint64_t heap_committed;
static unsigned grow_calls;
static unsigned shrink_calls;
static unsigned publish_calls;
static int coherent;
static int files[MAX_FILES];
static unsigned stat_calls;

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
    if (bytes % AAR_HEAP_GRANULE)
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
    if (bytes % AAR_HEAP_GRANULE || bytes > heap_committed)
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
    case ENOENT: return AAR_E_NOENT;
    case EEXIST: return AAR_E_EXIST;
    default:     return AAR_E_IO;
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

/* A path with a meaning of its own, for runtime_test: MAGIC_STAT_CALLS stats
 * as a file whose size is the number of ordinary stat calls so far. */
#define MAGIC_STAT_CALLS "stat-calls.magic"

static int app_file_open(const char *path, uint32_t flags)
{
    char full[300];
    int mode = O_RDONLY;
    int fd;
    int i;

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
    host_path(full, sizeof full, path);
    fd = open(full, mode, 0644);
    if (fd < 0)
        return aar_error();
    for (i = 0; i < MAX_FILES; i++) {
        if (!files[i]) {
            files[i] = fd;
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
    pos = lseek(fd, (off_t)off, whence);
    if (pos < 0)
        return AAR_E_IO;
    *out_pos = pos;
    return AAR_OK;
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
    host_path(full, sizeof full, path);
    if (stat(full, &st) != 0)
        return aar_error();
    out->size = (uint64_t)st.st_size;
    out->mtime = st.st_mtime;
    out->mode = S_ISDIR(st.st_mode) ? AAR_MODE_DIR : AAR_MODE_FILE;
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
    api->file_open = app_file_open;
    api->file_close = app_file_close;
    api->file_read = app_file_read;
    api->file_write = app_file_write;
    api->file_seek = app_file_seek;
    api->file_stat = app_file_stat;
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

/* Copy the PT_LOAD segments to a fresh mapping and apply the dynamic
 * relocations. Returns the load base, or NULL. */
static uint8_t *load_image(const uint8_t *img, size_t *mapped, int *relocs)
{
    const Ehdr *eh = (const Ehdr *)img;
    const Phdr *ph = (const Phdr *)(img + eh->e_phoff);
    uintptr_t top = 0;
    uintptr_t table = 0;
    uintptr_t table_bytes = 0;
    uint8_t *base;
    int i;

    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > top)
            top = ph[i].p_vaddr + ph[i].p_memsz;
    top = (top + 0xfffu) & ~(uintptr_t)0xfffu;
    base = mmap(NULL, top, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD)
            memcpy(base + ph[i].p_vaddr, img + ph[i].p_offset,
                   ph[i].p_filesz);
    for (i = 0; i < eh->e_phnum; i++) {
        const Dyn *d;

        if (ph[i].p_type != PT_DYNAMIC)
            continue;
        for (d = (const Dyn *)(base + ph[i].p_vaddr); d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_RELOC)
                table = d->d_un.d_ptr;
            if (d->d_tag == DT_RELOCSZ)
                table_bytes = d->d_un.d_val;
        }
    }
    *relocs = 0;
    for (const Reloc *r = (const Reloc *)(base + table);
         (const uint8_t *)r < base + table + table_bytes; r++) {
        uintptr_t *where = (uintptr_t *)(base + r->r_offset);

        if (RELOC_TYPE(r->r_info) != RELOC_RELATIVE) {
            fprintf(stderr, "relocation type %u is not RELATIVE\n",
                    (unsigned)RELOC_TYPE(r->r_info));
            return NULL;
        }
#if defined(__aarch64__)
        *where = (uintptr_t)base + (uintptr_t)r->r_addend;
#else
        *where += (uintptr_t)base;
#endif
        (*relocs)++;
    }
    __builtin___clear_cache((char *)base, (char *)base + top);
    *mapped = top;
    return base;
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
    base = load_image(img, &mapped, &relocs);
    if (!base)
        return 2;
    heap_base = mmap(NULL, HEAP_LIMIT, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap_base == MAP_FAILED)
        return 2;
    heap_committed = (init ? (uint64_t)atoi(init) : 0u) * AAR_HEAP_GRANULE;
    build_table(&api);
    entry = (int (*)(const aar_api *, int, char **))
            (base + ((const Ehdr *)img)->e_entry);
    fprintf(stderr, "[loader] %d relocations applied, entry offset %#lx\n",
            relocs, (unsigned long)((const Ehdr *)img)->e_entry);
    rc = entry(&api, 2, app_argv);
    fprintf(stderr, "[loader] entry returned %d\n", rc);
    return 99;
}
