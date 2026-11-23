#!/usr/bin/env python3
# Prepares the C-FILE fixture tree of conformance/payloads/conf_file.h in a
# directory, as the runner (runner/r_file.c) does under its ROOT, so that the
# payload can be tried on the host runner by hand:
#
#   conformance/tests/conf_file_fixtures.py DIR
#   build-host/conf_file --headless --root DIR -- conf:peer=1
#
# With conf:peer=1 and no runner, the tests that ask the runner to check a file
# (C-FILE.3, C-FILE.22) wait 10 s and fail; the others run as on a platform.
#
# SPDX-License-Identifier: MIT
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "payloads", "conf_file.h")


def macro(src, name):
    m = re.search(r"#define\s+%s\s+((?:\"[^\"]*\"\s*)+)" % name, src)
    if not m:
        raise SystemExit("conf_file.h: no macro %s" % name)
    return "".join(re.findall(r"\"([^\"]*)\"", m.group(1))).encode().decode("unicode_escape")


def pat(i):
    return ((i * 167) ^ (i >> 8) ^ (i >> 16) ^ 0x3C) & 0xFF


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: conf_file_fixtures.py DIR")
    src = open(HEADER, encoding="utf-8").read()
    root = os.path.join(sys.argv[1], "conf-fixtures")
    os.makedirs(root, exist_ok=True)
    for name in os.listdir(root):
        if name.endswith(".tmp"):
            os.remove(os.path.join(root, name))
    with open(os.path.join(root, "text.txt"), "w", newline="") as fh:
        fh.write(macro(src, "CONF_FILE_TEXT"))
    with open(os.path.join(root, "pattern.bin"), "wb") as fh:
        fh.write(bytes(pat(i) for i in range(1024 * 1024)))
    dated = os.path.join(root, "dated.txt")
    with open(dated, "w", newline="") as fh:
        fh.write(macro(src, "CONF_FILE_DATED_TEXT"))
    os.utime(dated, (1000000000, 1000000000))
    d5 = os.path.join(root, "dir5")
    os.makedirs(d5, exist_ok=True)
    for n in "abcde":
        with open(os.path.join(d5, n + ".txt"), "w", newline="") as fh:
            fh.write(n + ".txt\n")
    os.makedirs(os.path.join(root, "leakdir"), exist_ok=True)
    chain = root
    for n in range(1, 8):
        chain = os.path.join(chain, macro(src, "CONF_FILE_LD%d" % n))
    os.makedirs(chain, exist_ok=True)
    for leaf in ("CONF_FILE_LEAF24", "CONF_FILE_LEAF25"):
        with open(os.path.join(chain, macro(src, leaf)), "w", newline="") as fh:
            fh.write(macro(src, "CONF_FILE_LONG_TEXT"))
    print("fixtures in %s" % root)


if __name__ == "__main__":
    main()
