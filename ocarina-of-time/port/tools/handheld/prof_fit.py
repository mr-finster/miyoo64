#!/usr/bin/env python3
"""prof_fit.py: symbolize an on-device prof.txt (hh_prof.c) when the build's unstripped binary is not at hand
(2026-10-01, the FPS hunt: refs/prof_bins/ is local to the lead's machine; a game build needs the ROM).

The port layer (src/port/hh, gfx, kit) is compiled here with the device flags from a source tree at the build's
commit; its .text.<function> sections are laid out in link order (-ffunction-sections keeps input order); the
base address is fitted so the profile's lr samples (return addresses) land right after bl/blx instructions, per
object (an object whose code differs from the device build's gets its own shift, or no fit). PCs are then reported
per function, and optionally per source line (objdump -l) for chosen functions. Game code (src/code, overlays,
audio) needs generated headers and is not symbolized: it shows as "game/other".

usage: prof_fit.py <prof.txt> <port dir at the build's commit> [--sdk DIR] [--top N] [--lines fn1,fn2]
  e.g. git archive <sha> port/src/port port/kit port/include | tar -x -C /tmp/b; prof_fit.py prof.txt /tmp/b/port
A hint, like any profile: check the fit line (lr hits / lr samples in range) before trusting a row.
"""
import bisect, collections, os, re, subprocess, sys, tempfile

def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

prof, port = sys.argv[1], sys.argv[2]
sdk = arg("--sdk", "/opt/miyoomini-toolchain")
top = int(arg("--top", "40"))
want_lines = [f for f in arg("--lines", "").split(",") if f]
pre = os.path.join(sdk, "usr", "bin", "arm-linux-gnueabihf-")
inc = os.path.join(sdk, "usr", "arm-linux-gnueabihf", "sysroot", "usr", "include", "SDL")
flags = ("-c -O2 -g -fno-math-errno -ffp-contract=off -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 "
         "-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -ffunction-sections -fdata-sections -w "
         "-fno-builtin-sinf -fno-builtin-cosf -include src/port/hh/hh_compat.h -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB "
         "-DDEBUG_FEATURES=0 -DOOT_VERSION=NTSC_1_0 -DOOT_REVISION=0 -DOOT_REGION=REGION_US "
         "-DLIBULTRA_VERSION=LIBULTRA_VERSION_L -DLIBULTRA_PATCH=0 -DPLATFORM_PSP=1 -DPLATFORM_N64=0 -DPLATFORM_GC=0 "
         "-DPLATFORM_IQUE=0 -DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE -DOOT_PSP_GFX_DIAGNOSTICS=0 "
         "-DOOT_PSP_AUDIO_DIAGNOSTICS=0 -DMML_VERSION=MML_VERSION_OOT -DTARGET_HANDHELD=1 -DHANDHELD=1 "
         f"-I{inc} -D_GNU_SOURCE=1 -D_REENTRANT -Isrc/port/hh/gfx -Ikit/platform -iquote build/hh-texbe -iquote include "
         "-iquote include/libc -iquote src -iquote . -iquote extracted/ntsc-1.0 -iquote src/port/hh "
         "-iquote src/port/hh/shim -iquote src/port/psp -Isrc/port/hh/shim -iquote build/ntsc-1.0").split()
# Makefile.handheld's HH_PORT_SOURCES order (the port layer is linked after the game objects)
order = sorted(f"src/port/hh/{f}" for f in os.listdir(os.path.join(port, "src/port/hh")) if f.endswith(".c"))
order += ["src/port/hh/gfx/gfx_pc.c", "src/port/hh/gfx/gfx_cc.c", "src/port/hh/gfx/gfx_soft.c", "src/port/hh/gfx/gfx_sdl12.c",
          "kit/platform/savestate.c", "kit/platform/hh_priority.c"]
tmp = tempfile.mkdtemp(prefix="prof_fit_")
layout, calls, objs = [], set(), {}
off = 0
for src in order:
    o = os.path.join(tmp, src.replace("/", "_") + ".o")
    extra = ["-D_FILE_OFFSET_BITS=64"] if src.endswith("hh_convert.c") else []
    r = subprocess.run([pre + "gcc"] + flags + extra + [src, "-o", o], cwd=port, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"# {src}: not compiled here (the layout after it may be shifted; fitted per object)", file=sys.stderr)
        continue
    objs[src] = o
    secs = []
    for line in subprocess.run([pre + "readelf", "-S", "-W", o], capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)\s+PROGBITS\s+\S+\s+\S+\s+(\S+)\s+\S+\s+(\S+)\s+\S+\s+\S+\s+(\d+)", line)
        if m and m.group(2).startswith(".text") and "X" in m.group(4):
            name = m.group(2)
            if name.startswith((".text.unlikely", ".text.startup", ".text.hot", ".text.exit")) or int(m.group(3), 16) == 0:
                continue
            secs.append((name, int(m.group(3), 16), int(m.group(5))))
    bl = collections.defaultdict(list)
    cur = None
    for line in subprocess.run([pre + "objdump", "-d", "-w", o], capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"Disassembly of section (\S+):", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(bl|blx)\b", line)
        if m and cur:
            bl[cur].append(int(m.group(1), 16) + 4)
    for name, size, al in secs:
        off = (off + al - 1) // al * al
        layout.append((off, size, name[6:], src))
        calls.update(off + b for b in bl[name])
        off += size

samples = {"pc": collections.Counter(), "lr": collections.Counter()}
for line in open(prof, errors="replace"):
    p = line.split()
    if len(p) == 3 and p[0] in samples:
        samples[p[0]][int(p[1], 16)] += int(p[2])
lrs = [(a, n) for a, n in samples["lr"].items() if 0x40000000 <= a < 0x41000000]
starts = [l[0] for l in layout]
# global base from the gfx objects (the hot ones), then a shift per object
score = collections.Counter()
cs = sorted(calls)
for a, n in sorted(lrs, key=lambda x: -x[1])[:40]:
    for c in cs:
        score[a - c] += n
base = score.most_common(1)[0][0]
spans = {}
prev = 0
for src in objs:
    lo = min((l[0] for l in layout if l[3] == src), default=None)
    if lo is None:
        continue
    hi = max(l[0] + l[1] for l in layout if l[3] == src)
    inr = [(a, n) for a, n in lrs if lo - 0x2000 <= a - base < hi + 0x2000]
    best = max(range(-0x1000, 0x1004, 4), key=lambda d: (sum(n for a, n in inr if lo <= a - base - d < hi and a - base - d in calls), -abs(d)))
    distinct = len({a for a, n in inr if lo <= a - base - best < hi and a - base - best in calls})
    hits = sum(n for a, n in inr if lo <= a - base - best < hi and a - base - best in calls)
    tot = sum(n for a, n in inr if lo <= a - base - best < hi)
    if distinct < 3:  # one or two return addresses fit anywhere: keep the previous object's shift
        best, how = prev, "inherited (too few distinct lr to fit: rows from this object are a guess)"
    else:
        how = f"lr hits {hits}/{tot} at {distinct} addresses"
    prev = best
    spans[src] = (lo, hi, best)
    print(f"# fit {src}: shift {best:+d}, {how}", file=sys.stderr)

def sym(a):
    for src, (lo, hi, d) in spans.items():
        o = a - base - d
        if lo <= o < hi:
            i = bisect.bisect_right(starts, o) - 1
            if i >= 0 and o < layout[i][0] + layout[i][1]:
                return layout[i], o - layout[i][0]
    return None, 0

tot = sum(samples["pc"].values())
per = collections.Counter()
hits_by_fn = collections.defaultdict(list)
for a, n in samples["pc"].items():
    ent, rel = sym(a)
    if ent:
        per[f"{ent[2]} [{ent[3].split('/')[-1]}]"] += n
        hits_by_fn[ent[2]].append((rel, n))
    else:
        per["shared libraries" if a >= 0xB0000000 else "game/other (not symbolized)"] += n
print(f"{tot} samples, base {base:#x}")
for k, v in per.most_common(top):
    print(f"{100.0 * v / tot:6.2f}% {v:5d} {k}")
for fn in want_lines:
    ent = next((l for l in layout if l[2] == fn), None)
    if not ent:
        continue
    lines, cur = {}, None
    for line in subprocess.run([pre + "objdump", "-d", "-l", "-w", f"--section=.text.{fn}", objs[ent[3]]],
                               capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"^(/\S+|\S+\.[ch]):(\d+)", line)
        if m:
            cur = os.path.basename(m.group(1)) + ":" + m.group(2)
            continue
        m = re.match(r"\s*([0-9a-f]+):\s", line)
        if m:
            lines[int(m.group(1), 16)] = cur
    agg = collections.Counter()
    for rel, n in hits_by_fn[fn]:
        agg[lines.get(rel, "?")] += n
    print(f"== {fn}: " + ", ".join(f"{k} {v}" for k, v in agg.most_common(12)))
