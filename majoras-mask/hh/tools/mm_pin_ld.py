#!/usr/bin/env python3
"""mm_pin_ld.py <base.map> <base-objs-dir> <default.ld> <out.ld> <report.txt> [--overflow 0x41600000]

Save-state compatibility across builds (ALPHA4 -> ALPHA5): a state is a raw image of the game's .data/.bss, so
every address the game keeps in it must mean the same thing in the new build: its functions (actor update
pointers), its constant tables and strings, its own data and heap. This writes a complete linker script that puts
every input section of the base build (the release whose states must load) at the base build's address, as long as
the section is still the same size; a section that changed size, and any new section, goes to overflow output
sections above the game's .bss, where they move nothing else.

Rules for a build that keeps compatibility (checked here and by mm_pin_check.py):
  - the game's objects are unchanged (decomp.patch the same);
  - port objects may change code; their string literals (merged .rodata.str sections) must not change;
  - no section of .game_data / .game_bss may change size, and nothing new may join them.

<base.map>       the base build's ld -Map output
<base-objs-dir>  the base build's port objects (paths as on the link line), to compare merged-string sections
"""
import os, re, struct, sys, hashlib

sys.path.insert(0, os.path.dirname(__file__))

PINNED = ['.text', '.rodata', '.data', '.game_data', '.bss', '.game_bss']
OVL_LINE = re.compile(r'^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)')
SEC1 = re.compile(r'^ (\.\S+|COMMON)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')
SEC = re.compile(r'^ (\.\S+|COMMON)\s*$')
CONT = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')


def read_map(path):
    outs = {}
    entries = []
    out = None
    pend = None
    started = False
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('Linker script and memory map'):
            started = True
            continue
        if not started:
            continue
        m = OVL_LINE.match(line)
        if m and not line.startswith(' '):
            out = m.group(1)
            outs[out] = (int(m.group(2), 16), int(m.group(3), 16))
            pend = None
            continue
        if line.startswith('.') :
            out = line.split()[0]
            pend = None
            continue
        if out not in PINNED:
            pend = None
            continue
        m = SEC1.match(line)
        if m:
            entries.append((out, m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()))
            pend = None
            continue
        m = SEC.match(line)
        if m:
            pend = m.group(1)
            continue
        if pend:
            m = CONT.match(line)
            if m:
                entries.append((out, pend, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
            pend = None
    return outs, entries


def elf_sections(path):
    """{name: (size, bytes or None, flags, type)} of a relocatable ELF32 LE object (duplicates: name -> list)."""
    b = open(path, 'rb').read()
    shoff = struct.unpack_from('<I', b, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x2E)
    hdrs = [struct.unpack_from('<IIIIIIIIII', b, shoff + i * shentsize) for i in range(shnum)]
    stroff = hdrs[shstrndx][4]
    res = {}
    for h in hdrs:
        name = b[stroff + h[0]:b.index(b'\0', stroff + h[0])].decode()
        typ, flags, off, size = h[1], h[2], h[4], h[5]
        data = b[off:off + size] if typ != 8 else None  # SHT_NOBITS
        res.setdefault(name, []).append((size, data, flags, typ))
    return res


def spec(obj, sec):
    """A linker-script input section spec for a map entry's file."""
    m = re.match(r'^(.*)\((.*)\)$', obj)
    if m and m.group(1).endswith('.a'):
        return '*%s:%s(%s)' % (os.path.basename(m.group(1)), m.group(2), sec)
    if sec == 'COMMON':
        return '%s(COMMON)' % obj
    return '%s(%s)' % (obj, sec)


def main():
    args = sys.argv[1:]
    overflow = 0x41600000
    if '--overflow' in args:
        i = args.index('--overflow')
        overflow = int(args[i + 1], 0)
        del args[i:i + 2]
    base_map, base_objs, default_ld, out_ld, report = args
    outs, entries = read_map(base_map)
    rep = []
    pinned = {o: [] for o in PINNED}
    moved = 0
    cache_new = {}
    renamed_used = {}
    cache_old = {}
    for out, sec, addr, size, obj in entries:
        if (size == 0 and out != '.rodata') or addr == 0 or obj.startswith('linker stubs') or obj.startswith('*fill*'):
            continue
        keep = True
        if obj.endswith('.o') and not obj.startswith('/'):
            old = os.path.join(base_objs, obj)
            if os.path.exists(old):  # a port object: compare with the base build's copy
                if obj not in cache_new:
                    cache_new[obj] = elf_sections(obj) if os.path.exists(obj) else {}
                    cache_old[obj] = elf_sections(old)
                n = cache_new[obj].get(sec if sec != 'COMMON' else '.bss', [])
                o = cache_old[obj].get(sec if sec != 'COMMON' else '.bss', [])
                if not n and len(o) == 1 and ('.str1.' in sec or sec.startswith('.rodata.str')):
                    # a renumbered clone (.constprop.N): the same strings under another name
                    for nm, lst in cache_new[obj].items():
                        if len(lst) == 1 and lst[0][1] == o[0][1] and ('.str1.' in nm) and nm not in renamed_used.setdefault(obj, set()):
                            renamed_used[obj].add(nm)
                            rep.append('renamed: %s %s -> %s' % (obj, sec, nm))
                            sec = nm
                            n = lst
                            break
                if len(n) != 1 or len(o) != 1:
                    keep = len(n) == len(o) == 1
                    why = 'missing/duplicate in the new object'
                elif sec.startswith('.rodata.str') or '.str1.' in sec:
                    # merged strings: the same size keeps the layout (the release name and version strings change
                    # in place); mm_compat_fix.py lists any .game_data pointer that still differs
                    keep = n[0][0] == o[0][0]
                    why = 'merged strings changed size %#x -> %#x' % (o[0][0], n[0][0])
                    if keep and n[0][1] != o[0][1]:
                        rep.append('merged strings changed in place: %s %s' % (obj, sec))
                else:
                    keep = n[0][0] == o[0][0]
                    why = 'size %#x -> %#x' % (o[0][0], n[0][0])
                if not keep:
                    # port .rodata that is not a merged string may move (no state points at it; the game sections
                    # after it in .rodata are pinned with MAX and checked by mm_pin_check.py)
                    # port merged strings too: the overflow is its own merge domain; no state points at port
                    # strings, and the .game_data pointers that might are in the compat table (mm_compat_fix.py)
                    if out in ('.game_data', '.game_bss'):
                        sys.exit('mm_pin_ld.py: %s %s in %s: %s -- states of the base build would not load'
                                 % (obj, sec, out, why))
                    rep.append('moved to overflow: %s %s (%s)' % (obj, sec, why))
                    moved += 1
        if keep:
            pinned[out].append((addr, size, spec(obj, sec), obj, sec))
    # the script: the default one, with the six output sections' contents replaced by the pinned lists
    text = open(default_ld).read()
    body = {}
    for out in PINNED:
        start, size = outs[out]
        lines = []
        is_str = lambda sec: '.str1.' in sec or sec.startswith('.rodata.str')
        last_str = max([i for i, e in enumerate(pinned[out]) if is_str(e[4])] or [-1])
        for idx, (addr, sz, sp, obj, sec) in enumerate(pinned[out]):
            # .rodata: the base build's order only (its merged strings are sized after the layout pass, so placed
            # by hand they would read as overlapping); nothing in it may move, so the order gives the same layout
            if out != '.rodata':
                lines.append('    . = 0x%x;' % (addr - start))
            elif idx > last_str:
                # after the last merged-string section: a placement there splits no merge group
                lines.append('    . = MAX(., 0x%x);' % (addr - start))  # not before; mm_pin_check.py says if after
            lines.append('    KEEP(%s)' % sp)
        lines.append(('    . = MAX(., 0x%x);' if out == '.rodata' else '    . = 0x%x;') % size)
        body[out] = (start, '\n'.join(lines))
    gd = ('  .game_data 0x%x : {\n    __gamedata_start = .;\n%s\n    __gamedata_end = .;\n  }\n'
          % (body['.game_data'][0], body['.game_data'][1]))
    gb = ('  .game_bss 0x%x (NOLOAD) : {\n    __gamebss_start = .;\n%s\n    __gamebss_end = .;\n  }\n'
          % (body['.game_bss'][0], body['.game_bss'][1]))

    def replace_section(t, name, newsec):
        # replace "  .name   : ...{ ... }" (possibly multi-line, balanced braces) with newsec
        m = re.search(r'\n  %s\s+[^\n]*:' % re.escape(name), t)
        if not m:
            sys.exit('no %s in the default script' % name)
        i = t.index('{', m.end())
        depth = 0
        j = i
        while True:
            if t[j] == '{': depth += 1
            elif t[j] == '}':
                depth -= 1
                if depth == 0: break
            j += 1
        return t[:m.start()] + '\n' + newsec + t[j + 1:]

    t = text
    t = replace_section(t, '.text', '  .text 0x%x : {\n%s\n  }' % body['.text'])
    t = replace_section(t, '.rodata', '  .rodata 0x%x : {\n%s\n  }' % body['.rodata'])
    t = replace_section(t, '.data', '  .data 0x%x : {\n%s\n  }\n%s' % (body['.data'][0], body['.data'][1], gd))
    t = replace_section(t, '.bss', ('  .bss 0x%x (NOLOAD) : {\n%s\n  }\n%s'
                                    '  .ovf_text 0x%x : { *(.text.unlikely .text.*_unlikely .text.unlikely.*) *(.text.exit .text.exit.*)'
                                    ' *(.text.startup .text.startup.*) *(.text.hot .text.hot.*) *(.text .stub .text.* .gnu.linkonce.t.*)'
                                    ' *(.glue_7t) *(.glue_7) *(.vfp11_veneer) *(.v4_bx) }\n'
                                    '  .ovf_rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }\n'
                                    '  . = ALIGN(0x1000);\n'
                                    '  .ovf_data : { *(.data .data.* .gnu.linkonce.d.*) }\n'
                                    '  .ovf_bss (NOLOAD) : { *(.dynbss) *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON) . = ALIGN(4); }\n')
                         % (body['.bss'][0], body['.bss'][1], gb, overflow))
    # the overlay-reload ranges (hh_ovl.c), at the base build's addresses
    syms = re.findall(r'^\s+0x([0-9a-f]+)\s+(__hhovl_\w+) = \.', open(base_map, errors='replace').read(), re.M)
    t = t.replace('SECTIONS\n{', 'SECTIONS\n{\n' + ''.join('  %s = 0x%s;\n' % (n, a.lstrip('0') or '0') for a, n in syms), 1)
    open(out_ld, 'w').write('/* generated by hh/tools/mm_pin_ld.py from %s */\n' % os.path.basename(base_map) + t)
    with open(report, 'w') as f:
        f.write('%d input sections pinned, %d moved to overflow\n' % (sum(len(v) for v in pinned.values()), moved))
        f.write('\n'.join(rep) + '\n')
    print('pinned %d, moved %d' % (sum(len(v) for v in pinned.values()), moved))


if __name__ == '__main__':
    main()
