# aar_serve: the serving loop of an aar.library client

`aar_serve` answers the service requests of an app that runs on a platform
behind `aar.library` (spec section 9.6): files, console, display and audio. It
also posts the input events and the stop event. The reference launcher
[`aar-run`](../client/run/README.md) is a small program around it; a program
with its own window and event loop can use it the same way.

It is MIT code in this repository: `client/serve/aar_serve.c` is the one
translation unit of the loop (it includes the Amiga backends), and
`client/serve/aar_serve_core.c` holds the OS-free request handlers. The public
header is `libraries/aar_serve.h`. The calls are plain C functions linked into
the program, not library vectors. Link both `.c` files, with the program's
`AARBase`, `IntuitionBase` and `GfxBase`, `-lamiga` and `-noixemul`.

```sh
m68k-amigaos-gcc -std=gnu11 -noixemul -m68030 -O2 -Iclient/include -Iinclude \
    prog.c client/serve/aar_serve.c client/serve/aar_serve_core.c -lamiga
```

## Rules

- One `aar_serve` serves one `AARHandle`. The caller opens the handle, gives it
  its options (`AARSetTagList`), loads and starts the app, creates the
  `aar_serve`, steps it until the app has ended, destroys it and closes the
  handle. `aar_serve` never opens or closes the handle and never loads or starts
  an app.
- Every call comes from the task that opened the handle (`AAR_E_OWNER`
  otherwise). The hooks run in that task, inside `aar_serve_step`. Nothing runs
  in an interrupt.
- The program supplies `AARBase`, and `IntuitionBase` and `GfxBase` for a
  display. `aar_serve` opens `timer.device`, `Picasso96API.library` (when there
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
| `console_in`, `console_out` | the streams of the console; 0: `Input()` and `Output()`. A stream the caller gives is the caller's to close |
| `console_spec` | opened at the first console use when `console_out` is 0 (a Workbench start, for example `CON:0/0/640/160/aar-run/CLOSE/WAIT`); NULL: none, console I/O then fails as `AAR_E_IO` |
| `audio_route` | `AAR_AUDIO_ROUTE_PLATFORM` or `AAR_AUDIO_ROUTE_AHI`: what this loop serves; it must equal the `AAR_TAG_AUDIO_ROUTE` the handle was given |
| `groups` | `AAR_GROUP_*` to serve; the display and audio ops of a group not in the mask complete with `AAR_E_NOSYS`. 0: every group |
| `joy_ports` | bit 0 port 0, bit 1 port 1; default `AAR_SERVE_DEFAULT_JOY_PORTS` (port 1 only) |
| `poll_us` | the heartbeat of the timer in microseconds; 0: none (a DOS wait still ticks at the default). Default `AAR_SERVE_DEFAULT_POLL_US` |
| `break_mask` | the task signals the loop consumes: Ctrl-C stops the app, Ctrl-D, Ctrl-E and Ctrl-F become `AAR_EV_BREAK` events. Default `AAR_SERVE_BREAK_ALL`; 0: the program owns its signals |
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
written), `AAR_SERVE_WHY_QUEUE` (the request queue is corrupt) and
`AAR_SERVE_WHY_CALLER` (`aar_serve_stop` was called).

## Requests and the replaceable tables

`struct aar_serve_req` is a request as `aar.library` hands it out with its
payload buffer mapped into this process; `struct aar_serve_done` is the
completion a handler produces: `result` (`AAR_OK` or an `AAR_E_*`), the entry
`flags` and the result words `out[]`.

`struct aar_serve_display_ops` is the table under the display handlers: `open`
(opens the screen and fills the completion), `change_buffer`, `pan`, `wait_tof`,
`load_rgb32` and `close`. A program that draws into its own window replaces it
and calls `aar_serve_flip_shown` when a flip it started is on the screen.
`struct aar_serve_console_ops` is the table under the console handlers: `open`,
`write_start`, `read_start`, `read_allowed`, `live` and `output_error`. The first
member of both is the `ctx` that every call receives.

## Calls

### aar_serve_config_default

`void aar_serve_config_default(aar_serve_config *cfg)`: fills `*cfg` with its
size and the defaults.

### aar_serve_create

`LONG aar_serve_create(AARHandle *h, const aar_serve_config *cfg, aar_serve **out)`:
validates the configuration (`AAR_E_INVAL`), checks the handle (`AAR_E_OWNER`,
`AAR_E_STALE`), allocates about 20 KiB, duplicates the root, opens
`timer.device` (`AAR_E_NOSYS` when there is none) and `Picasso96API.library`,
installs the log sink with `AARSetLogSink` and arms the heartbeat. On an error
`*out` is NULL and nothing is left behind (`AAR_E_NOMEM` for memory).

### aar_serve_run

`LONG aar_serve_run(aar_serve *s)`: waits on `aar_serve_sigmask` and steps until
the app has ended. Returns `AAR_SERVE_ENDED` or `AAR_SERVE_LOST`.

### aar_serve_sigmask

`ULONG aar_serve_sigmask(const aar_serve *s)`: every signal the loop waits on
now: the break signals of `break_mask`, the timer, the console packet port and
the input window when they exist, and the library signal with
`AAR_SERVEF_USE_SIGNAL`. It changes while the app runs: ask again before each
`Wait`.

### aar_serve_step

`LONG aar_serve_step(aar_serve *s, ULONG signals)`: one pass of the loop;
`signals` is what `Wait` returned. It takes the break signals, drains the input
window and the joystick, flushes the stop event, serves up to 16 requests, pumps
the console, the display and the audio, flushes the completion backlog and reads
the state. It returns `AAR_SERVE_CONTINUE` until the app has ended and the
output that must be written is written (`AAR_SERVE_ENDED`, also when both grace
periods have elapsed after a stop), or `AAR_SERVE_LOST` when the claim is gone
(revoked, quarantined, reset).

### aar_serve_post_event

`LONG aar_serve_post_event(aar_serve *s, const aar_event *ev)`: posts an input
event for the app. A stop event that finds the queue full is kept and posted
again on every pass until it is delivered.

### aar_serve_flip_shown

`void aar_serve_flip_shown(aar_serve *s)`: a replacement display table tells the
loop that the flip it started with `change_buffer` is on the screen.

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
gives its full name. `AAR_OK`; `AAR_E_NOENT` when it is not found; `AAR_E_IO`
when the name could not be built. A Ctrl-C during the search counts as a user
stop.

### aar_serve_result

`LONG aar_serve_result(aar_serve *s, struct aar_serve_result *out)`: the outcome.
`out->flags` are valid at any time; when the claim is still ours the call reads
the final status and returns `AAR_OK`, otherwise the library's error
(`AAR_E_STALE` after `AAR_SERVE_LOST`).

`struct aar_serve_result` holds `size` (set by the caller), `flags`, `state`,
`end_reason`, `exit_code`, `dropped_logs`, `dropped_messages` and the final
`status`. The flags: `AAR_SERVE_R_FAULTED`, `AAR_SERVE_R_USER_STOP`,
`AAR_SERVE_R_STOP_ASKED`, `AAR_SERVE_R_CLAIM_LOST`, `AAR_SERVE_R_DISPLAY_FAILED`,
`AAR_SERVE_R_CONSOLE_FAILED`, `AAR_SERVE_R_AUDIO_FAILED` and
`AAR_SERVE_R_QUEUE_CORRUPT`. The process exit code and the fault report are the
launcher's to derive from it.

### aar_serve_destroy

`void aar_serve_destroy(aar_serve *s)`: stops the app if it still runs, removes
the log sink, closes AHI, closes the display (waits for the SafeMessages of the
flips and restores the public screen), finishes the console reads and writes and
the launcher text, closes every file and list, unlocks the root, closes
`timer.device` and the libraries it opened and frees the state. The handle stays
open: close it afterwards.

## An embedding program

```c
aar_serve_config_default(&cfg);     /* cfg.poll_us = 20000; USE_SIGNAL, NO_INPUT_WINDOW */
aar_serve_create(h, &cfg, &s);
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

## Not done yet

What the move from the first launcher did not change yet (each is a later change):
a console handle without a handler port (`>NIL:`) fails the console; app
messages are not drained (`AAR_SERVEF_KEEP_MESSAGES` is the behaviour and the
`message` hook and `dropped_messages` are not used); a refused audio ring does
not close AHI; the stop grace is the fixed 500 ms, not `AARInfo.stop_grace_ms`;
a completion queue that stays full can lose a completion; the loop waits for the
console, the input window and the timer but not for the flip and AHI ports (the
heartbeat services them); the client-side framebuffer window check is still
made; requests still held at `aar_serve_destroy` are not answered with
`AAR_E_IO`.
