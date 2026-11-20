#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write an accepted AAR image for the shell test.

  make_images.py OUTFILE

The image is the minimal valid A32 image of tests/aar_check_test.py, the one
tools/aar-check accepts. client/tests/shell_test.c probes it through the fake
dos.library and loads it through the reference platform.
"""

import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import aar_check_test as corpus  # noqa: E402


def main(argv):
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    with open(argv[1], "wb") as fh:
        fh.write(corpus.build("a32"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
