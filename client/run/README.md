# aar-run

`aar-run` is the reference launcher of the AAR standard (spec section 9.6). It
loads one trusted app through `aar.library` and serves it until it ends: files,
console, display, audio and input. It is a Shell command and a Workbench tool,
and it runs on any platform that has an `aar.library`. Nothing in it belongs to
one board. The ZZ9000 is the first platform behind the library, and what is
specific to it is in [Platform notes](#platform-notes).

aar-run is MIT licensed and is built from this repository (`client/run` and
`client/serve`). Its program name, icon, `PROGDIR:aar-run` icon lookup, `CON:`
window title, Picasso96 screen title and requester titles all say `aar-run`.

It needs `aar.library` (`LIBS:aar.library`, installed with aar-run), AmigaOS 3
graphics and Intuition 39 or later, `timer.device`, Picasso96 when the app asks
for a display and `ahi.device` for `AUDIO=AHI`. Without the library it prints
`aar.library not found.` and returns 20. With a library that has no platform
behind it, it prints `no AAR platform found` (or that `AARQuery` failed) and
returns 20.

Every instance claims its own app, so many apps can run at once. The platform's
resources are the limit. When the platform cannot admit another one, a launch
reports `claim refused:` with the reason (not enough memory, no translation
tables left, not enough shared memory for the queues) and returns 20.

## Starting an app

Run `Stack 32768` in the launching Shell first. aar-run requires at least a
16 KiB stack and prints `Run Stack 32768 in this Shell before aar-run.`
(returning 10) when it has less. For a Shell command the stack that counts is
the CLI's default stack (what the `Stack` command sets, `cli_DefaultStack`), even
when the task's own stack bounds are larger. A Workbench start uses the icon's
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
| app arguments | `max_args`, which counts `argv[0]`, the `ARGS` words and the documents of a Workbench start (0: no limit) | `this platform takes at most <n> arguments for the app.` |
| one argument | `max_arg_bytes`, which counts the NUL, for every argument including a document's path (0: no limit) | `an argument is longer than <n> bytes, the most this platform takes.` |
| `WEIGHT` | `weight_max` (0: the platform does not weight) | `WEIGHT must be 1 to <n> on this platform.` or `WEIGHT is not available: this platform does not weight apps.` |
| `HOSTMEM` | `shared_max_mib`, never above 4095, the rule of `AAR_TAG_SHARED_MIB` | `HOSTMEM must be 1 to <n> MiB on this platform.` |
| the ELF | `max_image_bytes` | reported by the load (`larger than the <n> MiB image limit`) |

`MAXMEM`, `HOSTMEM` and `WEIGHT` must be positive decimal numbers; anything
else prints the usage and returns 10. The library refuses an out-of-range tag
value in any case, so these checks change only where the message appears, and
aar-run takes no claim before them.

`MAXMEM=<MiB>` caps the private memory committed by this app.

```text
aar-run RAM:swquake.elf MAXMEM=256 +timedemo demo1
```

`HOSTMEM=<MiB>` sets how much shared (host-visible) memory this app may
allocate. This is the memory the Amiga can address through the platform,
including the buffers the runtime allocates for the app's console, files and
audio. Without it the app gets the platform default (`shared_default_mib`). The
limit is a cap, not a reservation. Both limits go to the library before the
load. The platform checks them at load and refuses what it cannot grant. aar-run
then prints `load refused:` with the reason (a platform limit means `HOSTMEM` or
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
`ERROR_OBJECT_NOT_FOUND` result. aar-run gives the library the full path of the
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

The profile is checked first, then the API, then the groups the image requires.
The groups it lists as optional are only a hint and never refuse an image. The
library's probe cannot tell a note newer than the library from a damaged one,
which is why the not-an-AAR-image line names both. A platform older than its
library is reported by the API line. A file name with a volume or assign colon
may be a multi-directory assign, which the library's own open does not search.
If the first probe finds nothing there, aar-run finds the file the way it does
for the load and probes the full name then. That needs the serving loop and so a
claim, which aar-run opens and closes again when it refuses the image. Every
other refusal, and a missing plain path, happens before any claim.

### Profile files

An app that runs on several platforms ships one image per profile, such as
`Game.a64.elf` and `Game.a32.elf` (spec section 2.1). Given the name of the app,
aar-run runs the first of these files that `AARProbeFile` accepts and the
platform runs:

| Given | Tried in this order |
| --- | --- |
| `Game.elf` | `Game.elf`, `Game.a64.elf`, `Game.a32.elf`, `Game` |
| `Game` | `Game`, `Game.a64.elf`, `Game.a32.elf` |

The name given always goes first, so a file that runs here is never replaced
by another. The profile files come in the platform's order: the profiles of
`AARInfo.profiles` first, A64 before A32 (the mask carries no other order), then
the profile the platform does not run. The suffix `.elf` is matched without
regard to case, and the other names are built from the part before it, so
`Work:Games/Q/quake.ELF` gives `Work:Games/Q/quake.a64.elf`. A name that ends
in `:` or `/` has no profile files. The rule applies to a Shell command, to the
`APP` ToolType and to the project of a Workbench start alike.

When aar-run takes another file than the one given it prints
`aar-run: using Game.a64.elf instead of Game.elf` before the `loading` line, and
`argv[0]` is the name of the file it took. When no file runs, the refusal is
the one of the first file that exists but cannot run here (for example an A64
image on an A32 platform), and `cannot open <name given>` when none exists.
Every file is probed before any claim is taken, except a name with a volume or
assign colon that the first probe does not find: that one is found on its assign
first, as described above.

### Exit codes

| Code | Meaning |
| --- | --- |
| the app's | on normal app exit, aar-run returns the app exit code (0 to 255) |
| 5 | Ctrl-C while aar-run opens, probes or loads the ELF, including when a DOS packet wait such as a file read or closing the ELF consumed the break |
| 10 | invalid launcher arguments, a stack below 16 KiB, a missing `APP` tool type, a bad ToolType value, an `ARGS` quote that is never closed, a document path that does not fit, or a launch the platform's limits refuse |
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

A crashed aar-run does not leave a claim behind. `aar.library` notices that the
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

A Workbench start has no Shell. aar-run reads ToolTypes from two icons: the
icon of the project first, then its own (the icon of the tool the start message
names, `PROGDIR:aar-run` for a plain double-click). A key the project icon sets
is not read from aar-run's icon, even when its value is empty; every other key
comes from aar-run's icon. A Shell start reads `PROGDIR:aar-run` only.

The project is the first icon after aar-run in the start message. That is the
icon the user double-clicked when its default tool is aar-run, or the first
icon selected when aar-run's own icon is shift-double-clicked. The ToolTypes of
the project's icon are those of the app, so an app ships as `Game.elf` with a
`Game.elf.info` whose default tool is aar-run and whose ToolTypes are its
settings, with no copy of aar-run for each app.

The first project is the app when it is an AAR image (`AARProbeFile` finds a
valid note, whatever the platform), or a file the probe cannot open or read, so
that the launch names the cause. Otherwise it is a document, and the app is
the `APP` ToolType, taken from the project icon first and from aar-run's icon
second. A drawer is never the app. A start with no project takes `APP` from
aar-run's icon. Without an app aar-run requests `Set the APP tooltype, or start
aar-run from a project icon.` and returns 10. The project directory lock is
resolved with `NameFromLock`, and any nonzero DOS BOOLEAN result is success,
including `DOSTRUE` (-1). The project name is then appended to that directory,
without adding a slash after a volume colon. The app's name goes through the
rule of [Profile files](#profile-files).

Every icon after the app is a document, the project too when it is not the
app. Documents follow the `ARGS` words in the app's `argv`, in the order of
the start message. A document is passed as a path: relative to `ROOT` when it
lies under `ROOT` (`maps/e1.bsp`), else in full (`Work:Docs/a.txt`). `ROOT`
itself and a path on another volume are passed in full. A drawer is a document
too and is passed as the path of the drawer. Names are compared without regard
to case. An app opens a full path only with `ABS`, so a shipped app that takes
documents from anywhere has `ABS` in its icon. Each document counts as an
argument in the limits of [What the platform takes](#what-the-platform-takes).
A path that cannot be built in aar-run's buffers is refused with `the path of a
document is too long.`

| ToolType | Meaning |
| --- | --- |
| `APP` | The app's file when the project is not an AAR image, or when there is no project. |
| `ROOT` | Root directory for relative app file paths. Write it with a volume or assign name. |
| `ABS` | Enabled when the ToolType has no value, is exactly `1` or `y`, or begins with `ye` or `t` (case-insensitive). Permits app paths containing an Amiga volume or assign colon. The root check does not apply to those paths. |
| `MODE` | Optional Picasso96 DisplayID, in decimal, `$` hexadecimal, or `0x` hexadecimal. The app still chooses width, height, and RGB format. |
| `ARGS` | Arguments for the app, separated by spaces or tabs. Double quotes keep spaces in one argument. |
| `JOYPORT0` | Enabled by the same values as `ABS`. Enables joystick port 0 as well as port 1. |
| `AUDIO=AHI` | Route app PCM to AHI. The default remains the platform's own audio path. |
| `MAXMEM=<MiB>` | Positive decimal MiB cap on the app's private memory (`AAR_TAG_PRIVATE_MIB`). |
| `HOSTMEM=<MiB>` | Positive decimal MiB shared memory limit (`AAR_TAG_SHARED_MIB`), up to the platform's `shared_max_mib` and 4095. |
| `WEIGHT=<n>` | Decimal CPU share weight from 1 to the platform's `weight_max` (`AAR_TAG_WEIGHT`). |

A value in the project icon replaces the one in aar-run's icon. That holds for
a flag too: `ABS=NO` in the project icon turns off an `ABS` of aar-run's icon.
The `ARGS` of the project replace the `ARGS` of aar-run's icon and are not
added to them. A bad `MAXMEM`, `HOSTMEM` or `WEIGHT` prints `bad <key> ToolType`
and the usage and returns 10.

In `ARGS`, a double quote starts a quoted part and the next double quote ends
it. Spaces and tabs inside it belong to the argument. A quoted part may sit
inside a word, `--name="a b"` is the argument `--name=a b`, and `""` is an empty
argument. There is no escape, so an argument cannot contain a double quote, and
single quotes and `*` are ordinary characters. A quote that is never closed
prints `ARGS has a double quote that is never closed.` and returns 10. The
words of `ARGS` of a Shell start's `PROGDIR:aar-run` icon come before the words
of the command line; the Shell splits the command line itself.

```text
ARGS=-fullscreen "My Saves:slot 1"
ROOT=Work:Games/Game
ABS
```

The same limits and refusals apply as in a Shell launch. The diagnostic words
below are not ToolTypes. A Workbench start opens one console window titled
`aar-run` for aar-run's lines and the app's console. See
[Console and audio routes](#console-and-audio-routes).

## Diagnostics

Two words of the command line exist so that the loop and the display paths can
be measured and forced. They are recognized after the ELF, in any case, and are
not passed to the app. A bad value prints the usage and returns 10.

| Word | Effect |
| --- | --- |
| `POLL=<us>` | The heartbeat of the loop, in microseconds. The default is 1000. The parser accepts 0 to 999999 (`AAR_RUN_POLL_US_LIMIT` in `aar_run_policy.h`). `POLL=20000` suits a program that tolerates a vertical-blank wake-up. `POLL=0` turns the heartbeat off and waits on the library's signal and timed wakes: 10 ms while a stop is pending, 20 ms for display joysticks, 1 ms for completions waiting for queue space, and 250 ms to see a revoked claim. With `AUDIO=AHI`, the serving loop requires 1 to 20000 us instead; other values fail at startup with `AAR_E_NOSYS` (currently reported as `timer.device is required.`). A slower heartbeat costs latency for DOS and audio work. |
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
with firmware that implements AAR API 0.5, `zz9k.library` revision 36 or later
(only to reach the board's mailbox) and `mmu.library` 41 or later, because the
board's shared buffers must be mapped cache-inhibited. The platform takes 16
strings of 127 bytes (so 15 app arguments), a `WEIGHT` from 1 to 256, and
`HOSTMEM` up to 62 MiB (`shared_max_mib`), with a 16 MiB shared default. aar-run
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
doorbell, shared-memory mappings and lease. aar-run links none of that.
Every claim has its own control block and queue pair, and the platform names
each buffer it wants aar-run to use in the request that needs it.

1. aar-run opens `aar.library` (version 0) and calls `AARQuery`. Without a
   platform behind the library the query fails and aar-run stops before
   claiming. It checks that the platform implements the API major it was built
   for, reads the limits it needs from `AARInfo` and checks the launch against
   them. An `aar-run STATUS` or `RELEASE` needs only the query and `AARExt`.
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
   reason with a small `AARStatus`. aar-run does not renew the lease. The
   library does that, so a requester or a long DOS wait in aar-run does not
   cost the app its claim. A completion is followed by a doorbell the library
   sends, but the queue indices stay authoritative. A lost doorbell costs
   latency, never data.
8. Ctrl-C from the Shell or an app-display raw key requests a stop. aar-run
   posts one stop event (retried until the queue has room for it) and calls
   `AARStop`, which writes the control block's stop request, posts the stop
   event and sends the platform's stop. Ctrl-D, Ctrl-E, and Ctrl-F on the app
   display remain normal raw-key events and also post a break event
   with codes 4, 5, and 6. The app display has one borderless input window and
   no close gadget. Its raw keys are forwarded to the app, including Ctrl-C and
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
pending event and flushes it, so a flood costs one entry. Every other event
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
streams) and the heartbeat. See [Diagnostics](#diagnostics) for the AHI `POLL`
bound and the parser's separate limit.
aar-run does not write to stderr while the app is running.

aar-run takes the app's messages itself. The serving loop drains them with
`AARGetMessage`, drops them and counts them, so an app that sends more than the
library's inbox holds cannot hang itself behind an unread message. After the
run aar-run prints `dropped <n> app messages.` when it dropped any. Apps that
exchange custom messages need a client that takes them, for example a program
that creates `aar_serve` with `AAR_SERVEF_KEEP_MESSAGES` (see the
[serve API](../../docs/aar-serve-api.md)).

### Console and audio routes

The app's console output arrives as `CONSOLE_WRITE` requests naming bytes in a
console buffer the platform allocated. A request completes when the DOS write
has finished, with the number of bytes written. A Shell launch writes those
bytes through `Output()` and takes app stdin from `Input()` (a Workbench start
opens one titled `CON:` window and uses it both ways).

A `CONSOLE_READ` request is the app's demand for input. aar-run starts one DOS
read for it and completes it with the bytes read, or with 0 at the end of
input. A redirected input (`<file` or a pipe) ends the app's stdin at the end of
that stream. From the keyboard, the app gets whatever ends a `Type *` in the
same console. On OS 3.2.3 with the standard or CCON handler Ctrl-\ does not, so
stop such an app with Ctrl-C.

aar-run starts a read only while the app it owns is running and has asked for
input. If the app is waiting for keyboard input when it stops, aar-run writes
`press Return to finish.` to the console that owns that read (the Workbench
`CON:` window, or `Input()`), not to redirected stdout, because the console
cannot cancel the line read it has open. A pipe or other non-interactive input
uses the same asynchronous read, so an open writer cannot stall the lease,
display, or audio. A regular file completes immediately. If a non-interactive
read is still pending at exit, aar-run writes `waiting for pending input to
finish.` to the user console (not stdout, and not the pipe) and waits, because
a pipe read cannot be cancelled. The custom screen is closed before either of
those waits, so the prompt is not hidden behind the app display.

A Workbench launch opens one closable `CON:` window titled `aar-run` when it
starts, and aar-run's own lines and the app's console output both go to it.
aar-run does not write through the C library's stdout, so no second, untitled
window appears. If that window cannot be opened, aar-run opens it at the app's
first console use instead. If writing app output fails, aar-run stops the app
and returns 20.

The default audio route is the platform's own audio path and needs nothing from
aar-run. Set `AUDIO=AHI` in the app icon or pass `AUDIO=AHI` after the ELF in a
Shell command to request the AHI route. It is passed to the library as the audio
route tag before the load. On that route the platform allocates the audio ring
and names it in an `AUDIO_OPEN` request. aar-run opens AHI at the requested rate
(stereo), and the library attaches the ring when aar-run completes the request
and detaches it at `AUDIO_CLOSE`.

aar-run reads the PCM with `AARAudioRead`, which converts the ring's interleaved
S16LE samples (rate and channel count in its header) into AHI's S16BE bounce
buffers without resampling or downmixing. The data goes through a reserve of up
to the size of two requests (a FIFO between the ring and AHI), in two requests
of up to 3/50 s (60 ms) each. This is the double buffering of the AHI developer
guide. Each request is linked (`ahir_Link`) to the other while that one is
queued, and a request is sent again as soon as it has finished.

AHI is not started before about 120 ms are in the reserve (the prebuffer). After
that every request is a whole one, never a short piece, because some drivers
leave a gap at every short request boundary and an app that fills its ring only
a short way ahead of the player (a tone generator) would otherwise produce many
gaps. Together the two requests keep about 120 ms of audio in AHI, so a delay of
the loop of up to about 100 ms plays out of AHI's queue. This costs latency. The
prebuffer adds about 120 ms, up to 240 ms in all between the app's ring and the
speakers.

If the app stops writing for 30 ms, what is left in the reserve is played as a
last, short request (the end of a sound). When the app closes the audio, the
queued audio plays out before AHI is closed (a run that ends without closing it
cuts the last 250 ms or so). If AHI runs dry while the app is still writing (the
reserve cannot keep up), aar-run counts it (`ran dry`), reads the empty ring
once so that the ring's underrun counter shows it, and buffers again before it
goes on.

The AHI line printed at the end gives underruns, times AHI ran dry, the number
of requests and their length in frames (minimum and average). With whole
requests they are equal, except for the last one of a sound. If AHI cannot be
opened, aar-run stops the app and returns 20. Before each read aar-run asks the
library how many frames are readable (`AARAudioRead` with no buffer), which
counts nothing, so an underrun in the ring header is counted only when AHI ran
dry and found the ring empty.

Output to `NIL:` works. A console handle with no handler port, which is what
`NIL:` is, cannot take the asynchronous DOS packet the loop uses for other
destinations. aar-run writes such a handle with the synchronous `Write()`
(dos.library answers it by itself and it cannot block) and completes the app's
request on the next pass of the loop, so `aar-run app.elf >NIL:` runs the app
to its end, discards its text and returns its exit code. Input from `NIL:`
ends at once, which is the end of input the app sees. aar-run's own notes (the
output-failure note and the finish prompts) go to the user's console, never
only into the redirected output. A write that fails, such as a full
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
double-height bitmap. A pan completes with the submitted flag. The platform
acknowledges the flip after its own next vertical blank has latched the new
address, which keeps the old front unavailable until scanout has stopped reading
it. Only if both panning layouts fail does it use one bitmap, which the platform
copies each frame into. When the app asks for latest-frame flips, the platform
owns every flip and aar-run sees none. It does not pan or change buffers. The
`DISPLAY=` diagnostic word chooses where this walk starts.

### Files and ROOT

File requests (`FILE_OPEN`, `FILE_READ`, `FILE_WRITE`, `FILE_SEEK`,
`FILE_STAT`, `FILE_CLOSE`, `LIST_OPEN`, `LIST_NEXT`, `LIST_CLOSE`) carry paths
and data in buffers the platform names. The app has up to 16 open files and 16 open
directory lists. A short read means end of file. Raw AmigaDOS Examine packets
return length-prefixed BCPL names; aar-run converts them to NUL-terminated
names in the request buffer before posting the list completion. The end of a
list is `AAR_E_NOENT`.

The open request carries the AAR file flags: `READ` 1, `WRITE` 2, `CREATE` 4,
`TRUNCATE` 8, and `EXCL` 16. aar-run refuses an invalid combination with
`AAR_E_INVAL` (-2) before it touches AmigaDOS: neither `READ` nor `WRITE`, `CREATE`
or `TRUNCATE` without `WRITE`, `EXCL` without `CREATE`, or an unknown bit.
Valid combinations open as follows.

- `READ`: `MODE_OLDFILE`; a missing file is `AAR_E_NOENT` (-5).
- `WRITE` or `READ|WRITE`: the file must exist (aar-run probes with `Lock`,
  because `MODE_READWRITE` creates a missing file); then `MODE_READWRITE`. A
  missing file is `AAR_E_NOENT`.
- `WRITE|TRUNCATE`: as above, then the file is set to size 0 with
  `ACTION_SET_FILE_SIZE`. A handler without that action gets `MODE_NEWFILE`
  after the probe.
- `WRITE|CREATE`: `MODE_READWRITE` on an existing file, keeping its contents;
  `MODE_NEWFILE` if it is missing.
- `WRITE|CREATE|TRUNCATE`: `MODE_NEWFILE`; create, or replace an existing file.
- `WRITE|CREATE|EXCL`, with or without `TRUNCATE`: aar-run probes with `Lock`. An
  existing file is `AAR_E_EXIST` (-11), including one that another program holds
  open exclusively (the handler refuses the lock with `ERROR_OBJECT_IN_USE`),
  for relative and `ABS` paths alike; otherwise `MODE_NEWFILE`.

`READ` may be added to any of the `WRITE` entries. AmigaDOS has no atomic exclusive
create. Within one serving loop the check and create do not interleave with
another request, but another Amiga program or aar-run instance can create the
same name between the probe and the open.

With `ABS` on a multi-directory assign, the probe finds the directory that holds
the file. The open, `SetFileSize` and `MODE_NEWFILE` fallback all go to that
same directory, so an existing file is updated, truncated, kept or replaced
where it is rather than shadowed by a new file in the first directory. Every
step that can create, truncate, replace or write a file is skipped, and answers
`AAR_E_IO`, once the request is no longer wanted. That is the case when the
claim was revoked, any stop is under way (Ctrl-C, `aar_serve_stop`, a stop from
the app's window, the platform ending the app), or an earlier wait for AmigaDOS
was abandoned.

Relative paths stay below `ROOT`, for open, stat and list alike, with or without
`ABS`. `..`, leading `/`, and empty path components that would leave the root
are rejected, and so is a link whose target lies outside it. Paths containing
`:` are rejected unless `ABS` is enabled (the only thing `ABS` admits). With
`ABS`, file open, stat, and directory-list requests use `GetDeviceProc` and try
later multi-directory assign candidates after `ERROR_OBJECT_NOT_FOUND`. This is
path hygiene for an app that is already trusted; it is not an isolation
boundary. File reads and writes are chunked at 8 KiB so one request does not
hold the serving loop for long. A stalled AmigaDOS volume can still block its
DOS call. The claim stays alive meanwhile, because the library renews the
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
it) and releases its Amiga-side resources. For the ZZ9000 mapping policy, see
the [transport note](../../docs/transport.md#visibility-of-the-control-structures).

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
links no transport code, and it does not include the ZZ9000 private wire. The
Workbench icon `aar-run.info` is generated by
`python3 client/run/make_icon.py` (`--check` tells whether the file is current).

The host tests (`sh client/tests/run_host.sh`) cover the request handlers
(`client/tests/serve_core_test.c`), the loop's decisions
(`client/tests/serve_policy_test.c`) and the launcher's (`client/tests/run_policy_test.c`).
They run without an Amiga.
