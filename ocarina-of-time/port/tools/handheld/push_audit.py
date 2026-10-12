#!/usr/bin/env python3
"""Pre-push audit: no ROM or ROM-derived bytes in what git is about to publish.

usage: push_audit.py <ref blob>... [--range A..B]
  Scans every file staged in the index (default) or changed in the commit range
  for 32-byte windows (>= 6 distinct byte values, so padding and simple ramps are
  ignored) that also occur in any reference blob: the ROM itself and the data
  built from it (build/hh-miyoo/embedded.bin, build/hh-gen/data/segments/*.bin).
  Also refuses binary files. Exit 1 on any hit. The blobs are only read locally.
"""
import subprocess
import sys

W = 32


def windows(blob):
    out = set()
    for i in range(0, len(blob) - W, 4):
        w = blob[i:i + W]
        if len(set(w)) >= 6:
            out.add(w)
    return out


def main():
    args = sys.argv[1:]
    rng = None
    if "--range" in args:
        i = args.index("--range")
        rng = args[i + 1]
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    if rng:
        names = subprocess.run(["git", "diff", "--name-only", "--diff-filter=AM", rng],
                               capture_output=True, text=True, check=True).stdout.split()
        read = lambda n: subprocess.run(["git", "show", f"{rng.split('..')[-1] or 'HEAD'}:{n}"],
                                        capture_output=True, check=True).stdout
    else:
        names = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=AM"],
                               capture_output=True, text=True, check=True).stdout.split()
        read = lambda n: subprocess.run(["git", "show", f":{n}"], capture_output=True, check=True).stdout
    ref = set()
    for path in args:
        ref |= windows(open(path, "rb").read())
    print(f"{len(names)} files vs {len(ref):,} reference windows")
    bad = 0
    for n in names:
        d = read(n)
        if b"\0" in d[:8192]:
            print(f"  BINARY {n}")
            bad += 1
            continue
        hits = sum(1 for i in range(len(d) - W) if d[i:i + W] in ref)
        if hits:
            print(f"  {hits:5d} windows  {n}")
            bad += 1
    print("audit:", "FAIL" if bad else "clean")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
