#!/usr/bin/env python3
"""rom_segments.py -- every top-level segment of the decomp's splat.yaml with its ROM range (M2).

Writes build/game/rom_segments.json: [{name, type, rom_start, rom_end}] in ROM order. Runs in the pm64hh:decomp image
(it needs PyYAML): docker run --rm -v $PWD:/work -w /work pm64hh:decomp python3 tools/port/rom_segments.py
The numbers are the segment table of the decomp's own source (splat.yaml), not ROM content.
"""
import json, os, sys
import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
cfg = yaml.safe_load(open(os.path.join(ROOT, 'refs/papermario/ver/us/splat.yaml')))
segs = []
def seg_name(name, dir_, type_):
    # splat's naming, as the linker script shows it: the name, else the dir, with '/' -> '_' (battle/area/arn ->
    # battle_area_arn, title/bg_1 -> title_bg_1); an unnamed extension segment takes its type (pm_msg -> msg)
    n = name or dir_ or (type_[3:] if isinstance(type_, str) and type_.startswith('pm_') else None)
    return n.replace('/', '_') if n else None

for s in cfg['segments']:
    if isinstance(s, dict):
        segs.append({'name': seg_name(s.get('name'), s.get('dir'), s.get('type')), 'type': s.get('type'), 'rom_start': s.get('start')})
    elif isinstance(s, list):
        segs.append({'name': seg_name(s[2] if len(s) > 2 else None, None, s[1] if len(s) > 1 else None),
                     'type': s[1] if len(s) > 1 else None, 'rom_start': s[0]})
segs = [s for s in segs if isinstance(s['rom_start'], int)]
for i, s in enumerate(segs):
    s['rom_end'] = segs[i + 1]['rom_start'] if i + 1 < len(segs) else None
if not segs:
    sys.exit('rom_segments.py: no segments in splat.yaml')
os.makedirs(os.path.join(ROOT, 'build/game'), exist_ok=True)
json.dump(segs, open(os.path.join(ROOT, 'build/game/rom_segments.json'), 'w'), indent=0)
print(f'rom_segments.py: {len(segs)} segments, ROM 0x{segs[0]["rom_start"]:X}..0x{segs[-1]["rom_start"]:X}')
