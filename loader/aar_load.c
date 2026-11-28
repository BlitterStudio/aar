/*
 * The reference loader of the AAR standard. See aar_load.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "aar_load.h"

#define PF_X 1u
#define PF_W 2u
#define R_ARM_RELATIVE 23u
#define R_AARCH64_RELATIVE 1027u

/* aar_load_place reads whole segments in pieces of this size, and the
 * relocation tables in blocks of this many bytes (a multiple of 8, 12 and 24). */
#define COPY_CHUNK_BYTES 0x10000000u
#define RELOC_BUF_BYTES 768u
#define A32_TOP 0x100000000ull

int aar_load_plan(aar_load_read_fn read, void *user, uint64_t length,
                  uint32_t profiles, struct aar_load_plan *plan,
                  const char **why)
{
    struct aar_image_info info;
    uint32_t i;
    int rc;

    if (why)
        *why = NULL;
    if (!read || !plan)
        return AAR_E_INVAL;
    rc = aar_image_check(read, user, length, 0, &info, why);
    if (rc != AAR_OK)
        return rc;
    memset(plan, 0, sizeof(*plan));
    plan->profile = info.profile;
    plan->length = length;
    plan->align = 1;
    for (i = 0; i < info.nloads; i++) {
        const struct aar_image_load *l = &info.load[i];
        struct aar_load_segment *s;

        if (l->memsz == 0)
            continue;
        s = &plan->seg[plan->nsegments++];
        s->offset = l->offset;
        s->filesz = l->filesz;
        s->memsz = l->memsz;
        s->vaddr = l->vaddr;
        s->flags = AAR_LOAD_R | ((l->flags & PF_W) ? AAR_LOAD_W : 0u) |
                   ((l->flags & PF_X) ? AAR_LOAD_X : 0u);
        if (l->vaddr + l->memsz > plan->span)
            plan->span = l->vaddr + l->memsz;
        if (l->align > plan->align && (l->align & (l->align - 1u)) == 0)
            plan->align = l->align;
    }
    plan->entry_offset = info.entry;
    for (i = 0; i < info.ntables; i++) {
        plan->reloc[i].offset = info.table[i].offset;
        plan->reloc[i].size = info.table[i].size;
        plan->reloc[i].count = info.table[i].count;
        plan->reloc[i].entsize = info.table[i].entsize;
        plan->reloc[i].rela = info.table[i].rela;
    }
    plan->nrelocs = info.ntables;
    plan->note_version = info.note_version;
    plan->api_major = info.api_major;
    plan->api_minor_required = info.api_minor_required;
    plan->stack_bytes = info.stack_bytes;
    plan->min_heap_mib = info.min_heap_mib;
    plan->groups_required = info.groups_required;
    plan->groups_optional = info.groups_optional;
    if (!(profiles & info.profile)) {
        if (why)
            *why = "the platform does not run the profile of this image";
        return AAR_E_PROFILE;
    }
    return AAR_OK;
}

/* ---- Placing ------------------------------------------------------------------- */

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t ld64(const uint8_t *p)
{
    return (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}

/* The targets of relocations have the alignment of their word, but the block
 * the loader writes through has none that the loader knows: bytes. */
static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void st64(uint8_t *p, uint64_t v)
{
    st32(p, (uint32_t)v);
    st32(p + 4, (uint32_t)(v >> 32));
}

static int refuse(const char **why, int code, const char *text)
{
    if (why)
        *why = text;
    return code;
}

/* The plan is one aar_load_plan makes, so far as place can tell without the
 * image: place trusts nothing it is given, and every write it makes is inside
 * [0, span). */
static int check_plan(const struct aar_load_plan *plan, const char **why)
{
    int a32 = plan->profile == AAR_PROFILE_A32;
    int in_x = 0;
    uint32_t i, j;

    if (!a32 && plan->profile != AAR_PROFILE_A64)
        return refuse(why, AAR_E_INVAL, "the plan names no profile");
    if (plan->nsegments == 0 || plan->nsegments > AAR_LOAD_MAX_SEGMENTS)
        return refuse(why, AAR_E_INVAL, "the plan has no segment, or too many");
    if (plan->nrelocs > AAR_LOAD_MAX_RELOCS)
        return refuse(why, AAR_E_INVAL, "the plan has too many relocation tables");
    for (i = 0; i < plan->nsegments; i++) {
        const struct aar_load_segment *s = &plan->seg[i];

        if (s->memsz < s->filesz)
            return refuse(why, AAR_E_INVAL, "a segment has memsz below filesz");
        if (s->vaddr > plan->span || s->memsz > plan->span - s->vaddr)
            return refuse(why, AAR_E_INVAL, "a segment is outside the span");
        if (s->offset > plan->length || s->filesz > plan->length - s->offset)
            return refuse(why, AAR_E_INVAL, "the bytes of a segment are not in the image");
        if ((s->flags & AAR_LOAD_W) && (s->flags & AAR_LOAD_X))
            return refuse(why, AAR_E_INVAL, "a segment is writable and executable");
        for (j = 0; j < i; j++) {
            const struct aar_load_segment *o = &plan->seg[j];

            /* Both lie inside the span, so the sums do not wrap. */
            if (s->memsz && o->memsz && s->vaddr < o->vaddr + o->memsz &&
                o->vaddr < s->vaddr + s->memsz)
                return refuse(why, AAR_E_INVAL, "the segments of the plan overlap");
        }
        if ((s->flags & AAR_LOAD_X) && plan->entry_offset >= s->vaddr &&
            plan->entry_offset - s->vaddr < s->filesz)
            in_x = 1;
    }
    if (!in_x)
        return refuse(why, AAR_E_INVAL, "the entry is not inside an executable segment");
    for (i = 0; i < plan->nrelocs; i++) {
        const struct aar_load_reloc *t = &plan->reloc[i];
        int ok = a32 ? (t->rela ? t->entsize == 12u : t->entsize == 8u)
                     : (t->rela && t->entsize == 24u);

        if (!ok)
            return refuse(why, AAR_E_INVAL, "a relocation table has an entry size the profile does not have");
        if (t->offset > plan->length || t->size > plan->length - t->offset)
            return refuse(why, AAR_E_INVAL, "a relocation table is not in the image");
    }
    return AAR_OK;
}

static int apply_table(const struct aar_load_plan *plan, const struct aar_load_reloc *t,
                       aar_load_read_fn read, void *user, uint8_t *mem,
                       uint64_t app_base, const char **why)
{
    const int a64 = plan->profile == AAR_PROFILE_A64;
    const uint32_t width = a64 ? 8u : 4u;
    uint8_t buf[RELOC_BUF_BYTES];
    uint64_t pos = 0;

    while (t->size - pos >= t->entsize) {
        uint64_t left = t->size - pos;
        uint32_t take = left < sizeof(buf) ? (uint32_t)left : (uint32_t)sizeof(buf);
        uint32_t k;

        take -= take % t->entsize;
        if (read(user, t->offset + pos, buf, take) < 0)
            return refuse(why, AAR_E_IO, "the image could not be read");
        for (k = 0; k < take; k += t->entsize) {
            const uint8_t *e = buf + k;
            uint64_t target, addend = 0;
            uint32_t type, sym;

            if (a64) {
                target = ld64(e);
                type = ld32(e + 8);
                sym = ld32(e + 12);
                addend = ld64(e + 16);
            } else {
                uint32_t info = ld32(e + 4);

                target = ld32(e);
                type = info & 0xFFu;
                sym = info >> 8;
                if (t->rela)
                    addend = ld32(e + 8);
            }
            if (type != (a64 ? R_AARCH64_RELATIVE : R_ARM_RELATIVE) || sym != 0)
                return refuse(why, AAR_E_INVAL, "a relocation is not RELATIVE with symbol 0");
            if (target & (width - 1u))
                return refuse(why, AAR_E_INVAL, "a relocation target is not aligned");
            if (plan->span < width || target > plan->span - width)
                return refuse(why, AAR_E_INVAL, "a relocation target is outside the image");
            if (a64) {
                st64(mem + (size_t)target, addend + app_base);
            } else {
                if (!t->rela)
                    addend = ld32(mem + (size_t)target);
                st32(mem + (size_t)target, (uint32_t)(addend + app_base));
            }
        }
        pos += take;
    }
    return AAR_OK;
}

int aar_load_place(const struct aar_load_plan *plan, aar_load_read_fn read,
                   void *user, void *write_base, uint64_t app_base,
                   uint64_t limit_bytes, uint64_t *entry, const char **why)
{
    uint8_t *mem = (uint8_t *)write_base;
    uint32_t i;
    int rc;

    if (why)
        *why = NULL;
    if (!plan || !read || !write_base || !entry)
        return AAR_E_INVAL;
    rc = check_plan(plan, why);
    if (rc != AAR_OK)
        return rc;
    if (plan->span > limit_bytes || plan->span > (uint64_t)SIZE_MAX)
        return refuse(why, AAR_E_NOMEM, "the image does not fit the memory offered");
    if (app_base > UINT64_MAX - plan->span ||
        (plan->profile == AAR_PROFILE_A32 && app_base + plan->span > A32_TOP))
        return refuse(why, AAR_E_INVAL, "the image does not fit the address space at the base");

    memset(mem, 0, (size_t)plan->span);
    for (i = 0; i < plan->nsegments; i++) {
        const struct aar_load_segment *s = &plan->seg[i];
        uint64_t done = 0;

        while (done < s->filesz) {
            uint64_t left = s->filesz - done;
            uint32_t chunk = left < COPY_CHUNK_BYTES ? (uint32_t)left : COPY_CHUNK_BYTES;

            if (read(user, s->offset + done, mem + (size_t)(s->vaddr + done), chunk) < 0)
                return refuse(why, AAR_E_IO, "the image could not be read");
            done += chunk;
        }
    }
    for (i = 0; i < plan->nrelocs; i++) {
        rc = apply_table(plan, &plan->reloc[i], read, user, mem, app_base, why);
        if (rc != AAR_OK)
            return rc;
    }
    *entry = app_base + plan->entry_offset;
    return AAR_OK;
}
