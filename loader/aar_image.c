/*
 * The image rules of the AAR standard. See aar_image.h. This is a port of the
 * image checks of tools/aar-check (check(), in the same order) that also
 * returns what the loader needs. Keep the two in step: client/tests/probe_corpus.py
 * and loader/tests/loader_corpus.py run both over the corpus of
 * tests/aar_check_test.py and over mutations of it.
 *
 * Every size and offset comes from the file and is untrusted. Arithmetic is on
 * uint64_t with the range checked before the add, so nothing wraps, there is no
 * 64-bit division (the m68k build of the probe has no helper for it), and every
 * read goes through rd(), which checks the length.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "aar_image.h"

#define ELFCLASS32 1u
#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define ET_DYN 3u
#define EM_ARM 40u
#define EM_AARCH64 183u
#define EF_ARM_EABIMASK 0xFF000000u
#define EF_ARM_EABI_VER5 0x05000000u
#define EF_ARM_ABI_FLOAT_SOFT 0x200u
#define EF_ARM_ABI_FLOAT_HARD 0x400u

#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_NOTE 4u
#define PF_X 1u
#define PF_W 2u
#define SHT_RELA 4u
#define SHT_DYNAMIC 6u
#define SHT_NOTE 7u
#define SHT_REL 9u
#define SHT_RELR 19u
#define SHF_ALLOC 2u

#define DT_NULL 0u
#define DT_NEEDED 1u
#define DT_PLTRELSZ 2u
#define DT_RELA 7u
#define DT_RELASZ 8u
#define DT_RELAENT 9u
#define DT_REL 17u
#define DT_RELSZ 18u
#define DT_RELENT 19u
#define DT_JMPREL 23u
#define DT_RELRSZ 35u
#define DT_RELR 36u
#define DT_RELRENT 37u

#define R_ARM_RELATIVE 23u
#define R_AARCH64_RELATIVE 1027u

#define STACK_MIN (64u << 10)
#define STACK_MAX (1u << 20)

#define WINDOW_BYTES 512u
#define MAX_LOADS AAR_IMAGE_MAX_LOADS

struct sec {
    uint32_t name, type;
    uint64_t flags, off, size;
};

struct seg {
    uint32_t type, flags;
    uint64_t off, vaddr, filesz, memsz, align;
};

struct image {
    struct aar_image_info *out;
    aar_image_read_fn fn;
    void *user;
    uint64_t len;
    const char *why;
    int strict; /* AAR_IMAGE_SECTIONS */
    int a64;
    uint64_t entry, phoff, shoff;
    uint32_t eflags;
    uint32_t etype, machine, phentsize, phnum, shentsize, shnum, shstrndx;
    uint64_t names_off, names_size;
    uint64_t win_off;
    uint32_t win_len;
    uint8_t win[WINDOW_BYTES];
};

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int refuse_as(struct image *p, int code, const char *why)
{
    p->why = why;
    return code;
}

static int refuse(struct image *p, const char *why)
{
    return refuse_as(p, AAR_E_INVAL, why);
}

/* [off, off + n) lies inside the image. */
static int in_range(const struct image *p, uint64_t off, uint64_t n)
{
    return off <= p->len && n <= p->len - off;
}

/* Copies 1 <= n <= WINDOW_BYTES bytes at off into dst, through a window of
 * the file that is read in one call and serves the walks over headers and
 * tables. AAR_OK, AAR_E_INVAL for a range outside the image, AAR_E_IO when the
 * source fails. */
static int rd(struct image *p, uint64_t off, void *dst, uint32_t n)
{
    if (!in_range(p, off, n))
        return refuse(p, "truncated image");
    if (!(off >= p->win_off && off - p->win_off <= p->win_len &&
          n <= p->win_len - (uint32_t)(off - p->win_off))) {
        uint64_t left = p->len - off;
        uint32_t want = left < WINDOW_BYTES ? (uint32_t)left : WINDOW_BYTES;

        p->win_len = 0;
        if (p->fn(p->user, off, p->win, want) < 0)
            return refuse_as(p, AAR_E_IO, "the image could not be read");
        p->win_off = off;
        p->win_len = want;
    }
    memcpy(dst, p->win + (size_t)(off - p->win_off), n);
    return AAR_OK;
}

#define TRY(call)                                                        \
    do {                                                                 \
        int try_rc_ = (call);                                            \
        if (try_rc_ != AAR_OK)                                           \
            return try_rc_;                                              \
    } while (0)

static uint64_t align_up(uint64_t v, uint32_t to)
{
    return (v + to - 1u) & ~(uint64_t)(to - 1u);
}

/* v modulo 3, without a 64-bit division: 2^16 is 1 modulo 3, so a value is
 * the sum of its 16-bit digits. */
static uint32_t mod3(uint64_t v)
{
    while (v > 0xFFFFu)
        v = (v >> 16) + (v & 0xFFFFu);
    return (uint32_t)v % 3u;
}

/* size is a multiple of ent, which is 8, 12 or 24. */
static int multiple_of(uint64_t size, uint32_t ent)
{
    if (size & (ent == 12u ? 3u : 7u))
        return 0;
    if (ent == 8u)
        return 1;
    return mod3(ent == 12u ? size >> 2 : size >> 3) == 0;
}

/* ---- Headers ----------------------------------------------------------------- */

static int read_headers(struct image *p)
{
    uint8_t h[64];
    uint32_t ehsize;

    if (p->len < 16)
        return refuse(p, "not a little-endian ELF32 or ELF64 image");
    TRY(rd(p, 0, h, 16));
    if (memcmp(h, "\x7f" "ELF", 4) != 0 || h[5] != ELFDATA2LSB ||
        (h[4] != ELFCLASS32 && h[4] != ELFCLASS64))
        return refuse(p, "not a little-endian ELF32 or ELF64 image");
    p->a64 = h[4] == ELFCLASS64;
    ehsize = p->a64 ? 64u : 52u;
    if (p->len < ehsize)
        return refuse(p, "truncated ELF header");
    TRY(rd(p, 0, h, ehsize));
    p->etype = le16(h + 16);
    p->machine = le16(h + 18);
    if (p->a64) {
        p->entry = le64(h + 24);
        p->phoff = le64(h + 32);
        p->shoff = le64(h + 40);
        p->eflags = le32(h + 48);
        p->phentsize = le16(h + 54);
        p->phnum = le16(h + 56);
        p->shentsize = le16(h + 58);
        p->shnum = le16(h + 60);
        p->shstrndx = le16(h + 62);
    } else {
        p->entry = le32(h + 24);
        p->phoff = le32(h + 28);
        p->shoff = le32(h + 32);
        p->eflags = le32(h + 36);
        p->phentsize = le16(h + 42);
        p->phnum = le16(h + 44);
        p->shentsize = le16(h + 46);
        p->shnum = le16(h + 48);
        p->shstrndx = le16(h + 50);
    }
    return AAR_OK;
}

static int check_header(struct image *p)
{
    if (p->etype != ET_DYN)
        return refuse(p, "e_type is not ET_DYN");
    if (p->machine != (p->a64 ? EM_AARCH64 : EM_ARM))
        return refuse(p, "e_machine does not match the ELF class");
    if (!p->a64) {
        if ((p->eflags & EF_ARM_EABIMASK) != EF_ARM_EABI_VER5)
            return refuse(p, "e_flags is not EABI version 5");
        if (p->eflags & EF_ARM_ABI_FLOAT_SOFT)
            return refuse(p, "soft-float ABI, the profile is hard-float");
        if (!(p->eflags & EF_ARM_ABI_FLOAT_HARD))
            return refuse(p, "EF_ARM_ABI_FLOAT_HARD is not set");
    } else if (p->eflags != 0) {
        return refuse(p, "e_flags is not 0");
    }
    return AAR_OK;
}

static int read_phdr(struct image *p, uint32_t i, struct seg *s)
{
    uint8_t b[56];
    uint64_t at = p->phoff + (uint64_t)i * p->phentsize;

    TRY(rd(p, at, b, p->a64 ? 56u : 32u));
    s->type = le32(b);
    if (p->a64) {
        s->flags = le32(b + 4);
        s->off = le64(b + 8);
        s->vaddr = le64(b + 16);
        s->filesz = le64(b + 32);
        s->memsz = le64(b + 40);
        s->align = le64(b + 48);
    } else {
        s->off = le32(b + 4);
        s->vaddr = le32(b + 8);
        s->filesz = le32(b + 16);
        s->memsz = le32(b + 20);
        s->flags = le32(b + 24);
        s->align = le32(b + 28);
    }
    return AAR_OK;
}

static int read_shdr(struct image *p, uint32_t i, struct sec *s)
{
    uint8_t b[64];
    uint64_t at = p->shoff + (uint64_t)i * p->shentsize;

    TRY(rd(p, at, b, p->a64 ? 64u : 40u));
    s->name = le32(b);
    s->type = le32(b + 4);
    if (p->a64) {
        s->flags = le64(b + 8);
        s->off = le64(b + 24);
        s->size = le64(b + 32);
    } else {
        s->flags = le32(b + 8);
        s->off = le32(b + 16);
        s->size = le32(b + 20);
    }
    return AAR_OK;
}

static int parse_tables(struct image *p)
{
    struct sec names;

    if (p->phentsize < (p->a64 ? 56u : 32u) ||
        (p->strict && p->shentsize < (p->a64 ? 64u : 40u)))
        return refuse(p, "unexpected ELF header size");
    if (!in_range(p, p->phoff, (uint64_t)p->phnum * p->phentsize))
        return refuse(p, "truncated ELF program headers");
    if (!p->strict)
        return AAR_OK;
    if (p->shnum == 0 || p->shoff == 0)
        return refuse(p, "section headers are missing");
    if (!in_range(p, p->shoff, (uint64_t)p->shnum * p->shentsize))
        return refuse(p, "truncated ELF section headers");
    if (p->shstrndx >= p->shnum)
        return refuse(p, "section name table is missing");
    TRY(read_shdr(p, p->shstrndx, &names));
    if (!in_range(p, names.off, names.size))
        return refuse(p, "truncated section name table");
    p->names_off = names.off;
    p->names_size = names.size;
    return AAR_OK;
}

/* The name of a section is equal to `want`: the bytes of the name table from
 * the offset to the first NUL (or the end of the table). */
static int name_is(struct image *p, uint32_t name, const char *want, int *equal)
{
    uint64_t at = name;
    uint32_t i;

    *equal = 0;
    for (i = 0;; i++) {
        uint8_t c = 0;

        if (at + i < p->names_size)
            TRY(rd(p, p->names_off + at + i, &c, 1));
        if (c != (uint8_t)want[i])
            break;
        if (c == 0) {
            *equal = 1;
            break;
        }
    }
    return AAR_OK;
}

/* ---- Segments ----------------------------------------------------------------- */

static int check_segments(struct image *p)
{
    struct aar_image_info *out = p->out;
    uint32_t i;
    uint32_t n = 0;

    for (i = 0; i < p->phnum; i++) {
        struct seg s;

        TRY(read_phdr(p, i, &s));
        if (s.type == PT_INTERP)
            return refuse(p, "PT_INTERP is present");
        if (!in_range(p, s.off, s.filesz))
            return refuse(p, "segment extends past the file");
        if (s.type == PT_LOAD) {
            if ((s.flags & PF_W) && (s.flags & PF_X))
                return refuse(p, "PT_LOAD is writable and executable");
            if (s.memsz < s.filesz)
                return refuse(p, "PT_LOAD p_memsz is below p_filesz");
            if (n < MAX_LOADS) {
                out->load[n].vaddr = s.vaddr;
                out->load[n].offset = s.off;
                out->load[n].filesz = s.filesz;
                out->load[n].memsz = s.memsz;
                out->load[n].align = s.align;
                out->load[n].flags = s.flags;
            }
            n++;
        }
    }
    if (n > MAX_LOADS)
        return refuse(p, "more than 4 PT_LOAD segments");
    if (n == 0)
        return refuse(p, "no PT_LOAD");
    out->nloads = n;
    {
        /* Sorted by (vaddr, memsz), the segments with memory must not
         * overlap. At most 4: an insertion sort. */
        uint32_t order[MAX_LOADS], m = 0, a, b;

        for (a = 0; a < n; a++) {
            if (out->load[a].memsz == 0)
                continue;
            b = m++;
            while (b > 0) {
                const struct aar_image_load *prev = &out->load[order[b - 1]];
                const struct aar_image_load *cur = &out->load[a];

                if (prev->vaddr < cur->vaddr ||
                    (prev->vaddr == cur->vaddr && prev->memsz <= cur->memsz))
                    break;
                order[b] = order[b - 1];
                b--;
            }
            order[b] = a;
        }
        for (a = 0; a + 1 < m; a++) {
            const struct aar_image_load *l0 = &out->load[order[a]];
            const struct aar_image_load *l1 = &out->load[order[a + 1]];

            if (l1->vaddr - l0->vaddr < l0->memsz)
                return refuse(p, "PT_LOAD segments overlap");
        }
    }
    return AAR_OK;
}

/* The file offset of [vaddr, vaddr + size) when one PT_LOAD holds it in its
 * file image. */
static int vaddr_to_off(const struct image *p, uint64_t vaddr, uint64_t size,
                        uint64_t *off)
{
    uint32_t i;

    for (i = 0; i < p->out->nloads; i++) {
        const struct aar_image_load *l = &p->out->load[i];

        if (vaddr >= l->vaddr && vaddr - l->vaddr <= l->filesz &&
            l->filesz - (vaddr - l->vaddr) >= size) {
            *off = l->offset + (vaddr - l->vaddr);
            return 1;
        }
    }
    return 0;
}

/* ---- Relocations and the dynamic table ------------------------------------------- */

/* `rela` is non-zero for a RELA table. Every entry must be RELATIVE with
 * symbol 0, aligned, and target a PT_LOAD. Stores the entry count in *count. */
static int check_reloc_table(struct image *p, uint64_t off, uint64_t size, int rela,
                             uint64_t *count)
{
    uint32_t ent, align;
    uint64_t pos, n = 0;

    if (p->a64 && !rela)
        return refuse(p, "A64 images use RELA tables only");
    ent = p->a64 ? 24u : (rela ? 12u : 8u);
    align = p->a64 ? 8u : 4u;
    /* The callers have checked the table is inside the image, so `size`
     * is below the image length and `pos` below does not wrap. */
    if (size > p->len)
        return refuse(p, "a relocation table is larger than the image");
    if (!multiple_of(size, ent))
        return refuse(p, "relocation table size is not a multiple of the entry size");
    for (pos = 0; pos < size; pos += ent, n++) {
        uint8_t b[24];
        uint64_t r_offset;
        uint32_t r_type, r_sym, r_info, i;
        int in_load = 0;

        TRY(rd(p, off + pos, b, ent));
        if (p->a64) {
            /* r_info is 64 bits: the type is the low word, the symbol the high. */
            r_offset = le64(b);
            r_type = le32(b + 8);
            r_sym = le32(b + 12);
        } else {
            r_offset = le32(b);
            r_info = le32(b + 4);
            r_type = r_info & 0xFFu;
            r_sym = r_info >> 8;
        }
        if (r_type != (p->a64 ? R_AARCH64_RELATIVE : R_ARM_RELATIVE) || r_sym != 0)
            return refuse(p, "a relocation is not RELATIVE with symbol 0");
        if (r_offset & (align - 1u))
            return refuse(p, "a relocation target is not aligned");
        for (i = 0; i < p->out->nloads; i++) {
            const struct aar_image_load *l = &p->out->load[i];

            if (l->vaddr <= r_offset && r_offset - l->vaddr <= l->memsz &&
                l->memsz - (r_offset - l->vaddr) >= align) {
                in_load = 1;
                break;
            }
        }
        if (!in_load)
            return refuse(p, "a relocation target is outside every PT_LOAD");
    }
    *count = n;
    return AAR_OK;
}

struct dyn_tags {
    int has_rel, has_relsz, has_relent;
    int has_rela, has_relasz, has_relaent;
    uint64_t rel, relsz, relent, rela, relasz, relaent;
};

/* Reads one dynamic table and refuses what no app may have. Records the
 * relocation tags, the last of each. */
static int read_dynamic(struct image *p, uint64_t off, uint64_t size,
                        struct dyn_tags *t)
{
    uint32_t dsz = p->a64 ? 16u : 8u;
    uint64_t n, count;

    memset(t, 0, sizeof(*t));
    if (!in_range(p, off, size) || (size & (dsz - 1u)))
        return refuse(p, "truncated dynamic table");
    count = p->a64 ? size >> 4 : size >> 3;
    for (n = 0; n < count; n++) {
        uint8_t b[16];
        uint64_t tag, val;

        TRY(rd(p, off + n * dsz, b, dsz));
        if (p->a64) {
            tag = le64(b);
            val = le64(b + 8);
        } else {
            tag = le32(b);
            val = le32(b + 4);
        }
        if (tag == DT_NULL)
            break;
        if (tag == DT_NEEDED)
            return refuse(p, "DT_NEEDED");
        if (tag == DT_RELR || tag == DT_RELRSZ || tag == DT_RELRENT)
            return refuse(p, "DT_RELR is not allowed");
        if (tag == DT_JMPREL || (tag == DT_PLTRELSZ && val))
            return refuse(p, "DT_JMPREL (PLT relocations) is not allowed");
        switch (tag) {
        case DT_REL: t->has_rel = 1; t->rel = val; break;
        case DT_RELSZ: t->has_relsz = 1; t->relsz = val; break;
        case DT_RELENT: t->has_relent = 1; t->relent = val; break;
        case DT_RELA: t->has_rela = 1; t->rela = val; break;
        case DT_RELASZ: t->has_relasz = 1; t->relasz = val; break;
        case DT_RELAENT: t->has_relaent = 1; t->relaent = val; break;
        default: break;
        }
    }
    return AAR_OK;
}

static int check_table_tags(struct image *p, int have_addr, uint64_t addr,
                            int have_size, uint64_t size, int have_ent,
                            uint64_t ent, uint64_t want_ent, int rela)
{
    struct aar_image_table *t;
    uint64_t off;

    if (!have_addr || !have_size || !have_ent)
        return refuse(p, "a relocation tag, its size and its entry size must all be present");
    if (ent != want_ent)
        return refuse(p, "a relocation table has the wrong entry size");
    if (!vaddr_to_off(p, addr, size, &off))
        return refuse(p, "a relocation table is not inside a PT_LOAD");
    t = &p->out->table[p->out->ntables];
    TRY(check_reloc_table(p, off, size, rela, &t->count));
    t->offset = off;
    t->size = size;
    t->entsize = (uint32_t)want_ent;
    t->rela = rela ? 1u : 0u;
    p->out->ntables++;
    return AAR_OK;
}

static int check_dynamic(struct image *p)
{
    uint32_t i, found = 0;
    struct seg dyn;
    struct dyn_tags t;

    memset(&dyn, 0, sizeof(dyn));
    for (i = 0; i < p->phnum; i++) {
        struct seg s;

        TRY(read_phdr(p, i, &s));
        if (s.type == PT_DYNAMIC) {
            dyn = s;
            found++;
        }
    }
    if (found != 1)
        return refuse(p, "need exactly one PT_DYNAMIC segment");
    TRY(read_dynamic(p, dyn.off, dyn.filesz, &t));
    if (t.has_rel || t.has_relsz || t.has_relent) {
        if (p->a64)
            return refuse(p, "A64 images use DT_RELA, not DT_REL");
        TRY(check_table_tags(p, t.has_rel, t.rel, t.has_relsz, t.relsz,
                             t.has_relent, t.relent, 8, 0));
    }
    if (t.has_rela || t.has_relasz || t.has_relaent)
        TRY(check_table_tags(p, t.has_rela, t.rela, t.has_relasz, t.relasz,
                             t.has_relaent, t.relaent, p->a64 ? 24u : 12u, 1));
    return AAR_OK;
}

/* The relocation and dynamic sections: the rules tools/aar-check applies to
 * the section headers (AAR_IMAGE_SECTIONS only). */
static int check_section_relocs(struct image *p)
{
    uint32_t i;

    for (i = 0; i < p->shnum; i++) {
        struct sec s;
        int equal;

        TRY(read_shdr(p, i, &s));
        if (s.type == SHT_RELR)
            return refuse(p, "an SHT_RELR section is not allowed");
        if ((s.type == SHT_REL || s.type == SHT_RELA) && (s.flags & SHF_ALLOC)) {
            int ok_name;
            uint64_t count;

            TRY(name_is(p, s.name, ".rel.dyn", &equal));
            ok_name = equal;
            TRY(name_is(p, s.name, ".rela.dyn", &equal));
            if (!ok_name && !equal)
                return refuse(p, "an allocated relocation section has a stray name");
            if (!in_range(p, s.off, s.size))
                return refuse(p, "a relocation section extends past the file");
            TRY(check_reloc_table(p, s.off, s.size, s.type == SHT_RELA, &count));
        }
        if (s.type == SHT_DYNAMIC) {
            struct dyn_tags t;

            TRY(read_dynamic(p, s.off, s.size, &t));
        }
    }
    return AAR_OK;
}

static int check_entry(struct image *p)
{
    uint32_t i;

    if (!p->a64 && (p->entry & 1u))
        return refuse(p, "e_entry has the Thumb bit set");
    for (i = 0; i < p->out->nloads; i++) {
        const struct aar_image_load *l = &p->out->load[i];

        if ((l->flags & PF_X) && p->entry >= l->vaddr &&
            p->entry - l->vaddr < l->filesz)
            return AAR_OK;
    }
    return refuse(p, "e_entry is not inside an executable PT_LOAD");
}

/* ---- The note ------------------------------------------------------------------------ */

struct found_note {
    uint32_t count;
    uint64_t pos, desc;
    uint32_t namesz, descsz, type;
    uint8_t name[4];
};

/* Walks one PT_NOTE segment. Every note is parsed (so a broken note anywhere
 * refuses the image); the first AAR note is recorded and all are counted. */
static int scan_notes(struct image *p, uint64_t off, uint64_t size, uint32_t pad,
                      struct found_note *f)
{
    uint64_t end = off + size;
    uint64_t pos = off;

    while (pos < end) {
        uint8_t h[12];
        uint64_t desc;
        uint32_t namesz, descsz, type;

        if (end - pos < 12) {
            uint64_t k;

            for (k = pos; k < end; k++) {
                uint8_t c;

                TRY(rd(p, k, &c, 1));
                if (c)
                    return refuse(p, "the note segment has stray bytes");
            }
            break;
        }
        TRY(rd(p, pos, h, 12));
        namesz = le32(h);
        descsz = le32(h + 4);
        type = le32(h + 8);
        desc = pos + 12 + align_up(namesz, pad);
        if (pos + 12 + namesz > end || desc + descsz > end)
            return refuse(p, "a note runs past its segment");
        {
            uint8_t name[4] = {0, 0, 0, 0};
            uint32_t have = namesz < 4 ? namesz : 4;
            int is_aar;

            if (have)
                TRY(rd(p, pos + 12, name, have));
            /* The owner up to its first NUL is "AAR". */
            is_aar = namesz >= 3 && memcmp(name, "AAR", 3) == 0 &&
                     (namesz == 3 || name[3] == 0);
            if (is_aar) {
                if (f->count == 0) {
                    f->pos = pos;
                    f->desc = desc;
                    f->namesz = namesz;
                    f->descsz = descsz;
                    f->type = type;
                    memcpy(f->name, name, 4);
                }
                f->count++;
            }
        }
        pos = desc + align_up(descsz, pad);
    }
    return AAR_OK;
}

/* The one .note.aar section holds the note (AAR_IMAGE_SECTIONS only). */
static int check_note_section(struct image *p, const struct found_note *f)
{
    uint32_t i, aar_sections = 0;
    uint64_t sec_off = 0, sec_size = 0;
    uint32_t sec_type = 0;
    uint64_t sec_flags = 0;

    for (i = 0; i < p->shnum; i++) {
        struct sec s;
        int equal;

        TRY(read_shdr(p, i, &s));
        TRY(name_is(p, s.name, ".note.aar", &equal));
        if (equal) {
            aar_sections++;
            sec_type = s.type;
            sec_flags = s.flags;
            sec_off = s.off;
            sec_size = s.size;
        }
    }
    if (aar_sections != 1)
        return refuse(p, "need exactly one .note.aar section");
    if (sec_type != SHT_NOTE || !(sec_flags & SHF_ALLOC))
        return refuse(p, ".note.aar is not an allocated SHT_NOTE section");
    /* The note is inside the section: [pos, pos + 12 + 4 + 40). */
    if (!(sec_off <= f->pos && f->pos - sec_off <= sec_size &&
          sec_size - (f->pos - sec_off) >= 12u + 4u + AAR_NOTE_DESC_BYTES))
        return refuse(p, "the AAR note is not inside .note.aar");
    return AAR_OK;
}

static int check_note(struct image *p)
{
    struct aar_image_info *out = p->out;
    struct found_note f;
    uint32_t i;
    uint8_t d[AAR_NOTE_DESC_BYTES];
    uint32_t w[10];

    memset(&f, 0, sizeof(f));
    for (i = 0; i < p->phnum; i++) {
        struct seg s;

        TRY(read_phdr(p, i, &s));
        if (s.type != PT_NOTE)
            continue;
        if (s.off & 3u)
            return refuse(p, "PT_NOTE is not 4-byte aligned");
        TRY(scan_notes(p, s.off, s.filesz,
                       (p->a64 && s.align == 8) ? 8u : 4u, &f));
    }
    if (f.count == 0)
        return refuse(p, "no AAR note in a PT_NOTE segment");
    if (f.count != 1)
        return refuse(p, "more than one AAR note");
    if (f.namesz != 4 || memcmp(f.name, "AAR\0", 4) != 0)
        return refuse(p, "the note owner is not \"AAR\" with its NUL");
    if (f.type != AAR_NOTE_TYPE)
        return refuse(p, "the note type is not 1");
    if (f.descsz != AAR_NOTE_DESC_BYTES)
        return refuse(p, "the note descriptor is not 40 bytes");
    if (p->strict)
        TRY(check_note_section(p, &f));

    TRY(rd(p, f.desc, d, AAR_NOTE_DESC_BYTES));
    for (i = 0; i < 10; i++)
        w[i] = le32(d + 4 * i);
    if (w[0] != AAR_NOTE_MAGIC)
        return refuse(p, "bad note magic");
    if (w[1] != AAR_NOTE_VERSION)
        return refuse(p, "unknown note version");
    if (w[2] != (p->a64 ? AAR_PROFILE_A64 : AAR_PROFILE_A32))
        return refuse_as(p, AAR_E_PROFILE,
                         "the note profile does not match the ELF class");
    if (w[3] != AAR_API_MAJOR)
        return refuse(p, "the note api major is not this library's");
    if (w[4] > AAR_API_MINOR)
        return refuse(p, "the note needs a newer api minor");
    if (w[5] != 0 && (w[5] < STACK_MIN || w[5] > STACK_MAX || (w[5] & 15u)))
        return refuse(p, "the note stack size is not 0 or 64 KiB to 1 MiB in steps of 16");
    /* groups_required and groups_optional are reported, not judged (spec 8.1):
     * the platform answers AAR_E_NOGROUP for a required group it lacks, whether
     * the bit is defined, reserved or experimental, and ignores the optional
     * bits it does not know. */
    if (w[9] != 0)
        return refuse(p, "the note reserved word is not 0");

    out->profile = w[2];
    out->note_version = w[1];
    out->api_major = w[3];
    out->api_minor_required = w[4];
    out->stack_bytes = w[5];
    out->min_heap_mib = w[6];
    out->groups_required = w[7];
    out->groups_optional = w[8];
    return AAR_OK;
}

/* ---- Entry point --------------------------------------------------------------------------- */

int aar_image_check(aar_image_read_fn read, void *user, uint64_t length,
                    uint32_t flags, struct aar_image_info *info,
                    const char **why)
{
    struct image p;
    int rc;

    if (why)
        *why = NULL;
    if (!read || !info || (length >> 62) != 0)
        return AAR_E_INVAL;
    memset(&p, 0, sizeof(p));
    memset(info, 0, sizeof(*info));
    p.out = info;
    p.fn = read;
    p.user = user;
    p.len = length;
    p.strict = (flags & AAR_IMAGE_SECTIONS) != 0;
    rc = read_headers(&p);
    if (rc == AAR_OK)
        rc = check_header(&p);
    if (rc == AAR_OK)
        rc = parse_tables(&p);
    if (rc == AAR_OK)
        rc = check_segments(&p);
    if (rc == AAR_OK)
        rc = check_dynamic(&p);
    if (rc == AAR_OK && p.strict)
        rc = check_section_relocs(&p);
    if (rc == AAR_OK)
        rc = check_entry(&p);
    if (rc == AAR_OK)
        rc = check_note(&p);
    if (rc != AAR_OK) {
        if (why)
            *why = p.why;
        return rc;
    }
    info->entry = p.entry;
    return AAR_OK;
}
