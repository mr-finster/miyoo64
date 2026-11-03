#!/usr/bin/env python3
"""Build a fresh native PAL binary/recipe pair without reusing US recipes.

Run from paper-mario mounted at /work in the documented offline pm64hh:decomp image (includes the ARM toolchain and PyYAML).
The pinned decomp must already have been configured and split for PAL.
--plan prints commands without reading a ROM or running a compiler.
"""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import subprocess

PAL_SHA1 = '2111d39265a317414d359e35a7d971c4dfa5f9e1'
B = 'build/game/pal/miyoo'


def commands(rom, cache_tag="<new-stamp>"):
    return [
        ['python3', '-B', 'tools/port/rom_segments.py', '--region', 'pal'],
        ['python3', '-B', 'tools/port/gen_build.py', 'handheld', '--region', 'pal'],
        ['python3', '-B', 'tools/port/gen_build.py', 'miyoo', '--region', 'pal'],
        ['make', '-B', '-f', 'build/game/pal/handheld_game.mk', '-j2'],
        ['ninja', '-f', 'build/game/pal/miyoo.ninja', '-j2', '/work/'+B+'/pm64', '/work/'+B+'/pm64_assets.elf'],
        ['python3', '-B', 'tools/assets/make_assets.py', B+'/pm64_assets.elf', B+'/pm64', '-o', B+'/assets.bin', '--page', '4096'],
        ['python3', '-B', 'tools/assets/derive_recipes.py', B+'/assets.bin', rom, '-o', B+'/recipes-derived.json', '--report', B+'/derive-report.txt', '--cache', B+'/derive-'+cache_tag+'.pkl'],
    ]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', required=True)
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args(argv)
    jobs = commands(args.rom)
    if args.plan:
        print(json.dumps({'commands': jobs, 'stamp': 'new nonzero random stamp after generation',
                          'emit': ['python3', '-B', 'tools/assets/emit_recipes.py', B+'/recipes-derived.json', B+'/assets.bin', '-o', B+'/recipes.bin', '--stamp', '<new stamp>'],
                          'note': 'Fresh derivation only; recipes and assets stay git-ignored. Run pal_release_gates.py on the final ZIP.'}, indent=2))
        return 0
    root = Path(__file__).resolve().parents[2]
    if root.resolve() != Path('/work').resolve():
        parser.error('mount the paper-mario source directory at /work')
    rom = Path(args.rom)
    if rom.stat().st_size != 67108864 or hashlib.sha1(rom.read_bytes()).hexdigest() != PAL_SHA1:
        parser.error('require the canonical big-endian PAL ROM; do not convert it implicitly')
    stamp = f'{secrets.randbelow(0xffffffff)+1:08X}'
    jobs = commands(args.rom, stamp)
    if (root/B/('derive-'+stamp+'.pkl')).exists():
        parser.error('fresh derivation cache already exists; rerun for a new identity')
    for i, command in enumerate(jobs):
        if i == 3:
            (root/B/'build_stamp.c').write_text('#include <stdint.h>\nconst uint32_t port_build_stamp = 0x'+stamp+';\n')
        subprocess.run(command, cwd=root, check=True)
    subprocess.run(['python3', '-B', 'tools/assets/emit_recipes.py', B+'/recipes-derived.json', B+'/assets.bin', '-o', B+'/recipes.bin', '--stamp', stamp], cwd=root, check=True)
    print('New PAL pair built with stamp '+stamp+'. Package and pass all release gates before distribution; no hardware validation is implied.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
