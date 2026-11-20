/*
 * Host stand-in for AmigaOS <exec/types.h>, for the tests only. It gives the
 * library headers the 68k word sizes: ULONG, LONG and APTR are 32 bits, so
 * every structure has the layout it has on the Amiga, whatever the host's
 * pointer size. The real header comes from the NDK on the Amiga.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

#include <stdint.h>

typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint16_t UWORD;
typedef int16_t WORD;
typedef uint8_t UBYTE;
typedef int8_t BYTE;
typedef uint32_t APTR;

#endif
