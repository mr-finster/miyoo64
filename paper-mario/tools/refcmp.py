# refcmp.py <ref.png> <ours_before.ppm> <ours_after.ppm> <out.png> [x0 y0 x1 y1 zoom]
# The reference (m64ref, 640x480 = the 320x240 framebuffer at 2x) is brought to our output geometry:
# halved to 320x240, then squeezed 4:3 to a square (box filters). Panels: reference | before | after.
# Fails loudly (exit 1) on a missing input, a reference that isn't 4:3, or renders that aren't square.
import sys, os
from PIL import Image

def die(msg):
    print('refcmp.py: ' + msg, file=sys.stderr)
    sys.exit(1)

if len(sys.argv) not in (5, 10): die('usage: refcmp.py <ref.png> <before.ppm> <after.ppm> <out.png> [x0 y0 x1 y1 zoom]')
for f in sys.argv[1:4]:
    if not os.path.isfile(f): die(f'no such file: {f}')
ref = Image.open(sys.argv[1]).convert('RGB')
if ref.size[0] * 3 != ref.size[1] * 4: die(f'reference {sys.argv[1]} is {ref.size}, not 4:3')
ref = ref.resize((320, 240), Image.BOX).resize((240, 240), Image.BOX)
a = Image.open(sys.argv[2]).convert('RGB'); b = Image.open(sys.argv[3]).convert('RGB')
for p, n in ((a, sys.argv[2]), (b, sys.argv[3])):
    if p.size != (240, 240): die(f'{n} is {p.size}, not 240 square')
panels = [ref, a, b]
if len(sys.argv) == 10:
    x0, y0, x1, y1, z = map(int, sys.argv[5:10])
    if not (0 <= x0 < x1 <= 240 and 0 <= y0 < y1 <= 240 and z >= 1): die(f'bad crop {x0},{y0}-{x1},{y1} x{z}')
    panels = [p.crop((x0, y0, x1, y1)).resize(((x1 - x0) * z, (y1 - y0) * z), Image.NEAREST) for p in panels]
w, h = panels[0].size
s = Image.new('RGB', (w * 3 + 16, h), (255, 255, 255))
for i, p in enumerate(panels): s.paste(p, (i * (w + 8), 0))
s.save(sys.argv[4])
print(f'refcmp.py: wrote {sys.argv[4]} ({s.size[0]}x{s.size[1]})')
