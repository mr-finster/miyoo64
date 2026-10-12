#!/usr/bin/env python3
"""halfwater_sheets.py: before/after sheets for the half-resolution water test (2026-10-02).

  halfwater_sheets.py <out.png> <title> <before.ppm> <after.ppm> [<before2.ppm> <after2.ppm> ...] [--crop x,y[,size]]...

Per pair, one row: before and after at 1:1 (square frames), the difference x4, then a 3x nearest zoom of before and after
around the spot that differs most (or each --crop x,y given, size 80 by default). Frames stay local (ROM-derived)."""
import sys
from PIL import Image, ImageChops, ImageDraw

def main():
    args = sys.argv[1:]
    out, title = args[0], args[1]
    crops, files = [], []
    i = 2
    while i < len(args):
        if args[i] == '--crop':
            v = [int(t) for t in args[i + 1].split(',')]
            crops.append((v[0], v[1], v[2] if len(v) > 2 else 80))
            i += 2
        else:
            files.append(args[i]); i += 1
    pairs = [(files[k], files[k + 1]) for k in range(0, len(files) - 1, 2)]
    rows = []
    for n, (fa, fb) in enumerate(pairs):
        a, b = Image.open(fa).convert('RGB'), Image.open(fb).convert('RGB')
        d = ImageChops.difference(a, b)
        dl = d.convert('L')
        amp = dl.point(lambda v: min(255, v * 4)).convert('RGB')
        if n < len(crops):
            cx, cy, sz = crops[n]
        else: # the 80x80 window with the largest summed difference (16-px steps)
            best, cx, cy, sz = -1, 120, 120, 80
            for y in range(40, 201, 16):
                for x in range(40, 201, 16):
                    s = sum(dl.crop((x - 40, y - 40, x + 40, y + 40)).getdata())
                    if s > best: best, cx, cy = s, x, y
        box = (max(0, cx - sz // 2), max(0, cy - sz // 2))
        box = (min(box[0], 240 - sz), min(box[1], 240 - sz))
        box = box + (box[0] + sz, box[1] + sz)
        za = a.crop(box).resize((sz * 3, sz * 3), Image.NEAREST)
        zb = b.crop(box).resize((sz * 3, sz * 3), Image.NEAREST)
        h = max(240, sz * 3)
        row = Image.new('RGB', (240 * 3 + sz * 6 + 40, h + 18), (24, 24, 24))
        dr = ImageDraw.Draw(row)
        for k, (im, lab) in enumerate(((a, 'before (off)'), (b, 'after (on)'), (amp, 'difference x4'))):
            row.paste(im, (k * 240 + (10 if k else 0), 18))
            dr.text((k * 240 + 4 + (10 if k else 0), 3), lab, fill=(230, 230, 230))
        for k, im in enumerate((a, b)):
            dr.rectangle((box[0] + k * 250, box[1] + 18, box[2] + k * 250 - 1, box[3] + 17), outline=(255, 255, 0))
        row.paste(za, (240 * 3 + 30, 18)); row.paste(zb, (240 * 3 + 30 + sz * 3 + 10, 18))
        dr.text((240 * 3 + 34, 3), 'before 3x', fill=(230, 230, 230))
        dr.text((240 * 3 + 44 + sz * 3, 3), 'after 3x', fill=(230, 230, 230))
        rows.append(row)
    W = max(r.width for r in rows)
    sheet = Image.new('RGB', (W, 22 + sum(r.height + 6 for r in rows)), (12, 12, 12))
    ImageDraw.Draw(sheet).text((6, 5), title, fill=(255, 255, 255))
    y = 22
    for r in rows:
        sheet.paste(r, (0, y)); y += r.height + 6
    sheet.save(out)

if __name__ == '__main__':
    main()
