#!/usr/bin/env python3
"""Generate top-level ROM segment ranges from a region's Splat configuration.

No arguments preserves the US output: build/game/rom_segments.json.
--region pal defaults to build/game/pal/rom_segments.json; --output PATH
selects another destination (relative paths are relative to the working directory).
Only refs/papermario/ver/<region>/splat.yaml is read, never ROM data.
Requires PyYAML, already provided by the pm64hh:decomp image.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import secrets
import stat
import sys

import yaml

ROOT = Path(__file__).resolve().parents[2]


def seg_name(name, dir_, type_):
    # Preserve Splat naming: explicit name, directory, then pm_ extension type.
    n = name or dir_ or (type_[3:] if isinstance(type_, str) and type_.startswith('pm_') else None)
    if n is not None and not isinstance(n, str):
        raise ValueError('segment names and directories must be strings')
    return n.replace('/', '_') if n else None


def segment_ranges(cfg):
    if not isinstance(cfg, dict) or not isinstance(cfg.get('segments'), list):
        raise ValueError('splat.yaml must contain a segments list')
    segs = []
    for index, entry in enumerate(cfg['segments']):
        if isinstance(entry, dict):
            start, kind = entry.get('start'), entry.get('type')
            name, directory = entry.get('name'), entry.get('dir')
        elif isinstance(entry, list) and entry:
            start = entry[0]
            kind = entry[1] if len(entry) > 1 else None
            name = entry[2] if len(entry) > 2 else None
            directory = None
        else:
            raise ValueError(f'segment {index}: expected a mapping or nonempty list')
        # As before, unresolved/missing starts do not participate in ROM ranges.
        if start is None or start == 'auto':
            continue
        if type(start) is not int or start < 0:
            raise ValueError(f'segment {index}: ROM start must be a nonnegative integer or auto')
        # BSS-only segments legitimately share a ROM start with their successor.
        # Keep zero-length ranges exactly as the original US generator did.
        if segs and start < segs[-1]['rom_start']:
            raise ValueError(f'segment {index}: ROM starts must not decrease')
        if kind is not None and not isinstance(kind, str):
            raise ValueError(f'segment {index}: type must be a string')
        segs.append({'name': seg_name(name, directory, kind), 'type': kind,
                     'rom_start': start})
    if not segs:
        raise ValueError('no segments in splat.yaml')
    for index, segment in enumerate(segs):
        segment['rom_end'] = segs[index + 1]['rom_start'] if index + 1 < len(segs) else None
    return segs


def resolved_output(output):
    try:
        try:
            return output.resolve(strict=True)
        except FileNotFoundError:
            return output.resolve()
    except (RuntimeError, OSError) as error:
        if isinstance(error, OSError) and error.errno != errno.ELOOP:
            raise
        raise ValueError(f'cannot resolve output path (symlink loop): {output}') from error


def write_atomic(output, segs, protected=None):
    """Follow output symlinks, then atomically replace the final regular target.

    Paths must not be concurrently retargeted; this is a local build tool,
    not a security boundary against changes to parent directories or symlinks.
    """
    target = resolved_output(output)
    if protected is not None and target == resolved_output(protected):
        raise ValueError('PAL output must not overwrite the US default build/game/rom_segments.json')
    # Match the old writer: create the requested output's parents, but do not
    # create missing directories behind a dangling file symlink.
    output.parent.mkdir(parents=True, exist_ok=True)
    if not target.parent.is_dir():
        raise ValueError(f'output target directory does not exist: {target.parent}')
    try:
        metadata = target.stat()
    except FileNotFoundError:
        mode = None
    else:
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError(f'output target must be a regular file: {target}')
        mode = stat.S_IMODE(metadata.st_mode)
    temporary = None
    descriptor = None
    try:
        candidate = target.parent / f'.rom_segments-{secrets.token_hex(16)}.tmp'
        # O_EXCL prevents collisions from touching another file. For new files,
        # the kernel applies umask to 0666, just as open(..., 'w') used to do.
        # Existing targets start with no broader permissions than their mode.
        descriptor = os.open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                             mode if mode is not None else 0o666)
        temporary = candidate
        with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
            descriptor = None  # owned and closed by stream from here
            json.dump(segs, stream, indent=0)
            stream.flush()
            if mode is not None:
                os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        os.replace(temporary, target)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--region', choices=('us', 'pal'), default='us',
                        help='Splat region (default: us)')
    parser.add_argument('-o', '--output', type=Path, help='JSON destination')
    args = parser.parse_args(argv)
    us_output = ROOT / 'build/game/rom_segments.json'
    output = args.output if args.output is not None else (
        us_output if args.region == 'us' else ROOT / 'build/game/pal/rom_segments.json')
    try:
        config = ROOT / 'refs/papermario/ver' / args.region / 'splat.yaml'
        with config.open(encoding='utf-8') as stream:
            segs = segment_ranges(yaml.safe_load(stream))
        write_atomic(output, segs, protected=us_output if args.region == 'pal' else None)
    except (OSError, ValueError, yaml.YAMLError) as error:
        print(f'rom_segments.py: {error}', file=sys.stderr)
        return 1
    print(f'rom_segments.py: {len(segs)} segments, ROM 0x{segs[0]["rom_start"]:X}..0x{segs[-1]["rom_start"]:X}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
