#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests of tools/aar-symbolize and tools/aar-profile.

No pytest. The ELF files are built here, byte by byte, with the symbols the
tests need; the crash report is tests/data/crash-sample.txt, which the host test
of aar-run (client/tests/run_debug_test.c) compares with what aar-run writes,
and the profile file is built to the layout of client/run/README.md.

  python3 tests/aar_debug_tools_test.py [app.elf ...]

Each ELF file named on the command line (make check names the fault template of
every profile it builds) is also read as a real image: a crash report for a
function of it must name that function, by the symbol table and, when the
cross toolchain's addr2line is found, by addr2line.
"""
import importlib.machinery
import importlib.util
import io
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SYMBOLIZE = os.path.join(ROOT, "tools", "aar-symbolize")
PROFILE = os.path.join(ROOT, "tools", "aar-profile")
SAMPLE = os.path.join(ROOT, "tests", "data", "crash-sample.txt")
REAL_ELFS = []

sys.path.insert(0, os.path.join(ROOT, "tools"))
import aar_elf  # noqa: E402


def load_tool(path, name):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


symbolize = load_tool(SYMBOLIZE, "aar_symbolize")
profile = load_tool(PROFILE, "aar_profile")

STT_NOTYPE, STT_OBJECT, STT_FUNC = 0, 1, 2
TEXT = 0x1000
DATA = 0x3000
# name, address, size, type: main, helper and crash_here are the functions the
# crash sample refers to (load base 0x10000).
SYMBOLS = [
    ("main", 0x1000, 0x100, STT_FUNC),
    ("helper", 0x1100, 0x80, STT_FUNC),
    ("crash_here", 0x1200, 0x40, STT_FUNC),
    ("asm_label", 0x1300, 0, STT_NOTYPE),
    ("thumb_fn", 0x1401, 0x20, STT_FUNC),  # a Thumb function has its low bit set
    ("$a", 0x1380, 0, STT_NOTYPE),          # an ARM mapping symbol: not a function
    ("table", 0x3000, 0x40, STT_OBJECT, 2),
    # A label in a data section, such as the end of an init array, is not code and
    # must not claim the addresses above it.
    ("__fini_array_end", 0x3040, 0, STT_NOTYPE, 2),
    # A label without a size at the end of the code ends with its section.
    ("end_label", 0x14f0, 0, STT_NOTYPE),
]


def build_elf(bits=32, symbols=SYMBOLS, strip=False):
    """An ELF with a code segment at TEXT, a data segment at DATA, and (unless
    strip) .symtab and .strtab."""
    a64 = bits == 64
    ehsz, phsz, shsz = (64, 56, 64) if a64 else (52, 32, 40)
    machine = 183 if a64 else 40
    strtab = b"\0"
    syms = [b"\0" * (24 if a64 else 16)]
    for name, addr, size, kind, *shndx in symbols:
        shndx = shndx[0] if shndx else 1  # section 1 is the code, 2 the data
        off = len(strtab)
        strtab += name.encode() + b"\0"
        info = kind  # a local symbol: binding 0
        if a64:
            syms.append(struct.pack("<IBBHQQ", off, info, 0, shndx, addr, size))
        else:
            syms.append(struct.pack("<IIIBBH", off, addr, size, info, 0, shndx))
    symtab = b"".join(syms)
    code_off = ehsz + 2 * phsz
    code = b"\0" * 0x500
    sym_off = code_off + len(code)
    str_off = sym_off + len(symtab)
    sh_off = sym_off if strip else str_off + len(strtab)
    if a64:
        phs = struct.pack("<IIQQQQQQ", 1, 5, code_off, TEXT, TEXT, len(code), len(code), 4096)
        phs += struct.pack("<IIQQQQQQ", 1, 6, code_off, DATA, DATA, 0x40, 0x100, 4096)
    else:
        phs = struct.pack("<IIIIIIII", 1, code_off, TEXT, TEXT, len(code), len(code), 5, 4096)
        phs += struct.pack("<IIIIIIII", 1, code_off, DATA, DATA, 0x40, 0x100, 6, 4096)
    nsec = 1 if strip else 5
    if a64:
        shs = struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        shs += struct.pack("<IIQQQQIIQQ", 0, 1, 6, TEXT, code_off, len(code), 0, 0, 4, 0)  # .text AX
        shs += struct.pack("<IIQQQQIIQQ", 0, 1, 3, DATA, code_off, 0x100, 0, 0, 4, 0)      # .data WA
        shs += struct.pack("<IIQQQQIIQQ", 0, 2, 0, 0, sym_off, len(symtab), 4, 1, 8, 24)
        shs += struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0)
        head = (b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8 +
                struct.pack("<HHIQQQIHHHHHH", 3, machine, 1, 0, ehsz, sh_off, 0, ehsz, phsz, 2, shsz, nsec, 0))
    else:
        shs = struct.pack("<IIIIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        shs += struct.pack("<IIIIIIIIII", 0, 1, 6, TEXT, code_off, len(code), 0, 0, 4, 0)  # .text AX
        shs += struct.pack("<IIIIIIIIII", 0, 1, 3, DATA, code_off, 0x100, 0, 0, 4, 0)      # .data WA
        shs += struct.pack("<IIIIIIIIII", 0, 2, 0, 0, sym_off, len(symtab), 4, 1, 4, 16)
        shs += struct.pack("<IIIIIIIIII", 0, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0)
        head = (b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8 +
                struct.pack("<HHIIIIIHHHHHH", 3, machine, 1, 0, ehsz, sh_off, 0, ehsz, phsz, 2, shsz, nsec, 0))
    assert len(head) == ehsz
    blob = head + phs + code
    if strip:
        return blob + shs[:shsz]
    return blob + symtab + strtab + shs


def crash_text(a64=False, stack_words=(0, 0x11134, 0xbffff040, 0x110c4, 0x13100, 0x1122c), pc=0x11210,
               lr=0x11150, elf="Work:Apps/game.elf", end="3 user fault", base=0x10000, stack=True):
    lines = ["aar-crash 1", "end_reason: " + end, "fault_class: 1 data abort", "syndrome: 0x00000805",
             "pstate: 0x60000010", "pc: 0x%016x" % pc, "lr: 0x%016x" % lr, "sp: 0x00000000bffff000",
             "fault_addr: 0x0000000000000004", "load_base: 0x%016x" % base,
             "profile: %s" % ("2 A64" if a64 else "1 A32"), "exit_code: -1", "elf: " + elf]
    data = b"".join(w.to_bytes(8 if a64 else 4, "little") for w in stack_words) if stack else b""
    lines += ["stack_sp: 0x00000000bffff000", "stack_bytes: %d" % len(data), "stack:"]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        words = ["%0*x" % (2 * len(chunk[j:j + 4]), int.from_bytes(chunk[j:j + 4], "little"))
                 for j in range(0, len(chunk), 4)]
        lines.append("0x%016x: %s" % (0xbffff000 + i, " ".join(words)))
    return "\n".join(lines) + "\n"


def profile_file(samples, hz=100, lost=0, base=0x10000, elf="Work:Apps/game.elf", extra=b""):
    name = elf.encode()
    padded = name + b"\0" * (-len(name) % 8)
    head = (b"AARP" + struct.pack(">IIIIIII", 1, hz, lost, base >> 32, base & 0xFFFFFFFF, 0, len(name)) + padded)
    return head + b"".join(struct.pack(">Q", s) for s in samples) + extra


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def path(self, name, data, mode="wb"):
        p = os.path.join(self.tmp.name, name)
        with open(p, mode) as f:
            f.write(data)
        return p

    def run_tool(self, module, args):
        out, err = io.StringIO(), io.StringIO()
        old = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = out, err
        try:
            code = module.main(args)
        finally:
            sys.stdout, sys.stderr = old
        return code, out.getvalue(), err.getvalue()


class ElfSymbols(Files):
    def check(self, bits):
        elf = aar_elf.Elf(self.path("a.elf", build_elf(bits)))
        self.assertEqual(elf.bits, bits)
        self.assertEqual(elf.lookup(0x1000), ("main", 0))
        self.assertEqual(elf.lookup(0x10ff), ("main", 0xff))
        self.assertEqual(elf.lookup(0x1150), ("helper", 0x50))
        self.assertEqual(elf.lookup(0x1210), ("crash_here", 0x10))
        self.assertIsNone(elf.lookup(0x1180))       # padding after helper: no symbol holds it
        self.assertEqual(elf.lookup(0x13f0), ("asm_label", 0xf0))  # no size: up to the next symbol
        self.assertEqual(elf.lookup(0x1410)[0], "thumb_fn")
        self.assertIsNone(elf.lookup(0xfff))        # before every symbol
        names = [s.name for s in elf.symbols]
        self.assertNotIn("$a", names)
        self.assertNotIn("table", names)            # data is not code
        self.assertNotIn("__fini_array_end", names)  # an untyped label in a data section is not a code symbol
        self.assertEqual(elf.lookup(0x14f8), ("end_label", 8))
        self.assertIsNone(elf.lookup(0x1500))       # a label ends with its section
        self.assertIsNone(elf.lookup(0x3050))       # no symbol, even above the data label
        self.assertIsNone(elf.lookup(0x1450))       # past the sized symbol before it
        self.assertTrue(elf.in_image(0x3050) and not elf.in_image(0x3200) and not elf.in_image(0x1500))
        self.assertTrue(elf.in_code(0x1000) and elf.in_code(0x14ff))
        self.assertFalse(elf.in_code(0x1500) or elf.in_code(0x3010))
        self.assertTrue(elf.in_image(0x3010) and not elf.in_image(0x3200))

    def test_elf32(self):
        self.check(32)

    def test_elf64(self):
        self.check(64)

    def test_thumb_symbol_has_no_low_bit(self):
        elf = aar_elf.Elf(self.path("a.elf", build_elf(32)))
        self.assertIn(0x1400, [s.addr for s in elf.symbols])

    def test_stripped_image_has_no_symbols(self):
        elf = aar_elf.Elf(self.path("a.elf", build_elf(32, strip=True)))
        self.assertEqual(elf.symbols, [])
        self.assertIsNone(elf.lookup(0x1000))
        self.assertTrue(elf.in_code(0x1000))

    def test_refusals(self):
        with self.assertRaises(aar_elf.ElfError):
            aar_elf.Elf(self.path("x", b"not an elf at all, but long enough to read " * 3))
        with self.assertRaises(aar_elf.ElfError):
            aar_elf.Elf(self.path("short", b"\x7fELF"))
        data = bytearray(build_elf(32))
        data[5] = 2  # big-endian
        with self.assertRaises(aar_elf.ElfError):
            aar_elf.Elf(self.path("be", bytes(data)))
        with self.assertRaises(aar_elf.ElfError):
            aar_elf.Elf(self.path("cut", build_elf(32)[:150]))


class Symbolize(Files):
    def elf(self, bits=32):
        return self.path("game.elf", build_elf(bits))

    def sym(self, text, *args, bits=32):
        log = self.path("crash.txt", text, "w")
        return self.run_tool(symbolize, [log, "--elf", self.elf(bits), "--no-addr2line"] + list(args))

    def test_the_sample_report(self):
        with open(SAMPLE, encoding="utf-8") as f:
            code, out, err = self.sym(f.read())
        self.assertEqual((code, err), (0, ""))
        self.assertIn("end reason   3 user fault", out)
        self.assertIn("load base    0x00010000", out)
        self.assertRegex(out, r"pc          0x00011210  crash_here\+0x10")
        self.assertRegex(out, r"lr          0x00011150  helper\+0x50")
        self.assertIn("fault addr   0x00000004", out)
        # Three of the six words are code; they come in stack order, each with its
        # offset in the snapshot.
        self.assertIn("3 of 6 words", out)
        self.assertRegex(out, r"#0  \[sp\+0x004\] 0x00011134  helper\+0x34")
        self.assertRegex(out, r"#1  \[sp\+0x00c\] 0x000110c4  main\+0xc4")
        self.assertRegex(out, r"#2  \[sp\+0x014\] 0x0001122c  crash_here\+0x2c")
        self.assertNotIn("0x00013100", out.split("Backtrace")[1])  # a data address
        self.assertNotIn("0xbffff040", out.split("Backtrace")[1])  # a stack address

    def test_the_elf_comes_from_the_report(self):
        elf = self.elf()
        text = crash_text(elf=elf)
        code, out, _ = self.run_tool(symbolize, [self.path("c.txt", text, "w"), "--no-addr2line"])
        self.assertEqual(code, 0)
        self.assertIn("crash_here+0x10", out)
        code, out, _ = self.run_tool(symbolize, [self.path("c2.txt", text, "w"), elf, "--no-addr2line"])
        self.assertEqual(code, 0)

    def test_the_thumb_bit_is_ignored_in_a_stack_word(self):
        code, out, _ = self.sym(crash_text(stack_words=(0x11135,)))
        self.assertEqual(code, 0)
        self.assertRegex(out, r"0x00011135  helper\+0x34")  # the name is of the address without the bit

    def test_a_word_below_the_load_base_or_past_the_code(self):
        code, out, _ = self.sym(crash_text(stack_words=(0x1100, 0x11500, 0x11000, 0x114ff)))
        self.assertEqual(code, 0)
        self.assertIn("2 of 4 words", out)  # 0x11000 (main) and 0x114ff (thumb_fn)
        self.assertNotIn("0x00001100", out.split("Backtrace")[1])

    def test_a64_words_are_eight_bytes(self):
        text = crash_text(a64=True, stack_words=(0x11134, 0xbffff040, 0x1_0001_0000 + 0x1110))
        code, out, err = self.sym(text, bits=64)
        self.assertEqual((code, err), (0, ""))
        self.assertIn("pc          0x0000000000011210  crash_here+0x10", out)
        self.assertIn("1 of 3 words", out)
        self.assertRegex(out, r"#0  \[sp\+0x000\] 0x0000000000011134  helper\+0x34")

    def test_pc_outside_the_image_and_without_a_symbol(self):
        code, out, _ = self.sym(crash_text(pc=0x80000000, lr=0x11180))
        self.assertEqual(code, 0)
        self.assertIn("(outside the image)", out)
        self.assertIn("(no symbol)", out)

    def test_a_pc_below_the_load_base(self):
        code, out, _ = self.sym(crash_text(pc=0x100))
        self.assertEqual(code, 0)
        self.assertIn("(below the load base)", out)

    def test_a_stall_has_no_fault_address(self):
        text = crash_text(end="4 stall watchdog").replace("fault_class: 1 data abort", "fault_class: 0 none")
        text = text.replace("fault_addr: 0x0000000000000004", "fault_addr: 0x0000000000000000")
        code, out, _ = self.sym(text)
        self.assertEqual(code, 0)
        self.assertIn("end reason   4 stall watchdog", out)
        self.assertNotIn("fault addr", out)

    def test_no_stack(self):
        code, out, _ = self.sym(crash_text(stack=False))
        self.assertEqual(code, 0)
        self.assertIn("no stack snapshot", out)

    def test_a_partial_last_word(self):
        text = crash_text(stack_words=(0x11134,)) + "0x00000000bffff010: 2c1201\n"
        code, out, _ = self.sym(text)
        self.assertEqual(code, 0)
        self.assertIn("1 of 1 words", out)  # three bytes are not a word

    def test_bad_input(self):
        for text, why in [("hello\n", "not a crash report"),
                          ("aar-crash 1\nend_reason: 3 x\n", "no usable"),
                          (crash_text().replace("0x00000000bffff000: ", "0x00000000bffff000: zz ", 1), "bad stack"),
                          (crash_text() + "zz\n", "bad stack line")]:
            code, out, err = self.sym(text)
            self.assertEqual(code, 1, why)
            self.assertIn(why, err)
        code, _, err = self.run_tool(symbolize, [self.path("missing.txt", "x", "w") + "x", "--elf", self.elf()])
        self.assertEqual(code, 1)
        code, _, err = self.run_tool(symbolize, [self.path("c.txt", crash_text(), "w"), "--elf", self.path("n", b"nope")])
        self.assertEqual(code, 1)
        self.assertIn("not an ELF", err)

    def test_the_report_must_name_an_elf_or_be_given_one(self):
        code, _, err = self.run_tool(symbolize, [self.path("c.txt", crash_text(elf=""), "w"), "--no-addr2line"])
        self.assertEqual(code, 1)
        self.assertIn("names no ELF", err)

    @unittest.skipIf(os.name == "nt", "the fake addr2line is a script")
    def test_addr2line_names_the_function_and_the_line(self):
        fake = self.path("fake-addr2line", "#!%s\n%s" % (sys.executable, """import sys
args = sys.argv[1:]
assert args[:4] == ["-e", args[1], "-f", "-C"], args
for a in args[4:]:
    print("helper" if int(a, 16) == 0x1150 else "??")
    print("game.c:42" if int(a, 16) == 0x1150 else "??:0")
"""), "w")
        os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)
        log = self.path("c.txt", crash_text(), "w")
        code, out, _ = self.run_tool(symbolize, [log, "--elf", self.elf(), "--cross", os.path.join(self.tmp.name, "fake-")])
        self.assertEqual(code, 0)
        self.assertIn("lr          0x00011150  helper+0x50 (game.c:42)", out)
        # An address addr2line cannot name falls back to the symbol table.
        self.assertIn("crash_here+0x10", out)
        self.assertNotIn("(game.c:42)\n  #", out.split("pc ")[0])

    @unittest.skipIf(os.name == "nt", "the fake addr2line is a script")
    def test_a_failing_addr2line_falls_back(self):
        fake = self.path("bad-addr2line", "#!/bin/sh\nexit 1\n", "w")
        os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)
        log = self.path("c.txt", crash_text(), "w")
        code, out, _ = self.run_tool(symbolize, [log, "--elf", self.elf(), "--cross", os.path.join(self.tmp.name, "bad-")])
        self.assertEqual(code, 0)
        self.assertIn("helper+0x50", out)

    def test_command_line_of_the_script(self):
        log = self.path("c.txt", crash_text(), "w")
        run = subprocess.run([sys.executable, SYMBOLIZE, log, "--elf", self.elf(), "--no-addr2line"],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("crash_here+0x10", run.stdout)
        run = subprocess.run([sys.executable, SYMBOLIZE], capture_output=True, text=True)
        self.assertEqual(run.returncode, 2)


class Profile(Files):
    def prof(self, samples, **kw):
        elf = kw.pop("elf_path", None) or self.path("game.elf", build_elf(32))
        data = self.path("p.bin", profile_file(samples, **kw))
        return self.run_tool(profile, [data, "--elf", elf])

    def test_flat_profile_sorted_by_samples(self):
        base = 0x10000
        samples = ([base + 0x1010] * 5 + [base + 0x1150] * 3 + [base + 0x1220] * 1 +
                   [base + 0x1180] + [0x80000000] + [base + 0x10])
        code, out, err = self.prof(samples, lost=7)
        self.assertEqual((code, err), (0, ""))
        self.assertIn("rate         100 Hz", out)
        self.assertIn("samples      12 (0.12 s)", out)
        self.assertIn("lost         7", out)
        self.assertIn("load base    0x10000", out)
        rows = [line.split(None, 2) for line in out.split("function\n")[1].splitlines()]
        counts = {r[2]: int(r[0]) for r in rows}
        # 0x10 lies before the image, as 0x80000000 lies after it: both are outside.
        self.assertEqual(counts, {"main": 5, "helper": 3, "crash_here": 1, "(no symbol)": 1,
                                  "(outside the image)": 2})
        self.assertEqual([r[2] for r in rows[:3]], ["main", "helper", "(outside the image)"])
        self.assertIn("41.67%", out)
        self.assertIn("25.00%", out)

    def test_two_static_functions_with_one_name_are_two_rows(self):
        base = 0x10000
        symbols = [("tick", 0x1000, 0x80, STT_FUNC), ("tick", 0x1100, 0x80, STT_FUNC),
                   ("main", 0x1200, 0x40, STT_FUNC)]
        elf = self.path("g.elf", build_elf(32, symbols=symbols))
        data = self.path("p.bin", profile_file([base + 0x1010] * 5 + [base + 0x1110] * 3 + [base + 0x1210]))
        code, out, _ = self.run_tool(profile, [data, "--elf", elf])
        self.assertEqual(code, 0)
        rows = [line.split(None, 2) for line in out.split("function\n")[1].splitlines()]
        self.assertEqual([(int(r[0]), r[2]) for r in rows],
                         [(5, "tick@0x1000"), (3, "tick@0x1100"), (1, "main")])

    def test_top(self):
        base = 0x10000
        data = self.path("p.bin", profile_file([base + 0x1010] * 3 + [base + 0x1150] * 2 + [base + 0x1210]))
        code, out, _ = self.run_tool(profile, [data, "--elf", self.path("g.elf", build_elf(32)), "--top", "2"])
        self.assertEqual(code, 0)
        self.assertIn("main", out)
        self.assertIn("helper", out)
        self.assertNotIn("crash_here", out)

    def test_an_address_past_the_last_symbol_is_not_that_symbol(self):
        base = 0x10000
        # Above a label at the end of a data section, in the data, past the code, past the image.
        code, out, _ = self.prof([base + 0x3050, base + 0x1500, base + 0x3200, base + 0x14f8])
        self.assertEqual(code, 0)
        rows = [line.split(None, 2) for line in out.split("function\n")[1].splitlines()]
        self.assertEqual({r[2]: int(r[0]) for r in rows},
                         {"(no symbol)": 1, "(outside the image)": 2, "end_label": 1})
        self.assertNotIn("__fini_array_end", out)

    def test_the_elf_comes_from_the_header(self):
        elf = self.path("game.elf", build_elf(32))
        data = self.path("p.bin", profile_file([0x11010], elf=elf))
        code, out, _ = self.run_tool(profile, [data])
        self.assertEqual(code, 0)
        self.assertIn("main", out)

    def test_a64_load_base_with_a_high_word(self):
        base = 0x1_0000_0000
        data = self.path("p.bin", profile_file([base + 0x1010, base + 0x1010], base=base))
        code, out, _ = self.run_tool(profile, [data, "--elf", self.path("g.elf", build_elf(64))])
        self.assertEqual(code, 0)
        self.assertIn("load base    0x100000000", out)
        self.assertRegex(out, r"2 +100\.00%  main")

    def test_a_cut_sample_is_ignored(self):
        data = self.path("p.bin", profile_file([0x11010, 0x11010], extra=b"\x00\x00\x00"))
        code, out, _ = self.run_tool(profile, [data, "--elf", self.path("g.elf", build_elf(32))])
        self.assertEqual(code, 0)
        self.assertIn("samples      2 ", out)
        self.assertIn("3 bytes of a cut sample", out)

    def test_bad_files(self):
        elf = self.path("g.elf", build_elf(32))
        for blob, why in [(b"", "not a profile"), (b"AARX" + b"\0" * 40, "not a profile"),
                          (b"AARP" + struct.pack(">IIIIIII", 9, 100, 0, 0, 0, 0, 0), "version 9"),
                          (b"AARP" + struct.pack(">IIIIIII", 1, 100, 0, 0, 0, 0, 64), "cut short"),
                          (profile_file([]), "no samples")]:
            code, _, err = self.run_tool(profile, [self.path("p.bin", blob), "--elf", elf])
            self.assertEqual(code, 1, why)
            self.assertIn(why, err)
        code, _, err = self.run_tool(profile, [self.path("p.bin", profile_file([1])), "--elf", self.path("n", b"nope")])
        self.assertEqual(code, 1)
        code, _, _ = self.run_tool(profile, [self.path("p.bin", profile_file([1])), "--elf", os.path.join(self.tmp.name, "no")])
        self.assertEqual(code, 1)

    def test_command_line_of_the_script(self):
        elf = self.path("g.elf", build_elf(32))
        data = self.path("p.bin", profile_file([0x11010]))
        run = subprocess.run([sys.executable, PROFILE, data, "--elf", elf], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("main", run.stdout)
        run = subprocess.run([sys.executable, PROFILE], capture_output=True, text=True)
        self.assertEqual(run.returncode, 2)


class RealImages(unittest.TestCase):
    """The images make check builds: the symbol table names aar_main, and so
    does addr2line when the cross toolchain is installed."""

    def test_images(self):
        if not REAL_ELFS:
            self.skipTest("no ELF file named on the command line")
        for path in REAL_ELFS:
            with self.subTest(elf=path):
                elf = aar_elf.Elf(path)
                sym = next((s for s in elf.symbols if s.name == "aar_main"), None)
                self.assertIsNotNone(sym, "aar_main is not in the symbol table")
                base = 0x80000000
                text = crash_text(a64=elf.is_a64, pc=base + sym.addr + 4, lr=base + sym.addr + 8, base=base,
                                  stack_words=(base + sym.addr + 4, 0), elf=path)
                with tempfile.TemporaryDirectory() as tmp:
                    log = os.path.join(tmp, "crash.txt")
                    with open(log, "w") as f:
                        f.write(text)
                    for extra in (["--no-addr2line"], []):
                        run = subprocess.run([sys.executable, SYMBOLIZE, log] + extra,
                                             capture_output=True, text=True)
                        self.assertEqual(run.returncode, 0, run.stderr)
                        self.assertIn("aar_main+0x4", run.stdout, " ".join(extra))
                        self.assertIn("#0  [sp+0x000]", run.stdout)
                    samples = os.path.join(tmp, "p.bin")
                    with open(samples, "wb") as f:
                        f.write(profile_file([base + sym.addr + 4] * 3, base=base, elf=path))
                    run = subprocess.run([sys.executable, PROFILE, samples], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertIn("aar_main", run.stdout)
                    self.assertIn("100.00%", run.stdout)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    REAL_ELFS.extend(a for a in args if os.path.exists(a))
    unittest.main(argv=[sys.argv[0]] + [a for a in sys.argv[1:] if a.startswith("-")])
