/*
 * Host stand-in for AmigaOS <exec/ports.h>, for the tests only. struct Message
 * is laid out as on the 68k (20 bytes, 2-byte alignment, 32-bit pointers).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef EXEC_PORTS_H
#define EXEC_PORTS_H

#include <exec/types.h>

#pragma pack(push, 2)
struct Node {
    ULONG ln_Succ;
    ULONG ln_Pred;
    UBYTE ln_Type;
    BYTE  ln_Pri;
    ULONG ln_Name;
};

struct MsgPort;

struct Message {
    struct Node mn_Node;
    ULONG mn_ReplyPort; /* struct MsgPort * on the 68k */
    UWORD mn_Length;
};
#pragma pack(pop)

#endif
