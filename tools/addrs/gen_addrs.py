"""Central game-address table: addrs_src.tsv -> src/addrs_table.inc

addrs_src.tsv is the one hand-kept list of every game function/global/vtable the mod
uses (name, kind, rva, used_in, first16, sig, description). The generated
src/addrs_table.inc is what src/addrs.cpp compiles in: per row the rva plus a
masked byte signature (relative call/jump targets and rip-relative displacements are
wildcards, written ??) and, for globals, an anchor (a table function that reads the
global through a rip-relative operand), so a patched exe can be re-resolved by scan.

usage:
  (in the repo: --src tools/addrs/addrs_src.tsv --out src/addrs_table.inc)
  python3 -I gen_addrs.py [--exe TS4_x64.exe] [--remap] [--write-src] [--src addrs_src.tsv] [--out ../../../src/addrs_table.inc]
    (no --exe)   signatures come from the first16 column (complete instructions only)
    --exe        signatures are rebuilt from the exe, extended until unique in .text; anchors are found;
                 the stamp is read from the exe
    --remap      first move every row to where the exe has it (functions: unique signature scan;
                 globals: through their anchor). Misses are printed: fix those rows by hand in the tsv
                 (rtti classes.tsv by name maps vtables old -> new), then run again without --remap
    --write-src  write the new rva/first16/sig back into addrs_src.tsv
  python3 -I gen_addrs.py --check [--src ...] [--out ...] [--srcdir ../../../src]
    no exe needed: table in sync with the tsv, every name used in src/*.cpp exists, no raw
    gameaddr::function/global call or Known{rva} constant left in src/
  python3 -I gen_addrs.py --selftest   builds a small fake exe and runs remap on it

Needs capstone (pip install capstone).
"""
import argparse, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_DEFAULT = os.path.join(HERE, 'addrs_src.tsv')
OUT_DEFAULT = os.path.abspath(os.path.join(HERE, '..', '..', '..', 'src', 'addrs_table.inc'))
SRCDIR_DEFAULT = os.path.dirname(OUT_DEFAULT)
MIN_LEN, MAX_LEN = 24, 96
COLS = ['name', 'kind', 'rva', 'used_in', 'first16', 'sig', 'description']


# ---- tsv -------------------------------------------------------------------

def read_src(path):
    meta, rows = {}, []
    for line in open(path):
        line = line.rstrip('\n')
        if line.startswith('#'):
            for k, v in re.findall(r'(\w+)=(\S+)', line):
                meta[k] = v
            continue
        if not line or line.startswith('name\t'):
            continue
        c = (line.split('\t') + [''] * len(COLS))[:len(COLS)]
        r = dict(zip(COLS, c))
        r['rva'] = int(r['rva'], 16)
        rows.append(r)
    return meta, rows


def write_src(path, meta, rows):
    with open(path, 'w') as f:
        f.write('# game=%s stamp=%s\n' % (meta.get('game', '?'), meta.get('stamp', '?')))
        f.write('\t'.join(COLS) + '\n')
        for r in rows:
            f.write('\t'.join([r['name'], r['kind'], '0x%x' % r['rva'], r['used_in'], r['first16'], r['sig'],
                               r['description']]) + '\n')


# ---- pe --------------------------------------------------------------------

class PE:
    def __init__(self, data):
        self.d = data
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        self.stamp = struct.unpack_from('<I', data, pe + 8)[0]
        nsec = struct.unpack_from('<H', data, pe + 6)[0]
        optsz = struct.unpack_from('<H', data, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = pe + 24 + optsz + i * 40
            n = data[o:o + 8].rstrip(b'\0').decode()
            vsz, va, rsz, raw = struct.unpack_from('<IIII', data, o + 8)
            self.secs.append((n, vsz, va, rsz, raw))
        t = [s for s in self.secs if s[0] == '.text'][0]
        self.text_rva, self.text_raw, self.text_size = t[2], t[4], t[3]
        self.text = data[self.text_raw:self.text_raw + self.text_size]

    def off(self, rva):
        for n, vsz, va, rsz, raw in self.secs:
            if va <= rva < va + max(rsz, vsz):
                return raw + rva - va
        return None

    def read(self, rva, n):
        o = self.off(rva)
        return self.d[o:o + n] if o is not None else b''


# ---- signatures --------------------------------------------------------------

def decoder():
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    return md


def masked_insns(md, code, rva):
    """[(bytes, mask-flags per byte, rip target or None, disp_off, size)] for each complete instruction."""
    from capstone import CS_GRP_CALL, CS_GRP_JUMP
    from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP
    out = []
    for i in md.disasm(code, rva):
        m = [False] * i.size
        tgt = None
        disp_at = 0
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                # Some capstone builds report disp_size 0 for SSE forms with 66/REX prefixes (SkinVerts,
                # 66 44 0f 6f 0d disp32): then find the 4 displacement bytes by value.
                off = i.disp_offset if i.disp_size == 4 else bytes(i.bytes).rfind(struct.pack('<i', op.mem.disp), 2)
                if off < 0 or off + 4 > i.size:
                    continue
                for k in range(4):
                    m[off + k] = True
                tgt = i.address + i.size + op.mem.disp
                disp_at = off
        if (i.group(CS_GRP_CALL) or i.group(CS_GRP_JUMP)) and i.imm_size == 4:
            for k in range(4):
                m[i.imm_offset + k] = True
        out.append((bytes(i.bytes), m, tgt, disp_at if tgt is not None else 0, i.size, i.address - rva))
    return out


def sig_text(insns, upto):
    parts = []
    for b, m, *_ in insns[:upto]:
        parts += ['??' if mm else '%02x' % x for x, mm in zip(b, m)]
    return ' '.join(parts)


def sig_regex(sig):
    return re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in sig.split()), re.S)


def find_all(text, sig, limit=3):
    rx, hits, pos = sig_regex(sig), [], 0
    while len(hits) < limit:
        m = rx.search(text, pos)
        if not m:
            break
        hits.append(m.start())
        pos = m.start() + 1
    return hits


def build_sig(md, pe, rva):
    """Masked signature at rva extended instruction by instruction until unique in .text."""
    code = pe.read(rva, MAX_LEN + 16)
    insns = masked_insns(md, code, rva)
    n = 0
    total = 0
    for k, ins in enumerate(insns):
        total += ins[4]
        n = k + 1
        if total >= MIN_LEN:
            sig = sig_text(insns, n)
            if len(find_all(pe.text, sig, 2)) == 1:
                return sig, True
        if total >= MAX_LEN:
            break
    return sig_text(insns, n), False


def sig_from_bytes(md, hexbytes, rva):
    """Offline: complete instructions out of the first16 column only."""
    code = bytes(int(x, 16) for x in hexbytes.split())
    insns = masked_insns(md, code, rva)
    if not insns:
        return ' '.join(hexbytes.split())
    return sig_text(insns, len(insns))


def find_anchor(md, pe, rows_by_name, grow):
    """A table function whose first 0x300 bytes read global rva `grow` via rip-relative: (name, dispOff, endOff)."""
    for r in rows_by_name.values():
        if r['kind'] != 'function' or not r.get('unique'):
            continue
        code = pe.read(r['rva'], 0x300)
        for b, m, tgt, doff, size, at in masked_insns(md, code, r['rva']):
            if tgt == grow:
                return r['name'], at + doff, at + size
    return None


# ---- main ------------------------------------------------------------------

def parse_sig_anchor(r):
    a = r.get('anchor', '')
    if not a:
        return None
    n, d, e = a.split(':')
    return n, int(d), int(e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe')
    ap.add_argument('--remap', action='store_true')
    ap.add_argument('--write-src', action='store_true')
    ap.add_argument('--src', default=SRC_DEFAULT)
    ap.add_argument('--out', default=OUT_DEFAULT)
    ap.add_argument('--srcdir', default=SRCDIR_DEFAULT)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.check:
        return check(a)
    meta, rows = read_src(a.src)
    md = decoder()
    by = {r['name']: r for r in rows}
    for r in rows:  # anchors live in the tsv's sig column as "sig | anchor=NAME:dispoff:endoff"
        r['anchor'] = ''
        if '|' in r['sig']:
            r['sig'], an = [x.strip() for x in r['sig'].split('|')]
            r['anchor'] = an.replace('anchor=', '')
    pe = PE(open(a.exe, 'rb').read()) if a.exe else None
    problems = []
    if pe:
        meta['stamp'] = '0x%08x' % pe.stamp
        if a.remap:
            for r in rows:  # functions first
                if r['kind'] != 'function' or not r['sig']:
                    continue
                hits = find_all(pe.text, r['sig'])
                if len(hits) == 1:
                    new = pe.text_rva + hits[0]
                    if new != r['rva']:
                        print('moved  %-18s 0x%x -> 0x%x' % (r['name'], r['rva'], new))
                    r['rva'] = new
                else:
                    problems.append('%s: signature %s in the new exe (hand-fix rva in addrs_src.tsv)' %
                                    (r['name'], 'ambiguous' if hits else 'not found'))
            for r in rows:
                if r['kind'] != 'global':
                    continue
                an = parse_sig_anchor(r)
                if an and an[0] in by:
                    f = by[an[0]]
                    disp = struct.unpack('<i', pe.read(f['rva'] + an[1], 4))[0]
                    new = f['rva'] + an[2] + disp
                    if new != r['rva']:
                        print('moved  %-18s 0x%x -> 0x%x (via %s)' % (r['name'], r['rva'], new, an[0]))
                    r['rva'] = new
                else:
                    problems.append('%s: global without anchor (find it from classes.tsv/known.tsv by name, hand-fix)' %
                                    r['name'])
        for r in rows:
            if r['kind'] != 'function':
                continue
            r['first16'] = ' '.join('%02x' % x for x in pe.read(r['rva'], 16))
            r['sig'], r['unique'] = build_sig(md, pe, r['rva'])
            if not r['unique']:
                problems.append('%s: no unique signature up to %d bytes (matched by rva only after a patch: hand-fix)' %
                                (r['name'], MAX_LEN))
        for r in rows:
            if r['kind'] == 'global':
                r['anchor'] = ''
                f = find_anchor(md, pe, by, r['rva'])
                if f:
                    r['anchor'] = '%s:%d:%d' % f
                else:
                    problems.append('%s: no anchor among the table functions (stamp-gated rva only)' % r['name'])
    else:
        for r in rows:
            if r['kind'] == 'function':
                r['sig'] = r['sig'] or sig_from_bytes(md, r['first16'], r['rva'])
                r['unique'] = False
    if a.write_src:
        for r in rows:
            if r['anchor']:
                r['sig'] = (r['sig'] + ' ' if r['sig'] else '') + '| anchor=' + r['anchor']
        write_src(a.src, meta, rows)
        for r in rows:  # keep the in-memory sig clean for the table
            if '|' in r['sig']:
                r['sig'] = r['sig'].split('|')[0].strip()
    stamp = int(meta['stamp'], 16)
    with open(a.out, 'w') as f:
        f.write('// Generated by re/exe/tools/gen_addrs.py from re/exe/tools/addrs_src.tsv. Do not edit.\n')
        f.write('// Game %s, exe stamp %s.\n' % (meta.get('game', '?'), meta['stamp']))
        f.write('constexpr uint32_t kTableStamp = 0x%08x;\n' % stamp)
        f.write('constexpr const char* kGameVersion = "%s";\n' % meta.get('game', '?'))
        f.write('// name, f(unction)/g(lobal or vtable), rva, signature, anchor function, disp32 offset, insn end offset\n')
        f.write('constexpr Row kRows[] = {\n')
        for r in rows:
            an = parse_sig_anchor(r)
            f.write('    {"%s", \'%s\', 0x%x, "%s", "%s", %d, %d},\n' %
                    (r['name'], 'f' if r['kind'] == 'function' else 'g', r['rva'], r['sig'],
                     an[0] if an else '', an[1] if an else 0, an[2] if an else 0))
        f.write('};\n')
    nf = sum(1 for r in rows if r['kind'] == 'function')
    print('wrote %s: %d functions, %d globals/vtables%s' % (a.out, nf, len(rows) - nf,
                                                          '' if pe else ' (offline: signatures from first16, not checked for uniqueness)'))
    for p in problems:
        print('PROBLEM', p)
    return 1 if problems and a.remap else 0


# ---- check (no exe) ---------------------------------------------------------

def check(a):
    meta, rows = read_src(a.src)
    names = {r['name'] for r in rows}
    bad = 0
    table = open(a.out).read()
    got = {m.group(1): (m.group(2), int(m.group(3), 16)) for m in
           re.finditer(r'\{"(\w+)", \'(\w)\', (0x[0-9a-f]+),', table) if False}
    for m in re.finditer(r'\{"(\w+)", \'(\w)\', (0x[0-9a-f]+), "([^"]*)"', table):
        got[m.group(1)] = (m.group(2), int(m.group(3), 16), m.group(4))
    for r in rows:
        g = got.get(r['name'])
        if not g or g[1] != r['rva'] or g[0] != ('f' if r['kind'] == 'function' else 'g'):
            print('table out of sync with tsv:', r['name']); bad += 1
        elif r['kind'] == 'function':
            lead = [x for x in g[2].split() if x != '??']
            if r['first16'].split()[:len(g[2].split())] and any(
                    t != '??' and t != b for t, b in zip(g[2].split(), r['first16'].split())):
                print('signature does not start with first16:', r['name']); bad += 1
    for n in set(got) - names:
        print('table row not in tsv:', n); bad += 1
    used = set()
    for fn in sorted(os.listdir(a.srcdir)):
        if not fn.endswith(('.cpp', '.h')) or fn.startswith('addrs'):
            continue
        t = open(os.path.join(a.srcdir, fn), errors='replace').read()
        for m in re.finditer(r'addrs::(?:fn|global)\("(\w+)"\)', t):
            used.add(m.group(1))
            if m.group(1) not in names:
                print('%s: unknown address name %s' % (fn, m.group(1))); bad += 1
        used |= {n for n in names if '"%s"' % n in t}
        if fn != 'gameaddr.cpp' and re.search(r'gameaddr::(function|global)\s*\(', t):
            print('%s: raw gameaddr::function/global call left' % fn); bad += 1
        if re.search(r'(const|constexpr) Known k\w+\s*\{\s*0x', t):
            print('%s: raw Known{rva} constant left' % fn); bad += 1
    for n in sorted(names - used):
        print('note: %s is in the table but not used by src' % n)
    print('check: %d rows, %d names used, %d problems' % (len(rows), len(used), bad))
    return 1 if bad else 0


# ---- selftest ---------------------------------------------------------------

def selftest():
    import tempfile
    # fake exe: .text at rva 0x1000 holding three functions and a global referenced by f2
    def pe_bytes(text, text_rva, gl_rva):
        hdr = bytearray(0x400)
        hdr[0:2] = b'MZ'
        struct.pack_into('<I', hdr, 0x3c, 0x80)
        hdr[0x80:0x84] = b'PE\0\0'
        struct.pack_into('<H', hdr, 0x80 + 6, 1)
        struct.pack_into('<I', hdr, 0x80 + 8, 0x12345678)
        struct.pack_into('<H', hdr, 0x80 + 20, 0xf0)
        o = 0x80 + 24 + 0xf0
        hdr[o:o + 8] = b'.text\0\0\0'
        struct.pack_into('<IIII', hdr, o + 8, len(text), text_rva, len(text), 0x400)
        return bytes(hdr) + bytes(text)

    def build(pad):
        t = bytearray()
        base = 0x1000
        t += b'\xcc' * pad
        f1 = base + len(t)
        t += bytes.fromhex('4883ec28 488b05') + b'\0\0\0\0' + bytes.fromhex('e8') + b'\0\0\0\0' + bytes.fromhex('4883c428c3') + b'\xcc' * 3
        f2 = base + len(t)
        t += bytes.fromhex('40534883ec20 8b0d') + b'\0\0\0\0' + bytes.fromhex('85c9 7405 e8') + b'\0\0\0\0' + bytes.fromhex('4883c4205bc3') + b'\xcc' * 2
        glob = base + len(t) + 0x40
        t += b'\xcc' * 0x80
        # fix rip-rel displacements to reach glob / f1
        def put(at, tgt, size):
            struct.pack_into('<i', t, at - base, tgt - (at + 4))
        put(f1 + 7, glob, 4); put(f1 + 12, f2, 4)
        put(f2 + 8, glob, 4); put(f2 + 17, f1, 4)
        return pe_bytes(t, base, glob), f1, f2, glob

    old, f1, f2, glob = build(0)
    new, nf1, nf2, nglob = build(0x230)
    d = tempfile.mkdtemp()
    open(d + '/old.exe', 'wb').write(old); open(d + '/new.exe', 'wb').write(new)
    src = d + '/src.tsv'
    write_src(src, {'game': 'test', 'stamp': '0x12345678'},
              [dict(name='F1', kind='function', rva=f1, used_in='t', first16='', sig='', description=''),
               dict(name='F2', kind='function', rva=f2, used_in='t', first16='', sig='', description=''),
               dict(name='G', kind='global', rva=glob, used_in='t', first16='', sig='', description='')])
    out = d + '/t.inc'
    rc = os.system('%s %s --exe %s/old.exe --write-src --src %s --out %s' % (sys.executable, __file__, d, src, out))
    print(open(src).read())
    rc |= os.system('%s %s --exe %s/new.exe --remap --write-src --src %s --out %s' % (sys.executable, __file__, d, src, out))
    meta, rows = read_src(src)
    got = {r['name']: r['rva'] for r in rows}
    want = {'F1': nf1, 'F2': nf2, 'G': nglob}
    print('selftest', 'OK' if got == want and rc == 0 else 'FAIL %s vs %s rc=%s' % (got, want, rc))
    return 0 if got == want and rc == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
