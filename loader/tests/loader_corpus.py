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
AAR_E_PROFILE, the profile word of the note) and what an accepted image says:
the note, the number of segments with memory, the span, the entry offset, and the
number and the entries of the relocation tables.
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
CRASHES = [0]

OK, INVAL, PROFILE = 0, 1, 2
FIELDS = 13


def code_of(exc):
    """A refusal of aar-check as the module's code."""
    return PROFILE if str(exc).startswith("note profile ") else INVAL


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
    strict = OK
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
        strict = code_of(exc)
    except Exception:  # aar-check raises on some damaged input
        CRASHES[0] += 1
        strict = INVAL

    spec, overflow, fields = OK, 0, [0] * FIELDS
    try:
        img = tool.Image(blob)
        img.check_header()
        spec_tables(img)
        loads = tool.check_segments(img)
        tool.check_dynamic(img, loads)
        tool.check_entry(img, loads)
        spec_check_note(img)
    except tool.CheckError as exc:
        spec = code_of(exc)
    except Exception:
        CRASHES[0] += 1
        spec = INVAL
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
