#!/usr/bin/env python3
"""Release gate: prove the files that ship contain no ROM game data.

Diddy Kong Racing keeps all of its game data in one region of the cartridge:
the asset LUT and the asset sections (us.v80: 0xED0E0..0xB8CFD0). The port
ships none of it; the device copies it out of the player's ROM at first boot.

Index: every 4-byte-aligned 32-byte window of
  * the raw asset region, as-is and 16-/32-bit byte-swapped (the port swaps
    assets to host order after loading them);
  * every rzip-compressed asset inflated (Rare's rzip = 4-byte LE size, the
    byte 0x09, raw DEFLATE), found by signature, again in the three byte
    orders.
Then every 32-byte-aligned window of each shipped file is looked up. Windows of
padding, low entropy (fewer than 6 distinct byte values) or plain counting
sequences are skipped. Any hit
is a LEAK and fails the gate.

Hits against the ROM's CODE region (0x1000..LUT) are reported for the record
but do not fail: that is where the decompiled C's own data (tables and
literals written out in decomp/src) comes from. --code-budget-kb caps them.

usage: leak_scan.py --rom baserom.z64 [--code-budget-kb N] file...
"""
import argparse, re, struct, sys, zlib

WIN = 32
LUT_START, ASSETS_END = 0xED0E0, 0xB8CFD0  # us.v80
CODE_START = 0x1000

def swap16(b):
    a = bytearray(b); n = len(b) // 2 * 2
    a[0:n:2], a[1:n:2] = b[1:n:2], b[0:n:2]
    return bytes(a)

def swap32(b):
    a = bytearray(b); n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4], a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4], b[1:n:4], b[0:n:4]
    return bytes(a)

def _progression(vals):
    d = vals[1] - vals[0]
    return all(vals[i + 1] - vals[i] == d for i in range(len(vals) - 1))

def interesting(w):
    """Enough entropy to mean something: at least 6 distinct byte values, and
    not a plain counting sequence (e.g. an index table 14, 15, 16 ... as s16),
    which matches somewhere in 11 MB by construction, not by copying."""
    if len(set(w)) < 6:
        return False
    for fmt in ('<16H', '>16H', '<8I', '>8I'):
        if _progression(struct.unpack(fmt, w)):
            return False
    return True

def add_windows(index, blob, tag):
    for i in range(0, len(blob) - WIN + 1, 4):
        w = blob[i:i + WIN]
        if interesting(w):
            index.setdefault(w, (tag, i))

def section_names(enums_h):
    txt = open(enums_h).read()
    body = txt[txt.index('typedef enum AssetSectionsEnum'):]
    body = body[body.index('{') + 1:body.index('}')]
    return [n.strip() for n in body.split(',') if n.strip()]

def inflated_assets(rom):
    """Every rzip stream in the asset region: 4-byte LE size, the byte 0x09,
    then raw DEFLATE. Found by trying every position whose fifth byte is 0x09
    and whose size is plausible (a cheap partial inflate weeds out the rest),
    so no knowledge of the section formats is needed."""
    out = []
    p = LUT_START
    end = ASSETS_END
    while True:
        p = rom.find(b'\x09', p + 1, end)
        if p < 0:
            break
        q = p - 4
        if q < LUT_START:
            continue
        size = struct.unpack('<I', rom[q:q + 4])[0]
        if not (32 <= size <= 0x200000):
            continue
        try:
            head = zlib.decompressobj(-15).decompress(rom[p + 1:p + 1 + 512], 64)
        except zlib.error:
            continue
        if len(head) < min(64, size):
            continue
        try:
            d = zlib.decompressobj(-15).decompress(rom[p + 1:end], size)
        except zlib.error:
            continue
        if len(d) == size:
            out.append(('rzip', q, d))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--enums', default='decomp/include/asset_enums.h')
    ap.add_argument('--code-budget-kb', type=int, default=48)
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()
    rom = open(a.rom, 'rb').read()
    assets = {}
    region = rom[LUT_START:ASSETS_END]
    for f, tag in ((lambda b: b, 'asset'), (swap16, 'asset/sw16'), (swap32, 'asset/sw32')):
        add_windows(assets, f(region), tag)
    inflated = inflated_assets(rom)
    for name, off, d in inflated:
        for f, tag in ((lambda b: b, ''), (swap16, '/sw16'), (swap32, '/sw32')):
            add_windows(assets, f(d), 'inflated %s+%X%s' % (name, off, tag))
    code = {}
    add_windows(code, rom[CODE_START:LUT_START], 'code')
    print('leak_scan: indexed %d asset windows (%d inflated assets), %d code windows'
          % (len(assets), len(inflated), len(code)))
    leaks, code_hits = 0, 0
    for path in a.files:
        data = open(path, 'rb').read()
        for i in range(0, len(data) - WIN + 1, WIN):
            w = data[i:i + WIN]
            if not interesting(w):
                continue
            if w in assets:
                leaks += 1
                if leaks <= 20:
                    print('  LEAK %s+0x%X matches %s at 0x%X' % (path, i, *assets[w]))
            elif w in code:
                code_hits += 1
    print('leak_scan: %d leak windows, %d code-region windows (%.1f KB, budget %d KB)'
          % (leaks, code_hits, code_hits * WIN / 1024, a.code_budget_kb))
    if leaks:
        print('FAIL: shipped files contain ROM asset data')
        return 1
    if code_hits * WIN > a.code_budget_kb * 1024:
        print('FAIL: code-region matches over budget')
        return 1
    print('PASS')
    return 0

if __name__ == '__main__':
    sys.exit(main())
