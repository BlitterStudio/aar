#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates the image corpus of C-ELF (conformance/negative/).

The images are built with the builder of tests/aar_check_test.py, the one that
tests tools/aar-check, so every refusal rule of spec 2.2 and 2.3 has an image
and the two stay in step. Each profile gets its own images:

    negative/MANIFEST               what the runner reads: one line per image
    negative/a32/<name>.aarimg      images of the profile A32
    negative/a64/<name>.aarimg      images of the profile A64

The extension is .aarimg and not .elf because .dockerignore drops *.elf (and
the files are not all ELF images), and .gitattributes marks it binary.

Every image has real code at its entry point: a few instructions that call the
table's exit slot with the exit code 3 (EXIT_CODE), and the read-write segment
sits on its own 4 KiB page, so an image that is meant to load and run does so
on a runtime that maps pages. Usually a refused image has one defect; names
that include two faults exercise precedence. The runner starts the accepted
images and reads the exit code.

The accepted images whose point is a relocation (C-ELF.16, C-ELF.17, C-ELF.18, and
C-ELF.23 for a target in the bss tail) have
different entry code: it loads the word at the relocation target and exits with 3
only if that word is the runtime address of the code itself, which a RELATIVE
relocation with the right addend makes it at any load base, and with 4 otherwise. A
loader that ignores or misapplies the relocations fails the image.
conformance/tests/negative_run.sh runs them under qemu with the loader's NO_RELOC=1
and expects 4.

The MANIFEST has one line per image, fields separated by a single space:

    <name> <profile> <ids> <load> <probe>

    name     file name without the extension
    profile  a32 or a64: the profile the image is built for
    ids      the C-ELF ids the image counts for, comma separated; "-" for none
             (images that other areas use). Every image also counts for C-ELF.41
             and C-ELF.42
    load     what AARLoadFile/Memory may return: "OK" (loads, starts and exits
             with code 3), or the codes of a refusal separated by "|": PROFILE,
             INVAL, VERSION, NOGROUP, NOMEM, NORESOURCE
    probe    what AARProbeFile does: "ok" (accepts), "refuse" (a negative code),
             "any" (the spec does not say: either); "INVAL" or "VERSION" requires
             the exact code from both AARProbeFile and AARProbeMemory

C-ELF.31 uses the image `unlisted-profile` of the profile that AARInfo.profiles
does not list.

    gen_negative.py            write the corpus
    gen_negative.py --check    check that the files are what the generator makes,
                               that the probe column is what the rules of spec 2.2
                               and 2.3 say (as the loader applies them) and that
                               tools/aar-check, the lint, agrees (it also demands
                               section headers), and that every C-ELF row with an
                               image has one
"""
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
EXT = ".aarimg"
EXIT_CODE = 3
PROFILES = ("a32", "a64")

_spec = importlib.util.spec_from_file_location(
    "aar_check_test", os.path.join(ROOT, "tests", "aar_check_test.py"))
checkmod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(checkmod)
build_image = checkmod.build

note_bytes = checkmod.note_bytes


def header_value(name):
    with open(os.path.join(ROOT, "include", "aar", "app.h")) as fh:
        return int(re.search(r"#define %s (\d+)u" % name, fh.read()).group(1))


# The accepted images carry the minor of the header, as AAR_NOTE() emits it. A
# minor below the floor or above the header is a refused image (C-ELF.33).
AAR_API_MINOR = header_value("AAR_API_MINOR")
AAR_API_MINOR_OLDEST = header_value("AAR_API_MINOR_OLDEST")


_pspec = importlib.util.spec_from_file_location(
    "probe_corpus", os.path.join(ROOT, "client", "tests", "probe_corpus.py"))
probe_corpus = importlib.util.module_from_spec(_pspec)
_pspec.loader.exec_module(probe_corpus)

# Images that the runtime rules accept and the lint aar-check refuses, because
# they have no section headers (spec 2.2 rule 4 lets an image have none).
NEEDS_NO_SECTIONS = ("ok-nosections",)

# Entry code, called with the table in r0 / x0: exit(EXIT_CODE) through the exit
# slot at offset 16 of the table (spec 3.3).
CODE = {
    "a32": struct.pack("<4I", 0xE5903010,                  # ldr r3, [r0, #16]
                       0xE3A00000 | EXIT_CODE,             # mov r0, #EXIT_CODE
                       0xE12FFF13,                         # bx r3
                       0xE1A00000),                        # nop
    "a64": struct.pack("<4I", 0xF9400803,                  # ldr x3, [x0, #16]
                       0x52800000 | (EXIT_CODE << 5),      # mov w0, #EXIT_CODE
                       0xD61F0060,                         # br x3
                       0xD503201F),                        # nop
}

OTHER = {"a32": "a64", "a64": "a32"}
PN = {"a32": 1, "a64": 2}
REFUSE = "PROFILE|INVAL"
EXACT_PROBE = {"INVAL": probe_corpus.INVAL, "VERSION": probe_corpus.VERSION}


def img(profile, **o):
    o.setdefault("data_align", 4096)
    o.setdefault("text", CODE[profile])
    return build_image(profile, **o)


def entry_of(blob, profile):
    at = 24
    if profile == "a64":
        return struct.unpack_from("<Q", blob, at)[0]
    return struct.unpack_from("<I", blob, at)[0]


def load_vaddr(blob, profile, n):
    """p_vaddr of program header n."""
    if profile == "a64":
        phoff = struct.unpack_from("<Q", blob, 32)[0]
        return struct.unpack_from("<Q", blob, phoff + 56 * n + 16)[0]
    phoff = struct.unpack_from("<I", blob, 28)[0]
    return struct.unpack_from("<I", blob, phoff + 32 * n + 8)[0]


def reloc_code(profile, diff):
    """Entry code, table in r0 / x0: load the word `diff` bytes after its own first
    instruction (the relocation target in the data page) and compare it with the
    address of its own first instruction; exit(EXIT_CODE) through the exit slot when
    they are equal, exit(EXIT_CODE + 1) when not. A RELATIVE relocation with the
    addend "offset of this code" makes them equal at any load base."""
    assert 0 < diff < 65536
    if profile == "a32":
        words = (0xE24F1008,                                         # sub r1, pc, #8
                 0xE3000000 | ((diff >> 12) << 16) | (2 << 12) | (diff & 0xFFF),  # movw r2, #diff
                 0xE0812002,                                         # add r2, r1, r2
                 0xE5923000,                                         # ldr r3, [r2]
                 0xE1530001,                                         # cmp r3, r1
                 0xE590C010,                                         # ldr r12, [r0, #16]
                 0x03A00000 | EXIT_CODE,                             # moveq r0, #EXIT_CODE
                 0x13A00000 | (EXIT_CODE + 1),                       # movne r0, #EXIT_CODE + 1
                 0xE12FFF1C)                                         # bx r12
    else:
        words = (0x10000001,                                         # adr x1, .
                 0xD2800000 | (diff << 5) | 2,                       # movz x2, #diff
                 0x8B020022,                                         # add x2, x1, x2
                 0xF9400043,                                         # ldr x3, [x2]
                 0xEB01007F,                                         # cmp x3, x1
                 0xF9400804,                                         # ldr x4, [x0, #16]
                 0x52800000 | (EXIT_CODE << 5) | 5,                  # mov w5, #EXIT_CODE
                 0x52800000 | ((EXIT_CODE + 1) << 5) | 6,            # mov w6, #EXIT_CODE + 1
                 0x1A8600A0,                                         # csel w0, w5, w6, eq
                 0xD61F0080)                                         # br x4
    return struct.pack("<%dI" % len(words), *words)


def reloc_img(profile, **o):
    """An image whose one relocation makes the word at its target the runtime address
    of the entry code, and whose entry code checks that (reloc_code). Built twice: the
    first pass finds where the code and the data page are."""
    first = img(profile, text=reloc_code(profile, 1), **o)
    text_off = entry_of(first, profile)
    diff = load_vaddr(first, profile, 1) + o.get("reloc_target", 0) - text_off
    return img(profile, text=reloc_code(profile, diff), reloc_addend=text_off, **o)


def corpus(p):
    """The images of profile p: (name, bytes, ids, load, probe)."""
    a64 = p == "a64"
    o = OTHER[p]
    out = []

    def add(name, blob, ids, load=REFUSE, probe="refuse"):
        out.append((name, blob, ids, load, probe))

    def note(**kw):
        return note_bytes(p, **kw)

    base = img(p)

    # ---- accepted images
    add("ok-stack0", base, ["C-ELF.36"], "OK", "ok")
    add("ok-stack-64k", img(p, note=note(stack=64 << 10)), ["C-ELF.36"], "OK", "ok")
    add("ok-stack-1m", img(p, note=note(stack=1 << 20)), ["C-ELF.36"], "OK", "ok")
    # Accepted images whose point is a relocation: the entry code loads the relocated
    # pointer and exits with EXIT_CODE only if it is the runtime address of the code
    # itself (EXIT_CODE + 1 otherwise), so a loader that ignores or misapplies the
    # relocations fails the image.
    add("ok-nosections", reloc_img(p, strip_sections=True), ["C-ELF.16"], "OK", "ok")
    if not a64:
        add("ok-rel", reloc_img(p, reloc_kind="rel"), ["C-ELF.17"], "OK", "ok")
        add("ok-rela", reloc_img(p, reloc_kind="rela"), ["C-ELF.18"], "OK", "ok")
    add("unlisted-profile", base, ["C-ELF.31"], "PROFILE|INVAL", "ok")
    # What C-CLIENT.9 and C-CLIENT.17 read: stack 128 KiB, heap 3 MiB, groups
    # required display and input, optional audio and services.
    add("probe-note", img(p, note=note(stack=128 << 10, heap=3, req=0x3, opt=0x14)),
        [], "ANY", "ok")

    # ---- C-ELF.1 to C-ELF.4: the header
    add("etype-exec", img(p, etype=2), ["C-ELF.1"])
    add("etype-rel", img(p, etype=1), ["C-ELF.1"])
    add("class-other", img(p, ei_class=2 if not a64 else 1), ["C-ELF.2"])
    add("class-0", img(p, ei_class=0), ["C-ELF.2"])
    add("data-msb", img(p, ei_data=2), ["C-ELF.3"])
    add("data-0", img(p, ei_data=0), ["C-ELF.3"])
    add("machine-x86-64", img(p, machine=62), ["C-ELF.4"])
    add("machine-other-profile", img(p, machine=183 if not a64 else 40), ["C-ELF.4"])
    add("machine-0", img(p, machine=0), ["C-ELF.4"])
    if not a64:
        add("float-soft", img(p, eflags=0x05000200), ["C-ELF.5"])
        add("float-none", img(p, eflags=0x05000000), ["C-ELF.5"])
        add("eabi-4", img(p, eflags=0x04000400), ["C-ELF.6"])
        add("eabi-0", img(p, eflags=0x00000400), ["C-ELF.6"])
    else:
        add("eflags-1", img(p, eflags=1), ["C-ELF.7"])
        add("eflags-arm-hard-float", img(p, eflags=0x05000400), ["C-ELF.7"])

    # ---- C-ELF.8 to C-ELF.15: the segments
    add("interp", img(p, interp=True), ["C-ELF.8"])
    add("needed", img(p, needed=True), ["C-ELF.9"])
    add("five-loads", img(p, extra_loads=3), ["C-ELF.10"])
    add("wx-segment", img(p, wx=True), ["C-ELF.11"])
    add("memsz-below-filesz", img(p, load1_memsz=8), ["C-ELF.12"])
    add("overlap", img(p, load1_vaddr=0x10), ["C-ELF.13"])
    huge = 0x4000000000 if a64 else 0xE0000000
    add("segment-huge", img(p, load1_memsz=huge), ["C-ELF.14"],
        "PROFILE|INVAL|NOMEM", "any")
    add("no-dynamic", img(p, no_dynamic=True), ["C-ELF.15"], REFUSE)

    # ---- C-ELF.19 to C-ELF.23: relocations
    odd = {"a32": [2, 21, 0, 1027], "a64": [257, 1025, 0, 23]}[p]
    for t, label in zip(odd, ("abs", "glob-dat", "none", "other-profile")):
        add("reloc-type-" + label, img(p, reloc_type=t), ["C-ELF.19"])
    add("relr", img(p, relr=True), ["C-ELF.20"])
    add("reloc-symbol", img(p, reloc_sym=3), ["C-ELF.21"])
    add("reloc-misaligned-2", img(p, reloc_target=2), ["C-ELF.22"])
    add("reloc-misaligned-4", img(p, reloc_target=4 if a64 else 2), ["C-ELF.22"])
    add("reloc-outside", img(p, reloc_target=0x10000), ["C-ELF.23"], REFUSE)
    # The target lies inside p_memsz of a PT_LOAD with room for the whole word
    # (spec 2.2 rule 5): a gap between two segments and the end of the memory do
    # not count, the bss tail does.
    add("reloc-gap", img(p, load1_gap=0x1000, reloc_target=0x800), ["C-ELF.23"], REFUSE)
    add("reloc-memsz-end", img(p, reloc_target=48), ["C-ELF.23"], REFUSE)
    add("reloc-straddle", img(p, load1_memsz=52 if a64 else 50, reloc_target=48),
        ["C-ELF.23"], REFUSE)
    add("ok-reloc-bss", reloc_img(p, reloc_kind="rela", reloc_target=32), ["C-ELF.23"],
        "OK", "ok")

    # ---- C-ELF.24 and C-ELF.25: the entry point
    add("entry-outside", img(p, entry=0x40000), ["C-ELF.24"])
    add("entry-in-data", img(p, entry_in_data=True), ["C-ELF.24"])
    if not a64:
        add("entry-thumb", img(p, entry=entry_of(base, p) | 1), ["C-ELF.25"])

    # ---- C-ELF.26 to C-ELF.29, C-ELF.38: the note
    add("no-note", img(p, note=None), ["C-ELF.26"])
    add("note-type-2", img(p, note=note(ntype=2)), ["C-ELF.26"])
    add("note-owner-other", img(p, note=struct.pack("<III", 6, 24, 1)
                                + b"OTHER\x00\x00\x00"
                                + struct.pack("<6I", 0x50505A41, 0, 0, 0, 8, 0)),
        ["C-ELF.26"])
    add("note-not-in-pt-note", img(p, no_pt_note=True), ["C-ELF.26"])
    add("two-notes", img(p, second_note=note()), ["C-ELF.27"])
    add("note-magic-bad", img(p, note=note(magic=0x11111111)), ["C-ELF.28"], "INVAL", "INVAL")
    add("note-magic-other", img(p, note=note(magic=0x50505A41)), ["C-ELF.28"],
        "INVAL", "INVAL")
    add("note-magic-swapped", img(p, note=note(magic=0x4141524E)), ["C-ELF.28"],
        "INVAL", "INVAL")
    add("note-version-0", img(p, note=note(version=0)), ["C-ELF.29"], "INVAL", "INVAL")
    add("note-version-2", img(p, note=note(version=2)), ["C-ELF.29"], "INVAL", "INVAL")
    add("note-profile-other", img(p, note=note(prof=PN[o])), ["C-ELF.30"],
        "PROFILE", "INVAL")
    add("note-profile-0", img(p, note=note(prof=0)), ["C-ELF.30"], "PROFILE", "INVAL")
    add("note-profile-9", img(p, note=note(prof=9)), ["C-ELF.30"], "PROFILE", "INVAL")
    add("api-major-1", img(p, note=note(major=1)), ["C-ELF.32"], "VERSION", "VERSION")
    add("api-major-65535", img(p, note=note(major=65535)), ["C-ELF.32"], "VERSION", "VERSION")
    add("api-minor-9999", img(p, note=note(minor=9999)), ["C-ELF.33"], "VERSION", "VERSION")
    add("api-minor-above", img(p, note=note(minor=AAR_API_MINOR + 1)),
        ["C-ELF.33"], "VERSION", "VERSION")
    add("api-minor-below", img(p, note=note(minor=AAR_API_MINOR_OLDEST - 1)),
        ["C-ELF.33"], "VERSION", "VERSION")
    add("api-minor-0", img(p, note=note(minor=0)), ["C-ELF.33"], "VERSION", "VERSION")
    add("stack-4", img(p, note=note(stack=4)), ["C-ELF.34"], "INVAL", "INVAL")
    add("stack-65520", img(p, note=note(stack=(64 << 10) - 16)), ["C-ELF.34"], "INVAL", "INVAL")
    add("stack-4-major-1", img(p, note=note(stack=4, major=1)), ["C-ELF.34"], "INVAL", "INVAL")
    add("stack-65544", img(p, note=note(stack=(64 << 10) + 8)), ["C-ELF.35"], "INVAL", "INVAL")
    add("stack-131080", img(p, note=note(stack=(128 << 10) + 8)), ["C-ELF.35"],
        "INVAL", "INVAL")
    add("group-experimental-28", img(p, note=note(req=1 << 28)), ["C-ELF.37"],
        "NOGROUP", "ok")
    add("group-experimental-31", img(p, note=note(req=1 << 31)), ["C-ELF.37"],
        "NOGROUP", "ok")
    # Reserved bits 5 to 27 are for later versions of the standard, and no platform
    # sets one: a required one is refused like any group the platform lacks. An
    # unknown bit of groups_optional is only a hint (spec 8.1).
    add("group-reserved-5", img(p, note=note(req=1 << 5)), ["C-ELF.37"],
        "NOGROUP", "ok")
    add("group-reserved-27", img(p, note=note(req=1 << 27)), ["C-ELF.37"],
        "NOGROUP", "ok")
    add("group-optional-unknown", img(p, note=note(opt=(1 << 5) | (1 << 27) | (1 << 28))),
        ["C-ELF.37"], "OK", "ok")
    add("note-namesz-8", img(p, note=note(owner=b"AAR\x00", namesz=8,
                                          name_pad=b"\x00" * 4)), ["C-ELF.38"])
    add("note-name-no-nul", img(p, note=note(owner=b"AAR", namesz=3)), ["C-ELF.38"])
    add("note-name-lower", img(p, note=note(owner=b"aar\x00")), ["C-ELF.38"])
    add("note-desc-36", img(p, note=note(descsz=36)), ["C-ELF.38"])
    add("note-desc-41", img(p, note=note(descsz=41)), ["C-ELF.38"])
    add("note-reserved", img(p, note=note(reserved=1)), ["C-ELF.38"], "INVAL", "INVAL")
    add("note-reserved-minor-below", img(p, note=note(reserved=1, minor=AAR_API_MINOR_OLDEST - 1)),
        ["C-ELF.38"], "INVAL", "INVAL")

    # ---- C-ELF.40: more heap than any platform commits. min_heap_mib is a 32-bit
    # count of MiB with no maximum (spec 4.1). 0xFFFFFFFF is the largest word;
    # 0x10000001 MiB is 1 MiB when converted to bytes in 32 bits.
    add("heap-max", img(p, note=note(heap=0xFFFFFFFF)), ["C-ELF.40"],
        "NOMEM|NORESOURCE", "ok")
    add("heap-wrap", img(p, note=note(heap=0x10000001)), ["C-ELF.40"],
        "NOMEM|NORESOURCE", "ok")

    # ---- C-ELF.41 only: files that are not images at all
    add("not-elf", b"not an elf at all", [], REFUSE)
    add("empty", b"", [], REFUSE)
    add("truncated-20", base[:20], [], REFUSE)
    add("truncated-header", base[:40], [], REFUSE)
    add("truncated-mid", base[:len(base) // 2], [], REFUSE)
    return out


def manifest_lines():
    lines = [
        "# Generated by gen_negative.py: do not edit. See that file for the format.",
        "# name profile ids load probe",
    ]
    for p in PROFILES:
        for name, _blob, ids, load, probe in corpus(p):
            lines.append("%s %s %s %s %s" % (name, p, ",".join(ids) or "-", load, probe))
    return "\n".join(lines) + "\n"


def expected_files():
    files = {os.path.join("MANIFEST"): manifest_lines().encode("ascii")}
    for p in PROFILES:
        for name, blob, _ids, _load, _probe in corpus(p):
            files[os.path.join(p, name + EXT)] = blob
    return files


def write(root):
    files = expected_files()
    for rel, data in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
    return files


def tests_rows():
    """C-ELF rows of TESTS.md: id -> (judge, profile)."""
    rows = {}
    with open(os.path.join(ROOT, "conformance", "TESTS.md"), encoding="utf-8") as fh:
        for line in fh:
            m = re.match(r"^\|\s*(C-ELF\.\d+)\s*\|(.*)\|\s*$", line)
            if m:
                cells = [c.strip() for c in m.group(2).split("|")]
                rows[m.group(1)] = (cells[4], cells[3])  # judged by, profile
    return rows


def aar_check(path, profile):
    proc = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "aar-check"),
                           "--profile", profile, path],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, check=False)
    return proc.returncode


def check():
    errors = []
    files = expected_files()
    for rel, data in files.items():
        path = os.path.join(HERE, rel)
        if not os.path.isfile(path):
            errors.append("%s is missing: run gen_negative.py" % rel)
        elif open(path, "rb").read() != data:
            errors.append("%s differs from what the generator makes" % rel)
    for dirpath, _dirs, names in os.walk(HERE):
        for n in names:
            rel = os.path.relpath(os.path.join(dirpath, n), HERE).replace(os.sep, "/")
            if n.endswith(EXT) or n == "MANIFEST":
                if os.path.join(*rel.split("/")) not in files:
                    errors.append("%s is not made by the generator" % rel)

    rows = tests_rows()
    covered = {p: set() for p in PROFILES}
    with tempfile.TemporaryDirectory() as tmp:
        for p in PROFILES:
            for name, blob, ids, load, probe in corpus(p):
                if load != "OK" and load != "ANY":
                    for code in load.split("|"):
                        if code not in ("PROFILE", "INVAL", "VERSION", "NOGROUP", "NOMEM",
                                        "NORESOURCE"):
                            errors.append("%s/%s: unknown load code %s" % (p, name, code))
                for i in ids:
                    if i not in rows:
                        errors.append("%s/%s: %s is not in TESTS.md" % (p, name, i))
                    elif rows[i][0] != "runner":
                        errors.append("%s/%s: %s is not judged by runner" % (p, name, i))
                    covered[p].add(i)
                if probe not in ("ok", "refuse", "any") and probe not in EXACT_PROBE:
                    errors.append("%s/%s: unknown probe code %s" % (p, name, probe))
                if probe != "any":
                    path = os.path.join(tmp, "%s-%s%s" % (p, name, EXT))
                    with open(path, "wb") as fh:
                        fh.write(blob)
                    # The probe applies the rules of spec 2.2 and 2.3 as the loader
                    # does (no section headers needed): aar-check's functions
                    # without the section rules say what that is.
                    runtime = probe_corpus.probe_code(probe_corpus.spec_check(blob)[0])
                    if ((runtime == probe_corpus.OK) != (probe == "ok")
                            or (probe in EXACT_PROBE and runtime != EXACT_PROBE[probe])):
                        errors.append("%s/%s: runtime probe code %d, manifest says %s"
                                      % (p, name, runtime, probe))
                    # aar-check, a lint for build output, also demands section headers.
                    rc = aar_check(path, p)
                    want = 0 if probe == "ok" and name not in NEEDS_NO_SECTIONS else 1
                    if rc != want:
                        errors.append("%s/%s: aar-check says %d, want %d (manifest probe %s)"
                                      % (p, name, rc, want, probe))
    # Every row that a corpus can show has an image for each profile it applies to.
    need = [i for i in rows if int(i.split(".")[1]) <= 38 or i == "C-ELF.40"]
    for i in sorted(need, key=lambda s: int(s.split(".")[1])):
        judge, prof = rows[i]
        if judge != "runner":
            continue
        for p in PROFILES:
            if prof in ("any", p) and i not in covered[p]:
                errors.append("%s has no image for %s" % (i, p))
    return errors


def main(argv):
    if len(argv) == 2 and argv[1] == "--check":
        errors = check()
        for e in errors:
            print("negative: " + e, file=sys.stderr)
        if errors:
            return 1
        n = sum(len(corpus(p)) for p in PROFILES)
        print("negative: %d images match the generator and agree with aar-check" % n)
        return 0
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    files = write(HERE)
    print("negative: wrote %d files" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
