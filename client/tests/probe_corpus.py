#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the probe corpus and run client/tests/probe_test over it.

  probe_corpus.py PROBE_TEST [--write CORPUS]

The oracle is tools/aar-check. The corpus has
  - every image that tests/aar_check_test.py builds (the accept and refuse
    rules of spec 2.2 to 2.4, both profiles), and
  - mutations of four valid images: every byte of the file set to 0x00, 0xFF,
    itself xor 1, itself xor 0x80 and 8 in turn. A mutation that turns a header
    or table field into garbage is the test that the C probe reads nothing out
    of range and decides as aar-check does.
Each image carries aar-check's decision and, for an accepted one, the note
fields as the Python tool reads them. probe_test checks the C probe against
both. An image on which aar-check itself raises (not a CheckError) is counted
and treated as refused: the tool exits 1 on it too, with a traceback.
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


def decide(blob):
    """(accepted, fields) as tools/aar-check decides, fields from its parser."""
    try:
        img = tool.Image(blob)
        img.check_header()
        img.parse_tables()
        loads = tool.check_segments(img)
        tool.check_dynamic(img, loads)
        tool.check_section_relocs(img, loads)
        tool.check_entry(img, loads)
        tool.check_note(img)
    except tool.CheckError:
        return False, None
    except Exception:  # aar-check raises on some damaged input
        CRASHES[0] += 1
        return False, None
    pos, _name, _namesz, _ntype, _descsz, desc = tool.find_aar_notes(img)[0]
    (magic, version, profile, major, minor, stack, heap, req, opt,
     _reserved) = struct.unpack("<10I", desc)
    return True, [profile, major, minor, stack, heap, req, opt, version, len(blob)]


def named_cases():
    """Every image of the aar-check tests, by its test name."""
    cases = []

    def ok(name, blob, args=()):
        cases.append((name, blob, True, bool(args)))

    def bad(name, blob, needle, args=()):
        cases.append((name, blob, False, bool(args)))

    corpus.ok, corpus.bad = ok, bad
    corpus.elf_rules()
    corpus.note_rules()
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


def build_corpus():
    records = []
    labels = 0
    for name, blob, label_ok, has_args in named_cases():
        accepted, fields = decide(blob)
        if not has_args and accepted != label_ok:
            raise SystemExit("aar_check_test labels %r %s but aar-check %s it"
                             % (name, "ok" if label_ok else "bad",
                                "accepts" if accepted else "refuses"))
        labels += 1
        records.append((name, blob, accepted, fields))
    named = len(records)
    accepted_mut = 0
    for name, blob in mutations():
        accepted, fields = decide(blob)
        accepted_mut += accepted
        records.append((name, blob, accepted, fields))
    return records, named, accepted_mut


def write(records, path):
    with open(path, "wb") as fh:
        fh.write(struct.pack("<I", len(records)))
        for name, blob, accepted, fields in records:
            raw = name.encode()
            fh.write(struct.pack("<H", len(raw)) + raw)
            fh.write(struct.pack("<B", 1 if accepted else 0))
            fh.write(struct.pack("<9I", *(fields or [0] * 9)))
            fh.write(struct.pack("<I", len(blob)) + blob)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    probe_test = argv[1]
    keep = argv[3] if len(argv) >= 4 and argv[2] == "--write" else None
    records, named, accepted_mut = build_corpus()
    print("probe corpus: %d named images, %d mutations (%d of them accepted by "
          "aar-check), %d on which aar-check raises"
          % (named, len(records) - named, accepted_mut, CRASHES[0]))
    with tempfile.TemporaryDirectory() as tmp:
        path = keep or os.path.join(tmp, "corpus.bin")
        write(records, path)
        return subprocess.call([probe_test, path])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
