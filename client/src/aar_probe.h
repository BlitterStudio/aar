/*
 * The image probe: reads the AAR note of an app image without loading it
 * (AARProbeFile and AARProbeMemory). It is portable C with no OS calls: the
 * AmigaOS shell supplies a file reader, a host test supplies memory.
 *
 * What it checks is what tools/aar-check checks of an image, rule for rule: the
 * ELF header of the profile, the program headers, the PT_DYNAMIC and section
 * relocation tables (only RELATIVE entries are allowed), the entry point, and
 * the AAR note (exactly one, in a PT_NOTE segment and in the .note.aar
 * section, with every descriptor rule of spec 2.3). The rules themselves are
 * the image rules of the AAR standard in loader/aar_image.c, shared with the
 * reference loader (loader/aar_load.h); the probe asks for the section checks
 * as well (AAR_IMAGE_SECTIONS), which is what aar-check does. The probe and
 * aar-check give the same decision for every image; client/tests/probe_corpus.py
 * proves it on the corpus of tests/aar_check_test.py. The note is judged
 * against the API this library was built with (AAR_API_MAJOR, AAR_API_MINOR),
 * as aar-check judges it against the SDK's header.
 *
 * It does not judge the platform: the profile is reported, and a launcher
 * compares it and the group masks with what AARQuery returns.
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
 *   AAR_OK      the image passes every check; *info is filled
 *   AAR_E_INVAL not an acceptable image: not a little-endian ELF32 or ELF64
 *               file, a header or note rule broken, no note or more than one,
 *               a bad relocation. The profile is not judged, so AAR_E_PROFILE
 *               is never returned
 *   AAR_E_IO    `read` failed
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
