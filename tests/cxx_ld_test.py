#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""libaar/<profile>/aar-cxx.ld is aar.ld plus the unwind tables.

The two scripts per profile are written out in full, because a linker script
cannot include a half of itself. This test keeps them from drifting: after
dropping the header comment, the block between the cxx-unwind markers and the
unwind lines of the /DISCARD/ list, the two must be identical, and only the
C++ script may name the unwind sections or their start symbols.

  tests/cxx_ld_test.py

Exit status: 0 the scripts agree, 1 they differ.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Per profile: a symbol only the C++ script defines.
PROFILES = {"a32": "__exidx_start", "a64": "__eh_frame_start"}

HEADER = re.compile(r"\A/\*.*?\*/\n", re.S)
UNWIND_BLOCK = re.compile(
    r"[ \t]*/\* cxx-unwind-begin \*/\n.*?[ \t]*/\* cxx-unwind-end \*/\n\n", re.S)
DISCARD = re.compile(r"(/DISCARD/ : \{\n)(.*?)(\n\t\})", re.S)


def normalize(text):
    text = HEADER.sub("", text, count=1)
    text = UNWIND_BLOCK.sub("", text)

    def trim(match):
        kept = []
        for line in match.group(2).split("\n"):
            stripped = line.strip()
            if stripped.startswith("/*") or ".ARM.ex" in stripped \
                    or ".eh_frame" in stripped:
                continue
            kept.append(line)
        return match.group(1) + "\n".join(kept) + match.group(3)

    return DISCARD.sub(trim, text)


def main():
    failed = False
    for profile, symbol in PROFILES.items():
        base = os.path.join(ROOT, "libaar", profile)
        with open(os.path.join(base, "aar.ld"), encoding="utf-8") as fh:
            plain = fh.read()
        with open(os.path.join(base, "aar-cxx.ld"), encoding="utf-8") as fh:
            cxx = fh.read()
        if symbol in plain:
            print("%s: aar.ld defines %s; only aar-cxx.ld may" % (profile, symbol))
            failed = True
        if symbol not in cxx or "cxx-unwind-begin" not in cxx:
            print("%s: aar-cxx.ld lacks the unwind block with %s" % (profile, symbol))
            failed = True
        if normalize(plain) != normalize(cxx):
            print("%s: aar.ld and aar-cxx.ld differ outside the unwind lines" % profile)
            failed = True
        else:
            print("%s: aar-cxx.ld matches aar.ld apart from the unwind lines" % profile)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
