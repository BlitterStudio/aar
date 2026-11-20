#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests of client/tools/fdgen.py and of the .fd file it reads.

The vector list below is the contract: 25 calls from -30 to -174 in this order,
with these registers. A change to the .fd file that moves or drops a call fails
here before it reaches a header.
"""

import importlib.util
import io
import pathlib
import re
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

CLIENT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fdgen", CLIENT / "tools" / "fdgen.py")
fdgen = importlib.util.module_from_spec(spec)
sys.modules["fdgen"] = fdgen
spec.loader.exec_module(fdgen)

FD_TEXT = (CLIENT / "fd" / "aar_lib.fd").read_text(encoding="utf-8")

# (name, registers) in vector order; the LVO of entry n is -(30 + 6 * n).
VECTORS = [
    ("AARQuery", "a0"),
    ("AAROpen", "a0/a1/a2"),
    ("AARClose", "a0"),
    ("AARProbeFile", "a0/a1"),
    ("AARProbeMemory", "a0/d0/a1"),
    ("AARSetTagList", "a0/a1"),
    ("AARLoadFile", "a0/a1"),
    ("AARLoadMemory", "a0/a1/d0"),
    ("AARStart", "a0/a1/d0"),
    ("AARStop", "a0"),
    ("AARStatus", "a0/a1"),
    ("AARSendMessage", "a0/a1"),
    ("AARGetMessage", "a0/a1"),
    ("AARMessageSignal", "a0"),
    ("AARSharedMap", "a0/d0/d1/d2/a1"),
    ("AARSharedUnmap", "a0/a1"),
    ("AARSharedPublish", "a0/a1/d0/d1"),
    ("AARSharedAcquire", "a0/a1/d0/d1"),
    ("AARServiceNext", "a0/a1"),
    ("AARServiceReply", "a0/a1"),
    ("AARPostEvent", "a0/a1"),
    ("AARAudioRead", "a0/a1/d0/a2"),
    ("AARSharedToken", "a0/a1/d0/a2"),
    ("AARExt", "d0/d1"),
    ("AARSetLogSink", "a0/a1/a2"),
]

GENERATED = ["clib/aar_protos.h", "pragmas/aar_pragmas.h", "inline/aar.h", "proto/aar.h"]


def quiet(fn, *args):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = fn(*args)
    return code, err.getvalue()


class VectorList(unittest.TestCase):
    def test_vectors_are_the_contract(self):
        funcs = fdgen.parse_fd(FD_TEXT)
        self.assertEqual(len(funcs), 25)
        for n, (func, (name, regs)) in enumerate(zip(funcs, VECTORS)):
            self.assertEqual(func.name, name)
            self.assertEqual(func.lvo, 30 + 6 * n, name)
            self.assertEqual("/".join(r for _, _, r in func.params), regs, name)
        self.assertEqual(funcs[0].lvo, 30)
        self.assertEqual(funcs[-1].lvo, 174)

    def test_results(self):
        funcs = {f.name: f for f in fdgen.parse_fd(FD_TEXT)}
        for name, func in funcs.items():
            want = {"AARMessageSignal": "ULONG", "AARExt": "APTR"}.get(name, "LONG")
            self.assertEqual(func.ret, want, name)

    def test_tag_call_has_varargs_companion(self):
        funcs = {f.name: f for f in fdgen.parse_fd(FD_TEXT)}
        self.assertTrue(funcs["AARSetTagList"].tag_list)
        self.assertEqual(funcs["AARSetTagList"].tags_name, "AARSetTags")
        self.assertEqual(sum(1 for f in funcs.values() if f.tag_list), 1)


class Output(unittest.TestCase):
    def setUp(self):
        self.funcs = fdgen.parse_fd(FD_TEXT)
        self.files = fdgen.generate(self.funcs)

    def test_four_files(self):
        self.assertEqual(sorted(self.files), sorted(GENERATED))

    def test_every_call_in_every_header(self):
        for name, _ in VECTORS:
            self.assertIn(" %s(" % name, self.files["clib/aar_protos.h"], name)
            self.assertIn("#pragma libcall AARBase %s " % name, self.files["pragmas/aar_pragmas.h"], name)
            self.assertIn("#define %s(" % name, self.files["inline/aar.h"], name)

    def test_pragma_codes(self):
        text = self.files["pragmas/aar_pragmas.h"]
        # Registers in reverse order, a 0, then the argument count.
        self.assertIn("AARQuery 01E 801\n", text)
        self.assertIn("AAROpen 024 A9803\n", text)
        self.assertIn("AARSharedMap 072 9210805\n", text)
        self.assertIn("AARExt 0A8 1002\n", text)
        self.assertIn("AARSetLogSink 0AE A9803\n", text)
        self.assertIn("#pragma tagcall AARBase AARSetTags 03C 9802\n", text)

    def test_inline_vectors(self):
        text = self.files["inline/aar.h"]
        for n, (name, _) in enumerate(VECTORS):
            self.assertIn("jsr -%d(a6)" % (30 + 6 * n), text, name)
        self.assertIn("jsr -174(a6)", text)

    def test_inline_clobbers_and_results(self):
        text = self.files["inline/aar.h"]
        # AARQuery uses only a0 (a1 and d1 are scratch it does not use).
        start = text.index("__AARQueryInline(")
        body = text[start:text.index("#define AARQuery(")]
        self.assertIn('"cc", "memory", "d1", "a1"', body)
        self.assertIn('"=r"(aar_d0), "+r"(aar_a0)', body)
        # AARSharedMap passes d0, d1 and d2, and a0 and a1; d2 is only read.
        start = text.index("__AARSharedMapInline(")
        body = text[start:text.index("#define AARSharedMap(")]
        self.assertIn('"+r"(aar_d0), "+r"(aar_a0), "+r"(aar_d1), "+r"(aar_a1)', body)
        self.assertIn('"r"(aar_a6), "r"(aar_d2)', body)
        self.assertIn('"cc", "memory"', body)
        # AARExt returns a pointer from d0, which also carries an argument.
        start = text.index("__AARExtInline(")
        body = text[start:text.index("#define AARExt(")]
        self.assertIn("return (APTR)aar_d0;", body)

    def test_proto_includes_in_order(self):
        text = self.files["proto/aar.h"]
        self.assertLess(text.index("clib/aar_protos.h"), text.index("pragmas/aar_pragmas.h"))
        self.assertLess(text.index("pragmas/aar_pragmas.h"), text.index("inline/aar.h"))
        self.assertIn("extern struct Library *AARBase;", text)

    def test_committed_headers_are_current(self):
        for name, text in self.files.items():
            have = (CLIENT / "include" / name).read_bytes().decode("utf-8")
            self.assertEqual(have, text, name)
            self.assertNotIn("\r", have, name)


SHELL = (CLIENT / "src" / "aar_shell.c").read_text(encoding="utf-8")


def f_name(call):
    """AARSetLogSink -> f_set_log_sink: the shell's entry for an fd call."""
    return "f_" + re.sub(r"(?<!^)(?=[A-Z])", "_", call[len("AAR"):]).lower()


class ShellVectorTable(unittest.TestCase):
    """The library's vector table (aar_lib_vectors in aar_shell.c) follows the
    .fd file: the four system slots, then every call in order, each entry a
    function that takes its arguments in the registers the .fd file names. The
    m68k build compiles this table but no host test runs it."""

    def table(self):
        body = re.search(r"aar_lib_vectors\[\]\s*=\s*\{(.*?)\};", SHELL, re.S).group(1)
        return re.findall(r"AAR_VEC\((\w+)\)|(\(APTR\)-1)", body)

    def test_order(self):
        entries = [a or b for a, b in self.table()]
        want = ["aar_lib_open", "aar_lib_close", "aar_lib_expunge", "aar_lib_null"]
        want += [f_name(name) for name, _ in VECTORS] + ["(APTR)-1"]
        self.assertEqual(entries, want)

    def test_registers(self):
        for name, regs in VECTORS:
            match = re.search(r"^static\s[^\n]*\b%s\((.*?)\)\n\{" % f_name(name), SHELL, re.S | re.M)
            self.assertIsNotNone(match, name)
            self.assertEqual("/".join(re.findall(r"REG\((\w+),", match.group(1))), regs, name)


class Check(unittest.TestCase):
    def copy_include(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp, True)
        for name in GENERATED:
            (tmp / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(CLIENT / "include" / name, tmp / name)
        return tmp

    def test_current_headers_pass(self):
        code, _ = quiet(fdgen.main, ["--check", "--include", str(self.copy_include())])
        self.assertEqual(code, 0)

    def test_edited_header_fails(self):
        tmp = self.copy_include()
        path = tmp / "pragmas" / "aar_pragmas.h"
        path.write_text(path.read_text().replace("AARQuery 01E 801", "AARQuery 01E 901"))
        code, err = quiet(fdgen.main, ["--check", "--include", str(tmp)])
        self.assertEqual(code, 1)
        self.assertIn("not up to date", err)

    def test_missing_header_fails(self):
        tmp = self.copy_include()
        (tmp / "proto" / "aar.h").unlink()
        code, _ = quiet(fdgen.main, ["--check", "--include", str(tmp)])
        self.assertEqual(code, 1)

    def test_changed_fd_fails_against_committed_headers(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp, True)
        fd = tmp / "changed.fd"
        fd.write_text(FD_TEXT.replace("AARSharedMap(handle,haddr,bytes,flags,map)(a0/d0/d1/d2/a1)",
                                      "AARSharedMap(handle,haddr,bytes,flags,map)(a0/d0/d1/d3/a1)"))
        code, _ = quiet(fdgen.main, ["--check", "--fd", str(fd)])
        self.assertEqual(code, 1)


class Malformed(unittest.TestCase):
    def bad(self, old, new, message):
        text = FD_TEXT.replace(old, new, 1)
        self.assertNotEqual(text, FD_TEXT, "the edit did not apply")
        with self.assertRaises(fdgen.FdError) as caught:
            fdgen.parse_fd(text)
        self.assertIn(message, str(caught.exception))

    def test_missing_prototype_line(self):
        self.bad("* C: LONG AARClose(AARHandle *handle)\n", "", "has no '* C:'")

    def test_parameter_names_must_match(self):
        self.bad("* C: LONG AARClose(AARHandle *handle)", "* C: LONG AARClose(AARHandle *h)", "differ")

    def test_function_name_must_match(self):
        self.bad("* C: LONG AARStop(AARHandle *handle)", "* C: LONG AARHalt(AARHandle *handle)", "names")

    def test_register_used_twice(self):
        self.bad("AARStatus(handle,status)(a0/a1)", "AARStatus(handle,status)(a0/a0)", "twice")

    def test_a6_is_not_an_argument(self):
        self.bad("AARStatus(handle,status)(a0/a1)", "AARStatus(handle,status)(a0/a6)", "cannot carry")

    def test_register_count_must_match(self):
        self.bad("AARStatus(handle,status)(a0/a1)", "AARStatus(handle,status)(a0)", "registers")

    def test_base_is_checked(self):
        self.bad("##base _AARBase", "##base _OtherBase", "##base")

    def test_bias_is_checked(self):
        self.bad("##bias 30", "##bias 36", "##bias")

    def test_end_is_required(self):
        self.bad("##end\n", "", "##end")

    def test_duplicate_name(self):
        text = FD_TEXT.replace("##end", "* C: LONG AARClose(AARHandle *handle)\nAARClose(handle)(a0)\n##end")
        with self.assertRaises(fdgen.FdError):
            fdgen.parse_fd(text)


if __name__ == "__main__":
    unittest.main(verbosity=1)
