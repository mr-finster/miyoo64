#!/usr/bin/env python3
"""IDO gives every enum type int; GCC makes an enum with no negative enumerators
`unsigned int`, which flips `e < 0`, `e >= 0`, `(e << 7) >= 0`, e / n and (float)e for
any enum-typed value holding a negative number (HEAD_FIXED = 0xFFFFFFFF, CHRFLAG words).
GCC does not warn about these (no -Wtype-limits for enums), so fix the class at the source:

  * right after each enum body's `{` insert HH_ENUM_INT(<unique>) = `HH_ENUM_INT_<n> = -1,`
    which makes the type int without changing any value (the next enumerator without an
    initializer is still 0);
  * wrap enumerator literals above INT_MAX in HH_I32(), their int value on the N64.

Both macros come from hh/include/hh_enum.h (force-included by Makefile.handheld).
Usage: enum_int.py [--check] files...   (idempotent)"""
import re, sys

TOK = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|^[ \t]*#[^\n]*(?:\\\n[^\n]*)*|\benum\b|[{}]',
                 re.S | re.M)
BIG = re.compile(r'(?<![\w.])(0[xX][89a-fA-F][0-9a-fA-F]{7}|0[xX][0-9a-fA-F]{9,}|[0-9]{10,})([uUlL]*)(?![\w.])')

def mangle(path, line):
    stem = re.sub(r'\W', '_', path.split('/src/')[-1].split('/include/')[-1])
    return f'{stem}_{line}'

def process(path, text):
    out, pos, n_ins, n_big = [], 0, 0, 0
    toks = list(TOK.finditer(text))
    i = 0
    while i < len(toks):
        m = toks[i]
        if m.group(0) != 'enum':
            i += 1
            continue
        # enum [tag] { ... } : the next significant token must be '{' (skipping comments)
        j = i + 1
        between = text[m.end():toks[j].start()] if j < len(toks) else ''
        while j < len(toks) and (toks[j].group(0).startswith('//') or toks[j].group(0).startswith('/*')):
            between += toks[j].group(0) + text[toks[j].end():toks[j + 1].start() if j + 1 < len(toks) else len(text)]
            j += 1
        if j >= len(toks) or toks[j].group(0) != '{':
            i += 1
            continue
        gap = text[m.end():toks[j].start()]
        gap_nc = re.sub(r'//[^\n]*|/\*.*?\*/', ' ', gap, flags=re.S)
        if not re.fullmatch(r'\s*(\w+\s*)?', gap_nc):
            i += 1
            continue
        # find the matching '}'
        depth, k = 0, j
        while k < len(toks):
            g = toks[k].group(0)
            if g == '{':
                depth += 1
            elif g == '}':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        lbrace, rbrace = toks[j].end(), toks[k].start()
        body = text[lbrace:rbrace]
        line = text.count('\n', 0, m.start()) + 1
        out.append(text[pos:lbrace])
        if 'HH_ENUM_INT(' not in body:
            out.append(f' HH_ENUM_INT({mangle(path, line)})')
            n_ins += 1
        # literals above INT_MAX outside comments
        def fix(seg):
            nonlocal n_big
            def rep(mm):
                nonlocal n_big
                if int(mm.group(1), 0) <= 0x7FFFFFFF:
                    return mm.group(0)
                n_big += 1
                return f'HH_I32({mm.group(1)}{mm.group(2)})'
            return BIG.sub(rep, seg)
        parts = re.split(r'(//[^\n]*|/\*.*?\*/|HH_I32\([^)]*\))', body, flags=re.S)
        body = ''.join(p if (p.startswith('//') or p.startswith('/*') or p.startswith('HH_I32(')) else fix(p)
                       for p in parts)
        out.append(body)
        pos = rbrace
        i = k + 1
    out.append(text[pos:])
    return ''.join(out), n_ins, n_big

check = sys.argv[1] == '--check'
files = sys.argv[2:] if check else sys.argv[1:]
tot_i = tot_b = 0
for f in files:
    s = open(f, encoding='latin-1').read()
    if 'enum' not in s:
        continue
    t, ni, nb = process(f, s)
    if ni or nb:
        print(f'{f}: {ni} enums, {nb} wide literals')
        tot_i += ni
        tot_b += nb
        if not check:
            open(f, 'w', encoding='latin-1').write(t)
print(f'total: {tot_i} enums, {tot_b} wide literals')
