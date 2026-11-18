#!/usr/bin/env python3
"""Fail if the API reference lacks a heading for a public symbol of include/aar.

Parses declarations. A word search of the header is not the check.

  app.h, note.h, gl.h, ext_zz9000.h  ->  docs/aar-api.md, one heading per
                                         public macro, function, variable and
                                         typedef
  transport.h                        ->  docs/transport.md, which names every
                                         symbol in a heading or an inline code
                                         span (its tables are the reference)

Usage:
  check_aar_api.py             check the real headers against the real docs
  check_aar_api.py --self-test also prove that an undocumented symbol injected
                               into each header is reported
"""
# SPDX-License-Identifier: MIT

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "include" / "aar"
API_DOC = ROOT / "docs" / "aar-api.md"
TRANSPORT_DOC = ROOT / "docs" / "transport.md"

# header name -> (reference file, how the reference names a symbol)
API_HEADERS = ("app.h", "note.h", "gl.h", "ext_zz9000.h")
TRANSPORT_HEADERS = ("transport.h",)

KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "_Bool", "_Complex",
    "_Imaginary",
}

# Macros that are not part of the interface: include guards (NAME_H) and the
# helpers that build the note (they end in an underscore or are the paste
# pair). Everything else that starts with AAR_ must be documented.
INTERNAL_MACROS = {"AAR_NOTE_PASTE"}

FUNCTION_PREFIX = re.compile(r"^(aar_[a-z0-9_]+|glu?[A-Z][A-Za-z0-9_]*)$")


def strip_comments(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise SystemExit("unterminated block comment")
            out.append(re.sub(r"[^\n]", " ", text[i:end + 2]))
            i = end + 2
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                end = n
            out.append(" " * (end - i))
            i = end
            continue
        if text[i] in "\"'":
            quote = text[i]
            out.append(text[i])
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def continued_lines(text):
    merged = []
    buf = ""
    for line in text.splitlines():
        if line.endswith("\\"):
            buf += line[:-1] + " "
            continue
        merged.append(buf + line)
        buf = ""
    if buf:
        merged.append(buf)
    return merged


def public_macros(text):
    found = []
    for line in continued_lines(strip_comments(text)):
        stripped = line.lstrip()
        if not stripped.startswith("#"):
            continue
        body = stripped[1:].strip()
        if not body.startswith("define"):
            continue
        rest = body[len("define"):].strip()
        match = re.match(r"(AAR_[A-Za-z0-9_]+)\b(.?)", rest)
        if not match:
            continue
        name = match.group(1)
        if name.endswith("_") or name in INTERNAL_MACROS:
            continue
        # An include guard is a NAME_H macro defined with no value.
        if name.endswith("_H") and not rest[len(name):].strip():
            continue
        found.append(name)
    return found


def tokenize(code):
    tokens = []
    i = 0
    n = len(code)
    while i < n:
        ch = code[i]
        if ch.isspace():
            i += 1
            continue
        if ch == "#":
            while i < n and code[i] != "\n":
                if code[i] == "\\":
                    i += 2
                    continue
                i += 1
            continue
        if ch in "\"'":
            quote = ch
            i += 1
            while i < n and code[i] != quote:
                i += 2 if code[i] == "\\" else 1
            i += 1
            tokens.append(("string", ""))
            continue
        if ch.isalpha() or ch == "_":
            start = i
            i += 1
            while i < n and (code[i].isalnum() or code[i] == "_"):
                i += 1
            tokens.append(("ident", code[start:i]))
            continue
        if ch.isdigit():
            start = i
            i += 1
            while i < n and (code[i].isalnum() or code[i] in ".xX"):
                i += 1
            tokens.append(("num", code[start:i]))
            continue
        if ch in "(){}[];,*":
            tokens.append(("punct", ch))
            i += 1
            continue
        tokens.append(("other", ch))
        i += 1
    return tokens


def drop_linkage_braces(tokens):
    """extern "C" { } is a linkage wrapper, not a type body."""
    out = []
    i = 0
    while i < len(tokens):
        if (i + 2 < len(tokens) and tokens[i] == ("ident", "extern")
                and tokens[i + 1][0] == "string"
                and tokens[i + 2] == ("punct", "{")):
            i += 3
            depth = 1
            while i < len(tokens) and depth:
                if tokens[i] == ("punct", "{"):
                    depth += 1
                elif tokens[i] == ("punct", "}"):
                    depth -= 1
                    if depth == 0:
                        i += 1
                        break
                out.append(tokens[i])
                i += 1
            continue
        out.append(tokens[i])
        i += 1
    return out


def skip_block(tokens, i):
    """tokens[i] is '{'. Return the index after the matching '}'."""
    depth = 0
    while i < len(tokens):
        if tokens[i] == ("punct", "{"):
            depth += 1
        elif tokens[i] == ("punct", "}"):
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise SystemExit("unbalanced braces")


def declared_names(decl):
    """Names a top-level declaration introduces: functions, typedef names,
    extern variables."""
    names = []
    idents = [(k, v) for k, v in decl if k == "ident"]
    is_typedef = bool(idents) and idents[0][1] == "typedef"
    is_extern = bool(idents) and idents[0][1] == "extern"
    paren = 0
    func_ptr_name = None
    for i, (kind, value) in enumerate(decl):
        if kind == "punct" and value == "(":
            # name(  at depth 0 is a function
            if (paren == 0 and i > 0 and decl[i - 1][0] == "ident"
                    and not is_typedef):
                names.append(decl[i - 1][1])
            # typedef ret (*name)(args)
            if (paren == 0 and is_typedef and i + 2 < len(decl)
                    and decl[i + 1] == ("punct", "*")
                    and decl[i + 2][0] == "ident"):
                func_ptr_name = decl[i + 2][1]
            paren += 1
        elif kind == "punct" and value == ")":
            paren = max(0, paren - 1)
    if is_typedef:
        if func_ptr_name:
            names.append(func_ptr_name)
        elif idents:
            # typedef struct tag { } name;  the declared name is the last ident
            names.append(idents[-1][1])
    elif is_extern and "(" not in [v for k, v in decl if k == "punct"]:
        names.append(idents[-1][1])
    return names


def public_symbols(text):
    """Functions, typedef names and extern variables, in header order."""
    tokens = drop_linkage_braces(tokenize(strip_comments(text)))
    found = []
    decl = []
    i = 0
    while i < len(tokens):
        kind, value = tokens[i]
        if kind == "punct" and value == "{":
            # A body or a type body. Remember the tag for a typedef; skip the
            # contents either way. A function body ends the declaration.
            is_function = bool(decl) and decl[-1] == ("punct", ")")
            end = skip_block(tokens, i)
            if is_function:
                found.extend(declared_names(decl))
                decl = []
            else:
                decl.append(("punct", "{"))
                decl.append(("punct", "}"))
            i = end
            continue
        if kind == "punct" and value == ";":
            found.extend(declared_names(
                [t for t in decl if t not in (("punct", "{"), ("punct", "}"))]))
            decl = []
            i += 1
            continue
        decl.append(tokens[i])
        i += 1
    names = []
    for name in found:
        if name in KEYWORDS:
            continue
        if name.startswith("aar_") or FUNCTION_PREFIX.match(name):
            names.append(name)
    return names


def required_symbols(text):
    seen = set()
    ordered = []
    for name in public_macros(text) + public_symbols(text):
        if name not in seen:
            seen.add(name)
            ordered.append(name)
    return ordered


def headings(text):
    """First identifier of every heading, with Markdown backticks removed."""
    found = set()
    for line in text.splitlines():
        match = re.match(r"#{1,6}\s+`?([A-Za-z_][A-Za-z0-9_]*)", line)
        if match:
            found.add(match.group(1))
    return found


def code_spans(text):
    """Identifiers that appear in an inline code span or a heading."""
    found = set(headings(text))
    for span in re.findall(r"`([^`\n]+)`", text):
        found.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", span))
    return found


def missing_in(header_text, doc_text, kind):
    have = headings(doc_text) if kind == "headings" else code_spans(doc_text)
    return [name for name in required_symbols(header_text) if name not in have]


def read(path):
    return path.read_text(encoding="utf-8") if path.exists() else ""


def check(include, api_doc, transport_doc):
    """Return a list of (reference, header, [missing names])."""
    problems = []
    api_text = read(api_doc)
    transport_text = read(transport_doc)
    for name in API_HEADERS:
        absent = missing_in(read(include / name), api_text, "headings")
        if absent:
            problems.append((api_doc.name, name, absent))
    for name in TRANSPORT_HEADERS:
        absent = missing_in(read(include / name), transport_text, "spans")
        if absent:
            problems.append((transport_doc.name, name, absent))
    return problems


def prove_injection(include, api_doc, transport_doc):
    """An undocumented symbol added to each header must be reported."""
    api_text = read(api_doc)
    transport_text = read(transport_doc)
    cases = []
    for name in API_HEADERS:
        cases.append((name, "headings", api_text))
    for name in TRANSPORT_HEADERS:
        cases.append((name, "spans", transport_text))
    for name, kind, doc_text in cases:
        base = read(include / name)
        injected = {
            "function": "\nint aar_not_documented(void);\n",
            "macro": "\n#define AAR_NOT_DOCUMENTED 1\n",
            "typedef": "\ntypedef struct aar_not_documented_t {\n"
                       "    uint32_t x;\n} aar_not_documented_t;\n",
            "inline function": "\nstatic inline int aar_not_documented_fn(int v)\n"
                               "{\n    return v;\n}\n",
        }
        wanted = {
            "function": "aar_not_documented",
            "macro": "AAR_NOT_DOCUMENTED",
            "typedef": "aar_not_documented_t",
            "inline function": "aar_not_documented_fn",
        }
        for what, snippet in injected.items():
            absent = missing_in(base + snippet, doc_text, kind)
            if wanted[what] not in absent:
                raise SystemExit(
                    "injection of %s %s into %s was not reported"
                    % (what, wanted[what], name))
        # A heading of the right name must satisfy the check.
        satisfied = missing_in(base + injected["function"],
                               doc_text + "\n### aar_not_documented\n", kind)
        if "aar_not_documented" in satisfied:
            raise SystemExit("a documented symbol was still reported")
    print("injection rejected for every header: function, macro, typedef, "
          "inline function")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--include", type=pathlib.Path, default=INCLUDE)
    parser.add_argument("--api-doc", type=pathlib.Path, default=API_DOC)
    parser.add_argument("--transport-doc", type=pathlib.Path,
                        default=TRANSPORT_DOC)
    args = parser.parse_args()

    problems = check(args.include, args.api_doc, args.transport_doc)
    if problems:
        for doc, header, names in problems:
            sys.stderr.write("%s lacks an entry for symbols of %s:\n"
                             % (doc, header))
            for name in names:
                sys.stderr.write("  %s\n" % name)
        return 1

    total = 0
    for name in API_HEADERS + TRANSPORT_HEADERS:
        count = len(required_symbols(read(args.include / name)))
        total += count
        print("%-14s %3d public symbols documented" % (name, count))
    print("total %d" % total)
    if args.self_test:
        prove_injection(args.include, args.api_doc, args.transport_doc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
