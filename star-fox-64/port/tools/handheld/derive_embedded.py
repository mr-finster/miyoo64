#!/usr/bin/env python3
"""The game data compiled into the decomp's C sources, rebuilt from the ROM.

The decomp's game code carries data as C initialisers: display lists,
textures, object and hitbox tables, audio constants. Those bytes are ROM
content, so the game folder must not ship them (round 1 decision: zero tolerance).
With -fdata-sections every such symbol is its own input section; this tool
finds the ones whose bytes the ROM reproduces and describes how.

  select  <linker map> <port ELF> <N64 ELF> <port image> <out list>
      Every .data.* / .rodata.* input section of a game object whose symbol
      has an N64 twin (same name, same size) and whose bytes are the twin's in
      the port's byte order, word by word: copy, 16- or 32-bit swap, or a
      pointer the N64 -> port symbol map explains. Writes "<object> <section>"
      lines: the sections Makefile.handheld renames to .hhasset.*. Game data
      sections without such a twin (string pools, unnamed .rodata, static
      locals, tables shared with other ports) move too when stretches of
      them are found in the port image (8+ bytes, any of the three orders).

  recipes <linker map> <port ELF (PROGBITS link)> <N64 ELF> <decompressed ROM> <port image> <out bin>
      For every .hhasset.* input section of the final layout: its port
      address, its ROM offset (N64 segment ROM start + offset in segment), its
      size, the overlay it belongs to (reloaded with it) and runs as in
      recipes.bin, plus unit 6: the word is a pointer, followed by the port
      value to store (an address in this build, not ROM content). Replayed and
      verified against the port bytes here; any symbol that fails fails the build.
      A section without a twin becomes several entries: its stretches found in
      the port image (read from there) and the bytes between them as literals.

  embedded.bin: "SF64EMB1", u32 entry count, per entry
      u32 port_addr, u32 rom_off, u32 size, u32 overlay (0xFFFFFFFF = main),
      u32 word count, words: runs (length << 3 | unit), unit 6 runs followed by
      their literals, unit 7 (a 1-3 byte literal tail) by one word.
"""
import bisect, collections, re, struct, sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from elf32 import Elf32

OVERLAYS = ['ovl_i1', 'ovl_i2', 'ovl_i3', 'ovl_i4', 'ovl_i5', 'ovl_i6', 'ovl_menu', 'ovl_ending', 'ovl_unused']


def parse_map(path, prefixes):
    """Input sections from a GNU ld map: (section, addr, size, object) for names with a prefix."""
    out = []
    lines = open(path).read().splitlines()
    i = 0
    while i < len(lines):
        l = lines[i]
        m = re.match(r'^ (\.\S+)\s*$', l)          # long name: address on the next line
        if m and i + 1 < len(lines):
            m2 = re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$', lines[i + 1])
            if m2:
                name = m.group(1)
                if name.startswith(prefixes):
                    out.append((name, int(m2.group(1), 16), int(m2.group(2), 16), m2.group(3)))
                i += 2
                continue
        m = re.match(r'^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$', l)
        if m and m.group(1).startswith(prefixes):
            out.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
        i += 1
    return out


class N64:
    def __init__(self, path):
        self.E = Elf32(path)
        E = self.E
        self.sym = {}                                # name -> [symbols] (sized objects)
        for s in E.symbols:
            if s['type'] == 1 and s['size'] > 0 and s['shndx'] not in (0, 0xfff1, 0xfff2):
                self.sym.setdefault(s['name'], []).append(s)
        self.all = sorted((s['value'], s['size'], s['name'], s['shndx']) for s in E.symbols
                          if s['type'] in (1, 2) and s['size'] > 0 and s['shndx'] not in (0, 0xfff1, 0xfff2))
        self.addr = [a for a, _, _, _ in self.all]
        self.abs = {s['name']: s['value'] for s in E.symbols}
        # linker symbols (overlay bounds, ROM offsets: unsized NOTYPE/absolute):
        # an exact address match maps by name to the port's symbol of that name
        self.linker = collections.defaultdict(list)
        for s in E.symbols:
            if s['size'] == 0 and s['type'] == 0 and s['name'] and not s['name'].startswith(('$', '.')):
                self.linker[s['value']].append(s['name'])

    def bytes_of(self, s):
        sec = self.E.sections[s['shndx']]
        if sec['type'] != 1:
            return None
        o = sec['offset'] + (s['value'] - sec['addr'])
        return self.E.data[o:o + s['size']]

    def rom_off(self, s):
        """ROM offset of an N64 symbol: its segment's ROM start + offset in the segment."""
        sec = self.E.sections[s['shndx']]
        seg = sec['name'].lstrip('.')
        start = self.abs.get(seg + '_ROM_START')
        if start is None:
            return None
        return start + (s['value'] - sec['addr'])

    def candidates(self, v):
        i = bisect.bisect_right(self.addr, v)
        out = []
        for k in range(i - 1, max(i - 64, -1), -1):
            a, sz, name, sh = self.all[k]
            if a <= v < a + sz or v == a + sz:
                out.append((a, name, sh))
            if a + 0x200000 < v:
                break
        return out


def port_names(P):
    names = collections.defaultdict(list)
    for s in P.symbols:
        if s['type'] in (1, 2) and s['shndx'] not in (0, 0xfff1, 0xfff2):
            names[s['name']].append(s['value'])
    return names


def translate(n64, pnames, v, home):
    cands = n64.candidates(v)
    if not cands:
        # an overlay bound or other linker symbol at exactly v, by name
        outs = {pnames[nm][0] for nm in n64.linker.get(v, ()) if len(pnames.get(nm, ())) == 1}
        return outs.pop() if len(outs) == 1 else None
    # a symbol that starts at v beats one that ends there (one-past-end reads
    # of the previous symbol only when nothing starts at v); then the
    # referring symbol's own section (overlays share VRAM)
    starts = [c for c in cands if c[0] == v]
    cands = starts or cands
    same = [c for c in cands if c[2] == home]
    outs = set()
    for a, name, sh in (same or cands):
        p = pnames.get(name)
        if p and len(p) == 1:
            outs.add((p[0] + (v - a)) & 0xFFFFFFFF)
    return outs.pop() if len(outs) == 1 else None


def derive(nb, pb, n64, pnames, home, stats=None):
    """Runs (len, unit[, literals]) turning the N64 bytes nb into the port bytes pb.

    Where the ROM has the bytes in some byte order they are rebuilt from it
    (units 1/2/4). Any other word is the port's own value -- a pointer or a
    linker address of this build, or a port-side change -- and is stored as a
    literal (unit 6; unit 7 for a 1-3 byte tail). A literal differs from the
    ROM's word at that place by construction, and the leak scan checks the
    recipe file too. `stats` counts literals that are explained pointers."""
    runs = []
    i, L = 0, len(pb)

    def add(unit, step, lit=None):
        if runs and runs[-1][1] == unit and unit != 7:
            runs[-1][0] += step
            if lit is not None:
                runs[-1][2].append(lit)
        else:
            runs.append([step, unit, [lit] if lit is not None else None])

    while i < L:
        if i + 4 <= L:
            w, q = nb[i:i + 4], pb[i:i + 4]
            if q == w:
                add(1, 4); i += 4; continue
            if q == w[::-1]:
                add(4, 4); i += 4; continue
            if q == w[1::-1] + w[3:1:-1]:
                add(2, 4); i += 4; continue
            if q[:2] in (w[:2], w[1::-1]) and q[2:] in (w[2:], w[3:1:-1]):
                add(1 if q[:2] == w[:2] else 2, 2)
                add(1 if q[2:] == w[2:] else 2, 2)
                i += 4; continue
            v = struct.unpack('>I', w)[0]
            lit = struct.unpack('<I', q)[0]
            if stats is not None:
                t = translate(n64, pnames, v, home)
                stats['pointer' if t == lit else 'port value'] += 1
            add(6, 4, lit); i += 4; continue
        # a 1-3 byte tail
        t = pb[i:L]
        if t == nb[i:L]:
            add(1, L - i)
        else:
            if stats is not None:
                stats['port value'] += 1
            add(7, L - i, struct.unpack('<I', t + b'\0' * (4 - len(t)))[0])
        i = L
    return runs


def apply(nb, runs):
    out = bytearray()
    pos = 0
    for length, unit, lits in runs:
        src = nb[pos:pos + length]
        if unit == 1:
            out += src
        elif unit == 2:
            out += bytes(src[k ^ 1] for k in range(length))
        elif unit == 4:
            out += bytes(src[(k & ~3) | (3 - (k & 3))] for k in range(length))
        elif unit == 6:
            out += b''.join(struct.pack('<I', x) for x in lits)
        elif unit == 7:
            out += struct.pack('<I', lits[0])[:length]
        pos += length
    return bytes(out)


class Image:
    """The port image (sf64.rom, as the device builds it): 8-byte keys at
    4-aligned offsets, first occurrence, for finding data the game code
    carries without an N64 symbol of its own (string pools, unnamed
    .rodata, static locals, the audio code's tables)."""
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        self.index = {}
        idx = self.index
        mv = memoryview(d)
        for o in range(0, len(d) - 7, 4):
            k = bytes(mv[o:o + 8])
            if k not in idx:
                idx[k] = o

    def longest(self, want, off):
        """Length of the common prefix of want and the image at off."""
        d, n = self.data, 0
        L = min(len(want), len(d) - off)
        step = 64
        while n < L:
            m = min(step, L - n)
            if d[off + n:off + n + m] == want[n:n + m]:
                n += m
            else:
                while n < L and d[off + n] == want[n]:
                    n += 1
                break
        return n


def to_n64(b, unit):
    """The port bytes as the ROM holds them (inverse of the unit's swap; a
    partial group at the end is left as it is, as apply() does)."""
    if unit == 1:
        return b
    k = len(b) - len(b) % unit
    if unit == 2:
        return b''.join(b[i:i + 2][::-1] for i in range(0, k, 2)) + b[k:]
    return b''.join(b[i:i + 4][::-1] for i in range(0, k, 4)) + b[k:]


MIN_CHUNK = 8


def cover(pb, img):
    """Chunks (start, length, rom_off, runs) rebuilding pb: stretches found in
    the image (copy or 16/32-bit swap), the rest literal words (units 6/7)."""
    chunks, lit_start, i, L = [], None, 0, len(pb)

    def flush(end):
        if lit_start is not None and end > lit_start:
            b = pb[lit_start:end]
            k = len(b) - len(b) % 4
            runs = []
            if k:
                runs.append([k, 6, list(struct.unpack(f'<{k // 4}I', b[:k]))])
            if len(b) > k:
                runs.append([len(b) - k, 7, [struct.unpack('<I', b[k:] + b'\0' * (4 - (len(b) - k)))[0]]])
            chunks.append((lit_start, len(b), 0, runs))

    while i < L:
        best = (0, None, None)
        if L - i >= MIN_CHUNK:
            for unit in (1, 4, 2):
                if i % unit:
                    continue
                want = to_n64(pb[i:], unit)
                o = img.index.get(want[:8])
                if o is None:
                    continue
                n = img.longest(want, o)
                if unit > 1:
                    n -= n % unit if n < L - i else 0
                if n > best[0]:
                    best = (n, o, unit)
        if best[0] >= MIN_CHUNK:
            flush(i)
            lit_start = None
            n, o, unit = best
            chunks.append((i, n, o, [[n, unit, None]]))
            i += n
        else:
            if lit_start is None:
                lit_start = i
            i += 1
    flush(L)
    return chunks


def apply_chunks(img, chunks, size):
    out = bytearray(size)
    for start, n, o, runs in chunks:
        out[start:start + n] = apply(img.data[o:o + n], runs)
    return bytes(out)


def sym_of(section):
    if section.startswith('.hhasset.'):  # hh_objects.py renames .data.X to .hhasset.data.X
        section = section[len('.hhasset'):]
    for p in ('.data.rel.ro.local.', '.data.rel.ro.', '.data.rel.local.', '.data.rel.', '.data.', '.rodata.'):
        if section.startswith(p):
            return section[len(p):]
    return None


def port_object(obj):
    return obj.startswith('build/') and '/src/port/' in obj and not obj.endswith(
        ('/port_romimage.o', '/port_rom.o', '/main.o', '/port_stubs.o', '/port_overlay.o'))


def game_object(obj):
    # the decomp's own objects only (not the port layer, libc or the toolchain)
    return (obj.startswith('build/') and '/src/' in obj and '/src/port/' not in obj
            and '/ast_radio' not in obj and '/build_stamp' not in obj)


def overlay_of(obj):
    for k, o in enumerate(OVERLAYS):
        if f'/src/overlays/{o}/' in obj:
            return k
    return 0xFFFFFFFF


def game_data_section(sec):
    return sec in ('.data', '.rodata') or sec.startswith(('.data.', '.rodata.'))


def select(map_path, port_elf, n64_elf, image_path, out_path):
    P, n64, img = Elf32(port_elf), N64(n64_elf), Image(image_path)
    chosen, why = [], collections.Counter()
    for sec, addr, size, obj in parse_map(map_path, ('.data', '.rodata')):
        if size == 0 or not game_data_section(sec):
            continue
        pb = P.bytes_at(addr, size)
        if pb is None:
            why['no bytes'] += 1
            continue
        if port_object(obj):
            # the port layer's own tables stay unless one carries ROM content
            # (the audio mixer's resample table is the microcode's); they are
            # read only once the game runs, after the fill
            if any(c[3][0][1] not in (6, 7) and c[1] >= 32 and len(set(pb[c[0]:c[0] + c[1]])) > 1
                   for c in cover(pb, img)):
                why['port-layer ROM table'] += 1
                print(f'derive_embedded select: port-layer {obj} {sec} ({size} bytes) holds ROM content: moves out')
                chosen.append((obj, sec, size))
            continue
        if not game_object(obj):
            continue
        name = sym_of(sec)
        tw = n64.sym.get(name) if name else None
        if tw and len(tw) == 1 and tw[0]['size'] == size and n64.bytes_of(tw[0]) is not None:
            why['twin'] += 1
        elif any(c[3][0][1] != 6 and c[3][0][1] != 7 for c in cover(pb, img)):
            why['found in the image'] += 1
        else:
            why['not ROM data'] += 1
            continue
        chosen.append((obj, sec, size))
    with open(out_path, 'w') as f:
        for obj, sec, size in chosen:
            f.write(f'{obj} {sec}\n')
    print(f'derive_embedded select: {len(chosen)} sections, {sum(s for _, _, s in chosen)} bytes move out; '
          + ', '.join(f'{v} {k}' for k, v in why.items()))


def recipes(map_path, port_elf, n64_elf, rom_path, image_path, out_path):
    P, n64, img = Elf32(port_elf), N64(n64_elf), Image(image_path)
    kinds = collections.Counter()
    rom = open(rom_path, 'rb').read()
    pnames = port_names(P)
    for s_ in P.symbols:  # linker symbols (overlay bounds) for pointer statistics
        if s_['size'] == 0 and s_['type'] == 0 and s_['name'] and not s_['name'].startswith('$'):
            pnames.setdefault(s_['name'], []).append(s_['value'])
    lit_stats = collections.Counter()
    entries = []
    for sec, addr, size, obj in parse_map(map_path, ('.hhasset.',)):
        if size == 0:
            continue
        name = sym_of(sec)
        tw = n64.sym.get(name) if name else None
        pb = P.bytes_at(addr, size)
        if not (tw and len(tw) == 1 and tw[0]['size'] == size and n64.bytes_of(tw[0]) is not None):
            chunks = cover(pb, img)
            if apply_chunks(img, chunks, size) != pb:
                sys.exit(f'derive_embedded: {obj} {sec}: pieces do not rebuild it')
            for start, n, o, runs in chunks:
                entries.append((addr + start, o, n, overlay_of(obj), runs))
                kinds['piece literal bytes' if runs[0][1] in (6, 7) else 'piece bytes from the image'] += n
            continue
        n = tw[0]
        rom_off = n64.rom_off(n)
        nb = rom[rom_off:rom_off + size] if rom_off is not None else None
        if nb is None or nb != n64.bytes_of(n):
            sys.exit(f'derive_embedded: {name}: ROM offset does not give the N64 bytes')
        runs = derive(nb, pb, n64, pnames, n['shndx'], lit_stats)
        if runs is None or apply(nb, runs) != pb:
            sys.exit(f'derive_embedded: {name}: not reproducible from the ROM in the final layout')
        entries.append((addr, rom_off, size, overlay_of(obj), runs))
    entries.sort()
    out = bytearray(b'SF64EMB1' + struct.pack('<I', len(entries)))
    for addr, rom_off, size, ovl, runs in entries:
        words = []
        for length, unit, lits in runs:
            words.append((length << 3) | unit)
            if unit in (6, 7):
                words.extend(lits)
        out += struct.pack('<IIIII', addr, rom_off, size, ovl, len(words))
        out += struct.pack(f'<{len(words)}I', *words)
    open(out_path, 'wb').write(out)
    print('derive_embedded recipes: ' + ', '.join(f'{v} {k}' for k, v in kinds.items()))
    print(f'derive_embedded recipes: {len(entries)} entries, {sum(e[2] for e in entries)} bytes, '
          f'literal words: {lit_stats["pointer"]} explained pointers + {lit_stats["port value"]} other port values; '
          f'{len(out)} bytes of recipe; all replay exactly')


if __name__ == '__main__':
    if len(sys.argv) == 7 and sys.argv[1] == 'select':
        select(*sys.argv[2:7])
    elif len(sys.argv) == 8 and sys.argv[1] == 'recipes':
        recipes(*sys.argv[2:8])
    else:
        sys.exit(__doc__)
