#!/usr/bin/env python3
"""Reproduce the validated Splat 0.31.0 provider adapter for the pinned decomp.

The pinned tree does not contain tools/splat. Use the provider already installed
in pm64hh:decomp; this command never downloads or installs anything.
"""
import argparse
from importlib import metadata, import_module
from pathlib import Path

ADAPTER = '.PHONY: all\nall:\n\t@python3 -c "import importlib.metadata; assert importlib.metadata.version(\'splat64\') == \'0.31.0\'; import splat.scripts.split"\n'

def prepare(root):
    if metadata.version('splat64') != '0.31.0':
        raise RuntimeError('require the validated installed splat64 0.31.0 provider')
    import_module('splat.scripts.split')
    path = Path(root)/'tools/splat/Makefile'
    if path.exists():
        if path.read_text() != ADAPTER:
            raise FileExistsError('refusing to replace a different Splat provider adapter')
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x') as stream:
        stream.write(ADAPTER)
    return path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decomp-root',type=Path,default=Path(__file__).resolve().parents[2]/'refs/papermario')
    args=parser.parse_args()
    print(prepare(args.decomp_root))
if __name__=='__main__':main()
