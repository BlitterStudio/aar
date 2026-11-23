/*
 * What the C-MEM payload (conf_mem.c) and the runner side (runner/r_mem.c) agree
 * on. Plain macros and a static inline function, no includes, so the m68k runner
 * and the Arm payload can both use it.
 *
 * Variants (the runner passes conf:variant=<name>):
 *   main     C-MEM.1 to C-MEM.8, C-MEM.10, C-MEM.11, C-MEM.13. Sends CONF-MEM-LIMIT
 *            with heap_info.limit, which the runner passes on to `caps`
 *   caps     C-MEM.9. The runner sets AAR_TAG_PRIVATE_MIB to CONF_MEM_CAPS_MIB
 *            and passes conf:caps_mib=<that value> and conf:cap_mib=<the limit
 *            that `main` saw, in MiB, 0 when unknown>
 *   lowmem   C-MEM.12. Exhausts the pool twice and counts AAR_EV_LOW_MEMORY
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_MEM_H
#define AAR_CONF_MEM_H

/* Peer message: this keyword in the first bytes, then at byte 16 the value
 * heap_info.limit as a little-endian 64-bit number. */
#define CONF_MSG_MEM_LIMIT "CONF-MEM-LIMIT"
#define CONF_MEM_LIMIT_OFFSET 16u

/* The private-memory cap the runner sets in the variant `caps`, MiB. More than
 * the 2 MiB of the payload's note, so the load succeeds. */
#define CONF_MEM_CAPS_MIB 4u

/* Little-endian 32-bit word at byte offset `off` of a message. */
static inline unsigned long conf_mem_le32(const unsigned char *b, unsigned off)
{
    return (unsigned long)b[off] | ((unsigned long)b[off + 1] << 8) |
           ((unsigned long)b[off + 2] << 16) | ((unsigned long)b[off + 3] << 24);
}

#endif
