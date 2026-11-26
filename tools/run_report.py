#!/usr/bin/env python3
"""One-page verdict-first report of a sims4vr.log (kit: test-harness "run report").

Usage: python3 tools/run_report.py sims4vr.log [--known tools/known_states.txt] [--learn] [--strict]
Prints PASS/WARN/FAIL lines first, then states. A state is a log line with numbers/hex/paths
stripped. States not in the known list print as NEW. --strict exits 1 on any FAIL or NEW;
--learn appends this run's NEW states to the known list (only after a person has looked).
"""
import re, sys, collections, pathlib

LINE = re.compile(r'^\[(\d\d:\d\d:\d\d\.\d+)\] \[([^\]]+)\] (.*)$')
BAD = re.compile(r'\b(refus\w*|stopped|failed|fail|error|crash\w*|not found|not recognised|DIFFERENT build|exception|timed out)\b', re.I)
FT = re.compile(r'Frame time (at rest|while the stick turns).*?(\d+) pictures, ([\d.]+) a second, ([\d.]+) shadow fits')

def norm(msg):
    m = re.sub(r'[A-Za-z]:\\[^\s,;)]*', '<path>', msg)
    m = re.sub(r'0x[0-9a-fA-F]+|\b(?=[0-9a-f]*\d)(?=[0-9a-f]*[a-f])[0-9a-f]{6,}\b|\b[0-9a-f]{8}\b', '<hex>', m)
    m = re.sub(r'-?\d+(\.\d+)?', '#', m)
    return m[:140]

def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    path = a[0]
    known_p = pathlib.Path(a[a.index('--known') + 1]) if '--known' in a else pathlib.Path(__file__).with_name('known_states.txt')
    known = set(known_p.read_text().splitlines()) if known_p.exists() else set()
    states = collections.Counter(); first = {}
    ver = build = None; fts = {'at rest': [], 'while the stick turns': []}; lines = 0
    for raw in open(path, errors='replace'):
        m = LINE.match(raw.rstrip('\n'))
        if not m: continue
        lines += 1; t, tag, msg = m.groups()
        if tag == 'session' and 'loading from' in msg: ver = msg.split(' loading')[0]
        if 'Game build' in msg: build = msg[:120]
        f = FT.search(msg)
        if f: fts[f.group(1)].append((float(f.group(3)), float(f.group(4))))
        if BAD.search(msg):
            k = f'[{tag}] {norm(msg)}'; states[k] += 1; first.setdefault(k, t)
    new = [k for k in states if k not in known]
    res = []
    res.append(('FAIL' if build and 'not recognised' in build else 'PASS', 'game build: ' + (build or 'no build line')))
    if fts['at rest']:
        fps = sorted(x[0] for x in fts['at rest']); res.append(('PASS', f"frame rate at rest: median {fps[len(fps)//2]:.0f}/s, worst window {fps[0]:.0f}/s, {len(fps)} windows"))
        sh = sorted(x[1] for x in fts['at rest']); res.append(('PASS', f"shadow fits a picture at rest: median {sh[len(sh)//2]:.2f}, max {sh[-1]:.2f}"))
    else: res.append(('WARN', 'no "Frame time at rest" lines'))
    res.append(('WARN' if new else 'PASS', f'{len(new)} NEW state(s) not in {known_p.name}'))
    print(f'{ver or path}: {lines} lines')
    for v, t in res: print(f'{v}  {t}')
    print('\nstates (count, first seen):')
    for k, n in states.most_common(40): print(f"  {'NEW ' if k in new else '    '}{n:5d}  {first[k]}  {k}")
    if '--learn' in a:
        known_p.write_text('\n'.join(sorted(known | set(states))) + '\n'); print(f'\nlearned {len(new)} state(s) into {known_p}')
    bad = any(v == 'FAIL' for v, _ in res) or bool(new)
    sys.exit(1 if bad and '--strict' in a else 0)
main()
