/*
 * A builder of small AAR app images for the loader tests: A32 with a REL and/or
 * a RELA table, A64 with a RELA table, no section headers (spec 2.2 rule 4
 * lets an image have none). It describes where every relocation target is and
 * what it must hold after the load, so a test can check a placed image
 * without a second ELF reader.
 *
 * Layout: a read-execute segment at vaddr 0 (headers, the note, 32 bytes of
 * code) and a read-write segment at `data_vaddr` (default 0x4000) that holds the
 * dynamic table, the relocation tables and the relocated 8-byte slots, followed
 * by `bss` zero bytes (default 0x800). The data segment sits at a file offset
 * that does not equal its vaddr.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_LOADER_TEST_ELFGEN_H
#define AAR_LOADER_TEST_ELFGEN_H

#include <stdint.h>
#include <string.h>

#include <aar/app.h>

#define GEN_CAP 4096u
#define GEN_MAX_SLOTS 16u
#define GEN_CANARY32 0x5A5A5A5Au
#define GEN_PLACEHOLDER 0xDEADBEEFu

struct gen {
    int a64;
    int rel;              /* A32: a REL table (default when neither is set) */
    int rela;             /* A32: a RELA table; A64: always */
    uint32_t nrel;        /* entries per table, default 3, at most 4 */
    uint32_t bss;         /* zero bytes after the data, default 0x800 */
    int bss_target;       /* one more entry of the last table, aimed into the bss */
    uint32_t data_vaddr;  /* default 0x4000 */
    uint32_t text_extra;  /* added to the memsz of the text segment */
    int no_note_segment;  /* no PT_NOTE: the image must be refused */
    uint64_t first_target; /* when not 0: where the first relocation lands, instead of its slot */
};

struct gen_slot {
    uint64_t vaddr;       /* where the relocation lands */
    uint64_t addend;      /* the addend, from the entry or the target word */
    uint32_t width;       /* bytes the relocation writes: 4 or 8 */
};

struct gen_out {
    size_t len;
    uint64_t entry;       /* e_entry */
    uint64_t text_end;    /* end of the file bytes of the text segment */
    uint64_t data_vaddr;
    uint64_t data_filesz;
    uint64_t data_memsz;
    uint64_t span;        /* end of the data segment's memsz */
    uint32_t nslots;
    struct gen_slot slot[GEN_MAX_SLOTS];
};

static inline void gen_put16(uint8_t *b, uint64_t off, uint32_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
}

static inline void gen_put32(uint8_t *b, uint64_t off, uint32_t v)
{
    gen_put16(b, off, v & 0xFFFFu);
    gen_put16(b, off + 2, v >> 16);
}

static inline void gen_put64(uint8_t *b, uint64_t off, uint64_t v)
{
    gen_put32(b, off, (uint32_t)v);
    gen_put32(b, off + 4, (uint32_t)(v >> 32));
}

static inline uint32_t gen_get32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static inline uint64_t gen_get64(const uint8_t *b)
{
    return (uint64_t)gen_get32(b) | ((uint64_t)gen_get32(b + 4) << 32);
}

static inline uint64_t gen_addend(int a64, uint32_t slot)
{
    if (slot == 2)
        return a64 ? 0xFFFFFFFFFFFFFF00ull : 0xFFFFFF00u;
    if (slot == 1 && a64)
        return 0x100000010ull;
    return 0x1000u + 0x40u * slot;
}

/* Writes the image to `b` (at least GEN_CAP bytes) and describes it. */
static inline void gen_image(uint8_t *b, const struct gen *g, struct gen_out *o)
{
    const int a64 = g->a64;
    const int rela = a64 || g->rela;
    const int rel = !a64 && (g->rel || !g->rela);
    const uint32_t ehsz = a64 ? 64u : 52u, phsz = a64 ? 56u : 32u, dsz = a64 ? 16u : 8u;
    const uint32_t nrel = g->nrel ? g->nrel : 3u;
    const uint32_t bss = g->bss ? g->bss : 0x800u;
    const uint32_t dv = g->data_vaddr ? g->data_vaddr : 0x4000u;
    const uint32_t relent = 8u, relaent = a64 ? 24u : 12u;
    uint32_t note_off = (ehsz + 4u * phsz + 7u) & ~7u;
    uint32_t code_off = (note_off + 56u + 15u) & ~15u;
    uint32_t text_end = code_off + 32u;
    uint32_t doff = (text_end + 15u) & ~15u;
    uint32_t ntab = (uint32_t)(rel ? 1 : 0) + (uint32_t)(rela ? 1 : 0);
    uint32_t last_extra = g->bss_target ? 1u : 0u;
    uint32_t dynsz = (ntab * 3u + 1u) * dsz;
    uint32_t t0 = (dynsz + 7u) & ~7u;
    uint32_t n_rel = rel ? nrel + (rela ? 0u : last_extra) : 0u;
    uint32_t n_rela = rela ? nrel + last_extra : 0u;
    uint32_t t1 = (t0 + n_rel * relent + 7u) & ~7u;
    uint32_t words = (t1 + n_rela * relaent + 7u) & ~7u;
    uint32_t slots = (n_rel + n_rela);
    uint32_t dfile = words + 8u * slots;
    uint32_t i, k, ph = ehsz, d;
    uint32_t type_rel = a64 ? 1027u : 23u;

    memset(b, 0, GEN_CAP);
    memset(o, 0, sizeof(*o));
    /* ELF header */
    memcpy(b, "\x7f" "ELF", 4);
    b[4] = a64 ? 2 : 1;
    b[5] = 1;
    b[6] = 1;
    gen_put16(b, 16, 3);
    gen_put16(b, 18, a64 ? 183u : 40u);
    gen_put32(b, 20, 1);
    if (a64) {
        gen_put64(b, 24, code_off);
        gen_put64(b, 32, ehsz);
        gen_put32(b, 48, 0);
        gen_put16(b, 52, ehsz);
        gen_put16(b, 54, phsz);
        gen_put16(b, 56, g->no_note_segment ? 3u : 4u);
        gen_put16(b, 58, 64);
    } else {
        gen_put32(b, 24, code_off);
        gen_put32(b, 28, ehsz);
        gen_put32(b, 36, 0x05000400u);
        gen_put16(b, 40, ehsz);
        gen_put16(b, 42, phsz);
        gen_put16(b, 44, g->no_note_segment ? 3u : 4u);
        gen_put16(b, 46, 40);
    }
    /* Program headers: text, data, dynamic, note. */
#define PHDR(type, flags, off, vaddr, filesz, memsz, align)                  \
    do {                                                                     \
        if (a64) {                                                           \
            gen_put32(b, ph, type);                                          \
            gen_put32(b, ph + 4, flags);                                     \
            gen_put64(b, ph + 8, off);                                       \
            gen_put64(b, ph + 16, vaddr);                                    \
            gen_put64(b, ph + 24, vaddr);                                    \
            gen_put64(b, ph + 32, filesz);                                   \
            gen_put64(b, ph + 40, memsz);                                    \
            gen_put64(b, ph + 48, align);                                    \
        } else {                                                             \
            gen_put32(b, ph, type);                                          \
            gen_put32(b, ph + 4, off);                                       \
            gen_put32(b, ph + 8, vaddr);                                     \
            gen_put32(b, ph + 12, vaddr);                                    \
            gen_put32(b, ph + 16, filesz);                                   \
            gen_put32(b, ph + 20, memsz);                                    \
            gen_put32(b, ph + 24, flags);                                    \
            gen_put32(b, ph + 28, align);                                    \
        }                                                                    \
        ph += phsz;                                                          \
    } while (0)
    PHDR(1u, 5u, 0u, 0u, text_end, text_end + g->text_extra, 0x1000u);
    PHDR(1u, 6u, doff, dv, dfile, dfile + bss, 0x1000u);
    PHDR(2u, 6u, doff, dv, dynsz, dynsz, 8u);
    if (!g->no_note_segment)
        PHDR(4u, 4u, note_off, note_off, 56u, 56u, 4u);
#undef PHDR
    /* The note: owner "AAR", type 1, a 40-byte descriptor. */
    gen_put32(b, note_off, 4);
    gen_put32(b, note_off + 4, 40);
    gen_put32(b, note_off + 8, 1);
    memcpy(b + note_off + 12, "AAR", 4);
    gen_put32(b, note_off + 16, AAR_NOTE_MAGIC);
    gen_put32(b, note_off + 20, AAR_NOTE_VERSION);
    gen_put32(b, note_off + 24, a64 ? AAR_PROFILE_A64 : AAR_PROFILE_A32);
    gen_put32(b, note_off + 28, AAR_API_MAJOR);
    gen_put32(b, note_off + 32, AAR_API_MINOR);
    gen_put32(b, note_off + 36, 0);
    gen_put32(b, note_off + 40, 2);   /* min_heap_mib */
    gen_put32(b, note_off + 44, 1);   /* groups_required: display */
    gen_put32(b, note_off + 48, 4);   /* groups_optional: audio */
    gen_put32(b, note_off + 52, 0);
    /* Code: 32 recognisable bytes. */
    for (i = 0; i < 32u; i++)
        b[code_off + i] = (uint8_t)(0xC0u + i);
    /* The data segment. The dynamic table first. */
    d = doff;
    if (rel) {
        if (a64) {
            gen_put64(b, d, 17), gen_put64(b, d + 8, dv + t0), d += 16;
            gen_put64(b, d, 18), gen_put64(b, d + 8, n_rel * relent), d += 16;
            gen_put64(b, d, 19), gen_put64(b, d + 8, relent), d += 16;
        } else {
            gen_put32(b, d, 17), gen_put32(b, d + 4, dv + t0), d += 8;
            gen_put32(b, d, 18), gen_put32(b, d + 4, n_rel * relent), d += 8;
            gen_put32(b, d, 19), gen_put32(b, d + 4, relent), d += 8;
        }
    }
    if (rela) {
        if (a64) {
            gen_put64(b, d, 7), gen_put64(b, d + 8, dv + t1), d += 16;
            gen_put64(b, d, 8), gen_put64(b, d + 8, n_rela * relaent), d += 16;
            gen_put64(b, d, 9), gen_put64(b, d + 8, relaent), d += 16;
        } else {
            gen_put32(b, d, 7), gen_put32(b, d + 4, dv + t1), d += 8;
            gen_put32(b, d, 8), gen_put32(b, d + 4, n_rela * relaent), d += 8;
            gen_put32(b, d, 9), gen_put32(b, d + 4, relaent), d += 8;
        }
    }
    /* DT_NULL is the zero entry that memset left. */
    /* Slots and the entries that point at them. The last entry of the last
     * table aims into the bss when asked. */
    for (k = 0; k < slots; k++) {
        int in_rel = k < n_rel;
        uint32_t idx = in_rel ? k : k - n_rel;
        uint32_t table_entries = in_rel ? n_rel : n_rela;
        int is_bss = g->bss_target && idx + 1 == table_entries &&
                     (in_rel ? !rela : 1);
        uint64_t target = is_bss ? (uint64_t)dv + dfile + 8u
                                 : (uint64_t)dv + words + 8u * k;
        uint64_t addend = is_bss && in_rel ? 0 : gen_addend(a64, k);
        uint32_t ent_off = (in_rel ? t0 + idx * relent : t1 + idx * relaent) + doff;
        if (k == 0 && g->first_target)
            target = g->first_target;

        if (a64) {
            gen_put64(b, ent_off, target);
            gen_put64(b, ent_off + 8, type_rel);
            gen_put64(b, ent_off + 16, addend);
        } else {
            gen_put32(b, ent_off, (uint32_t)target);
            gen_put32(b, ent_off + 4, type_rel);
            if (!in_rel)
                gen_put32(b, ent_off + 8, (uint32_t)addend);
        }
        /* The slot in the file. */
        if (!is_bss) {
            uint32_t at = doff + words + 8u * k;

            if (a64) {
                gen_put64(b, at, 0xDEADBEEFDEADBEEFull);
            } else {
                gen_put32(b, at, in_rel ? (uint32_t)addend : GEN_PLACEHOLDER);
                gen_put32(b, at + 4, GEN_CANARY32);
            }
        }
        o->slot[o->nslots].vaddr = target;
        o->slot[o->nslots].addend = addend;
        o->slot[o->nslots].width = a64 ? 8u : 4u;
        o->nslots++;
    }
    o->len = (size_t)doff + dfile;
    o->entry = code_off;
    o->text_end = text_end;
    o->data_vaddr = dv;
    o->data_filesz = dfile;
    o->data_memsz = (uint64_t)dfile + bss;
    o->span = (uint64_t)dv + dfile + bss;
}

#endif /* AAR_LOADER_TEST_ELFGEN_H */
