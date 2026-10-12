#!/usr/bin/env python3
"""Release gate 1b: a code object (whose bytes ship) may #include a file from
the decomp's untracked, ROM-derived data only if tools/assets/untracked_includes.txt
lists it with why none of its bytes ship. Reads the compiler's dependency files.

usage (in the build container, after a build): check_includes.py miyoo|pc
"""
import fnmatch, os, subprocess, sys

plat = sys.argv[1] if len(sys.argv) > 1 else 'miyoo'
root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
port = os.path.join(root, 'port'); build = 'build/us_%s' % plat
allow = []
for l in open(os.path.join(root, 'tools/assets/untracked_includes.txt')):
    l = l.strip()
    if l and not l.startswith('#'):
        allow.append(l.split()[0])
import re
srcs = [w for w in subprocess.run(['make', 'PLAT=' + plat, '-s', 'print-code-sources'], cwd=port,
                                  capture_output=True, text=True, check=True).stdout.split()
        if re.fullmatch(r'[A-Za-z0-9_./-]+\.(c|s|cpp)', w)]  # the Makefile's banner lines are not sources
tracked = set(subprocess.run(['git', 'ls-files', 'port'], cwd=root, capture_output=True, text=True,
                             check=True).stdout.split())
bad = {}; seen = set(); allowed = {}
for src in srcs:
    d = os.path.join(port, build, os.path.splitext(src)[0] + '.d')
    if not os.path.exists(d): sys.exit('ERROR: %s missing: build PLAT=%s first' % (d, plat))
    deps = open(d).read().replace('\\\n', ' ').split()
    for dep in deps:
        if dep.endswith(':') or dep.startswith('/'): continue  # targets, system and SDK headers
        dep = os.path.normpath(dep)  # src/hh/gfx/../port.h
        f = dep[len(build) + 1:] if dep.startswith(build + '/') else dep
        seen.add(f)
        if 'port/' + f in tracked or 'port/' + dep in tracked: continue
        if not os.path.exists(os.path.join(port, dep)): continue
        pat = next((p for p in allow if fnmatch.fnmatch(f, p)), None)
        if pat: allowed.setdefault(pat, set()).add(src)
        else: bad.setdefault(f, set()).add(src)
for pat in allow:
    print('  allowed %-28s %d code files' % (pat, len(allowed.get(pat, ()))))
for f, users in sorted(bad.items()):
    print('UNTRACKED include not on the allowlist: %s (in %s)' % (f, ', '.join(sorted(users)[:4])))
print('includes: %d code sources, %d files included, %d untracked not allowed' % (len(srcs), len(seen), len(bad)))
if len(srcs) < 90: sys.exit('ERROR: only %d code sources listed - the check did not run' % len(srcs))
sys.exit(1 if bad else 0)
