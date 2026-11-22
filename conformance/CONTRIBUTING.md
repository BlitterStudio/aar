# Writing the tests of an area

Read `README.md` first (what the suite is, the result format, how a payload and the
runner work together). This file is for the person who writes one area, for
example C-FILE, and is laid out so that areas can be written in parallel without
touching each other's files.

## What an area author owns

For an area `C-<AREA>` (file names use the area in lower case, `<area>`):

| File | What goes there |
|---|---|
| `TESTS.md`, the rows `C-<AREA>.n` | The catalogue rows of the area, and its spec questions. Edit only your own rows (see below) |
| `payloads/conf_<area>.c` | The payload: the tests that are judged from inside the app. One source file per area |
| `payloads/conf_<area>.h` | Optional. What the payload and `r_<area>.c` share: peer keywords, message layouts, fixture names, expected values |
| `payloads/conf_<area>.host` | Optional marker, a one-line text file: the payload also builds for the host runner and runs under CTest |
| `payloads/conf_<area>.qemu` | Optional marker: the payload runs under qemu user mode in `make check-runtime` (only when the loader of `libaar/tests/loader.c` has what the payload needs) |
| `runner/r_<area>.c` | The runner side: runner-judged tests, peer keywords, payload variants, observers. It exists already as a stub for every area |

Everything else is shared and is not edited for a normal area: `runner/aar-conf.c`
and `runner/aar-conf.h` (the core), `runner/r_areas.h` (the list of areas, already
complete), `payloads/conf.h` and `payloads/conf_proto.h` (the payload protocol),
`tools/`, `tests/`, the `Makefile`, `host-runner/CMakeLists.txt`,
`client/tests/m68k_check.sh`, the CI workflow. None of them lists areas: the
`Makefile` and CMake find payloads and markers by file name, the m68k build compiles
every `.c` file of `runner/`, and `conf_catalogue.py` finds the rest. If you need
a change in a shared file (a new helper in the core, a field in `struct conf_run`),
make it small, keep it additive, and say so in the commit message.
`runner/conf_catalogue.h` is generated from `TESTS.md` and is not in git.

## Rows and impl

`TESTS.md` has one row per id. The columns are described at its top. To mark a test
done, change the `impl` cell of that row from `todo` to `yes` in the same commit as
the code that prints the id. Edit only the rows of your area; the first and last
rows of an area touch the neighbouring area's rows in a merge, so keep changes to
the rows you really change. Do not renumber ids: other tests and questions refer to
them. A new test needs a new id at the end of the area (the numbers must run
without gaps) and a row. A statement that cannot be tested gets a `none` row whose
check starts with `not tested:` and says why.

`python3 conformance/tools/conf_catalogue.py check` fails when:

- an id is printed by a payload or by an `r_<area>.c` and its row is `todo`, or the
  row says another judge, or the id is in another area's file;
- a row is `impl yes` and nothing prints it;
- `aar-conf.c` prints an id (the core prints none);
- a `.host` or `.qemu` marker has no payload;
- an area has no runner file or no line in `runner/r_areas.h`.

## The payload

A payload is an app (`aar_main`) that prints one line per id with the helpers of
`payloads/conf.h`; see `payloads/conf_time.c` for a complete area. Rules:

- Print only the ids of the area whose row says `payload`, `impl yes`, and that
  apply to the profile and group. The runner fails the ones that did not arrive.
- Use the `C-<AREA>.n` ids as string literals in the call (`conf_begin(&t,
  "C-TIME.1")`); the check finds them there. Put the area name in `conf_finish`.
- Use `conf_skip` for what cannot be done in an environment that is not a platform,
  with a reason that starts with `host:`. A platform run must not skip MUST tests.
- A test that uses the runner as the peer must check `conf_have_peer(argc, argv)`
  first and skip with `host:` without it.
- No libc in the payload unless the test is about the libc flavour. Build
  with `AAR_NOTE(...)` for the groups the area needs (`groups_required` makes a
  platform without them refuse the load; use `groups_optional` and check the table
  when the same payload should also run on a platform without the group).
- The same source builds for A32, A64 and the host. Keep profile-specific code
  under `#if defined(__arm__)` and `#if defined(__aarch64__)`, and give the rows
  their profile in `TESTS.md` (`a32`, `a64` or `any`).
- Variants: when one run is not enough (the tests of a limit set by the client, a
  file test that needs fixtures), the runner runs the payload once per variant and
  passes `conf:variant=<name>`. The payload prints an id only in the variant that
  owns it.

## The runner side (`runner/r_<area>.c`)

The file defines one `const struct conf_area conf_area_<area>` (see the stubs and
`runner/aar-conf.h`). Everything in it is optional:

- **Runner-judged tests**: rows judged `runner`. Write `static void t_xxx(int
  idx)` that calls `conf_set_result(idx, CONF_R_PASS|CONF_R_FAIL|CONF_R_SKIP,
  detail)` and list `{ "C-<AREA>.n", t_xxx }` in the `tests` table. `r_client.c` is
  the example. A test may open its own claim and use the library directly, or run
  a payload with `conf_run_init`, `conf_run_payload` and `conf_parse_capture`
  (set `run.norelay = 1` when the lines should not be taken as results, for
  example when the test starts the same payload 20 times).
- **Peer keywords**: a payload asks the runner for something by sending a 32-byte
  message that starts with an ASCII keyword. Define the keyword in
  `payloads/conf_<area>.h`, start it with `CONF-<AREA>-` (the core owns
  `CONF-STOP-NOW`), and list `{ "CONF-<AREA>-WHAT", handler }` in the `peers` table.
  The handler gets the `struct conf_run` (with the handle `run->h` for
  `AARSendMessage`, the loop `run->s` for `aar_serve_post_event`, and `run->user`
  for the area's own state) and the message. It runs inside the serving loop, so it
  must return quickly. Messages that match no keyword go to `run->on_message`.
- **Variants**: list `{ "name", prepare }` in `variants`. `prepare(run)` sets what
  the run needs before the load: `run->tags` (a tag list for `AARSetTagList`, for
  example `AAR_TAG_PRIVATE_MIB`), `run->extra_args` (more arguments),
  `run->argv_override` (all arguments), `run->timeout_s`, `run->serve_groups`,
  the console modes, `run->poll` and `run->on_message`; `run->display` (a display table
  that replaces the Picasso96 screen, see `runner/r_gfx.h`), `run->serve_flags_off`
  (flags the core sets that the run does not want, for example the input window) and
  `run->on_finished` (called after the serving loop returned, with the handle still
  open: the messages and the status are readable). It is also the place to
  prepare fixture files under `conf_g.root`.
- **Console modes** (slow output and scripted input) are in the core, in
  `run->con`: `write_hold_ticks` delays the completion of each console write (the
  output backs up in the app, which parks), `input` and `input_len` script what
  `console_read` returns, `input_hold_ticks` delays the first read, and
  `input_block` makes the read after the script wait for ever so that only a stop
  ends it. A C-CON author sets these in a variant's `prepare`. If a mode is
  missing, add it in `aar-conf.c` (`con_write_start`, `con_read_start` and
  `console_pump`) and say so in the commit.
- **Observers**: `on_open` and `on_end` are called for every payload run of every
  area, for tests that hold for any app (`r_life.c` judges C-LIFE.1 to C-LIFE.3 that
  way).
- A manual service loop (`AARServiceNext` and `AARServiceReply`, needed to withhold
  or misaddress completions for the queue and timeout tests) is not in the core.
  Write it in your `r_<area>.c` as a test that does its own open, load and start
  (the core's `conf_run_payload` always uses `aar_serve`). `runner/r_mq.h` is one:
  open, load and start a payload by hand, take requests with a stash, wait for a
  message or a request, and finish; use it before writing another. A payload judged
  by `payload` whose run is manual has its console text parsed with
  `conf_parse_capture` (see `stage_late` in `r_queue.c`). A runner source may not name
  the id of a `payload` row, so a manual run that serves a payload row hangs it
  on a `runner` row's test function (`t_manual` in `r_queue.c`).

The runner is plain C for AmigaOS m68k with warnings as errors; it may use
`aar.library`, `aar_serve`, dos and exec, and nothing platform specific.

## Gaps in the spec

When the spec is silent or ambiguous about what a test needs, do not pin a value
the spec does not state. Test the behaviour the spec requires (for example "a
negative code", or "one of `AAR_E_INVAL` and `AAR_E_NOSYS`"), accept every answer
the text allows, and add a numbered entry to "Spec questions for review" at the end
of `TESTS.md` that says what is unstated. Point the row's check text at the
question (`[spec question 15]`). Add new questions at the end, with the next number;
do not renumber. If a statement cannot be observed at all, write a `none` row with
the reason instead of a test that proves nothing.

## Running the checks

From the repository root, with the SDK image (`docker build -f docker/Dockerfile -t
aar-sdk .`), mounting the tree read-only and working in a copy:

```sh
docker run --rm -v "$PWD:/src:ro" aar-sdk sh -c \
  'cp -r /src /w && cd /w && make AAR_SDK=/w check-conf-payloads check-conf-catalogue'
```

builds every payload for A32 and A64, runs `aar-check` on it, and runs the catalogue
check and the tests of the scripts. For qemu user mode, install `qemu-user`,
`gcc-arm-linux-gnueabihf libc6-dev-armhf-cross gcc-aarch64-linux-gnu
libc6-dev-arm64-cross` in the container and run `make AAR_SDK=/w check-runtime`;
only payloads with a `.qemu` marker run there.

For the host runner (SDL2, CMake):

```sh
cmake -S host-runner -B build-host && cmake --build build-host
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ctest --test-dir build-host -R conf
```

For the runner on m68k, in the pinned image (this compiles every `runner/*.c` with
warnings as errors and links `aar-conf`):

```sh
docker run --rm -v "$PWD:/src:ro" \
  amigadev/crosstools@sha256:9c81fcd973058fb37f1ca5b1c608c65473dc1dd657ab5c8533e7b43b80f01cf3 \
  sh -c 'cp -r /src /w && cd /w && sh client/tests/m68k_check.sh'
```

`python3 conformance/tools/conf_catalogue.py summary` prints the counts per area. A
payload that is not marked `.host` still has to build for the profiles; one that
needs a platform for every test may stay off the host and off qemu.
