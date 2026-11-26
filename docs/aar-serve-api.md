# aar_serve: the serving loop of an aar.library client

`aar_serve` answers the service requests of an app that runs on a platform
behind `aar.library` ([spec section 9.6](../spec/aar.md)): files, console, display and audio. It
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
  is one) and `ahi.device` (for the AHI audio route) itself and keeps those
  bases to itself.
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
| `joy_ports` | bit 0 port 0, bit 1 port 1; default `AAR_SERVE_DEFAULT_JOY_PORTS` (port 1 only) |
| `poll_us` | the heartbeat of the timer in microseconds; 0: none. Without a heartbeat the loop still wakes for what waits on time: a stop that was asked or a stop event that found the queue full (10 ms), the joysticks of the input window (20 ms), completions waiting for room in the completion queue (1 ms), and a claim that is revoked without a signal (250 ms). Default `AAR_SERVE_DEFAULT_POLL_US` |
| `break_mask` | the task signals the loop consumes: Ctrl-C stops the app, Ctrl-D, Ctrl-E and Ctrl-F become `AAR_EV_BREAK` events. A signal left out is the program's: the loop's DOS packet waits and the keys of its window do not wait on it, test it, or clear it either. Default `AAR_SERVE_BREAK_ALL`; 0: the program owns its signals |
| `display_strategy` | `AAR_SERVE_DISPLAY_*`, the first strategy to try |
| `hooks` | an `aar_serve_hooks`, may be NULL |
| `user` | passed to every hook |
| `platform` | reserved, 0 |

`AAR_SERVEF_ABS_PATHS`: paths with a volume or assign colon are allowed and
`root` does not confine them. `AAR_SERVEF_KEEP_MESSAGES`: the program calls
`AARGetMessage` itself. `AAR_SERVEF_NO_INPUT_WINDOW`: no backdrop input window;
the program feeds `aar_serve_post_event`. `AAR_SERVEF_USE_SIGNAL`: arm the
handle and add the library signal to `aar_serve_sigmask`.

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

## Calls

### aar_serve_config_default

`void aar_serve_config_default(aar_serve_config *cfg)`: fills `*cfg` with its
size and the defaults.

### aar_serve_create

`LONG aar_serve_create(AARHandle *h, const aar_serve_config *cfg, aar_serve **out)`:
validates the configuration (`AAR_E_INVAL`), checks the handle (`AAR_E_OWNER`,
`AAR_E_STALE`), allocates about 20 KiB, reads `AARInfo.stop_grace_ms`,
duplicates the root, opens `timer.device` (`AAR_E_NOSYS` when there is none) and
`Picasso96API.library`, installs the log sink with `AARSetLogSink` and arms the
heartbeat. `AAR_E_NOSYS` also for the AHI route with a `poll_us` of 0 or above
20000. On an error `*out` is NULL and nothing is left behind (`AAR_E_NOMEM` for
memory).

### aar_serve_run

`LONG aar_serve_run(aar_serve *s)`: waits on `aar_serve_sigmask` and steps until
the app has ended. Returns `AAR_SERVE_ENDED` or `AAR_SERVE_LOST`.

### aar_serve_sigmask

`ULONG aar_serve_sigmask(const aar_serve *s)`: every signal the loop waits on
now: the break signals of `break_mask`, the timer, the console packet port, the
input window, the SafeMessage port of a flip and the AHI port when they exist,
and the library signal with `AAR_SERVEF_USE_SIGNAL`. It changes while the app
runs, so ask again before each `Wait`. The heartbeat timer is armed whenever it
is in the mask. `aar_serve_create`, the end of every step and every call that
waits on DOS (`aar_serve_locate` included) arm it again, so a program may call
those between `aar_serve_create` and its first `Wait`.

### aar_serve_step

`LONG aar_serve_step(aar_serve *s, ULONG signals)`: one pass of the loop;
`signals` is what `Wait` returned. It takes the break signals, drains the input
window and the joystick, flushes the stop event, serves up to 16 requests, pumps
the console, the display and the audio, flushes the completion backlog and reads
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
  takes one (open, stat, list), whatever `AAR_SERVEF_ABS_PATHS` says: `..`, a
  leading `/`, and a link whose target lies outside `ROOT` answer `AAR_E_ACCES`.
  The flag only admits paths with a volume or assign colon, which go to AmigaDOS
  as they are.
- Once any stop is requested (a break, `aar_serve_stop`, a stop from the input
  window, or the platform ending the app) no request that changes the file
  system runs. An open that creates, truncates, replaces or updates, and a
  write, answer `AAR_E_IO`. Reads, seeks, closes, stat and list change nothing
  and still run. The check is one predicate, `aar_serve_stopping`, with the
  platform state read live before each such step.
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
