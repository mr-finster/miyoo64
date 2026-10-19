#!/usr/bin/env python3
"""hm64_pack.py: the data pack the native port reads in place of the cartridge (hh_host.c, nuPiReadRom).

The pack is the ROM, byte for byte, except the game's bytecode and text: the cutscene and dialogue bytecode and the
text banks and their indices, which the decomp builds from assembly (assets/cutscenes, assets/dialogues,
assets/text, transpiled from src/bytecode and the ROM). Each of those is assembled again for ARM, so its halfwords
and words are little-endian as the native code reads them, and linked at its N64 address against the native
executable, so the addresses of game variables in the bytecode (npcAffection, gCutsceneIndex, ...) are the
executable's. Every file must keep the N64 file's size (checked). Sprites, maps, fonts and audio stay as the ROM
has them; the code reads their multi-byte fields big-endian (decomp.patch, BE16/BE32) and the renderer takes the
textures in the N64's byte order.

  hm64_pack.py --rom hm64.z64 --n64-map hm64.map --native build/hh/hm64 --syms build/hh/hm64_syms.ld \
               --out build/hh/hm64-pack.bin --work build/hh/pack

The pack is ROM-derived: local only, never in git or a zip.
"""
import argparse, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

TC = os.environ.get('MIYOO_TOOLCHAIN', '/opt/miyoomini-toolchain') + '/usr/bin/arm-linux-gnueabihf-'
DIRS = ('assets/cutscenes', 'assets/dialogues', 'assets/text')
OBJ_RE = re.compile(r'^ \.data\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) build/(assets/(?:cutscenes|dialogues/bytecode|text))/(\w+)\.bin\.o$')


def n64_symbols(elf):
    out = subprocess.run(['mips-linux-gnu-nm', elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return syms


def arm_source(src):
    """The decomp's MIPS-flavoured data assembly as ARM gas takes it: '#' comments dropped, .half -> .hword."""
    out = []
    for line in open(src):
        i = line.find('#')
        if i >= 0:
            line = line[:i]
        line = re.sub(r'^(\s*)\.half\b', r'\1.hword', line.rstrip())
        out.append(line)
    return '\n'.join(out) + '\n'


def build_one(job):
    name, src, vram, size, work, native, syms_ld = job
    s_path = os.path.join(work, name + '.s')
    o_path = os.path.join(work, name + '.o')
    e_path = os.path.join(work, name + '.elf')
    b_path = os.path.join(work, name + '.bin')
    open(s_path, 'w').write(arm_source(src))
    subprocess.run([TC + 'as', '-o', o_path, s_path], check=True)
    # at its N64 address; game variables from the native executable, segment symbols from hm64_syms.ld
    subprocess.run([TC + 'ld', '-o', e_path, '--section-start=.data=0x%08X' % vram, '-e', '0', '-R', native, o_path,
                    syms_ld], check=True)
    subprocess.run([TC + 'objcopy', '-O', 'binary', '-j', '.data', e_path, b_path], check=True)
    data = open(b_path, 'rb').read()
    for p in (s_path, o_path, e_path, b_path):
        os.remove(p)
    if len(data) < size:
        data += b'\0' * (size - len(data))
    return name, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--n64-map', required=True)
    ap.add_argument('--n64-elf', default='hm64.elf')
    ap.add_argument('--native', required=True)
    ap.add_argument('--syms', default='build/hh/hm64_syms.ld')
    ap.add_argument('--out', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('-j', type=int, default=4)
    a = ap.parse_args()

    rom = bytearray(open(a.rom, 'rb').read())
    syms = n64_symbols(a.n64_elf)
    sources = {}
    for d in DIRS:
        for f in os.listdir(d):
            if f.endswith('.s'):
                sources[f[:-2]] = os.path.join(d, f)

    os.makedirs(a.work, exist_ok=True)
    jobs = []
    for line in open(a.n64_map):
        m = OBJ_RE.match(line.rstrip())
        if not m:
            continue
        vram, size, name = int(m.group(1), 16), int(m.group(2), 16), m.group(4)
        if size == 0:
            continue
        if name not in sources:
            sys.exit(f'hm64_pack: no assembly for {name}')
        seg_start, seg_rom = syms.get(f'_{name}SegmentStart'), syms.get(f'_{name}SegmentRomStart')
        if seg_start is None or seg_rom is None or syms.get(f'_{name}SegmentDataStart') != vram:
            sys.exit(f'hm64_pack: no segment for {name}')
        rom_addr = seg_rom + (vram - seg_start)
        jobs.append((name, sources[name], vram, size, a.work, a.native, a.syms, rom_addr))

    places = {j[0]: (j[7], j[3]) for j in jobs}
    with ThreadPoolExecutor(a.j) as ex:
        results = list(ex.map(build_one, [j[:7] for j in jobs]))
    changed = 0
    for name, data in results:
        rom_addr, size = places[name]
        if len(data) != size:
            sys.exit(f'hm64_pack: {name}: {len(data)} bytes for ARM, {size} on the N64')
        if rom[rom_addr:rom_addr + size] != data:
            changed += 1
        rom[rom_addr:rom_addr + size] = data
    open(a.out, 'wb').write(rom)
    print(f'hm64_pack: {len(results)} bytecode/text files re-assembled for ARM ({changed} differ from the ROM), '
          f'{len(rom) // 1024} KB -> {a.out}')


if __name__ == '__main__':
    main()
