#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail if docs/aar-client-api.md lacks a public symbol of the client headers.

Required in the document, as a heading or an inline code span:
  - every AAR_ macro that libraries/aar.h and libraries/aar_zz9000.h define,
  - every structure and typedef name they declare,
  - every call of client/fd/aar_lib.fd (and the AARSetTags macro),
  - every AAR_E_ error code of include/aar/app.h (the errors table).

  check_client_doc.py              check the real files
  check_client_doc.py --self-test  also prove that an undocumented symbol of each
                                   kind is reported
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADERS = [ROOT / "client" / "include" / "libraries" / "aar.h",
           ROOT / "client" / "include" / "libraries" / "aar_zz9000.h"]
FD = ROOT / "client" / "fd" / "aar_lib.fd"
APP_H = ROOT / "include" / "aar" / "app.h"
DOC = ROOT / "docs" / "aar-client-api.md"


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def macros(text):
    out = []
    for match in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(AAR_[A-Za-z0-9_]+)", strip_comments(text), re.M):
        name = match.group(1)
        if not name.endswith("_H"):
            out.append(name)
    return out


def types(text):
    code = strip_comments(text)
    out = re.findall(r"^\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;", code, re.M)
    out += re.findall(r"^typedef\s+[^;{(]*?\s\(?\*?([A-Za-z_][A-Za-z0-9_]*)\)?\s*\([^;]*\)\s*;", code, re.M)
    out += re.findall(r"^typedef\s+struct\s+\w+\s+([A-Za-z_][A-Za-z0-9_]*)\s*;", code, re.M)
    out += ["struct " + n for n in re.findall(r"^struct\s+(AAR[A-Za-z0-9_]*)\s*\{", code, re.M)]
    out += re.findall(r"^\}\s*(aar_[a-z0-9_]+)\s*;", code, re.M)
    return out


def calls(fd_text):
    names = re.findall(r"^(AAR[A-Za-z0-9_]*)\(", fd_text, re.M)
    return names + ["AARSetTags"]


def errors(app_text):
    return re.findall(r"^#define\s+(AAR_E_[A-Z0-9_]+)", app_text, re.M)


def required(headers_text, fd_text, app_text):
    names = []
    for text in headers_text:
        names += macros(text) + types(text)
    names += calls(fd_text) + errors(app_text)
    seen, ordered = set(), []
    for name in names:
        if name not in seen:
            seen.add(name)
            ordered.append(name)
    return ordered


def documented(doc_text):
    found = set()
    for line in doc_text.splitlines():
        match = re.match(r"#{1,6}\s+(.*)", line)
        if match:
            found.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", match.group(1).replace("`", " ")))
    for span in re.findall(r"`([^`\n]+)`", doc_text):
        found.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", span))
    return found


def missing(headers_text, fd_text, app_text, doc_text):
    have = documented(doc_text)
    out = []
    for name in required(headers_text, fd_text, app_text):
        word = name.split()[-1]
        if word not in have:
            out.append(name)
    return out


def self_test(headers_text, fd_text, app_text, doc_text):
    base = headers_text[0]
    cases = {
        "macro": (base + "\n#define AAR_NOT_DOCUMENTED 1\n", "AAR_NOT_DOCUMENTED"),
        "typedef": (base + "\ntypedef struct AARNotDocumentedT {\n    ULONG x;\n} AARNotDocumentedT;\n",
                    "AARNotDocumentedT"),
        "struct": (base + "\nstruct AARNotDocumentedS {\n    ULONG x;\n};\n", "struct AARNotDocumentedS"),
    }
    for kind, (text, want) in cases.items():
        got = missing([text] + headers_text[1:], fd_text, app_text, doc_text)
        if want not in got:
            raise SystemExit("injected %s %s was not reported" % (kind, want))
    got = missing(headers_text, fd_text + "\nAARNotACall(x)(a0)\n", app_text, doc_text)
    if "AARNotACall" not in got:
        raise SystemExit("injected call was not reported")
    got = missing(headers_text, fd_text, app_text + "\n#define AAR_E_NEW -99\n", doc_text)
    if "AAR_E_NEW" not in got:
        raise SystemExit("injected error code was not reported")
    got = missing(headers_text, fd_text, app_text, doc_text + "\n### AAR_NOT_DOCUMENTED\n")
    if got:
        raise SystemExit("the real files report %s with an extra heading" % got)
    print("injection rejected: macro, typedef, struct, call, error code")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    headers_text = [h.read_text(encoding="utf-8") for h in HEADERS]
    fd_text = FD.read_text(encoding="utf-8")
    app_text = APP_H.read_text(encoding="utf-8")
    doc_text = DOC.read_text(encoding="utf-8")
    absent = missing(headers_text, fd_text, app_text, doc_text)
    if absent:
        sys.stderr.write("%s lacks an entry for:\n" % DOC.name)
        for name in absent:
            sys.stderr.write("  %s\n" % name)
        return 1
    print("client API doc: %d symbols documented" %
          len(required(headers_text, fd_text, app_text)))
    if args.self_test:
        self_test(headers_text, fd_text, app_text, doc_text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
