/*
 * The image probe: reads the AAR note of an app image without loading it
 * (AARProbeFile and AARProbeMemory). It is portable C with no OS calls: the
 * AmigaOS shell supplies a file reader, a host test supplies memory.
 *
 * It applies the image rules of spec 2.2 and 2.3 as the loader does: the ELF
 * header of the profile, the program
 * headers, the PT_DYNAMIC relocation tables (only RELATIVE entries, every
 * target inside a PT_LOAD), the entry point, and the AAR note (exactly one,
 * in a PT_NOTE segment, with every descriptor rule of spec 2.3). An image the
 * loader accepts is accepted, with or without section headers; the rules that
 * read section headers belong to tools/aar-check, a lint for build output. The
 * rules themselves are the image rules in loader/aar_image.c, shared with the
 * reference loader (loader/aar_load.h). The probe and aar-check give the same
 * decision for every image that has its section headers and obeys the section
 * rules; client/tests/probe_corpus.py proves it on the corpus of
 * tests/aar_check_test.py. The note is judged against the API this library
 * was built with (AAR_API_MAJOR, AAR_API_MINOR), as aar-check judges it
 * against the SDK's header.
 *
 * It does not judge the platform: the profile, the groups, the heap and the
 * stack are reported, and a launcher compares them with what AARQuery returns.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_PROBE_H
#define AAR_PROBE_H

#include <stdint.h>

#include <libraries/aar.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A source of image bytes. Copies `count` bytes at `offset` into `dst`.
 * Returns AAR_OK, or a negative AAR_E_* (the probe returns AAR_E_IO). It is
 * only asked for bytes inside the length given to aar_probe_source, in reads
 * of at most 512 bytes for headers and table entries and sequentially, so a
 * file reader that seeks and reads is cheap enough.
 */
typedef int (*aar_probe_read_fn)(void *user, uint32_t offset, void *dst,
                                 uint32_t count);

/*
 * Probes an image of `length` bytes behind `read`. On AAR_OK, fills `info`
 * (see below). Returns:
 *   AAR_OK        the image passes every check; *info is filled
 *   AAR_E_INVAL   not an acceptable image: not a little-endian ELF32 or ELF64
 *                 file, a header or note rule broken (a note profile word that
 *                 is not the ELF class included), no note or more than one, a
 *                 bad relocation, no PT_DYNAMIC. The platform's profiles are
 *                 not judged, so AAR_E_PROFILE is never returned
 *   AAR_E_IO      `read` failed
 * `why`, when not NULL, receives a static string that says which rule
 * failed (NULL on success).
 *
 * `info->size` is the caller's size (at least 4, else AAR_E_INVAL). The call
 * fills min(info->size, sizeof(AARImageInfo)) bytes, the rest of what it
 * fills zero, and leaves `size` as it was. On failure `info` is untouched.
 */
int aar_probe_source(aar_probe_read_fn read, void *user, uint32_t length,
                     AARImageInfo *info, const char **why);

/* The same for an image in memory. `data` needs no alignment. */
int aar_probe_memory(const void *data, uint32_t length, AARImageInfo *info);

/* The same, with the reason. */
int aar_probe_memory_why(const void *data, uint32_t length, AARImageInfo *info,
                         const char **why);

#ifdef __cplusplus
}
#endif

#endif /* AAR_PROBE_H */
