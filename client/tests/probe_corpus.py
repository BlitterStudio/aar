#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the probe corpus and run client/tests/probe_test over it.

  probe_corpus.py PROBE_TEST [--write CORPUS]

The oracle is tools/aar-check, run as a runtime applies spec 2.2 and 2.3: every
rule that does not read a section header (header, segments, PT_DYNAMIC tables,
entry, note descriptor), with the section-only rules left out. The probe accepts
an image without section headers, as the loader does, and aar-check does not;
loader_corpus.py keeps the strict verdict of aar-check as well. The corpus has
  - every image that tests/aar_check_test.py builds (the accept and refuse
    rules of spec 2.2 to 2.4, both profiles), and
  - mutations of four valid images: every byte of the file set to 0x00, 0xFF,
    itself xor 1, itself xor 0x80 and 8 in turn. A mutation that turns a header
    or table field into garbage is the test that the C probe reads nothing out
    of range and decides as the oracle does.
Each image carries the exact decision: an incompatible application API version
is AAR_E_VERSION, other malformed images are AAR_E_INVAL in the probe (the
loader says AAR_E_PROFILE for a note that does not match the ELF class).
For an accepted image the corpus carries the note fields as the Python tool
reads them. An image on which aar-check itself raises (not a CheckError) is
counted and treated as refused: the tool exits 1 on it too, with a traceback.
"""

import importlib.machinery
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

TESTS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(TESTS, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import aar_check_test as corpus  # noqa: E402  (the builders)

loader = importlib.machinery.SourceFileLoader(
    "aar_check_tool", os.path.join(ROOT, "tools", "aar-check"))
spec = importlib.util.spec_from_loader("aar_check_tool", loader)
tool = importlib.util.module_from_spec(spec)
loader.exec_module(tool)

CRASHES = [0]

OK, INVAL, PROFILE, VERSION = 0, 1, 2, 3


def code_of(exc):
    """Translate the independent lint's diagnostic into the loader's result."""
    text = str(exc)
    if text.startswith("note profile "):
        return PROFILE
    if text.startswith(("note api major ", "note api_minor_required ")):
        return VERSION
    return INVAL


def probe_code(code):
    """The probe treats a note/ELF profile mismatch as an invalid image."""
    return INVAL if code == PROFILE else code


def spec_tables(img):
    """parse_tables without the section headers."""
    blob, lay = img.blob, img.lay
    if img.phentsize < lay["phdr"].size:
        tool.fail("unexpected ELF header size")
    if img.phoff + img.phnum * img.phentsize > len(blob):
        tool.fail("truncated ELF program headers")
    img.phdrs = []
    for i in range(img.phnum):
        f = lay["phdr"].unpack_from(blob, img.phoff + i * img.phentsize)
        if img.cls == tool.ELFCLASS32:
            p_type, off, vaddr, _, filesz, memsz, flags, align = f
        else:
            p_type, flags, off, vaddr, _, filesz, memsz, align = f
        img.phdrs.append((p_type, flags, off, vaddr, filesz, memsz, align))


def spec_check_note(img):
    """check_note without the .note.aar section: the one section it asks for is
    replaced by one that covers the whole file."""
    whole = (0, tool.SHT_NOTE, tool.SHF_ALLOC, 0, 0, len(img.blob), 0, 0, 0, 0)
    img.section = lambda wanted: [whole]
    tool.check_note(img)


def strict_code(blob):
    """OK or the refusal code of tools/aar-check as it runs on a build: the
    section headers are required and checked too."""
    try:
        img = tool.Image(blob)
        img.check_header()
        img.parse_tables()
        loads = tool.check_segments(img)
        tool.check_dynamic(img, loads)
        tool.check_section_relocs(img, loads)
        tool.check_entry(img, loads)
        tool.check_note(img)
    except tool.CheckError as exc:
        return code_of(exc)
    except Exception:  # aar-check raises on some damaged input
        CRASHES[0] += 1
        return INVAL
    return OK


def spec_check(blob):
    """(code, img, loads): the rules of spec 2.2 and 2.3 as a runtime applies
    them, with aar-check's own functions for every rule that does not read a
    section header. `img` and `loads` are set for an accepted image."""
    try:
        img = tool.Image(blob)
        img.check_header()
        spec_tables(img)
        loads = tool.check_segments(img)
        tool.check_dynamic(img, loads)
        tool.check_entry(img, loads)
        spec_check_note(img)
    except tool.CheckError as exc:
        return code_of(exc), None, None
    except Exception:
        CRASHES[0] += 1
        return INVAL, None, None
    return OK, img, loads


def decide(blob):
    """(code, fields): the runtime's decision and, for an accepted image, the
    note fields from aar-check's parser."""
    code, img, _loads = spec_check(blob)
    if code != OK:
        return code, None
    _pos, _name, _namesz, _ntype, _descsz, desc = tool.find_aar_notes(img)[0]
    (_magic, version, profile, major, minor, stack, heap, req, opt,
     _reserved) = struct.unpack("<10I", desc)
    return OK, [profile, major, minor, stack, heap, req, opt, version, len(blob)]


def named_cases():
    """Named checker images and compound faults that exercise precedence."""
    cases = []

    def ok(name, blob, args=()):
        cases.append((name, blob, True, bool(args)))

    def bad(name, blob, needle, args=()):
        cases.append((name, blob, False, bool(args)))

    corpus.ok, corpus.bad = ok, bad
    corpus.elf_rules()
    corpus.note_rules()
    for profile in ("a32", "a64"):
        for label, fields in (
                ("wrong major with invalid stack", {"major": 1, "stack": 4}),
                ("old minor with reserved word set",
                 {"minor": tool.AAR_API_MINOR_OLDEST - 1, "reserved": 1})):
            blob = corpus.build(profile, note=corpus.note_bytes(profile, **fields))
            cases.append(("%s [%s]" % (label, profile), blob, False, False))
    return cases


def mutations():
    bases = [
        ("a32", corpus.build("a32")),
        ("a64", corpus.build("a64")),
        ("a32 rela", corpus.build("a32", reloc_kind="rela", reloc_count=2)),
        ("a64 stack", corpus.build("a64", reloc_count=3, note=corpus.note_bytes(
            "a64", stack=64 << 10, heap=4, req=0x9, opt=0x6))),
    ]
    for label, blob in bases:
        for at in range(len(blob)):
            for value in sorted({0x00, 0xFF, blob[at] ^ 1, blob[at] ^ 0x80, 8}):
                if value == blob[at]:
                    continue
                m = bytearray(blob)
                m[at] = value
                yield "mut %s @%d=%02x" % (label, at, value), bytes(m)


def needed_length(blob):
    """The length of the shortest prefix of an accepted image that is still
    accepted: the section headers and the string table after the last byte a
    runtime reads are not needed, so a runtime accepts the image without them."""
    n = len(blob)
    while n > 0 and spec_check(blob[:n - 1])[0] == OK:
        n -= 1
    return n


def build_corpus():
    """(records, named, strict_only): a record is (name, image, code, fields,
    needed length); `strict_only` counts the images that the runtime rules accept
    and the strict aar-check refuses (the ones whose only fault is a section
    rule)."""
    records = []
    strict_only = 0
    for name, blob, label_ok, has_args in named_cases():
        code, fields = decide(blob)
        strict = strict_code(blob)
        if not has_args and (strict == OK) != label_ok:
            raise SystemExit("aar_check_test labels %r %s but aar-check %s it"
                             % (name, "ok" if label_ok else "bad",
                                "accepts" if strict == OK else "refuses"))
        if strict == OK and code != OK:
            raise SystemExit("aar-check accepts %r, which the runtime rules refuse"
                             % name)
        strict_only += code == OK and strict != OK
        records.append((name, blob, code, fields,
                        needed_length(blob) if code == OK else 0))
    named = len(records)
    for name, blob in mutations():
        code, fields = decide(blob)
        records.append((name, blob, code, fields, len(blob) if code == OK else 0))
    return records, named, strict_only


def write(records, path):
    with open(path, "wb") as fh:
        fh.write(struct.pack("<I", len(records)))
        for name, blob, code, fields, need in records:
            raw = name.encode()
            fh.write(struct.pack("<H", len(raw)) + raw)
            fh.write(struct.pack("<B", probe_code(code)))
            fh.write(struct.pack("<9I", *(fields or [0] * 9)))
            # the shortest prefix that is still accepted (0 for a refused image)
            fh.write(struct.pack("<I", need))
            fh.write(struct.pack("<I", len(blob)) + blob)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    probe_test = argv[1]
    keep = argv[3] if len(argv) >= 4 and argv[2] == "--write" else None
    records, named, strict_only = build_corpus()
    accepted = sum(1 for r in records if r[2] == OK)
    if strict_only < 1:
        raise SystemExit("no named image is accepted by the runtime rules and "
                         "refused by aar-check: the section rules are not covered")
    print("probe corpus: %d named images (%d that only aar-check's section rules "
          "refuse), %d mutations, %d accepted by the runtime rules, %d on which "
          "aar-check raises"
          % (named, strict_only, len(records) - named, accepted, CRASHES[0]))
    with tempfile.TemporaryDirectory() as tmp:
        path = keep or os.path.join(tmp, "corpus.bin")
        write(records, path)
        return subprocess.call([probe_test, path])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
