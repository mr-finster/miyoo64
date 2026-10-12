#!/usr/bin/env python3
"""tour_sheets.py <tourdir> <outprefix> [per_sheet] [cols]: native-240 sheets, one row per scene (its first cols frames)."""
import os, sys
from PIL import Image, ImageDraw
d, pre = sys.argv[1], sys.argv[2]; per = int(sys.argv[3]) if len(sys.argv) > 3 else 6
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 3
names = sorted(n for n in os.listdir(d) if os.path.isdir(os.path.join(d, n)))
for s in range(0, len(names), per):
    grp = names[s:s + per]
    S = Image.new('RGB', (cols * 244 + 4, len(grp) * 258), 'white'); dr = ImageDraw.Draw(S)
    for r, n in enumerate(grp):
        dr.text((4, r * 258 + 2), n, fill=(0, 0, 0))
        fs = sorted(f for f in os.listdir(os.path.join(d, n)) if f.endswith('.ppm'))[:cols]
        for k, f in enumerate(fs):
            S.paste(Image.open(os.path.join(d, n, f)).convert('RGB'), (4 + k * 244, r * 258 + 16))
    S.save('%s_%02d.png' % (pre, s // per + 1))
print((len(names) + per - 1) // per, 'sheets')
