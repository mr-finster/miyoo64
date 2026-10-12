#!/usr/bin/env python3
"""qlogcount.py <log> [binary sym n]: executed guest instructions per symbol from a qemu-user run with
`-d in_asm,exec,nochain -D <log>` -- for a qemu built without TCG plugin support (insnprof.c needs one).
ARM-mode blocks count 4 bytes an instruction (Thumb blocks, e.g. a distro's static libc, are approximated).
With <binary sym n>: the n hottest blocks of that symbol, disassembled (objdump of <binary>).
A hint only: qemu counts instructions, not the Cortex-A7's stalls; the device Bench decides (round 72)."""
import collections, re, subprocess, sys

tb, cnt, blk, execs = {}, collections.Counter(), collections.Counter(), collections.Counter()
want = sys.argv[3] if len(sys.argv) > 3 else None
cur_pc, cur_len = None, 0
with open(sys.argv[1], errors='replace') as f:
    for l in f:
        if l.startswith('Trace '):
            m = re.search(r'\[[0-9a-f]+/([0-9a-f]+)/[0-9a-f]+/[0-9a-f]+\]\s*(\S*)', l)
            pc, sym = int(m.group(1), 16), m.group(2) or '?'
            n = tb.get(pc, 0)
            cnt[sym] += n
            if sym == want:
                blk[pc] += n; execs[pc] += 1
        elif l.startswith('IN:'):
            cur_pc, cur_len = None, 0
        elif cur_pc is None and re.match(r'^0x[0-9a-f]+:', l):
            cur_pc = int(l.split(':')[0], 16)
        elif l.startswith('OBJD-'):
            cur_len += len(l.split(':', 1)[1].strip()) // 2
            tb[cur_pc] = cur_len // 4 if l[5] == 'A' else cur_len // 3
tot = sum(cnt.values()) or 1
print('total %d' % tot)
for s, c in cnt.most_common(12):
    print('%12d %5.1f%% %s' % (c, 100.0 * c / tot, s))
if want:
    dis = subprocess.run(['arm-linux-gnueabihf-objdump', '-d', '--no-show-raw-insn', sys.argv[2]],
                         capture_output=True, text=True).stdout
    lines = {int(m.group(1), 16): m.group(2) for m in (re.match(r'^\s*([0-9a-f]+):\s*(.*)$', x) for x in dis.splitlines()) if m}
    for pc, c in blk.most_common(int(sys.argv[4]) if len(sys.argv) > 4 else 8):
        print('--- 0x%x: %d insns, %d runs, %d long' % (pc, c, execs[pc], tb[pc]))
        for i in range(tb[pc]):
            print('   ', lines.get(pc + 4 * i, '?'))
