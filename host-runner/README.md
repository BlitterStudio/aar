# aar-host

`libaar-host` runs a native build of an AAR app. SDL2 stands in for the
platform. The app includes `aar/app.h`, defines `aar_main`, and links this
library. The library provides `main()`, libaar's own `aar_*` and `gl*` wrappers
(`libaar/src/aar.c` and `gl.c`, which need no libc), the `aar_alloc` and
`aar_free` of the libc flavor, and an `aar_api` table with the groups listed
below. It does not link the Arm `crt0`, `syscalls.c` or `freestanding.c`. It
always links the host C library, so `AAR_FREESTANDING` has no effect on the
runner: an app that calls `malloc` runs on the host but fails to link for a
target in freestanding mode. `freestanding_unit` compiles libaar's `aar.c` and
`gl.c` with the macro set, and its `freestanding.c` by inclusion, to test the
wrappers over a fake table and the `mem*` routines.

The table claims `AAR_GROUP_DISPLAY`, `AAR_GROUP_INPUT`, `AAR_GROUP_AUDIO` and
`AAR_GROUP_SERVICES`, in `aar_api.groups` and in `aar_info.groups`. It adds
`AAR_GROUP_GL` when the host can make an OpenGL compatibility context (see
"GL"); without it, `gl_bind`, `gl_swap` and `gl_unbind` are NULL,
`aar_gl_bind` returns NULL, and the `gl*` wrappers do nothing. The services
group is present and answers `AAR_E_NOSYS` for what the host does not provide
(see "Differences from a target").

## Build

Linux (CI uses this):

```sh
sudo apt-get install cmake libsdl2-dev python3
# for the GL tests: Mesa (llvmpipe) for EGL and GLX, and a virtual X server
sudo apt-get install libegl1 libegl-mesa0 libgl1 libgl1-mesa-dri libglx-mesa0 \
  xvfb xauth
cmake -S host-runner -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

macOS: `brew install cmake sdl2 pkg-config python3`, then the same cmake
commands. Windows: install SDL2 and a GCC or Clang toolchain (MSYS2 MinGW, for
example), then the same commands. The sources avoid POSIX-only calls without a
Windows fallback. CI itself is Ubuntu only. MSVC is not supported, because
libaar's `aar.c` uses the GCC atomic builtins.

A third-party app in its own CMake project:

```cmake
add_subdirectory(path/to/aar/host-runner)
aar_host_executable(myapp SOURCES myapp.c)
```

`myapp.c` defines `aar_main`. Do not define `main`. `AAR_NOTE` expands to
nothing in a build for the host, because the compiler is not an Arm target;
define `AAR_HOST` when an Arm-hosted compiler builds the runner.

CTest runs the native template tests, marked conformance payloads, contract
and queue/display fixtures, GL tests, source-level unit tests and libaar's
freestanding tests. `host-runner/CMakeLists.txt` lists the cases; the Python
drivers use SDL's dummy video and audio drivers, except for the GL tests, which
need a context. `gl` and `glconf` run `--headless` on SDL's offscreen driver
(EGL, Mesa llvmpipe is enough). `glwindow` runs in a window, on the display of
the machine or under `xvfb-run`. A GL test exits 77, which CTest reports as
skipped, when the host gives no GL group; set `AAR_TEST_REQUIRE_GL=1` to make
that a failure.

## Run

```text
./pixels --root . --scale 2
./filter --root "$PWD" -- in.bin out.bin
./console -- one
./pixels --headless --frames 2 --dump-front frame.bin
./mgl-conformance --scale 2
./mgl-conformance --headless --dump-front frame.bin
./mgl-conformance --no-gl
```

`argv[0]` passed to `aar_main` is this program's name. Arguments after the
runner options, or after `--`, are the app's arguments.

## Options

| Option | Meaning |
| --- | --- |
| `--root DIR` | Root for relative app paths. Default `.`. |
| `--abs` | Allow a path that contains `:`. The root check does not apply. |
| `--scale N` | Window scale, 1..8. The surface stays at the app's size. A GL window ignores it. |
| `--headless` | No visible window. SDL's dummy audio driver, and the offscreen video driver when GL is wanted (see "GL"), else the dummy video driver. |
| `--no-gl` | Leave the GL group out, as on a host without GL. |
| `--frames N` | Set the stop flag after N presented frames. |
| `--input FILE` | Scripted input events, injected before `aar_main`. |
| `--audio-null` | Consume PCM on `aar_yield` instead of opening SDL audio. |
| `--audio-frames N` | Stop after N consumed PCM frames. |
| `--audio-dump FILE` | Write consumed S16LE stereo frames (`--audio-null`). |
| `--msg-script FILE` | Inject host-to-app messages. |
| `--msg-out FILE` | Record app-to-host messages as 64 hex digits per line. |
| `--dump-front FILE` | Write the last presented surface, raw bytes, `pitch * height`. |
| `--help` | Print the option list. |

Runner paths (`--root`, scripts, dumps) are host paths. App paths go through
the file API. The environment variable `AAR_HOST_SELFTEST=1` runs the runner's
own conversion, input and SHA-256 self-tests instead of the app.

On exit the runner prints `aar-host: exit=<code> flips=<n> audio_frames=<n>`
to stderr.

### Input script

```text
# comment
key down 0x22 8
key up 34
mouse 2 -1 1
mousebtn 1 down
joy 1 -1 0 1
break 4
lowmem
resize 320 240
stop
```

`key` takes an Amiga raw code and optional qualifier bits. `break` is
`AAR_EV_BREAK` (`4`, `5`, `6` for Ctrl-D/E/F). `stop` sets the stop flag and
latches `AAR_EV_STOP`. `lowmem` latches an `AAR_EV_LOW_MEMORY` at once, for
an app that does not exhaust the heap. `resize` enqueues `AAR_EV_RESIZE`,
which a runtime never sends in API 0.5; it tests handling of an event the app
should ignore.

### Message script

Without `--msg-script`, `aar_msg_send` loops back into the queue
`aar_msg_poll` reads. With a script, lines are host-to-app messages and sends
are recorded instead of fed back. A line is 64 hex digits, a quoted string
padded with NULs to 32 bytes, or `stop`. `stop` requests stop once every
injected message has been sent back. Both queues hold `AAR_MSG_CAPACITY` (63)
messages, the capacity of a 64-slot queue with one slot kept free, and
`aar_info.msg_capacity` says so; a full send returns `AAR_E_BUSY`.

`aar_msg_wait` returns `AAR_OK` at once when a message is queued, and
`AAR_E_TIMEDOUT` when its timeout passes. It sleeps in 1 ms slices and keeps
pumping the runner, and SIGTERM, Ctrl-C, or a window close ends it with
`AAR_E_IO`. A queued message is reported before a stop. In loopback mode only
the app's own sends queue messages, so a wait on an empty queue runs to its
timeout.

## Keys

SDL scancodes become Amiga raw codes. Key-up is `AAR_EV_KEY_UP`; the up bit is
not stored in `code`. Qualifiers are Amiga `IEQUALIFIER` bits in `buttons`:
left/right shift `0x0001`/`0x0002`, caps `0x0004`, control `0x0008`, left/right
alt `0x0010`/`0x0020`, left/right Amiga (GUI) `0x0040`/`0x0080`, numeric pad
`0x0100`, repeat `0x0200`.

Ctrl-D/E/F (raw `0x22`/`0x12`/`0x23`) also enqueue `AAR_EV_BREAK` with codes
`4`/`5`/`6`, on key-down only. Window close and Ctrl-C set the stop flag and
latch one `AAR_EV_STOP`. Unmapped scancodes are ignored. The queue holds
256 ordinary events and drops the newest when full. `AAR_EV_STOP` and
`AAR_EV_LOW_MEMORY` are separate latches: a full queue cannot drop them, and
`aar_event_poll` delivers them before queued input. It never blocks.

| SDL | Raw | SDL | Raw |
| --- | --- | --- | --- |
| `` ` `` | `0x00` | A S D F G H J K L | `0x20`–`0x28` |
| 1–0 | `0x01`–`0x0A` | `;` `'` | `0x29` `0x2A` |
| `-` `=` `\` | `0x0B` `0x0C` `0x0D` | ISO `\` | `0x30` |
| keypad 0 | `0x0F` | Z–M | `0x31`–`0x37` |
| Q–P | `0x10`–`0x19` | `,` `.` `/` | `0x38`–`0x3A` |
| `[` `]` | `0x1A` `0x1B` | keypad `.` 7 8 9 | `0x3C`–`0x3F` |
| keypad 1 2 3 | `0x1D`–`0x1F` | space backspace tab | `0x40`–`0x42` |
| keypad 4 5 6 | `0x2D`–`0x2F` | keypad enter, return, esc, del | `0x43`–`0x46` |
| arrows up down right left | `0x4C`–`0x4F` | F1–F10 | `0x50`–`0x59` |
| keypad `-` `/` `*` `+` | `0x4A` `0x5C` `0x5D` `0x5E` | shifts, caps, ctrl, alts, GUI | `0x60`–`0x67` |

Both Control keys are raw `0x63`. SDL joystick 0 is Amiga port 1 (the game
port); joystick 1 is port 0. `dx`/`dy` are `-1`, `0`, or `1`. Fire is button
0 in `buttons` bit 0. Mouse movement is deltas. Mouse button codes are 1, 2,
3 for left, right, middle, and `buttons` bits 0, 1, 2 match.

## Differences from a target

### Info

`aar_get_info` reports `profile` from the data model of the host
(A64 on a 64-bit host, A32 on a 32-bit one), `platform_id` 0
(`AAR_PLATFORM_NONE`), `AAR_INFOF_SHARED_COHERENT`, a 1 MiB heap granule, a
4 KiB shared granule, `msg_capacity` 63, a 1 ms sleep granule, a stall limit
of 10000 ms (the minimum, though the runner has no watchdog), a stop grace of
0 (the runner never forces a stop), no CPU features, a shared limit of 16 MiB
and 16 files. `aar_ext` returns NULL for every extension.

### Time

Headless vsync does not wait for a 50/60 Hz blank. A windowed
renderer uses SDL vsync when the driver provides it. `aar_sleep_us` is
host-clock sleep, not a platform tick.

### Log

`aar_log` is printed immediately, truncated to `AAR_LOG_MAX`, with
a line end added. There is no rate limit.

### Display

Pitch is `width * bytes per pixel`; apps must use
`surface.pitch`. Layouts: RGB565 is `AAR_LAYOUT_RGB565_BE`, 32-bit is
`AAR_LAYOUT_BGRA8888`, 8-bit is `AAR_LAYOUT_CLUT8`. Palette entries are
`0x00RRGGBB`. There is one display, up to `AAR_DISPLAY_MAX_DIM` square. The
surface grants `AAR_DISPCAP_ASYNC` and `AAR_DISPCAP_CLUT8`.
`AAR_DISPLAY_LATEST` is granted (`AAR_DISPCAP_LATEST`) and uses three
buffers. A flip replaces the pending frame, counts it in
`aar_display_latest_dropped`, and `aar_display_flip_idle` stays 1. Without
it an async flip stays pending until `aar_yield` or `aar_display_vsync`, and
a flip or a refresh in the meantime is `AAR_E_BUSY`. `aar_display_flip_idle`
and `aar_display_vsync` without a display are `AAR_E_INVAL`.

### GL

The group is present when the runner finds an OpenGL compatibility context with
a depth buffer of at least 16 bits and every GL 1.1 entry point of the table,
which spec 8.5 asks of a platform that sets `AAR_GROUP_GL`. It looks once at
startup, with a hidden window and a throwaway context, and says why on stderr
when it finds none. The dummy video driver cannot make a context and gets no
message. `--no-gl` skips the look and keeps the group out.

A surface has a software window or a GL window. The software window shows the
converted surface through an SDL renderer. `aar_gl_bind(id)` needs the open
surface: another id, a second bind before `aar_gl_unbind`, and a context that
cannot be made all return NULL. A bind destroys the software window, opens a
window with an OpenGL pixel format at the same position, and creates the
context. The window has the size of the surface, so `--scale` does not apply
while GL is bound: an app's `glViewport` is in surface pixels. The pixel format
is RGB with no alpha, double buffered, with 24 bits of depth and 8 of stencil,
or else 24 of depth, or else 16. `aar_gl_unbind` deletes the context, makes the
software window again and shows the surface. `aar_display_close` with a context
bound releases it first. The window changes in a bind and an unbind, so it may
flicker.

`aar_gl_swap` calls `SDL_GL_SwapWindow` and counts one presented frame, as a
flip does (`--frames`). The host presents at once, so no swapped frame is in
flight. The swap waits for the display refresh in a window and not headless.
It then runs the host pump, so window input and a close request reach an app
whose loop only swaps, polls events and checks `aar_should_stop`. After a stop
the swap presents nothing.
GL draws into the window, and the surface's own buffers are left as the app set
them. With `--dump-front` a swap first reads the back buffer into the front
buffer of the surface, in the layout of the surface (RGB565 or BGRA8888; CLUT8
is not written), so that the dump and `aar_display_refresh` show the GL frame.
That costs a readback per frame, so it is off otherwise.

The table has `version` `AAR_GL_VERSION_ARRAYS` and the full `size`. A slot
points at a wrapper in the runner and not at the host function, because the
calling convention of the host's GL is not the app's (stdcall on 32-bit
Windows), and a slot whose entry the host lacks can stay NULL. The wrappers
are in `src/hostgl.c`, listed in two macros, one for the entry points a GL 1.1
host must have and one for optional ones; a slot that `aar/gl.h` appends is one
line in one of them, and a build check fails until it is there.

GLU is not linked. `Perspective` calls the host's `glFrustum`.
`Build2DMipmaps` takes `GL_TEXTURE_2D`, `GL_UNSIGNED_BYTE` and the formats
`GL_RGBA`, `GL_RGB`, `GL_LUMINANCE_ALPHA`, `GL_LUMINANCE` and `GL_ALPHA`. It
scales a size that is not a power of two to the power of two that GLU picks,
builds the levels with a box filter, uploads them, and returns 0, or `100900`
(enum), `100901` (size) or `100902` (memory) without touching GL. It honours
the unpack alignment, row length and skip settings. `ClientActiveTextureARB`
uses `glClientActiveTextureARB` or else `glClientActiveTexture`, and
`LockArrays` and `UnlockArrays` use the EXT entry points. When the host has
none of them the slot is NULL and the `gl*` wrapper of libaar does nothing.

`--headless` picks SDL's offscreen video driver, which makes contexts through
EGL. Mesa's llvmpipe is enough, so a machine without a GPU or a display can run
a GL app. No SDL renderer is made for the hidden window, because nothing would
show it. When the offscreen driver or EGL is missing, the runner says so on
stderr, falls back to the dummy video driver, and the GL group is absent. Many
SDL builds for Windows and macOS have no EGL in the offscreen driver, so there
GL needs a window.

The host GL is more capable than a card's renderer. It takes enums, formats and
texture sizes that the target does not handle, and it follows the GL
specification where a card's renderer makes its own choice. An app that is
right on the host can still draw wrongly on the target, so check the frame
there. The runner has no strict mode that rejects what the reference platform
lacks. The matrix of `templates/mgl_conformance.c` shows the effect: it records
what MiniGL on RasterIX draws, so on the host it reports the cases where GL
differs (the colour that an alpha texture gives, the alpha of an RGB or
luminance texture under `GL_REPLACE`, `GL_DECAL` on a format that has no
defined result) and exits 1. `glconf` requires the cases that the GL 1.1
specification fixes to match.

### Audio

Stereo S16LE with an 8192-frame ring at 22050, 44100 or 48000 Hz;
the granted rate is the requested one. `aar_audio_get_stats` counts an
underrun when the SDL callback finds the ring short of a buffer; with
`--audio-null` nothing underruns. The ring is plain host memory shared with
the SDL callback.

### Shared buffers

`aar_shared_alloc` takes ordinary process memory under
a 16 MiB limit, in 4 KiB granules, and at most 64 app blocks. The memory is
zeroed. `haddr` is a synthetic token from a 32-bit space that starts at
`0x033F0000`, aligned to the block's alignment; the lowest free range is
reused after a free. The token is not an address in the process.
`aar_host_shared_resolve(haddr, offset, bytes, &p)` (header `aar_host.h`)
turns it back into the memory, as the client side of a real platform would.
The host is coherent, so `aar_publish` and `aar_acquire` are fences and
`AAR_SHAREDF_COHERENT` is set. An alignment is rounded up to the granule and
must then be a power of two.

### Files

The path policy of the standard applies. Relative paths stay under
`--root`. A parent step above it or a leading `/` that would leave it is
`AAR_E_ACCES`, and so is a `:` unless `--abs`. `//` is a parent step,
as in an AmigaDOS path. `aar_map_file` reads at most 16 MiB into a
runtime-owned shared block, so the file counts against the shared limit.
A larger file is `AAR_E_NOSPC` and a zero-length file is `AAR_E_NOMEM`.

### Console

`printf` uses the host C library. On a target, newlib routes fd
1 and fd 2 through the console calls. `aar_console_write` writes stdout;
`aar_console_read` reads stdin and returns `AAR_OK` with zero bytes at end
of input.

### Heap

`aar_heap_grow` and `aar_heap_shrink` commit and release whole 1
MiB frames inside a reserved 8 MiB arena. The range does not move. Growth
past that limit, or when the arena could not be reserved, returns
`AAR_E_NOMEM`. A zero or non-MiB size is `AAR_E_INVAL`. `aar_get_heap_info`
reports the commit, that limit, and the uncommitted remainder as `pool_free`.
`aar_alloc` and `aar_free` use the host `malloc`; an app that uses
`malloc` directly never touches the arena. The arena is the pool for
`AAR_EV_LOW_MEMORY`. The event is queued when the free pool falls below
1 MiB (low-water mark), and again only after it has risen above 2 MiB
(high-water mark). An app that grows the heap until `AAR_E_NOMEM` gets one
event; the input script can also enqueue one.

### Services

SHA-256 is implemented, including an empty buffer.
`aar_decompress`, `aar_image_open` / `aar_image_decode` / `aar_image_close`,
and `aar_audio_decode_open` / `aar_audio_decode` / `aar_audio_decode_close`
return `AAR_E_NOSYS`, which the services group allows for unavailable codecs.

### Isolation

The host is not an isolated runtime. A bad pointer crashes the
process instead of returning `AAR_E_FAULT`, and `isolate` does not run here.
