#!/usr/bin/env python3
"""Pack a port's Miyoo Mini build into ONE zip that unzips onto the SD card's root and
works on every supported Miyoo OS at once (the shared miyoo/, KM1).

    miyoo_pack.py <port>/miyoo/game.json --build <dir with the files> -o <out.zip>
                  [--rom <the player's dump>]      # personal / builder-page zip: rom.z64 inside
                  [--layouts app,onion-ports,minui,allium] [--check]

The game lives once, in App/<folder>/ (Onion, the stock OS and spruceOS list App/); every other
OS gets a tiny launcher that hands over to App/<folder>/launch.sh (templates/forward.sh), so the
game always starts the same way, keeps one data folder, and one copy of the binary is enough.

game.json (one per port, in the port repo's miyoo/):
  id        "mk64"                     short id (the builder page's GAMES key)
  title     "Mario Kart 64"            menu name
  folder    "MarioKart64"              App/<folder>
  exe       "mk64"                     the executable in --build
  files     ["mk64", "recipes.bin"]    everything from --build that ships (code and numbers only)
  icon      "icon.png"                 relative to game.json; 8-bit RGBA PNG, scaled up to 64x64
  env       "MK64"                     the port's environment prefix (<ENV>_DATA, <ENV>_ROTATE)
  data      "data" (default) or "."    <ENV>_DATA: App/<folder>/data, or App/<folder> itself for a
                                       port that makes its own data/ there (MK64, DKR)
  ports_category ""  (default)         Onion's Ports list: "" puts the shortcut straight in
                                       Roms/PORTS/Shortcuts (listed in Ports itself); a name makes a
                                       category folder there, as the Ports Collection's ("Racing")
  rom       {"name": "Mario Kart 64 (USA)", "sha1": "...", "size": 12582912}
  system    "MARIOKART64"              Onion's Games system folder (Emu/<system>, Roms/<system>; only with
                                       --layouts ...,onion-games, no longer a default)
  minui_tag "MK64"                     MinUI's folder tag: Roms/<title> (<tag>)/

--rom: the dump in any byte order is checked against rom.sha1 and written as App/<folder>/rom.z64
(big-endian), the auto-ROM contract every port's code checks first. A zip made with --rom holds the player's ROM: never share it.
"""
import argparse, hashlib, json, os, re, stat, struct, sys, zipfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = os.path.join(HERE, '..', 'templates')
ALL_LAYOUTS = ['app', 'onion-games', 'onion-ports', 'minui', 'allium']
# The default: on Onion each game shows in Apps and in Ports. The one-system-per-game Games entry
# (onion-games) doubled every game there and is no longer packed (2026-10-12); it stays available by name.
DEFAULT_LAYOUTS = ['app', 'onion-ports', 'minui', 'allium']


def tpl(name, **kv):
    s = open(os.path.join(TEMPLATES, name)).read()
    for k, v in kv.items():
        s = s.replace('@' + k + '@', v)
    left = re.findall(r'@[A-Z_]+@', s)
    if left:
        sys.exit('template %s: unfilled %s' % (name, sorted(set(left))))
    return s


def rom_z64(path, want_sha1, want_size):
    d = bytearray(open(path, 'rb').read())
    if d[:4] == b'\x37\x80\x40\x12':
        d[0::2], d[1::2] = d[1::2], d[0::2]
    elif d[:4] == b'\x40\x12\x37\x80':
        for i in range(0, len(d) - 3, 4):
            d[i:i + 4] = d[i:i + 4][::-1]
    elif d[:4] != b'\x80\x37\x12\x40':
        sys.exit('%s: not an N64 ROM' % path)
    sha = hashlib.sha1(d).hexdigest()
    if sha != want_sha1 or (want_size and len(d) != want_size):
        sys.exit('%s: not the supported dump (SHA-1 %s, %d bytes)' % (path, sha, len(d)))
    return bytes(d)


def png_rgba(d):
    """An 8-bit RGBA, non-interlaced PNG -> (width, height, rows of RGBA bytes)."""
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit('icon is not a PNG')
    i, idat, hdr = 8, b'', None
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8])
        body = d[i + 8:i + 8 + n]
        if t == b'IHDR':
            hdr = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT':
            idat += body
        i += 12 + n
    w, h, bd, ct, _, _, il = hdr
    if (bd, ct, il) != (8, 6, 0):
        sys.exit('icon: expected an 8-bit RGBA, non-interlaced PNG')
    raw, bpp, stride = zlib.decompress(idat), 4, w * 4
    rows, prev, p = [], bytearray(stride), 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def png_write(w, h, rows):
    """Rows of RGBA bytes -> a PNG with only the image chunks (no metadata)."""
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    out = b''.join(b'\0' + r for r in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(out, 9)) + chunk(b'IEND', b''))


def scaled(w, rows, k):
    """Nearest neighbour, a whole factor k."""
    return [b''.join(r[x * 4:x * 4 + 4] * k for x in range(w)) for r in rows for _ in range(k)]


def icon_64(d):
    """The icon at 64x64 at least: a smaller 8-bit RGBA PNG (such as a 32x32 one) is scaled up by a
    whole factor, nearest neighbour; only the image chunks are kept (no metadata)."""
    w, h, rows = png_rgba(d)
    k = max(1, -(-64 // min(w, h)))
    return png_write(w * k, h * k, scaled(w, rows, k))


def boxart(d, bw=256, bh=360):
    """Onion's Ports box art (Roms/PORTS/Imgs/<name>.png: 256x360, as the Ports Collection's): the icon
    scaled up by the largest whole factor that fits, centred on a transparent canvas."""
    w, h, rows = png_rgba(d)
    k = max(1, min(bw // w, bh // h))
    img = scaled(w, rows, k)
    left, top = (bw - w * k) // 2, (bh - h * k) // 2
    blank = b'\0' * (bw * 4)
    pad_l, pad_r = b'\0' * (left * 4), b'\0' * ((bw - left - w * k) * 4)
    out = [blank] * top + [pad_l + r + pad_r for r in img]
    return png_write(bw, bh, out + [blank] * (bh - len(out)))


class Zip:
    def __init__(self, path):
        self.z = zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED)
        self.names = []

    def add(self, arc, data, exe=False, store=False):
        if isinstance(data, str):
            data = data.encode()
        m = None if arc.endswith('/rom.z64') else re.search(rb'xmpmeta|Photoshop|/Users/|/home/', data)
        if m:   # release gate 4: nothing published may carry authoring metadata or host paths
            sys.exit('%s: %r inside (authoring metadata or a host path)' % (arc, m.group(0)))
        zi = zipfile.ZipInfo(arc, date_time=(2020, 1, 1, 0, 0, 0))
        zi.external_attr = ((0o755 if exe else 0o644) | stat.S_IFREG) << 16
        zi.compress_type = zipfile.ZIP_STORED if store else zipfile.ZIP_DEFLATED
        self.z.writestr(zi, data)
        self.names.append(arc)

    def mkdir(self, arc):
        """An empty folder: created on the card by unzip (no file in it)."""
        zi = zipfile.ZipInfo(arc.rstrip('/') + '/', date_time=(2020, 1, 1, 0, 0, 0))
        zi.external_attr = (0o755 | stat.S_IFDIR) << 16 | 0x10
        self.z.writestr(zi, b'')
        self.names.append(zi.filename)

    def close(self):
        self.z.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game')
    ap.add_argument('--build', required=True)
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--rom')
    ap.add_argument('--layouts', default=','.join(DEFAULT_LAYOUTS))
    ap.add_argument('--check', action='store_true', help='list the zip and verify it reads back')
    a = ap.parse_args()
    g = json.load(open(a.game))
    gdir = os.path.dirname(os.path.abspath(a.game))
    layouts = [l for l in a.layouts.split(',') if l]
    for l in layouts:
        if l not in ALL_LAYOUTS:
            sys.exit('unknown layout %s (known: %s)' % (l, ', '.join(ALL_LAYOUTS)))
    title, folder, exe, env = g['title'], g['folder'], g['exe'], g['env']
    app = 'App/%s/' % folder
    data = g.get('data', 'data')
    if data not in ('.', 'data'):
        sys.exit('game.json data: "." or "data"')
    kv = dict(TITLE=title, FOLDER=folder, EXE=exe, ENV=env, ROMNAME=g['rom']['name'],
              DATADIR='$DIR' if data == '.' else '$DIR/data')
    icon_src = open(os.path.join(gdir, g['icon']), 'rb').read()
    icon = icon_64(icon_src)

    z = Zip(a.out)
    # the game, once: App/<folder>/ (the stock OS and Onion: Apps)
    for f in g['files']:
        z.add(app + f, open(os.path.join(a.build, f), 'rb').read(), exe=(f == exe))
    z.add(app + 'launch.sh', tpl('launch.sh', **kv), exe=True)
    z.add(app + 'config.json', tpl('app_config.json', **kv))
    z.add(app + 'icon.png', icon)
    # Onion on a Mini v4 / Mini Flip (752x560 panel, /tmp/new_res_available) switches the screen to 752x560
    # before an Apps launch only when App/<folder>/full_resolution exists, and puts 640x480 back after any
    # launch (Onion 07505ea runtime.sh:400-423, get_full_resolution_path :586-611). An empty marker; on a
    # 640x480 Mini, Onion never reads it.
    z.add(app + 'full_resolution', '')
    if a.rom:
        z.add(app + 'rom.z64', rom_z64(a.rom, g['rom']['sha1'], g['rom'].get('size')), store=True)

    if 'onion-games' in layouts:
        # Onion's Games list: a system of its own whose one "ROM" is a shortcut with the game's own
        # extension. That takes Onion's normal game path (runtime.sh launch_game): cmd_to_run.sh with
        # libpadsp.so, Emu/<system>/launch.sh run with the shortcut, play-time tracking, and the v4
        # 752x560 switch when Emu/<system>/full_resolution exists. NOT a .miyoocmd: Onion runs that
        # file directly, skipping all of it (the Games entry that never started, 2026-10-02; Onion
        # 07505ea runtime.sh:389-397; its Ports collection uses .miyoocmd only for helper scripts).
        ext = g['id']
        sysd, roms = 'Emu/%s/' % g['system'], 'Roms/%s/' % g['system']
        # A game list file, as every one of Onion's own systems names one (2026-10-10: with "gamelist": ""
        # MainUI listed the entry but never launched it; Onion's runtime.log showed no command at all).
        cfg = {"label": title, "icon": "icon.png", "iconsel": "icon.png", "launch": "launch.sh",
               "rompath": "../../" + roms.rstrip('/'), "imgpath": "../../" + roms + "Imgs",
               "gamelist": "../../" + roms + "miyoogamelist.xml",
               "useswap": 0, "shortname": 0, "hidebios": 0, "extlist": ext}
        z.add(sysd + 'config.json', json.dumps(cfg, indent=1) + '\n')
        z.add(sysd + 'launch.sh', tpl('forward.sh', WHO='Onion Games', EXTRA='', **kv), exe=True)
        z.add(sysd + 'icon.png', icon)
        z.add(sysd + 'full_resolution', '')   # the v4 / Flip 752x560 switch (Onion: next to launch.sh)
        z.add(roms + '%s.%s' % (title, ext), '%s: picking this starts App/%s (Emu/%s/launch.sh)\n' % (title, folder, g['system']))
        z.add(roms + 'miyoogamelist.xml', '<?xml version="1.0"?>\n<gameList>\n\t<game>\n\t\t<path>./%s.%s</path>\n'
              '\t\t<name>%s</name>\n\t\t<image>./Imgs/%s.png</image>\n\t</game>\n</gameList>\n' % (title, ext, title, title))
        z.add(roms + 'Imgs/%s.png' % title, icon)

    if 'onion-ports' in layouts:
        # Onion's Ports list (its Ports Collection package: Emu/PORTS/config.json lists
        # Roms/PORTS/Shortcuts, extlist "port|miyoocmd", box art from Roms/PORTS/Imgs/<name>.png;
        # Emu/PORTS/launch.sh runs the picked .port file itself, from its own folder). The .port only
        # hands over to App/<folder>/launch.sh, as every other launcher: nothing of the game is copied.
        # Onion's "~Import ports" (Emu/PORTS/import.sh) renames a shortcut to .notfound (hidden) unless
        # it finds GameDataFile within two levels of /mnt/SDCARD/Roms/PORTS/Games/<GameDir>: GameDir
        # climbs from there to App/<folder> (so Roms/PORTS/Games/ itself has to exist: an empty folder
        # in the zip), and GameDataFile is launch.sh, so the entry stays whether or not rom.z64 is there.
        # Nothing goes in Roms/PORTS/Games/ (other ports' folders), nor a Roms/PORTS game list (the
        # player's own). Onion 07505ea: static/packages/Emu/Ports Collection/Emu/PORTS/{config.json,
        # launch.sh,import.sh}.
        # The other OSes (checked in their sources, 2026-10-12): stock lists only Emu/*/config.json
        # systems and has no PORTS one; MinUI lists Roms/PORTS only with an Emus/<platform>/PORTS.pak;
        # spruce's Emu/PORTS (PyUI) takes only top-level .sh files (scanSubfolders false) and isn't
        # offered on the Mini family at all. Allium (ee0dda9) does list Roms/PORTS ("Ports Collection",
        # extension "port"), but its native core runs `cd "$1"; exec ./launch.sh`, which a .port file
        # can't satisfy, so there the entry starts nothing (the game is in Allium's Apps). Hiding it
        # would take a "_" or "." name (entry/mod.rs), which Onion shows as typed; Allium's gamelist has
        # no hidden flag. The user chose the clean Onion list (2026-10-12; a "_N64" folder before).
        # By default the shortcut sits straight in Roms/PORTS/Shortcuts, listed in Ports itself (Onion's
        # import.sh finds .port files at any depth; its own "~Import ports" sits there too).
        cat = g.get('ports_category', '')
        if cat and not re.match(r'^[A-Za-z0-9][A-Za-z0-9 ._-]*$', cat):
            sys.exit('game.json ports_category: letters, digits, space . _ -')
        port = tpl('forward.sh', WHO='Onion Ports', EXTRA=(
            '# for Onion\'s ~Import ports (Emu/PORTS/import.sh): the game is at /mnt/SDCARD/App/%s\n'
            'GameName="%s"\nGameDir="../../../App/%s"\nGameDataFile="launch.sh"\n'
            '# for Onion on a Mini v4 / Mini Flip: the screen at 752x560 for this game (runtime.sh looks for this line)\n'
            'FullResolution=1\n') % (folder, title, folder), **kv)
        z.add('Roms/PORTS/Shortcuts/%s%s.port' % (cat + '/' if cat else '', title), port, exe=True)
        z.add('Roms/PORTS/Imgs/%s.png' % title, boxart(icon_src))
        z.mkdir('Roms/PORTS/Games')

    if 'minui' in layouts:
        # MinUI / MyMinUI: a .pak inside a Roms folder is launched directly, and MinUI lists a Roms
        # folder only when an emulator pak with its tag exists (the GoldenEye port's layout: the game
        # on the home screen). MinUI's own SDL turns the panel itself, so the game doesn't.
        tag = g['minui_tag']
        z.add('Roms/%s (%s)/%s.pak/launch.sh' % (title, tag, title),
              tpl('forward.sh', WHO='MinUI', EXTRA='export %s_ROTATE=0 # MinUI\'s SDL turns the panel\n' % env, **kv), exe=True)
        z.add('Emus/miyoomini/%s.pak/launch.sh' % tag,
              '#!/bin/sh\n# %s: MinUI lists Roms/%s (%s) only when this emulator pak exists; the game is the .pak there.\n'
              'exec sh "$1/launch.sh"\n' % (title, title, tag), exe=True)

    if 'allium' in layouts:
        # Allium: Apps/<name>.pak/ with a stock/Onion-style config.json (label, icon, launch).
        pak = 'Apps/%s.pak/' % title
        z.add(pak + 'config.json', json.dumps({"label": title, "icon": "icon.png", "launch": "launch.sh",
                                               "description": "Native port"}, indent=1) + '\n')
        z.add(pak + 'icon.png', icon)
        z.add(pak + 'launch.sh', tpl('forward.sh', WHO='Allium', EXTRA='', **kv), exe=True)
    z.close()

    with zipfile.ZipFile(a.out) as r:
        bad = r.testzip()
        if bad:
            sys.exit('zip reads back wrong at %s' % bad)
        if a.check:
            for i in r.infolist():
                print('%10d  %s' % (i.file_size, i.filename))
    print('%s: %d files%s' % (a.out, len(z.names), ', WITH THE PLAYER\'S ROM (never share it)' if a.rom else ''))


if __name__ == '__main__':
    main()
