/*
 * The reference loader of the AAR standard (spec 2.2 and 2.3): the code a
 * platform takes to accept an app image and put it in memory. MIT, portable C99,
 * no OS calls, no allocation, both profiles (A32 and A64) in one build. Take
 * the four files of this directory (aar_image.[ch], aar_load.[ch]) and the
 * headers of include/aar; nothing else is needed.
 *
 * Two calls. aar_load_plan reads the image through a callback, applies every
 * rule of spec 2.2 and 2.3 (the rules live in aar_image.c) and returns a plan:
 * the profile, the memory the image needs and how it is aligned, its segments
 * with the permissions to map them with, its relocation tables, the entry
 * point and the fields of the AAR note. The platform checks the plan against
 * what it can run (the groups, the heap, the stack) and reserves the memory.
 * aar_load_place then copies the segments there, zeroes the rest, and applies
 * the relocations. The platform enters the app after that, as spec 3.2 says.
 *
 * Every size and offset in the file is untrusted. The arithmetic does not
 * wrap, every read goes through the callback and stays inside the image, and
 * aar_load_place writes nowhere outside the memory the platform offers. The
 * platform does the rest of what loading means: it maps the segments with the
 * permissions of the plan, makes the instruction and data caches agree over
 * the code, and starts the app.
 *
 * Only RELATIVE relocations exist (R_ARM_RELATIVE as REL or RELA on A32,
 * R_AARCH64_RELATIVE as RELA on A64), and they come from PT_DYNAMIC. Section
 * headers may be absent (spec 2.2 rule 4); this loader never reads them.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_LOAD_H
#define AAR_LOAD_H

#include <stdint.h>

#include "aar_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A source of image bytes: copies `count` bytes at `offset` into `dst` and
 * returns AAR_OK, or a negative AAR_E_* when it cannot (the loader then
 * returns AAR_E_IO). The loader asks only for bytes inside the length it was
 * given. aar_load_plan reads headers and tables in pieces of at most 512
 * bytes; aar_load_place reads whole segments, in pieces of at most 256 MiB.
 */
typedef aar_image_read_fn aar_load_read_fn;

/* Permissions of a segment (aar_load_segment.flags). Every segment is readable. */
#define AAR_LOAD_R 1u
#define AAR_LOAD_W 2u
#define AAR_LOAD_X 4u

#define AAR_LOAD_MAX_SEGMENTS AAR_IMAGE_MAX_LOADS
#define AAR_LOAD_MAX_RELOCS   AAR_IMAGE_MAX_TABLES

/*
 * One segment to place. The image bytes [offset, offset + filesz) go to
 * [vaddr, vaddr + filesz) of the app's memory and the bytes up to vaddr + memsz
 * are zero. vaddr is relative to the base the app runs at. A segment that has
 * no memory (memsz 0) is not in the plan. The segments do not overlap, none is
 * both writable and executable, and filesz is at most memsz.
 */
struct aar_load_segment {
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t vaddr;
    uint32_t flags; /* AAR_LOAD_R | AAR_LOAD_W | AAR_LOAD_X */
};

/*
 * A relocation table, as aar_load_place applies it. `offset` and `size` are the
 * table's bytes in the image (inside the image), `count` the number of entries,
 * `entsize` the entry size (8: A32 REL, 12: A32 RELA, 24: A64 RELA) and `rela`
 * 1 for a RELA table (the addend is in the entry) and 0 for a REL table (the
 * addend is the word at the target). Every entry is RELATIVE with symbol 0, the
 * target is aligned (4 bytes on A32, 8 on A64), and the target lies inside a
 * segment. An image may have no table.
 */
struct aar_load_reloc {
    uint64_t offset;
    uint64_t size;
    uint64_t count;
    uint32_t entsize;
    uint32_t rela;
};

struct aar_load_plan {
    uint32_t profile;          /* AAR_PROFILE_A32 or AAR_PROFILE_A64 */
    uint64_t length;           /* the image length the plan was made from */

    /* Memory. The app needs `span` bytes from its base, and the base a
     * multiple of `align`. `span` covers every segment up to the end of its
     * memsz (so the bss), it is not rounded to a page. `align` is the
     * largest p_align of a segment that is a power of two, and 1 when the
     * image says none. A platform that maps pages rounds span up and uses at
     * least its page size as alignment. Nothing in the standard bounds either
     * number: compare span with the memory offered. */
    uint64_t span;
    uint64_t align;

    /* Where the app starts: an offset from its base, inside a segment with
     * AAR_LOAD_X, even on A32. */
    uint64_t entry_offset;

    uint32_t nsegments;        /* 1 to AAR_LOAD_MAX_SEGMENTS */
    struct aar_load_segment seg[AAR_LOAD_MAX_SEGMENTS];

    uint32_t nrelocs;          /* 0 to AAR_LOAD_MAX_RELOCS */
    struct aar_load_reloc reloc[AAR_LOAD_MAX_RELOCS];

    /* The AAR note (spec 2.3). The loader has judged the note's own rules
     * (magic, version, profile, API major and minor, stack size, group bits);
     * whether the platform has the groups and the heap is the platform's. */
    uint32_t note_version;
    uint32_t api_major;
    uint32_t api_minor_required;
    uint32_t stack_bytes;      /* 0: the default of 1 MiB */
    uint32_t min_heap_mib;
    uint32_t groups_required;  /* AAR_GROUP_* bits: AAR_E_NOGROUP when missing */
    uint32_t groups_optional;
};

/*
 * Reads the image of `length` bytes behind `read` and applies every rule of
 * spec 2.2 and 2.3. `profiles` is the set of AAR_PROFILE_* values the platform
 * runs, or-ed together (AAR_PROFILE_A32 | AAR_PROFILE_A64 for both). Returns
 *   AAR_OK         *plan is filled
 *   AAR_E_PROFILE  the image is acceptable, but its profile is not in `profiles`
 *                  (the platform answers AAR_E_PROFILE), or the note names a
 *                  profile that does not match the ELF class
 *   AAR_E_INVAL    a rule is broken: not a little-endian ELF32 or ELF64 image,
 *                  a header, segment, relocation or note rule, or a segment
 *                  that ends beyond the 64-bit address space
 *   AAR_E_IO       `read` failed
 * The rules are checked before the profile: an image that breaks a rule is
 * AAR_E_INVAL (or the note's AAR_E_PROFILE) whatever `profiles` is. `why`, when
 * not NULL, receives a static string that names the rule (NULL on success).
 * `length` is below 2^62. On failure *plan is unspecified.
 */
int aar_load_plan(aar_load_read_fn read, void *user, uint64_t length,
                  uint32_t profiles, struct aar_load_plan *plan,
                  const char **why);

/*
 * Puts the image of `plan` in memory. The platform has made the plan with
 * aar_load_plan from the same image and reserved memory for it:
 *   write_base   where this call writes: `limit_bytes` writable bytes, at least
 *                plan->span of them. A runtime often writes through one
 *                mapping and runs the app at another, so this need not be
 *                where the app runs.
 *   app_base     the address the app runs at. The relocations are made against
 *                it: every RELATIVE target becomes app_base plus its addend.
 *                It should be a multiple of plan->align. On A32 the image must
 *                end below 4 GiB (app_base + span at most 2^32).
 *   limit_bytes  the size of the memory at write_base. The loader writes
 *                nothing at or beyond write_base + limit_bytes.
 * The whole of [write_base, write_base + span) is written: the bytes of the
 * segments, zero for the bss and for the gaps between segments, then the
 * relocations. The platform makes the instruction and data caches agree over
 * the code before it enters the app, and applies the permissions of the
 * segments when it maps them. On AAR_OK, *entry is app_base + entry_offset.
 * Returns
 *   AAR_OK         placed
 *   AAR_E_NOMEM    plan->span does not fit in limit_bytes (or in the host's
 *                  address space): nothing was written
 *   AAR_E_INVAL    the plan is not one aar_load_plan makes (a segment outside
 *                  the span or the image, overlapping segments, a bad
 *                  relocation entry), or the image does not fit the address
 *                  space at app_base
 *   AAR_E_IO       `read` failed
 * After AAR_E_INVAL or AAR_E_IO the memory holds a partly loaded image and
 * must not be used. `why` is as in aar_load_plan.
 */
int aar_load_place(const struct aar_load_plan *plan, aar_load_read_fn read,
                   void *user, void *write_base, uint64_t app_base,
                   uint64_t limit_bytes, uint64_t *entry, const char **why);

#ifdef __cplusplus
}
#endif

#endif /* AAR_LOAD_H */
