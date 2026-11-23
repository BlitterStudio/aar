<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# Porting AAR to a platform

This page is for the people who make a platform run AAR apps: an emulator that
can run ARM code next to AmigaOS, a card with its own ARM cores, a bridge to a
single-board computer. An app author does not need it. It says what the
platform has to provide, which parts of this repository it can take as they
are, how the app is entered, and how to run the conformance suite and report
the result. It links to the documents that hold the details and does not
repeat them.

A platform implements one or more profiles, **A32** and **A64**, and the two
are treated alike here. Where the text says "the profile", read it once for
each profile the platform runs. The ZZ9000 is the existing implementation (A32
only, Cortex-A9); nothing on this page depends on it.

## What a platform provides

The standard (section 10.7) splits a port into three parts:

| Part | What it is | What you can reuse |
|---|---|---|
| **The ARM runtime** | The code on the ARM side: the loader, the table (`aar_api`) the app calls through, the limits, the lifecycle (start, stop, fault, watchdog), the services of the groups the platform declares | `loader/` for the image rules (below), `include/aar/app.h` for the table, `tools/aar-check` and the corpus to test it |
| **The host transport** | How the runtime and the Amiga side exchange data: shared memory, a control block and a queue pair per app, a doorbell in each direction, a mapping of tokens to 68k pointers | `include/aar/transport.h` and [transport.md](transport.md): the layouts and a reference producer and consumer |
| **`aar.library`** | The library an Amiga program calls to start and watch apps | `client/src/`: the core (`aar_core.c`, no OS calls), the image probe, and the AmigaOS shell (`aar_shell.c`). You write the platform layer under them |

### The ARM runtime

The runtime has to do the following, in this order of dependence:

1. **Load** an app image: check it against the image rules, place it in memory,
   relocate it (the next section).
2. **Build the table** and enter the app with the register state of the entry
   contract (the section after next). Every core slot of the table is
   non-NULL; the groups the platform declares in `groups` have all their slots
   filled. [aar-api.md](aar-api.md) lists every call and its rules.
3. **Enforce the limits and the lifecycle**: the private heap and its cap, the
   shared-memory limit, the stop request and its grace period, the watchdog,
   the fault report, the release of everything on every end. The rules are in
   the standard, sections 4 and 5; the conformance tests (`conformance/TESTS.md`,
   areas C-MEM, C-SHARED, C-LIFE) are the checklist.
4. **Serve the transport**: post the app's requests (files, console, display,
   audio) to the submission queue and take the client's completions, messages
   and events from the completion queue.

### The host transport

A platform supplies three things (see [transport.md](transport.md), "What a
platform supplies"): shared memory that both sides reach, a doorbell in each
direction (a hint only, since the queues stay authoritative, so a platform
that can only poll is correct), and a client mapping from a token to a 68k
pointer with its cache mode.

The layouts are byte offsets in `aar/transport.h`, not C structures, and every
word is big-endian. `aar_cb_init`, `aar_cb_check` and the `aar_queue_*`
functions are the reference code. Use them on the ARM side with the
byte-reversing accessors rather than writing the offsets again. The host test
`tests/transport_model_test.c` exercises the model, including the barrier
order (acquire after reading the other side's index, release before writing
your own).
A platform whose memory is not coherent between the two sides has to map the
control structures non-cacheable on the ARM side, or provide real `aar_publish`
and `aar_acquire`.

If the app and the Amiga side run in the same machine and share RAM (an
emulator, for example), the transport is simple: a token is an Amiga address,
`resolve` is the identity and the doorbell can be a signal. The layouts and the
queue rules stay the same.

### `aar.library`

The library above the transport is shared. `client/src/aar_core.c` holds the
handles, owner checks, queue pump, inbox, events and messages, status and
tags, and it makes no OS call. `client/src/aar_shell.c` is the AmigaOS part
(the Exec library, the lease server, the dispatcher). Under them is one
interface, `client/src/aar_plat.h`, which a platform fills in:

| Operation | Purpose |
|---|---|
| `claim`, `release` | admission of an app (the handle, the tokens of the control block and the queues, or a refusal with a reason) and the release of all of it |
| `load_begin`, `load_chunk`, `load_end` | move the image to the ARM side with progress in the control block |
| `start`, `stop`, `status` | the control calls |
| `map`, `token` | tokens and 68k pointers |
| `kick`, `poll` | the doorbell toward the app, and the housekeeping that retires a finished doorbell request |
| `cache_publish`, `cache_acquire` | 68k data cache maintenance, or nothing |
| `ext`, `events_sample`, `claim_news` | platform extensions and the optional event counts |

`client/src/refplat.c` is a complete platform on plain memory. It is the best
short example of what each operation has to do, and the host tests of the core
run on it (`client/tests/`). The calls the library offers to Amiga programs
are in [aar-client-api.md](aar-client-api.md). `aar-run` (`client/run/`) and
the serving loop `aar_serve` (`client/serve/`) are built on the library and
need no change for a new platform: they serve the app's files, console,
display, input and audio through it.

## The image rules and `loader/`

An app is a position-independent ELF of one profile. The rules a runtime must
apply to it are the ELF rules and the AAR note of the standard (sections 2.2
and 2.3): the type, class, machine and flags of the profile, no interpreter,
no needed libraries, at most four load segments, none writable and executable,
no overlap, relocations only from the dynamic segment and only of the RELATIVE
kind, an entry point inside an executable segment, and exactly one valid note.

Three implementations of these rules exist in this repository, and they are
tested against each other:

| Implementation | Language | Use |
|---|---|---|
| `tools/aar-check` | Python | checks a finished ELF on the build machine; the independent second implementation |
| `client/src/aar_probe.c` | portable C | reads the note and checks the image without loading it; the AmigaOS library uses it so that a launcher can pick the right image |
| `loader/` | portable C99, MIT | the reference loader: the same checks, and then the placing of the image |

`loader/` is a module a runtime compiles in, as it is: portable C99, no OS
calls, no allocation, both profiles in one build, for 32-bit and 64-bit hosts.
Take its four files (`aar_image.[ch]`, `aar_load.[ch]`) and the headers of
`include/aar`; nothing else is needed. The rules themselves are in
`loader/aar_image.c`, which the loader and the image probe of `aar.library`
share, so they cannot drift apart; `aar-check` stays the independent second
implementation. The loader reads the relocations from `PT_DYNAMIC` only and
never reads section headers, so an image without them loads, as the standard
allows. It does not know where memory is or how the platform maps it; the
runtime tells it. The interface is `loader/aar_load.h`:

```c
typedef int (*aar_load_read_fn)(void *user, uint64_t offset, void *dst, uint32_t count);

int aar_load_plan(aar_load_read_fn read, void *user, uint64_t length,
                  uint32_t profiles, struct aar_load_plan *plan, const char **why);

int aar_load_place(const struct aar_load_plan *plan, aar_load_read_fn read, void *user,
                   void *write_base, uint64_t app_base, uint64_t limit_bytes,
                   uint64_t *entry, const char **why);
```

**Reading.** Every byte of the image comes through the read callback, which
returns `AAR_OK` or a negative `AAR_E_*` (the loader then returns `AAR_E_IO`).
The module never assumes the whole file is in memory. Every size and offset it
reads from the file is untrusted: the arithmetic does not wrap, no read goes
outside `length` (below 2^62), and `aar_load_place` writes nowhere outside the
memory it was given. `aar_load_plan` reads headers and tables in pieces of at
most 512 bytes; `aar_load_place` reads whole segments.

**Planning.** `aar_load_plan` applies every rule of 2.2 and 2.3 and fills
`struct aar_load_plan`:

| Field | Meaning |
|---|---|
| `profile` | `AAR_PROFILE_A32` or `AAR_PROFILE_A64` |
| `length` | the image length the plan was made from |
| `span`, `align` | the bytes the app needs from its base, up to the end of the last segment including its BSS (not rounded to a page), and the alignment of the base (the largest power-of-two `p_align`, or 1). A platform that maps pages rounds `span` up and uses at least its page size as alignment. The standard bounds neither number: compare `span` with the memory you offer |
| `entry_offset` | the entry point as an offset from the base, inside an executable segment, even on A32 |
| `nsegments`, `seg[]` | up to 4 segments, each with `offset`, `filesz`, `memsz`, `vaddr` (relative to the base) and `flags` (`AAR_LOAD_R`, `AAR_LOAD_W`, `AAR_LOAD_X`). Every segment is readable, none is writable and executable, none overlaps another |
| `nrelocs`, `reloc[]` | up to 2 relocation tables (`offset`, `size`, `count`, `entsize`, `rela`); every entry is RELATIVE, so `aar_load_place` applies them and the platform need not |
| the note | `note_version`, `api_major`, `api_minor_required`, `stack_bytes` (0 is the default of 1 MiB), `min_heap_mib`, `groups_required`, `groups_optional` |

`profiles` is the set of `AAR_PROFILE_*` values the platform runs, or-ed
together. The result is:

| Result | Meaning |
|---|---|
| `AAR_OK` | the plan is filled |
| `AAR_E_PROFILE` | the image is acceptable but its profile is not in `profiles`, or the note names a profile that does not match the ELF class |
| `AAR_E_INVAL` | a rule is broken |
| `AAR_E_IO` | the callback failed |

The rules are checked before the profile, so an image that breaks a rule is
`AAR_E_INVAL` whatever `profiles` is. `why`, when not NULL, receives a static
text that names the rule, for the platform's log. Planning writes nothing, so it
can run before any memory is reserved, which is the right time to refuse an
image.

The note's own rules are judged here: magic, version, profile, the API against
the headers the module was built with (a different major, or a required minor
above the header's, is refused, standard section 2.3), the stack size and the
group bits. What the plan does not decide is the platform's: whether the groups
the note requires are available (`AAR_E_NOGROUP`), and whether the stack and
the heap the note asks for fit (`AAR_E_NOMEM`).

**Placing.** The runtime reserves memory for `span` bytes, then calls
`aar_load_place` with the plan of the same image:

| Argument | Meaning |
|---|---|
| `write_base` | where this call writes: `limit_bytes` writable bytes, at least `span` of them |
| `app_base` | the address the app runs at. The relocations are made against it: every RELATIVE target becomes `app_base` plus its addend. It should be a multiple of `align`. On A32 the image must end below 4 GiB |
| `limit_bytes` | the size of the memory at `write_base`. Nothing is written at or beyond `write_base + limit_bytes` |

`write_base` and `app_base` differ when a runtime writes the image through one
mapping and runs it through another: a card that writes to its own memory
through a window and runs the app at another address, or an emulator that loads
into host memory and runs at a guest address. When they are the same, pass the
same value. The whole of `[write_base, write_base + span)` is written: the
bytes of the segments, zero for the BSS and for the gaps between segments, then
the relocations. On `AAR_OK`, `*entry` is `app_base` plus the entry offset.

| Result | Meaning |
|---|---|
| `AAR_OK` | placed |
| `AAR_E_NOMEM` | `span` does not fit in `limit_bytes` (or the host's address space); nothing was written |
| `AAR_E_INVAL` | the plan is not one `aar_load_plan` makes, or the image does not fit the address space at `app_base` |
| `AAR_E_IO` | the callback failed |

After `AAR_E_INVAL` or `AAR_E_IO` the memory holds a partly loaded image and
must not be used.

The platform does the rest of what loading means: it maps the segments with the
permissions of the plan (read and execute for code and read-only data, read
and write for data and the BSS), makes the instruction and data caches agree
over the code, and enters the app.

A minimal use, for a platform that has the whole file in memory:

```c
static int read_mem(void *user, uint64_t offset, void *dst, uint32_t count)
{
    const struct image *img = user;

    if (offset > img->length || count > img->length - offset)
        return AAR_E_IO;
    memcpy(dst, img->bytes + offset, count);
    return AAR_OK;
}

int load_app(const struct image *img, void *window, uint64_t app_base,
             uint64_t room, uint64_t *entry)
{
    struct aar_load_plan plan;
    const char *why;
    int rc = aar_load_plan(read_mem, (void *)img, img->length,
                           AAR_PROFILE_A64, &plan, &why);   /* or both bits */

    if (rc != AAR_OK)
        return rc;
    /* plan.span, plan.align: reserve the memory; plan.seg[i].flags: map it;
     * plan.groups_required: refuse the app that needs a group you lack. */
    return aar_load_place(&plan, read_mem, (void *)img, window, app_base, room,
                          entry, &why);
}
```

The module offers random access through the callback, not a streaming
interface. A runtime that receives the image in pieces keeps it where the
callback can read it (a staging buffer or a file) until the plan and the
placing are done.

**Tests.** The corpus `conformance/negative/` holds an image for every rule in
both profiles, and `MANIFEST` says which refusals each may give. A port tests
its loader against it, as the host tests of `loader/` do, and the conformance
runner does the same through the platform's own `aar.library` (area C-ELF).

## The entry contract

The runtime starts the app by entering `e_entry` (the loaded entry point of the
image) with this state; the standard (section 3.2) is the reference. Every
other general and vector register is 0.

| | A32 | A64 |
|---|---|---|
| Table pointer | `r0` | `x0` |
| `argc` | `r1` | `w1` |
| `argv` | `r2` | `x2` |
| Stack pointer | top of the app stack, 8-byte aligned | top of the app stack, 16-byte aligned |
| Link register | 0 | `x30 = 0` and `x29 = 0` |
| Mode | user, A32 state, little-endian, interrupts enabled | EL0, AArch64, little-endian |
| Float state | `FPSCR = 0`; VFP and NEON enabled; `d0` to `d31` are 0 | `FPCR = 0`, `FPSR = 0`; ASIMD enabled; `v0` to `v31` are 0 |
| Memory | image loaded and relocated, BSS zeroed, the `argv` strings in app memory | same |

`argv[0]` is the name of the image file. The runtime supports at least 16
arguments of up to 127 bytes each and reports the exact limit in `aar_info`.
The stack is `stack_bytes` of the note (1 MiB when it is 0). The runtime runs
no constructors: `crt0` of `libaar` does, then calls `aar_main`.

None of this is an accident of the reference platform. The conformance payload
`conf_entry` (area C-ENTRY) reads the registers at the first instruction and
judges every line of the table, so a platform that enters the app with leftover
state fails there. Two points are easy to miss: the floating-point control
registers must be 0 (an app can rely on round to nearest with flush-to-zero
off), and the vector registers must be 0 as well, which also means the
platform must not leak the previous app's state.

### A worked example: `libaar/tests/loader.c`

`libaar/tests/loader.c` is a small Linux program that loads an app image into
anonymous memory, builds a table over POSIX calls, and enters the image. It
is a test tool, not a platform: it has no client and its table is partial. In
its `ENTRY_CONTRACT=1` mode it enters the image as the contract says, and the
code shows what that takes on each profile (search for `enter_contract`):

- **A32** (`loader_enter_a32`, ARM state). It sets `sp` to the top of a fresh
  1 MiB stack, clears `r3` to `r12`, sets `lr` to 0, clears the 16 quad
  registers (`q0` to `q15`, which is `d0` to `d31`), writes 0 to `FPSCR`, and
  jumps with `ldr pc`, reading the entry address from the stack so that no
  register holds it. `r0` to `r2` carry the table, `argc` and `argv` from the
  call.
- **A64** (a stub copied to a page next to the image). It sets `sp`, clears
  `x3` to `x30` (so `x29` and `x30` are 0), clears `v0` to `v31`, writes 0 to
  `FPCR` and `FPSR`, and ends in a branch patched to the entry, again so that
  no register carries the entry address. `w1` is zero-extended.

On a bare-metal runtime the same state comes from restoring a prepared exception
frame at the drop to user mode (EL0, or user mode on A32), with the argument
registers and `sp` set and the rest zeroed.

To run it in the SDK image (the qemu and Linux cross-compiler packages are
installed on top, as the CI does):

```sh
arm-linux-gnueabihf-gcc -static -marm -O1 -I include \
  libaar/tests/loader.c loader/aar_load.c loader/aar_image.c -o /tmp/loader
make build-a32-conf_entry
ENTRY_CONTRACT=1 HEAP_INIT_MIB=2 qemu-arm /tmp/loader build/a32/conf_entry.elf
```

For A64, use `aarch64-linux-gnu-gcc` (without `-marm`), `build-a64-conf_entry`
and `qemu-aarch64`. The output is the payload's `AAR-CONF` lines. `make
check-runtime` runs this for every payload that has a `.qemu` marker, for both
profiles, together with the corpus (`conformance/tests/negative_run.sh`).

## The transport

A port implements the transport on both sides. [transport.md](transport.md)
describes the control block (its lines and their writers), the queue pair
(depth, ordering, backpressure, message credits), the entry layout and the
operation codes, the doorbell, and the optional event counts. The points that
decide whether a port works:

- Every word has one writer, and the layouts are byte offsets in big-endian.
  Use the accessors of the header.
- The producer runs an acquire barrier after it reads the consumer's index and
  a release barrier before it writes its own. The consumer runs a release
  barrier before it writes the head. On the 68k side the barriers are no-ops.
- The doorbell is a hint. A missed or merged ring must not lose an entry.
- A claim must reply, and every resource reserved at admission is released on
  every failure path.
- A platform that publishes event counts only increases them.

`include/aar/transport.h` is complete in itself, and `tests/transport_model_test.c`
and the host tests of the library core show how the two sides use it.

## Running the conformance suite

The suite is described in [conformance/README.md](../conformance/README.md);
the catalogue of every test and its spec reference is `conformance/TESTS.md`.
What follows is the order a port works in.

### During bring-up

The payloads are ordinary apps, so they run as soon as the platform can load
and enter one:

1. Build them (`make` in the SDK image builds `build/a32/conf_*.elf` and
   `build/a64/conf_*.elf`) or take them from the bundle below.
2. Run `conf_entry`, `conf_table`, `conf_mem`, `conf_time` and `conf_free` on
   the runtime first. Most of their tests need no client and no group: they
   judge the entry state, the table, the private heap, the clock and a
   freestanding link. The test loader of `libaar` passes them under qemu user
   mode, which is the baseline for what the payload expects from the app side.
3. Run the corpus images (`conformance/negative/`) through the loader. A
   refusal must be one of the codes the `MANIFEST` allows, and an accepted
   image must run.
4. When `aar.library` works, run the full suite with `aar-conf` on the
   platform.

### The bundle

CI builds the whole suite for a platform developer and uploads it as the
artifact **`aar-conformance-bundle`** of every run of the CI workflow (the
`conf-bundle` job; download it from the run's page or with `gh run download`).
Locally, `make conf-bundle` in the SDK image stages the payloads and the
corpus, `AAR_CONF_OUT=<file> sh client/tests/m68k_check.sh` in the m68k image
builds the runner, and `python3 conformance/tools/conf_bundle.py manifest`
writes the manifest. The layout:

```
aar-conf                  the runner, an m68k program
a32/conf_<area>.elf       the 16 payloads of profile A32
a64/conf_<area>.elf       the 16 payloads of profile A64
negative/MANIFEST         the C-ELF image corpus and the list of allowed refusals
negative/a32/*.aarimg
negative/a64/*.aarimg
MANIFEST.txt              suite version, source commit, run commands, sha256 of every file
```

`MANIFEST.txt` carries the commands, so it is the page to start from. Check the
download with `grep -E '^[0-9a-f]{64}  ' MANIFEST.txt | sha256sum -c`.

### Running it

Copy the directory to the platform and, from an AmigaOS Shell:

```
Stack 32768
aar-conf DIR=<the directory> PROFILE=a64 TO=RAM:conf-a64.txt
```

`PROFILE` is `a32` or `a64`. Run the suite once for every profile the platform
declares: a platform that runs only A64 runs only `PROFILE=a64`, and the A32
payloads in the bundle are not used. `ONLY` runs one area or one test (`ONLY=C-FILE`,
`ONLY=C-TIME.1`) and gives the verdict `PARTIAL`, which is for debugging, not for a
report. The options are in [conformance/README.md](../conformance/README.md).

**Operator tests.** Four tests need a person: C-LIFE.24 (reset the machine,
a MUST), C-DISPLAY.17 (a picture), C-INPUT.11 (keys, mouse and joystick) and
C-AUDIO.12 (a tone). Without `INTERACTIVE` the runner skips them with the
reason `manual`. Because C-LIFE.24 is a MUST, a run without an operator cannot
say `CONFORMS`. Do the procedure of C-LIFE.24 before the run, then start it
with `INTERACTIVE` and follow the prompts. `conformance/README.md` says what
each prompt shows and what the evidence rule is for C-INPUT.11.

### What `CONFORMS` needs

The standard (section 11.1) says a platform conforms to a profile at an API
version when:

1. it passes every MUST test of that profile and of every group it declares;
2. it does not declare a group it does not pass;
3. the suite ran with the platform's own `aar.library` and the unmodified
   payloads of the bundle, and the results are in the standard format with
   the suite version and the platform's `AARInfo` dump;
4. every SKIP has a reason, and a SKIP is only for a test of an undeclared
   group or a SHOULD test.

The runner prints the verdict. `python3 conformance/tools/check_results.py
report conf-a64.txt --require-conforms` checks the report: the suite line, the
`AARInfo` lines, one line for every test of the profile, a summary that
matches the lines, and a verdict that follows from them. A `SKIP host:` line
is allowed in the host runner and the qemu runs only, never in the report of a
platform. A test marked `todo` in the catalogue prints
`SKIP suite: not implemented`, which a MUST test cannot have, so the verdict
of a suite that is not complete is `DOES-NOT-CONFORM` whatever the platform
does; read the PASS and FAIL lines then.

### Reporting

Keep the report file and the `MANIFEST.txt` of the bundle you used. A report
for a profile is the file the runner wrote with `TO=`; it contains the suite
version, the `AAR-CONF-INFO` lines, and the line of every test. If a test
fails because the text of the standard is unclear, say which id and what reading
the platform takes: `conformance/TESTS.md` ends with the questions that earlier
runs raised, and a new one belongs there.

## A short checklist

- [ ] The loader refuses every image of the corpus with an allowed code and loads the accepted ones.
- [ ] The app is entered with the state of the entry contract; `conf_entry` passes.
- [ ] The table has every core slot, and `aar_info` is complete.
- [ ] The transport uses the layouts and barriers of `aar/transport.h`; the doorbell loses nothing when it is missed.
- [ ] `aar.library` runs over the platform layer (`aar_plat.h`).
- [ ] `aar-conf` ran for every declared profile, with the operator tests, and `check_results.py report --require-conforms` accepts the report.
