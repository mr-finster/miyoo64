#!/usr/bin/env bash
# GoldenEye's renderer census (the cross-port lead's item 5): which of GoldenEye's kit
# fixes SM64's display lists would hit. Builds the desktop proxy with -DHH_CENSUS (the
# counters in gfx_pc.c, printed at exit), runs the intro, every course level, every room
# of the castle's main floor and both pause menus, sums the counts, then rebuilds the
# normal proxy. tools/census_static.py is the static half (every combiner, render mode
# and TLUT written in the sources and data, rooms the runs never visit included).
# usage: tools/census.sh [outdir=refs/census]
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-refs/census}
rm -rf "$out"; mkdir -p "$out"
touch port/src/hh/gfx/gfx_pc.c
./dev.sh 'cd /work/port && make PLAT=pc -j8 HH_OPT="-O2 -DHH_CENSUS" 2>&1 | grep -E "error" || true'
./dev.sh "cd /work/port && one() { n=\$1 f=\$2; shift 2; env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SM64_DATA=/tmp/cen_\$n \
    SM64_AUDIO_SYNC=1 SM64_W=240 SM64_H=240 SM64_EXIT_AT=\$f \"\$@\" timeout 900 ./build/us_pc/sm64 > /work/$out/\$n.log 2>&1; echo \$n exit=\$?; }
  export -f one
  { echo 'intro_head 1500'
    echo 'intro_game 2400 SM64_AUTO=1'
    echo 'pause_course 700 SM64_AUTO=1 SM64_LEVEL=9 SM64_PRESS=450:S'
    echo 'pause_castle 700 SM64_AUTO=1 SM64_LEVEL=6 SM64_PRESS=450:S'
    for l in 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 26 27 28 29 30 31 33 34 36; do echo \"L\$l 600 SM64_AUTO=2 SM64_LEVEL=\$l\"; done
    for r in \$(seq 1 17); do echo \"castle_room\$r 300 SM64_AUTO=1 SM64_LEVEL=6 SM64_ROOM=\$r\"; done
  } | xargs -P 6 -L 1 bash -c 'one \"\$@\"' _"
python3 - "$out" <<'EOF'
import glob, os, re, sys, collections
tot = collections.Counter(); other = collections.Counter(); combos = collections.defaultdict(lambda: [0, 0, set()]); nfb = collections.Counter(); frac = []
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.log'))):
    name = os.path.basename(f)[:-4]
    for l in open(f, errors='replace'):
        if 'census:' not in l: continue
        l = l.split('census:', 1)[1].strip()
        for pat, keys in ((r'(\d+) tris, (\d+) 2D quads', ('tris', 'quads')),
                          (r'blend 1-A without FORCE_BL: (\d+) tris, (\d+) quads', ('nofbl tris', 'nofbl quads')),
                          (r'1/2-cycle texrects (\d+), with a fractional edge (\d+)', ('1/2-cycle texrects', 'fractional')),
                          (r'R34 into the depth image: texrects (\d+), fill rects (\d+), tris (\d+)', ('R34 Z texrects', 'R34 Z fills', 'R34 Z tris')),
                          (r'LOADTLUTs (\d+), with a start texel (\d+)', ('LOADTLUTs', 'with a start texel')),
                          (r'textured draws with a CI texture (\d+), under an IA16 TLUT (\d+)', ('CI draws', 'IA16 TLUT')),
                          (r'G_RDPHALF_1/2/CONT outside texrects (\d+), raw RDP triangle commands (\d+)', ('RDPHALF', 'raw RDP tris')),
                          (r'scissors (\d+), with a fractional edge on the target (\d+)', ('scissors', 'fractional scissors')),
                          (r'inverted scissors (\d+)()', ('inverted scissors', None)),
                          (r'R24 textured draws from a LoadTile tile (\d+), wrapping under the mask period (\d+)', ('R24 LoadTile-tile draws', 'R24 under the period')),
                          (r'R25 2-cycle draws whose cycle 2 reads TEXEL0/1 (\d+)()', ('R25 cycle-2 texel draws', None)),
                          (r'R26 LoadTiles (\d+), rectangle unlike the render tile.s (\d+), textured draws with LoadTile corners (\d+)', ('R26 LoadTiles', 'R26 unlike the render tile', 'R26 draws')),
                          (r'R27 textured draws from a tile inside a load at an offset (\d+), from an overwritten slot (\d+)', ('R27 offset draws', 'R27 stale-slot draws')),
                          (r'R29 3D triangles with a render-tile shift (\d+), with a tile-1 shift under a TEXEL1 combiner (\d+)', ('R29 shift tris', 'R29 tile-1 shift tris')),
                          (r'textured draws from a LoadBlock tile (\d+), wrapping with a corner-to-corner size under the mask period (\d+)',
                           ('LoadBlock-tile draws', 'under the mask period'))):
            m = re.match(pat, l)
            if m:
                for i, k in enumerate(keys):
                    if k: tot[k] += int(m[i + 1])
                break
        else:
            m = re.match(r'nofbl othermode_l (\w+) h (\w+): (\d+) tris (\d+) quads', l)
            if m: nfb[(m[1], m[2])] += int(m[3]) + int(m[4]); continue
            m = re.match(r'(\d+) tris +(\d+) quads (cyc\d) (.*?) ?c1 rgb (.*)', l)
            if m:
                k = (m[3], m[4].strip(), m[5]); combos[k][0] += int(m[1]); combos[k][1] += int(m[2]); combos[k][2].add(name)
                continue
            if l.startswith('fractional texrect'): frac.append((name, l))
            if l.startswith(('fractional scissor', 'block tile', 'loadtile tile', 'inverted scissor', 'R25 ', 'R27 ', 'R29 ')): other[l] += 1
for k, v in tot.items(): print(f'{k:22} {v}')
for k, v in nfb.items(): print('nofbl mode', k, v)
for k, v in sorted(combos.items(), key=lambda kv: -(kv[1][0] + kv[1][1])):
    print(f'odd combiner: {v[0]} tris {v[1]} quads {k[0]} {k[1]} c1 rgb {k[2]} in {" ".join(sorted(v[2]))}')
for x in frac[:10]: print('fractional texrect:', *x)
for k, v in other.most_common(20): print('%5d x %s' % (v, k))
EOF
touch port/src/hh/gfx/gfx_pc.c
./dev.sh 'cd /work/port && make PLAT=pc -j8 2>&1 | grep -E "error" || true'
