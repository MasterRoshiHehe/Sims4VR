"""Dump a template object's resources (OBJD, COBJ, MODL/MLOD RCOL chunks, VRTF, MATD params, FTPT) for the CC builder.
Usage: inspect_tmpl.py <dir of TYPE_GROUP_INSTANCE.bin files>"""
import sys, os, struct, glob
d = sys.argv[1]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def f32(b, o): return struct.unpack_from('<f', b, o)[0]
def tag(b, o): return b[o:o+4].decode('latin1')
def rcol(b):
    ver, pub, unused, ext, internal = struct.unpack_from('<5I', b, 0)
    at = 20; ikeys = []
    for i in range(internal):
        ikeys.append(struct.unpack_from('<QII', b, at)); at += 16
    ekeys = []
    for i in range(ext):
        ekeys.append(struct.unpack_from('<QII', b, at)); at += 16
    chunks = []
    for i in range(internal):
        pos, size = struct.unpack_from('<II', b, at); at += 8
        chunks.append((tag(b, pos), pos, size))
    return ver, pub, ikeys, ekeys, chunks
for fn in sorted(glob.glob(os.path.join(d, '*.bin'))):
    b = open(fn, 'rb').read(); name = os.path.basename(fn); t = name[:8]
    print('=====', name, len(b))
    if t in ('319E4F1D', 'D382BF57', '03B4C61D'):
        print(b.hex()); continue
    if t == 'C0DB5AE7':
        ver, tp = struct.unpack_from('<HI', b, 0); cnt = struct.unpack_from('<H', b, tp)[0]
        ents = [struct.unpack_from('<II', b, tp + 2 + 8 * k) for k in range(cnt)]
        print('ver', ver, 'table', tp, 'count', cnt)
        offs = sorted(o for _, o in ents) + [tp]
        for tg, o in ents:
            nxt = min(x for x in offs if x > o)
            print(f'  {tg:08X} @{o} len{nxt-o}: {b[o:nxt].hex()}')
        continue
    if t in ('01661233', '01D10F34'):
        ver, pub, ik, ek, ch = rcol(b)
        print('rcol ver', ver, 'public', pub)
        for k in ik: print('  ikey %016X %08X %08X' % k)
        for k in ek: print('  ekey %016X %08X %08X' % k)
        for c in ch:
            tg, pos, size = c
            print('  chunk', tg, pos, size)
            if tg == 'MODL':
                v = u32(b, pos+4); n = u32(b, pos+8)
                print('    ver', hex(v), 'lods', n, 'box', [round(f32(b, pos+12+4*k), 3) for k in range(6)])
                print('    raw', b[pos:pos+size].hex())
            elif tg == 'MLOD':
                v = u32(b, pos+4); n = u32(b, pos+8); at = pos+12
                print('    ver', hex(v), 'meshes', n)
                for m in range(n):
                    ln = u32(b, at); e = at+4
                    vals = struct.unpack_from('<11I', b, e)
                    print('    mesh', m, 'len', ln, 'name/mat/vrtf/vbuf/ibuf/flags/stream/sv/si/minv/vc/prims', [hex(x) for x in vals[:6]], vals[6:11], struct.unpack_from('<I', b, e+44)[0])
                    print('      box', [round(f32(b, e+48+4*k), 3) for k in range(6)])
                    print('      rest', b[e+72:e+ln].hex())
                    at = e+ln
            elif tg == 'VRTF':
                v, stride, n, extd = struct.unpack_from('<4I', b, pos+4)
                print('    ver', hex(v), 'stride', stride, 'elems', n, 'ext', extd)
                ea = pos+20
                for k in range(n):
                    if extd: print('     ', struct.unpack_from('<4I', b, ea)); ea += 16
                    else: print('     ', tuple(b[ea:ea+4])); ea += 4
            elif tg == 'MATD':
                print('    head', b[pos:pos+40].hex())
                v = u32(b, pos+4); shader = u32(b, pos+12)
                block = pos+20+(8 if v >= 0x103 else 0)
                cnt = u32(b, block+12)
                print('    ver', hex(v), 'shader', hex(shader), 'params', cnt, 'blockhdr', b[block:block+16].hex())
                for i in range(cnt):
                    p = block+16+i*16
                    h, ty, n2, off = struct.unpack_from('<4I', b, p)
                    data = b[block+off: block+off+4*n2]
                    print('     %08X type %08X n %d: %s' % (h, ty, n2, data.hex()))
            elif tg in ('VBUF', 'IBUF'):
                print('    hdr', b[pos:pos+16].hex(), 'datalen', size-16)
            else:
                print('    raw', b[pos:pos+min(size, 160)].hex())
