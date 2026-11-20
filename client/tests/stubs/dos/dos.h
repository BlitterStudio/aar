/*
 * Host stand-in for the part of AmigaOS <dos/dos.h> that the aar client headers
 * name, for the tests only: the BCPL pointer type and the break signals.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DOS_DOS_H
#define DOS_DOS_H

#include <exec/types.h>

typedef LONG BPTR;

#define SIGBREAKB_CTRL_C 12
#define SIGBREAKB_CTRL_D 13
#define SIGBREAKB_CTRL_E 14
#define SIGBREAKB_CTRL_F 15
#define SIGBREAKF_CTRL_C (1UL << SIGBREAKB_CTRL_C)
#define SIGBREAKF_CTRL_D (1UL << SIGBREAKB_CTRL_D)
#define SIGBREAKF_CTRL_E (1UL << SIGBREAKB_CTRL_E)
#define SIGBREAKF_CTRL_F (1UL << SIGBREAKB_CTRL_F)

#endif
