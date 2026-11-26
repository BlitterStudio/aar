<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# AAR developer guide

AAR, the Amiga ARM Runtime, lets an AmigaOS program start a native ARM program,
called an app, on hardware next to or under the Amiga. The Amiga side loads the
ELF, the runtime on the ARM side checks it and passes it a function table, and
the runtime reclaims every resource when the app stops or faults. The app sees
a display, input, audio, files, a console, a message queue, a heap and shared
buffers, and nothing else.

Apps are trusted programs. A runtime may isolate them from each other and from
the platform (`AAR_INFOF_ISOLATED`), which protects against ordinary bugs. It
is not a sandbox: the user chooses what code to run, and the app still
receives the services and the shared buffers it asks for.

The public headers are in `include/aar/`: `app.h` (the table, the types, the
wrappers), `note.h` (`AAR_NOTE`), `gl.h` (the GL table and wrappers) and
`ext_zz9000.h` (ZZ9000-only extras). The call reference is
[aar-api.md](aar-api.md). `transport.h` is for platform implementers and is
described in [transport.md](transport.md); an app does not include it. The
normative text is the [specification](../spec/aar.md).

## Profiles

An app image belongs to exactly one profile.

| | A32 (`AAR_PROFILE_A32`) | A64 (`AAR_PROFILE_A64`) |
|---|---|---|
| Instruction set | ARMv7-A, A32 state only | ARMv8-A AArch64 |
| Data model | ILP32 | LP64 |
| Floating point | hard-float AAPCS, VFPv3-D32 and NEON | AAPCS64, FP and ASIMD |
| Byte order | little-endian | little-endian |
| Typical cores | Cortex-A9 (ZZ9000) | Cortex-A72 and Cortex-A76 class |
| Toolchain triplet | `arm-none-eabi` | `aarch64-none-elf` |

Optional instructions (VFPv4, integer divide, half precision on A32; CRC32,
LSE, AES, SHA, dot product, FP16 on A64) are never assumed. Read
`aar_info.cpu_features` through `aar_get_info` and pick a code path; do not
execute an instruction and trap. An app that wants to run everywhere ships one
image per profile, for example `name.a32.elf` and `name.a64.elf`. There are no
fat images, and a launcher picks the file that fits its platform; `aar-run` does
this for `name.elf` and `name` (see its
[manual](../client/run/README.md#profile-files)).

Structures that the app and the host share are built from fixed-width
integers, and every fixed-width field has the same offset on A32 and A64. A
structure that contains pointers puts them last, so no padding differs between
the profiles. Use `uint64_t` for sizes that can exceed 4 GiB and `uintptr_t`
for pointer arithmetic, and never assume `long` is 32 bits.

## Lifecycle

The client, an AmigaOS task, opens one app instance and gets a handle. It
streams the ELF, then starts it with an argument list. The runtime checks the
note, applies the relocations, zeroes BSS, and enters at `e_entry`. The ELF
entry symbol is `aar_start`, which is crt0. It stores the table pointer through
`aar_crt0_bind`, runs the `.preinit_array` and `.init_array` functions (C++
globals and `constructor` functions work), calls `aar_main(argc, argv)`, and
passes the result to `exit` (libc flavor) or `aar_exit` (freestanding flavor).
Returning from the entry point itself, without exiting, is undefined.

Stop is Ctrl-C, the client's stop call, or the client closing its handle. The
app sees `aar_should_stop` become true and gets an `AAR_EV_STOP` event. The
grace period (`aar_info.stop_grace_ms`) runs, and then the runtime stops the
app by force (`AAR_END_FORCED`). Read the platform's reported value; the
[lifecycle rules](../spec/aar.md#52-load-start-stop) define the default. The app's
exit code is the return value of `aar_main` or the argument of `aar_exit`.

A fault in app code (a bad data or instruction access, an undefined
instruction, an alignment fault) ends that app only and leaves a profile-neutral
fault class, the raw syndrome and the registers for the client to read.
A fault in runtime code ends every app and the platform restarts its runtime.
A platform reset ends every app. The end reason and the fault record stay
readable until the client closes its handle: `AAR_END_EXIT`, `_FORCED`,
`_USER_FAULT`, `_STALL`, `_STUCK_CALL`, `_LEASE`, `_PLATFORM_RESET`,
`_RUNTIME_RESTART` and `_QUARANTINED`. The client keeps the app alive with a
lease that it renews from an interrupt or timer context. If the owning task dies
without closing its handle, the library releases the handle and stops the app.

Teardown is ordered and idempotent: new calls are blocked, waits and work that
hold app pointers are cancelled, display, audio, GL and decoder resources are
released, mappings and shared buffers are released, and only then is the end
reason published. An app that exits normally has no cleanup to do for the
runtime's sake, but should still release a resource promptly when another
program might want it.

## Watchdog

Call the API at least every 5 seconds during a long computation. Read
`aar_info.stall_limit_ms` for the platform's limit. The minimum stall and
stuck-call limits and their end reasons are defined in
[spec section 5.4](../spec/aar.md#54-watchdog).
A call parked in a declared wait counts toward neither limit.
`aar_yield` is enough to count as activity. `aar_sleep_us`, file
calls, console reads, a flip wait and `aar_msg_wait` are already inside an API
call, and `aar_should_stop` and `aar_time_us` count too. A loop that only polls
them is not a stall. A loop that only touches app memory is.

A watchdog end (`AAR_END_STALL`) reports where the app was: the client reads `pc`,
`sp` and `lr` and a stack snapshot, as for a fault (see [Debugging](#debugging)).

**ZZ9000 note.** Its watchdog and scheduler choices are platform behaviour,
not app guarantees; see [spec section 5.4](../spec/aar.md#54-watchdog).

## Memory

### Private memory

An app has private memory: its image, its stack and a heap. The platform decides
where it is, so apps are position-independent and must not assume an
address. The heap starts at the end of the image. `aar_heap_range` reports its
current base and length. `aar_heap_grow` extends it by a whole number of
`heap_granule` units (1 MiB) and returns the old end, where the heap ended
before the call. The new memory starts there, so the heap stays contiguous.
Existing pointers never move and new memory is zeroed. `aar_heap_shrink`
returns whole granules at the top. A failed growth is `AAR_E_NOMEM`, and in the
libc flavor it makes `malloc` return NULL. It does not fault.

`min_heap_mib` in the ELF note is committed at load; a refused admission is
`AAR_E_NOMEM` from the load call. `aar_get_heap_info` reports what is
committed, the effective limit (the smaller of the platform's cap and the
client's private-memory cap) and what is still free. The runtime keeps a
system reserve. When the free pool crosses below its low-water mark it sends
`AAR_EV_LOW_MEMORY` through `aar_event_poll`, once per crossing; another event
comes only after the pool has risen above its high-water mark and fallen again.

**ZZ9000 note.** Each app has its own virtual window and its memory is mapped from
1 MiB frames of the card's pool. The heap grows upward and the stack is at the
top of the window. Firmware memory, other apps' private frames and device
mappings are not user mappings.

### Shared buffers

Memory that the Amiga side must read or write is a shared buffer, allocated
with `aar_shared_alloc` (see the next section). Shared buffers are accounted
separately from private memory, up to 16 MiB per app by default
(`AAR_SHARED_DEFAULT_MIB`). The client can change that limit at load. It is a
cap and reserves nothing, so an allocation can still fail below it while the
platform's shared region is busy. `aar_get_info` reports `shared_limit` and
`shared_used`. The runtime itself uses shared buffers for the console, the audio
ring and file payloads. They are charged to `shared_used` but do not count
toward the 64 live blocks (`AAR_SHARED_MIN_BLOCKS`) that an app is promised.

### Isolation

On a runtime with `AAR_INFOF_ISOLATED`, user code may access its private memory,
its granted shared buffers and its own surfaces. Any other access takes an app
fault and ends only that app. The runtime also validates every pointer that an
app passes to a call: a bad pointer returns `AAR_E_FAULT` (or, for a GL draw
call, an invalid-value error), so do not use a bad pointer to probe memory. On a
runtime without the flag a bad pointer is undefined behaviour and the platform
documents what happens.

## Shared buffers

A shared buffer is app memory that the host can read and write. It is how bulk
data, such as pixels or a file image, moves between the app and the Amiga side.
Large payloads never travel inside a message. A message is 32 bytes, so it
holds a token and a range.

```c
aar_shared buf;

if (aar_shared_alloc(1u << 20, 64, 0, &buf) != AAR_OK)
    return 1;
/* buf.ptr is my mapping, buf.haddr is the token the Amiga side uses. */
```

`aar_shared.haddr` is an opaque 32-bit token. It is not a pointer, and the app
must not interpret it. It names the allocated block, only for this app, and only
until `aar_shared_free`, exit, fault or stop. To describe part of a block, send
the token and an offset. The block is rounded up to `shared_granule`.

### The visibility problem

When the app writes data and the Amiga reads it (or the other way round), three
things can make the reader see old bytes:

1. The CPU may reorder its own stores, so a flag can become visible before the
   data it announces.
2. The ARM side may hold the data in a cache that the host cannot see, on a
   platform whose bus is not coherent.
3. The 68k has its own data cache, which no ARM-side card can reach. A 68040 or
   68060 client that has read a line of the buffer once can read the old copy
   again from its own cache.

The standard fixes the first two with two calls and a rule about messages. The
third is the client's job, and the client library does it.

### The rules, in plain words

**Writing for the host.** Finish writing the data. Call `aar_publish(ptr,
bytes)` on the range you wrote. Only then send the message that tells the host
the data is there.

```c
render(buf.ptr, used);
aar_publish(buf.ptr, used);                  /* data is visible to the host */
uint32_t msg[8] = { MSG_FRAME, buf.haddr, 0, (uint32_t)used };
aar_msg_send(msg);                           /* now say so */
```

**Reading what the host wrote.** Wait for the message that announces the data.
Then call `aar_acquire(ptr, bytes)` on that range, and only then read it.

```c
if (aar_msg_poll(msg) == 1 && msg[0] == MSG_INPUT_READY) {
    aar_acquire((const uint8_t *)buf.ptr + msg[2], msg[3]);
    consume((const uint8_t *)buf.ptr + msg[2], msg[3]);
}
```

**One writer per granule.** A granule is the unit `shared_granule` reports. Do
not write into a granule that the client is writing, and the client must not
write into one you are writing, between two hand-offs. Split a buffer into
granule-aligned halves if both sides need to write.

**Messages order the bus, not the caches.** `aar_msg_send` is a release point:
every earlier store is ordered before the message becomes visible. A message you
receive is an acquire point: loads after it are ordered after it. That orders
the data against the flag, but it does not clean or invalidate a cache. That is
what `aar_publish` and `aar_acquire` do.

**Why they are table calls.** User mode on A64 cannot invalidate a cache line by
address, and a platform with a non-coherent bridge needs privileged maintenance.
The calls are no-ops with a barrier on a coherent platform. Always call them;
skipping them requires a buffer that reports `AAR_SHAREDF_COHERENT`.

**Surfaces are the exception.** A display flip publishes the surface itself.
Before the host sees the frame the runtime cleans the presented range. A
surface needs no `aar_publish`.

### What the client does

The Amiga client library maps a shared buffer with a call that reports whether
the 68k cache holds the range and whether the map is coherent.
Before it posts a message that announces data it wrote, it calls its publish
function (which pushes dirty 68k cache lines). After it receives a message that
announces data you wrote, it calls its acquire function (which invalidates the
range in the 68k data cache) before reading. It must not write into a range it
is about to acquire, because on a copyback cache an invalidate-by-push would
write a stale dirty line over your fresh bytes. That is the reason for the
granule rule. A client that always calls publish and acquire is correct on every
platform.

For the ZZ9000's ACP path and cache-inhibited client mapping, see the
[transport's ZZ9000 note](transport.md#visibility-of-the-control-structures).

## Messages

A message is exactly `AAR_MSG_BYTES` (32) opaque bytes, delivered in order in
each direction. The runtime and the library never look inside. The content is
your protocol. A convention that works is that word 0 is a type and the words
are little-endian when the app writes them. Use `aar_bswap32` if the protocol
says big-endian.

- `aar_msg_send(msg)` copies 32 bytes toward the host. A full queue is
  `AAR_E_BUSY` and the call never blocks. `aar_info.msg_capacity` messages fit
  (at least 32).
- `aar_msg_poll(msg)` returns 1 and copies one message, 0 when the queue is
  empty, or a negative code. It never blocks.
- `aar_msg_wait(timeout_us)` parks the app until a host-to-app message is queued
  or the timeout passes. `AAR_OK` means a message is queued and the next
  `aar_msg_poll` returns it. `AAR_E_TIMEDOUT` means the queue was empty at the
  timeout. A timeout of 0 only tests. A stop request ends the wait with
  `AAR_E_IO`, but a message that is already queued is reported first, so a stop
  never hides one. A parked app is not counted by the stall watchdog.

A server loop waits instead of spinning:

```c
while (!aar_should_stop()) {
    int rc = aar_msg_wait(500000u);

    if (rc == AAR_E_TIMEDOUT)
        continue;               /* idle work, then wait again */
    if (rc != AAR_OK)
        break;                  /* stop, or a corrupt queue */
    while (aar_msg_poll(message) == 1)
        handle(message);
}
```

`aar_msg_wait` returns `AAR_OK` without taking the message, so drain with
`aar_msg_poll`. The client can still read the app's messages after the app has
exited, until it closes its handle, and drains them before it does.

## Events

`aar_event_poll(out, max, &n)` copies up to `max` queued events and never
blocks. It always delivers the two lifecycle events: `AAR_EV_STOP` (a stop was
requested) and `AAR_EV_LOW_MEMORY` (the free pool crossed below its low-water
mark). A runtime with `AAR_GROUP_INPUT` also delivers `AAR_EV_KEY_DOWN`,
`AAR_EV_KEY_UP`, `AAR_EV_MOUSE`, `AAR_EV_JOY` and `AAR_EV_BREAK`.
`AAR_EV_RESIZE` is defined and never sent. `code` is the Amiga raw key without
the up bit, or an `AAR_BREAK_*` value. `dx` and `dy` are mouse deltas, or
joystick axes in -1, 0 and 1. `buttons` holds the key qualifiers, mouse buttons
in bits 0 to 2 (left, right, middle), or joystick fire in bit 0. Map raw keys
yourself. The runtime keeps at least 64 events. When the completion queue is
full, the client drops the newest event and counts the drop. The client
coalesces mouse motion before it posts.

Ctrl-D, Ctrl-E and Ctrl-F in the display window are raw keys and also
`AAR_EV_BREAK` with codes 4, 5 and 6. They are not console end of input.

Poll once per frame or loop and honour `AAR_EV_STOP`. An app that has nothing to
do waits with `aar_msg_wait` or `aar_sleep_us`, or yields with `aar_yield`; it
does not spin on `aar_event_poll`.

## Files

The app does not see the disk. Every file call goes through the client, which
serves it with dos.library and its own AmigaDOS rights. Paths are AmigaDOS
paths, at most 255 bytes plus a NUL. The client chooses the root. The
recommended default, which aar-run follows, is:

- Relative paths resolve under the Shell's current directory, the icon's
  directory, or an explicit root.
- A path that would leave the root through `..` or a leading `/` is refused
  with `AAR_E_ACCES`.
- A path with a volume or assign prefix (a `:`) is refused unless the client
  allowed absolute paths.

This is path hygiene, not a security boundary.

At least 16 files and, in a separate table, 16 directory lists can be open.
Ids are small integers from `AAR_FD_BASE` (3), chosen by the runtime, and a bad
id is `AAR_E_BADF`. A call waits up to 30 seconds for the client
(`AAR_E_TIMEDOUT`), and after a timeout that id is not reused for the rest of
the app's life. A stop ends the wait with `AAR_E_IO`. A short `aar_file_read`
with `AAR_OK` is end of file. `aar_file_stat` gives size, a 64-bit
modification time in Unix seconds (0 when the filesystem has none) and a mode.
`aar_map_file` is optional: it reads a whole file into a read-only buffer that
the runtime owns, and a platform may return `AAR_E_NOSYS`. Put data files under
the root, and do not hard-code a volume unless the launcher allows absolute
paths.

## Console and log

In the libc flavor, `printf`, `puts` and `write` of fd 1 or 2 go to the console
through `aar_console_write`. It writes every byte or returns an error; when the
client's buffer is full the call parks until the client has taken it. No byte is
dropped. A stop cancels the wait with `AAR_E_IO`. `read` of fd 0 and
`aar_console_read` park for client input and return success with zero bytes at
end of input. `isatty` returns 0.

`aar_log` is a different channel: a lossy diagnostic record of at most 1024
bytes, separate from stdout, for the platform's debug output and for the client.
Use `aar_log` for traces and stdout for the user. The ZZ9000 rate limits are
recorded in [C-CON.10](../conformance/TESTS.md#c-con-console-and-log-section-72-104).

The client decides what the console is: the Shell's `Output()` and `Input()`, or
a window it opens on the first console call for a Workbench launch. On AmigaOS
3.2, Ctrl-\ does not end keyboard input, not even for `Type *`. Stop a
keyboard-fed app with Ctrl-C, and deliver end of input by redirecting a file
(`aar-run app.elf <file`). If the app is blocked in a keyboard read when it stops,
the console cannot cancel the line read it has open, and aar-run prints
`press Return to finish.`.

## Building

### The SDK image

The SDK image `ghcr.io/blitterstudio/aar-sdk` has Arm GNU Toolchain 15.3.rel2
for both targets (`arm-none-eabi` and `aarch64-none-elf`), a PIC newlib sysroot
for each (stock newlib is not position-independent and cannot satisfy the
loader), a PIC C++ runtime in the same sysroots ([C++ apps](#c-apps)) and this
repository at `/opt/aar-sdk`. It sets `AAR_SDK`,
`AAR_SYSROOT_A32` and `AAR_SYSROOT_A64`. Build it yourself with:

```sh
docker build -f docker/Dockerfile -t aar-sdk .
```

from the root of this repository. Outside the image, install Arm GNU Toolchain
15.3.rel2, run `AAR_PROFILE=a32 ./build_pic_newlib.sh` and
`AAR_PROFILE=a64 ./build_pic_newlib.sh` for the profiles you need, and
`./build_pic_libstdcxx.sh` with the same `AAR_PROFILE` and `AAR_SYSROOT` if
apps use C++. Point `AAR_SYSROOT_A32` and `AAR_SYSROOT_A64` at their output, and
set `AAR_SDK` to the root of this repository. Freestanding apps (below) need no
sysroot.

### The note

One source file declares the note:

```c
#include <aar/app.h>
#include <aar/note.h>

/* stack bytes (0 = 1 MiB), min heap MiB, groups required, groups optional;
   no ';' */
AAR_NOTE(0, 0, AAR_GROUP_DISPLAY | AAR_GROUP_INPUT, AAR_GROUP_AUDIO)

int aar_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}
```

`AAR_NOTE` declares the profile of the compiler, the API major, and
`AAR_API_MINOR` as the minor the app needs. `AAR_NOTE_EX` takes an explicit
minor, for an app that uses only calls an older minor has. Require a group only
if the app cannot work without it: the runtime refuses the load with
`AAR_E_NOGROUP` when a required group is absent. List a group as optional if the
app can run without it but a launcher might want to prepare for it. At run
time, test the `groups` word that `aar_get_info` fills for an optional group. A
host build (a compiler that is not an Arm target, or `AAR_HOST` defined)
expands the macros to nothing.

### With make

```make
AAR_SDK ?= /opt/aar-sdk
APP := myapp
SRCS := main.c render.c
include $(AAR_SDK)/mk/aar.mk
```

`make` builds `build/a32/myapp.elf` and `build/a32/myapp.map`. Set
`AAR_PROFILE=a64` for the A64 image (`build/a64/myapp.elf`); `a32` is the
default. `make check` also runs `aar-check`, and `make clean` removes the build.
`CFLAGS`, `CXXFLAGS` and `LDLIBS` set before the include are added to the SDK's
flags. `AAR_FREESTANDING=1` selects the freestanding mode. A `.cpp`, `.cc` or
`.cxx` file in `SRCS` makes it a C++ app ([C++ apps](#c-apps)).

### With CMake

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp C ASM)
include("${AAR_SDK}/cmake/aar.cmake")
aar_add_executable(myapp SOURCES main.c render.c)
```

Configure with
`-DCMAKE_TOOLCHAIN_FILE=$AAR_SDK/cmake/aar-toolchain-a32.cmake` (or
`aar-toolchain-a64.cmake`). `aar_add_executable(<name> SOURCES ... [FREESTANDING]
[PROFILE a32|a64])` links the image and runs `aar-check` after the link. A
project that builds both profiles configures twice, once per toolchain file, or
passes `PROFILE` to build one target for a profile. A project with C++ sources
lists `CXX` in `project()`: `project(myapp C CXX ASM)`.

### Checking an image

```text
tools/aar-check [--profile a32|a64] app.elf [more.elf ...]
tools/aar-check --expect-note app.elf STACK_BYTES MIN_HEAP_MIB GROUPS_REQUIRED GROUPS_OPTIONAL
tools/aar-check --no-libc app.map
```

The profile is inferred from the ELF class when `--profile` is omitted. The exit
code is 0 when every check passes, 1 when a check failed, and 2 for a usage
error. `aar-check` applies the rules the runtime applies at load:

- The image is a position-independent executable (`ET_DYN`).
- The ELF class, data, machine and flags match the profile (A32: `ELFCLASS32`,
  `EM_ARM`, EABI version 5, hard-float flag set; A64: `ELFCLASS64`,
  `EM_AARCH64`, flags 0).
- There is no `PT_INTERP` and no `DT_NEEDED`, there are at most four `PT_LOAD`,
  and none is both writable and executable.
- Relocations come from the dynamic segment and are `R_ARM_RELATIVE` (A32) or
  `R_AARCH64_RELATIVE` (A64) only, with no symbol.
- The entry point is inside an executable segment.
- Exactly one AAR note is present and valid.

`--no-libc` checks the link map of a freestanding build.

## Freestanding apps

An app can leave the C library out. Build it with `AAR_FREESTANDING=1` in the
make include, with `FREESTANDING` on `aar_add_executable`, or configure CMake
with `-DAAR_FREESTANDING=ON` to make every target freestanding. The image links
`crt0`, the app, `libaar` and `libgcc`. The PIC sysroot is not needed for make
or for `-DAAR_FREESTANDING=ON`. Without that option the CMake toolchain file
still looks for the sysroot, so a project that mixes a `FREESTANDING` target
with normal targets sets it for the normal ones. The macro `AAR_FREESTANDING` is
defined for the app's sources, so one file can build in both modes.

What differs:

- `crt0` still runs `.preinit_array` and `.init_array`, so a `constructor`
  function works, and passes the value `aar_main` returns to
  `aar_exit`. Nothing flushes stdio or runs `atexit` handlers.
- There is no C++ runtime. The make include and `aar_add_executable` refuse C++
  sources in this mode, because `libstdc++` sits on newlib. Use the libc flavor
  for C++.
- The link has no `-lc` and no `-lm`, and `--no-undefined` stays. A call to
  `malloc`, `printf`, `strlen` or `sinf` fails at link time. Integer division
  comes from `libgcc` where the CPU has no divide instruction.
- `libaar` leaves out its libc glue (`syscalls.c`): there is no `_sbrk`,
  `_write` or `_exit`. Write output with `aar_log` or `aar_console_write`.
- `aar_alloc` and `aar_free` are not declared. The app brings its own allocator
  over the heap. `aar_heap_range` reports the committed span, `aar_heap_grow`
  commits whole granules at its end and returns the old break, and
  `aar_heap_shrink` gives them back. Shared buffers work as before.
- `libaar` has weak `memcpy`, `memmove`, `memset` and `memcmp`
  (`libaar/src/freestanding.c`). GCC calls them for struct copies and zero
  initialisers even when the source never names them, and `libgcc` does not
  provide them. Define your own strong versions to replace them. No other
  `<string.h>` function exists in this mode.

After linking, the make include and the CMake function run `aar-check --no-libc`
on the link map. It fails when any archive other than `libaar` and `libgcc`
appears, for example when `LDLIBS=-lc` brings newlib back. `templates/freestanding.c`
is a worked app with a bump allocator over the heap, a hand-written hash and its
own number formatting. The make include keeps each mode's objects and archive in
their own directory under the build directory, so switching `AAR_FREESTANDING` in
one build directory never reuses the other mode's `crt0.o` or archive members.

## The libc layer

In the normal (libc) flavor, `libaar` has two layers. The wrappers, crt0 and
the linker script are layer 1. Layer 2 is the glue that lets a PIC newlib
program run:

- `_sbrk` is built on `aar_heap_grow` and `aar_heap_shrink`, so `malloc` works
  and a failed growth is a NULL return.
- fd 0, 1 and 2 are the console: `read`, `write`, `printf`, `puts`, `scanf`.
  `isatty` returns 0.
- fd 3 and above are files, over `aar_file_*`: `open`, `close`, `read`, `write`,
  `lseek` and `fstat`. `fopen`, `fread`, `fwrite`, `fseek` and `fclose` work.
  `open` passes `O_CREAT`, `O_TRUNC` and `O_EXCL` to the client as
  `AAR_FILE_CREATE`, `AAR_FILE_TRUNCATE` and `AAR_FILE_EXCL` in one
  `aar_file_open` call, with no separate existence check. How exclusive
  `O_EXCL` is depends on the client; see `aar_file_open` in the API reference.
  `stat` and `fstat` return -1 with `errno` set to `EOVERFLOW` when the size
  does not fit `off_t`, which is 32 bits on A32 (a file of 2 GiB or more);
  `lseek` does the same for a position. A64 has a 64-bit `off_t`.
  The path rules of the client apply. For a directory use `aar_file_list_open`,
  `aar_file_list_next` and `aar_file_list_close`, and for a path use
  `aar_file_stat`, which reports a 64-bit size.
- `exit` flushes stdio, runs the `atexit` handlers, and ends in `aar_exit`.
- `aar_alloc(bytes, align)` and `aar_free(p)` are `malloc` with an alignment.
  Shared memory is `aar_shared_alloc`, and the two allocators do not mix.

## C++ apps

A C++ app uses the libc flavor. List `.cpp`, `.cc` or `.cxx` files in `SRCS`
(make) or `SOURCES` (CMake, with `CXX` in `project()`), next to C files or
instead of them. `templates/cxx_hello.cpp` is a worked app: a global object
with a constructor and a destructor, `std::vector` and `std::string`, `new` and
`delete`, a virtual call, RTTI, and `throw` and `catch`. `aar_main` has C
linkage in `aar/app.h`, so a definition after the include keeps it; the template
writes `extern "C"` to make that visible.

What the build does:

- C++ sources compile with `arm-none-eabi-g++` or `aarch64-none-elf-g++`, the
  profile flags and the code-generation flags of the C sources (`-fPIE -O2
  -ffp-contract=off -Wall -Wextra -Werror`), and `-std=c++17`. Flags in
  `CXXFLAGS` (make) or `CMAKE_CXX_FLAGS` (CMake) come after them, so
  `-std=c++20` or `-fno-rtti` there takes effect.
- The link adds `libstdc++` and `libgcc_eh` from the sysroot, and uses
  `libaar/<profile>/aar-cxx.ld` in place of `aar.ld`. `libaar/src/cxx.c` is
  linked in as an object. A C-only app keeps `aar.ld` and does not change.
- The runtime is not the toolchain's. The archives that come with Arm GNU
  Toolchain cannot be linked into an app: on A32 they are Thumb code with
  absolute relocations, and on A64 `ld -z text` stops with `read-only segment
  has dynamic relocations`. `build_pic_libstdcxx.sh` builds `libstdc++`,
  `libsupc++` and `libgcc_eh` with `-fPIC` from the GCC source of the toolchain
  release (GCC 15.3.1), in ARM state on A32, with the toolchain's own library
  configuration, and installs them next to the PIC newlib. The headers stay the
  toolchain's, and the script stops unless the `bits/c++config.h` it generates
  is the one the toolchain ships for the profile. Without the PIC runtime in the
  sysroot, a build with C++ sources stops with a message that names the script.

What works:

- Global and namespace-scope objects. Their constructors run from
  `.init_array` before `aar_main`, and their destructors run when `aar_main`
  returns or the app calls `exit`, because `libaar/src/cxx.c` supplies
  `__dso_handle` and newlib's `exit` runs the `__cxa_atexit` list. Function-level
  statics work.
- `operator new` and `operator delete`, including the sized and aligned forms,
  call newlib's `malloc` and `free`, so they use the private heap that
  `_sbrk` grows ([The libc layer](#the-libc-layer)). A failed allocation throws
  `std::bad_alloc`, or ends in `std::terminate` when exceptions are off.
- The containers and strings, algorithms, smart pointers, `std::function`,
  `std::optional`, `std::variant` and `<sstream>`. `<iostream>` works and
  writes `std::cout` to the console, and `<fstream>` works over the file calls.
  Both pull in the locale code and add 600 to 700 KiB to the image, so a small
  app prefers `aar_log` or `printf`.
- Exceptions and RTTI, on by default. See the next paragraphs.

Exceptions. With `AAR_CXX_EXCEPTIONS=1` (the make default) or
`-DAAR_CXX_EXCEPTIONS=ON` (the CMake default) C++ sources compile with
`-fexceptions`. On A32 the unwind tables are `.ARM.exidx` and `.ARM.extab`,
which `aar-cxx.ld` places in the read-only segment between `__exidx_start` and
`__exidx_end`. On A64 they are `.eh_frame` and `.gcc_except_table`; the
`.preinit_array` entry in `libaar/src/cxx.c` registers `.eh_frame` with
`__register_frame` before any C++ constructor runs, at any priority, because bare-metal libgcc has no
`dl_iterate_phdr`. Both table forms hold PC-relative words, so the image still
has no relocation but `R_ARM_RELATIVE` or `R_AARCH64_RELATIVE`, and `aar-check`
passes: the rules of the specification are unchanged. An exception thrown in
C++ code and caught in C++ code works across any number of C++ frames, and
through the library's own frames. C code of the SDK build has no unwind tables
(`-fno-unwind-tables`), so an exception must not cross a C function such as a
`qsort` comparator. An exception nobody catches prints
`terminate called after throwing an instance of 'T'` on the console and ends the
app with status 1.

`AAR_CXX_EXCEPTIONS=0` (or `-DAAR_CXX_EXCEPTIONS=OFF`) compiles C++ with
`-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables`. The link
is the same, and the image is only a few KiB smaller, because `libstdc++` itself
is built with exceptions: `new` and the library's checks still contain `throw`
sites, and a throw from them with no table to find ends in `std::terminate`.
RTTI stays on in both modes. `-fno-rtti` in `CXXFLAGS` turns it off.

What does not work:

- The freestanding flavor. See [Freestanding apps](#freestanding-apps).
- Threads. The runtime has no thread model. `std::mutex` and
  `std::condition_variable` are not declared, and `std::thread` has no
  constructor that starts a thread.
- The wall-clock and steady clocks of `<chrono>`. `system_clock::now()` and
  `steady_clock::now()` need `_gettimeofday`, which the libc layer leaves out
  because the table has no such call: the link fails. Use `aar_time_us()`.
- `std::random_device` returns a fixed sequence, because there is no entropy
  source. Seed a generator from `aar_time_us()`.

## Running on AmigaOS

Use [`aar-run`](../client/run/README.md), the reference Shell command and
Workbench tool. It uses `aar.library` ([client API](aar-client-api.md)) and
`aar_serve` ([serve API](aar-serve-api.md)) on any supported platform.
The manual is the reference for arguments, ToolTypes, exit codes and platform
requirements.

**ZZ9000 note.** AAR 0.5 images need firmware that implements AAR 0.5.
See the firmware's
[runtime contract](https://github.com/BlitterStudio/zz9000-firmware/blob/master/docs/app-runtime.md)
and the manual's [platform notes](../client/run/README.md#platform-notes).
Do not include firmware headers in an app.

Shell:

```text
Stack 32768
aar-run RAM:app.elf [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [WEIGHT=<n>] [AUDIO=AHI] [args]
```

`argv[0]` is the ELF file name; further tokens are the app's arguments.
aar-run checks the image and requested limits against `AARInfo` before
taking a claim where possible. `MAXMEM` caps private memory, `HOSTMEM`
sets the shared-buffer limit (the platform default when omitted), and
`AUDIO=AHI` selects AHI instead of the platform audio path. See the
[manual](../client/run/README.md) for Shell and Workbench use.

### The client library

`aar.library` ([aar-client-api.md](aar-client-api.md)) is the client library on
every platform, for programs that run apps without aar-run. A handle belongs to
the task that opened it; do not use or close it from another task. A task that
dies holding a handle loses it within about a second. `AARStart` passes `argv`
unchanged, so put the program name in `argv[0]` if the app expects the aar-run
convention. `notify_port` receives at most one outstanding event (message,
completion or fault). Before closing a handle, drain the notify port and read
messages until none is queued. A task has one signal bit shared by all its
handles, not one per handle.

Use [`templates/echo.c`](../templates/echo.c) for a message peer and
[`templates/ringflood.c`](../templates/ringflood.c) for queue load testing.
The [firmware SDK smoke guide](https://github.com/BlitterStudio/zz9000-firmware/blob/master/sdk/docs/zz9k-release-smoke.md)
describes its ZZ9000 test tools.

### Scheduling and shared resources

The standard does not prescribe a scheduler. Read
[spec section 5.6](../spec/aar.md#56-concurrent-apps) for the
weight convention and admission limits. Release scarce resources promptly.

**ZZ9000 note.** Its scheduler, audio stream count and GL ownership are
documented in the firmware's
[runtime contract](https://github.com/BlitterStudio/zz9000-firmware/blob/master/docs/app-runtime.md).
Apps must not depend on the scheduler quantum or API-body preemption policy.

## Running on a PC

The [host-runner manual](../host-runner/README.md) describes native PC builds,
its options and differences from a target. SDL2 supplies display, input,
audio and, through its OpenGL context, the GL group, so a GL app runs there
too; that GL is more capable than a card's renderer, so passing on the PC does
not show that the app stays inside what the target supports. The runner does
not load an ARM ELF. Check timing, memory limits and watchdog behaviour on the
target platform.

## Debugging

On a fault the client can read the fault class, the raw syndrome, the processor
state, `pc`, `sp`, `lr`, the fault address, the exit code and the load base. On a
watchdog stall (`AAR_END_STALL`) it reads `pc`, `sp` and `lr` as the watchdog
found them, with class `AAR_FAULT_NONE`. In both cases the platform also copies up
to 512 bytes of the app's stack, from `sp` upward, which the client reads with
`AARReadStack`. aar-run prints the registers; its
[manual](../client/run/README.md#exit-codes) defines the launcher exit codes.

Resolve `pc` and `lr` against the app ELF, not the firmware ELF. They are
virtual addresses in the app's window. Subtract the load base that the status
reports if `addr2line` expects the ELF's unrelocated address:

```text
arm-none-eabi-addr2line -e app.elf -f -C <pc minus load base>
aarch64-none-elf-addr2line -e app.elf -f -C <pc minus load base>
```

### Crash log and symbolizer

`aar-run app.elf CRASHLOG=RAM:crash.txt` writes the end reason, the registers, the
load base, the ELF path and the stack words to a text file when the app faults or
stalls. The format is in the [aar-run manual](../client/run/README.md#crashlog).
Build the app with `-g` and keep the ELF that you ran, then:

```text
python3 tools/aar-symbolize crash.txt
```

`aar-symbolize` subtracts the load base, names `pc` and `lr` with
`<cross>addr2line -e app.elf -f -C` when the cross toolchain is on the path (source
file and line need `-g`), and with the ELF symbol table otherwise. `--elf` names the
ELF when the report's path is not valid on the PC, `--cross` the toolchain prefix and
`--no-addr2line` forces the symbol table.

Its backtrace is a guess. The platform does not unwind the stack: the tool lists, in
stack order, every word of the snapshot that falls inside an executable segment of
the ELF after the load base is subtracted. Many of them are saved return addresses,
and some are stale ones left by earlier calls or code pointers stored as data. Read
it from the top, and trust it least where the code has no frame pointers.

### Profiler

To see where a program spends its time, ask the platform to sample the pc:

```text
aar-run app.elf PROFILE=100 PROFILEFILE=RAM:samples.bin
python3 tools/aar-profile RAM:samples.bin
```

`PROFILE` is the rate in samples per second, which the platform may lower, or ignore
when it has no profiler (aar-run says so). The tool prints one line per function with
its samples and share, most sampled first, plus the rate, the load base and how many
samples the ring lost before aar-run took them. A sample is a point in time: a
function with a third of the samples held the pc about a third of the run. A client
of its own does the same with `AAR_TAG_PROFILE_HZ` and `AARReadProfile`
([client API](aar-client-api.md#aarreadprofile)).

A live debugger (a GDB stub with breakpoints and single step) is not part of this
change. The crash log and the profiler need nothing from the platform beyond the
words of the [control block](transport.md).

`aar_log` lines arrive at the client; platform sinks may have separate rate
limits (see [C-CON.10](../conformance/TESTS.md#c-con-console-and-log-section-72-104)).
stdout and stderr are the console, not that log. A missing console on
Workbench means the app has not called a console function yet.

## Performance

**A32.** Compile with the profile's flags, `-mcpu=cortex-a9 -marm -mfpu=neon
-mfloat-abi=hard`; the make include and the CMake toolchain file set them. NEON
and the hard-float ABI are the app ABI. Use NEON for bulk pixel and sample
work. Do not ship a soft-float object: it will not link against `libaar`.

**A64.** The baseline is ARMv8-A with FP and ASIMD. Use `aar_info.cpu_features`
before you use an optional instruction. A32 and A64 do not give bit-identical
results for denormal inputs in NEON or ASIMD code: A32 NEON always flushes
denormals to zero, and A64 ASIMD does not unless `FPCR.FZ` is set. An app that
needs identical results across profiles must set the mode itself.

For both: draw into private memory or the surface back buffer, then present.
Pitch is the bitmap stride, not `width * bytes_per_pixel`. After a flip,
refresh the surface before writing the new back buffer. `aar_display_flip` waits
for acknowledgement. `aar_display_flip_async` (when `AAR_DISPCAP_ASYNC` is set)
plus `aar_display_flip_idle` overlaps the next frame with scanout. With
`AAR_DISPLAY_LATEST` and `AAR_DISPCAP_LATEST`, a flip replaces a pending frame
instead of waiting. `aar_event_poll` never blocks. Pace a game loop with
`aar_sleep_us` or `aar_yield`, not a spin. A spin burns the CPU and can trip
the stall watchdog if you never enter the API.

Audio is interleaved S16LE stereo at 22050 Hz, which every platform supports.
A platform may also grant 44100 and 48000 Hz; read the granted rate from the
`aar_audio` structure rather than assuming. Write ahead of `*read_frame` and
never more than `frames` ahead. Store the samples first, then publish
`*write_frame` with a release store. Read `*read_frame` with an acquire load
before you work out the free space (`__atomic_store_n` and `__atomic_load_n`
on GCC and Clang). `volatile` alone does not order the samples against the
index. See `templates/tone.c`. The audio contract is in
[spec section 8.4](../spec/aar.md#84-audio-aar_group_audio); AHI route behaviour is in the
[aar-run manual](../client/run/README.md#console-and-audio-routes).

SHA-256, decompress, and image or audio decode run inside the call.
Their codec availability is platform-defined.

## ZZ9000 note: bit-identical maths

A lockstep simulation or a replay needs the same floating-point results on
every run. `tests/fp_codegen_test.py` (part of `make check`, arm-none-eabi-gcc
15.3.1) checks the code generation claims, except two that come from the GCC
documentation: that `__FLT_EVAL_METHOD__` is 0 and that fast-math reassociates
reductions. The entry value and the denormal behaviour can be checked on the
card with `templates/fpenv.c`.

Entry state. A new app starts with a zeroed context: the floating-point status
and control register is 0 (round to nearest, flush-to-zero off, default NaN off,
no trapped exceptions, cumulative flags clear), and `d0` to `d31` are 0. The
standard guarantees this at entry. VFP and NEON are enabled for user mode.

Per-thread state. Every switch into or out of an app saves and restores the
vector registers and the FPSCR with the integer registers. An app that changes
its rounding mode keeps the change across preemption, and no other app sees it.
An API call does not reset the FPSCR, so runtime code that runs inside a call
sees yours; restore the entry value before a call if you changed it.

NEON and VFP differ. Advanced SIMD floating-point arithmetic on A32 always
flushes denormal inputs and results to zero and rounds to nearest, whatever the
FPSCR holds (Arm ARM DDI 0406C, "Flush-to-zero"; NEON Programmer's Guide
DEN0018A, "Flush-to-zero mode"). Scalar VFP code follows the FPSCR, which is 0
on entry, so it keeps denormals. The same expression on an S register and on a Q
register gives different bits for denormal data. `fpenv.c` shows the two results
on the card.

Fused multiply-add. The Cortex-A9 implements VFPv3, which has no fused
multiply-add (VFMA arrived with VFPv4; `AAR_CPU_A32_VFPV4` reports it). With
`-mfpu=neon`:

- GCC defines no `__ARM_FEATURE_FMA`, rejects `vfmaq_f32` ("target specific
  option mismatch") and turns `__builtin_fmaf` into a call to `fmaf` in libm.
  `-mfpu=neon-vfpv4` would emit `vfma`, which this CPU cannot run.
- `a * b + c` compiles to `vmla.f32` (S or D registers) and the intrinsic
  `vmlaq_f32` to `vmla.f32` on Q registers. VMLA is a multiply followed by an
  add, and the Arm ARM ("Floating-point exceptions") describes it as two
  operations, so the product is rounded before the add and the result equals a
  separate `vmul` and `vadd`. The generated code is identical for
  `-ffp-contract=off` and `fast`.

Flags. The SDK's flags already keep float code on VFP and IEEE-exact. Keep it
that way:

- Do not use `-ffast-math` or `-funsafe-math-optimizations`. With them, `-O3`
  moves float loops onto Q registers (flush-to-zero) and reassociates
  reductions such as a dot product. Without them the same loops stay scalar VFP,
  in source order.
- Add `-ffp-contract=off` (`CFLAGS := -ffp-contract=off` before the make
  include). It changes nothing on this target. It keeps a build for the PC from
  fusing `a * b + c`, which GCC does by default on hardware with FMA.
- `__FLT_EVAL_METHOD__` is 0, so no operation is evaluated at a wider type.
- GCC folds a call such as `sinf(0.5f)` with a constant argument at build time.
  Compile with `-fno-builtin` (or `-fno-builtin-sinf`) when the result must be
  what the libm in the image computes.
- libm is linked statically from the PIC newlib sysroot, so results match for
  the same sysroot. Pin the SDK image tag, because another newlib version can
  change the last bit of a function such as `sinf`. A freestanding app has no
  libm and brings its own functions.

The SDL host runner is a native build. Use it for logic. Its denormal handling,
fusing and libm differ from the card, so compare bits only on the card. On A64
the same rules apply with different mechanics: set `FPCR.FZ` yourself if you
want flush-to-zero to match A32 NEON, and check the compiler's contraction
default (`-ffp-contract=off`).

## Porting a POSIX command-line tool

1. Rename `main` to `aar_main`. Return the exit code. `exit` works in the libc
   flavor and ends in `aar_exit`.
2. Keep `printf` and `fprintf(stderr, ...)`. They reach the console. `fopen`,
   `fread`, `fwrite`, `fseek` and `fclose` work over the client's files. For a
   directory use `aar_file_list_*`, and for one read-only input that fits in the
   shared limit consider `aar_map_file` where the platform has it.
3. Treat stdin end of input as a zero-length read. Do not wait for Ctrl-\ on OS
   3.2. Document that the user redirects input, or stop on Ctrl-C through
   `aar_should_stop`.
4. In any loop that can run for more than a few seconds without other API calls,
   call `aar_yield`.
5. Put data files under the root. Do not hard-code a volume unless the launcher
   allows absolute paths.
6. Link as a PIE ELF with the note macro. Pass 0 for the groups of a tool that
   has no display, audio or GL.

A filter that hashes a file and writes 32 bytes is `templates/filter.c`.
`templates/memhog.c` is an allocator-limit probe: it touches 1 MiB blocks until
`malloc` returns NULL, reports each 16 MiB and low-memory events, then holds the
memory until stopped and frees every block.

## Porting an SDL-style game loop

There is no SDL window and no event pump thread. The loop is ordinary C.

1. `aar_display_open` with the size and `AAR_FMT_*` you draw. Inspect `layout`
   before storing pixels. `format` stays the logical request.
2. Draw `surface.back`. `aar_display_flip`, or queue `aar_display_flip_async`
   and wait with `aar_display_flip_idle` before drawing the next frame. Then
   `aar_display_refresh`.
3. `aar_event_poll` each frame. Honour `AAR_EV_STOP`. Map raw keys yourself.
   Mouse deltas are in the event. Joystick axes are -1, 0 or 1.
4. `aar_audio_open(22050, 2, &audio)` and fill interleaved S16LE up to
   `*read_frame + frames`.
5. Sleep until the next frame with `aar_sleep_us`. Do not busy-wait on
   `aar_time_us`.
6. Require `AAR_GROUP_DISPLAY` and `AAR_GROUP_INPUT` in the note, and list
   `AAR_GROUP_AUDIO` if you open audio.

`templates/pixels.c` shows display and input. `templates/tone.c` shows the
audio ring. GL goes through `aar_gl_bind` and the `gl*` wrappers, not through a
second library. The GL implementation stays in the platform. Apps reach it only
through the table.

## Versioning

The table is append-only. A new call is a new field at the end and a minor bump.
`AAR_API_MINOR` is 5. API 0.5 is the first AAR version. Before 1.0 any
minor may break, and 1.0 is cut when a second platform passes conformance.

Gate every appended slot with `AAR_API_COVERS` and a non-NULL function pointer.
`libaar` already does this and returns `AAR_E_NOSYS` when a slot is missing. A
direct call that skips the test reads off the end of an older table. A group is
present when its bit is set in `aar_api.groups`, and then every call of that
group that the table covers is non-NULL, so an app that needs a group checks the
bit once. Do not gate on `aar_api.version` alone; use `size`, `groups` and
`aar_get_info`. `AAR_API_AT_LEAST` is there for a code path that needs a whole
release.

A breaking change needs a new major. The runtime rejects a note whose
`api_major` differs from its own, or whose `api_minor_required` is above its own
minor. Do not renumber or reorder fields. Do not change the meaning of an
`AAR_*` value.

The note layout has its own version, `AAR_NOTE_VERSION`, which is not the API
minor.

## Licensing

This repository is [MIT licensed](../LICENSE). Each platform implementation
keeps its own licence. For the ZZ9000 firmware and the boundary between the
runtime and an app, see the firmware's
[LICENSING.md](https://github.com/BlitterStudio/zz9000-firmware/blob/master/LICENSING.md).
