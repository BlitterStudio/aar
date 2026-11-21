# aar-run

`aar-run` is the reference launcher of the AAR standard (spec section 9.6). It
loads one trusted app through `aar.library` and serves it until it ends: files,
console, display, audio and input. It is a Shell command and a Workbench tool,
and it runs on any platform that has an `aar.library`. Nothing in it belongs to
one board; the ZZ9000 is the first platform behind the library, and what is
specific to it is in [Platform notes](#platform-notes).

aar-run is MIT licensed and is built from this repository (`client/run` and
`client/serve`). It was called ZZRun while it lived with the ZZ9000 drivers.
The rename is complete and no old name is kept: the program, its icon, the
`PROGDIR:aar-run` icon lookup, the `CON:` window title, the Picasso96 screen
title and the requester titles all say `aar-run`, and there is no `ZZRun`
alias or tool type.

It needs `aar.library` (`LIBS:aar.library`, installed with aar-run), AmigaOS 3
graphics and Intuition 39 or later, `timer.device`, Picasso96 when the app asks
for a display and `ahi.device` for `AUDIO=AHI`. Without the library it prints
`aar.library not found.` and returns 20; with a library that has no platform
behind it, it prints `no AAR platform found` (or that `AARQuery` failed) and
returns 20.

Every instance claims its own app, so many apps can run at once; the platform's
resources are the limit. When the platform cannot admit another one, a launch
reports `claim refused:` with the reason (not enough memory, no translation
tables left, not enough shared memory for the queues) and returns 20.

## Starting an app

Run `Stack 32768` in the launching Shell first. aar-run requires at least a
16 KiB stack and prints `Run Stack 32768 in this Shell before aar-run.`
(returning 10) when it has less. For a Shell command the stack that counts is
the CLI's default stack (what the `Stack` command sets, `cli_DefaultStack`), also
when the task's own stack bounds are larger; a Workbench start uses the icon's
stack.

```text
aar-run <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [WEIGHT=<n>] [AUDIO=AHI] [args]
```

Give the ELF first. The app's `argv[0]` is the ELF's file name and the
remaining Shell tokens follow it, as C programs expect. `AUDIO=AHI`,
`MAXMEM=<MiB>`, `HOSTMEM=<MiB>`, `WEIGHT=<n>` and the two diagnostic words
`POLL=<us>` and `DISPLAY=<...>` are recognized anywhere after the ELF, so they
cannot be passed to the app as arguments with those exact forms. `WEIGHT`,
`POLL` and `DISPLAY` are matched without regard to case.

```text
Stack 32768
aar-run RAM:swquake.elf +timedemo demo1
```

### What the platform takes

aar-run does not assume what a platform takes. It reads the limits from
`AARQuery` (`AARInfo`) and checks the launch against them before it claims
anything. A launch the platform would refuse prints the limit and returns 10:

| Setting | Limit from `AARInfo` | Line when exceeded |
| --- | --- | --- |
| app arguments | `max_args`, which counts `argv[0]` (0: no limit) | `this platform takes at most <n> arguments for the app.` |
| one argument | `max_arg_bytes`, which counts the NUL (0: no limit) | `an argument is longer than <n> bytes, the most this platform takes.` |
| `WEIGHT` | `weight_max` (0: the platform does not weight) | `WEIGHT must be 1 to <n> on this platform.` or `WEIGHT is not available: this platform does not weight apps.` |
| `HOSTMEM` | `shared_max_mib`, never above 4095, the rule of `AAR_TAG_SHARED_MIB` | `HOSTMEM must be 1 to <n> MiB on this platform.` |
| the ELF | `max_image_bytes` | reported by the load (`larger than the <n> MiB image limit`) |

`MAXMEM`, `HOSTMEM` and `WEIGHT` must be positive decimal numbers; anything
else prints the usage and returns 10. The library refuses an out of range tag
value in any case, so these checks only change where the message appears and
that no claim is taken first.

`MAXMEM=<MiB>` caps the private memory committed by this app.

```text
aar-run RAM:swquake.elf MAXMEM=256 +timedemo demo1
```

`HOSTMEM=<MiB>` sets how much shared (host-visible) memory this app may
allocate: the memory the Amiga can address through the platform, including the
buffers the runtime allocates for the app's console, files and audio. Without
it the app gets the platform default (`shared_default_mib`). The limit is a
cap, not a reservation. Both limits go to the library before the load; the
platform checks them at load and refuses what it cannot grant. aar-run then
prints `load refused:` with the reason (a platform limit means `HOSTMEM` or
`MAXMEM` is too large) and returns 20.

```text
aar-run RAM:swquake.elf HOSTMEM=32
```

`WEIGHT=<n>` sets the app's share of the CPU when several apps run at once.
The platform gives the CPU to the runnable app that has used the least time in
proportion to its weight, so an app with `WEIGHT=2` gets about twice the run
time of an app with `WEIGHT=1` while both want to run. An app that is blocked
or sleeping does not save up credit. Without it the app gets the platform's
default weight. The weight is given to the library with the memory limits and
lasts until the claim ends.

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

### Checks before a claim

aar-run reads the image's AAR note with `AARProbeFile` and compares it with the
platform before `AAROpen`, so an image that cannot run takes no claim. The line
names the cause and the exit code is 20:

| Cause | Line |
| --- | --- |
| the file cannot be opened or read | `cannot open <file>`, `cannot read <file>` |
| not an AAR image | `<file> is not an AAR image (no valid AAR ELF note, or one built for another API than this library)` |
| wrong profile, for example an A64 image on an A32 platform | `<file> is an A64 app; this platform runs A32 apps only.` |
| API too new for the platform | `<file> needs AAR API 0.5, this platform has 0.3.` |
| a required service group the platform lacks | `<file> needs service groups this platform lacks: gl.` |

The profile is checked first, then the API, then the groups the image requires;
the groups it lists as optional are only a hint and never refuse an image. The
library's probe cannot tell a note newer than the library from a damaged one,
which is why the not-an-AAR-image line names both; a platform older than its
library is the API line. A file name with a volume or assign colon may be a
multi-directory assign, which the library's own open does not search: if the
first probe finds nothing there, aar-run finds the file the way it does for the
load and probes the full name then. That needs the serving loop and so a claim,
which aar-run opens and closes again when it refuses the image. Every other
refusal, and a missing plain path, happens before any claim.

### Exit codes

| Code | Meaning |
| --- | --- |
| the app's | on normal app exit, aar-run returns the app exit code (0 to 255) |
| 5 | Ctrl-C while aar-run opens, probes or loads the ELF, including when a DOS packet wait such as a file read or closing the ELF consumed the break |
| 10 | invalid launcher arguments, a stack below 16 KiB, a missing `APP` tool type, or a launch the platform's limits refuse |
| 20 | an app fault, a watchdog, lease loss or any other end the platform imposed; an unavailable runtime or no `aar.library`; a refused claim, image or load; a revoked claim; a display failure; a console that cannot be opened or written; an AHI route that cannot be opened; `STATUS` or `RELEASE` where the platform does not support them; any other launcher error |

Lease expiry prints `aar-run: claim lost (lease expired).` and quarantine prints
`aar-run: claim lost (quarantined).` rather than a normal app exit. If a call
reports a stale handle and the final status is no longer available, aar-run
reports `claim lost (stale handle; final status unavailable)` without guessing
why the claim was revoked.

## Runtime status

```text
aar-run STATUS
```

`STATUS` is case-insensitive. It prints one line for each claim the platform
holds: its handle, state, app name, committed MiB, CPU seconds, block reason
and end reason. The block reason is `none`, `file`, `console`, `event`,
`sleep`, `vsync`, or `audio`. `event` means the app is waiting for a message.

`STATUS` and `RELEASE` go through `AARExt`, the library's platform extension
table. Where `AARExt` returns no table (a platform that does not offer the
claim list) they print `aar-run: STATUS is not supported on this platform.`
(or `RELEASE`) and return 20. Launching an app never needs the extension.

A crashed aar-run does not leave a claim behind: `aar.library` notices that the
task is gone within about a second, stops the app and releases the claim. A
claim the library could not release (its own helper stopped, or the platform
revoked it) keeps its control block and buffers until it is released, because
the old launcher may still write through its mapping. `STATUS` shows it as
ended, with end reason `lease loss` or `quarantined`. When nothing owns it any
more, release it by its handle:

```text
aar-run RELEASE <handle>
```

The handle is the hexadecimal number `STATUS` prints, written as `$` or `0x`
followed by the digits, or in decimal. `RELEASE` ends a claim that is finished
(an app that ended or faulted, or a claim the platform revoked) and refuses one
whose app is running, loading or being claimed. A platform reset also clears
every claim.

## Workbench ToolTypes

A Workbench start reads the ToolTypes of the aar-run icon (`PROGDIR:aar-run`,
or the icon of the tool a project icon names). Dragging an ELF project onto the
icon, or giving a project icon whose default tool is `aar-run`, takes
precedence over `APP`. A Workbench start has no Shell: the ELF comes from the
project icon or `APP`, and without either aar-run requests `Set the APP
tooltype, or start aar-run from a project icon.` and returns 10.
The project directory lock is resolved with `NameFromLock`: any nonzero DOS
BOOLEAN result is success, including `DOSTRUE` (-1). The project name is then
appended to that directory, without adding a slash after a volume colon.

| ToolType | Meaning |
| --- | --- |
| `APP` | ELF path when aar-run is started without a project file. |
| `ROOT` | Root directory for relative app file paths. |
| `ABS` | Enabled when the ToolType has no value, is exactly `1` or `y`, or begins with `ye` or `t` (case-insensitive). Permits app paths containing an Amiga volume or assign colon. The root check does not apply to those paths. |
| `MODE` | Optional Picasso96 DisplayID, in decimal, `$` hexadecimal, or `0x` hexadecimal. The app still chooses width, height, and RGB format. |
| `ARGS` | Optional space-separated arguments. Quoting is not parsed. |
| `JOYPORT0` | Enabled by the same values as `ABS`. Enables joystick port 0 as well as port 1. |
| `AUDIO=AHI` | Route app PCM to AHI. The default remains the platform's own audio path. |
| `MAXMEM=<MiB>` | Positive decimal MiB cap on the app's private memory (`AAR_TAG_PRIVATE_MIB`). |
| `HOSTMEM=<MiB>` | Positive decimal MiB shared memory limit (`AAR_TAG_SHARED_MIB`), up to the platform's `shared_max_mib` and 4095. |
| `WEIGHT=<n>` | Decimal CPU share weight from 1 to the platform's `weight_max` (`AAR_TAG_WEIGHT`). |

The same limits and refusals apply as in a Shell launch. The diagnostic words
below are not ToolTypes. A Workbench start opens one console window titled
`aar-run` for aar-run's lines and the app's console: see
[Console and audio routes](#console-and-audio-routes).

## Diagnostics

Two words of the command line exist so that the loop and the display paths can
be measured and forced. They are recognized after the ELF, in any case, and are
not passed to the app. A bad value prints the usage and returns 10.

| Word | Effect |
| --- | --- |
| `POLL=<us>` | The heartbeat of the loop, in microseconds. The default is 1000 (one wake-up per millisecond, the latency the launcher has always had). `POLL=20000` suits a program that is happy with a vertical-blank wake-up; `POLL=0` turns the timer off and waits on the library's signal alone (the library wakes the loop within about one vertical blank). The value is below 1000000. A slower heartbeat costs latency for DOS and audio work, not correctness. |
| `DISPLAY=<BUFFERS\|PAN3\|PAN2\|SINGLE>` | The display strategy aar-run starts with, and the fallbacks after it. `BUFFERS` (the default) uses screen buffers, then a three page tall screen, a two page one and one buffer. `PAN3` starts at the three page tall screen, `PAN2` at the two page one, and `SINGLE` uses one buffer the platform copies each frame. A strategy the screen cannot open falls back to the next one, as in the default order. |

```text
aar-run RAM:swquake.elf POLL=20000 DISPLAY=PAN2
```

## Platform notes

**Any platform.** aar-run needs the library to implement the API major it was
built for (`AAR_API_MAJOR`) and reads everything else from `AARInfo`:
the limits above, `max_image_bytes`, `stop_grace_ms` (the grace the loop gives
an app after a stop, read by `aar_serve`), the supported `profiles` and
`groups`. A launch the platform cannot run is refused before it claims anything.

**ZZ9000.** The first platform behind `aar.library`. It needs a Zorro III board
with firmware that has the AAR queue transport (app ABI 0.5), `zz9k.library`
revision 36 or later (only to reach the board's mailbox) and `mmu.library` 41
or later, because the board's shared buffers must be mapped cache-inhibited.
The platform takes 16 strings of 127 bytes (so 15 app arguments), a `WEIGHT`
from 1 to 256, a `HOSTMEM` up to 4095 MiB and a 16 MiB shared default. aar-run
prints `caps <hex>` after the `runtime api` line, from the platform words of
`AARInfo`, and a note when the board is not a Zorro III board (the library then
decides at the claim). `STATUS` and `RELEASE` are available through the
library's ZZ9000 extension (`libraries/aar_zz9000.h`). Without the board or the
firmware the library has no platform and aar-run says so and returns 20. The
firmware SDK package ships `C/aar-run`, its icon and `Docs/aar-run.guide`.

## How aar-run talks to the platform

aar-run calls `aar.library`, the resident AAR client library of the platform
(`LIBS:aar.library`). The library implements the AAR transport contract
(`aar/transport.h`): it owns the claim, the control block and queue pair, the
doorbell, the cache-inhibited maps and the lease. aar-run links none of that.
Every claim has its own control block and queue pair, and the platform names
each buffer it wants aar-run to use in the request that needs it.

1. aar-run opens `aar.library` (version 0) and calls `AARQuery`. Without a
   platform behind the library the query fails and aar-run stops before
   claiming. It checks that the platform implements the API major it was built
   for, reads the limits it needs from `AARInfo` and checks the launch against
   them. A `aar-run STATUS` or `RELEASE` needs only the query and `AARExt`.
2. `AARProbeFile` reads the image's note, and aar-run compares it with the
   platform (profile, API, required groups). A refusal prints its cause and
   takes no claim.
3. `AAROpen` admits an app. The library returns a handle that belongs to this
   task; a refusal says why (`claim refused:` plus the reason).
4. `aar_serve_create` builds the serving state for the handle: the timer, the
   ROOT lock and the log sink. `AARSetTagList` carries `MAXMEM`, `HOSTMEM` and
   `WEIGHT` (only those the launch set), what aar-run can serve (CLUT8 screens,
   pan acknowledgement, platform-owned flips), the longer lease, and the AHI
   audio route. The library checks the list as a whole and refuses what it
   cannot grant.
5. aar-run finds the ELF itself, from the current directory or through every
   `GetDeviceProc` candidate of an assign (`aar_serve_locate`), locks it and
   turns the lock into a full path with `NameFromLock`. `AARLoadFile` opens and
   reads that path in chunks and has the platform validate and relocate the
   image. A stalled device holds up the load (the lease does not depend on it),
   and Ctrl-C is honoured between 64 KiB chunks. A refusal prints `load
   refused:` with the reason, as the library reports it in the status (a
   platform limit means `HOSTMEM` or `MAXMEM` is too large). An empty or
   oversized image is refused by the library against the platform's image
   limit.
6. `AARStart` takes the argument strings. They travel in the load buffer.
7. `aar_serve_run` is the main loop. It wakes every millisecond (`POLL`) and
   each pass forwards input, serves the requests the app has posted
   (`AARServiceNext` and `AARServiceReply`), completes the ones that were
   waiting for DOS, the display or AHI, and reads the app's state and end
   reason with a small `AARStatus`. aar-run does not renew the lease: the
   library does that, so a requester or a long DOS wait in aar-run does not
   cost the app its claim. A completion is followed by a doorbell the library
   sends, but the queue indices stay authoritative: a lost doorbell costs
   latency, never data.
8. Ctrl-C from the Shell or an app-display raw key requests a stop: aar-run
   posts one stop event (retried until the queue has room for it) and calls
   `AARStop`, which writes the control block's stop request, posts the stop
   event and sends the platform's stop. Ctrl-D, Ctrl-E, and Ctrl-F on the app
   display remain normal raw-key events and additionally post a break event
   with codes 4, 5, and 6. The app display has one borderless input window, not
   a close gadget; its raw keys are forwarded to the app, including Ctrl-C and
   the app's own quit controls. aar-run remains alive for the platform's stop
   grace period (`AARInfo.stop_grace_ms`) so the platform can publish the final
   state. Ctrl-C ends aar-run after that grace, plus a second one, even if
   console output is still queued. It closes the custom screen and returns the
   public screen even after a fault.
9. When the app ends, aar-run reads the exit code, end reason and fault record
   with `AARStatus`, prints them, and closes the claim with `AARClose`. The
   close returns the control block, the queues and every buffer to the
   platform. If the platform revokes the claim first (lease lost), every call
   answers `claim revoked`, aar-run stops serving and returns 20.

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

The platform posts the app's log records on the submission queue. aar-run stages them
and its own runtime status through the same asynchronous writer as app console
output. If a stalled destination fills the bounded staging queue, aar-run drops
diagnostic log records but reserves space for launcher status and errors. The
runtime itself drops a log record when its submission queue is full and counts
it (`AARStatusInfo.log_dropped`). After the run, once the pending writes have
finished, aar-run prints one line, `log records not shown: <n> dropped by the
runtime (its queue was full), <m> by the launcher's console queue.`, when
either count is not zero.
For `AUDIO=AHI`, the summary also prints the ring's underrun count (summed over
streams) and the heartbeat. AHI requires `POLL` between 1 and 20000 us; the
default is 1000 us.
aar-run does not write to stderr while the app is running.

aar-run takes the app's messages itself: the serving loop drains them with
`AARGetMessage`, drops them and counts them, so an app that sends more than the
library's inbox holds cannot hang itself behind an unread message. After the
run aar-run prints `dropped <n> app messages.` when it dropped any. Apps that
exchange custom messages need a client that takes them: the SDK echo and
ringtest tools, or a program of its own that creates `aar_serve` with
`AAR_SERVEF_KEEP_MESSAGES` (`docs/aar-serve-api.md`).

### Console and audio routes

The app's console output arrives as `CONSOLE_WRITE` requests naming bytes in a
console buffer the platform allocated; a request completes when the DOS write has
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
so the prompt is not hidden behind the app display. A Workbench launch opens one
closable `CON:` window titled `aar-run` when it starts, and aar-run's own lines
and the app's console output both go to it; aar-run does not write through the C
library's stdout, so no second, untitled window appears. If that window cannot be
opened, aar-run opens it at the app's first console use instead. If writing app
output fails, aar-run stops the app and returns 20.

The default audio route is the platform's own audio path and needs nothing from aar-run. Set
`AUDIO=AHI` in the app icon or pass `AUDIO=AHI` after the ELF in a Shell
command to request the AHI route; it is passed to the library as the audio
route tag before the load. On that route the platform allocates the audio ring and
names it in an `AUDIO_OPEN` request; aar-run opens AHI at the requested rate
(stereo), and the library attaches the ring when aar-run completes the request
and detaches it at `AUDIO_CLOSE`. aar-run reads the PCM with `AARAudioRead`,
which converts the ring's interleaved S16LE samples (rate and channel count in
its header) into AHI's S16BE bounce buffers without resampling or downmixing,
one request of at most 1/50 s (20 ms) at a time. aar-run keeps six such requests
queued in AHI (about 120 ms of audio, each chained to the one before it) and
refills every free slot in one pass while whole chunks wait in the ring, so a
delay of the loop of up to about 100 ms, such as a low priority task on a busy
machine, plays out of AHI's queue. The price is up to 120 ms of latency between
the app's ring and the speakers when the app writes ahead of the player; a ring
that fills in real time adds none, because the last, partial chunk is queued as
soon as it is 5 ms long or AHI has nothing left to play. If AHI cannot be opened, aar-run stops
the app and returns 20. Before each read aar-run asks the library how many frames
are readable (`AARAudioRead` with no buffer), which counts nothing, so an
underrun in the ring header is counted only when AHI finished a buffer and
found the ring empty.

Output to `NIL:` works. A console handle with no handler port, which is what
`NIL:` is, cannot take the asynchronous DOS packet the loop uses for other
destinations. aar-run writes such a handle with the synchronous `Write()`
(dos.library answers it by itself and it cannot block) and completes the app's
request on the next pass of the loop, so `aar-run app.elf >NIL:` runs the app
to its end, discards its text and returns its exit code. Input from `NIL:`
ends at once, which is the end of input the app sees. aar-run's own notes (the
output-failure note and the finish prompts) go to the user's console, never
only into the redirected output. A write that really fails, such as a full
volume or a closed pipe, still stops the app and returns 20.

When the app opens its display, the platform posts `DISPLAY_OPEN` with the width,
height and format. RGB565 is the first path; CLUT8 is supported and aar-run loads
the palette the app posts (`DISPLAY_PALETTE`); BGRA8888 is accepted when P96
returns that format. aar-run opens the P96 screen and completes the request with
the bitmap's actual pitch and the tokens of its memory (the platform's own
addresses, opaque to aar-run).
With two displayable P96 bitmaps (`AllocScreenBuffer`), each flip request
changes screen buffer and completes when P96 has finished with the old one. If
P96 refuses screen buffers, aar-run first opens a bitmap three times the
requested height and pans between its pages. If that fails, it retries with a
double-height bitmap. A pan completes with the submitted flag: the platform
acknowledges the flip after its own next vertical blank has latched the new
address, which keeps the old front unavailable until scanout has stopped reading
it. Only if both panning layouts fail does it use one bitmap, which the platform
copies each frame into. When the app asks for latest-frame flips, the platform owns
every flip and aar-run sees none: it does not pan or change buffers. The
`DISPLAY=` diagnostic word chooses where this walk starts.

### Files and ROOT

File requests (`FILE_OPEN`, `FILE_READ`, `FILE_WRITE`, `FILE_SEEK`,
`FILE_STAT`, `FILE_CLOSE`, `LIST_OPEN`, `LIST_NEXT`, `LIST_CLOSE`) carry paths
and data in buffers the platform names. The app has up to 16 open files and 16 open
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
