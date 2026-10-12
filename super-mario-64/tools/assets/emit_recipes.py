#!/usr/bin/env python3
"""Turn derive_recipes.py's JSON (link-independent) plus the current link's
assets.bin (pointer values) into recipes.bin, the table that ships in the zip
(numbers only, no game data). The game reads it into scratch memory while
extracting on first boot (src/hh/assets_handheld.c).

From the MK64 port (tools/psp/emit_recipes.py). SM64's patterns are
runs of word shapes (u16: shape << 13 | words), so one can cover a whole symbol
of mixed fields (object layouts, level scripts, sound banks), not only a
repeating period of up to 16 words; MK64's packed course display lists are gone.

usage: emit_recipes.py recipes_miyoo.json assets.bin -o recipes.bin --stamp <hex>
"""
import argparse, json, struct, zlib

SHAPE = {'4': 0, '22': 1, '211': 2, '112': 3, '1111': 4}
KIND = {'RAW': 1, 'MIO0': 2, 'LITERAL': 3, 'RELOCS': 4, 'UNPACK': 5}
XF = {'id': 0, 'sw16': 1, 'sw32': 2}

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('json'); ap.add_argument('assets'); ap.add_argument('-o', required=True)
    ap.add_argument('--stamp', default=None, help='hex build stamp of the executable this table belongs to (handheld: build_stamp.c)')
    a = ap.parse_args()
    js = json.load(open(a.json))
    d = open(a.assets, 'rb').read()
    magic, ver, addr, size, nrel, nsym, nnames, bid, crc, pad = struct.unpack_from('<8sIIIIIIIII', d, 0)
    assert size == js['region_size'], 'assets.bin region size differs from the recipes'
    # The recipes record region OFFSETS: any relink that moves a symbol (new
    # asset, compiler or flag change) needs `assets-recipes` again. Same size
    # but a different layout would otherwise pass here and fail the CRC on the
    # device with a misleading "wrong ROM?".
    if 'build_id' in js and js['build_id'] != bid:
        raise SystemExit('recipes are for another asset layout (build id %08x, assets.bin %08x): run assets-recipes' % (js['build_id'], bid))
    relocs = list(struct.unpack_from('<%dI' % nrel, d, 44))
    p = pad if pad else (44 + nrel * 4 + nsym * 16 + nnames + 15) & ~15
    data = bytearray(d[p:p + size])
    rel_entries = []
    for off in relocs:
        val = struct.unpack_from('<I', data, off)[0]
        rel_entries.append((off, val - addr) if addr <= val <= addr + size else (off | 0x80000000, val))  # flag on the offset: the value may use bit 31
        data[off:off + 4] = b'\0\0\0\0'
    data_crc = zlib.crc32(bytes(data)) & 0xffffffff

    # patterns: runs of word shapes, split into u16 entries of up to 8191 words
    runs = []; pat_entries = []; pat_words = []
    for pat in js['patterns']:
        first = len(runs); words = 0
        for shape, count in pat:
            words += count
            while count > 0:
                c = min(count, 8191); runs.append(SHAPE[shape] << 13 | c); count -= c
        pat_entries.append((words, first, len(runs) - first)); pat_words.append(words)

    # recipes: resolve literals, then merge runs with the same source/transform
    lit = bytearray(); recs = []
    for r in js['recipes']:
        kind = KIND[r['kind']]; xf = r['xform']
        x = 16 + int(xf[3:]) if xf.startswith('pat') else XF[xf]
        if r['kind'] == 'LITERAL':
            src = len(lit); lit += bytes.fromhex(r['extra']); extra = 0
        elif r['kind'] == 'RAW':
            src = r['extra']; extra = 0
        else:
            src = r['src']; extra = r['extra']
        recs.append([r['off'], r['size'], kind, x, src, extra])
    recs.sort(key=lambda r: (r[2] == 2 and r[4] or 0, r[0]))  # MIO0 grouped by block, else by dst
    merged = []
    for r in recs:
        m = merged[-1] if merged else None
        if m and m[2] == r[2] and m[3] == r[3] and m[2] in (1, 2, 3) and m[0] + m[1] == r[0] \
           and ((m[2] == 1 and m[4] + m[1] == r[4]) or (m[2] == 2 and m[4] == r[4] and m[5] + m[1] == r[5]) or (m[2] == 3 and m[4] + m[1] == r[4])) \
           and (m[3] < 16 or (m[1] % (4 * pat_words[m[3] - 16])) == 0):
            m[1] += r[1]
        else:
            merged.append(list(r))
    blocks = {b['rom_off']: b for b in js['blocks']}
    out = bytearray()
    out += struct.pack('<8sIIIIIIIIII', b'SM64RCP1', size, data_crc, len(merged), len(lit), len(pat_entries),
                       len(runs), len(js['blocks']), len(rel_entries),
                       max([b['decomp_len'] for b in js['blocks']] or [0]), max(pat_words or [0]))
    for r in merged:
        out += struct.pack('<IIHHII', r[0], r[1], r[2], r[3], r[4], r[5])
    out += lit; out += b'\0' * (-len(out) % 4)
    for words, first, count in pat_entries:
        out += struct.pack('<III', words, first, count)
    out += struct.pack('<%dH' % len(runs), *runs); out += b'\0' * (-len(out) % 4)
    for b in js['blocks']:
        out += struct.pack('<III', b['rom_off'], b['rom_len'], b['decomp_len'])
    for off, t in rel_entries:
        out += struct.pack('<II', off, t)
    # trailer: a cache id the game ties assets.bin to. It covers the layout id,
    # the region address and every pointer VALUE (a code-only change moves the
    # pointers into code), so a cache written by any other build of the game
    # is regenerated from the ROM, never loaded.
    cache_id = zlib.crc32(struct.pack('<III', bid, addr, size) + b''.join(struct.pack('<II', o, t) for o, t in rel_entries)
                          + struct.pack('<I', data_crc)) & 0xffffffff
    out += b'BLID' + struct.pack('<I', cache_id)
    # + the executable's build stamp: the game refuses a table from any other link
    if a.stamp:
        out += b'STMP' + struct.pack('<I', int(a.stamp, 16) & 0xffffffff)
    open(a.o, 'wb').write(out)
    print('wrote %s: %d bytes (%d recipes merged from %d, %d patterns in %d runs, %d literal bytes, %d relocs, %d outside), data crc %08X, stamp %s' % (
        a.o, len(out), len(merged), len(recs), len(pat_entries), len(runs), len(lit), len(rel_entries),
        sum(1 for o, t in rel_entries if o & 0x80000000), data_crc, a.stamp or '-'))

if __name__ == '__main__':
    main()
