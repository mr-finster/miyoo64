# ROM inspection helpers for The New Tetris (scratch: reads the player's ROM; nothing ROM-derived is kept)
import struct, sys
_ROMFILE = __import__("os").environ.get("TNT_ROMFILE")  # or set tntrom.ROM after import (tools/tnt_inflate.py does)
ROM = open(_ROMFILE, "rb").read() if _ROMFILE else b""
def u16(b,o): return struct.unpack_from('>H',b,o)[0]
def u32(b,o): return struct.unpack_from('>I',b,o)[0]

def lzo1x(s):
    o = bytearray(); ip = 0
    def cp(dist, n):
        p = len(o) - dist
        for i in range(n): o.append(o[p + i])
    def ext(t, base):
        nonlocal ip
        while s[ip] == 0: t += 255; ip += 1
        t += base + s[ip]; ip += 1; return t
    state = 'loop'
    if s[0] > 17:
        t = s[0] - 17; ip = 1
        if t < 4: state = 'match_next'
        else: o.extend(s[ip:ip+t]); ip += t; state = 'first'
    while True:
        if state == 'loop':
            t = s[ip]; ip += 1
            if t >= 16: state = 'match'; continue
            if t == 0: t = ext(0, 15)
            o.extend(s[ip:ip+t+3]); ip += t + 3; state = 'first'; continue
        if state == 'first':
            t = s[ip]; ip += 1
            if t >= 16: state = 'match'; continue
            dist = 1 + 0x800 + (t >> 2) + (s[ip] << 2); ip += 1
            cp(dist, 3); state = 'done'; continue
        if state == 'match':
            if t >= 64:
                dist = 1 + ((t >> 2) & 7) + (s[ip] << 3); ip += 1; cp(dist, (t >> 5) + 1); state = 'done'; continue
            elif t >= 32:
                t &= 31
                if t == 0: t = ext(0, 31)
                dist = 1 + ((s[ip] | (s[ip+1] << 8)) >> 2); ip += 2
            elif t >= 16:
                d = (t & 8) << 11; t &= 7
                if t == 0: t = ext(0, 7)
                d += (s[ip] | (s[ip+1] << 8)) >> 2; ip += 2
                if d == 0: return bytes(o)
                dist = d + 0x4000
            else:
                dist = 1 + (t >> 2) + (s[ip] << 2); ip += 1; cp(dist, 2); state = 'done'; continue
            cp(dist, t + 2); state = 'done'; continue
        if state == 'done':
            t = s[ip-2] & 3
            if t == 0: state = 'loop'; continue
            state = 'match_next'; continue
        if state == 'match_next':
            o.extend(s[ip:ip+t]); ip += t; t = s[ip]; ip += 1; state = 'match'; continue

def lut(base, i):
    n = u16(ROM, base)
    a = u32(ROM, base + 2 + 4*i); b = u32(ROM, base + 2 + 4*(i+1))
    magic = ROM[base+a:base+a+4]; size = u32(ROM, base+a+4)
    raw = ROM[base+a:base+b]
    if magic[3:4] == b'S': data = lzo1x(raw[8:])
    else: data = raw[8:]
    return n, magic, size, data

LUTS = dict(image=0x273A00, anim=0x5206B0, dcm=0x5B9AB0, sample=0x7C1960)
if __name__ == '__main__':
    name = sys.argv[1]; base = LUTS[name]
    n = u16(ROM, base)
    for i in range(int(sys.argv[2]) if len(sys.argv)>2 else n):
        try:
            _, m, sz, d = lut(base, i)
            print(i, m, hex(sz), len(d), d[:16].hex())
        except Exception as e:
            print(i, 'ERR', e)

FXLUT = 0xA8190; FXDATA = 0xADAE0
def fx_count(): return struct.unpack_from('>i', ROM, FXLUT)[0]
def fx_entry(i):
    t, c, cl, dl, off = struct.unpack_from('>hhiii', ROM, FXLUT + 8 + 16*i)
    raw = ROM[FXDATA+off:FXDATA+off+cl]
    data = lzo1x(raw) if c else raw
    return t, c, cl, dl, off, data
