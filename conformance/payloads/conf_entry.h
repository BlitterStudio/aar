/*
 * What the C-ENTRY payload (conf_entry.c) and the runner side (runner/r_entry.c)
 * agree on: the 16 long arguments of the variant `long` (C-ENTRY.4). Plain C
 * with no includes, so the m68k runner and the Arm payload can both use it.
 *
 * The variant passes 16 arguments of 127 bytes each, argv[0] included, which is
 * the least a runtime must support (spec 3.2). argv[0] is a 127-byte path whose
 * last component is the payload's file name; argument i (1 to 15) holds, at byte
 * j, the character conf_entry_long_char(i, j): printable, no space, a different
 * pattern per argument, so a swap, a shift or a dropped byte shows.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_ENTRY_H
#define AAR_CONF_ENTRY_H

#define CONF_ENTRY_LONG_ARGS  16u
#define CONF_ENTRY_LONG_BYTES 127u
#define CONF_ENTRY_FILE       "conf_entry.elf"

static inline char conf_entry_long_char(unsigned i, unsigned j)
{
    return (char)(0x21u + ((i * 37u + j * 11u) % 94u));
}

#endif
