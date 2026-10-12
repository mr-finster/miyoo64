"""Minimal ELF32 reader for the port tools (sections, symbols, section bytes).

Handles both the N64 ELF (MIPS, big-endian) and the port's (ARM / i386,
little-endian). No dependencies.
"""
import struct


class Elf32:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        if d[:4] != b'\x7fELF' or d[4] != 1:
            raise ValueError(f'{path}: not an ELF32 file')
        self.e = '<' if d[5] == 1 else '>'
        e = self.e
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, _, _, _, _,
         self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from(e + 'HHIIIIIHHHHHH', d, 16)
        self.sections = []
        for i in range(self.shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = \
                struct.unpack_from(e + 'IIIIIIIIII', d, self.shoff + i * self.shentsize)
            self.sections.append(dict(name_off=name, type=typ, flags=flags, addr=addr, offset=off,
                                      size=size, link=link, info=info, align=align, entsize=entsize, index=i))
        strtab = self.sections[self.shstrndx]
        for s in self.sections:
            s['name'] = self._str(strtab, s['name_off'])
        self.symbols = []
        for s in self.sections:
            if s['type'] == 2:  # SHT_SYMTAB
                names = self.sections[s['link']]
                for k in range(s['size'] // 16):
                    nm, val, sz, info, other, shndx = struct.unpack_from(e + 'IIIBBH', d, s['offset'] + k * 16)
                    self.symbols.append(dict(name=self._str(names, nm), value=val, size=sz,
                                             type=info & 0xF, bind=info >> 4, shndx=shndx))

    def _str(self, sec, off):
        d = self.data
        start = sec['offset'] + off
        end = d.index(b'\0', start)
        return d[start:end].decode('latin-1')

    def section(self, name):
        for s in self.sections:
            if s['name'] == name:
                return s
        return None

    def bytes_at(self, addr, size):
        """Bytes of a PROGBITS section at virtual address addr (None if not in a loaded section)."""
        for s in self.sections:
            if s['type'] == 1 and s['addr'] and s['addr'] <= addr and addr + size <= s['addr'] + s['size']:
                o = s['offset'] + (addr - s['addr'])
                return self.data[o:o + size]
        return None

    def section_of(self, addr):
        for s in self.sections:
            if s['addr'] and (s['flags'] & 2) and s['addr'] <= addr < s['addr'] + max(s['size'], 1):
                return s
        return None
