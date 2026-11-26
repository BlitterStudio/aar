#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Feed synthetic ELF32 and ELF64 images and link maps to tools/aar-check.

No pytest. The images are built here, byte by byte, so every accept and
refuse rule of spec 2.2 to 2.4 has a case. Run: python3 tests/aar_check_test.py
"""
import importlib.machinery
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CHECK = os.path.join(ROOT, "tools", "aar-check")

PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE = 1, 2, 3, 4
SHT_RELA, SHT_DYNAMIC, SHT_NOTE, SHT_REL, SHT_RELR = 4, 6, 7, 9, 19
DT_NEEDED, DT_RELA, DT_RELASZ, DT_RELAENT = 1, 7, 8, 9
DT_REL, DT_RELSZ, DT_RELENT, DT_JMPREL = 17, 18, 19, 23
DT_RELRSZ, DT_RELR, DT_RELRENT = 35, 36, 37
R_ARM_RELATIVE, R_AARCH64_RELATIVE = 23, 1027
PROFILE_NUMBER = {"a32": 1, "a64": 2}
A32_FLAGS = 0x05000400  # EABI version 5, hard float


def align(value, to):
    return (value + to - 1) & ~(to - 1)


def note_bytes(profile, *, stack=0, heap=0, req=0, opt=0, magic=0x4E524141,
               version=1, prof=None, major=0, minor=6, reserved=0,
               owner=b"AAR\x00", namesz=None, descsz=None, ntype=1,
               name_pad=b""):
    """One AAR note. Defaults are what AAR_NOTE(stack, heap, req, opt) emits."""
    desc = struct.pack("<10I", magic, version,
                       PROFILE_NUMBER[profile] if prof is None else prof,
                       major, minor, stack, heap, req, opt, reserved)
    name = owner + name_pad
    return struct.pack("<III", len(owner) if namesz is None else namesz,
                       len(desc) if descsz is None else descsz,
                       ntype) + name + b"\x00" * (align(len(name), 4) - len(name)) + desc


def build(profile="a32", **o):
    """A minimal valid image for the profile, changed by keyword options."""
    a64 = profile == "a64"
    ehsz, phsz, shsz = (64, 56, 64) if a64 else (52, 32, 40)
    note = o.get("note", note_bytes(profile))
    kind = o.get("reloc_kind", "rela" if a64 else "rel")
    ent = 8 if kind == "rel" else (24 if a64 else 12)
    nrel = 0 if o.get("no_reloc_tags") else o.get("reloc_count", 1)

    phs = ["load0", "load1"] + ([] if o.get("no_dynamic") else ["dynamic"])
    if note is not None and not o.get("no_pt_note"):
        phs.append("note")
    if o.get("interp"):
        phs.append("interp")
    phs += ["load0"] * o.get("extra_loads", 0)

    cur = ehsz + phsz * len(phs)
    note_off = align(cur, 8)
    note_blob = (note or b"") + o.get("second_note", b"")
    cur = note_off + len(note_blob)
    text_off = align(cur, 8)
    text = o.get("text", b"\x00" * 8)  # the code at e_entry (the conformance corpus runs it)
    cur = text_off + len(text)
    rel_off = align(cur, 8)
    cur = rel_off + nrel * ent

    dyn_fmt = "<qQ" if a64 else "<II"
    dyn = []
    if o.get("no_reloc_tags"):
        pass
    elif o.get("relr"):
        dyn += [(DT_RELR, rel_off), (DT_RELRSZ, 8), (DT_RELRENT, 8)]
    elif o.get("jmprel"):
        dyn += [(DT_JMPREL, rel_off)]
    elif kind == "rel":
        dyn += [(DT_REL, rel_off), (DT_RELSZ, nrel * ent), (DT_RELENT, ent)]
    else:
        dyn += [(DT_RELA, rel_off), (DT_RELASZ, nrel * ent),
                (DT_RELAENT, o.get("dt_ent", ent))]
    if o.get("drop_dt_ent"):
        dyn = dyn[:2]
    if o.get("needed"):
        dyn.append((DT_NEEDED, 1))
    dyn += o.get("dt_extra", [])
    dyn.append((0, 0))
    dyn_off = align(cur, 8)
    dyn_size = len(dyn) * struct.calcsize(dyn_fmt)
    cur = dyn_off + dyn_size
    data_off = align(cur, o.get("data_align", 16))  # 4096: the RW segment on its own page

    reloc_type = o.get("reloc_type", R_AARCH64_RELATIVE if a64 else R_ARM_RELATIVE)
    r_off = data_off + o.get("reloc_target", 0)
    rel_blob = b""
    for _ in range(nrel):
        if a64:
            info = (o.get("reloc_sym", 0) << 32) | reloc_type
            rel_blob += struct.pack("<QQq", r_off, info, o.get("reloc_addend", 0))
        else:
            info = (o.get("reloc_sym", 0) << 8) | reloc_type
            rel_blob += struct.pack("<II", r_off, info)
            if kind == "rela":
                rel_blob += struct.pack("<I", o.get("reloc_addend", 0))
    data_len = 32
    shstr = b"\x00"
    name_at = {}

    def name(text):
        nonlocal shstr
        name_at[text] = len(shstr)
        shstr += text.encode() + b"\x00"
        return name_at[text]

    shdr_fmt = "<IIQQQQIIQQ" if a64 else "<IIIIIIIIII"
    shdrs = [struct.pack(shdr_fmt, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]

    def sh(label, typ, flags, off, size, addr=None, ent_size=0):
        shdrs.append(struct.pack(shdr_fmt, name(label), typ, flags,
                                 off if addr is None else addr, off, size, 0, 0,
                                 8, ent_size))

    if not (o.get("no_reloc_section") or o.get("no_reloc_tags")):
        default_name = ".rela.dyn" if kind == "rela" else ".rel.dyn"
        sh(o.get("reloc_section", default_name),
           o.get("reloc_sh_type", SHT_RELA if kind == "rela" else SHT_REL), 2,
           rel_off, nrel * ent, ent_size=ent)
    if not o.get("no_dynamic"):
        sh(".dynamic", SHT_DYNAMIC, 2, dyn_off, dyn_size,
           ent_size=struct.calcsize(dyn_fmt))
    if note is not None and not o.get("no_note_section"):
        sh(o.get("note_section", ".note.aar"), SHT_NOTE, 2, note_off, len(note))
    if o.get("relr_section"):
        sh(".relr.dyn", SHT_RELR, 2, rel_off, 8)
    shstr_off = data_off + data_len
    shstr_name = name(".shstrtab")
    shdrs.append(struct.pack(shdr_fmt, shstr_name, 3, 0, 0, shstr_off,
                             len(shstr), 0, 0, 1, 0))
    shnum = len(shdrs)
    shstr_final = shstr
    shoff = align(shstr_off + len(shstr_final), 8)

    entry = o.get("entry", data_off + 4 if o.get("entry_in_data") else text_off)
    load0_flags = 5
    load1_flags = 7 if o.get("wx") else 6
    load1_vaddr = o.get("load1_vaddr", data_off + o.get("load1_gap", 0))
    load1_memsz = o.get("load1_memsz", data_len + 16)
    phdr_rows = []
    for kind_name in phs:
        if kind_name == "load0":
            row = (PT_LOAD, load0_flags, 0, 0, data_off, data_off, 0x1000)
        elif kind_name == "load1":
            row = (PT_LOAD, load1_flags, data_off, load1_vaddr, data_len,
                   load1_memsz, 0x1000)
        elif kind_name == "dynamic":
            row = (PT_DYNAMIC, 6, dyn_off, dyn_off, dyn_size, dyn_size, 8)
        elif kind_name == "note":
            row = (PT_NOTE, 4, note_off + o.get("note_seg_shift", 0),
                   note_off, o.get("note_filesz", len(note_blob)),
                   o.get("note_filesz", len(note_blob)), 4)
        else:
            row = (PT_INTERP, 4, text_off, text_off, 8, 8, 1)
        phdr_rows.append(row)

    total = shoff + shnum * shsz
    blob = bytearray(total)
    blob[0:4] = b"\x7fELF"
    blob[4] = o.get("ei_class", 2 if a64 else 1)
    blob[5] = o.get("ei_data", 1)
    blob[6] = 1
    eflags = o.get("eflags", 0 if a64 else A32_FLAGS)
    machine = o.get("machine", 183 if a64 else 40)
    if a64:
        struct.pack_into("<HHIQQQIHHHHHH", blob, 16, o.get("etype", 3), machine,
                         1, entry, ehsz, shoff, eflags, ehsz, phsz, len(phs),
                         shsz, shnum, shnum - 1)
    else:
        struct.pack_into("<HHIIIIIHHHHHH", blob, 16, o.get("etype", 3), machine,
                         1, entry, ehsz, shoff, eflags, ehsz, phsz, len(phs),
                         shsz, shnum, shnum - 1)
    if o.get("strip_sections"):
        # e_shoff, e_shnum and e_shstrndx all zero, as after `strip --strip-sections`
        struct.pack_into("<Q" if a64 else "<I", blob, 40 if a64 else 32, 0)
        struct.pack_into("<HH", blob, 60 if a64 else 48, 0, 0)
    for i, (typ, flags, off, vaddr, filesz, memsz, al) in enumerate(phdr_rows):
        at = ehsz + i * phsz
        if a64:
            struct.pack_into("<IIQQQQQQ", blob, at, typ, flags, off, vaddr,
                             vaddr, filesz, memsz, al)
        else:
            struct.pack_into("<IIIIIIII", blob, at, typ, off, vaddr, vaddr,
                             filesz, memsz, flags, al)
    blob[note_off:note_off + len(note_blob)] = note_blob
    blob[text_off:text_off + len(text)] = text
    blob[rel_off:rel_off + len(rel_blob)] = rel_blob
    if kind == "rel" and o.get("reloc_addend") and 0 <= o.get("reloc_target", 0) <= data_len - 4:
        # A REL entry has no addend: it is the word at the target (the conformance
        # corpus uses it to make the relocated pointer point at known code).
        struct.pack_into("<Q" if a64 else "<I", blob, r_off, o["reloc_addend"])
    for i, (tag, val) in enumerate(dyn):
        struct.pack_into(dyn_fmt, blob, dyn_off + i * struct.calcsize(dyn_fmt),
                         tag, val)
    blob[shstr_off:shstr_off + len(shstr_final)] = shstr_final
    blob[shoff:shoff + shnum * shsz] = b"".join(shdrs)
    return bytes(blob)


FAILURES = []
COUNT = [0]


def run(args, blobs=()):
    with tempfile.TemporaryDirectory() as root:
        paths = []
        for i, blob in enumerate(blobs):
            path = os.path.join(root, "app%d.elf" % i)
            with open(path, "wb") as fh:
                fh.write(blob)
            paths.append(path)
        proc = subprocess.run([sys.executable, CHECK] + args + paths,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True, check=False)
        return proc.returncode, proc.stdout, proc.stderr


def record(name, got, want_rc, needle, out, err):
    COUNT[0] += 1
    if got != want_rc or needle not in (out + err):
        FAILURES.append("%s: exit %s (want %s), needle %r\n  stdout=%r\n  stderr=%r"
                        % (name, got, want_rc, needle, out, err))


def ok(name, blob, args=()):
    rc, out, err = run(list(args), [blob])
    record(name, rc, 0, " ok", out, err)


def bad(name, blob, needle, args=()):
    rc, out, err = run(list(args), [blob])
    record(name, rc, 1, needle, out, err)


def usage(name, args, blobs=()):
    rc, out, err = run(list(args), list(blobs))
    record(name, rc, 2, "usage:", out, err)


def elf_rules():
    for profile in ("a32", "a64"):
        ok("minimal image [%s]" % profile, build(profile))
        ok("--profile matches [%s]" % profile, build(profile),
           ["--profile", profile])
        ok("--profile=%s [%s]" % (profile, profile), build(profile),
           ["--profile=" + profile])
        bad("--profile mismatches the class [%s]" % profile, build(profile),
            "does not match profile",
            ["--profile", "a64" if profile == "a32" else "a32"])
        ok("stack 64 KiB [%s]" % profile,
           build(profile, note=note_bytes(profile, stack=64 << 10)))
        ok("stack 1 MiB [%s]" % profile,
           build(profile, note=note_bytes(profile, stack=1 << 20)))
        ok("every defined group [%s]" % profile,
           build(profile, note=note_bytes(profile, req=0x1F, opt=0x1F, heap=4)))
        ok("experimental group bits [%s]" % profile,
           build(profile, note=note_bytes(profile, opt=0x80000000)))
        # Spec 8.1: the group words are not judged. A platform refuses a required
        # group it lacks (AAR_E_NOGROUP), whether the bit is defined, reserved or
        # experimental; unknown optional bits are a hint.
        ok("reserved group bits 5 to 27, required [%s]" % profile,
           build(profile, note=note_bytes(profile, req=0x0FFFFFE0)))
        ok("reserved group bits 5 to 27, optional [%s]" % profile,
           build(profile, note=note_bytes(profile, opt=0x0FFFFFE0)))
        ok("experimental group bits, required [%s]" % profile,
           build(profile, note=note_bytes(profile, req=0xF0000000)))
        ok("api minor 0 [%s]" % profile,
           build(profile, note=note_bytes(profile, minor=0)))
        ok("api minor 5 [%s]" % profile,
           build(profile, note=note_bytes(profile, minor=5)))
        ok("two relocations, PT_LOAD with BSS [%s]" % profile,
           build(profile, reloc_count=2))
        bad("big-endian [%s]" % profile, build(profile, ei_data=2),
            "not a little-endian")
        bad("ET_EXEC [%s]" % profile, build(profile, etype=2),
            "e_type is not ET_DYN")
        bad("wrong machine [%s]" % profile,
            build(profile, machine=62), "e_machine 62")
        bad("W+X segment [%s]" % profile, build(profile, wx=True),
            "writable and executable")
        bad("PT_INTERP [%s]" % profile, build(profile, interp=True),
            "PT_INTERP is present")
        bad("DT_NEEDED [%s]" % profile, build(profile, needed=True),
            "DT_NEEDED")
        bad("DT_RELR [%s]" % profile, build(profile, relr=True),
            "DT_RELR is not allowed")
        bad("SHT_RELR section [%s]" % profile, build(profile, relr_section=True),
            "SHT_RELR")
        bad("DT_JMPREL [%s]" % profile, build(profile, jmprel=True),
            "DT_JMPREL")
        bad("five PT_LOAD [%s]" % profile, build(profile, extra_loads=3),
            "5 PT_LOAD")
        bad("overlapping PT_LOAD [%s]" % profile,
            build(profile, load1_vaddr=0x10), "PT_LOAD segments overlap")
        bad("p_memsz below p_filesz [%s]" % profile,
            build(profile, load1_memsz=8), "p_memsz")
        bad("no PT_DYNAMIC [%s]" % profile, build(profile, no_dynamic=True),
            "0 PT_DYNAMIC")
        bad("entry outside the text segment [%s]" % profile,
            build(profile, entry=0x4000), "not inside an executable PT_LOAD")
        bad("entry in a writable segment [%s]" % profile,
            build(profile, entry_in_data=True), "not inside an executable")
        bad("relocation symbol not 0 [%s]" % profile,
            build(profile, reloc_sym=3), "reloc 0 is not")
        bad("relocation target misaligned [%s]" % profile,
            build(profile, reloc_target=2), "not 4-byte aligned"
            if profile == "a32" else "not 8-byte aligned")
        bad("relocation target outside the image [%s]" % profile,
            build(profile, reloc_target=0x10000), "outside every PT_LOAD")
        # Spec 2.2 rule 5: the target is inside p_memsz of a PT_LOAD (the bss
        # counts, a gap between segments does not) with room for the whole word.
        bad("relocation target in a gap between segments [%s]" % profile,
            build(profile, load1_gap=0x1000, reloc_target=0x800),
            "outside every PT_LOAD")
        ok("relocation target in the bss tail [%s]" % profile,
           build(profile, reloc_target=32))
        bad("relocation target without room for the whole word [%s]" % profile,
            build(profile, load1_memsz=50 if profile == "a32" else 52,
                  reloc_target=48), "outside every PT_LOAD")
        bad("relocation section with a stray name [%s]" % profile,
            build(profile, reloc_section=".rel.plt"),
            "allocated relocation is named .rel.plt")
        ok("zero relocations, no tables [%s]" % profile,
           build(profile, no_reloc_tags=True))
        ok("zero relocations, empty tables [%s]" % profile,
           build(profile, reloc_count=0))
        ok("no relocation section, table in PT_DYNAMIC [%s]" % profile,
           build(profile, no_reloc_section=True))
        bad("zero relocations, no note [%s]" % profile,
            build(profile, no_reloc_tags=True, note=None), "no AAR note")
        bad("relocation symbol still refused beside the zero cases [%s]" % profile,
            build(profile, reloc_count=1, reloc_sym=1), "reloc 0 is not")
        bad("stripped section headers [%s]" % profile,
            build(profile, strip_sections=True), "section headers are missing")
        bad("DT_RELAENT missing [%s]" % profile,
            build(profile, reloc_kind="rela", drop_dt_ent=True),
            "must all be present")
        bad("DT_RELAENT wrong [%s]" % profile,
            build(profile, reloc_kind="rela", dt_ent=16), "entry size 16")
        bad("not an ELF [%s]" % profile, b"not an elf at all", "not a little-endian")
        bad("truncated header [%s]" % profile, build(profile)[:20],
            "truncated")

    # A32 specifics
    bad("A32 soft float", build("a32", eflags=0x05000200), "soft-float")
    bad("A32 no float flag", build("a32", eflags=0x05000000),
        "EF_ARM_ABI_FLOAT_HARD is not set")
    bad("A32 EABI version 4", build("a32", eflags=0x04000400), "EABI version 5")
    bad("A32 entry with the Thumb bit", build("a32", entry=0x81), "Thumb bit")
    ok("A32 RELA table", build("a32", reloc_kind="rela"))
    bad("A32 R_ARM_ABS32", build("a32", reloc_type=2),
        "reloc 0 is not R_ARM_RELATIVE/0")
    bad("A32 R_ARM_GLOB_DAT", build("a32", reloc_type=21),
        "reloc 0 is not R_ARM_RELATIVE/0")
    bad("A32 relocation type 3", build("a32", reloc_type=3),
        "reloc 0 is not R_ARM_RELATIVE/0")
    bad("A32 image with the A64 machine", build("a32", machine=183),
        "e_machine 183")

    # A64 specifics
    bad("A64 nonzero e_flags", build("a64", eflags=1), "e_flags 0x1 is not 0")
    bad("A64 REL table", build("a64", reloc_kind="rel"),
        "A64 images use DT_RELA")
    bad("A64 R_AARCH64_ABS64", build("a64", reloc_type=257),
        "reloc 0 is not R_AARCH64_RELATIVE/0")
    bad("A64 R_AARCH64_GLOB_DAT", build("a64", reloc_type=1025),
        "reloc 0 is not R_AARCH64_RELATIVE/0")
    bad("A64 R_ARM_RELATIVE value", build("a64", reloc_type=23),
        "reloc 0 is not R_AARCH64_RELATIVE/0")
    bad("A64 image with the ARM machine", build("a64", machine=40),
        "e_machine 40")
    bad("class mismatch, ELF64 checked as a32", build("a64"),
        "does not match profile", ["--profile", "a32"])


def note_rules():
    for profile in ("a32", "a64"):
        def case(label, needle, **kw):
            bad("%s [%s]" % (label, profile),
                build(profile, note=note_bytes(profile, **kw)), needle)

        case("bad magic", "note magic", magic=0x11111111)
        case("a foreign note magic", "note magic", magic=0x50505A41)
        case("note version 2", "note version 2", version=2)
        case("note version 0", "note version 0", version=0)
        case("A32 note in an A64 image" if profile == "a64" else
             "A64 note in an A32 image", "does not match", prof=3 - PROFILE_NUMBER[profile])
        case("unknown profile word", "does not match", prof=9)
        case("api major 1", "note api major 1 is not 0", major=1)
        case("api minor 7", "above 6", minor=7)
        case("stack 4", "note stack 4", stack=4)
        case("stack below 64 KiB", "note stack", stack=(64 << 10) - 16)
        case("stack above 1 MiB", "note stack", stack=(1 << 20) + 16)
        case("stack not a multiple of 16", "note stack", stack=(64 << 10) + 8)
        case("reserved word set", "reserved word", reserved=1)
        case("namesz 8 owner", "note owner", owner=b"AAR\x00",
             namesz=8, name_pad=b"\x00" * 4)
        case("owner without the NUL", "note owner", owner=b"AAR", namesz=3)
        case("note type 2", "note type 2", ntype=2)
        case("descriptor 36 bytes", "descriptor is 36 bytes", descsz=36)
        case("descriptor 41 bytes", "runs past its segment", descsz=41)
        bad("owner other than AAR only [%s]" % profile,
            build(profile, note=struct.pack("<III", 6, 24, 1) + b"OTHER\x00\x00\x00"
                  + struct.pack("<6I", 0x50505A41, 0, 0, 0, 8, 0)),
            "no AAR note")
        bad("no note [%s]" % profile, build(profile, note=None),
            "no AAR note")
        bad("note outside PT_NOTE [%s]" % profile,
            build(profile, no_pt_note=True), "no AAR note in a PT_NOTE")
        bad("no .note.aar section [%s]" % profile,
            build(profile, no_note_section=True), "0 .note.aar sections")
        bad("note section misnamed [%s]" % profile,
            build(profile, note_section=".note.other"), "0 .note.aar sections")
        bad("note truncated by the segment size [%s]" % profile,
            build(profile, note_filesz=len(note_bytes(profile)) - 4),
            "runs past its segment")
        bad("note truncated by 1 byte [%s]" % profile,
            build(profile, note_filesz=len(note_bytes(profile)) - 1),
            "runs past its segment")
        bad("PT_NOTE not 4-byte aligned [%s]" % profile,
            build(profile, note_seg_shift=2), "PT_NOTE is not 4-byte aligned")
        bad("stray bytes after the note [%s]" % profile,
            build(profile, second_note=b"\x01\x02\x03\x04"), "stray bytes")
        bad("two AAR notes [%s]" % profile,
            build(profile, second_note=note_bytes(profile)),
            "2 AAR notes")
        ok("zero padding after the note [%s]" % profile,
           build(profile, second_note=b"\x00\x00\x00\x00"))
        ok("a GNU build-id note beside the AAR note [%s]" % profile,
           build(profile, second_note=struct.pack("<III", 4, 4, 3)
                 + b"GNU\x00abcd"))


def expect_note_rules():
    for profile in ("a32", "a64"):
        note = note_bytes(profile, stack=64 << 10, heap=4, req=0x9, opt=0x6)
        blob = build(profile, note=note)
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "app.elf")
            with open(path, "wb") as fh:
                fh.write(blob)

            def expect(label, nums, rc_want, needle):
                proc = subprocess.run(
                    [sys.executable, CHECK, "--expect-note", path] + nums,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    universal_newlines=True, check=False)
                record("%s [%s]" % (label, profile), proc.returncode, rc_want,
                       needle, proc.stdout, proc.stderr)

            expect("--expect-note matches", [str(64 << 10), "4", "9", "6"], 0,
                   "matches")
            expect("--expect-note hex arguments", ["0x10000", "0x4", "0x9", "0x6"],
                   0, "matches")
            expect("--expect-note stack differs", ["0", "4", "9", "6"], 1,
                   "expected")
            expect("--expect-note heap differs", [str(64 << 10), "5", "9", "6"], 1,
                   "expected")
            expect("--expect-note required groups differ",
                   [str(64 << 10), "4", "1", "6"], 1, "expected")
            expect("--expect-note optional groups differ",
                   [str(64 << 10), "4", "9", "0"], 1, "expected")
            expect("--expect-note too few arguments", ["0", "0", "0"], 2, "usage:")
            expect("--expect-note too many arguments",
                   ["0", "0", "0", "0", "0"], 2, "usage:")
            expect("--expect-note not a number", ["x", "0", "0", "0"], 2, "usage:")
            expect("--expect-note negative", ["-1", "0", "0", "0"], 2, "usage:")
        # the minor AAR_NOTE() emits is AAR_API_MINOR; an older minor differs
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "app.elf")
            with open(path, "wb") as fh:
                fh.write(build(profile, note=note_bytes(profile, minor=3)))
            proc = subprocess.run(
                [sys.executable, CHECK, "--expect-note", path, "0", "0", "0", "0"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True, check=False)
            record("--expect-note api minor differs [%s]" % profile,
                   proc.returncode, 1, "expected", proc.stdout, proc.stderr)
        # the other profile's note bytes are not the same
        other = "a32" if profile == "a64" else "a64"
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "app.elf")
            with open(path, "wb") as fh:
                fh.write(build(profile, note=note_bytes(other)))
            proc = subprocess.run(
                [sys.executable, CHECK, "--expect-note", path, "0", "0", "0", "0"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True, check=False)
            record("--expect-note profile word differs [%s]" % profile,
                   proc.returncode, 1, "expected", proc.stdout, proc.stderr)


HEAD = """Archive member included to satisfy reference by file (symbol)

"""

A32_MAP = HEAD + """build/a32/libaar.a(aar.o)
                              build/a32/templates/freestanding.o (aar_log)
build/a32/libaar.a(freestanding.o)
                              build/a32/templates/freestanding.o (memcpy)
/opt/arm/lib/gcc/arm-none-eabi/15.3.1/thumb/v7-a+fp/hard/libgcc.a(_udivsi3.o)
                              build/a32/templates/freestanding.o (__aeabi_uidiv)

Linker script and memory map

 .text.aar_log  0x00000100       0x20 build/a32/libaar.a(aar.o)
"""

A64_MAP = HEAD + """build/a64/libaar.a(aar.o)
                              build/a64/templates/freestanding.o (aar_log)
/opt/arm/lib/gcc/aarch64-none-elf/15.3.1/libgcc.a(lse-init.o)
                              build/a64/libaar.a(aar.o) (__aarch64_have_lse_atomics)
/opt/arm/lib/gcc/aarch64-none-elf/15.3.1/libgcc.a(_clz.o)
                              build/a64/templates/freestanding.o (__clzsi2)

Linker script and memory map

LOAD /opt/arm/aarch64-none-elf/lib/libc.a
 .text.aar_log  0x0000000000000100       0x20 build/a64/libaar.a(aar.o)
"""

CMAKE_OBJECTS = HEAD + """/opt/arm/lib/gcc/arm-none-eabi/15.3.1/libgcc.a(_udivsi3.o)
                              CMakeFiles/app.dir/templates/freestanding.c.obj (__aeabi_uidiv)

Linker script and memory map

 .text.aar_log  0x00000100       0x20 CMakeFiles/app.dir/libaar/src/aar.c.obj
"""


def swap_member(text, member):
    return text.replace("\nLinker script", member + "\nLinker script", 1)


def map_rules():
    cases = [
        ("a32 freestanding map", A32_MAP, 0, b""),
        ("a64 freestanding map", A64_MAP, 0, b""),
        ("cmake map, objects only", CMAKE_OBJECTS, 0, b""),
        ("a32 newlib member", swap_member(
            A32_MAP, "/opt/aar-pic-newlib/arm-none-eabi/lib/libc.a(lib_a-malloc.o)\n"
            "                              build/hello/hello.o (malloc)\n"),
         1, b"libc.a(lib_a-malloc.o)"),
        ("a64 newlib member", swap_member(
            A64_MAP, "/opt/aar-pic-newlib/aarch64-none-elf/lib/libc.a(lib_a-memcpy.o)\n"
            "                              build/hello/hello.o (memcpy)\n"),
         1, b"libc.a(lib_a-memcpy.o)"),
        ("libm member", swap_member(
            A64_MAP, "libm.a(lib_a-sf_sin.o)\n"
            "                              build/hello/hello.o (sinf)\n"),
         1, b"libm.a(lib_a-sf_sin.o)"),
        ("libnosys member, Windows path", swap_member(
            A32_MAP, "C:\\\\sysroot\\\\arm-none-eabi\\\\lib\\\\libnosys.a(_exit.o)\n"
            "                              (_exit)\n"),
         1, b"libnosys.a"),
        ("an archive that is not libaar.a", A32_MAP.replace("libaar.a", "libother.a"),
         1, b"libother.a"),
        ("not a link map", "Memory Configuration\n", 1, b"not a GNU ld link map"),
    ]
    for name, text, rc_want, needle in cases:
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "app.map")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
            proc = subprocess.run([sys.executable, CHECK, "--no-libc", path],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  check=False)
            out = (proc.stdout + proc.stderr).decode("utf-8", "replace")
            record("--no-libc: " + name, proc.returncode, rc_want,
                   needle.decode(), out, "")
    proc = subprocess.run([sys.executable, CHECK, "--no-libc"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, check=False)
    record("--no-libc without a map", proc.returncode, 2, "usage:", proc.stdout,
           proc.stderr)
    proc = subprocess.run([sys.executable, CHECK, "--no-libc",
                           os.path.join(ROOT, "no-such.map")],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, check=False)
    record("--no-libc with a missing map", proc.returncode, 1, "no-such.map",
           proc.stdout, proc.stderr)


def cli_rules():
    usage("no arguments", [])
    usage("unknown option", ["--frobnicate"], [build("a32")])
    usage("bad profile name", ["--profile", "a16"], [build("a32")])
    usage("--profile without a value", ["--profile"])
    usage("--profile without an image", ["--profile", "a32"])
    rc, out, err = run([], [build("a32"), build("a64")])
    record("two images, profiles inferred", rc, 0, "a64 ok", out, err)
    rc, out, err = run([], [build("a32"), build("a64", wx=True), build("a32")])
    record("one bad image among three fails the run", rc, 1,
           "writable and executable", out, err)
    COUNT[0] += 1
    if out.count(" ok:") != 2:
        FAILURES.append("good images before and after a bad one must both be "
                        "reported: %r" % out)
    proc = subprocess.run([sys.executable, CHECK,
                           os.path.join(ROOT, "no-such.elf")],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, check=False)
    record("missing image", proc.returncode, 1, "no-such.elf", proc.stdout,
           proc.stderr)


def header_constants():
    """The tool's numbers must be the ones in include/aar/app.h."""
    header = os.path.join(ROOT, "include", "aar", "app.h")
    with open(header, encoding="utf-8") as fh:
        text = fh.read()

    def define(name):
        match = re.search(r"#define\s+%s\s+\(?(0x[0-9A-Fa-f]+|\d+)u?\)?" % name,
                          text)
        if not match:
            raise SystemExit("%s is not defined in %s" % (name, header))
        return int(match.group(1), 0)

    loader = importlib.machinery.SourceFileLoader("aar_check_tool", CHECK)
    spec = importlib.util.spec_from_loader("aar_check_tool", loader)
    tool = importlib.util.module_from_spec(spec)
    loader.exec_module(tool)
    pairs = [
        ("AAR_API_MAJOR", tool.AAR_API_MAJOR),
        ("AAR_API_MINOR", tool.AAR_API_MINOR),
        ("AAR_NOTE_MAGIC", tool.NOTE_MAGIC),
        ("AAR_NOTE_VERSION", tool.NOTE_VERSION),
        ("AAR_NOTE_TYPE", tool.NOTE_TYPE),
        ("AAR_NOTE_DESC_BYTES", tool.NOTE_DESC_BYTES),
        ("AAR_PROFILE_A32", tool.PROFILE_NUMBER["a32"]),
        ("AAR_PROFILE_A64", tool.PROFILE_NUMBER["a64"]),
        ("AAR_GROUPS_DEFINED", tool.GROUPS_DEFINED),
        ("AAR_GROUPS_EXPERIMENTAL", tool.GROUPS_EXPERIMENTAL),
    ]
    for name, value in pairs:
        COUNT[0] += 1
        if define(name) != value:
            FAILURES.append("tools/aar-check has %s = %#x, include/aar/app.h "
                            "says %#x" % (name, value, define(name)))


def main():
    if not os.path.isfile(CHECK):
        raise SystemExit("missing %s" % CHECK)
    header_constants()
    elf_rules()
    note_rules()
    expect_note_rules()
    map_rules()
    cli_rules()
    if FAILURES:
        sys.stderr.write("\n".join(FAILURES) + "\n")
        raise SystemExit("%d of %d aar-check cases failed"
                         % (len(FAILURES), COUNT[0]))
    print("aar-check tests passed (%d cases)" % COUNT[0])


if __name__ == "__main__":
    main()
