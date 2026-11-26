/*
 * What a C++ app needs from its start-up code and that crtbegin.o supplies in
 * a normal link. mk/aar.mk and cmake/aar.cmake link this file into every app
 * that has a C++ source (libc flavour only), as an object, not through
 * libaar.a, so the constructor below is always present.
 *
 * __dso_handle is the handle global constructors pass to __cxa_atexit. An app
 * is one image with no shared objects, so 0 is the value crtbegin.o gives it
 * in a static link.
 *
 * On A64 the unwinder finds frame descriptions through a registry, because
 * there is no dl_iterate_phdr. The linker script libaar/a64/aar-cxx.ld puts
 * .eh_frame between __eh_frame_start and a zero word, and a .preinit_array
 * entry registers it. crt0 runs .preinit_array after it binds the table (so
 * malloc works) and before every .init_array entry, whatever priority a
 * constructor of the app has, so a constructor that throws and catches finds
 * its frames. On A32 the unwinder reads .ARM.exidx between __exidx_start and
 * __exidx_end and needs nothing here.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>

__attribute__((visibility("hidden"))) void *__dso_handle = NULL;

#if defined(__aarch64__)

extern const char __eh_frame_start[] __attribute__((visibility("hidden")));
extern void __register_frame(void *begin);

static void aar_cxx_register_frames(void)
{
    __register_frame((void *)__eh_frame_start);
}

__attribute__((section(".preinit_array"), used))
static void (*aar_cxx_preinit)(void) = aar_cxx_register_frames;

#endif
