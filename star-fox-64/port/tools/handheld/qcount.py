#!/usr/bin/env python3
"""qcount.py: total executed guest instructions from `qemu-arm -d in_asm,exec,nochain` on stdin (pipe it, never
write the trace to disk). ARM-mode blocks count 4 bytes an instruction. A hint only: the device decides."""
import re, sys
tb, tot, cur_pc, cur_len = {}, 0, None, 0
pat = re.compile(r'\[[0-9a-f]+/([0-9a-f]+)/')
for l in sys.stdin:
    if l.startswith('Trace '):
        m = pat.search(l)
        if m: tot += tb.get(int(m.group(1), 16), 0)
    elif l.startswith('IN:'):
        cur_pc, cur_len = None, 0
    elif cur_pc is None and re.match(r'^0x[0-9a-f]+:', l):
        cur_pc = int(l.split(':')[0], 16)
    elif l.startswith('OBJD-'):
        cur_len += len(l.split(':', 1)[1].strip()) // 2
        tb[cur_pc] = cur_len // 4
print(tot)
