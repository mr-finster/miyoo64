#!/usr/bin/env python3
"""gen_build.py -- the native build of Paper Mario's own code (M2), generated from the decomp's split.

Reads pmret/papermario's generated linker script (refs/papermario/ver/us/papermario.ld: every segment, its VRAM window
and its objects in link order) and build.ninja (each object's source and flags), and writes build/game/<plat>.ninja:
every game C file compiled natively, in the decomp's own pipeline (cpp | iconv UTF-8 -> CP932 | cc), with our shim
headers first on the include path. The N64-only parts are left out and replaced by game/port/: libultra and nusys
(src/os), the boot code, the assembly files, and the dead maps.

  python3 tools/port/gen_build.py [pc|handheld|miyoo] [--region us|pal]
  make -f build/game/pc_game.mk -j12 -k           the game's C on the Mac (clang for i686-linux: the decomp needs no
                                                  system headers, and the shared Docker VM is slow)
  ./dev.sh ninja -f build/game/pc.ninja           the host side (system headers, the renderer kit) and the link

Writes build/game/segments.json too: each segment's name, VRAM, class and objects, for the overlay tables.

miyoo (the Miyoo Mini, README_MIYOO.md): the game side is the handheld's, object for object (build/game/handheld/: the
same compile, so the same asset region and recipes), linked with the host side built by the Mini's toolchain
(MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain: gcc 8.3, glibc 2.28, the device's SDL 1.2) with MIYOO defined
beside HANDHELD. Build the handheld game side first (make -f build/game/handheld_game.mk), then ninja -f build/game/miyoo.ninja.
PAL selects ver/pal and assets/pal and isolates every output under build/game/pal/.
The US default paths remain unchanged. Missing split prerequisites abort before publishing artifacts.
Nothing here reads the ROM; the split itself (./configure us in the pm64hh:decomp image) is local and git-ignored.
"""
import argparse
import errno
from dataclasses import dataclass
import json, os, re, sys
from pathlib import Path
import tempfile
import stat

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DECOMP = os.path.join(ROOT, 'refs', 'papermario')
C_ROOT = '/work'                     # the repo inside the build container (./dev.sh)
C_DECOMP = C_ROOT + '/refs/papermario'

@dataclass(frozen=True)
class BuildPaths:
    region: str = 'us'

    def __post_init__(self):
        if self.region not in ('us', 'pal'):
            raise ValueError('region must be us or pal')

    @property
    def relative(self):
        return 'build/game' + ('/pal' if self.region == 'pal' else '')

    @property
    def local(self):
        return ROOT + '/' + self.relative

    @property
    def container(self):
        return C_ROOT + '/' + self.relative

    @property
    def prefix(self):
        return f'ver/{self.region}/build/'


def ninja_lines(path):
    """Join Ninja continuation lines before parsing the pinned generator's output."""
    pending = ''
    with open(path) as stream:
        for line in stream:
            line = pending + line.rstrip('\n')
            if line.endswith('$'):
                pending = line[:-1] + ' '
                continue
            yield line + '\n'
            pending = ''
    if pending:
        raise ValueError(f'incomplete Ninja continuation in {path}')


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

def prove_linker_follow(lines, headers, index, symbol, previous):
    """Recognize Splat's end-of-predecessor relation, never evaluate a VMA.

    Only the generated section/NOLOAD pair and its bounded trailer grammar are
    supported. Object sizes and the location counter remain the N64 linker's job.
    """
    def fail():
        raise ValueError(f'unsupported linker section address: {lines[index].strip()} '
                         '(unproven predecessor VRAM end)')

    if previous is None or symbol != previous['name'] + '_VRAM_END':
        fail()
    name = previous['name']
    # Count all assignments, including compound assignments and definitions in
    # other scopes. A matching spelling alone is not evidence of an end address.
    assignment = r'\b' + re.escape(symbol) + r'\s*(?:(?:<<|>>|[+*/%&|^\-])?=)'
    definitions = [i for i, line in enumerate(lines) if re.search(assignment, line)]
    if len(definitions) != 1 or len(re.findall(assignment, ''.join(lines))) != 1:
        fail()
    end = definitions[0]
    if end >= index or lines[end].strip() != symbol + ' = .;':
        fail()

    # Uniqueness and BSS placement must include future headers: parse_ld later
    # merges a matching BSS into its owner even if it occurs after the follower.
    owners = [position for position, header in enumerate(headers) if header[1] == name]
    bss = [position for position, header in enumerate(headers) if header[1] == name + '_bss']
    follower = next(header[1] for header in headers if header[0] == index)
    if len(owners) != 1 or len(bss) > 1 or sum(h[1] == follower for h in headers) != 1:
        fail()
    if bss and (bss[0] != owners[0] + 1 or headers[bss[0]][0] >= end or
                headers[bss[0]][2] != '(NOLOAD)'):
        fail()
    preceding = [header for header in headers if header[0] < index]
    start, last_name, address = preceding[-1]
    if last_name == name + '_bss' and address == '(NOLOAD)':
        if len(preceding) < 2 or preceding[-2][1] != name:
            fail()
        start = preceding[-2][0]
    elif last_name != name:
        fail()

    # Walk the predecessor's complete body, optional BSS, and final trailer.
    # A stale end marker before BSS or before a later ALIGN must not be accepted.
    depth = 0
    expecting_body = True
    closed = False
    end_proved = False
    for i in range(start + 1, index):
        text = lines[i].strip()
        if not text:
            continue
        if expecting_body:
            if text != '{':
                fail()
            expecting_body = False
            depth = 1
            continue
        if depth:
            depth += text.count('{') - text.count('}')
            if depth not in (0, 1) or (depth == 0 and text != '}'):
                fail()
            if depth == 0:
                closed = True
            continue
        if re.fullmatch(
                r'\.' + re.escape(name) + r'_bss \(NOLOAD\) : SUBALIGN\([1-9][0-9]*\)', text):
            if i > end:
                fail()
            expecting_body = True
            closed = False
            continue
        if i == end:
            if not closed:
                fail()
            end_proved = True
            continue
        align = re.fullmatch(r'\. = ALIGN\(\., ([1-9][0-9]*)\);', text)
        if align:
            alignment = int(align[1])
            if i > end or alignment & (alignment - 1):
                fail()
            continue
        # These generated statements cannot change the VMA location counter.
        safe = (r'__romPos \+= SIZEOF\(\.' + re.escape(name) + r'\);',
                r'__romPos = ALIGN\(__romPos, [1-9][0-9]*\);',
                r'[A-Za-z_][A-Za-z0-9_]*_ROM_(?:START|END) = __romPos;',
                r'[A-Za-z_][A-Za-z0-9_]*_VRAM = ADDR\(\.[A-Za-z0-9_]+\);')
        if not any(re.fullmatch(pattern, text) for pattern in safe):
            fail()
    if depth or expecting_body or not closed or not end_proved:
        fail()
    return {'segment': name, 'symbol': symbol}


def parse_ld(path, region='us'):
    """Segments in link order: name, vram (None = follows the previous), objects per kind."""
    segs, cur = [], None
    sec_re = re.compile(r'^    \.([A-Za-z0-9_]+)(.*?) :')
    section = r'(\.[a-z]+)' if region == 'us' else r'(\.[a-z]+|COMMON)'
    obj_re = re.compile(r'^        (ver/' + re.escape(region) + r'/build/\S+\.o)\(' + section + r'\*?\);')
    lines = list(ninja_lines(path))
    headers = [(i, m.group(1), m.group(2).strip())
               for i, line in enumerate(lines) if (m := sec_re.match(line))]
    for index, line in enumerate(lines):
        m = sec_re.match(line)
        if m:
            name, addr = m.group(1), m.group(2).strip()
            follow = None
            if addr and addr != '(NOLOAD)' and not re.fullmatch(r'0x[0-9A-Fa-f]+', addr):
                follow = prove_linker_follow(lines, headers, index, addr, segs[-1] if segs else None)
            if name.endswith('_bss') and addr == '(NOLOAD)':
                cur = next((s for s in reversed(segs) if s['name'] == name[:-4]), None)
                if cur is None:
                    cur = {'name': name[:-4], 'vram': None, 'objs': []}
                    segs.append(cur)
                continue
            cur = {'name': name, 'vram': int(addr, 16) if addr and addr.startswith('0x') else None, 'objs': []}
            if follow is not None:
                cur['vram_follow'] = follow
            segs.append(cur)
            continue
        if re.match(r'^    \.[A-Za-z0-9_]+.* :', line):
            raise ValueError(f'unsupported linker section address: {line.strip()}')
        m = obj_re.match(line)
        if region == 'pal' and not m and re.match(r'\s+ver/(?:us|pal)/build/\S+\.o\(', line):
            raise ValueError(f'unsupported or wrong-region linker object: {line.strip()}')
        if m and cur is not None:
            obj = m.group(1)
            if obj not in cur['objs']:
                cur['objs'].append(obj)
    return segs

def parse_ninja(path):
    """Object -> (rule, source, cppflags, cflags) for every object build statement."""
    objs, cur = {}, None
    build_re = re.compile(r'^build (\S+\.o):\s+(\S+)\s+(\S+)')
    for line in ninja_lines(path):
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

# The overlays: the segments the N64 copies from the ROM each time it loads one (a map on entry and again after each
# battle, a battle area per battle, a move per use, the partners, actions, items and effects, the battle code, the
# menus), which puts their .data back as linked. Natively they are linked in and a load copies nothing, so
# port_segments.c restores each one's .data from the pristine region when the game DMAs it whole. Their objects keep
# the N64's split of .data and .bss (-fno-zero-initialized-in-bss: a variable written "= 0" is .data on the N64, and
# clang would put it in .bss), and their data is bracketed by __pm64ovl_<segment>_start/_end in the asset region
# (assets_*.ld). The .bss is left alone, as the N64's DMA does (Lakilester's and Sushie's ride state lives in a
# partner's .bss across map changes). Not overlays here: the resident code, loaded once at boot, and the entity models
# and effect graphics, which the N64 copies into the heap per entity or effect (natively all share one copy).
OVL_CLASSES = {'map', 'battle_area', 'battle_move', 'battle_partner', 'battle_star', 'world_item', 'world_action',
               'world_partner', 'effect'}
OVL_FIXED = {'battle_code', 'pause', 'filemenu', 'world_use_item', 'world_script_api', 'dgb_01_smash_bridges',
             'inspect_icon', 'speech_bubble', 'pulse_stone', 'i_spy', 'entity_default', 'entity_jan_iwa',
             'entity_sbk_omo'} | {f'battle_area_omo2_{i}' for i in range(1, 7)}
OVL_CFLAGS = '-fno-zero-initialized-in-bss'

# PAL's relocated VRAM windows do not identify overlay classes. Use the
# pinned segment/object identities instead, verified against the US Splat
# overlay inventory. Keep graphics-only effect objects and resident code out.
PAL_OVL_FIXED = OVL_FIXED | {'effect_globals', 'btl_states_menus', 'level_up', 'starpoint'}
PAL_OVL_PATHS = ('src/world/area_', 'src/battle/area/', 'src/battle/move/',
                 'src/battle/partner/', 'src/battle/action_cmd/', 'src/world/partner/',
                 'src/world/action/', 'src/world/item/', 'src/world/model_anim/')

def is_overlay(seg, region='us'):
    if region == 'us':
        return seg['class'] in OVL_CLASSES or seg['name'] in OVL_FIXED
    if seg['name'] in PAL_OVL_FIXED or seg['class'] == 'effect':
        return True
    prefix = BuildPaths(region).prefix
    for obj in seg['objs']:
        if not obj.startswith(prefix):
            raise ValueError('foreign overlay object prefix: ' + obj)
        path = obj[len(prefix):]
        if path.endswith('.c.o') and (path.startswith(PAL_OVL_PATHS) or
                (path.startswith('src/effects/') and not path.startswith('src/effects/gfx/'))):
            return True
    return False


def seg_class(seg, region='us'):
    v = seg['vram']
    if v is None:
        return 'follow'
    if 0xE0000000 <= v < 0xE1000000:
        return 'effect'
    return (WINDOWS if region == 'us' else {}).get(v, 'fixed')

def pal_splat_code_facts(path):
    """Read only direct, scalar fields of top-level PAL Splat segments.

    This is deliberately a narrow, fail-closed reader, not a YAML parser. It
    ignores nested subsegment scalars and accepts only unsigned numeric start/
    VRAM fields. The ROM is never consulted.
    """
    facts = {}
    source = Path(path).read_text(encoding='utf-8')
    # JSON is a YAML subset. The existing synthetic rom_segments integration
    # fixture writes this representation; accept it without a YAML dependency.
    # Read only direct top-level segment scalars, as for block-style Splat.
    if source.lstrip().startswith('{'):
        try:
            document = json.loads(source)
        except json.JSONDecodeError as exc:
            raise ValueError(f'invalid PAL Splat JSON: {exc}') from exc
        if not isinstance(document, dict) or not isinstance(document.get('segments'), list):
            raise ValueError('PAL Splat lacks top-level segments list')
        for segment in document['segments']:
            if not isinstance(segment, dict):
                raise ValueError('invalid PAL Splat JSON segment')
            if segment.get('type') != 'code':
                continue
            name = segment.get('name')
            if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_]+', name):
                continue
            if name in facts:
                raise ValueError(f'ambiguous PAL Splat code segment: {name}')
            children = segment.get('subsegments', [])
            if not isinstance(children, list):
                raise ValueError(f'invalid PAL Splat subsegments for {name}')
            fact = {'name': name, 'type': 'code',
                    'has_data': any(isinstance(child, dict) and child.get('type') == '.data'
                                    for child in children)}
            for key in ('start', 'vram'):
                value = segment.get(key)
                if type(value) is int and 0 <= value <= 0xFFFFFFFF:
                    fact[key] = value
            follows = segment.get('follows_vram')
            if isinstance(follows, str):
                fact['follows_vram'] = follows
            facts[name] = fact
        return facts

    in_segments = False
    item_indent = None
    current = None

    def finish():
        if current is None or current.get('type') != 'code':
            return
        name = current.get('name')
        if not name or not re.fullmatch(r'[A-Za-z0-9_]+', name):
            return
        if name in facts:
            raise ValueError(f'ambiguous PAL Splat code segment: {name}')
        facts[name] = current.copy()

    for raw in source.splitlines():
        if not in_segments:
            if re.fullmatch(r'segments:\s*(?:#.*)?', raw):
                in_segments = True
            continue
        if raw and not raw[0].isspace() and not raw.lstrip().startswith('#'):
            break
        match = re.match(r'^( *)(- )(.+)$', raw)
        if match and (item_indent is None or len(match[1]) < item_indent):
            item_indent = len(match[1])
        if match and len(match[1]) == item_indent:
            finish()
            current = {'has_data': False}
            field = match[3]
        elif current is not None and re.match(r'^ {' + str(item_indent + 2) + r'}\S', raw):
            field = raw.strip()
        else:
            field = None
        if current is None:
            continue
        # Splat permits both a one-line `- type: .data` child and a
        # mapping child with `- start: ...` followed by `type: .data`.
        # Only nested types count: top-level segment metadata stays separate.
        if (len(raw) - len(raw.lstrip()) > item_indent and
                re.fullmatch(r'\s*(?:-\s*)?type:\s*\.data\s*(?:#.*)?', raw)):
            current['has_data'] = True
        if field is None:
            continue
        scalar = re.fullmatch(r'(name|type|start|vram|follows_vram):\s*([^#\s]+)\s*(?:#.*)?', field)
        if not scalar:
            continue
        key, value = scalar.groups()
        if key in ('vram', 'start'):
            try:
                current[key] = int(value, 0)
            except ValueError:
                pass
        else:
            current[key] = value
    if not in_segments:
        raise ValueError('PAL Splat lacks top-level segments list')
    finish()
    return facts


def pal_linker_prefixes(romsegs, segs):
    """Normalize only linker identifiers; never rename a ROM segment."""
    original = {name for name in
                ([r['name'] for r in romsegs if r['name']] + [s['name'] for s in segs])}
    names = {}
    for name in sorted(original):
        prefix = 'seg_' + name if name[0].isdigit() else name
        if prefix in names and names[prefix] != name:
            raise ValueError(f'PAL linker symbol collision: {names[prefix]} and {name}')
        names[prefix] = name
    return {name: ('seg_' + name if name[0].isdigit() else name) for name in original}


def write_port_effects(out_dir, region='us'):
    """build/game/port_effects.c: the fx_* entry points. On the N64 each is a stub (asm/effects/*.s) that loads the
    effect (load_effect, TLB + DMA) and jumps to its main function through gEffectTable; the port calls it.
    From the decomp's generated effect headers (src/effects.yaml), not the ROM."""
    fxdir = os.path.join(DECOMP, 'assets', region, 'effects')
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
        if key not in macros:
            die(f'missing effect macro {key} for {region}')
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
    if not n:
        die(f'no fx_ entry points in {region} effect headers')
    with open(os.path.join(out_dir, 'port_effects.c'), 'w') as f:
        f.write('\n'.join(out) + '\n')
    return n

def write_asset_wrappers(region='us'):
    """build/game/assetwrap/<path>: for every .inc.c the decomp's split generates from the ROM (vertices, display lists),
    a wrapper found first on the include path that puts its data in .assetdata (#include_next the real one)."""
    root = os.path.join(DECOMP, 'assets', region)
    out_root = BuildPaths(region).local + '/assetwrap'
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

def write_asset_ld(out_dir, ovl_groups):
    """The asset region after .bss: NOLOAD in the handheld's shipped link (the device rebuilds it from the ROM), PROGBITS
    for the desktop proxy and for the handheld's second link that make_assets.py reads. Each overlay's data first,
    between its __pm64ovl_<segment>_start/_end (ovl_groups: segment name, its objects' paths under the build root)."""
    rels = [r for _, objs in ovl_groups for r in objs]
    for r in rels:  # the patterns are '*/<path>': no path may end another
        if any(o != r and o.endswith('/' + r) for o in rels):
            die(f'overlay object {r} is the tail of another object path')
    groups = ''.join(f'    __pm64ovl_{name}_start = .;\n' +
                     ''.join(f'    */{r}(.assetdata .assetdata.*)\n' for r in objs) +
                     f'    __pm64ovl_{name}_end = .;\n' for name, objs in ovl_groups)
    for kind, attr in (('noload', ' (NOLOAD)'), ('progbits', '')):
        with open(os.path.join(out_dir, f'assets_{kind}.ld'), 'w') as f:
            f.write('/* generated by tools/port/gen_build.py: the ROM-derived data (tools/assets) */\n'
                    'SECTIONS {\n'
                    f'  .assetdata{attr} : ALIGN(4096) {{\n    __assets_start = .;\n{groups}'
                    '    *(.assetdata .assetdata.*) *(.assetrodata .assetrodata.*) . = ALIGN(4096); __assets_end = .;\n  }\n'
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

def write_mac_makefile(plat, units, defs, paths=None, out_dir=None, ovl_segs=()):
    """build/game/<plat>_game.mk: the game side (the decomp's C and game/port's game-side files) on the Mac."""
    paths = paths or BuildPaths()
    region = paths.region
    out_dir = out_dir or paths.local
    host_side = {'port_host.c', 'port_rom.c', 'port_gfx.c', 'port_input.c', 'port_segments.c', 'port_sdl.c', 'port_assets_hh.c',
                 'port_mixer.c', 'port_audio_out.c', 'port_hh.c', 'port_state.c'}
    incs = ' '.join(f'-I{p}' for p in (ROOT + '/game/include', DECOMP + f'/ver/{region}/include',
                                         DECOMP + f'/ver/{region}/build/include', DECOMP + '/include', DECOMP + '/src',
                                         DECOMP + f'/assets/{region}'))
    cc = MAC_CC[plat]
    obj_root = f'{paths.local}/{plat}'
    out = [f'# generated by tools/port/gen_build.py {plat}: do not edit. make -f {paths.relative}/{plat}_game.mk -j12 -k',
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
        obj = obj_root + '/' + o[len(paths.prefix):]
        # every decomp file's data goes to the asset region: the decompiled game's data (scripts, tables) is the ROM's
        # too, so the device rebuilds all of it from the player's ROM (the release rule; tools/assets/leak_scan.py)
        out.append(f'{obj}: {src}\n\t$(call cc_game,{info["cppflags"]} -include port_asset_section.h)')
        if seg in ovl_segs:
            out.append(f'{obj}: CFLAGS += {OVL_CFLAGS}')
        objs.append(obj)
    for fn in sorted(os.listdir(os.path.join(ROOT, 'game', 'port'))):
        if fn.endswith('.c') and fn not in host_side:
            obj = f'{obj_root}/port/{fn[:-2]}.o'
            out.append(f'{obj}: {ROOT}/game/port/{fn}\n\t@mkdir -p $(@D)\n'
                       f'\t@cd {DECOMP} && $(CC) -std=gnu11 $(CPPFLAGS) -I{ROOT}/game/port $(CFLAGS) -MD -MF $@.d -c $< -o $@'
                       ' || { echo "FAILED: $<"; exit 1; }')
            objs.append(obj)
    gen = f'{paths.local}/port_effects.c'
    obj = f'{obj_root}/port/port_effects.o'
    out.append(f'{obj}: {gen}\n\t@mkdir -p $(@D)\n'
               f'\t@cd {DECOMP} && $(CC) -std=gnu11 $(CPPFLAGS) $(CFLAGS) -MD -MF $@.d -c $< -o $@ || {{ echo "FAILED: $<"; exit 1; }}')
    objs.append(obj)
    # every object's own .d: make's wildcard has no recursive **, so a glob only reached one directory level and a
    # header change never rebuilt src/pause/, src/world/... (the window-style bitfields, 2026-09-25)
    out += ['', 'all: ' + ' '.join(objs), '', '-include $(wildcard ' + ' '.join(o + '.d' for o in objs) + ')']
    with open(os.path.join(out_dir, f'{plat}_game.mk'), 'w') as f:
        f.write('\n'.join(out) + '\n')

def normalized_input(path):
    """Lexically normalize a PMRET input without cancelling symlink traversal.

    Missing ordinary directories may be cancelled, as in Ninja's generated
    dependency names. A preceding symlink makes '..' ambiguous, so reject that
    combination rather than silently replacing filesystem semantics.
    """
    prefix = ''
    if path.startswith(C_DECOMP + '/'):
        prefix, path = C_DECOMP + '/', path[len(C_DECOMP) + 1:]
    elif path.startswith(DECOMP + '/'):
        prefix, path = DECOMP + '/', path[len(DECOMP) + 1:]
    elif os.path.isabs(path):
        raise ValueError(f'foreign absolute PMRET input path: {path}')
    original = prefix + path
    parts, symlink_seen = [], False
    for part in path.split('/'):
        if part in ('', '.'):
            continue
        if part == '..':
            if not parts:
                raise ValueError(f'PMRET input escapes its root: {original}')
            if symlink_seen:
                raise ValueError(f'cannot normalize PMRET input across a symlink: {original}')
            parts.pop()
        else:
            parts.append(part)
            component = Path(DECOMP).joinpath(*parts)
            try:
                metadata = component.lstat()
            except OSError as error:
                # Only ENOENT proves absence. In particular EACCES, EIO and
                # ENOTDIR must not turn an uninspectable component into a
                # supposedly ordinary directory which '..' could remove.
                if error.errno != errno.ENOENT:
                    raise ValueError(f'cannot inspect PMRET input component {component} '
                                     f'(input {original}): {error}') from error
            else:
                symlink_seen |= stat.S_ISLNK(metadata.st_mode)
    if not parts:
        raise ValueError(f'PMRET input must name a file: {original}')
    relative = '/'.join(parts)
    target = resolved_path(Path(DECOMP) / relative)
    if not target.is_relative_to(resolved_path(Path(DECOMP))):
        raise ValueError(f'PMRET input resolves outside its root: {original}')
    return relative


def local_input(path):
    return os.path.join(DECOMP, normalized_input(path))


def identity_ninja_output_key(output):
    """Lexical lookup key only; do not resolve symlinks or rewrite build paths."""
    absolute = os.path.isabs(output)
    roots = {Path(os.path.normpath(root)).parts[1:] for root in (C_DECOMP, DECOMP)}
    parts = []
    for part in output.split('/'):
        if part in ('', '.'):
            continue
        if part == '..':
            if not parts or (absolute and tuple(parts) in roots):
                raise ValueError(f'Ninja output escapes its identity root: {output}')
            parts.pop()
        else:
            parts.append(part)
    if absolute:
        matches = [root for root in roots if tuple(parts[:len(root)]) == root]
        if not matches:
            raise ValueError(f'foreign absolute Ninja output in PAL identity proof: {output}')
        parts = parts[len(max(matches, key=len)):]
    if not parts:
        raise ValueError(f'Ninja output names its identity root: {output}')
    return '/'.join(parts)


def identity_ninja_edges(path):
    """Index literal build outputs for the limited PAL identity proof only.

    Token-boundary continuations are supported. Escaped/variable outputs cannot
    be ruled out as aliases, so the proof rejects them instead of guessing.
    Other rules' commands/variables are not interpreted or modified.
    """
    edges, pending, current = {}, None, None
    with open(path) as stream:
        for raw in stream:
            line = raw.rstrip('\n')
            if pending is not None:
                line = pending + line.lstrip(' \t')
            elif not line.strip() or line.lstrip().startswith('#'):
                continue
            elif line.startswith((' ', '\t')):
                if current is not None:
                    current[1].append(line.strip())
                continue
            elif not re.match(r'^build[ \t]', line):
                if re.match(r'^(?:include|subninja)[ \t]', line):
                    raise ValueError(f'unsupported Ninja include in PAL identity proof: {line}')
                current = None
                continue
            trailing = len(line) - len(line.rstrip('$'))
            if trailing % 2:
                if len(line) < 2 or line[-2] not in ' \t':
                    raise ValueError(f'unsupported Ninja continuation in PAL identity proof: {line}')
                pending = line[:-1]
                continue
            pending = None
            match = re.fullmatch(r'build[ \t]+([^:]+):[ \t]*(.*)', line)
            if not match:
                raise ValueError(f'unsupported Ninja build syntax in PAL identity proof: {line}')
            outputs = match[1].split()
            if not outputs or any(token != '|' and not re.fullmatch(r'[A-Za-z0-9_./+\-]+', token)
                                  for token in outputs):
                raise ValueError(f'unsupported Ninja output syntax in PAL identity proof: {line}')
            current = [line, []]
            for output in outputs:
                if output == '|':
                    continue
                # Lookup only: aliases are counted, never accepted as canonical
                # output spellings or used as filesystem publication paths.
                key = identity_ninja_output_key(output)
                edges.setdefault(key, []).append(current)
    if pending is not None:
        raise ValueError('incomplete Ninja continuation in PAL identity proof')
    return edges


def pal_rom_identities(romsegs, segs, ninja):
    """Resolve only proven adjacent CI8/palette aliases; never change raw JSON.

    The pinned Splat data-section grammar and bin rules establish the identities.
    stat sizes check ROM lengths without reading extracted binary contents.
    """
    groups = {}
    for index, entry in enumerate(romsegs):
        if entry['name'] is not None:
            groups.setdefault(entry['name'], []).append(index)
    result = [dict(entry) for entry in romsegs]
    inputs = []
    edges = identity_ninja_edges(ninja) if any(len(g) > 1 for g in groups.values()) else {}
    text = ''.join(ninja_lines(os.path.join(DECOMP, 'ver/pal/papermario.ld')))
    for raw, indices in groups.items():
        if len(indices) == 1:
            continue

        def fail():
            die(f'ambiguous PAL ROM segment name: {raw}; CI8/palette identity not proven')

        if len(indices) != 2 or indices[1] != indices[0] + 1:
            fail()
        image, palette = (romsegs[i] for i in indices)
        if image['type'] != 'ci8' or palette['type'] != 'palette':
            fail()
        if any(entry['rom_end'] is None or not 0 <= entry['rom_start'] < entry['rom_end'] <= 0xFFFFFFFF
               for entry in (image, palette)):
            fail()
        if image['rom_end'] != palette['rom_start']:
            fail()
        matches = [s for s in segs if s['name'] == raw]
        if len(matches) != 1 or len(matches[0]['objs']) != 1:
            fail()
        image_obj = matches[0]['objs'][0]
        prefix = 'ver/pal/build/assets/pal/'
        if not image_obj.startswith(prefix) or not image_obj.endswith('.png.o'):
            fail()
        if sum(image_obj in s['objs'] for s in segs) != 1:
            fail()
        palette_obj = image_obj[:-6] + '.pal.o'
        candidates = [s for s in segs if palette_obj in s['objs']]
        if len(candidates) != 1 or candidates[0]['objs'] != [palette_obj]:
            fail()
        palette_name = candidates[0]['name']
        if palette_name == raw or palette_name in groups:
            fail()
        if sum(s['name'] == palette_name for s in segs) != 1:
            fail()
        blocks = []
        for name, obj, entry in ((raw, image_obj, image), (palette_name, palette_obj, palette)):
            # One data object, no BSS, extra padding, expressions or LMA changes.
            block = (f'    {name}_ROM_START = __romPos;\n'
                     f'    {name}_VRAM = ADDR(.{name});\n'
                     f'    .{name} : AT({name}_ROM_START) SUBALIGN(8)\n'
                     '    {\n        FILL(0x00000000);\n'
                     f'        {name}_DATA_START = .;\n        {obj}(.data);\n'
                     f'        {name}_DATA_END = .;\n'
                     f'        {name}_DATA_SIZE = ABSOLUTE({name}_DATA_END - {name}_DATA_START);\n'
                     '    }\n'
                     f'    __romPos += SIZEOF(.{name});\n'
                     f'    {name}_ROM_END = __romPos;\n    {name}_VRAM_END = .;\n')
            if text.count(block) != 1 or re.search(r'^    \.' + re.escape(name) + r'_bss\b', text, re.M):
                fail()
            for suffix in ('ROM_START', 'ROM_END'):
                assignment = r'\b' + re.escape(name + '_' + suffix) + r'\s*(?:(?:<<|>>|[+*/%&|^\-])?=)'
                if len(re.findall(assignment, text)) != 1:
                    fail()
            expected = obj[:-2] + '.bin'
            definitions = edges.get(obj, [])
            if len(definitions) != 1:
                fail()
            line, bindings = definitions[0]
            complete = (r'build[ \t]+' + re.escape(obj) + r'[ \t]*:[ \t]+bin[ \t]+' +
                        re.escape(expected) + r'[ \t]*')
            if not re.fullmatch(complete, line):
                fail()
            if len(bindings) > 1 or any(not re.fullmatch(r'version[ \t]*=[ \t]*pal', b) for b in bindings):
                fail()
            local = local_input(expected)
            actual = str(resolved_path(Path(local)).relative_to(resolved_path(Path(DECOMP))))
            if re.search(r'\b(?:ver|assets)/us/', actual):
                fail()
            metadata = os.stat(local)
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_size != entry['rom_end'] - entry['rom_start']:
                fail()
            inputs.append(Path(local))
            blocks.append(block)
        # Adjacent complete blocks prove one uninterrupted __romPos chain.
        if blocks[0] + '\n' + blocks[1] not in text:
            fail()
        result[indices[1]]['name'] = palette_name
    names = [entry['name'] for entry in result if entry['name'] is not None]
    if len(names) != len(set(names)):
        die('ambiguous PAL ROM segment name after identity resolution')
    return result, inputs


def preflight(paths, segs, units, ninja, plat):
    """Check prerequisites without opening any ROM or extracted binary data."""
    region = paths.region
    if not units:
        die(f'no native C units for {region}; check the selected split and build.ninja')
    required = [f'ver/{region}/undefined_syms.txt', 'src/effect_shims.yaml',
                f'ver/{region}/build/include/ld_addrs.h',
                f'ver/{region}/build/include/message_ids.h',
                f'assets/{region}/effects/effect_macros.h', f'assets/{region}/effects/effect_defs.h']
    if region == 'pal':
        required.append('ver/pal/splat.yaml')
    for segment, obj, info in units:
        if not obj.startswith(paths.prefix) or '..' in obj.split('/'):
            die(f'invalid {region} object path: {obj}')
        if info['rule'] not in ('cc', 'cxx', 'cc_modern', 'cc_272', 'cc_egcs', 'cc_ido'):
            die(f'unsupported native object rule {info["rule"]}: {obj}')
        other = 'pal' if region == 'us' else 'us'
        if (re.search(rf'\b(?:ver|assets)/{other}/', info['src']) or
                re.search(rf'-DVERSION(?:_{other.upper()}\b|={other}\b)', info['cppflags'])):
            die(f'{obj} references the wrong region: {info["src"]} {info["cppflags"]}')
        required.append(info['src'])
    local_sources = [os.path.join(ROOT, 'game/port', name)
                     for name in os.listdir(os.path.join(ROOT, 'game/port')) if name.endswith('.c')]
    local_sources += [os.path.join(ROOT, 'bench/kit', name) for name in ('gfx_pc.c', 'gfx_cc.c', 'gfx_soft.c')]
    kit = ['savestate.c']
    if plat == 'miyoo':
        kit += ['miyoo_panel.c', 'miyoo_menu.c', 'miyoo_remap.c', 'miyoo_worker.c', 'miyoo_clock.c', 'miyoo_sleepsave.c']
    local_sources += [os.path.join(ROOT, 'kit/platform', name) for name in kit]
    for filename in local_sources:
        if not os.path.isfile(filename):
            die(f'missing native port source {filename}')
    # PMRET emits these two phony targets even when their dependency list is empty.
    # Check existence only; never read generated binary image includes here.
    targets = {f'generated_code_{region}', f'inc_img_bins_{region}'}
    found = set()
    for line in ninja_lines(ninja):
        match = re.match(r'^build (\S+): phony(?: (.*))?\s*$', line.rstrip())
        if match and match[1] in targets:
            found.add(match[1])
            required.extend(token for token in (match[2] or '').split() if token not in ('|', '||'))
    if region == 'pal' and found != targets:
        die('missing PAL prerequisite targets in build.ninja; run ./configure pal first')
    for filename in dict.fromkeys(required):
        if '$' in filename:
            die(f'unsupported unresolved Ninja input: {filename}')
        local = local_input(filename)
        other = 'pal' if region == 'us' else 'us'
        spellings = (normalized_input(filename),
                     str(resolved_path(Path(local)).relative_to(resolved_path(Path(DECOMP)))))
        if any(re.search(rf'\b(?:ver|assets)/{other}/', spelling) for spelling in spellings):
            die(f'prerequisite references the wrong region: {filename}')
        if not os.path.isfile(local):
            die(f'missing {filename}; run ./configure {region}, then ninja generated_code_{region} inc_img_bins_{region}')
    filename = os.path.join(paths.local, 'rom_segments.json')
    if not os.path.isfile(filename):
        die(f'missing {filename}; run tools/port/rom_segments.py --region {region}')
    with open(filename) as stream:
        romsegs = json.load(stream)
    if not isinstance(romsegs, list) or not romsegs:
        die(f'invalid {region} ROM segment table: expected a nonempty list')
    for index, entry in enumerate(romsegs):
        if not isinstance(entry, dict) or not {'name', 'type', 'rom_start', 'rom_end'} <= entry.keys():
            die(f'invalid ROM segment entry {index}')
        if entry['name'] is not None and (not isinstance(entry['name'], str) or not re.fullmatch(r'[A-Za-z0-9_]+', entry['name'])):
            die(f'invalid ROM segment name at {index}')
        start, end = entry['rom_start'], entry['rom_end']
        if type(start) is not int or start < 0 or (end is not None and (type(end) is not int or end < start)):
            die(f'invalid ROM segment bounds at {index}')
        next_start = romsegs[index + 1].get('rom_start') if index + 1 < len(romsegs) and isinstance(romsegs[index + 1], dict) else None
        if end != next_start:
            die(f'inconsistent ROM segment end at {index}')
    identity_inputs = []
    if region == 'pal':
        romsegs, identity_inputs = pal_rom_identities(romsegs, segs, ninja)
    native = {segment for segment, obj, info in units}
    names = {entry['name'] for entry in romsegs}
    if region == 'pal':
        if not {'msg_pal_en', 'msg_pal_de', 'msg_pal_fr', 'msg_pal_es'} <= names:
            die('PAL ROM segment table must include all four msg_pal language segments')
        if native - names:
            die('PAL native segments missing ROM ranges: ' + ', '.join(sorted(native - names)))
        by_name = {}
        for entry in romsegs:
            if entry['name'] is not None:
                if entry['name'] in by_name:
                    die(f'ambiguous PAL ROM segment name: {entry["name"]}')
                by_name[entry['name']] = entry
        # These resident units contain only BSS in the pinned PMRET source.
        # Require both the segment name and its sole source, not a caller's type tag.
        bss_sources = {name: os.path.normpath(os.path.join(DECOMP, 'src', name + '.c'))
                       for name in ('heaps', 'heaps2', 'heaps3', 'heaps4',
                                    'texture_memory', 'model_clear_render_tasks')}
        languages = {'msg_pal_en', 'msg_pal_de', 'msg_pal_fr', 'msg_pal_es'}
        for name in sorted(native | languages):
            entry = by_name[name]
            start, end = entry['rom_start'], entry['rom_end']
            bss_only = (name in bss_sources and name in native and
                        all(os.path.normpath(local_input(info['src'])) == bss_sources[name]
                            for segment, obj, info in units if segment == name))
            if end is None or max(start, end) > 0xFFFFFFFF or (end == start and not bss_only):
                die(f'incomplete or empty required PAL ROM range: {name}')
    inputs = [Path(local_input(name)) for name in required]
    inputs += identity_inputs
    inputs += [Path(name) for name in local_sources]
    inputs += [Path(DECOMP) / 'ver' / region / 'papermario.ld', Path(ninja), Path(filename)]
    return romsegs, inputs


def output_targets(paths, plat, units):
    names = ['.ninja_log', '.ninja_deps', 'segments.json', 'port_effects.c', 'assets_noload.ld', 'assets_progbits.ld',
             'dontsave.ld', 'port_segtab.c', 'rom_syms.ld', f'{plat}.ninja',
             f'{plat}/build_stamp.c', f'{plat}/pm64', f'{plat}/pm64.rsp',
             f'{plat}/pm64_assets.elf', f'{plat}/pm64_assets.elf.rsp']
    if plat in MAC_CC:
        names.append(f'{plat}_game.mk')
    game = 'handheld' if plat == 'miyoo' else plat
    names.extend(f'{game}/{obj[len(paths.prefix):]}' for segment, obj, info in units)
    # Include generated sources, objects and dependency/response files.
    port_names = [name[:-2] for name in os.listdir(os.path.join(ROOT, 'game/port')) if name.endswith('.c')]
    for platform in {plat, game}:
        names.extend(f'{platform}/port/{name}.o' for name in port_names + ['port_effects', 'port_segtab', 'build_stamp'])
        names.extend(f'{platform}/kit/{name}.o' for name in ('gfx_pc', 'gfx_cc', 'gfx_soft'))
        names.extend(f'{platform}/hhkit/{name}.o' for name in ('savestate', 'miyoo_panel', 'miyoo_menu',
                     'miyoo_remap', 'miyoo_worker', 'miyoo_clock', 'miyoo_sleepsave'))
    names += [name + '.d' for name in names if name.endswith('.o')]
    return [Path(paths.local) / name for name in names]


def resolved_path(path):
    try:
        try:
            return path.resolve(strict=True)
        except FileNotFoundError:
            return path.resolve()
    except (RuntimeError, OSError) as error:
        if isinstance(error, OSError) and error.errno != errno.ELOOP:
            raise
        raise ValueError(f'cannot resolve path (symlink loop): {path}') from error


def file_identity(path):
    try:
        metadata = path.stat()
    except FileNotFoundError:
        return None
    return metadata.st_dev, metadata.st_ino


def check_output_paths(paths, plat, units, inputs):
    """Plan every final file before publication; paths must remain stable."""
    protected = list(inputs)
    if paths.region == 'pal':
        base = resolved_path(Path(ROOT) / 'build/game')
        expected = base / 'pal'
        if resolved_path(Path(paths.local)) != expected:
            die('PAL output directory must not be a symlink alias of another build directory')
        # Existing US aliases are also protected by inode identity. This matters
        # for future compiler outputs which may truncate rather than replace files.
        us_units = [(segment, 'ver/us/build/' + obj[len(paths.prefix):], info)
                    for segment, obj, info in units]
        for platform in ('pc', 'handheld', 'miyoo'):
            protected.extend(output_targets(BuildPaths('us'), platform, us_units))
        protected.extend([Path(ROOT) / 'build/game/rom_segments.json',
                          Path(ROOT) / '.ninja_log', Path(ROOT) / '.ninja_deps'])
    protected_paths = {resolved_path(Path(path)) for path in protected}
    protected_inodes = {identity for path in protected_paths
                        if (identity := file_identity(path)) is not None}
    plan, seen_paths, seen_inodes = {}, {}, {}
    for requested in dict.fromkeys(output_targets(paths, plat, units)):
        target = resolved_path(requested)
        if paths.region == 'pal' and not target.is_relative_to(expected):
            die(f'PAL output escapes its isolated directory: {requested}')
        if target in protected_paths:
            die(f'output/input collision with protected input or US artifact: {requested} -> {target}')
        identity = file_identity(target)
        if identity is not None and identity in protected_inodes:
            die(f'output/input hardlink collision with protected input or US artifact: {requested}')
        if target in seen_paths:
            die(f'output/output collision: {seen_paths[target]} and {requested} -> {target}')
        if identity is not None and identity in seen_inodes:
            die(f'output/output hardlink collision: {seen_inodes[identity]} and {requested}')
        if target.exists() and not stat.S_ISREG(target.stat().st_mode):
            die(f'output target must be a regular file: {target}')
        plan[requested] = target
        seen_paths[target] = requested
        if identity is not None:
            seen_inodes[identity] = requested
    return plan


def publish(staged, paths, plat, units, inputs):
    plan = check_output_paths(paths, plat, units, inputs)
    files = sorted(path for path in Path(staged).rglob('*') if path.is_file())
    # Publish build entry points last, after successful rendering of ALL files.
    files.sort(key=lambda path: path.suffix in ('.ninja', '.mk'))
    publications = []
    for source in files:
        requested = Path(paths.local) / source.relative_to(staged)
        if requested not in plan:
            die(f'unplanned generated artifact: {requested}')
        publications.append((source, requested, plan[requested]))
    for source, requested, target in publications:
        requested.parent.mkdir(parents=True, exist_ok=True)
        if not target.parent.is_dir():
            raise ValueError(f'output symlink target directory does not exist: {target.parent}')
        if target.exists():
            os.chmod(source, target.stat().st_mode & 0o777)
        os.replace(source, target)


def generate(plat, region='us'):
    paths = BuildPaths(region)
    ld = os.path.join(DECOMP, 'ver', region, 'papermario.ld')
    nj = os.path.join(DECOMP, 'build.ninja')
    for filename in (ld, nj):
        if not os.path.isfile(filename):
            die(f'missing {filename}: run ./configure {region} with the pinned PMRET split first')
    segs, objs = parse_ld(ld, region), parse_ninja(nj)
    if not segs or not objs:
        die(f'nothing parsed for {region} from the linker script or build.ninja')

    skip_src = re.compile(r'^(src/os/|src/boot/|src/gcc/|src/world/dead/|ver/' + region + r'/)')  # src/gcc: MIPS libgcc (the host has its own)
    # libultra's portable C (matrix math, trig, VI mode tables) compiles as it is; the rest of src/os is hardware
    # and nusys, replaced by game/port/.
    portable_os = re.compile(r'^src/os/(gu[A-Za-z]*|perspective|position|rotateRPY|lookathil|lookatref|frustum|sins|coss|'
                             r'sqrtf|ldiv|vitbl|vimode[a-z0-9]*)\.c$')
    units, skipped = [], {'asm': 0, 'bin': 0, 'os/boot/dead': 0}
    for s in segs:
        s['class'] = seg_class(s, region)
        for o in s['objs']:
            info = objs.get(o)
            if info is None:
                die(f'no build.ninja object rule for {o}; regenerate ./configure {region}')
            if info['rule'] in ('as',):
                skipped['asm'] += 1
                continue
            if info['rule'] == 'bin':
                skipped['bin'] += 1
                continue
            # The same spelling must reach validation and emitted Make/Ninja
            # source dependencies; do not fix only the existence check.
            info['src'] = normalized_input(info['src'])
            if skip_src.match(info['src']) and not portable_os.match(info['src']):
                skipped['os/boot/dead'] += 1
                continue
            units.append((s['name'], o, info))

    romsegs, inputs = preflight(paths, segs, units, nj, plat)
    check_output_paths(paths, plat, units, inputs)
    os.makedirs(paths.local, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.gen-build-', dir=paths.local) as out_dir:
        with open(os.path.join(out_dir, 'segments.json'), 'w') as f:
            json.dump([{'name': s['name'], 'vram': s['vram'], 'class': s['class'], 'objs': s['objs']} for s in segs], f, indent=0)

        if plat == 'pc':
            cc = 'gcc -m32 -fno-pie -malign-data=abi -malign-double -msse2 -mfpmath=sse'
            if PM64_SAN:
                cc += ' ' + PM64_SAN + ' -fno-omit-frame-pointer' + (' --param asan-globals=0' if 'address' in PM64_SAN else '')
        else:  # miyoo, and the handheld game side's own host half (the Miyoo links only its game objects)
            mtc = os.environ.get('MIYOO_TOOLCHAIN', '/opt/miyoomini-toolchain')
            cc = f'{mtc}/usr/bin/arm-linux-gnueabihf-gcc -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfloat-abi=hard'
        incs = ' '.join(f'-I{p}' for p in (C_ROOT + '/game/include', C_DECOMP + f'/ver/{region}/include', C_DECOMP + f'/ver/{region}/build/include',
                                             C_DECOMP + '/include', C_DECOMP + '/src', C_DECOMP + f'/assets/{region}'))
        defs = (f'-D_LANGUAGE_C -D_FINALROM -DVERSION={region} -DVERSION_{region.upper()} -DF3DEX_GBI_2 -D_MIPS_SZLONG=32 -DMODERN_COMPILER '
                '-DSHIFT -DNON_MATCHING -DAVOID_UB -DPM64_PORT=1 ' + ('-DTARGET_PC=1' if plat == 'pc' else '-DTARGET_HANDHELD=1'))
        cflags = ('-O2 -g -funsigned-char -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off '
                  '-fno-single-precision-constant -fno-common -fno-builtin-bcopy -w')
        obj_root = f'{paths.container}/{plat}'
        # the game side's objects: the Miyoo links the handheld's (built by build/game/handheld_game.mk), so its asset region is
        # the handheld's byte for byte and the handheld's recipes fit it
        game_root = f'{paths.container}/handheld' if plat == 'miyoo' else obj_root
        shared_game = plat == 'miyoo'
        nfx = write_port_effects(out_dir, region)
        nwrap = 0  # write_asset_wrappers(): superseded by whole-file routing (every decomp file's data is an asset)
        ovl_segs = {s['name'] for s in segs if is_overlay(s, region)}
        ovl_groups = []
        for seg, o, info in units:
            if seg in ovl_segs:
                if not ovl_groups or ovl_groups[-1][0] != seg:
                    ovl_groups.append((seg, []))
                ovl_groups[-1][1].append(o[len(paths.prefix):])
        write_asset_ld(out_dir, ovl_groups)
        write_dontsave_ld(out_dir)
        if plat in MAC_CC:
            write_mac_makefile(plat, units, defs, paths, out_dir, ovl_segs)
        lines = [f'# generated by tools/port/gen_build.py {plat}: do not edit', 'ninja_required_version = 1.3', '',
                 f'rule cc', f'  command = cd {C_DECOMP} && {cc} -E -nostdinc {incs} {defs} $cppflags -MD -MF $out.d $in -o - '
                 f'| iconv --from UTF-8 --to CP932 | {cc} {cflags} -x c -c - -o $out',
                 '  depfile = $out.d', '  deps = gcc', '  description = cc $in', '']
        if region == 'pal':
            lines[3:3] = [f'builddir = {paths.container}', '']
        game_objs = []
        for seg, o, info in units:
            src = C_DECOMP + '/' + info['src'] if not info['src'].startswith('/') else info['src']
            out = game_root + '/' + o[len(paths.prefix):]
            if plat not in MAC_CC and not shared_game:
                lines.append(f'build {out}: cc {src}')
            game_objs.append(out)

        # The port layer (game/port): the game side takes the decomp's headers (as above, -std=gnu11 so the decomp's
        # bool stays its 4-byte enum); the host side takes system headers and the renderer kit's.
        host_side = {'port_host.c', 'port_rom.c', 'port_gfx.c', 'port_input.c', 'port_segments.c', 'port_sdl.c', 'port_assets_hh.c',
                     'port_mixer.c', 'port_audio_out.c', 'port_hh.c', 'port_state.c'}
        kit_inc = f'-I{C_ROOT}/bench/kit -I{C_ROOT}/kit/platform -I{C_DECOMP}/include'
        kit_defs = '-DTARGET_HANDHELD=1 -DPM64=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DPORT_NO_FPS=1 -DNON_MATCHING=1 -DAVOID_UB=1'
        if region == 'pal':
            # Host-side ROM readers don't see the decomp's VERSION_PAL define.
            # US output intentionally stays byte-for-byte unchanged.
            kit_defs += ' -DPM64_ROM_REGION_PAL=1'
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
            lines.append(f'build {obj_root}/port/port_effects.o: cc_game_port {paths.container}/port_effects.c')
        # the link's build stamp (tools/port/miyoo.sh writes a random one per build; recipes.bin and the cache carry it)
        stamp_c = os.path.join(paths.local, plat, 'build_stamp.c')
        if not os.path.exists(stamp_c):
            staged_stamp = os.path.join(out_dir, plat, 'build_stamp.c')
            os.makedirs(os.path.dirname(staged_stamp), exist_ok=True)
            open(staged_stamp, 'w').write('#include <stdint.h>\nconst uint32_t port_build_stamp = 0;\n')
        lines.append(f'build {obj_root}/port/build_stamp.o: cc_host {paths.container}/{plat}/build_stamp.c')
        port_objs.append(f'{obj_root}/port/build_stamp.o')
        gen_c = f'{paths.container}/port_segtab.c'
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
        asset_ld = f'{paths.container}/assets_' + ('progbits' if plat == 'pc' else 'noload') + '.ld'
        ldflags += f' -Wl,-T,{paths.container}/dontsave.ld'
        lines += ['rule link', f'  command = {cc} {ldflags} -Wl,-T,{asset_ld} -o $out @$out.rsp {paths.container}/rom_syms.ld {libs}',
                  '  rspfile = $out.rsp', '  rspfile_content = $in', '  description = link $out', '',
                  f'build {obj_root}/pm64: link ' + ' '.join(game_objs + port_objs), '']
        if plat != 'pc':
            lines += ['rule link_assets',
                      f'  command = {cc} {ldflags} -Wl,-q -Wl,-T,{paths.container}/assets_progbits.ld -o $out @$out.rsp '
                      f'{paths.container}/rom_syms.ld {libs}',
                      '  rspfile = $out.rsp', '  rspfile_content = $in', '  description = link $out', '',
                      f'build {obj_root}/pm64_assets.elf: link_assets ' + ' '.join(game_objs + port_objs), '']
        with open(os.path.join(out_dir, f'{plat}.ninja'), 'w') as f:
            f.write('\n'.join(lines) + '\n')

        # The code segments the game DMAs (port_segments.c): linked in natively, so a DMA of one is not a copy.
        # Only segments with C in them are linked in natively; an image-only "code" segment (the logos) is read from the ROM.
        native = {seg for seg, o, info in units}
        code = [r for r in romsegs if r['name'] in native and r['rom_end'] is not None and r['rom_end'] > r['rom_start']]
        missing = native - {r['name'] for r in romsegs}
        if missing:
            print(f'gen_build.py: {len(missing)} linked segments have no ROM range in splat.yaml (not intercepted): '
                  + ' '.join(sorted(missing)[:12]) + (' ...' if len(missing) > 12 else ''))
        with_data = {name for name, _ in ovl_groups}
        with open(os.path.join(out_dir, 'port_segtab.c'), 'w') as f:
            f.write('/* generated by tools/port/gen_build.py from splat.yaml: the ROM ranges of the code segments, and each\n'
                    ' * overlay\'s data in the asset region (assets_*.ld), which port_segments.c restores when it is loaded */\n'
                    '#include <stdint.h>\n')
            for r in code:
                if r['name'] in with_data:
                    f.write(f'extern uint8_t __pm64ovl_{r["name"]}_start[], __pm64ovl_{r["name"]}_end[];\n')
            f.write('const struct { uint32_t start, end; const char *name; uint8_t *data, *data_end; } port_code_segs[] = {\n')
            for r in code:
                d = (f'__pm64ovl_{r["name"]}_start, __pm64ovl_{r["name"]}_end' if r['name'] in with_data else '0, 0')
                f.write(f'    {{ 0x{r["rom_start"]:08X}, 0x{r["rom_end"]:08X}, "{r["name"]}", {d} }},\n')
            f.write('};\n' f'const int port_code_segs_n = {len(code)};\n')
        # PAL's explicitly declared Splat addresses can backfill provably
        # followed VRAMs. Original segment names (JSON, DMA tables) stay intact.
        pal_facts = (pal_splat_code_facts(os.path.join(DECOMP, 'ver/pal/splat.yaml'))
                     if region == 'pal' else {})
        prefixes = pal_linker_prefixes(romsegs, segs) if region == 'pal' else {}
        declared = (set(re.findall(r'\bextern\s+Addr\s+([A-Za-z_][A-Za-z0-9_]*)\s*;',
                                   Path(DECOMP, 'ver/pal/build/include/ld_addrs.h').read_text()))
                    if region == 'pal' else set())
        rom_by_name = {r['name']: r for r in romsegs if r['name']}
        seg_by_name = {sg['name']: sg for sg in segs}
        # Every segment's ROM range (and a VRAM where the decomp fixes one), for the code that names them (ld_addrs.h).
        with open(os.path.join(out_dir, 'rom_syms.ld'), 'w') as f:
            f.write('/* generated by tools/port/gen_build.py: the ROM ranges of the decomp segments and their fixed VRAM addresses */\n')
            # data linked at VRAM 0 and read as <seg>_ROM_START + symbol (imgfx_data, charset, entity animations): natively
            # the symbol is the data's address, so <seg>_ROM_START is 0 and port_pi_read copies from memory
            at_zero = {sg['name'] for sg in segs if sg['vram'] == 0 and sg['name'] in native}
            for r in romsegs:
                if not r['name'] or r['rom_end'] is None:
                    continue
                symbol = prefixes.get(r['name'], r['name'])
                if r['name'] in at_zero:
                    f.write(f'PROVIDE({symbol}_ROM_START = 0);\n')
                    f.write(f'PROVIDE({symbol}_ROM_END = 0x{r["rom_end"] - r["rom_start"]:08X});\n')
                    # An explicitly data-bearing, zero-based native segment can
                    # use its full ROM span as an allocation size. Only emit an
                    # identifier that PMRET actually declares.
                    fact = pal_facts.get(r['name'], {})
                    if (region == 'pal' and fact.get('has_data') and
                            fact.get('vram') == 0 and fact.get('start') == r['rom_start'] and
                            f'{symbol}_DATA_SIZE' in declared):
                        f.write(f'PROVIDE({symbol}_DATA_SIZE = 0x{r["rom_end"] - r["rom_start"]:08X});\n')
                    continue
                f.write(f'PROVIDE({symbol}_ROM_START = 0x{r["rom_start"]:08X});\n')
                f.write(f'PROVIDE({symbol}_ROM_END = 0x{r["rom_end"]:08X});\n')
            for sg in segs:
                symbol = prefixes.get(sg['name'], sg['name'])
                if sg['vram'] is not None:
                    f.write(f'PROVIDE({symbol}_VRAM = 0x{sg["vram"]:08X});\n')
                    continue
                if region != 'pal' or 'vram_follow' not in sg or f'{symbol}_VRAM' not in declared:
                    continue
                fact = pal_facts.get(sg['name'], {})
                previous = sg['vram_follow']['segment']
                pred = pal_facts.get(previous, {})
                rom = rom_by_name.get(sg['name'], {})
                pred_rom = rom_by_name.get(previous, {})
                # The Splat scalar must explicitly specify this VRAM and the
                # same proven predecessor as parse_ld; never infer from offsets.
                if (fact.get('type') == 'code' and fact.get('follows_vram') == previous and
                        type(fact.get('vram')) is int and 0 <= fact['vram'] <= 0xFFFFFFFF and
                        fact.get('start') == rom.get('rom_start') and
                        pred.get('start') == pred_rom.get('rom_start') and
                        type(pred.get('vram')) is int and
                        seg_by_name.get(previous, {}).get('vram') == pred['vram']):
                    f.write(f'PROVIDE({symbol}_VRAM = 0x{fact["vram"]:08X});\n')
            f.write('PROVIDE(heap_battleHead = gFrameBuf2);\nPROVIDE(gBackgroundImage = D_80200000);\n')
            # the selected region's fixed symbols (ver/<region>/undefined_syms.txt): the anti-piracy "obfuscated_" pointers keep their
            # arithmetic; hardware registers and the dead maps' pointers are not needed
            for line in open(os.path.join(DECOMP, 'ver', region, 'undefined_syms.txt')):
                m = re.match(r'^\s*(\w+)\s*=\s*([^;]+);', line)
                if m and not re.match(r'^(D_[AB][0-9A-F]{7}|dead_\w+|gBackgroundImage|gMapShapeData|heap_battleHead)$', m.group(1)):
                    f.write(f'PROVIDE({m.group(1)} = {m.group(2).strip()});\n')
            # linker-script symbols with no native meaning yet: the overlays' section bounds (nuPiReadRomOverlay clears
            # nothing natively) and the N64 image bounds
            overlays = ['pause', 'filemenu']
            if region == 'pal':
                # Image-only PAL pause overlays share a VMA window. N64 section
                # clear bounds have no native meaning (nuPiReadRomOverlay).
                for name, fact in sorted(pal_facts.items()):
                    if (re.fullmatch(r'pause_gfx_[a-z]{2}', name) and
                            fact.get('has_data') and type(fact.get('vram')) is int and
                            name in seg_by_name and seg_by_name[name]['vram'] == fact['vram'] and
                            fact.get('start') == rom_by_name.get(name, {}).get('rom_start')):
                        overlays.append(name)
            for ov in overlays:
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
        publish(out_dir, paths, plat, units, inputs)
        print(f'gen_build.py: wrote {paths.relative}/{plat}.ninja and {paths.relative}/segments.json')

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', nargs='?', choices=('pc', 'handheld', 'miyoo'), default='pc')
    parser.add_argument('--region', choices=('us', 'pal'), default='us')
    args = parser.parse_args(argv)
    try:
        generate(args.platform, args.region)
    except (OSError, ValueError) as error:
        die(str(error))


if __name__ == '__main__':
    main()
