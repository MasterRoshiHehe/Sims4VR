#!/usr/bin/env python3
"""List raw game addresses in src/ that bypass the addrs table (kit: vr-mod-game-patches).

A hex literal of 6-8 digits in code (not in a comment or string) that falls inside the exe image
is probably an RVA. Prints file:line and the line. Exit 1 if any are found outside the allowlist
(tools/check_addrs_allow.txt: lines of 'file:line-substring # reason').
Usage: python3 tools/check_addrs.py [src_dir]
"""
import re, sys, pathlib

IMAGE = 0x2bb0000          # exe image size, ~45 MB
LIT = re.compile(r'\b0x([0-9a-fA-F]{5,8})\b')
SKIP_FILES = {'addrs_table.inc'}

def code_part(line, in_block):
    out, i, n = [], 0, len(line)
    while i < n:
        if in_block:
            j = line.find('*/', i)
            if j < 0: return ''.join(out), True
            i, in_block = j + 2, False; continue
        if line.startswith('//', i): break
        if line.startswith('/*', i): in_block = True; i += 2; continue
        if line[i] == '"':
            j = i + 1
            while j < n and line[j] != '"':
                j += 2 if line[j] == '\\' else 1
            i = j + 1; continue
        out.append(line[i]); i += 1
    return ''.join(out), in_block

def main():
    src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'src')
    allow = []
    ap = pathlib.Path(__file__).with_name('check_addrs_allow.txt')
    if ap.exists():
        for l in ap.read_text().splitlines():
            l = l.split('#')[0].strip()
            if l: allow.append(l)
    found = 0
    for f in sorted(list(src.rglob('*.cpp')) + list(src.rglob('*.h')) + list(src.rglob('*.inc'))):
        if f.name in SKIP_FILES: continue
        blk = False
        for no, line in enumerate(f.read_text(errors='replace').splitlines(), 1):
            code, blk = code_part(line, blk)
            hits = [m for m in LIT.finditer(code) if 0x10000 <= int(m.group(1), 16) < IMAGE]
            if not hits: continue
            # only literals that look like addresses: used with + (offset from module base) or a *_RVA name
            if not re.search(r'(\+\s*0x|rva|RVA|Rva|ModuleHandle)', code): continue
            tag = f'{f.relative_to(src)}:{no}'
            if any(a.split(':', 1)[0] == str(f.relative_to(src)) and a.split(':', 1)[1] in line for a in allow if ':' in a): continue
            found += 1
            print(f'{tag}: {line.strip()[:150]}')
    print(f'{found} raw address use(s) outside the table')
    sys.exit(1 if found else 0)
main()
