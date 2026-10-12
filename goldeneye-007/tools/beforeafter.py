#!/usr/bin/env python3
"""beforeafter.py -- the sheet every screen-changing fix is shown with: before | after | the
real game (m64ref z64) at the same instant, native 1:1 at the port's square size, and under them a labelled
zoom of the same area of all three.

    tools/beforeafter.py OUT.png --title "..." --row BEFORE AFTER [REAL] --zoom X,Y,W,H[,F]
                         [--real-map band|square|crop|fit] [--note "..."] [--row ...]

BEFORE/AFTER are the port's square frame dumps (.ppm/.png). REAL is an m64ref frame (4:3):
--real-map band puts it where the N64's view lies on the square 3D view (full width, rows
30..210: the fill projection keeps the N64's horizontal view), square squeezes the whole frame
onto the square as the port does for 2D screens and the folder menus, crop keeps the
centre 240 of its 320 columns at 1:1 (the first-person gun's framing), fit letterboxes any
other picture whole (a reference the user sends). --zoom is in the port's
frame pixels (F: magnification, default 3; nearest-neighbour, so pixels stay pixels).
Several --row/--zoom/--note groups stack vertically."""
import argparse
from PIL import Image, ImageDraw

T = 240
GAP = 8
LAB = 14
BG = (22, 22, 22)


def real_panel(path, mode):
    img = Image.open(path).convert('RGB')
    panel = Image.new('RGB', (T, T), (0, 0, 0))
    if mode == 'band':
        panel.paste(img.resize((T, T * 3 // 4), Image.BILINEAR), (0, (T - T * 3 // 4) // 2))
    elif mode == 'crop':  # the N64 frame's centre 240 columns at 1:1 (4:3 -> square by cropping)
        w, h = img.size
        panel = img.crop((w // 8, 0, w - w // 8, h)).resize((T, T), Image.BILINEAR)
    elif mode == 'fit':  # any other picture (a user's reference): whole, aspect kept, letterboxed
        w, h = img.size
        s = min(T / w, T / h)
        fw, fh = max(1, round(w * s)), max(1, round(h * s))
        panel.paste(img.resize((fw, fh), Image.BILINEAR), ((T - fw) // 2, (T - fh) // 2))
    else:
        panel = img.resize((T, T), Image.BILINEAR)
    return panel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--title', default='')
    ap.add_argument('--row', nargs='+', action='append', required=True)
    ap.add_argument('--zoom', action='append', default=[])
    ap.add_argument('--note', action='append', default=[])
    ap.add_argument('--real-map', action='append', default=[])
    ap.add_argument('--labels', default='BEFORE,AFTER,N64 (z64)')
    a = ap.parse_args()
    labels = a.labels.split(',')
    groups = []
    for i, row in enumerate(a.row):
        zoom = [int(v) for v in a.zoom[i].split(',')] if i < len(a.zoom) else None
        if zoom and len(zoom) == 4:
            zoom.append(3)
        mode = a.real_map[i] if i < len(a.real_map) else 'band'
        note = a.note[i] if i < len(a.note) else ''
        # a panel that isn't a square port frame is an m64ref frame, mapped like REAL
        panels = [Image.open(p).convert('RGB') for p in row[:3]]
        panels = [p if p.size == (T, T) else real_panel(path, mode) for p, path in zip(panels, row)]
        groups.append((row, panels, zoom, note))
    width = 3 * T + 2 * GAP
    height = LAB + 4
    for row, panels, zoom, note in groups:
        height += LAB + T + GAP
        if zoom:
            height += LAB + zoom[3] * zoom[4] + GAP
        if note:
            height += LAB + 2
    sheet = Image.new('RGB', (width, height), BG)
    d = ImageDraw.Draw(sheet)
    d.text((2, 2), a.title, fill=(255, 255, 255))
    y = LAB + 4
    colours = [(255, 130, 130), (110, 255, 140), (255, 230, 90)]
    for row, panels, zoom, note in groups:
        for k, p in enumerate(panels):
            name = row[k].split('/')[-1]
            d.text((k * (T + GAP) + 2, y), '%s  %s' % (labels[k], name), fill=colours[k])
            sheet.paste(p, (k * (T + GAP), y + LAB))
            if zoom:
                x0, y0, w, h, f = zoom
                d.rectangle([k * (T + GAP) + x0 - 1, y + LAB + y0 - 1, k * (T + GAP) + x0 + w, y + LAB + y0 + h],
                            outline=(255, 0, 255))
        y += LAB + T + GAP
        if zoom:
            x0, y0, w, h, f = zoom
            for k, p in enumerate(panels):
                d.text((k * (T + GAP) + 2, y), 'ZOOM x%d  %s' % (f, labels[k]), fill=colours[k])
                crop = p.crop((x0, y0, x0 + w, y0 + h)).resize((w * f, h * f), Image.NEAREST)
                sheet.paste(crop, (k * (T + GAP), y + LAB))
            y += LAB + h * f + GAP
        if note:
            d.text((2, y), note, fill=(200, 200, 200))
            y += LAB + 2
    sheet.save(a.out)
    print(a.out, sheet.size)


if __name__ == '__main__':
    main()
