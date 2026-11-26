# SPDX-License-Identifier: MIT
"""The part of an ELF file that tools/aar-symbolize and tools/aar-profile read:
the loadable segments and the symbol table of an app image (ELF32 or ELF64,
little-endian, as both profiles of the standard produce).

It is a module next to the tools, not a command. Nothing here judges an image
(tools/aar-check does); it reads what it needs and raises ElfError for a file it
cannot make sense of.
"""
import struct

PT_LOAD = 1
PF_X = 1
SHT_SYMTAB = 2
SHT_DYNSYM = 11
STT_NOTYPE = 0
STT_FUNC = 2
STT_GNU_IFUNC = 10
SHF_EXECINSTR = 4
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
EM_ARM = 40
EM_AARCH64 = 183


class ElfError(Exception):
    pass


class Segment:
    def __init__(self, vaddr, memsz, filesz, flags):
        self.vaddr = vaddr
        self.memsz = memsz
        self.filesz = filesz
        self.flags = flags

    @property
    def executable(self):
        return bool(self.flags & PF_X)

    def contains(self, addr):
        return self.vaddr <= addr < self.vaddr + self.memsz


class Symbol:
    def __init__(self, name, addr, size, end):
        self.name = name
        self.addr = addr
        self.size = size
        self.end = end  # the first address past it: addr + size, or the end of its section for a label


class Elf:
    """An app image: bits (32 or 64), machine, loadable segments and the code
    symbols sorted by address."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            data = f.read()
        self.data = data
        if len(data) < 52 or data[:4] != b"\x7fELF":
            raise ElfError("%s: not an ELF file" % path)
        if data[5] != 1:
            raise ElfError("%s: not little-endian" % path)
        if data[4] == 1:
            self.bits = 32
        elif data[4] == 2:
            self.bits = 64
        else:
            raise ElfError("%s: bad ELF class" % path)
        try:
            self._parse()
        except struct.error as exc:
            raise ElfError("%s: truncated (%s)" % (path, exc))

    def _parse(self):
        data = self.data
        if self.bits == 32:
            (self.machine,) = struct.unpack_from("<H", data, 18)
            phoff, shoff = struct.unpack_from("<II", data, 28)
            phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", data, 42)
        else:
            if len(data) < 64:
                raise ElfError("%s: truncated" % self.path)
            (self.machine,) = struct.unpack_from("<H", data, 18)
            phoff, shoff = struct.unpack_from("<QQ", data, 32)
            phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", data, 54)
        self.segments = []
        for i in range(phnum):
            at = phoff + i * phentsize
            if self.bits == 32:
                ptype, _off, vaddr, _paddr, filesz, memsz, flags, _align = struct.unpack_from(
                    "<IIIIIIII", data, at)
            else:
                ptype, flags, _off, vaddr, _paddr, filesz, memsz, _align = struct.unpack_from(
                    "<IIQQQQQQ", data, at)
            if ptype == PT_LOAD:
                self.segments.append(Segment(vaddr, memsz, filesz, flags))
        self.symbols = self._symbols(shoff, shentsize, shnum)

    def _sections(self, shoff, shentsize, shnum):
        out = []
        for i in range(shnum):
            at = shoff + i * shentsize
            if self.bits == 32:
                name, stype, flags, addr, off, size, link, _info, _al, entsize = struct.unpack_from(
                    "<IIIIIIIIII", self.data, at)
            else:
                name, stype, flags, addr, off, size, link, _info, _al, entsize = struct.unpack_from(
                    "<IIQQQQIIQQ", self.data, at)
            out.append((stype, flags, addr, off, size, link, entsize))
        return out

    def _symbols(self, shoff, shentsize, shnum):
        """The code symbols of .symtab, else .dynsym (a stripped image), sorted
        by address. Only functions, and untyped labels that lie in an executable
        section, are code symbols: undefined and absolute symbols, data, sections,
        files, labels in data sections (such as __fini_array_end) and the ARM
        mapping symbols ($a, $t, $d, $x) are left out. A symbol without a size
        ends where its section does."""
        if shoff == 0 or shnum == 0:
            return []
        sections = self._sections(shoff, shentsize, shnum)
        table = next((s for s in sections if s[0] == SHT_SYMTAB), None)
        if table is None:
            table = next((s for s in sections if s[0] == SHT_DYNSYM), None)
        if table is None:
            return []
        _stype, _flags, _addr, off, size, link, entsize = table
        if link >= len(sections):
            raise ElfError("%s: symbol table without a string table" % self.path)
        _t, _f, _a, str_off, str_size, _l, _e = sections[link]
        strings = self.data[str_off:str_off + str_size]
        entsize = entsize or (16 if self.bits == 32 else 24)
        out = []
        for at in range(off, off + size - entsize + 1, entsize):
            if self.bits == 32:
                name, value, symsize, info, _other, shndx = struct.unpack_from("<IIIBBH", self.data, at)
            else:
                name, info, _other, shndx, value, symsize = struct.unpack_from("<IBBHQQ", self.data, at)
            if (info & 0xF) not in (STT_NOTYPE, STT_FUNC, STT_GNU_IFUNC):
                continue
            if shndx in (SHN_UNDEF, SHN_ABS) or shndx >= len(sections):
                continue
            _t, sec_flags, sec_addr, _o, sec_size, _l, _e = sections[shndx]
            if not sec_flags & SHF_EXECINSTR:
                continue
            end = strings.find(b"\0", name)
            text = strings[name:end if end >= 0 else len(strings)].decode("utf-8", "replace")
            if not text or text[0] == "$":
                continue
            if self.machine == EM_ARM:
                value &= ~1  # a Thumb function's address has its low bit set
            out.append(Symbol(text, value, symsize,
                              value + symsize if symsize else max(sec_addr + sec_size, value)))
        out.sort(key=lambda s: (s.addr, -s.size, s.name))
        return out

    # -- queries ----------------------------------------------------------------

    def in_image(self, addr):
        """Is the (image-relative) address inside a loadable segment?"""
        return any(s.contains(addr) for s in self.segments)

    def in_code(self, addr):
        """Is it inside an executable segment?"""
        return any(s.executable and s.contains(addr) for s in self.segments)

    def find(self, addr):
        """The Symbol that holds the image-relative address, or None. A symbol
        with a size holds the addresses of that size; one without (an assembly
        label) holds everything up to the next symbol or the end of its section."""
        lo, hi = 0, len(self.symbols)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.symbols[mid].addr <= addr:
                lo = mid + 1
            else:
                hi = mid
        i = lo - 1
        if i < 0:
            return None
        sym = self.symbols[i]
        return sym if addr < sym.end else None

    def lookup(self, addr):
        """(name, offset into the symbol) for the address, or None."""
        sym = self.find(addr)
        return (sym.name, addr - sym.addr) if sym else None

    @property
    def is_a64(self):
        return self.machine == EM_AARCH64
