# aar-run

`aar-run` loads a trusted ARM app into the ZZ9000 app runtime. It needs a
Zorro III board, firmware with the AAR queue transport (app ABI 0.5), and
Picasso96 when the app asks for a display. It calls the app runtime through
`aar.library` (`LIBS:aar.library`, installed with aar-run), which needs
`zz9k.library` revision 36 or later (only to reach the board's mailbox) and
`mmu.library` 41 or later, because the card's shared buffers must be mapped
cache-inhibited. Without the library it prints `aar.library not found.` and
returns 20; without the board or the firmware it prints `aar-run requires a
Zorro III board and app-runtime firmware.` (or that `zz9k.library` or the
firmware was not found) and returns 20.

The launcher is a Shell command and a Workbench tool. Every instance claims its
own app, so many apps can run at once; the card's shared memory is the limit.
When the card cannot admit another one, a launch reports `claim refused:` with
the reason (not enough card memory, no translation tables left, not enough
shared memory for the queues) and returns 20.

## Starting an app

Run `Stack 32768` in the launching Shell first. aar-run requires at least a
16 KiB process stack.

```text
aar-run <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [WEIGHT=<1-256>] [AUDIO=AHI] [args]
```

Give the ELF first. The app's `argv[0]` is the ELF's file name and the
remaining Shell tokens follow it, as C programs expect (up to 15 arguments).
`AUDIO=AHI`, `MAXMEM=<MiB>`, `HOSTMEM=<MiB>`, and `WEIGHT=<n>` are recognized
anywhere after the ELF, so they cannot be passed to the app as arguments with
those exact forms.

```text
Stack 32768
aar-run RAM:swquake.elf +timedemo demo1
```

`MAXMEM=<MiB>` caps the card memory committed by this app. The value must be a
positive decimal MiB count. For example:

```text
aar-run RAM:swquake.elf MAXMEM=256 +timedemo demo1
```

`HOSTMEM=<MiB>` sets how much shared (host-visible) card memory this app may
allocate: the memory the Amiga can address through the board, including the
buffers the runtime allocates for the app's console, files and audio. The value
must be a positive decimal MiB count up to 4095; anything else prints the
usage and returns 10. Without it the app gets the firmware default of 16 MiB.
The limit is a cap, not a reservation. Both limits go to the library before the
load; the card checks them at load and refuses what it cannot grant. aar-run then
prints `load refused:` with the reason (a platform limit means `HOSTMEM` or
`MAXMEM` is too large) and returns 20.

```text
aar-run RAM:swquake.elf HOSTMEM=32
```

`WEIGHT=<n>` sets the app's share of the ARM CPU when several apps run at
once. The card gives the CPU to the runnable app that has used the least time
in proportion to its weight, so an app with `WEIGHT=2` gets about twice the run
time of an app with `WEIGHT=1` while both want to run. An app that is blocked
or sleeping does not save up credit. The value is a decimal number from 1 to
256; anything else prints the usage and returns 10 before aar-run claims
anything. The key is matched without regard to case (`weight=2` works). Without
it the app gets weight 1. The weight is given to the library with the memory
limits and lasts until the claim ends.

```text
aar-run RAM:swquake.elf WEIGHT=4
```

`ROOT` controls files requested by the app. A Shell launch defaults to the
current directory. A Workbench launch defaults to the directory holding the
aar-run icon. For Quake, set `ROOT` to the directory containing `id1/`.

A relative ELF path is opened from aar-run's current AmigaDOS directory. A path
with a volume or assign prefix is resolved through `GetDeviceProc`; on a
multi-directory assign, aar-run tries each directory after an
`ERROR_OBJECT_NOT_FOUND` result. aar-run hands the library the full path of the
file it found.

On normal app exit, aar-run returns the app exit code. It returns 20 after an
app fault, a watchdog, lease loss or any other end the platform imposed, an
unavailable runtime, a revoked claim, display failure, a console that cannot be
opened or written, an AHI route that cannot be opened, or another launcher
error. It returns 10 for invalid launcher arguments. Ctrl-C while aar-run opens
or loads the ELF stops the load and returns 5, including when a DOS packet
wait such as a file read or closing the ELF consumed the break. Other load
failures return 20.

Lease expiry prints `aar-run: claim lost (lease expired).` and quarantine prints
`aar-run: claim lost (quarantined).` rather than a normal app exit. If a call
reports a stale handle and the final status is no longer available, aar-run
reports `claim lost (stale handle; final status unavailable)` without guessing
why the claim was revoked.

## Runtime status

```text
aar-run STATUS
```

`STATUS` is case-insensitive. It prints one line for each claim the card
holds: its handle, state, app name, committed MiB, CPU seconds, block reason
and end reason. The block reason is `none`, `file`, `console`, `event`,
`sleep`, `vsync`, or `audio`. `event` means the app is waiting for a message.

A crashed aar-run does not leave a claim behind: `aar.library` notices that the
task is gone within about a second, stops the app and releases the claim. A
claim the library could not release (its own helper stopped, or the card
revoked it) keeps its control block and buffers on the card until it is
released, because the old launcher may still write through its mapping.
`STATUS` shows it as ended, with end reason `lease loss` or `quarantined`.
When nothing owns it any more, release it by its handle:

```text
aar-run RELEASE <handle>
```

The handle is the hexadecimal number `STATUS` prints, written as `$` or `0x`
followed by the digits, or in decimal. `RELEASE` ends a claim that is finished
(an app that ended or faulted, or a claim the card revoked) and refuses one
whose app is running, loading or being claimed. A card reset also clears every
claim.

## Workbench ToolTypes

| ToolType | Meaning |
| --- | --- |
| `APP` | ELF path when aar-run is started without a project file. |
| `ROOT` | Root directory for relative app file paths. |
| `ABS` | Enabled when the ToolType has no value, is exactly `1` or `y`, or begins with `ye` or `t` (case-insensitive). Permits app paths containing an Amiga volume or assign colon. The root check does not apply to those paths. |
| `MODE` | Optional Picasso96 DisplayID, in decimal, `$` hexadecimal, or `0x` hexadecimal. The app still chooses width, height, and RGB format. |
| `ARGS` | Optional space-separated arguments. Quoting is not parsed. |
| `JOYPORT0` | Enabled by the same values as `ABS`. Enables joystick port 0 as well as port 1. |
| `AUDIO=AHI` | Route app PCM to AHI. The default remains the card audio fabric. |
| `MAXMEM=<MiB>` | Positive decimal MiB cap on the app's private memory (`AAR_TAG_PRIVATE_MIB`). |
| `HOSTMEM=<MiB>` | Positive decimal MiB (1 to 4095) shared memory limit (`AAR_TAG_SHARED_MIB`). |
| `WEIGHT=<n>` | Decimal CPU share weight from 1 to 256 (`AAR_TAG_WEIGHT`). |

Dragging an ELF project onto the aar-run icon takes precedence over `APP`.

## How aar-run talks to the card

aar-run calls `aar.library`, the resident AAR client library for this board
(`LIBS:aar.library`, installed with aar-run). The library implements the AAR
transport contract (`aar/transport.h`): it owns the claim, the control block
and queue pair, the mailbox, the cache-inhibited maps and the lease. aar-run
links none of that. Every claim has its own control block and queue pair, and
the card names each buffer it wants aar-run to use in the request that needs it.

1. aar-run opens `aar.library` (version 0) and calls `AARQuery`. The library
   reaches the board through `zz9k.library` and needs `mmu.library` only when a
   claim is opened. aar-run checks that the platform is a ZZ9000 with a Zorro III
   board and the API major it was built for, and that it takes the 16 argument
   strings of 127 bytes aar-run passes. Without a platform (no board, an old
   `zz9k.library`, firmware without the app runtime) the query fails and aar-run
   stops before claiming. A `aar-run STATUS` or `RELEASE` needs only the query
   and the library's ZZ9000 client extension.
2. `AAROpen` admits an app. The library returns a handle that belongs to this
   task; a refusal says why (`claim refused:` plus the reason).
3. `AARSetTagList` carries `MAXMEM`, `HOSTMEM` and `WEIGHT` (only those the
   launch set), what aar-run can serve (CLUT8 screens, pan acknowledgement,
   card-owned flips), the longer lease, and the AHI audio route. The library
   checks the list as a whole and refuses what it cannot grant.
4. aar-run finds the ELF itself, from the current directory or through every
   `GetDeviceProc` candidate of an assign, locks it and turns the lock into a
   full path with `NameFromLock`. `AARLoadFile` opens and reads that path in
   chunks and has the card validate and relocate the image. A stalled device
   holds up the load (the lease does not depend on it), and Ctrl-C is honoured
   between 64 KiB chunks. A refusal prints `load refused:` with the reason, as
   the library reports it in the status (a platform limit means `HOSTMEM` or
   `MAXMEM` is too large). An empty or oversized image is refused by the
   library against the platform's image limit.
5. `AARStart` takes the argument strings. They travel in the load buffer;
   strings are capped at 127 bytes plus NUL.
6. The main loop wakes every millisecond. Each pass it forwards input,
   serves the requests the app has posted (`AARServiceNext` and
   `AARServiceReply`), completes the ones that were waiting for DOS, the
   display or AHI, and reads the app's state and end reason with a small
   `AARStatus`. aar-run does not renew the lease: the library does that from a
   vertical-blank interrupt, so a requester or a long DOS wait in aar-run does
   not cost the app its claim. A completion is followed by a doorbell the
   library sends, but the queue indices stay authoritative: a lost doorbell
   costs latency, never data.
7. Ctrl-C from the Shell or an app-display raw key requests a stop: aar-run
   posts one stop event (retried until the queue has room for it) and calls
   `AARStop`, which writes the control block's stop request, posts the stop
   event and sends the platform's stop. Ctrl-D, Ctrl-E, and
   Ctrl-F on the app display remain normal raw-key events and additionally
   post a break event with codes 4, 5, and 6. The app display has one
   borderless input window, not a close gadget; its raw keys are forwarded to
   the app, including Ctrl-C and the app's own quit controls. aar-run remains
   alive for the 500 ms grace period so the card can publish the final state.
   Ctrl-C ends aar-run after that grace, plus a second 500 ms, even if console
   output is still queued. It closes the custom screen and returns the public
   screen even after a fault.
8. When the app ends, aar-run reads the exit code, end reason and fault record
   with `AARStatus`, prints them, and closes the claim with `AARClose`. The
   close returns the control block, the queues and every buffer to the card.
   If the card revokes the claim first (lease lost), every call answers `claim
   revoked`, aar-run stops serving and returns 20.

All words in the control block and in queue entries are big-endian. The
`aar/` headers are the layout authority; `libraries/aar.h` and
`libraries/aar_zz9000.h` are the library's.

### Input, display, and logs

aar-run forwards IDCMP raw keys without converting them to ASCII. A key-up has
`AAR_EV_KEY_UP`; its raw key code has the Amiga up bit removed. Key qualifiers
stay in `buttons`. Mouse movement carries deltas, and mouse buttons use bits
0, 1, and 2 for left, right, and middle. Joystick events report one fire
button in bit 0 and `dx`/`dy` in `-1, 0, 1`. aar-run reads joystick port 1 only
while its display window is active; `JOYPORT0=1` also enables port 0. It only
reads `JOYxDAT` and the CIA-A fire bits and never drives the port pins, so the
mouse and joystick ports are left exactly as it found them.

Events travel on the completion queue, which holds 64 entries shared with
completions and messages. The library sums consecutive mouse motion into one
pending event and flushes it, so a flood costs one entry; every other event
keeps its order behind the motion that came before it. An event that finds the
queue full is dropped and counted in the control block's events-dropped word,
except the stop event, which aar-run retries until it is delivered.

The card posts the app's log records on the submission queue. aar-run stages them
and its own runtime status through the same asynchronous writer as app console
output. If a stalled destination fills the bounded staging queue, aar-run drops
diagnostic log records but reserves space for launcher status and errors; after
pending writes finish during cleanup it reports the number of dropped records.
aar-run does not write to stderr while the app is running.

aar-run has no app-message consumer. The library holds unread app messages
without dropping them. Once its inbox fills, a message stays at the
submission queue head and requests behind it wait too. Apps exchanging custom
messages need a client that takes them, such as the SDK echo and ringtest tools.

### Console and audio routes

The app's console output arrives as `CONSOLE_WRITE` requests naming bytes in a
console buffer the card allocated; a request completes when the DOS write has
finished, with the number of bytes written. A Shell launch writes those bytes
through `Output()` and takes app stdin from `Input()`. A `CONSOLE_READ` request
is the app's demand for input; aar-run starts one DOS read for it and completes it
with the bytes read, or with 0 at the end of input. A redirected input (`<file`
or a pipe) ends the app's stdin at the end of that stream. From the keyboard,
the app gets whatever ends a `Type *` in the same console; on OS 3.2.3 with the
standard or CCON handler Ctrl-\ does not, so stop such an app with Ctrl-C.
aar-run starts a read only while the app it owns is running and has asked for
input. If the app is waiting for keyboard input when it stops, aar-run writes
`press Return to finish.` to the console that owns that read (the Workbench
`CON:` window, or `Input()`), not to redirected stdout: the console cannot
cancel the line read it has open. A pipe or other non-interactive input uses
the same asynchronous read, so an open writer cannot stall the lease, display,
or audio. A regular file completes immediately. If a non-interactive read is
still pending at exit, aar-run writes `waiting for pending input to finish.` to
the user console (not stdout, and not the pipe) and waits, because a pipe read
cannot be cancelled. The custom screen is closed before either of those waits,
so the prompt is not hidden behind the app display. A Workbench launch opens
no console by default; only when the app first uses console I/O, aar-run opens a
closable `CON:` window. If opening that requested console fails, or if writing
app output fails, aar-run stops the app and returns 20.

The default audio route is the card fabric and needs nothing from aar-run. Set
`AUDIO=AHI` in the app icon or pass `AUDIO=AHI` after the ELF in a Shell
command to request the AHI route; it is passed to the library as the audio
route tag before the load. On that route the card allocates the audio ring and
names it in an `AUDIO_OPEN` request; aar-run opens AHI at the requested rate
(stereo), and the library attaches the ring when aar-run completes the request
and detaches it at `AUDIO_CLOSE`. aar-run reads the PCM with `AARAudioRead`,
which converts the ring's interleaved S16LE samples (rate and channel count in
its header) into AHI's S16BE bounce buffers without resampling or downmixing,
one buffer of at most 1/50 s per pass. If AHI cannot be opened, aar-run stops
the app and returns 20. Before each read aar-run asks the library how many frames
are readable (`AARAudioRead` with no buffer), which counts nothing, so an
underrun in the ring header is counted only when AHI finished a buffer and
found the ring empty.

Known limitation: running an app with stdout redirected to `NIL:` (`>NIL:`)
made swquake's file seeks fail on the card with every aar-run build tested,
while `>file` works. This is under investigation.

When the app opens its display, the card posts `DISPLAY_OPEN` with the width,
height and format. RGB565 is the first path; CLUT8 is supported and aar-run loads
the palette the app posts (`DISPLAY_PALETTE`); BGRA8888 is accepted when P96
returns that format. aar-run opens the P96 screen and completes the request with
the bitmap's actual pitch and the tokens of its memory, which are board offsets.
With two displayable P96 bitmaps (`AllocScreenBuffer`), each flip request
changes screen buffer and completes when P96 has finished with the old one. If
P96 refuses screen buffers, aar-run first opens a bitmap three times the
requested height and pans between its pages. If that fails, it retries with a
double-height bitmap. A pan completes with the submitted flag: the card
acknowledges the flip after its own next vertical blank has latched the new
address, which keeps the old front unavailable until scanout has stopped reading
it. Only if both panning layouts fail does it use one bitmap, which the card
copies each frame into. When the app asks for latest-frame flips, the card owns
every flip and aar-run sees none: it does not pan or change buffers.

### Files and ROOT

File requests (`FILE_OPEN`, `FILE_READ`, `FILE_WRITE`, `FILE_SEEK`,
`FILE_STAT`, `FILE_CLOSE`, `LIST_OPEN`, `LIST_NEXT`, `LIST_CLOSE`) carry paths
and data in buffers the card names. The app has up to 16 open files and 16 open
directory lists. A short read is end of file. Raw AmigaDOS Examine packets
return length-prefixed BCPL names; aar-run converts them to NUL-terminated
names in the request buffer before posting the list completion. The end of a
list is `ENOENT`.

The open request carries the AAR file flags: `READ` 1, `WRITE` 2, `CREATE` 4,
`TRUNCATE` 8, and `EXCL` 16. aar-run refuses an invalid combination with
`EINVAL` (-2) before it touches AmigaDOS: neither `READ` nor `WRITE`, `CREATE`
or `TRUNCATE` without `WRITE`, `EXCL` without `CREATE`, or an unknown bit.
Valid combinations open as follows.

- `READ`: `MODE_OLDFILE`; a missing file is `ENOENT` (-5).
- `WRITE` or `READ|WRITE`: the file must exist (aar-run probes with `Lock`,
  because `MODE_READWRITE` creates a missing file); then `MODE_READWRITE`. A
  missing file is `ENOENT`.
- `WRITE|TRUNCATE`: as above, then the file is set to size 0 with
  `ACTION_SET_FILE_SIZE`. A handler without that action gets `MODE_NEWFILE`
  after the probe.
- `WRITE|CREATE`: `MODE_READWRITE` on an existing file, keeping its contents;
  `MODE_NEWFILE` if it is missing.
- `WRITE|CREATE|TRUNCATE`: `MODE_NEWFILE`; create, or replace an existing file.
- `WRITE|CREATE|EXCL`, with or without `TRUNCATE`: aar-run probes with `Lock`. An
  existing file is `EEXIST` (-11), including one that another program holds
  open exclusively (the handler refuses the lock with `ERROR_OBJECT_IN_USE`),
  for relative and `ABS` paths alike; otherwise `MODE_NEWFILE`.

`READ` may be added to any of the `WRITE` entries. AmigaDOS has no atomic exclusive
create, so `EXCL` is exclusive against the other apps aar-run serves, which share
one serving loop, but not against other Amiga programs that create the same name
between the probe and the open.

With `ABS` on a multi-directory assign, the probe finds the directory that holds
the file and the open, `SetFileSize` and `MODE_NEWFILE` fallback all go to that
same directory, so an existing file is updated, truncated, kept or replaced
where it is rather than shadowed by a new file in the first directory. Every
step that can create, truncate or replace a file is skipped, and answers `EIO`,
once the request is no longer wanted: the claim was revoked, a Ctrl-C or stop is
pending, or an earlier wait for AmigaDOS was abandoned.

Relative paths stay below `ROOT`. `..`, leading `/`, and empty path components
that would leave the root are rejected, and so is a link whose target lies
outside it. Paths containing `:` are rejected unless `ABS` is enabled. With
`ABS`, file open, stat, and directory-list requests use `GetDeviceProc` and try
later multi-directory assign candidates after `ERROR_OBJECT_NOT_FOUND`. This is
path hygiene for an app that is already trusted; it is not an isolation
boundary. File reads and writes are chunked at 8 KiB so one request does not
hold the serving loop for long. A stalled AmigaDOS volume can still block its
DOS call; the claim stays alive meanwhile, because the library renews the
lease.

The launcher reports file modification times as Unix seconds derived from an
Amiga `DateStamp`, or zero when the time is absent.

## Faults and cleanup

A data abort, prefetch abort, undefined instruction or alignment fault prints
the captured pc, sp, lr, fault address, fault syndrome, program status, exit
code and the handle's generation, then returns 20. A watchdog, a runtime
restart or a lost lease is reported the same way and returns 20. A forced stop
caused by Ctrl-C returns the app's normal exit code when it has one. Every exit
path closes opened files and directory handles, restores the public screen,
closes the claim (`AARClose`, which stops an app that still runs and releases
it) and releases its Amiga-side resources. The library inhibits caching on the
shared buffers it maps and does not restore the previous cache mode; those
pages stay cache-inhibited.

## Building

aar-run is built from this repository (`client/run` and `client/serve`, MIT).
The firmware SDK build compiles and packages it next to `Libs/aar.library`. It
is three files and the library headers:

```sh
m68k-amigaos-gcc -std=gnu11 -noixemul -m68030 -O2 -Wall -Wextra \
    -Iclient/include -Iinclude \
    client/run/aar_run.c client/serve/aar_serve.c client/serve/aar_serve_core.c \
    -lamiga -o aar-run
```

`sh client/tests/m68k_check.sh` does this with the pinned cross compiler image
and fails on a warning. aar-run calls the library through its vectors, so it
links no transport code, and it does not include the ZZ9000 private wire
(`zzapp_proto.h`). The Workbench icon `aar-run.info` is generated by
`python3 client/run/make_icon.py` (`--check` tells whether the file is current).

The host tests (`sh client/tests/run_host.sh`) cover the request handlers
(`client/tests/serve_core_test.c`), the loop's decisions
(`client/tests/serve_policy_test.c`) and the launcher's (`client/tests/run_policy_test.c`).
They run without an Amiga.
