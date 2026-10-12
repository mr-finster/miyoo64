#!/usr/bin/env python3
"""Derive, per asset symbol, how its bytes come from the ROM (JSON for
emit_recipes.py; the device replays it on first boot, assets_handheld.c).

From the MK64 port (tools/psp/derive_recipes.py, after beckerd's PSP
tool), by way of super-mario-64. Paper Mario's asset region (vertices, display
lists, textures, the font and the image-effect keyframes, all raw in the ROM)
needs no MIO0 blocks; the rest is unchanged.

Kinds
  RAW      bytes at a ROM offset            (xform: id / sw16 / sw32 / pattern)
  MIO0     bytes inside a decompressed mio0 block at a ROM offset (same xforms)
  LITERAL  <= 16-byte constants carried verbatim (numbers, not assets)
  RELOCS   symbol made only of pointers (filled by the relocation pass)

usage: derive_recipes.py assets.bin "Paper Mario (USA).z64" -o recipes_handheld.json [--cache f] [--report f]
"""
import struct, sys, re, os, pickle, collections, argparse

# ---------------------------------------------------------------- helpers
def mio0_decode(d, off):
    if d[off:off+4] != b'MIO0': return None
    size, coff, uoff = struct.unpack('>III', d[off+4:off+16])
    out = bytearray(); layout = off + 16; cp = off + coff; up = off + uoff
    bits = 0; nbits = 0; hi = off + 16
    while len(out) < size:
        if nbits == 0:
            bits = struct.unpack('>I', d[layout:layout+4])[0]; layout += 4; nbits = 32
        if bits & 0x80000000:
            out.append(d[up]); up += 1
        else:
            b0, b1 = d[cp], d[cp+1]; cp += 2
            length = (b0 >> 4) + 3; dist = ((b0 & 0xF) << 8 | b1) + 1
            for _ in range(length): out.append(out[-dist])
        bits = (bits << 1) & 0xffffffff; nbits -= 1
    mio0_decode.consumed = max(layout, cp, up) - off
    return bytes(out[:size])

def swap16(b):
    n = len(b)//2*2; a = bytearray(b)
    a[0:n:2], a[1:n:2] = b[1:n:2], b[0:n:2]; return bytes(a)
def swap32(b):
    n = len(b)//4*4; a = bytearray(b)
    a[0:n:4], a[1:n:4], a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4], b[1:n:4], b[0:n:4]; return bytes(a)
def sorted4(b):
    n = len(b)//4*4
    return b''.join(bytes(sorted(b[i:i+4])) for i in range(0, n, 4))

# word shapes: field widths inside one 4-byte word (LE target from BE source)
SHAPES = {'4': [4], '22': [2, 2], '211': [2, 1, 1], '112': [1, 1, 2], '1111': [1, 1, 1, 1]}
def apply_shape(word, shape):
    out = b''; i = 0
    for w in SHAPES[shape]:
        out += word[i:i+w][::-1]; i += w
    return out
def apply_pattern(b, shapes):
    n = len(b)//4*4; out = bytearray(b)
    for i in range(0, n, 4):
        out[i:i+4] = apply_shape(b[i:i+4], shapes[(i//4) % len(shapes)])
    return bytes(out)

ORDER = ['1111', '22', '4', '211', '112']   # preference when a word fits several shapes
PERIODS = [1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16]
MAX_PERIODIC = 16                           # longer patterns are one symbol's own (aperiodic)

def pattern_periodic(shapes):
    """The smallest period whose per-phase shape sets all intersect, as a pattern tuple."""
    for p in PERIODS:
        pat = []
        for ph in range(p):
            opts = None
            for w in range(ph, len(shapes), p):
                if shapes[w] is None: continue
                opts = set(shapes[w]) if opts is None else opts & set(shapes[w])
            if opts is None: opts = {'1111'}
            if not opts: pat = None; break
            pat.append(sorted(opts, key=ORDER.index)[0])
        if pat is not None: return tuple(pat)
    return None

def pattern_aperiodic(shapes):
    """One shape per word for the whole symbol (object layouts, level scripts, sound
    banks: structs of mixed fields in no fixed period). A word that fits several
    keeps its predecessor's where it can, so the runs stay long."""
    pat = []; prev = None
    for opts in shapes:
        if opts is None or (prev is not None and prev in opts): pick = prev or '1111'
        else: pick = opts[0]
        pat.append(pick); prev = pick
    return tuple(pat)

def rle(pat):
    out = []
    for sh in pat:
        if out and out[-1][0] == sh: out[-1][1] += 1
        else: out.append([sh, 1])
    return out

def load_assets(path):
    d = open(path,'rb').read()
    magic, ver, addr, size, nrel, nsym, nnames, bid, crc, pad = struct.unpack_from('<8sIIIIIIIII', d, 0)
    p = 44
    relocs = list(struct.unpack_from('<%dI'%nrel, d, p)); p += nrel*4
    symtab = p; p += nsym*16
    names = d[p:p+nnames]; p += nnames
    syms = []
    for i in range(nsym):
        off, sz, h, nm = struct.unpack_from('<IIII', d, symtab + i*16)
        syms.append((names[nm:names.index(b'\x00', nm)].decode(), off, sz))
    p = pad if pad else (p + 15) & ~15  # header's last word: explicit data offset (page-aligned builds)
    load_assets.build_id = bid
    return addr, size, relocs, syms, d[p:p+size]

# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('assets'); ap.add_argument('rom'); ap.add_argument('-o', required=True)
    ap.add_argument('--cache', default=None)
    ap.add_argument('--report', default=None)
    a = ap.parse_args()
    rom = open(a.rom, 'rb').read()
    addr, size, relocs, syms, data = load_assets(a.assets)
    relset = set(relocs)

    # candidate sources (cached: mio0 decode in python is slow)
    cache = a.cache or (a.assets + '.mio0cache')
    if os.path.exists(cache):
        blocks = pickle.load(open(cache, 'rb'))
    else:
        blocks = []
        for m in re.finditer(b'MIO0', rom):
            b = mio0_decode(rom, m.start())
            if b: blocks.append((m.start(), b, mio0_decode.consumed))
        pickle.dump(blocks, open(cache, 'wb'))
    if blocks and len(blocks[0]) == 2:  # old cache without lengths
        blocks = [(o, b, len(mio0_decode(rom, o)) and mio0_decode.consumed) for o, b in blocks]
        pickle.dump(blocks, open(cache, 'wb'))
    block_len = {o: (l, len(b)) for o, b, l in blocks}
    cands = [('RAW', 0, rom)] + [('MIO0', o, b) for o, b, l in blocks]
    cand_stream = {(kind, off): b for kind, off, b in cands}
    variants = []
    for kind, off, b in cands:
        variants.append((kind, 'id', off, b)); variants.append((kind, 'sw16', off, swap16(b))); variants.append((kind, 'sw32', off, swap32(b)))
    sorted_cands = None  # built lazily for pattern inference

    recipes = []      # (name, off, size, kind, xform, src, extra)
    patterns = {}     # shapes tuple -> id
    class _PInv(dict):
        def __getitem__(self, k):
            pid = int(k[3:])
            return next(p for p, i in patterns.items() if i == pid)
    patterns_inv = _PInv()
    unmatched = []
    stats = collections.Counter()

    def match_simple(b, sz, rel_in, near=None):
        runs = []; prev = 0
        for r in rel_in + [sz]:
            if r - prev >= 8: runs.append((prev, r))
            prev = r + 4
        if not runs: return None
        ps, pe = max(runs, key=lambda x: x[1]-x[0])
        probe = b[ps:min(pe, ps+64)]
        # Near the last ROM match first: the same bytes often sit in several maps (a Duplighost's
        # settings in pra_* and elsewhere), and the first copy in the ROM breaks the chain of
        # neighbours that small ambiguous symbols after it rely on (pra_39_DefaultNPCs)
        passes = [(near, near + (1 << 18)), (max(0, near - (1 << 16)), near)] if near is not None else []
        for lo, hi in passes + [(0, None)]:
            for kind, tr, coff, cb in variants:
                if hi is not None and kind != 'RAW': continue
                end = len(cb) if hi is None else min(len(cb), hi + ps + len(probe))
                k = cb.find(probe, lo + ps, end)
                # a swapped variant is the whole stream swapped: its words line up
                # with the game's (which swaps from the symbol's start) only at a
                # word-aligned start (half-word for sw16)
                align = 4 if tr == 'sw32' else 2 if tr == 'sw16' else 1
                while k >= 0:
                    start = k - ps
                    if start >= 0 and start % align == 0 and start + sz <= len(cb):
                        ok = True; cur = 0
                        for r in rel_in:
                            if cb[start+cur:start+r] != b[cur:r]: ok = False; break
                            cur = r + 4
                        if ok and cb[start+cur:start+sz] == b[cur:sz]:
                            return (kind, tr, coff, start)
                    k = cb.find(probe, k+1, end)
        return None

    def word_shapes(b, sz, rel_in, cb, start):
        """Per word (the last may be partial): the shapes that turn cb's word at
        `start` into b's; None for a pointer word. None if some word has none."""
        n4r = (sz + 3) & ~3
        if start < 0 or start + sz > len(cb) or n4r < 4: return None
        relw = set(r // 4 for r in rel_in)
        shapes = []
        for w in range(n4r // 4):
            if w in relw: shapes.append(None); continue
            tw = b[w*4:w*4+4]; cw = cb[start+w*4:start+w*4+4]
            if len(cw) < 4: cw = cw + b'\0' * (4 - len(cw))
            ok = [sh for sh in ORDER if apply_shape(cw, sh)[:len(tw)] == tw]
            if not ok: return None
            shapes.append(ok)
        return shapes

    def infer_pattern(b, sz, rel_in):
        nonlocal sorted_cands
        if sorted_cands is None:
            sorted_cands = [(kind, off, ph, sorted4(cb[ph:])) for kind, off, cb in cands for ph in range(4)]
        n4 = sz // 4 * 4
        if n4 < 16: return None
        relw = set(r // 4 for r in rel_in)
        # probe: longest run of reloc-free aligned words
        best = (0, 0); cur = None
        for w in range(n4 // 4):
            if w in relw:
                cur = None; continue
            if cur is None: cur = w
            if w - cur + 1 > best[1] - best[0]: best = (cur, w + 1)
        if best[1] - best[0] < 4: return None
        pw0, pw1 = best; pw1 = min(pw1, pw0 + 16)
        probe = sorted4(b[pw0*4:pw1*4])
        aper = None  # the first place every word fits, if no period does anywhere
        for kind, coff, ph, scb in sorted_cands:
            k = scb.find(probe)
            while k >= 0:
                start = ph + k - pw0 * 4
                cb = cand_stream[(kind, coff)]
                if k % 4 == 0 and start >= 0 and start + sz <= len(cb):
                    shapes = word_shapes(b, sz, rel_in, cb, start)
                    if shapes is not None:
                        pat = pattern_periodic(shapes)
                        if pat and verify_at(b, sz, rel_in, cb, start, pat):
                            return (kind, coff, start, pat)
                        if aper is None:
                            pat = pattern_aperiodic(shapes)
                            if verify_at(b, sz, rel_in, cb, start, pat): aper = (kind, coff, start, pat)
                k = scb.find(probe, k+1)
        return aper

    unpack_syms = collections.defaultdict(list)
    pending = []   # (name, off, sz, b, rel_in)

    def verify_at(b, sz, rel_in, cb, start, xf):
        """Does candidate stream cb (untransformed) at `start` reproduce b under transform xf?"""
        if start < 0 or start + sz > len(cb): return False
        n4 = sz // 4 * 4
        seg = cb[start:start + sz]
        if xf == 'id': t = seg
        elif xf == 'sw16': t = swap16(cb[start:start + ((sz + 1) & ~1)])[:sz]
        elif xf == 'sw32': t = swap32(cb[start:start + ((sz + 3) & ~3)])[:sz]
        else:  # like the generator: transform the word-rounded range, keep sz bytes
            n4r = (sz + 3) & ~3
            seg = cb[start:start + n4r]; seg += b'\0' * (n4r - len(seg))  # symbol may end at the block end
            t = apply_pattern(seg, xf)[:sz]
        cur = 0
        for r in rel_in:
            if t[cur:r] != b[cur:r]: return False
            cur = r + 4
        return t[cur:sz] == b[cur:sz]

    # Fast path (Paper Mario: a 40 MB ROM and 31k symbols make each whole-ROM probe slow): a symbol usually sits in the
    # ROM right after the one before it in the region (the objects' data are laid out as in the ROM), or at the ROM
    # offset its name gives (splat's D_<vram>_<rom>). Tried first, with the transforms and patterns seen so far.
    name_rom = re.compile(r'^D_[0-9A-F]{8}_([0-9A-F]{5,7})$')
    prev = None  # (region off, size, rom start) of the last symbol matched in the plain ROM
    def try_at(b, sz, rel_in, start):
        if start is None or start < 0 or start + sz > len(rom): return None
        for xf in ['id', 'sw32', 'sw16'] + [tuple(p) for p in patterns if len(p) <= MAX_PERIODIC]:
            if verify_at(b, sz, rel_in, rom, start, xf):
                return xf if isinstance(xf, str) else 'pat%d' % patterns[xf]
        pat = infer_at(b, sz, rel_in, rom, start)
        if pat: return 'pat%d' % patterns.setdefault(pat, len(patterns))
        return None
    def infer_at(b, sz, rel_in, cb, start):
        shapes = word_shapes(b, sz, rel_in, cb, start)
        if shapes is None: return None
        pat = pattern_periodic(shapes)
        if pat and verify_at(b, sz, rel_in, cb, start, pat): return pat
        pat = pattern_aperiodic(shapes)
        return pat if verify_at(b, sz, rel_in, cb, start, pat) else None

    for name, off, sz in syms:
        # trailing zero bytes are alignment padding (the region starts zeroed): match the content only
        full = sz
        stripped = len(data[off:off+sz].rstrip(b'\0'))
        if stripped == 0:
            stats['ZERO'] += 1; continue
        # (the last symbol of a page-aligned region carries up to 4 KB of padding)
        if stripped < sz and (sz - stripped) <= 4096:
            sz = stripped
        b = data[off:off+sz]
        rel_in = sorted(r - off for r in relocs if off <= r < off+sz)
        if 4*len(rel_in) >= sz:
            recipes.append((name, off, sz, 'RELOCS', 'id', 0, 0)); stats['RELOCS'] += 1; continue
        found = None
        for start in ((prev[2] + (off - prev[0])) if prev and 0 < off - prev[0] < (1 << 16) else None,
                      int(name_rom.match(name).group(1), 16) if name_rom.match(name) else None):
            xf = try_at(b, sz, rel_in, start)
            if xf:
                found = (start, xf); break
        if found:
            recipes.append((name, off, sz, 'RAW', found[1], 0, found[0])); stats['RAW/near'] += 1
            prev = (off, sz, found[0]); continue
        f = match_simple(b, sz, rel_in, max(0, prev[2] + (off - prev[0])) if prev else None)
        if f:
            recipes.append((name, off, sz, f[0], f[1], f[2], f[3])); stats['%s/%s' % (f[0], f[1])] += 1
            if f[0] == 'RAW': prev = (off, sz, f[3])
            continue
        f = infer_pattern(b, sz, rel_in)
        if f:
            pid = patterns.setdefault(f[3], len(patterns))
            recipes.append((name, off, sz, f[0], 'pat%d' % pid, f[1], f[2])); stats['%s/pattern' % f[0]] += 1
            if f[0] == 'RAW': prev = (off, sz, f[2])
            continue
        pending.append((name, off, sz, b, rel_in))

    # ---- locality: a symbol sits at the same distance from its matched neighbours in the source as in the region
    cand_by = {('RAW', 0): rom}
    for o, cb, l in blocks: cand_by[('MIO0', o)] = cb
    def xf_list():
        return ['id', 'sw16', 'sw32'] + [tuple(p) for p in patterns if len(p) <= MAX_PERIODIC]
    def xf_name(xf):
        if isinstance(xf, tuple): return 'pat%d' % patterns.setdefault(xf, len(patterns))
        return xf
    def infer_at(b, sz, rel_in, cb, start):
        """Per-word field shapes of b vs candidate cb at a known start -> a pattern
        (periodic if one fits, else the symbol's own) or None."""
        shapes = word_shapes(b, sz, rel_in, cb, start)
        if shapes is None: return None
        pat = pattern_periodic(shapes)
        if pat and verify_at(b, sz, rel_in, cb, start, pat): return pat
        pat = pattern_aperiodic(shapes)
        return pat if verify_at(b, sz, rel_in, cb, start, pat) else None

    progress = True
    while pending and progress:
        progress = False
        by_off = sorted(recipes, key=lambda r: r[1])
        offs = [r[1] for r in by_off]
        still = []
        import bisect
        for name, off, sz, b, rel_in in pending:
            i = bisect.bisect_left(offs, off)
            neigh = [r for r in by_off[max(0, i-3):i] + by_off[i:i+3] if r[3] in ('RAW', 'MIO0')]
            found = None
            for nb in neigh:
                kind, xf0, coff, start0 = nb[3], nb[4], nb[5], nb[6]
                cb = cand_by[(kind, coff)]
                start = start0 + (off - nb[1])
                xfs = [tuple(patterns_inv[xf0]) if xf0.startswith('pat') else xf0]
                if len(xfs[0]) > MAX_PERIODIC and not isinstance(xfs[0], str): xfs = []  # another symbol's own
                xfs += [x for x in xf_list() if x not in xfs]
                for xf in xfs:
                    if verify_at(b, sz, rel_in, cb, start, xf):
                        found = (kind, xf_name(xf), coff, start); break
                if not found:
                    pat = infer_at(b, sz, rel_in, cb, start)
                    if pat: found = (kind, xf_name(pat), coff, start)
                if found: break
            if found:
                recipes.append((name, off, sz) + found); stats['%s/locality' % found[0]] += 1; progress = True
            else:
                still.append((name, off, sz, b, rel_in))
        pending = still

    # ---- masked search: pointer words wild-carded.  The transforms are involutions,
    # so the SYMBOL is transformed (per pattern and phase) and the plain streams
    # are searched, near the matched neighbours first (same block / a +-1 MB ROM
    # window, all patterns), then globally with the cheap transforms only.
    if pending:
        plain = {(kind, coff): cb for kind, xf, coff, cb in variants if xf == 'id'}
        all_pats = [('id',), ('22',), ('4',), ('112',), ('211',)] + [tuple(p) for p in patterns if len(p) <= MAX_PERIODIC]
        cheap_pats = [('id',), ('22',), ('4',), ('112',), ('211',)]
        by_off = sorted(recipes, key=lambda r: r[1]); offs = [r[1] for r in by_off]
        import bisect
        def try_search(b, sz, rel_in, pats, streams):
            n4 = sz // 4 * 4
            for pat in pats:
                for ph in range(len(pat)):
                    rot = pat[ph:] + pat[:ph]
                    tb = b[:n4] if rot == ('id',) else apply_pattern(b[:n4], rot)
                    if n4 == 0: tb = b[:sz]
                    parts = []; cur = 0
                    for r in rel_in:
                        if r >= len(tb): break
                        parts.append(re.escape(tb[cur:r])); parts.append(b'.{4}'); cur = r + 4
                    parts.append(re.escape(tb[cur:len(tb)]))
                    rx = re.compile(b''.join(parts), re.S)
                    xf = 'id' if rot == ('id',) else ('sw16' if rot == ('22',) else ('sw32' if rot == ('4',) else rot))
                    for kind, coff, cb, lo, hi in streams:
                        nhit = 0
                        for mm in rx.finditer(cb, lo, hi):
                            nhit += 1
                            if nhit > 500: break
                            if verify_at(b, sz, rel_in, cb, mm.start(), xf):
                                return (kind, xf_name(xf) if isinstance(xf, tuple) else xf, coff, mm.start())
            return None
        for name, off, sz, b, rel_in in list(pending):
            i = bisect.bisect_left(offs, off)
            near = []
            for nb in by_off[max(0, i-4):i+4]:
                if nb[3] not in ('RAW', 'MIO0'): continue
                key = (nb[3], nb[5]); cb = plain[key]
                if nb[3] == 'RAW':
                    lo, hi = max(0, nb[6] - (1 << 20)), min(len(cb), nb[6] + (1 << 20))
                else:
                    lo, hi = 0, len(cb)
                if (key, lo, hi) not in [(n[0], n[3], n[4]) for n in near]:
                    near.append((key, key[0], key[1], cb, lo, hi))
            near_streams = [(k[1], k[2], k[3], k[4], k[5]) for k in near]
            found = try_search(b, sz, rel_in, all_pats, near_streams)
            if not found:
                found = try_search(b, sz, rel_in, cheap_pats, [(k[0], k[1], cb, 0, len(cb)) for k, cb in plain.items()])
            if found:
                recipes.append((name, off, sz) + found); stats['%s/masked' % found[0]] += 1; pending.remove((name, off, sz, b, rel_in))
    # ---- brute force: every occurrence of the first non-zero non-pointer word
    # (under every field shape) in every stream, kept only where the second
    # such word fits too, then the pattern inferred there. For symbols with no
    # 4-word pointer-free stretch to probe with (the level table: a pointer
    # every third word) and small ones.
    def anchors(b, sz, rel_in):
        n4r = (sz + 3) & ~3
        nz = [i for i in range(0, n4r, 4) if i not in rel_in and any(b[i:i+4])]
        return nz[:2]
    def fits(cw, tw):
        if len(cw) < 4: cw = cw + b'\0' * (4 - len(cw))
        return any(apply_shape(cw, sh)[:len(tw)] == tw for sh in SHAPES)
    def brute_candidates(b, sz, rel_in, max_tries=20000):
        """(kind, coff, cb, start) where the anchor words fit, in stream order."""
        an = anchors(b, sz, rel_in)
        if not an: return
        wi = an[0]
        w0 = b[wi:wi+4]; w0 += b'\0' * (4 - len(w0))
        for sh in SHAPES:
            probe = apply_shape(w0, sh)
            for kind, coff, cb in plain_streams:
                k = cb.find(probe); tries = 0
                while k >= 0 and tries < max_tries:
                    tries += 1
                    st = k - wi
                    if st >= 0 and st % 4 == 0 and st + sz <= len(cb) and \
                       (len(an) < 2 or fits(cb[st+an[1]:st+an[1]+4], b[an[1]:an[1]+4])):
                        yield kind, coff, cb, st
                    k = cb.find(probe, k + 1)
    plain_streams = [(kind, coff, cb) for kind, xf, coff, cb in variants if xf == 'id']
    if pending:
        for item in list(pending):
            name, off, sz, b, rel_in = item
            found = None
            for kind, coff, cb, st in brute_candidates(b, sz, rel_in):
                pat = infer_at(b, sz, rel_in, cb, st)
                if pat: found = (kind, xf_name(pat), coff, st); break
            if found:
                recipes.append((name, off, sz) + found); stats['%s/brute' % found[0]] += 1; pending.remove(item)

    # ---- pieces: every word is in the ROM, but not all in one place. sm64-port
    # moved one CALL of the act selector's script ahead of a TRANSITION, SLEEP
    # and SET_MENU_MUSIC under NO_SEGMENTED_MEMORY: the script matches at the
    # place where most of its words fit, and each stretch that doesn't is
    # found on its own near there.
    def piece_recipe(name, off, sz, b, rel_in, kind, coff, cb, st):
        pat = infer_at(b, sz, rel_in, cb, st)
        return (name, off, sz, kind, xf_name(pat), coff, st) if pat else None
    if pending:
        for item in list(pending):
            name, off, sz, b, rel_in = item
            n4r = (sz + 3) & ~3
            best = None
            for kind, coff, cb, st in brute_candidates(b, sz, rel_in, 5000):
                good = [w for w in range(0, n4r, 4) if w in rel_in or fits(cb[st+w:st+w+4], b[w:w+4])]
                if best is None or len(good) > len(best[4]): best = (kind, coff, cb, st, set(good))
            if best is None or len(best[4]) * 2 < n4r // 4: continue  # most words must sit together
            kind, coff, cb, st, good = best
            # stretches: runs of fitting / non-fitting words
            runs = []; w = 0
            while w < n4r:
                ok = w in good; e = w
                while e < n4r and (e in good) == ok: e += 4
                runs.append((w, min(e, sz), ok)); w = e
            pieces = []
            for a0, a1, ok in runs:
                pb = b[a0:a1]; prel = [r - a0 for r in rel_in if a0 <= r < a1]
                pname = '%s+%d' % (name, a0)
                if ok:
                    r = piece_recipe(pname, off + a0, a1 - a0, pb, prel, kind, coff, cb, st + a0)
                else:
                    r = None
                    for d in range(0, 8192, 4):  # nearest first, either side
                        for p in ((st + a0 + d, st + a0 - d) if d else (st + a0,)):
                            if p < 0 or p + (a1 - a0) > len(cb): continue
                            r = piece_recipe(pname, off + a0, a1 - a0, pb, prel, kind, coff, cb, p)
                            if r: break
                        if r: break
                if r is None: pieces = None; break
                pieces.append(r)
            if pieces:
                recipes.extend(pieces); stats['%s/pieces' % kind] += 1; pending.remove(item)

    # ---- explicit port-computed tables: numbers the port derives itself (not ROM bytes)
    PORT_TABLES = set()  # none yet for SM64
    for item in list(pending):
        name, off, sz, b, rel_in = item
        if name in PORT_TABLES:
            recipes.append((name, off, sz, 'LITERAL', 'id', 0, b)); stats['LITERAL(port)'] += 1; pending.remove(item)
    for name, off, sz, b, rel_in in pending:
        unmatched.append((name, off, sz, len(rel_in))); stats['UNMATCHED'] += 1

    total_bytes = size
    un_bytes = sum(u[2] for u in unmatched)
    rep = ['symbols %d' % len(syms)] + ['  %-16s %5d' % (k, v) for k, v in stats.most_common()]
    rep.append('unmatched: %d symbols, %d bytes (%.2f%%)' % (len(unmatched), un_bytes, 100.0 * un_bytes / total_bytes))
    for u in unmatched: rep.append('  %s size %d relocs %d' % (u[0], u[2], u[3]))
    rep.append('patterns: %d' % len(patterns))
    for pat, pid in patterns.items():
        rep.append('  pat%d: %s' % (pid, ' '.join(pat)) if len(pat) <= MAX_PERIODIC else '  pat%d: %d words, %d runs' % (pid, len(pat), len(rle(pat))))
    print('\n'.join(rep))
    if a.report: open(a.report, 'w').write('\n'.join(rep))
    if unmatched:
        print('NOT emitting recipes: unmatched symbols remain'); sys.exit(1)

    # ------------------------------------------------------------ end to end
    # Rebuild the region from the ROM the way the game does (assets_handheld.c:
    # each recipe transforms its word-rounded source, zero past the stream's
    # end, and keeps `size` bytes; uncovered bytes stay zero) and compare with
    # the linked bytes, pointer words zeroed on both sides.
    pat_by_id = {pid: p for p, pid in patterns.items()}
    block_by_off = {o: b for o, b, l in blocks}
    sim = bytearray(size)
    for name, off, sz, kind, xf, src, extra in recipes:
        if kind == 'RELOCS': continue
        if kind == 'LITERAL': sim[off:off + sz] = extra; continue
        cb = rom if kind == 'RAW' else block_by_off[src]
        n4r = (sz + 3) & ~3
        seg = cb[extra:extra + n4r]; seg += b'\0' * (n4r - len(seg))
        if xf == 'sw16': seg = swap16(seg)
        elif xf == 'sw32': seg = swap32(seg)
        elif xf != 'id': seg = apply_pattern(seg, pat_by_id[int(xf[3:])])
        sim[off:off + sz] = seg[:sz]
    ref = bytearray(data)
    for r in relocs:
        ref[r:r + 4] = b'\0\0\0\0'; sim[r:r + 4] = b'\0\0\0\0'
    if sim != ref:
        bad = [i for i in range(0, size, 4) if sim[i:i + 4] != ref[i:i + 4]]
        import bisect
        starts = [o for n, o, z in syms]
        names = sorted(set(syms[max(0, bisect.bisect_right(starts, i) - 1)][0] for i in bad[:2000]))
        print('END-TO-END MISMATCH: %d words differ, in %s' % (len(bad), ', '.join(names[:40])))
        for i in bad[:24]:
            owner = syms[max(0, bisect.bisect_right(starts, i) - 1)]
            cover = [r for r in recipes if r[1] <= i < r[1] + r[2]]
            print('  %#09x in %s (%#x+%d): linked %s rebuilt %s; recipes: %s' % (
                i, owner[0], owner[1], owner[2], ref[i:i + 4].hex(), sim[i:i + 4].hex(),
                '; '.join('%s %#x+%d %s %s src %#x extra %#x' % (r[0], r[1], r[2], r[3], r[4], r[5], r[6] if not isinstance(r[6], bytes) else 0) for r in cover) or 'none'))
        sys.exit(1)
    print('end to end: the recipes rebuild all %d bytes of the region from the ROM' % size)

    # ------------------------------------------------------------ emit JSON (link-independent)
    import json
    used_blocks = sorted(set(r[5] for r in recipes if r[3] == 'MIO0'))
    js = {
        'region_size': size,
        'build_id': load_assets.build_id,  # emit_recipes.py refuses an assets.bin with another layout
        'patterns': [rle(p) for p, pid in sorted(patterns.items(), key=lambda kv: kv[1])],  # [[shape, words], ...]
        'recipes': [{'name': n, 'off': o, 'size': sz, 'kind': k, 'xform': x, 'src': src,
                     'extra': (extra.hex() if k == 'LITERAL' else extra)} for n, o, sz, k, x, src, extra in recipes],
        'blocks': [{'rom_off': o, 'rom_len': block_len[o][0], 'decomp_len': block_len[o][1]} for o in used_blocks],
    }
    json.dump(js, open(a.o, 'w'))
    print('wrote %s: %d recipes, %d patterns, %d mio0 blocks (max decomp %d)' % (
        a.o, len(recipes), len(patterns), len(used_blocks), max([block_len[o][1] for o in used_blocks] or [0])))

if __name__ == '__main__':
    main()
