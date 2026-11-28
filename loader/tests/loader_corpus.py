#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the loader corpus and run loader/tests/corpus_test over it.

  loader_corpus.py CORPUS_TEST [--write CORPUS]

The oracle is tools/aar-check, the independent second implementation of spec 2.2
and 2.3. The corpus is the one of client/tests/probe_corpus.py: every image that
tests/aar_check_test.py builds, and mutations of four valid images (every byte
set to 0x00, 0xFF, itself xor 1, itself xor 0x80 and 8 in turn).

The module has two sets of rules, and the corpus carries the decision of each:

  strict  aar_image_check with AAR_IMAGE_SECTIONS: exactly aar-check, section
          headers included. Decision and note fields must be the same.
  spec    aar_image_check without it, and aar_load_plan: spec 2.2 rule 4, the
          loader reads PT_DYNAMIC and the section headers may be absent. The
          oracle is aar-check's own functions for every rule that does not read
          a section header (header, segments, PT_DYNAMIC tables, entry, note
          descriptor), with the section-only rules left out: the sections are
          not parsed, the relocation sections are not read, and the note need not
          lie in a .note.aar section. aar_load_plan adds one rule of its own: a
          segment must not end beyond the 64-bit address space.

Each record has the code of each decision (0 accepted, 1 AAR_E_INVAL, 2
AAR_E_PROFILE for a note/ELF profile mismatch, 3 AAR_E_VERSION for an
incompatible application API) and what an accepted image says: the note, the
number of segments with memory, the span, the entry offset, and the number and
the entries of the relocation tables.
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
sys.path.insert(0, os.path.join(ROOT, "client", "tests"))

import probe_corpus as pc  # noqa: E402  (the corpus and the aar-check module)

tool = pc.tool
CRASHES = pc.CRASHES

OK, INVAL, PROFILE = pc.OK, pc.INVAL, pc.PROFILE
FIELDS = 13


def reloc_tables(img):
    """(tables, entries) of an accepted image: what check_dynamic validated."""
    dyn = [p for p in img.phdrs if p[0] == tool.PT_DYNAMIC][0]
    tags = dict(tool.read_dynamic(img, dyn[2], dyn[4]))
    a32 = img.profile == "a32"
    tables = entries = 0
    if tool.DT_REL in tags or tool.DT_RELSZ in tags or tool.DT_RELENT in tags:
        tables += 1
        entries += tags[tool.DT_RELSZ] // 8
    if tool.DT_RELA in tags or tool.DT_RELASZ in tags or tool.DT_RELAENT in tags:
        tables += 1
        entries += tags[tool.DT_RELASZ] // (12 if a32 else 24)
    return tables, entries


def decide(blob):
    """(strict code, spec code, overflow, fields) as aar-check decides."""
    strict = pc.strict_code(blob)
    spec, img, loads = pc.spec_check(blob)
    overflow, fields = 0, [0] * FIELDS
    if spec == OK:
        _pos, _name, _namesz, _ntype, _descsz, desc = tool.find_aar_notes(img)[0]
        (_magic, version, profile, major, minor, stack, heap, req, opt,
         _reserved) = struct.unpack("<10I", desc)
        mem = [l for l in loads if l[3]]  # (vaddr, off, filesz, memsz, flags)
        span = max(v + m for v, _, _, m, _ in mem)
        overflow = 1 if span >= 1 << 64 else 0
        tables, entries = reloc_tables(img)
        fields = [profile, major, minor, stack, heap, req, opt, version,
                  len(mem), span & (2 ** 64 - 1), img.entry, entries, tables]
    if strict == OK and spec != OK:
        raise SystemExit("aar-check accepts an image that the spec rules refuse")
    return strict, spec, overflow, fields


def build_corpus():
    records = []
    named = 0
    for name, blob, _label_ok, _has_args in pc.named_cases():
        records.append((name,) + (blob,) + decide(blob))
        named += 1
    for name, blob in pc.mutations():
        records.append((name,) + (blob,) + decide(blob))
    return records, named


def write(records, path):
    with open(path, "wb") as fh:
        fh.write(struct.pack("<I", len(records)))
        for name, blob, strict, spec, overflow, fields in records:
            raw = name.encode()
            fh.write(struct.pack("<H", len(raw)) + raw)
            fh.write(struct.pack("<BBB", strict, spec, overflow))
            fh.write(struct.pack("<%dQ" % FIELDS, *fields))
            fh.write(struct.pack("<I", len(blob)) + blob)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    corpus_test = argv[1]
    keep = argv[3] if len(argv) >= 4 and argv[2] == "--write" else None
    records, named = build_corpus()
    strict_ok = sum(1 for r in records if r[2] == OK)
    spec_ok = sum(1 for r in records if r[3] == OK)
    print("loader corpus: %d named images, %d mutations; aar-check accepts %d, "
          "the spec rules accept %d; %d on which aar-check raises"
          % (named, len(records) - named, strict_ok, spec_ok, CRASHES[0]))
    with tempfile.TemporaryDirectory() as tmp:
        path = keep or os.path.join(tmp, "corpus.bin")
        write(records, path)
        return subprocess.call([corpus_test, path])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
