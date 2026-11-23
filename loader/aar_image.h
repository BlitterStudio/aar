/*
 * The image rules of the AAR standard: spec 2.2 (ELF rules) and 2.3 (the AAR
 * note), for both profiles in one build. This is the one place that decides
 * whether a file is an acceptable app image. The loader (aar_load.h) builds on
 * it, and the image probe of aar.library (client/src/aar_probe.c) is a thin
 * layer over it. tools/aar-check stays the independent second implementation:
 * client/tests/probe_corpus.py and loader/tests/loader_corpus.py prove that the
 * two decide the same on a corpus and on mutations of it.
 *
 * Portable C99 with no OS calls and no allocation (the SDK header <aar/app.h>
 * it includes has C11's _Static_assert, which GCC and Clang take in C99 mode).
 * Every size and offset in
 * the file is untrusted: the arithmetic is on uint64_t with the range checked
 * before every add, so nothing wraps, and every byte is read through the
 * callback, which is only asked for bytes inside the image.
 *
 * The note is judged against the API this build of the headers has
 * (AAR_API_MAJOR, AAR_API_MINOR in <aar/app.h>), as aar-check judges it
 * against the SDK's header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_IMAGE_H
#define AAR_IMAGE_H

#include <stdint.h>

#include <aar/app.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A source of image bytes: copies `count` bytes at `offset` into `dst`.
 * Returns AAR_OK, or a negative AAR_E_* when the source fails (the check then
 * returns AAR_E_IO). aar_image_check only asks for bytes inside the length it
 * was given, `count` is between 1 and 512, and the reads of a header or table
 * walk forward, so a file reader that seeks and reads is cheap enough.
 */
typedef int (*aar_image_read_fn)(void *user, uint64_t offset, void *dst,
                                 uint32_t count);

#define AAR_IMAGE_MAX_LOADS  4u /* spec 2.2 rule 3: at most 4 PT_LOAD */
#define AAR_IMAGE_MAX_TABLES 2u /* a DT_REL table and a DT_RELA table (A32) */

/* aar_image_check flags */

/*
 * Also require what tools/aar-check requires and the spec only allows: the
 * section headers are present, the relocation sections and dynamic sections
 * they list obey the same rules as the tables of PT_DYNAMIC, and the note is
 * inside the one .note.aar section. Spec 2.2 rule 4 says the loader reads
 * PT_DYNAMIC only and section headers MAY be absent, so a loader does not set
 * this. The image probe of aar.library sets it, to give aar-check's verdict.
 */
#define AAR_IMAGE_SECTIONS 0x1u

/* One PT_LOAD segment. `flags` are the ELF p_flags (PF_X 1, PF_W 2, PF_R 4). */
struct aar_image_load {
    uint64_t vaddr;  /* p_vaddr */
    uint64_t offset; /* p_offset: where the file bytes are */
    uint64_t filesz; /* p_filesz */
    uint64_t memsz;  /* p_memsz, at least filesz; the rest is zero-filled */
    uint64_t align;  /* p_align as the file has it (0 and 1: none) */
    uint32_t flags;
};

/*
 * One RELATIVE relocation table named by PT_DYNAMIC. It lies inside the image
 * (file offset `offset`), `size` is a multiple of `entsize`, and every one of
 * the `count` entries is RELATIVE with symbol 0, aligned, and targets a
 * PT_LOAD.
 */
struct aar_image_table {
    uint64_t offset;
    uint64_t size;
    uint64_t count;
    uint32_t entsize; /* 8 (A32 REL), 12 (A32 RELA) or 24 (A64 RELA) */
    uint32_t rela;    /* 1 for RELA (addend in the entry), 0 for REL (in the target word) */
};

/* What an accepted image says. */
struct aar_image_info {
    uint32_t profile;            /* AAR_PROFILE_A32 or AAR_PROFILE_A64 */
    uint32_t note_version;       /* the note, spec 2.3 words 1 and 3 to 8 */
    uint32_t api_major;
    uint32_t api_minor_required;
    uint32_t stack_bytes;        /* 0: the default */
    uint32_t min_heap_mib;
    uint32_t groups_required;
    uint32_t groups_optional;
    uint64_t entry;              /* e_entry: inside an executable PT_LOAD */
    uint32_t nloads;             /* 1 to AAR_IMAGE_MAX_LOADS */
    struct aar_image_load load[AAR_IMAGE_MAX_LOADS];
    uint32_t ntables;            /* 0 to AAR_IMAGE_MAX_TABLES */
    struct aar_image_table table[AAR_IMAGE_MAX_TABLES];
};

/*
 * Applies every rule of spec 2.2 and 2.3 to the image of `length` bytes behind
 * `read` (`length` below 2^62). Returns
 *   AAR_OK         the image passes; *info is filled
 *   AAR_E_PROFILE  the note names a profile that is not the one of the ELF
 *                  class (spec 2.3: the profile word)
 *   AAR_E_INVAL    any other rule is broken: not a little-endian ELF32 or
 *                  ELF64 file, a header, segment, relocation or note rule, no
 *                  note or more than one
 *   AAR_E_IO       `read` failed
 * The profile itself is not judged against a platform; aar_load_plan does that.
 * `why`, when not NULL, receives a static string that names the rule that
 * failed (NULL on success). When the call fails, the contents of *info are
 * unspecified.
 */
int aar_image_check(aar_image_read_fn read, void *user, uint64_t length,
                    uint32_t flags, struct aar_image_info *info,
                    const char **why);

#ifdef __cplusplus
}
#endif

#endif /* AAR_IMAGE_H */
