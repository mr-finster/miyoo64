#!/usr/bin/env python3
"""Objects for the final links: copies with the selected sections renamed.

usage: hh_objects.py <hhasset.txt> <out dir> <objcopy> <out objects.rsp> <objects...>

hhasset.txt (derive_embedded.py select) lists "<object> <section>" pairs: data
the ROM reproduces. Each such section becomes .hhasset.<section> in a copy of
its object under <out dir> (same relative path, so the overlay grouping
patterns still see /overlays/<ovl>/); the originals are untouched. The
response file lists every object the final links take, in the original order.
"""
import collections, os, shutil, subprocess, sys

hhlist, out_dir, objcopy, rsp = sys.argv[1:5]
objects = sys.argv[5:]
rename = collections.defaultdict(list)
for line in open(hhlist):
    obj, sec = line.split()
    rename[obj].append(sec)

final, copies = [], 0
for o in objects:
    secs = rename.get(o)
    if not secs:
        final.append(o)
        continue
    dst = os.path.join(out_dir, os.path.relpath(o))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    args = [objcopy]
    for sec in secs:
        args += ['--rename-section', f'{sec}=.hhasset{sec}']
    tmp = dst + '.tmp'
    shutil.copyfile(o, tmp)
    subprocess.run(args + [tmp], check=True)
    os.replace(tmp, dst)
    final.append(dst)
    copies += 1

missing = set(rename) - set(objects)
if missing:
    sys.exit(f'hh_objects: listed objects not in the link: {sorted(missing)[:3]}')
with open(rsp, 'w') as f:
    f.write('\n'.join(final) + '\n')
print(f'hh_objects: {sum(len(v) for v in rename.values())} sections renamed in {copies} object copies')
