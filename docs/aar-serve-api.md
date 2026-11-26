# aar_serve: the serving loop of an aar.library client

`aar_serve` answers the service requests of an app that runs on a platform
behind `aar.library` ([spec section 9.6](../spec/aar.md)): files, time, locale, environment and clipboard, console, display and audio. It
also posts the input events and the stop event. The reference launcher
[`aar-run`](../client/run/README.md) is a small program around it; a program
with its own window and event loop can use it the same way.

It is MIT code in this repository. `client/serve/aar_serve.c` is the one
translation unit of the loop and includes the Amiga backends.
`client/serve/aar_serve_core.c` holds the OS-free request handlers. The public
header is `libraries/aar_serve.h`. The calls are plain C functions linked into
the program, not library vectors. Link `libaarserve.a`, or compile both `.c`
files below, and supply the program's `AARBase`, `IntuitionBase` and `GfxBase`.
The Amiga link also needs `-lamiga` and `-noixemul`.

```sh
m68k-amigaos-gcc -std=gnu11 -noixemul -m68030 -O2 -Iclient/include -Iinclude \
    prog.c client/serve/aar_serve.c client/serve/aar_serve_core.c -lamiga
```

## Rules

- One `aar_serve` serves one `AARHandle`. The caller opens the handle and
  controls options, load and start. It creates the serving loop after open,
  steps it while serving is needed, destroys it and closes the handle.
  `aar_serve` never opens or closes the handle and never loads or starts an app.
- Every call comes from the task that opened the handle (`AAR_E_OWNER`
  otherwise). The hooks run in that task, inside `aar_serve_step`. Nothing runs
  in an interrupt.
- The program supplies `AARBase`, `IntuitionBase` and `GfxBase`.
  `aar_serve` opens `timer.device`, `Picasso96API.library` (when there
  is one), `ahi.device` (for the AHI audio route), `lowlevel.library` (only for
  `cd32_ports`) and `keymap.library` (only for a display that asks for text
  input) itself and keeps those bases to itself. It opens `locale.library`
  and `iffparse.library` at the first request that needs them and closes
  them at `aar_serve_destroy` (see "Time, locale, environment and clipboard"
  below).
- The calling task needs `AAR_SERVE_MIN_STACK` bytes of stack (16 KiB); a
  palette update holds a 3 KiB table. `aar_serve` does not check it.
- A request's buffer is touched only between the request and its completion,
  and only while the claim is live.
- An op that is not served completes with `AAR_E_NOSYS`.

## Configuration

`aar_serve_config_default` fills a `aar_serve_config` with its size and the
defaults; change the fields you need and pass it to `aar_serve_create`. The
structure is size-first and grows at the end only.

| Field | Meaning |
|---|---|
| `flags` | `AAR_SERVEF_*`, below |
| `root` | a lock the app's relative paths resolve under; duplicated by `aar_serve_create`. 0: a duplicate of the current directory |
| `screen_mode` | the Picasso96 DisplayID to force; 0 chooses by size and format |
| `screen_title` | title of the screen and of requesters; NULL: `aar-serve` |
| `console_in`, `console_out` | the streams of the console; 0: `Input()` and `Output()`, each default independent of the other (an output stream alone leaves reads on `Input()`). A stream the caller gives is the caller's to close |
| `console_spec` | opened at the first console use when `console_out` is 0 (a Workbench start, for example `CON:0/0/640/160/aar-run/CLOSE/WAIT`). That window is one bidirectional console: with `console_in` 0 it is the input as well. NULL: none, console I/O then fails as `AAR_E_IO` |
| `audio_route` | `AAR_AUDIO_ROUTE_PLATFORM` or `AAR_AUDIO_ROUTE_AHI`: what this loop serves; it must equal the `AAR_TAG_AUDIO_ROUTE` the handle was given |
| `groups` | `AAR_GROUP_*` to serve; the display and audio ops of a group not in the mask complete with `AAR_E_NOSYS`. 0: every group |
| `joy_ports` | bit 0 port 0, bit 1 port 1; default `AAR_SERVE_DEFAULT_JOY_PORTS` (port 1 only). These ports are read from `JOYxDAT` and the CIA: the axes and one fire button |
| `cd32_ports` | bit 0 port 0, bit 1 port 1: read these ports with `lowlevel.library` `ReadJoyPort`, which reports the seven buttons of a CD32 pad. A port named here is polled whether or not `joy_ports` names it. Opt-in, because `ReadJoyPort` drives the port pins to sense the controller. `aar_serve_create` answers `AAR_E_NOSYS` when `lowlevel.library` does not open. Default 0 |
| `poll_us` | the heartbeat of the timer in microseconds; 0: none. Without a heartbeat the loop still wakes for what waits on time: a stop that was asked or a stop event that found the queue full (10 ms), the joysticks of the input window (20 ms), completions waiting for room in the completion queue (1 ms), a claim that is revoked without a signal (250 ms), and the timeout of a waiting `DISPLAY_VBLANK`. Default `AAR_SERVE_DEFAULT_POLL_US` |
| `break_mask` | the task signals the loop consumes: Ctrl-C stops the app, Ctrl-D, Ctrl-E and Ctrl-F become `AAR_EV_BREAK` events. A signal left out is the program's: the loop's DOS packet and clipboard waits and the keys of its window do not wait on it, test it, or clear it either. Default `AAR_SERVE_BREAK_ALL`; 0: the program owns its signals |
| `display_strategy` | `AAR_SERVE_DISPLAY_*`, the first strategy to try |
| `hooks` | an `aar_serve_hooks`, may be NULL |
| `user` | passed to every hook |
| `platform` | reserved, 0 |

`AAR_SERVEF_ABS_PATHS`: paths with a volume or assign colon are allowed and
`root` does not confine them. `AAR_SERVEF_KEEP_MESSAGES`: the program calls
`AARGetMessage` itself. `AAR_SERVEF_NO_INPUT_WINDOW`: no backdrop input window;
the program feeds `aar_serve_post_event`. `AAR_SERVEF_USE_SIGNAL`: arm the
handle and add the library signal to `aar_serve_sigmask`. `AAR_SERVEF_NO_ENV`:
the app's reads of environment variables are refused with `AAR_E_ACCES`.
`AAR_SERVEF_NO_CLIPBOARD`: the app's clipboard reads and writes are refused with
`AAR_E_ACCES`. Both are served when the flag is clear.

`display_strategy`: `AAR_SERVE_DISPLAY_BUFFERS` tries two screen buffers, then a
three page tall screen that is panned, then a two page one, then a single buffer
that the platform copies. `AAR_SERVE_DISPLAY_PAN3`, `AAR_SERVE_DISPLAY_PAN2` and
`AAR_SERVE_DISPLAY_SINGLE` start later in that list.

## Hooks

`aar_serve_hooks` is a size-first table of optional callbacks, all called in the
owner task with `user`.

| Hook | When |
|---|---|
| `status` | a launcher diagnostic line (no newline). Default: queued to the console writer |
| `log` | one app log record, valid for the call. Default: queued as a diagnostic line |
| `message` | one app message, when `AAR_SERVEF_KEEP_MESSAGES` is not set |
| `stop_wanted` | the loop wants the app stopped, with a reason `AAR_SERVE_WHY_*`. Default: stop it |
| `display` | a `struct aar_serve_display_ops` replacing the Picasso96 screen |
| `console` | a `struct aar_serve_console_ops` replacing the console |

Reasons: `AAR_SERVE_WHY_BREAK` (Ctrl-C, also the raw key in the display window),
`AAR_SERVE_WHY_WINDOW` (the input window was closed), `AAR_SERVE_WHY_CONSOLE`,
`AAR_SERVE_WHY_DISPLAY`, `AAR_SERVE_WHY_AUDIO` (the output cannot be opened or
written) and `AAR_SERVE_WHY_QUEUE` (the request queue is corrupt). The hook may
call `aar_serve_stop` now or later, or not at all; `aar_serve_stop` itself does
not call the hook.

## Requests and the replaceable tables

`struct aar_serve_req` is a request as `aar.library` gives it out with its
payload buffer mapped into this process; `struct aar_serve_done` is the
completion a handler produces: `result` (`AAR_OK` or an `AAR_E_*`), the entry
`flags` and the result words `out[]`.

`struct aar_serve_display_ops` is the table under the display handlers: `open`
(required; opens the screen and fills the completion with `AAR_OK` and the
pitch, front, back and spare tokens), `change_buffer`, `pan`, `wait_tof`,
`load_rgb32` and `close` (all optional). The loop records the screen from the
completion that `open` filled, so flips, palettes and the close work on a
replacement table. The table decides the mode:

- `change_buffer` and a back token make two screen buffers (a flip completes at
  `aar_serve_flip_shown`).
- Otherwise `pan` and a back token make the pages of one bitmap.
- Otherwise there is one buffer.

An `open` that gives no front token or pitch has opened nothing: the result is
`AAR_E_IO` and `close` is called. A negative result is the table's own failure.
With `load_rgb32` NULL, palette requests complete with `AAR_E_NOSYS`. `close` is
called at `DISPLAY_CLOSE` and at `aar_serve_destroy`.

Seven more members serve the ops of API 0.6, all optional. `present` is called
after each flip of a one buffer display (the built-in table draws its window or
its scaled screen there). `info` fills the refresh rate, the screen size and the
`AAR_DISPINFO_ACTIVE` flag of `DISPLAY_INFO`; with it NULL the refresh rate is
unknown and the screen is the surface. `modes` writes the first records of the
table's modes and returns how many it has in all. `resize` is called for
`DISPLAY_RESIZE` as `open` is, with the new size in the request's open arguments,
and fills the same completion; without it, or without `AAR_DISPCAP_RESIZE`, the
call answers `AAR_E_NOSYS`, which a replacement table always does, because it is
granted no cap. `set_title` gets the title as ISO-8859-1. `set_pointer` gets the
mode and, for `AAR_POINTER_CUSTOM`, the size, the hot spot and the big-endian
ARGB words. `vblank` is told to watch (1) or stop (0) and returns 0 when it
cannot see a vertical blank; the program then calls `aar_serve_vblank` at each
one. A call a table does not have answers `AAR_E_NOSYS`.

`struct aar_serve_console_ops` is the table under the console handlers:
`write_start`, `read_start` and `live` are required, `open`, `read_allowed` and
`output_error` optional. A start that returns 1 is completed by exactly one
`aar_serve_write_done` or `aar_serve_read_done`, outside the table call, in the
task that runs the loop. `output_error` is called after the loop has asked the
app to stop. `aar_serve_destroy` does not wait for a replacement console. The
first member of both tables is the `ctx` that every call receives.
`aar_serve_create` answers `AAR_E_INVAL` for a table without what it requires.
The other hooks (`status`, `log`, `message`, `stop_wanted`) are synchronous
notifications that nothing completes, and the audio route cannot be replaced.

## The display and the input of API 0.6

The display flags of `aar_display_open` are requests. The built-in display
grants the caps for the ones it could meet and leaves the others out of the
reply, so an app reads `aar_surface.caps` to see what it got. A replacement
display table is granted none of the caps below, because the table, not the loop,
owns the window.

`AAR_DISPLAY_WINDOW` shows the surface in a window on the default public screen.
The surface is then a bitmap of its own in the requested format, made with
`p96AllocBitMap` on the board and named by a token like any other surface. The
token is the address of the bitmap, which Picasso96 may move while it is not
locked, so `p96LockBitMap` pins it before the token exists and the lock is kept
until the surface is replaced by a resize or the display is closed. A bitmap
that cannot be locked is not used (the display opens without the window).
There is one buffer, and each flip draws it into the window with
`p96WritePixelArray` from the RenderInfo of that lock, without locking again,
at the top left of the inner area and clipped to it. The window has a drag bar,
a depth gadget and a close gadget, the title of `screen_title`, and the inner
size of the surface. The cap `AAR_DISPCAP_WINDOW` is granted when the public
screen is a Picasso96 screen of at least 16 bits per pixel and the bitmap could
be made and named. Otherwise the display opens as it does without the flag, on a
screen of its own, and none of the caps that need a window are granted. A CLUT8
surface always takes a screen of its own, because its palette would change the
public screen's.

`AAR_DISPLAY_RESIZABLE` needs `AAR_DISPLAY_WINDOW`: the window gets a size gadget
in its border and the input window's `IDCMP_NEWSIZE` becomes `AAR_EV_RESIZE` with
the new inner size. It is granted as `AAR_DISPCAP_RESIZE` only with an input
window. `DISPLAY_RESIZE` then makes a new surface bitmap of the size the app
asked for, locks it, frees the old one after it (releasing its lock once), and
answers as `DISPLAY_OPEN` does; a bitmap that cannot be made or locked leaves the
display as it was and answers `AAR_E_NOMEM`. It
answers `AAR_E_BUSY` while a flip is pending (a window display has none, a
replacement table may).

`AAR_DISPLAY_POINTER_ABS`: the input window reports the pointer position in
`dx` and `dy` of every `AAR_EV_MOUSE`, relative to the top-left pixel of the
surface, with `AAR_MOUSE_ABSOLUTE` in `buttons`. It leaves out `IDCMP_DELTAMOVE`.
In a window the position is taken from the inner area, and on a scaled display
it is mapped back through the rectangle the surface is drawn in, rounded down.
It can lie outside the surface. The client library keeps the latest of two
absolute positions when it has to merge motion. Granted with an input window.

`AAR_DISPLAY_TEXT_INPUT`: each key press is also mapped with `keymap.library`
(`MapRawKey`, the default keymap, with the previous keys of a dead key sequence
that Intuition keeps in the message) and every character it makes is sent as an
`AAR_EV_TEXT` after the key event, with the qualifiers of the key. Latin-1
characters are the code points of the same number. Control characters, the
escape sequences of cursor and function keys, and a key pressed with an Amiga
key held make none. A key release makes none. The cap is granted with an input
window when `keymap.library` opens, which the loop does at the first display
that asks for text.

`AAR_DISPLAY_CLOSE_REQUEST`: the close gadget of a window display sends
`AAR_EV_CLOSE_REQUEST` and does not stop the app. Ctrl-C and the other break
keys still do. The cap is granted for a window with an input window only: a
display on a screen has no close gadget.

`AAR_DISPLAY_SCALE`: the client shows the surface scaled to its window or its
screen, keeping the aspect ratio, centred, with the rest of the area black. A
window display follows its window: a size change redraws the frame at the new
size. On a screen the surface is a bitmap of its own again, and the screen has the
size of the Workbench screen, in the mode that suits the surface's format. The
cap `AAR_DISPCAP_SCALED` is granted when that surface could be made. A BGRA8888
surface, an RGB565 surface and a CLUT8 surface alike, is scaled by the loop,
nearest neighbour, in strips of 16 rows drawn with `p96WritePixelArray` in the
surface's own format. `cybergraphics.library` `ScalePixelArray` is not used: it
takes RECTFMT_RGB, RECTFMT_RGBA, RECTFMT_ARGB (alpha first) and RECTFMT_GREY8
only, none of them the byte order B, G, R, A of a BGRA8888 surface.

Two events need no flag. `AAR_EV_ACTIVE` is sent for `IDCMP_ACTIVEWINDOW` and
`IDCMP_INACTIVEWINDOW`, with code 1 and 0, so also once when the display opens
active. The NewMouse raw keys 0x7A and 0x7B (up and down) and 0x7C and 0x7D (left
and right) become `AAR_EV_WHEEL` with one step in `dy` (positive down) or `dx`
(positive right) and the qualifiers; neither the press nor the release of a
wheel key is sent as a key. `AAR_EV_MOUSE` carries the button that changed in
`code` (0 for motion, else 1 left, 2 right, 3 middle) and the state of all the
buttons in `buttons`.

`DISPLAY_INFO` reports the surface size, the size of the screen it is on, the
refresh rate in millihertz and the flags `AAR_DISPINFO_WINDOW` and
`AAR_DISPINFO_SCALED` (what was granted) and `AAR_DISPINFO_ACTIVE` (the window
has the focus, or for a display with no window of ours its screen is the active
one). The refresh rate comes from the monitor of the mode shown (`MonitorInfo`
total rows and colour clocks of 280 ns) and is 0 when that gives nothing sensible
(below 20 Hz or above 300 Hz).

`DISPLAY_MODES` lists the Picasso96 modes of the display database that are
available and show at least one of the surface formats, up to 256 of them, with
their size, refresh rate and the formats the mode is compatible with. The first
mode of the size of the Workbench screen is marked `AAR_MODEF_PREFERRED`. It
needs no open display. Without `Picasso96API.library` the list is empty.

`DISPLAY_TITLE` converts the UTF-8 title to ISO-8859-1 (a character above U+00FF
and a malformed sequence become `?`) and sets it as the title of the window of a
window display. On a screen it is the screen title, set through the input
window, or on the screen itself when the program has no input window of ours.

`DISPLAY_POINTER` acts on the window of the display, or its input window. The
default pointer is the system's. The busy pointer is `WA_BusyPointer`. A hidden
pointer and a custom one are sprite images that need Intuition 39: a two plane
bitmap in a `pointerclass` object, with the hot spot as the object's offset. The
ARGB pixels (alpha below 0x80 is transparent) are quantised to the nearest of the
three pointer colours of Preferences (colours 17 to 19, read with `GetPrefs`), so
the image takes the user's pointer colours. The image is at most 64 by 64. A
program with no window of ours has nothing to set it on and the call answers
`AAR_E_NOSYS`.

`DISPLAY_VBLANK` parks the app until the next vertical blank of the display's
screen and does not block the loop. The built-in displays (a Picasso96 screen, a
window, a scaled surface) cannot see that blank: the vertical blank that
`timer.device` counts is the chipset's, not an RTG board's, so they answer
`AAR_E_NOSYS`. A replacement display table whose `vblank` call watches answers it:
the loop completes a parked call when the program calls `aar_serve_vblank`, and
after `arg1` microseconds (rounded up to a millisecond; the loop wakes for it even
with no heartbeat) with `AAR_E_TIMEDOUT`. A close or the destroy of the loop ends
it with `AAR_E_IO`. A platform that can serve the call itself does so before it
reaches the client.

The joystick ports are read from `JOY0DAT`, `JOY1DAT` and the CIA by default,
which changes no state: one fire button, the axes of a digital stick. The second
and third button of a stick are on the POT lines. Those belong to
`potgo.resource` and the input handlers, and a mouse in the port puts its right
and middle buttons on the same lines, so they are not read. A port in
`cd32_ports` is read with `lowlevel.library` `ReadJoyPort` instead, and reports
all seven buttons of a CD32 pad as `AAR_JOY_*` bits with the axes. It is read
about once a frame. A port with nothing a pad or a joystick in it (a mouse, no
device) reads as released. `ReadJoyPort` senses the controller by driving the
port pins, which is why it is not the default: an earlier launcher that used it
left the left mouse button repeating and releasing on its own after it had
exited. The window must have the focus for either kind of reading.

## Files, time, locale, environment and clipboard

These requests are served by the OS-free handlers of `aar_serve_core.c` over
AmigaDOS packets (files) and the system calls of `aar_serve_host.c`. A request
that is not wanted any more (the claim is lost) is answered with `AAR_E_IO`
before it touches anything.

### Files

Every request with a path uses the path rules of `FILE_OPEN` (see Notes): a
relative path is resolved below `ROOT`, `..`, a leading `/` and a link out of
`ROOT` are `AAR_E_ACCES`, a path with a volume or assign needs
`AAR_SERVEF_ABS_PATHS`, and an empty path is `AAR_E_INVAL`. A path that names
`ROOT` itself, a directory reached with `..`, or only a volume (`Work:`) has no
name to act on and is `AAR_E_ACCES` for the requests below that change a name.

| Request | AmigaDOS | Notes |
|---|---|---|
| `FILE_DELETE` | `ACTION_DELETE_OBJECT` | a file or an empty directory |
| `FILE_RENAME` | `ACTION_RENAME_OBJECT` | Both paths are checked. A volume or assign name is sent to the first `GetDeviceProc` candidate only. If both names identify the same object, the call succeeds without changing it, with or without `AAR_RENAME_REPLACE`. Before deleting an existing target for `AAR_RENAME_REPLACE`, `SameLock` must confirm that the objects are on the same volume. A replacement that would delete a target is refused with `AAR_E_INVAL` if either name uses a multi-directory assign: the delete and rename could reach different directories. A cross-volume refusal leaves both objects unchanged. |
| `DIR_CREATE` | `ACTION_CREATE_DIR` | one directory; the lock the handler returns is released at once |
| `FILE_SET_DATE` | `ACTION_SET_DATE` | see Time below |
| `FILE_SET_PROTECT` | `ACTION_SET_PROTECT` | the bits `AAR_PROT_*` (0 to 7); any other bit is `AAR_E_INVAL`. Bits 0 to 3 deny, as AmigaDOS has them |
| `FILE_TRUNCATE` | `ACTION_SET_FILE_SIZE`, `ACTION_WRITE` | an id opened with `AAR_FILE_WRITE`, else `AAR_E_BADF`. Shorter: one `ACTION_SET_FILE_SIZE`, whose answer is checked: with another handle open on the file the handler cuts only as far as that handle's position allows and answers the size it kept, not an error. A size other than the one asked is `AAR_E_BUSY` and the file stays longer. `FILE_OPEN` with `AAR_FILE_TRUNCATE` is `AAR_E_BUSY` for the same reason when the file could not be emptied. Longer: the new bytes are written as zeros at the end, because handlers differ on what a longer `ACTION_SET_FILE_SIZE` leaves in them. The position stays where it was, unless the file now ends before it. An extension that fails part way (full volume, write error) leaves the file longer by what was written, but the position is put back where it was (as far as a seek can) and the error of the failed write is the answer. A size from 2 GiB is `AAR_E_INVAL` |
| `VOLUME_INFO` | `ACTION_INFO` | out: blocks total, blocks used, bytes per block, and `AAR_VOLF_READONLY` when the disk is write-protected |
| `FILE_STAT` | `ACTION_EXAMINE_OBJECT` | out5 is `fib_Protection` as `AAR_PROT_*`, bits 0 to 7 |

Every request that changes the volume (delete, rename, create, set date, set
protection, truncate) is refused with `AAR_E_IO` once a stop is under way, as
opens and writes are (see Notes). `VOLUME_INFO` changes nothing and still runs.

AmigaDOS errors become `AAR_E_*` in one function, `aar_serve_errno_from_io`:

| `IoErr()` | Result |
|---|---|
| `ERROR_OBJECT_NOT_FOUND`, `ERROR_DIR_NOT_FOUND`, `ERROR_DEVICE_NOT_MOUNTED`, `ERROR_NO_MORE_ENTRIES` | `AAR_E_NOENT` |
| `ERROR_OBJECT_EXISTS`, `ERROR_DIRECTORY_NOT_EMPTY` | `AAR_E_EXIST` |
| `ERROR_DELETE_PROTECTED`, `ERROR_WRITE_PROTECTED`, `ERROR_READ_PROTECTED` | `AAR_E_ACCES` |
| `ERROR_OBJECT_IN_USE` | `AAR_E_BUSY` |
| `ERROR_RENAME_ACROSS_DEVICES`, `ERROR_SEEK_ERROR` | `AAR_E_INVAL` |
| `ERROR_DISK_FULL` | `AAR_E_NOSPC` |
| `ERROR_NO_FREE_STORE` | `AAR_E_NOMEM` |
| any other | `AAR_E_IO` |

A packet the handler never got, or whose answer was thrown away after a break,
is `AAR_E_IO` whatever `IoErr()` says.

### Time, locale, environment and clipboard

These requests are always served; the two policy flags only turn the environment
and the clipboard off.

`TIME_UNIX` and `LOCALE_INFO` use `locale.library` (version 38, opened at the
first request that needs it: a time, a locale, a `FILE_STAT` that has a date, a
`FILE_SET_DATE`). The Amiga clock is the user's local time, and `loc_GMTOffset`
counts minutes west of GMT, so

- UTC is the `DateStamp` as Unix seconds (it counts from 1978-01-01, 2922 days
  after 1970-01-01) plus the minutes west times 60.
- `TIME_UNIX` answers Unix microseconds (the ticks are fiftieths of a second)
  and `AAR_LOCALEF_OFFSET_KNOWN` when `locale.library` gave the offset.
- `FILE_STAT` mtime and `FILE_SET_DATE` use the same conversion, so a date that
  was set reads back the same. An unset stamp is mtime 0. A time before 1978-01-01
  on the user's clock, or after 2106, is `AAR_E_INVAL` for `FILE_SET_DATE`.
- Without `locale.library` the offset is 0 and the flag is clear. A locale
  record then is all zeros and `AAR_OK`.

`LOCALE_INFO` fills the 64 byte record of `transport.h`: the offset in minutes
east (the negated `loc_GMTOffset`), the flag, the language name without
`.language` (31 characters at most), the country code (the vehicle registration
code in `loc_CountryCode`: its non-zero bytes, most significant first, as text such as `D` or `GB`, 7 at most), and the decimal point and group separator
converted from ISO-8859-1 to UTF-8 (whole characters, 3 bytes at most each).

`ENV_GET` reads `ENV:<name>` with `GetVar()` and `GVF_GLOBAL_ONLY |
GVF_BINARY_VAR`, so the bytes are the stored ones. It uses the result of
`GetVar()` of dos.library 37 and later as documented: the return is the number
of bytes copied (a value that does not fit is cut to one byte less than the
buffer and NUL terminated), and `IoErr()` is the full length of the
variable. The backend takes the full length from there. It fetches again, with
a buffer of that size (up to 1 MiB less one byte), only a value that the
request can hold and the first 1 KiB buffer could not; a value longer than the
request's room is counted by the first call. A variable that changes while it
is read is fetched again, in four calls at most, then it is `AAR_E_IO`, as is
a dos.library that leaves `IoErr()` below the bytes copied. The reply carries
the real length in out0. The value goes to the request's buffer, which also
held the name: the first `min(buf_len, length)` bytes, and a NUL after them
when the length is below `buf_len`. A name of 1 to 255 bytes with a NUL inside
the buffer is required (`AAR_E_INVAL`). A name with a volume (`:`), a leading
`/`, an empty component (`//` anywhere, or a `/` at the end: in AmigaDOS each
slash after the first of a run is a parent) or a `..` could leave `ENV:` and is
`AAR_E_ACCES`; it never reaches `GetVar()`. An unset variable is `AAR_E_NOENT`.

`CLIP_READ` and `CLIP_WRITE` use `clipboard.device`, opened through
`iffparse.library`'s `OpenClipboard()` (version 37) at the first clipboard
request. Without the library, or when the device does not open, they answer
`AAR_E_NOSYS`. The units are 0 to 255 (`AAR_E_INVAL` above), and the types
`AAR_CLIP_TEXT` and `AAR_CLIP_IFF`.

- Text is read from the first `FORM FTXT` of the clip: its `CHRS` chunks in
  order (the text itself). The charset is in a `CSET` chunk: code set 106
  (UTF-8) passes the bytes through; every other code set, and no `CSET`, is
  ISO-8859-1 and is converted to UTF-8. Those two are the charsets this
  backend handles. A clip with no form, another kind of form or no text is
  `AAR_E_NOENT`.
- Text is written as `FORM FTXT` with one `CHRS` chunk in ISO-8859-1 and no
  `CSET` chunk. A character above U+00FF, and a byte that is not UTF-8,
  becomes `?`.
- `AAR_CLIP_IFF` is the first form of the clip with its pad byte, as it is. A
  write needs a `FORM` whose size fits the data; the clip gets that form only.
- A clip is moved in chunks. A read sequence is one clip: the clip is taken at
  offset 0 and kept until the last byte was read or offset 0 is asked again,
  so a program that changes the clipboard in between does not mix two clips. A
  write sequence is collected in memory and written to the device when the
  chunk with `AAR_CLIPF_LAST` arrives; a sequence that does not reach it, or
  breaks the order (an offset that is not the next one, a changed unit, type or
  total), changes nothing. A new chunk at offset 0 drops an unfinished sequence.
  A clip is at most 8 MiB (`AAR_SERVE_CLIP_MAX`), else `AAR_E_NOMEM`. The limit
  is on what the app moves: the UTF-8 text for `AAR_CLIP_TEXT` (so a text of
  exactly 8 MiB written is read back whole, though its `FTXT` wrapper makes
  the stream 20 bytes longer, and ISO-8859-1 text counts the two bytes of
  every character from U+0080), and the whole form for `AAR_CLIP_IFF`. The
  device's stream is taken with room for the longest text's wrapper
  (`AAR_SERVE_CLIP_RAW_MAX`, 64 bytes more) and read to its end; a longer one
  is `AAR_E_NOMEM` without being kept, and the text or form limit is checked on
  what was taken.
- Every device command is an asynchronous I/O request (`SendIO`), waited for
  on its reply port, the heartbeat timer and Ctrl-C (when `break_mask` has it),
  as a DOS packet is. `clipboard.device` keeps a read or write back while
  another program has a read or write sequence open on the unit (the reader
  has not read past the end yet, the writer has not sent `CMD_UPDATE`), so a
  request can wait as long as that program likes. A Ctrl-C (it also asks the
  app to stop) or a claim the heartbeat finds revoked abandons the request: it
  is aborted with `AbortIO`, once, and answers `AAR_E_IO`. Only the device's
  reply ends the wait. The request and its buffers belong to the device until
  then and the handle is closed only after, so a device that does not honour
  `AbortIO` is waited for; the loop is not responsive in the meantime, exactly
  as with a handler that does not reply. A command that was abandoned before
  it was sent is not sent. The clip is replaced only by a `CMD_UPDATE` that
  was sent after its `CMD_WRITE` completed in time: an abandoned or failed
  write is dropped when the handle closes and the old clip stays. `CMD_UPDATE`
  does not wait for another program, so it is not aborted: it is waited out
  and its own result stands.

## Calls

### aar_serve_config_default

`void aar_serve_config_default(aar_serve_config *cfg)`: fills `*cfg` with its
size and the defaults.

### aar_serve_create

`LONG aar_serve_create(AARHandle *h, const aar_serve_config *cfg, aar_serve **out)`:
validates the configuration (`AAR_E_INVAL`), checks the handle (`AAR_E_OWNER`,
`AAR_E_STALE`), allocates about 26 KiB, reads `AARInfo.stop_grace_ms`,
duplicates the root, opens `timer.device` (`AAR_E_NOSYS` when there is none) and
`Picasso96API.library`, installs the log sink with `AARSetLogSink` and arms the
heartbeat. `AAR_E_NOSYS` also for the AHI route with a `poll_us` of 0 or above
20000, and for a `cd32_ports` that names a port when `lowlevel.library` does not
open. On an error `*out` is NULL and nothing is left behind (`AAR_E_NOMEM` for
memory).

### aar_serve_run

`LONG aar_serve_run(aar_serve *s)`: waits on `aar_serve_sigmask` and steps until
the app has ended. Returns `AAR_SERVE_ENDED` or `AAR_SERVE_LOST`.

### aar_serve_sigmask

`ULONG aar_serve_sigmask(const aar_serve *s)`: every signal the loop waits on
now: the break signals of `break_mask`, the timer, the console packet port, the
input window, the SafeMessage port of a flip and the AHI port when they exist,
and the library signal with `AAR_SERVEF_USE_SIGNAL`. It changes while the
app runs, so ask again before each `Wait`. The heartbeat timer is armed whenever it
is in the mask. `aar_serve_create`, the end of every step and every call that
waits on DOS (`aar_serve_locate` included) arm it again, so a program may call
those between `aar_serve_create` and its first `Wait`.

### aar_serve_step

`LONG aar_serve_step(aar_serve *s, ULONG signals)`: one pass of the loop;
`signals` is what `Wait` returned. It takes the break signals, drains the input
window and the joystick, flushes the stop event, serves up to 16 requests, pumps
the console, the display (a flip's SafeMessage, the timeout of a
`DISPLAY_VBLANK`) and the audio, flushes the completion backlog and reads
the state. It returns `AAR_SERVE_CONTINUE` until the app has ended and the
output that must be written is written. Then it returns `AAR_SERVE_ENDED`, also
when both grace periods have elapsed after a stop. It returns `AAR_SERVE_LOST`
when the claim is gone (revoked, quarantined, reset).

### aar_serve_post_event

`LONG aar_serve_post_event(aar_serve *s, const aar_event *ev)`: posts an input
event for the app. A stop event that finds the queue full is kept and posted
again on every pass until it is delivered.

### aar_serve_flip_shown

`void aar_serve_flip_shown(aar_serve *s)`: a replacement display table tells the
loop that the flip it started with `change_buffer` is on the screen.

### aar_serve_vblank

`void aar_serve_vblank(aar_serve *s)`: a replacement display table, whose
`vblank` call was told to watch, tells the loop that a vertical blank has come.
A parked `DISPLAY_VBLANK` completes with `AAR_OK`. Ignored when none is parked.
The built-in table does not need it.

### aar_serve_write_done

`void aar_serve_write_done(aar_serve *s, LONG written)`: a replacement console
tells the loop how the write that `write_start` started ended: `written` bytes
were taken. If that is all of them, the app's request (or the launcher's line)
completes. Fewer, or a negative value, fails the output: the app is stopped once
and every later write completes with `AAR_E_IO`. Ignored when no write is started.

### aar_serve_read_done

`void aar_serve_read_done(aar_serve *s, LONG got, const void *data)`: a
replacement console tells the loop how the read that `read_start` started ended:
`got` bytes in `data` (at most the `max` it was given; copied before the call
returns), or 0 or less at the end of input, which stays the end. Ignored when no
read is started.

### aar_serve_stop

`void aar_serve_stop(aar_serve *s)`: asks the app to stop: the stop event, then
`AARStop`. Idempotent.

### aar_serve_status

`void aar_serve_status(aar_serve *s, const char *text)`: queues a launcher
diagnostic line before the app's console text.

### aar_serve_locate

`LONG aar_serve_locate(aar_serve *s, BPTR dir, const char *name, char *full, ULONG size)`:
finds a file through every directory of a multi-directory assign (`dir` is the
directory a relative `name` is searched from, 0 for a volume or assign path) and
gives its full name. Returns `AAR_OK`, `AAR_E_NOENT` when it is not found, or
`AAR_E_IO` when the name could not be built. A Ctrl-C during the search counts
as a user stop.

### aar_serve_result

`LONG aar_serve_result(aar_serve *s, struct aar_serve_result *out)`: the outcome.
`out->flags` are valid at any time; when the claim is still ours the call reads
the final status and returns `AAR_OK`, otherwise the library's error
(`AAR_E_STALE` after `AAR_SERVE_LOST`).

`struct aar_serve_result` holds `size` (set by the caller), `flags`, `state`,
`end_reason`, `exit_code`, `dropped_logs`, `runtime_dropped_logs`,
`dropped_messages`, `ahi_underruns`, `ahi_gaps`, `ahi_requests`,
`ahi_min_frames`, `ahi_avg_frames` and the final `status`.
The flags: `AAR_SERVE_R_FAULTED`, `AAR_SERVE_R_USER_STOP`,
`AAR_SERVE_R_STOP_ASKED`, `AAR_SERVE_R_CLAIM_LOST`, `AAR_SERVE_R_DISPLAY_FAILED`,
`AAR_SERVE_R_CONSOLE_FAILED`, `AAR_SERVE_R_AUDIO_FAILED` and
`AAR_SERVE_R_QUEUE_CORRUPT`. The process exit code and the fault report are the
launcher's to derive from it.

### aar_serve_destroy

`void aar_serve_destroy(aar_serve *s)`: stops the app if it still runs, removes
the log sink, closes AHI, closes the display (waits for the SafeMessages of the
flips and restores the public screen), finishes the console reads and writes and
the launcher text, answers every request a handler still holds with `AAR_E_IO`,
closes every file and list, unlocks the root, closes `timer.device` and the
libraries it opened and frees the state. The handle stays open: close it
afterwards.

## An embedding program

```c
aar_serve_config_default(&cfg);
cfg.poll_us = 20000;
cfg.flags |= AAR_SERVEF_USE_SIGNAL | AAR_SERVEF_NO_INPUT_WINDOW;
if (aar_serve_create(h, &cfg, &s) != AAR_OK) {
    AARClose(h);
    return 20;
}
for (;;) {
    ULONG mine = mywin_sigmask();
    ULONG got = Wait(mine | aar_serve_sigmask(s));  /* recompute each time */
    if (got & mine) handle_my_window();
    if (aar_serve_step(s, got) != AAR_SERVE_CONTINUE) break;
}
aar_serve_result(s, &res); aar_serve_destroy(s); AARClose(h);
```

Signals that are not in the mask are never cleared by the loop, so the
program's own window signals are not lost.

## Notes

- Console output to `NIL:`, or to any handle with no handler port, is written
  with a plain `Write()`, which cannot block; the result is delivered on the
  next pass. A destination that fails stops the app, and the note about it goes
  to the user's console (`CONSOLE:`), not into the destination that failed.
- The library keeps 64 unread app messages, and a message the full inbox cannot
  take blocks every request behind it. Unless `AAR_SERVEF_KEEP_MESSAGES` is set
  the loop takes them every pass: the `message` hook gets each one, or it is
  dropped and counted in `aar_serve_result.dropped_messages`.
- The runtime drops a log record when its queue is full and counts it
  (`AARStatusInfo.log_dropped`, copied into
  `aar_serve_result.runtime_dropped_logs` by `aar_serve_result`). The launcher's
  console queue counts the records it had no room for in
  `aar_serve_result.dropped_logs`. A program that reports lost output adds the
  two. After the claim is lost `aar_serve_result` returns `AAR_E_STALE` and
  `runtime_dropped_logs` is the value of the last successful call.
- `aar_serve_result.ahi_underruns` sums the AHI ring headers' underrun counts
  across streams (the app wrote less than AHI needed). `ahi_gaps` counts the
  times AHI ran dry while the app was still writing, which no ring counter
  shows. `ahi_requests`, `ahi_min_frames` and `ahi_avg_frames` give the number
  of requests sent and their shortest and average length in frames. Whole
  requests are all the same size; only the last one of a sound is shorter. All
  are sampled while the claim runs, survive audio close and app end, and are 0
  on the platform route. AHI needs a heartbeat of 1..20000 us (1000 by
  default). aar-run prints them with its heartbeat for `AUDIO=AHI`.
- The stop grace is `AARInfo.stop_grace_ms` (read by `aar_serve_create`). It is
  500 ms only when the platform reports none.
- Framebuffer tokens are checked by the library and the platform. The loop has
  no window of its own for them.
- A refused audio ring closes AHI again. The library refuses it by turning an
  `AUDIO_OPEN` completion whose ring does not check out into an error.
- Requests still held at `aar_serve_destroy` (a console read that never started,
  a console write, a flip waiting for its SafeMessage) are answered with
  `AAR_E_IO`, once. A completion that cannot be posted because the completion
  queue is full stays unanswered. The app has gone away with the claim.
- PCM goes from the app's ring into a reserve and from there into two AHI
  requests of up to 3/50 s (60 ms). The reserve is a FIFO of up to the size of
  two requests, and at most 20 ms is read from the ring per pass. The two
  requests follow the double buffering of the AHI developer guide: each is
  linked to the other while that one is queued, and a request is sent again as
  soon as it has finished (`io_Offset` is set to 0 every time, because the
  device trashes it). AHI is started when the reserve holds the size of two
  requests (about 120 ms). After that every request is whole. When the app stops
  writing for 30 ms the rest is played as a last, short request. At
  `AUDIO_CLOSE` the queued audio plays out before AHI is closed
  (`aar_serve_destroy` cuts it). If AHI runs dry while the app is still
  writing, `ahi_gaps` counts it, the empty ring is read once (a ring underrun)
  and the stream is buffered again. The added latency is about 120 ms (240 ms in
  all). More than two requests in flight are not used by this backend.
- The AHI route needs a heartbeat of at most 20 ms, because a buffer has to be
  refilled well before the other one has played. With the AHI route,
  `aar_serve_create` answers `AAR_E_NOSYS` for other values. Without AHI,
  `poll_us` 0 is allowed. The program is then woken by the flip and AHI ports,
  the console, the input window and, with `AAR_SERVEF_USE_SIGNAL`, by the
  library.
- A relative path is resolved below `ROOT` and checked there for every op that
  takes one (open, stat, list, delete, rename, create, set date, set
  protection, volume info), whatever `AAR_SERVEF_ABS_PATHS` says: `..`, a
  leading `/`, and a link whose target lies outside `ROOT` answer `AAR_E_ACCES`.
  The flag only admits paths with a volume or assign colon, which go to AmigaDOS
  as they are.
- Once any stop is requested (a break, `aar_serve_stop`, a stop from the input
  window, or the platform ending the app) no request that changes the file
  system runs. An open that creates, truncates, replaces or updates, a write, a
  delete, a rename, a directory create, a set date or protection and a truncate
  answer `AAR_E_IO`. Reads, seeks, closes, stat, list and volume info change
  nothing and still run. The check is one predicate, `aar_serve_stopping`, with
  the platform state read live before each such step.
- A requester (a Workbench console that cannot be opened) shows its text as the
  argument of a fixed `%s` format. A `%` in the screen title or in a path stays
  a `%`. No caller or app text is used as a format string anywhere in
  `client/serve`, `client/run` or the example.

## The example and the library

`client/examples/aar-embed.c` is the embedding program above as a complete
Workbench/Shell tool: its own window with a close gadget that stops the app, the
app's last message in the window title, a 20 ms heartbeat, the library signal,
no input window, messages kept for `AARGetMessage`. `client/tests/m68k_check.sh`
compiles it and links it against `libaarserve.a` (`aar_serve.o` and
`aar_serve_core.o`). The ZZ9000 firmware SDK ships the archive in
`Developer/Lib/` and the public header in `Developer/Include/`.
