#!/usr/bin/env python3
"""sprof.py -- the device profile (game/src/port/sdl/sprof.c, the Bench's "ship, prof" runs; after goldeneye-007's tools/sprof.py) as tables.

usage (in the dev container, where the ARM binutils are):
  tools/sprof.py <bench_prof.bin> [binary] [--lines FUNC ...] [--top N]
    binary: the UNSTRIPPED build of the game folder that ran (default game/build/miyoo/fzerox; the game folder's
            fzerox is the same code, so its addresses match -- but only for that build)
Per block (one per profiled run): samples per function of the executable, per shared library
(libc, SDL, the kernel's vdso; with the executable function at lr, its caller when the library
function is a leaf), and "(kernel)" for samples that interrupted a system call; then,
for each --lines FUNC, samples per source line inside it (addr2line). Each sample is ~FZX_SPROF_US
of wall time, so a function's share is its share of the frame time the bench measured.
"""
import bisect, collections, os, struct, subprocess, sys

args = [a for a in sys.argv[1:]]
lines_for = []
top = 40
if '--lines' in args:
    i = args.index('--lines'); lines_for = args[i + 1:]; args = args[:i]
if '--top' in args:
    i = args.index('--top'); top = int(args[i + 1]); del args[i:i + 2]
path = args[0]
binary = args[1] if len(args) > 1 else 'game/build/miyoo/fzerox'
TC = '/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-'

def nm(obj, dynamic=False):
    out = subprocess.run([TC + 'nm', '-n'] + (['-D'] if dynamic else []) + [obj],
                         capture_output=True, text=True).stdout
    syms = []
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 3 and p[1] in 'TtWw':
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    return syms

def lookup(syms, addrs, a):
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][1] if i >= 0 else '?'

exe_syms = nm(binary); exe_addrs = [a for a, _ in exe_syms]
lib_cache = {}
SYSROOT = '/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot'

def lib_syms(name):
    if name not in lib_cache:
        cand = [os.path.join(SYSROOT, name.lstrip('/')), os.path.join(SYSROOT, 'usr/lib', os.path.basename(name)),
                os.path.join(SYSROOT, 'lib', os.path.basename(name))]
        s = []
        for c in cand:
            if os.path.exists(c):
                s = nm(c, True) or nm(c)
                break
        lib_cache[name] = (s, [a for a, _ in s])
    return lib_cache[name]

data = open(path, 'rb').read()
pos = 0
while pos < len(data):
    nl = data.index(b'\n', pos); head = data[pos:nl].decode(); pos = nl + 1
    if not head.startswith('FZXSPROF1'):
        break
    nl = data.index(b'\n', pos); n, lost = map(int, data[pos:nl].split()); pos = nl + 1
    samples = [struct.unpack_from('<II', data, pos + 8 * i) for i in range(n)]; pos += 8 * n
    maps = []
    while True:
        nl = data.index(b'\n', pos); l = data[pos:nl].decode(errors='replace'); pos = nl + 1
        if l == 'END':
            break
        p = l.split()
        lo, hi = (int(x, 16) for x in p[0].split('-'))
        off = int(p[2], 16)
        maps.append((lo, hi, off, p[5] if len(p) > 5 else '[anon]'))
    exe_name = os.path.basename(maps[0][3]) if maps else ''
    fn = collections.Counter(); per_pc = collections.defaultdict(collections.Counter)
    callers = collections.defaultdict(collections.Counter) # library function -> the executable function at lr
    for pc, lr in samples:
        m = next((m for m in maps if m[0] <= pc < m[1]), None)
        if m is None:
            fn['(kernel / unmapped)'] += 1
            continue
        if m[3].endswith(('fzerox', 'fzerox_bench')):
            f = lookup(exe_syms, exe_addrs, pc)
            fn[f] += 1; per_pc[f][pc] += 1
        else:
            s, a = lib_syms(m[3])
            rel = pc - m[0] + m[2]
            f = '%s:%s' % (os.path.basename(m[3]), lookup(s, a, rel) if s else '?')
            fn[f] += 1
            ml = next((m2 for m2 in maps if m2[0] <= lr < m2[1]), None)
            if ml and ml[3].endswith(('fzerox', 'fzerox_bench')):
                who = lookup(exe_syms, exe_addrs, lr)
            elif ml:  # a library function: name it too (lr inside SDL, libc, ...)
                s2, a2 = lib_syms(ml[3])
                who = '%s:%s' % (os.path.basename(ml[3]), lookup(s2, a2, lr - ml[0] + ml[2]) if s2 else '?')
            else:
                who = '?'
            callers[f][who] += 1
    print('== %s: %d samples (%d lost)' % (head[10:], n, lost))
    for f, c in fn.most_common(top):
        print('  %6d  %5.1f%%  %s' % (c, 100.0 * c / max(n, 1), f))
    for f, c in fn.most_common(top):
        if f in callers and c * 200 >= n:
            print('  %s called from (lr): %s' % (f, ', '.join('%s %d' % kv for kv in callers[f].most_common(4))))
    for want in lines_for:
        pcs = per_pc.get(want)
        if not pcs:
            print('  (no samples in %s)' % want); continue
        out = subprocess.run([TC + 'addr2line', '-e', binary] + ['%x' % p for p in pcs], capture_output=True, text=True).stdout.split()
        byline = collections.Counter()
        for (p, c), l in zip(pcs.items(), out):
            byline[l.split('/')[-1]] += c
        tot = sum(pcs.values())
        print('  -- %s by line (%d samples)' % (want, tot))
        for l, c in byline.most_common(25):
            print('     %6d  %5.1f%%  %s' % (c, 100.0 * c / tot, l))
