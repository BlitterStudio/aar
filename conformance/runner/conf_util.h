/*
 * Helpers that several area files of the runner share (r_elf.c, r_client.c,
 * r_life.c, r_shared.c): paths of the payloads and of the image corpus, reading
 * a file, starting an app by hand (no serving loop) and waiting for its end,
 * counting a task's free signal bits, and a child process.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_UTIL_H
#define AAR_CONF_UTIL_H

#include "aar-conf.h"

/* The exit code of every accepted image of conformance/negative (gen_negative.py). */
#define CONF_IMAGE_EXIT 3L

/* <DIR>/<profile>/<file>: a payload. 0 when the path does not fit. */
int conf_util_payload_path(char *out, ULONG size, const char *file);
/* <DIR>/negative/<file>: the MANIFEST. */
int conf_util_negative_path(char *out, ULONG size, const char *file);
/* <DIR>/negative/<profile>/<name>.aarimg: an image of the corpus. The profile is
 * "a32" or "a64". */
int conf_util_image_path(char *out, ULONG size, const char *profile, const char *name);

/* The whole file in AllocVec memory, followed by a NUL byte that is not counted in
 * *len (so an empty file gives one byte): 0, or -1
 * when it cannot be opened and -2 when it cannot be read. Free with FreeVec. */
LONG conf_util_read_file(const char *path, UBYTE **data, ULONG *len);

/* What an app did, read from AARStatus until it ended. */
struct conf_util_end {
    LONG status_rc;            /* the last AARStatus result */
    int ended;                 /* state IDLE or FAULTED with an end reason */
    int timed_out;
    ULONG state, end_reason, fault_class;
    LONG exit_code;
    ULONG waited_ticks;        /* 1/50 s from the call to the end */
    AARStatusInfo st;          /* the last status */
};

/* Polls AARStatus every 20 ms until the app has ended or timeout_ticks have
 * passed. Does not serve: use it for apps that make no service request. */
void conf_util_wait_end(AARHandle *h, ULONG timeout_ticks, struct conf_util_end *e);

/* AAROpen (no notify port), AARLoadFile and AARStart. On a failure the handle is
 * closed, *h is NULL and the failing call's result is returned. */
LONG conf_util_start_file(AARHandle **h, const char *path,
                          const char *const *argv, ULONG argc);
/* The same for the payload <DIR>/<profile>/<file> started with argv[0] = name and
 * the arguments conf:variant=<variant> and, when ms is not 0, conf:ms=<ms>.
 * The argument strings live in static buffers until the next call. */
LONG conf_util_start_payload(AARHandle **h, const char *file, const char *variant,
                             ULONG ms);
/* The same with a notify port for AAROpen. */
LONG conf_util_start_file_port(AARHandle **h, struct MsgPort *port, const char *path,
                               const char *const *argv, ULONG argc);
LONG conf_util_start_payload_port(AARHandle **h, struct MsgPort *port, const char *file,
                                  const char *variant, ULONG ms);

/* The number of signal bits this task can still allocate. */
int conf_util_free_signals(void);

/* A child process that runs fn. Returns 0 when it cannot be created. The child
 * reports to the caller through variables of its own (conf_util_wait_flag waits
 * for one). It has 16 KiB of stack, no libc stdio and no Shell. */
int conf_util_spawn(const char *name, void (*fn)(void));
/* Waits until *flag is not 0, for at most seconds. Returns the flag. */
int conf_util_wait_flag(volatile LONG *flag, ULONG seconds);

#endif
