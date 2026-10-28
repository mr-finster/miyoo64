#!/usr/bin/env python3
"""gen_build.py -- the native build of Paper Mario's own code (M2), generated from the decomp's split.

Reads pmret/papermario's generated linker script (refs/papermario/ver/us/papermario.ld: every segment, its VRAM window
and its objects in link order) and build.ninja (each object's source and flags), and writes build/game/<plat>.ninja:
every game C file compiled natively, in the decomp's own pipeline (cpp | iconv UTF-8 -> CP932 | cc), with our shim
headers first on the include path. The N64-only parts are left out and replaced by game/port/: libultra and nusys
(src/os), the boot code, the assembly files, and the dead maps.

  python3 tools/port/gen_build.py [pc|handheld|miyoo]
  make -f build/game/pc_game.mk -j12 -k           the game's C on the Mac (clang for i686-linux: the decomp needs no
                                                  system headers, and the shared Docker VM is slow)
  ./dev.sh ninja -f build/game/pc.ninja           the host side (system headers, the renderer kit) and the link

Writes build/game/segments.json too: each segment's name, VRAM, class and objects, for the overlay tables.

miyoo (the Miyoo Mini, README_MIYOO.md): the game side is the handheld's, object for object (build/game/handheld/: the
same compile, so the same asset region and recipes), linked with the host side built by the Mini's toolchain
(MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain: gcc 8.3, glibc 2.28, the device's SDL 1.2) with MIYOO defined
beside HANDHELD. Build the handheld game side first (make -f build/game/handheld_game.mk), then ninja -f build/game/miyoo.ninja.
Nothing here reads the ROM; the split itself (./configure us in the pm64hh:decomp image) is local and git-ignored.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DECOMP = os.path.join(ROOT, 'refs', 'papermario')
C_ROOT = '/work'                     # the repo inside the build container (./dev.sh)
C_DECOMP = C_ROOT + '/refs/papermario'

# The game side on the Mac: Apple clang emits ELF for the port's targets, and the decomp's C needs only its own headers.
MAC_CC = {'pc': 'clang -target i686-linux-gnu -msse2 -mfpmath=sse -malign-double -fno-pic -fno-pie',
          'handheld': 'clang -target armv7a-unknown-linux-musleabihf -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard '
                    '-marm -fno-pic -fno-pie'}
# PM64_SAN="-fsanitize=address,bounds,object-size -fsanitize-recover=address" python3 tools/port/gen_build.py pc: the crash
# sweep's desktop build (a clean build/game/pc). Globals are left unpadded so the data layout is kept.
PM64_SAN = os.environ.get('PM64_SAN', '')
if PM64_SAN:
    MAC_CC['pc'] += ' ' + PM64_SAN + ' -fno-omit-frame-pointer' + (' -mllvm -asan-globals=0' if 'address' in PM64_SAN else '')
MAC_CFLAGS = ('-O2 -g1 -funsigned-char -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -fno-common '
              '-fno-builtin-bcopy -w -Wno-error=incompatible-function-pointer-types -Wno-error=implicit-function-declaration '
              '-Wno-error=implicit-int -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=return-mismatch')

def die(msg):
    print('gen_build.py: ' + msg, file=sys.stderr)
    sys.exit(1)

def parse_ld(path):
    """Segments in link order: name, vram (None = follows the previous), objects per kind."""
    segs, cur = [], None
    sec_re = re.compile(r'^    \.([A-Za-z0-9_]+)(?: (0x[0-9A-Fa-f]+|\(NOLOAD\)))? :')
    obj_re = re.compile(r'^        (ver/us/build/\S+\.o)\((\.[a-z]+)\*?\);')
    for line in open(path):
        m = sec_re.match(line)
        if m:
            name, addr = m.group(1), m.group(2)
            if name.endswith('_bss') and addr == '(NOLOAD)':
                cur = next((s for s in reversed(segs) if s['name'] == name[:-4]), None)
                if cur is None:
                    cur = {'name': name[:-4], 'vram': None, 'objs': []}
                    segs.append(cur)
                continue
            cur = {'name': name, 'vram': int(addr, 16) if addr and addr.startswith('0x') else None, 'objs': []}
            segs.append(cur)
            continue
        m = obj_re.match(line)
        if m and cur is not None:
            obj = m.group(1)
            if obj not in cur['objs']:
                cur['objs'].append(obj)
    return segs

def parse_ninja(path):
    """Object -> (rule, source, cppflags, cflags) for every object build statement."""
    objs, cur = {}, None
    build_re = re.compile(r'^build (\S+\.o): (\S+) (\S+)')
    for line in open(path):
        m = build_re.match(line)
        if m:
            cur = {'rule': m.group(2), 'src': m.group(3), 'cppflags': '', 'cflags': ''}
            objs[m.group(1)] = cur
            continue
        if cur is not None and line.startswith('  '):
            k, _, v = line.strip().partition(' = ')
            if k in ('cppflags', 'cflags'):
                cur[k] = v
        elif not line.startswith('  '):
            cur = None
    return objs

# Segment classes by VRAM window (ver/us/splat.yaml vram_classes): overlays share a window on the N64.
WINDOWS = {0x80240000: 'map', 0x80218000: 'battle_area', 0x802A1000: 'battle_move', 0x80234000: 'battle_partner',
           0x80238000: 'battle_star', 0x802A9000: 'world_item', 0x802B6000: 'world_action', 0x802BD100: 'world_partner',
           0x09000000: 'effect_gfx', 0x0A000000: 'entity_model'}

def seg_class(seg):
    v = seg['vram']
    if v is None:
        return 'follow'
    if 0xE0000000 <= v < 0xE1000000:
        return 'effect'
    return WINDOWS.get(v, 'fixed')

def write_port_effects(out_dir):
    """build/game/port_effects.c: the fx_* entry points. On the N64 each is a stub (asm/effects/*.s) that loads the
    effect (load_effect, TLB + DMA) and jumps to its main function through gEffectTable; the port calls it.
    From the decomp's generated effect headers (src/effects.yaml), not the ROM."""
    fxdir = os.path.join(DECOMP, 'assets', 'us', 'effects')
    macros = {}
    for m in re.finditer(r'^#define EFFECT_DEF_(\w+)\(func_name\) (.+?) func_name\((.*)\)$',
                         open(os.path.join(fxdir, 'effect_macros.h')).read(), re.M):
        macros[m.group(1)] = (m.group(2), [a.strip() for a in m.group(3).split(',') if a.strip() and a.strip() != 'void'])
    defs_h = open(os.path.join(fxdir, 'effect_defs.h')).read()
    enum = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'^    (EFFECT_\w+) = 0x([0-9A-Fa-f]+),', defs_h, re.M)}
    out = ['/* generated by tools/port/gen_build.py from the decomp\'s effect headers: the fx_* entry points */',
           '#include "common.h"', '#include "effects.h"', '', 's32 load_effect(s32 effectIndex);',
           'extern EffectTableEntry gEffectTable[];', '']
    n = 0
    for m in re.finditer(r'^EFFECT_DEF_(\w+)\((fx_\w+)\);', defs_h, re.M):
        key, fx = m.group(1), m.group(2)
        ret, args = macros[key]
        idx = enum.get('EFFECT_' + key, enum.get(key))
        if idx is None:
            die(f'no effect index for {key}')
        params = ', '.join(f'{t} a{i}' for i, t in enumerate(args)) or 'void'
        names = ', '.join(f'a{i}' for i in range(len(args)))
        types = ', '.join(args) or 'void'
        call = f'(({ret} (*)({types})) gEffectTable[0x{idx:02X}].entryPoint)({names})'
        out += [f'{ret} {fx}({params}) {{', f'    load_effect(0x{idx:02X});',
                f'    {"return " if ret != "void" else ""}{call};', '}']
        n += 1
    with open(os.path.join(out_dir, 'port_effects.c'), 'w') as f:
        f.write('\n'.join(out) + '\n')
    return n

def write_asset_wrappers():
    """build/game/assetwrap/<path>: for every .inc.c the decomp's split generates from the ROM (vertices, display lists),
    a wrapper found first on the include path that puts its data in .assetdata (#include_next the real one)."""
    root = os.path.join(DECOMP, 'assets', 'us')
    out_root = os.path.join(ROOT, 'build', 'game', 'assetwrap')
    n = 0
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if not f.endswith('.inc.c'):
                continue
            rel = os.path.relpath(os.path.join(dp, f), root)
            dst = os.path.join(out_root, rel)
            text = f'#include "port_asset_section.h"\n#include_next "{rel}"\n#include "port_asset_section_end.h"\n'
            if not os.path.exists(dst) or open(dst).read() != text:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, 'w').write(text)
            n += 1
    return n

# Generated whole files that are ROM-derived (the rest of assets/us that compiles, effect_table.c, comes from the
# decomp's own effects.yaml): their data goes to .assetdata too.
ASSET_WHOLE_FILES = ('assets/us/imgfx/imgfx_data.c',)

def write_asset_ld(out_dir):
    """The asset region after .bss: NOLOAD in the handheld's shipped link (the device rebuilds it from the ROM), PROGBITS
    for the desktop proxy and for the handheld's second link that make_assets.py reads."""
    for kind, attr in (('noload', ' (NOLOAD)'), ('progbits', '')):
        with open(os.path.join(out_dir, f'assets_{kind}.ld'), 'w') as f:
            f.write('/* generated by tools/port/gen_build.py: the ROM-derived data (tools/assets) */\n'
                    'SECTIONS {\n'
                    f'  .assetdata{attr} : ALIGN(4096) {{ __assets_start = .; *(.assetdata .assetdata.*) '
                    '*(.assetrodata .assetrodata.*) . = ALIGN(4096); __assets_end = .; }\n'
                    '} INSERT AFTER .bss;\n')

# The port's host side, whole, in the "dontsave" section (hh_game.h): a save state is .data and .bss from
# __data_start to the asset region minus that section, so a load restores the game (the decomp and the game-side port
# files: the OS threads, nusys, the audio manager) and never the screen, the sound ring, the renderer, the save file,
# the asset loader or the kit's own files. port_state.c is not here: its staged pages are state (it tags the rest).
DONTSAVE_OBJS = ('port/port_host.o', 'port/port_rom.o', 'port/port_gfx.o', 'port/port_input.o', 'port/port_segments.o',
                 'port/port_sdl.o', 'port/port_assets_hh.o', 'port/port_mixer.o', 'port/port_audio_out.o', 'port/port_hh.o',
                 'port/build_stamp.o', 'port/port_segtab.o', 'kit/*.o', 'hhkit/*.o')

def write_dontsave_ld(out_dir):
    rules = ' '.join(f'*/{o}(.data .data.* .bss .bss.* COMMON)' for o in DONTSAVE_OBJS)
    with open(os.path.join(out_dir, 'dontsave.ld'), 'w') as f:
        f.write('/* generated by tools/port/gen_build.py: the port\'s host side, outside the save states (hh_game.h) */\n'
                f'SECTIONS {{\n  dontsave : ALIGN(64) {{ *(dontsave) {rules} . = ALIGN(64); }}\n}} INSERT AFTER .data;\n')

def write_mac_makefile(plat, units, defs):
    """build/game/<plat>_game.mk: the game side (the decomp's C and game/port's game-side files) on the Mac."""
    host_side = {'port_host.c', 'port_rom.c', 'port_gfx.c', 'port_input.c', 'port_segments.c', 'port_sdl.c', 'port_assets_hh.c',
                 'port_mixer.c', 'port_audio_out.c', 'port_hh.c', 'port_state.c'}
    incs = ' '.join(f'-I{p}' for p in (ROOT + '/game/include', DECOMP + '/ver/us/include',
                                         DECOMP + '/ver/us/build/include', DECOMP + '/include', DECOMP + '/src',
                                         DECOMP + '/assets/us'))
    cc = MAC_CC[plat]
    obj_root = f'{ROOT}/build/game/{plat}'
    out = [f'# generated by tools/port/gen_build.py {plat}: do not edit. make -f build/game/{plat}_game.mk -j12 -k',
           'SHELL := /bin/bash', '.SHELLFLAGS := -o pipefail -c', f'CC := {cc}', f'CFLAGS := {MAC_CFLAGS}',
           f'CPPFLAGS := -nostdinc {incs} {defs}', '',
           '# the decomp pipeline: cpp | iconv UTF-8 -> CP932 (the N64 strings) | cc',
           'define cc_game', '\t@mkdir -p $(@D)',
           f'\t@cd {DECOMP} && $(CC) -E -w $(CPPFLAGS) $(1) -MD -MF $@.d -MT $@ $< | iconv -f UTF-8 -t CP932 | '
           '$(CC) $(CFLAGS) -x c -c - -o $@ || { echo "FAILED: $<"; rm -f $@; exit 1; }',
           'endef', '', 'all:', '']
    objs = []
    for seg, o, info in units:
        src = info['src'] if info['src'].startswith('/') else DECOMP + '/' + info['src']
        src = src.replace(C_DECOMP, DECOMP)
        obj = obj_root + '/' + o[len('ver/us/build/'):]
        # every decomp file's data goes to the asset region: the decompiled game's data (scripts, tables) is the ROM's
        # too, so the device rebuilds all of it from the player's ROM (the release rule; tools/assets/leak_scan.py)
        out.append(f'{obj}: {src}\n\t$(call cc_game,{info["cppflags"]} -include port_asset_section.h)')
        objs.append(obj)
    for fn in sorted(os.listdir(os.path.join(ROOT, 'game', 'port'))):
        if fn.endswith('.c') and fn not in host_side:
            obj = f'{obj_root}/port/{fn[:-2]}.o'
            out.append(f'{obj}: {ROOT}/game/port/{fn}\n\t@mkdir -p $(@D)\n'
                       f'\t@cd {DECOMP} && $(CC) -std=gnu11 $(CPPFLAGS) -I{ROOT}/game/port $(CFLAGS) -MD -MF $@.d -c $< -o $@'
                       ' || { echo "FAILED: $<"; exit 1; }')
            objs.append(obj)
    gen = f'{ROOT}/build/game/port_effects.c'
    obj = f'{obj_root}/port/port_effects.o'
    out.append(f'{obj}: {gen}\n\t@mkdir -p $(@D)\n'
               f'\t@cd {DECOMP} && $(CC) -std=gnu11 $(CPPFLAGS) $(CFLAGS) -MD -MF $@.d -c $< -o $@ || {{ echo "FAILED: $<"; exit 1; }}')
    objs.append(obj)
    # every object's own .d: make's wildcard has no recursive **, so a glob only reached one directory level and a
    # header change never rebuilt src/pause/, src/world/... (the window-style bitfields, 2026-09-25)
    out += ['', 'all: ' + ' '.join(objs), '', '-include $(wildcard ' + ' '.join(o + '.d' for o in objs) + ')']
    with open(os.path.join(ROOT, 'build', 'game', f'{plat}_game.mk'), 'w') as f:
        f.write('\n'.join(out) + '\n')

def main():
    plat = sys.argv[1] if len(sys.argv) > 1 else 'pc'
    if plat not in ('pc', 'handheld', 'miyoo'):
        die('usage: gen_build.py [pc|handheld|miyoo]')
    ld = os.path.join(DECOMP, 'ver', 'us', 'papermario.ld')
    nj = os.path.join(DECOMP, 'build.ninja')
    for f in (ld, nj):
        if not os.path.isfile(f):
            die(f'missing {f}: run ./configure us in the pm64hh:decomp image first (README.md, "Building")')
    segs, objs = parse_ld(ld), parse_ninja(nj)
    if not segs or not objs:
        die('nothing parsed from the linker script or build.ninja')

    skip_src = re.compile(r'^(src/os/|src/boot/|src/gcc/|src/world/dead/|ver/us/)')  # src/gcc: MIPS libgcc (the host has its own)
    # libultra's portable C (matrix math, trig, VI mode tables) compiles as it is; the rest of src/os is hardware
    # and nusys, replaced by game/port/.
    portable_os = re.compile(r'^src/os/(gu[A-Za-z]*|perspective|position|rotateRPY|lookathil|lookatref|frustum|sins|coss|'
                             r'sqrtf|ldiv|vitbl|vimode[a-z0-9]*)\.c$')
    units, skipped = [], {'asm': 0, 'bin': 0, 'os/boot/dead': 0}
    for s in segs:
        s['class'] = seg_class(s)
        for o in s['objs']:
            info = objs.get(o)
            if info is None:
                continue
            if info['rule'] in ('as',):
                skipped['asm'] += 1
                continue
            if info['rule'] == 'bin':
                skipped['bin'] += 1
                continue
            if skip_src.match(info['src']) and not portable_os.match(info['src']):
                skipped['os/boot/dead'] += 1
                continue
            units.append((s['name'], o, info))

    out_dir = os.path.join(ROOT, 'build', 'game')
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'segments.json'), 'w') as f:
        json.dump([{'name': s['name'], 'vram': s['vram'], 'class': s['class'], 'objs': s['objs']} for s in segs], f, indent=0)

    if plat == 'pc':
        cc = 'gcc -m32 -fno-pie -malign-data=abi -malign-double -msse2 -mfpmath=sse'
        if PM64_SAN:
            cc += ' ' + PM64_SAN + ' -fno-omit-frame-pointer' + (' --param asan-globals=0' if 'address' in PM64_SAN else '')
    else:  # miyoo, and the handheld game side's own host half (the Miyoo links only its game objects)
        mtc = os.environ.get('MIYOO_TOOLCHAIN', '/opt/miyoomini-toolchain')
        cc = f'{mtc}/usr/bin/arm-linux-gnueabihf-gcc -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfloat-abi=hard'
    incs = ' '.join(f'-I{p}' for p in (C_ROOT + '/game/include', C_DECOMP + '/ver/us/include', C_DECOMP + '/ver/us/build/include',
                                         C_DECOMP + '/include', C_DECOMP + '/src', C_DECOMP + '/assets/us'))
    defs = ('-D_LANGUAGE_C -D_FINALROM -DVERSION=us -DVERSION_US -DF3DEX_GBI_2 -D_MIPS_SZLONG=32 -DMODERN_COMPILER '
            '-DSHIFT -DNON_MATCHING -DAVOID_UB -DPM64_PORT=1 ' + ('-DTARGET_PC=1' if plat == 'pc' else '-DTARGET_HANDHELD=1'))
    cflags = ('-O2 -g -funsigned-char -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off '
              '-fno-single-precision-constant -fno-common -fno-builtin-bcopy -w')
    obj_root = f'{C_ROOT}/build/game/{plat}'
    # the game side's objects: the Miyoo links the handheld's (built by build/game/handheld_game.mk), so its asset region is
    # the handheld's byte for byte and the handheld's recipes fit it
    game_root = f'{C_ROOT}/build/game/handheld' if plat == 'miyoo' else obj_root
    shared_game = plat == 'miyoo'
    nfx = write_port_effects(os.path.join(ROOT, 'build', 'game'))
    nwrap = 0  # write_asset_wrappers(): superseded by whole-file routing (every decomp file's data is an asset)
    write_asset_ld(os.path.join(ROOT, 'build', 'game'))
    write_dontsave_ld(os.path.join(ROOT, 'build', 'game'))
    if plat in MAC_CC:
        write_mac_makefile(plat, units, defs)
    lines = [f'# generated by tools/port/gen_build.py {plat}: do not edit', 'ninja_required_version = 1.3', '',
             f'rule cc', f'  command = cd {C_DECOMP} && {cc} -E -nostdinc {incs} {defs} $cppflags -MD -MF $out.d $in -o - '
             f'| iconv --from UTF-8 --to CP932 | {cc} {cflags} -x c -c - -o $out',
             '  depfile = $out.d', '  deps = gcc', '  description = cc $in', '']
    game_objs = []
    for seg, o, info in units:
        src = C_DECOMP + '/' + info['src'] if not info['src'].startswith('/') else info['src']
        out = game_root + '/' + o[len('ver/us/build/'):]
        if plat not in MAC_CC and not shared_game:
            lines.append(f'build {out}: cc {src}')
        game_objs.append(out)

    # The port layer (game/port): the game side takes the decomp's headers (as above, -std=gnu11 so the decomp's
    # bool stays its 4-byte enum); the host side takes system headers and the renderer kit's.
    host_side = {'port_host.c', 'port_rom.c', 'port_gfx.c', 'port_input.c', 'port_segments.c', 'port_sdl.c', 'port_assets_hh.c',
                 'port_mixer.c', 'port_audio_out.c', 'port_hh.c', 'port_state.c'}
    kit_inc = f'-I{C_ROOT}/bench/kit -I{C_ROOT}/kit/platform -I{C_DECOMP}/include'
    kit_defs = '-DTARGET_HANDHELD=1 -DPM64=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DPORT_NO_FPS=1 -DNON_MATCHING=1 -DAVOID_UB=1'
    kit_cflags = ('-std=gnu11 -O2 -g -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off '
                  '-fsingle-precision-constant -w')
    if plat == 'pc':
        kit_cflags += ' -DPM64_ASSETS_LINKED=1'  # the desktop proxy links the asset region's bytes in
    if plat == 'handheld':  # as the bench's device build: NEON, -O3, SDL 1.2
        kit_cflags = kit_cflags.replace('-O2 -g', '-O3 -mfpu=neon-vfpv4 -fomit-frame-pointer') + ' -DPM64_SDL12=1 -DHANDHELD=1 -D_REENTRANT'
    if plat == 'miyoo':  # the handheld host side with MIYOO beside HANDHELD; 64-bit file offsets (32-bit glibc's readdir
        # fails with EOVERFLOW on 64-bit inodes: the ROM search saw an empty folder, MK64's Miyoo port); the device's SDL
        # 1.2 from the sysroot (its sdl-config names the toolchain's build paths, so the flags are written out here)
        kit_cflags = (kit_cflags.replace('-O2 -g', '-O3 -mfpu=neon-vfpv4 -fomit-frame-pointer') +
                      ' -DPM64_SDL12=1 -DHANDHELD=1 -DMIYOO=1 -DTARGET_MIYOO=1 -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE=1 -D_REENTRANT')
    lines += ['rule cc_game_port',
              f'  command = cd {C_DECOMP} && {cc} -std=gnu11 {incs} -I{C_ROOT}/game/port {defs} {cflags} -MD -MF $out.d -c $in -o $out',
              '  depfile = $out.d', '  deps = gcc', '  description = cc $in',
              'rule cc_host',
              f'  command = {cc} {kit_inc} -I{C_ROOT}/game/port {kit_defs} {kit_cflags} -MD -MF $out.d -c $in -o $out',
              '  depfile = $out.d', '  deps = gcc', '  description = cc $in', '']
    port_objs = []
    for fn in sorted(os.listdir(os.path.join(ROOT, 'game', 'port'))):
        if not fn.endswith('.c'):
            continue
        if shared_game and fn not in host_side:  # the handheld's game-side port file
            port_objs.append(f'{game_root}/port/{fn[:-2]}.o')
            continue
        out = f'{obj_root}/port/{fn[:-2]}.o'
        if fn in host_side or plat not in MAC_CC:
            lines.append(f'build {out}: {"cc_host" if fn in host_side else "cc_game_port"} {C_ROOT}/game/port/{fn}')
        port_objs.append(out)
    for fn in ('gfx_pc.c', 'gfx_cc.c', 'gfx_soft.c'):
        out = f'{obj_root}/kit/{fn[:-2]}.o'
        lines.append(f'build {out}: cc_host {C_ROOT}/bench/kit/{fn}')
        port_objs.append(out)
    # the kit (vendored at kit/): the save states everywhere, the menu on the device
    kit_files = ('savestate.c',)
    if plat == 'miyoo':  # the Miyoo kit (the shared Miyoo kit): the panel, its menu, the second core, the clock
        kit_files += ('miyoo_panel.c', 'miyoo_menu.c', 'miyoo_remap.c', 'miyoo_worker.c', 'miyoo_clock.c', 'miyoo_sleepsave.c')
    for fn in kit_files:
        out = f'{obj_root}/hhkit/{fn[:-2]}.o'
        lines.append(f'build {out}: cc_host {C_ROOT}/kit/platform/{fn}')
        port_objs.append(out)
    port_objs.append(f'{game_root}/port/port_effects.o')
    if plat not in MAC_CC and not shared_game:
        lines.append(f'build {obj_root}/port/port_effects.o: cc_game_port {C_ROOT}/build/game/port_effects.c')
    # the link's build stamp (tools/port/miyoo.sh writes a random one per build; recipes.bin and the cache carry it)
    stamp_c = os.path.join(ROOT, 'build', 'game', plat, 'build_stamp.c')
    if not os.path.exists(stamp_c):
        os.makedirs(os.path.dirname(stamp_c), exist_ok=True)
        open(stamp_c, 'w').write('#include <stdint.h>\nconst uint32_t port_build_stamp = 0;\n')
    lines.append(f'build {obj_root}/port/build_stamp.o: cc_host {C_ROOT}/build/game/{plat}/build_stamp.c')
    port_objs.append(f'{obj_root}/port/build_stamp.o')
    gen_c = f'{C_ROOT}/build/game/port_segtab.c'
    lines.append(f'build {obj_root}/port/port_segtab.o: cc_host {gen_c}')
    port_objs.append(f'{obj_root}/port/port_segtab.o')

    # The link: everything static, above 0x40000000 (real pointers stay above 0x10000000, where port_seg_to_ptr
    # tells them from segmented addresses); rom_syms.ld gives the decomp's segment symbols their ROM values.
    ldflags = '-m32 -no-pie -Wl,-Ttext-segment=0x40000000' if plat == 'pc' else '-no-pie -Wl,-Ttext-segment=0x40000000'
    libs = '-lm' if plat == 'pc' else '-lSDL -lSDL_ttf -lSDL_image -lm -lpthread'
    if plat == 'miyoo':  # no SDL_ttf / SDL_image: the Miyoo's screens use the menu's bitmap font
        libs = '-lSDL -lm -lpthread'
    # The asset region: PROGBITS on the desktop proxy; on the handheld the shipped link has it NOLOAD and a second,
    # identical link keeps its bytes and relocations (-q) for tools/assets/make_assets.py.
    asset_ld = f'{C_ROOT}/build/game/assets_' + ('progbits' if plat == 'pc' else 'noload') + '.ld'
    ldflags += f' -Wl,-T,{C_ROOT}/build/game/dontsave.ld'
    lines += ['rule link', f'  command = {cc} {ldflags} -Wl,-T,{asset_ld} -o $out @$out.rsp {C_ROOT}/build/game/rom_syms.ld {libs}',
              '  rspfile = $out.rsp', '  rspfile_content = $in', '  description = link $out', '',
              f'build {obj_root}/pm64: link ' + ' '.join(game_objs + port_objs), '']
    if plat != 'pc':
        lines += ['rule link_assets',
                  f'  command = {cc} {ldflags} -Wl,-q -Wl,-T,{C_ROOT}/build/game/assets_progbits.ld -o $out @$out.rsp '
                  f'{C_ROOT}/build/game/rom_syms.ld {libs}',
                  '  rspfile = $out.rsp', '  rspfile_content = $in', '  description = link $out', '',
                  f'build {obj_root}/pm64_assets.elf: link_assets ' + ' '.join(game_objs + port_objs), '']
    with open(os.path.join(out_dir, f'{plat}.ninja'), 'w') as f:
        f.write('\n'.join(lines) + '\n')

    # The code segments the game DMAs (port_segments.c): linked in natively, so a DMA of one is not a copy.
    romsegs_path = os.path.join(out_dir, 'rom_segments.json')
    if not os.path.isfile(romsegs_path):
        die('missing build/game/rom_segments.json: run tools/port/rom_segments.py in the pm64hh:decomp image')
    romsegs = json.load(open(romsegs_path))
    # Only segments with C in them are linked in natively; an image-only "code" segment (the logos) is read from the ROM.
    native = {seg for seg, o, info in units}
    code = [r for r in romsegs if r['name'] in native and r['rom_end'] is not None and r['rom_end'] > r['rom_start']]
    missing = native - {r['name'] for r in romsegs}
    if missing:
        print(f'gen_build.py: {len(missing)} linked segments have no ROM range in splat.yaml (not intercepted): '
              + ' '.join(sorted(missing)[:12]) + (' ...' if len(missing) > 12 else ''))
    with open(os.path.join(out_dir, 'port_segtab.c'), 'w') as f:
        f.write('/* generated by tools/port/gen_build.py from splat.yaml: the ROM ranges of the code segments */\n'
                '#include <stdint.h>\n'
                'const struct { uint32_t start, end; const char *name; } port_code_segs[] = {\n')
        for r in code:
            f.write(f'    {{ 0x{r["rom_start"]:08X}, 0x{r["rom_end"]:08X}, "{r["name"]}" }},\n')
        f.write('};\n' f'const int port_code_segs_n = {len(code)};\n')
    # Every segment's ROM range (and a VRAM where the decomp fixes one), for the code that names them (ld_addrs.h).
    with open(os.path.join(out_dir, 'rom_syms.ld'), 'w') as f:
        f.write('/* generated by tools/port/gen_build.py: the ROM ranges of the decomp segments and their fixed VRAM addresses */\n')
        # data linked at VRAM 0 and read as <seg>_ROM_START + symbol (imgfx_data, charset, entity animations): natively
        # the symbol is the data's address, so <seg>_ROM_START is 0 and port_pi_read copies from memory
        at_zero = {sg['name'] for sg in segs if sg['vram'] == 0 and sg['name'] in native}
        for r in romsegs:
            if not r['name'] or r['rom_end'] is None:
                continue
            if r['name'] in at_zero:
                f.write(f'PROVIDE({r["name"]}_ROM_START = 0);\n')
                f.write(f'PROVIDE({r["name"]}_ROM_END = 0x{r["rom_end"] - r["rom_start"]:08X});\n')
                continue
            f.write(f'PROVIDE({r["name"]}_ROM_START = 0x{r["rom_start"]:08X});\n')
            f.write(f'PROVIDE({r["name"]}_ROM_END = 0x{r["rom_end"]:08X});\n')
        for sg in segs:
            if sg['vram'] is not None:
                f.write(f'PROVIDE({sg["name"]}_VRAM = 0x{sg["vram"]:08X});\n')
        f.write('PROVIDE(heap_battleHead = gFrameBuf2);\nPROVIDE(gBackgroundImage = D_80200000);\n')
        # the decomp's fixed symbols (ver/us/undefined_syms.txt): the anti-piracy "obfuscated_" pointers keep their
        # arithmetic; hardware registers and the dead maps' pointers are not needed
        for line in open(os.path.join(DECOMP, 'ver', 'us', 'undefined_syms.txt')):
            m = re.match(r'^\s*(\w+)\s*=\s*([^;]+);', line)
            if m and not re.match(r'^(D_[AB][0-9A-F]{7}|dead_\w+|gBackgroundImage|gMapShapeData|heap_battleHead)$', m.group(1)):
                f.write(f'PROVIDE({m.group(1)} = {m.group(2).strip()});\n')
        # linker-script symbols with no native meaning yet: the overlays' section bounds (nuPiReadRomOverlay clears
        # nothing natively) and the N64 image bounds
        for ov in ('pause', 'filemenu'):
            for k in ('TEXT_START', 'TEXT_END', 'DATA_START', 'RODATA_END', 'BSS_START', 'BSS_END'):
                f.write(f'PROVIDE({ov}_{k} = 0);\n')
        for sym in ('_fbss', '_ftext', '__assets_start', '__assets_end', 'world_action_CLASS_VRAM'):
            f.write(f'PROVIDE({sym} = 0);\n')
        # the effects' engine calls (src/effect_shims.yaml; effectFuncs in effect_utils.c): shim_X is X
        for line in open(os.path.join(DECOMP, 'src', 'effect_shims.yaml')):
            m = re.match(r'^- shim_(\w+)', line)
            if m and m.group(1) != 'NULL':
                f.write(f'PROVIDE(shim_{m.group(1)} = {m.group(1)});\n')
    classes = {}
    for s in segs:
        classes[s['class']] = classes.get(s['class'], 0) + 1
    print(f'gen_build.py: {len(segs)} segments {classes}; {len(units)} C units; skipped {skipped}; {nfx} fx_ entry points; '
          f'{nwrap} asset include wrappers')
    print(f'gen_build.py: wrote build/game/{plat}.ninja and build/game/segments.json')

main()
