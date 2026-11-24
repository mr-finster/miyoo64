#!/usr/bin/env python3
"""Release gate: prove the files that ship contain no ROM game data.

Diddy Kong Racing keeps all of its game data in one region of the cartridge:
the asset LUT and the asset sections (us.v80: 0xED0E0..0xB8CFD0). The port
ships none of it; the device copies it out of the player's ROM at first boot.

Index the interesting 32-byte-aligned windows of the shipped files. Search
all 4-byte-aligned windows of the raw asset region and each inflated rzip
asset in all three byte orders, then search the code region. Windows of
padding, low entropy (fewer than 6 distinct byte values) or plain counting
sequences are skipped. Any asset hit is a LEAK and fails the gate.

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

def match_windows(index, blob, tag):
    """Record the first ROM location for each indexed package window."""
    for i in range(0, len(blob) - WIN + 1, 4):
        w = blob[i:i + WIN]
        if w in index and index[w] is None:
            index[w] = (tag, i)

def file_windows(path):
    """Read aligned package windows without retaining the entire file."""
    with open(path, 'rb') as f:
        while True:
            w = f.read(WIN)
            if len(w) < WIN:
                return
            if interesting(w):
                yield w

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
            yield 'rzip', q, d

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--version', choices=('us_v80', 'pal_v80'), default='us_v80')
    ap.add_argument('--enums', default='decomp/include/asset_enums.h')
    ap.add_argument('--code-budget-kb', type=int, default=48)
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()
    global LUT_START, ASSETS_END
    if a.version == 'pal_v80':
        LUT_START, ASSETS_END = 0xED170, 0xB8D060
    rom = open(a.rom, 'rb').read()
    # Only windows present in the package need to be indexed. A None value
    # means no ROM match yet; retain the first match in the old scan order.
    windows = {}
    for path in a.files:
        for w in file_windows(path):
            windows.setdefault(w, None)
    region = rom[LUT_START:ASSETS_END]
    for f, tag in ((lambda b: b, 'asset'), (swap16, 'asset/sw16'), (swap32, 'asset/sw32')):
        match_windows(windows, f(region), tag)
    del region
    inflated_count = 0
    for name, off, d in inflated_assets(rom):
        inflated_count += 1
        for f, tag in ((lambda b: b, ''), (swap16, '/sw16'), (swap32, '/sw32')):
            match_windows(windows, f(d), 'inflated %s+%X%s' % (name, off, tag))
    match_windows(windows, rom[CODE_START:LUT_START], 'code')
    print('leak_scan: indexed %d distinct package windows (%d inflated assets)'
          % (len(windows), inflated_count))
    leaks, code_hits = 0, 0
    for path in a.files:
        # Keep original file offsets even when uninteresting windows are skipped.
        with open(path, 'rb') as f:
            i = 0
            while True:
                w = f.read(WIN)
                if len(w) < WIN:
                    break
                match = windows.get(w)
                if match is not None:
                    if match[0] == 'code':
                        code_hits += 1
                    else:
                        leaks += 1
                        if leaks <= 20:
                            print('  LEAK %s+0x%X matches %s at 0x%X' % (path, i, *match))
                i += WIN
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
