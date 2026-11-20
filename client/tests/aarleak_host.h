/*
 * The AmigaOS calls the leak probe (client/tests/amiga/aarleak.c) makes, as
 * declarations for the host test (client/tests/aarleak_test.c), which defines
 * them as a small model of processes. Only what the probe uses.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AARLEAK_HOST_H
#define AARLEAK_HOST_H

#include <stdint.h>

#include <exec/libraries.h>
#include <exec/types.h>
#include <utility/tagitem.h>

typedef uint32_t BPTR;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

struct MsgPort;
struct Process;

#define MEMF_ANY 0u
#define MODE_OLDFILE 1005
#define OFFSET_END 1
#define OFFSET_BEGINNING (-1)
#define NP_Entry 1u
#define NP_Name 2u
#define NP_StackSize 3u
#define NP_CloseInput 4u
#define NP_CloseOutput 5u

ULONG AvailMem(ULONG attributes);
BPTR Open(STRPTR name, LONG mode);
LONG Seek(BPTR file, LONG position, LONG mode);
LONG Read(BPTR file, void *buffer, LONG length);
LONG Close(BPTR file);
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version);
void CloseLibrary(struct Library *library);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *port);
void Delay(ULONG ticks);
struct Process *CreateNewProcTags(ULONG tag, ...);

/* A function address in a tag value, which is 32 bits on the Amiga. */
#define AARLEAK_FN(f) ((ULONG)(uintptr_t)(f))

#endif /* AARLEAK_HOST_H */
