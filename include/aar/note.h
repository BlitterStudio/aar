/*
 * The AAR ELF note, emitted from C. AAR_NOTE() at file scope adds the note
 * (spec 2.3, note version 1): owner name "AAR", n_type 1, a 40-byte
 * descriptor of ten little-endian uint32_t, in section .note.aar. The linker
 * script must place that section in a PT_NOTE segment; the runtime refuses an
 * image without exactly one AAR note.
 *
 *   AAR_NOTE(stack_bytes, min_heap_mib, groups_required, groups_optional)
 *   AAR_NOTE_EX(api_minor_required, stack_bytes, min_heap_mib,
 *               groups_required, groups_optional)
 *
 * stack_bytes     0 for the default (1 MiB); otherwise 64 KiB up to the
 *                 platform maximum, a multiple of 16.
 * min_heap_mib    private heap committed at load, in MiB; 0 for none.
 * groups_*        AAR_GROUP_* bits. The runtime fails the load with
 *                 AAR_E_NOGROUP when a required group is absent.
 * api_minor_required
 *                 the lowest API minor that has every call the app needs, from
 *                 AAR_API_MINOR_OLDEST to AAR_API_MINOR (a static assert).
 *                 AAR_NOTE() declares AAR_API_MINOR, the minor of this header.
 *
 * The profile word comes from the compiler: AAR_PROFILE_A64 when
 * __aarch64__ is defined, AAR_PROFILE_A32 for __arm__. The api_major word is
 * AAR_API_MAJOR. All arguments must be integer constant expressions.
 *
 * A host build (any compiler that is not an Arm target, or one compiled with
 * AAR_HOST defined) expands both macros to nothing. Write the macro at file
 * scope without a trailing semicolon.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_NOTE_H
#define AAR_NOTE_H

#include "app.h"

#if (defined(__arm__) || defined(__aarch64__)) && !defined(AAR_HOST)

#define AAR_NOTE_PASTE_(a, b) a##b
#define AAR_NOTE_PASTE(a, b) AAR_NOTE_PASTE_(a, b)

#if defined(__aarch64__)
#define AAR_NOTE_PROFILE_ AAR_PROFILE_A64
#else
#define AAR_NOTE_PROFILE_ AAR_PROFILE_A32
#endif

#define AAR_NOTE_KEEP_(id, minor, stack_bytes, min_heap_mib, groups_required, \
                       groups_optional)                                       \
    static void AAR_NOTE_PASTE(aar_note_keep_, id)(void)                      \
        __attribute__((used, noinline));                                      \
    static void AAR_NOTE_PASTE(aar_note_keep_, id)(void)                      \
    {                                                                         \
        AAR_STATIC_ASSERT((minor) >= AAR_API_MINOR_OLDEST,                    \
                          "api_minor_required is below AAR_API_MINOR_OLDEST");\
        AAR_STATIC_ASSERT((minor) <= AAR_API_MINOR,                           \
                          "api_minor_required is above AAR_API_MINOR");       \
        __asm__ __volatile__(                                                 \
            ".pushsection .note.aar, \"a\", %%note\n\t"                       \
            ".balign 4\n\t"                                                   \
            ".long 4\n\t"                                                     \
            ".long 40\n\t"                                                    \
            ".long 1\n\t"                                                     \
            ".asciz \"AAR\"\n\t"                                              \
            ".balign 4\n\t"                                                   \
            ".long 0x4E524141\n\t"                                            \
            ".long 1\n\t"                                                     \
            ".long %c0\n\t"                                                   \
            ".long %c1\n\t"                                                   \
            ".long %c2\n\t"                                                   \
            ".long %c3\n\t"                                                   \
            ".long %c4\n\t"                                                   \
            ".long %c5\n\t"                                                   \
            ".long %c6\n\t"                                                   \
            ".long 0\n\t"                                                     \
            ".popsection\n\t"                                                 \
            :                                                                 \
            : "i"((unsigned int)AAR_NOTE_PROFILE_),                           \
              "i"((unsigned int)AAR_API_MAJOR),                               \
              "i"((unsigned int)(minor)),                                     \
              "i"((unsigned int)(stack_bytes)),                               \
              "i"((unsigned int)(min_heap_mib)),                              \
              "i"((unsigned int)(groups_required)),                           \
              "i"((unsigned int)(groups_optional)));                          \
    }

#define AAR_NOTE_EX(api_minor_required, stack_bytes, min_heap_mib,            \
                    groups_required, groups_optional)                         \
    AAR_NOTE_KEEP_(__COUNTER__, api_minor_required, stack_bytes,              \
                   min_heap_mib, groups_required, groups_optional)

#define AAR_NOTE(stack_bytes, min_heap_mib, groups_required, groups_optional) \
    AAR_NOTE_EX(AAR_API_MINOR, stack_bytes, min_heap_mib, groups_required,    \
                groups_optional)

#else

#define AAR_NOTE_EX(api_minor_required, stack_bytes, min_heap_mib,            \
                    groups_required, groups_optional)
#define AAR_NOTE(stack_bytes, min_heap_mib, groups_required, groups_optional)

#endif

#endif
