#!/usr/bin/env python3
"""Per-eye call counts from sims4vr.log (kit: test-harness "Stereo side effects", count calls per eye).

Reads the "Pass time (last 10 s ...)" and "Shadow lists (last 10 s)" lines, sums left/right runs per
stage over the whole log and prints right/left. A per-view stage should be about 1.0 (both eyes run
it); well under 0.75 means the right eye skipped it, well over 1.25 means extra work. Windows where
only one eye ran at all (mono, loads) are counted apart.
Usage: python3 tools/eye_counts.py sims4vr.log [more.log ...]
"""
import re, sys, collections

PASS = re.compile(r'Pass time \(last 10 s.*?\): (.*)$')
STAGE = re.compile(r'([\w ]+?): left (\d+) x [\d.]+ ms, right (\d+) x')
SHL = re.compile(r'Shadow lists \(last 10 s\): left eye (\d+) runs [\d.]+ ms each, right eye (\d+) runs')

def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    for path in sys.argv[1:]:
        tot = collections.defaultdict(lambda: [0, 0]); mono = collections.Counter(); win = collections.Counter()
        for line in open(path, errors='replace'):
            m = PASS.search(line)
            if m:
                for s in STAGE.finditer(m.group(1)):
                    name, l, r = s.group(1).strip(), int(s.group(2)), int(s.group(3))
                    if l and r: tot[name][0] += l; tot[name][1] += r; win[name] += 1
                    elif l or r: mono[name] += 1
            m = SHL.search(line)
            if m:
                l, r = int(m.group(1)), int(m.group(2))
                if l and r: tot['shadow lists'][0] += l; tot['shadow lists'][1] += r; win['shadow lists'] += 1
                elif l or r: mono['shadow lists'] += 1
        print(f'{path}')
        if not tot: print('  no two-eye windows found'); continue
        for k, (l, r) in tot.items():
            q = r / l if l else 0
            v = 'PASS' if 0.75 <= q <= 1.25 else 'WARN'
            print(f'  {v}  {k:14s} left {l:7d} right {r:7d} right/left {q:.2f} ({win[k]} two-eye windows, {mono[k]} one-eye windows)')
        for k in mono:
            if k not in tot: print(f'  WARN  {k:14s} only one eye ever ran ({mono[k]} windows)')
main()
