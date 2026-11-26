#!/usr/bin/env python3
"""Tests for tools/run_report.py and tools/check_addrs.py: python3 tools/test_tools.py"""
import subprocess, sys, tempfile, pathlib, textwrap
here = pathlib.Path(__file__).parent
def run(*a): return subprocess.run([sys.executable, *map(str, a)], capture_output=True, text=True)
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    bad = d / 'bad.log'
    bad.write_text("[10:00:00.000] [session] sims4vr v0.0.1 (commit x) loading from C:\\m\n"
                   "[10:00:00.100] [misc] Game build not recognised (exe stamp 1, table is 2): off\n"
                   "[10:00:10.000] [misc] Frame time at rest (last 10 s, 700 pictures, 70.0 a second, 1.00 shadow fits and 0 body placements a picture): x\n")
    r = run(here / 'run_report.py', bad, '--known', d / 'none.txt', '--strict')
    assert r.returncode == 1 and 'FAIL  game build' in r.stdout, r.stdout
    good = d / 'good.log'
    good.write_text("[10:00:00.000] [session] sims4vr v0.0.1 (commit x) loading from C:\\m\n"
                    "[10:00:00.100] [misc] Game build 1.129 recognised\n"
                    "[10:00:10.000] [misc] Frame time at rest (last 10 s, 700 pictures, 70.0 a second, 1.00 shadow fits and 0 body placements a picture): x\n")
    r = run(here / 'run_report.py', good, '--known', d / 'none.txt', '--strict')
    assert r.returncode == 0 and 'FAIL' not in r.stdout, r.stdout
    s = d / 'src'; s.mkdir()
    (s / 'a.cpp').write_text("void f(){ auto p = base + 0x280cb7c; }\n// base + 0x280cb7c in a comment\nint x = 0x811c9dc5;\n")
    r = run(here / 'check_addrs.py', s)
    assert r.returncode == 1 and r.stdout.count('a.cpp:') == 1, r.stdout
    (s / 'a.cpp').write_text("int x = 0x811c9dc5;\n")
    assert run(here / 'check_addrs.py', s).returncode == 0
print('ok')
