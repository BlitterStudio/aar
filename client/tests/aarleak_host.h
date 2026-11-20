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
void Forbid(void);
void Permit(void);
struct Process *CreateNewProcTags(ULONG tag, ...);

/* A function address in a tag value is 32 bits on the Amiga and not on the host:
 * the model hands out an identifier and finds the function again. A name is not
 * used by the model. */
ULONG aarleak_fn_id(void (*fn)(void));
#define AARLEAK_FN(f) aarleak_fn_id((void (*)(void))(f))
#define AARLEAK_STR(s) 0u

/* An APTR is 32 bits on the Amiga and not on the host: the model hands out an
 * identifier here as well. */
const void *aarleak_ptr(APTR id);
#define AARLEAK_PTR(p) aarleak_ptr(p)

#endif /* AARLEAK_HOST_H */
