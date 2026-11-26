"""Build the "SCRUM Board" CC object (GLA-125): an empty cork board in an oak frame on a two-legged stand,
a 1-tile floor object cloned from the base-game etagere (sculptFloorQAEtagere_01, tuning object_sculpture) with
our own mesh, texture, catalog strings and thumbnails. In first person, linearboard.py offers "Open Linear
scrumboard" on it (definition id = OBJD below).

Usage: python3 build_corkboard.py <out.package> [preview.png]
Needs: numpy, Pillow. Template bytes: template_etagere.json (read from the game 2026-11-26, see its "source").

Layout (metres; y up, +z = the front, the side a Sim faces): posts at x +-0.485, cork panel 0.86 x 0.62 in a 3 cm
oak frame from 0.77 to 1.45 m, a ledge at 0.72 m, feet 0.48 m deep, braces from the ledge to the feet.
"""
import io, json, math, os, random, struct, sys, zlib
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
T = json.load(open(os.path.join(HERE, 'template_etagere.json')))
H = bytes.fromhex


def fnv32(s):
    h = 0x811C9DC5
    for c in s.lower().encode():
        h = (h * 0x01000193) & 0xFFFFFFFF
        h ^= c
    return h


def fnv64(s):
    h = 0xCBF29CE484222325
    for c in s.lower().encode():
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        h ^= c
    return h


def cc_id(s):
    return fnv64('glarthon:sims4vr:' + s) | 0x8000000000000000


OBJ = cc_id('LinearScrumboard')          # OBJD + COBJ instance = the object's definition id in Python
MODEL = cc_id('LinearScrumboard:model')
TEX_DIFFUSE = cc_id('LinearScrumboard:diffuse')
TEX_SHADOW = cc_id('LinearScrumboard:shadowmap')
STBL = fnv64('glarthon:sims4vr:LinearScrumboard:strings') & 0x00FFFFFFFFFFFFFF  # top byte 00 = English
NAME_KEY = fnv32('glarthon_LinearScrumboard_Name')
DESC_KEY = fnv32('glarthon_LinearScrumboard_Description')
NAME = 'SCRUM Board'  # user 2026-11-26: renamed from "Linear Scrumboard"
DESC = ('A cork board on an oak stand. In first person VR, pick "Open Linear scrumboard" on it to pin your '
        'Linear board on the cork, and move the notes by hand.')
PRICE = 150
# Placement test (GLA-125, 2026-11-26: the user could not place the board, and no game log names it). A second
# catalog object that is the same clone but keeps the etagere's own base-game model. If this one places and the
# SCRUM Board does not, the fault is in our model resources; if neither places, it is in the OBJD/COBJ clone.
TEST_OBJ = cc_id('ScrumBoardPlacementTest')
TEST_NAME_KEY = fnv32('glarthon_ScrumBoardPlacementTest_Name')
TEST_NAME = 'SCRUM Board placement test (etagere look)'

T_OBJD, T_COBJ, T_MODL, T_MLOD, T_IMG, T_STBL, T_THUMB = (0xC0DB5AE7, 0x319E4F1D, 0x01661233, 0x01D10F34,
                                                          0x00B2D882, 0x220557DA, 0x3C2A8647)
FLT_MAX = 3.4028234663852886e+38

# ---------------------------------------------------------------------------------------------------------------
# Texture atlas 256 x 512 (v down): cork rows 0-185, oak rows 192-447, metal rows 456-503.
TW, TH = 256, 512
CORK = (0, 0, 256, 186)
OAK = (0, 192, 256, 448)
METAL = (0, 456, 256, 504)
PX_PER_M = 256 / 0.98  # oak: one atlas width = 0.98 m of wood


def make_texture(seed=7):
    rng = np.random.default_rng(seed)
    img = np.zeros((TH, TW, 3), np.float32)
    img[:] = (200, 160, 112)  # the gaps between regions: oak-ish, so small mip levels don't bleed black seams

    def noise(h, w, scale):
        small = rng.random((max(2, h // scale), max(2, w // scale))).astype(np.float32)
        return np.asarray(Image.fromarray((small * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC),
                          np.float32) / 255

    # Cork: warm tan, granules at a few scales, a few dark and light flecks.
    x0, y0, x1, y1 = CORK
    h, w = y1 - y0, x1 - x0
    base = np.array([186, 140, 92], np.float32)
    n = 0.45 * noise(h, w, 2) + 0.35 * noise(h, w, 5) + 0.2 * noise(h, w, 13)
    cork = base[None, None, :] * (0.78 + 0.42 * n[..., None])
    fleck = rng.random((h, w))
    cork[fleck < 0.035] *= 0.62
    cork[fleck > 0.985] = cork[fleck > 0.985] * 0.6 + np.array([235, 205, 160]) * 0.4
    img[y0:y1, x0:x1] = cork

    # Oak: light oak, grain running along u, slow colour drift, a few darker streaks.
    x0, y0, x1, y1 = OAK
    h, w = y1 - y0, x1 - x0
    yy = np.arange(h, dtype=np.float32)[:, None]
    xx = np.arange(w, dtype=np.float32)[None, :]
    warp = 6 * noise(h, w, 32) + 2 * noise(h, w, 8)
    grain = 0.5 + 0.5 * np.sin((yy + warp * 2) * 0.8 + 1.5 * np.sin(xx / 37.0))
    grain = grain ** 3
    streak = noise(h, w, 4)
    drift = noise(h, w, 64)
    base = np.array([212, 172, 122], np.float32)
    oak = base[None, None, :] * (0.92 + 0.12 * drift[..., None] - 0.10 * grain[..., None]
                                 - 0.06 * (streak[..., None] > 0.8))
    img[y0:y1, x0:x1] = oak

    # Brushed dark metal for the pivot knobs.
    x0, y0, x1, y1 = METAL
    h, w = y1 - y0, x1 - x0
    img[y0:y1, x0:x1] = np.array([64, 64, 68], np.float32)[None, None, :] * (0.85 + 0.3 * noise(h, w, 3)[..., None])
    return np.clip(img, 0, 255).astype(np.uint8)


def dxt1_block(px):
    """px: 16 x 3 uint8 -> 8 bytes (endpoints on the principal axis, 4-colour mode)."""
    p = px.astype(np.float32)
    mean = p.mean(0)
    c = p - mean
    cov = c.T @ c
    if cov.trace() < 1e-3:
        axis = np.array([1.0, 1.0, 1.0]) / math.sqrt(3)
    else:
        w, v = np.linalg.eigh(cov)
        axis = v[:, -1]
    t = c @ axis
    lo, hi = mean + axis * t.min(), mean + axis * t.max()

    def to565(col):
        col = np.clip(np.round(col), 0, 255).astype(int)
        return ((col[0] >> 3) << 11) | ((col[1] >> 2) << 5) | (col[2] >> 3)

    def from565(v):
        return np.array([((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31])

    a, b = to565(hi), to565(lo)
    if a < b:
        a, b = b, a
    if a == b:
        return struct.pack('<HHI', a, b, 0)
    ca, cb = from565(a), from565(b)
    pal = np.array([ca, cb, (2 * ca + cb) / 3, (ca + 2 * cb) / 3])
    d = ((p[:, None, :] - pal[None, :, :]) ** 2).sum(2)
    idx = d.argmin(1)
    bits = 0
    for i in range(16):
        bits |= int(idx[i]) << (2 * i)
    return struct.pack('<HHI', a, b, bits)


def dds_dxt1(rgb):
    """rgb: H x W x 3 -> DDS (DXT1) with a full mip chain."""
    levels = []
    im = Image.fromarray(rgb)
    while True:
        a = np.asarray(im)
        h, w = a.shape[:2]
        out = bytearray()
        for by in range(0, max(h, 4), 4):
            for bx in range(0, max(w, 4), 4):
                blk = np.zeros((4, 4, 3), np.uint8)
                part = a[by:by + 4, bx:bx + 4]
                blk[:part.shape[0], :part.shape[1]] = part
                if part.shape[0] < 4 or part.shape[1] < 4:
                    blk[:, :] = part.reshape(-1, 3).mean(0)
                out += dxt1_block(blk.reshape(16, 3))
        levels.append(bytes(out))
        if w == 1 and h == 1:
            break
        im = im.resize((max(1, w // 2), max(1, h // 2)), Image.LANCZOS)
    h, w = rgb.shape[:2]
    hdr = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x000A1007, h, w, len(levels[0]), 0, len(levels))
    pf = struct.pack('<II4sIIIII', 32, 0x4, b'DXT1', 0, 0, 0, 0, 0)
    caps = struct.pack('<IIIII', 0x401008, 0, 0, 0, 0)
    return hdr + pf + caps + b''.join(levels)


# ---------------------------------------------------------------------------------------------------------------
# Geometry: boxes with per-face material ('cork', 'oak', 'metal').
class Mesh:
    def __init__(self):
        self.v = []  # (pos, normal, uv, tangent)
        self.i = []
        self.boxes = []  # corner sets for the shadow mesh
        self.faces = []  # (corners, normal, material) for the thumbnail
        self.rng = random.Random(3)
        self.flipped = 0

    def quad(self, c, n, mat, tdir):
        """c: 4 corners counter-clockwise seen from outside; tdir: the direction u runs along."""
        n = np.asarray(n, float)
        c = [np.asarray(p, float) for p in c]
        e1, e2 = c[1] - c[0], c[3] - c[0]
        L1, L2 = np.linalg.norm(e1), np.linalg.norm(e2)
        if mat == 'cork':
            x0, y0, x1, y1 = CORK
            uv = [(x0, y1), (x1, y1), (x1, y0), (x0, y0)]
            tdir = e1
        elif mat == 'metal':
            x0, y0, x1, y1 = METAL
            uv = [(x0 + 8, y1 - 8), (x0 + 40, y1 - 8), (x0 + 40, y0 + 8), (x0 + 8, y0 + 8)]
            tdir = e1
        else:
            # Wood grain along the face's longer edge.
            along_first = L1 >= L2
            la, lb = (L1, L2) if along_first else (L2, L1)
            ua = la * PX_PER_M
            ub = max(2.0, lb * PX_PER_M)
            u0 = self.rng.uniform(0, max(0.0, 256 - ua))
            v0 = self.rng.uniform(OAK[1] + 2, OAK[3] - 2 - ub)
            if along_first:
                uv = [(u0, v0 + ub), (u0 + ua, v0 + ub), (u0 + ua, v0), (u0, v0)]
                tdir = e1
            else:
                uv = [(u0, v0), (u0, v0 + ub), (u0 + ua, v0 + ub), (u0 + ua, v0)]
                tdir = e2
        t = np.asarray(tdir, float)
        t = t - n * np.dot(t, n)
        t /= np.linalg.norm(t)
        base = len(self.v)
        for p, (u, v) in zip(c, uv):
            self.v.append((p, n, (u / TW, v / TH), t))
        # The game's front faces wind counter-clockwise around the normal (checked on the template's mesh).
        if np.dot(np.cross(c[1] - c[0], c[2] - c[0]), n) > 0:
            self.i += [base, base + 1, base + 2, base, base + 2, base + 3]
        else:
            self.i += [base, base + 2, base + 1, base, base + 3, base + 2]
            self.flipped += 1
        self.faces.append((c, n, mat))

    def box(self, lo, hi, front='oak', back='oak', side='oak', topbottom='oak'):
        (x0, y0, z0), (x1, y1, z1) = lo, hi
        P = lambda x, y, z: (x, y, z)
        self.quad([P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)], (0, 0, 1), front, (1, 0, 0))
        self.quad([P(x1, y0, z0), P(x0, y0, z0), P(x0, y1, z0), P(x1, y1, z0)], (0, 0, -1), back, (-1, 0, 0))
        self.quad([P(x1, y0, z1), P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1)], (1, 0, 0), side, (0, 0, -1))
        self.quad([P(x0, y0, z0), P(x0, y0, z1), P(x0, y1, z1), P(x0, y1, z0)], (-1, 0, 0), side, (0, 0, 1))
        self.quad([P(x0, y1, z1), P(x1, y1, z1), P(x1, y1, z0), P(x0, y1, z0)], (0, 1, 0), topbottom, (1, 0, 0))
        self.quad([P(x0, y0, z0), P(x1, y0, z0), P(x1, y0, z1), P(x0, y0, z1)], (0, -1, 0), topbottom, (1, 0, 0))
        self.boxes.append([P(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)])

    def beam(self, a, b, w, d, mat='oak'):
        """A beam from a to b (centre line), w wide (across, in the plane with z) and d deep (along z)."""
        a, b = np.asarray(a, float), np.asarray(b, float)
        ax = b - a
        L = np.linalg.norm(ax)
        ax /= L
        zref = np.array([0, 0, 1.0]) if abs(ax[2]) < 0.9 else np.array([1.0, 0, 0])
        side = np.cross(zref, ax)
        side /= np.linalg.norm(side)
        dep = np.cross(ax, side)
        hw, hd = side * w / 2, dep * d / 2
        corners = {}
        for s in (-1, 1):
            for t in (-1, 1):
                corners[(0, s, t)] = a + s * hw + t * hd
                corners[(1, s, t)] = b + s * hw + t * hd
        C = corners
        self.quad([C[(0, -1, 1)], C[(0, 1, 1)], C[(1, 1, 1)], C[(1, -1, 1)]], dep, mat, ax)
        self.quad([C[(0, 1, -1)], C[(0, -1, -1)], C[(1, -1, -1)], C[(1, 1, -1)]], -dep, mat, ax)
        self.quad([C[(0, 1, 1)], C[(0, 1, -1)], C[(1, 1, -1)], C[(1, 1, 1)]], side, mat, ax)
        self.quad([C[(0, -1, -1)], C[(0, -1, 1)], C[(1, -1, 1)], C[(1, -1, -1)]], -side, mat, ax)
        self.quad([C[(1, -1, 1)], C[(1, 1, 1)], C[(1, 1, -1)], C[(1, -1, -1)]], ax, mat, side)
        self.quad([C[(0, -1, -1)], C[(0, 1, -1)], C[(0, 1, 1)], C[(0, -1, 1)]], -ax, mat, side)
        self.boxes.append([tuple(p) for p in C.values()])


# Board, in the object's space. The numbers linearboard.py uses (BOARD_*) must match.
BOARD_Y0, BOARD_Y1 = 0.77, 1.45     # frame outer edges
BOARD_HALF_W = 0.46
CORK_Y0, CORK_Y1, CORK_HALF_W = 0.80, 1.42, 0.43
FRAME_Z = 0.012                     # frame half depth
POST_X, POST_HALF = 0.485, 0.015
PIVOT_Y = 1.11


def build_mesh():
    m = Mesh()
    # Cork panel (both faces cork: a reversible board), oak edges hidden in the frame.
    m.box((-CORK_HALF_W, CORK_Y0, -0.006), (CORK_HALF_W, CORK_Y1, 0.006), front='cork', back='cork')
    # Frame.
    m.box((-BOARD_HALF_W, CORK_Y1, -FRAME_Z), (BOARD_HALF_W, BOARD_Y1, FRAME_Z))
    m.box((-BOARD_HALF_W, BOARD_Y0, -FRAME_Z), (BOARD_HALF_W, CORK_Y0, FRAME_Z))
    m.box((-BOARD_HALF_W, CORK_Y0, -FRAME_Z), (-CORK_HALF_W, CORK_Y1, FRAME_Z))
    m.box((CORK_HALF_W, CORK_Y0, -FRAME_Z), (BOARD_HALF_W, CORK_Y1, FRAME_Z))
    for s in (-1, 1):
        x = s * POST_X
        # Post.
        m.box((x - POST_HALF, 0.03, -0.015), (x + POST_HALF, 1.20, 0.015))
        # Foot.
        m.box((x - 0.02, 0.0, -0.24), (x + 0.02, 0.03, 0.24))
        # Pivot: a bolt through the post into the frame, a knob on the outside.
        inner, outer = s * BOARD_HALF_W, s * (POST_X - POST_HALF)
        m.box((min(inner, outer), PIVOT_Y - 0.008, -0.008), (max(inner, outer), PIVOT_Y + 0.008, 0.008),
              'metal', 'metal', 'metal', 'metal')
        k0, k1 = s * (POST_X + POST_HALF), s * (POST_X + POST_HALF + 0.022)
        m.box((min(k0, k1), PIVOT_Y - 0.016, -0.016), (max(k0, k1), PIVOT_Y + 0.016, 0.016),
              'metal', 'metal', 'metal', 'metal')
        # Brace from the ledge down to the foot, and two small braces from the post to the foot.
        m.beam((s * 0.27, 0.72, 0), (s * (POST_X - 0.02), 0.04, 0), 0.026, 0.02)
        for zs in (-1, 1):
            m.beam((x, 0.20, zs * 0.012), (x, 0.03, zs * 0.17), 0.022, 0.02)
    # Ledge between the posts, under the board.
    m.box((-(POST_X - POST_HALF), 0.705, -0.02), (POST_X - POST_HALF, 0.735, 0.02))
    return m


# ---------------------------------------------------------------------------------------------------------------
# RCOL / model resources.
def ikey(inst, typ, grp=0):
    return struct.pack('<QII', inst, typ, grp)


def rcol(public, ikeys, ekeys, chunks):
    out = struct.pack('<5I', 3, public, 0, len(ekeys), len(chunks))
    out += b''.join(ikeys) + b''.join(ekeys)
    pos = len(out) + 8 * len(chunks)
    table = b''
    for c in chunks:
        table += struct.pack('<II', pos, len(c))
        pos += len(c)
    return out + table + b''.join(chunks)


def priv(i, public):
    return 0x10000000 | (i - public + 1)


def ibuf(idx):
    out = struct.pack('<4sIII', b'IBUF', 2, 1, 0)
    prev = 0
    for k in idx:
        out += struct.pack('<h', k - prev)
        prev = k
    return out


def mesh_entry(name, mat, vrtf, vbuf, ib, flags, vc, prims, box, skin, scale):
    body = struct.pack('<6I4I2I', name, mat, vrtf, vbuf, ib, flags, 0, 0, 0, 0, vc, prims)
    body += struct.pack('<6f', *box)
    body += struct.pack('<III', skin, 1, 0xCD68F001)  # the object's one joint (template)
    body += struct.pack('<I', scale)
    body += struct.pack('<4I', 0, 0, 0, 0) + struct.pack('<f', 1.0) + struct.pack('<II', 0, 0)
    return struct.pack('<I', len(body)) + body


def bounds(points):
    p = np.asarray(points)
    lo, hi = p.min(0), p.max(0)
    return (lo[0], lo[1], lo[2], hi[0], hi[1], hi[2])


def s16(x):
    return int(max(-32767, min(32767, round(x))))


def u8n(x):
    return int(max(0, min(255, round((x + 1) * 127.5))))


def build_model(m):
    POS = 16383.0
    vb = bytearray()
    for p, n, uv, t in m.v:
        vb += struct.pack('<4h', s16(p[0] * POS), s16(p[1] * POS), s16(p[2] * POS), 16383)
        vb += bytes([u8n(n[0]), u8n(n[1]), u8n(n[2]), 0x80])
        vb += struct.pack('<2h', s16(uv[0] * 32767), s16(uv[1] * 32767))
        vb += bytes([0, 0, 0, 0, 255, 0, 0, 0])
        vb += bytes([u8n(t[0]), u8n(t[1]), u8n(t[2]), 0xFF])
        vb += struct.pack('<2h', s16(uv[0] * 32767), s16(uv[1] * 32767))
    box = bounds([v[0] for v in m.v])
    pub = 2
    # Chunks: 0 MODL, 1 MLOD | 2 MTST, 3 VRTF, 4 VBUF, 5 IBUF, 6 SKIN, 7 MATD scale, 8 MATD, 9 swizzle.
    R = lambda i: priv(i, pub)
    mtst_name = fnv32('LinearScrumboard')
    # Material: the template's clean phong material, its textures swapped for ours.
    matd = bytearray(H(T['m18']))
    struct.pack_into('<I', matd, 8, mtst_name)

    def tex_param(h, inst):
        block = 20 + 8
        cnt = struct.unpack_from('<I', matd, block + 12)[0]
        for k in range(cnt):
            p = block + 16 + k * 16
            hh, ty, n, off = struct.unpack_from('<4I', matd, p)
            if hh == h:
                struct.pack_into('<QII', matd, block + off, inst, T_IMG, 0)
                return
        raise KeyError(hex(h))
    tex_param(0x6CC0FD85, TEX_DIFFUSE)  # diffusemap
    tex_param(0xC3FAAC4F, TEX_DIFFUSE)  # alphamap (same picture; DXT1 = opaque)
    tex_param(0x79C44C9B, TEX_SHADOW)   # the object's own baked map on UV 1 (template: dark = clean)
    # Material set: every state and variant of the template -> our one material.
    tm = H(T['m10'])
    count = struct.unpack_from('<I', tm, 16)[0]
    mtst = bytearray(struct.pack('<4sIIII', b'MTST', 0x300, mtst_name, 0, count))
    for k in range(count):
        ref, state, variant = struct.unpack_from('<III', tm, 20 + 12 * k)
        mtst += struct.pack('<III', R(8), state, variant)
    vbuf = struct.pack('<4sIII', b'VBUF', 0x101, 0, R(9)) + bytes(vb)
    swz = struct.pack('<4I', 1, 32, len(m.v), 0) + struct.pack('<8I', 2, 2, 1, 2, 1, 1, 1, 2)
    mlod = struct.pack('<4sII', b'MLOD', 0x205, 1) + mesh_entry(
        fnv32('LinearScrumboard_main'), R(2), R(3), R(4), R(5), 0x185003, len(m.v), len(m.i) // 3, box, R(6), R(7))
    modl = bytearray(struct.pack('<4sII', b'MODL', 0x300, 2) + struct.pack('<6f', *box))
    modl += H(T['m0'])[36:56]
    modl += struct.pack('<IIIff', 0x00000002, 0, 0, 0.0, FLT_MAX)          # LOD 0: our MLOD chunk
    modl += struct.pack('<IIIff', 0x30000001, 0, 0x10000, FLT_MAX, FLT_MAX)  # shadow caster: external MLOD
    chunks = [bytes(modl), mlod, bytes(mtst), H(T['m11']), vbuf, ibuf(m.i), H(T['m14']), H(T['m15']),
              bytes(matd), swz]
    ik = [ikey(MODEL, T_MODL), ikey(MODEL, T_MLOD), ikey(MODEL, 0x02019972), ikey(0, 0x01D0E723),
          ikey(MODEL, 0x01D0E6FB), ikey(MODEL, 0x01D0E70F), ikey(0, 0x01D0E76B), ikey(0, 0x01D0E75D),
          ikey(fnv64('LinearScrumboard:matd'), 0x01D0E75D), ikey(0, 0)]
    ek = [ikey(MODEL, T_MLOD, 0x10000)]
    modl_res = rcol(pub, ik, ek, chunks)

    # Shadow caster (group 0x10000): positions only, one copy of each box's 8 corners.
    pts, idx = [], []
    for corners in m.boxes:
        c = np.asarray(corners, float)
        ctr = c.mean(0)
        base = len(pts)
        pts += list(c)
        # Hull of 8 points of a box: triangulate faces by picking, per face, the 4 points on it.
        # The box's own axes from corner differences.
        d = c - c[0]
        dirs = sorted(((np.linalg.norm(v), tuple(v)) for v in d[1:]), key=lambda x: x[0])
        e = [np.asarray(dirs[0][1]), None, None]
        for L, v in dirs[1:]:
            v = np.asarray(v)
            if e[1] is None and abs(np.dot(v, e[0])) < 1e-6 * max(1e-9, L):
                e[1] = v
        e[2] = np.cross(e[0], e[1])
        for a in e:
            a = a / np.linalg.norm(a)
            proj = (c - ctr) @ a
            for sgn in (1, -1):
                on = [k for k in range(8) if (proj[k] * sgn) > 0]
                q = c[on] - ctr
                # order the 4 face points around the face normal
                nrm = a * sgn
                ref = q[0] - nrm * np.dot(q[0], nrm)
                ref /= np.linalg.norm(ref)
                oth = np.cross(nrm, ref)
                ang = [math.atan2(np.dot(p, oth), np.dot(p, ref)) for p in q]
                order = [on[k] for k in np.argsort(ang)]
                idx += [base + order[0], base + order[1], base + order[2],
                        base + order[0], base + order[2], base + order[3]]
    svb = bytearray()
    for p in pts:
        svb += struct.pack('<4h', s16(p[0] * POS), s16(p[1] * POS), s16(p[2] * POS), 16383)
    spub = 1
    SR = lambda i: priv(i, spub)
    sbox = bounds(pts)
    smlod = struct.pack('<4sII', b'MLOD', 0x205, 1) + mesh_entry(
        fnv32('LinearScrumboard_shadow'), SR(1), 0, SR(2), SR(3), 0x181003, len(pts), len(idx) // 3, sbox,
        SR(4), SR(5))
    smatd = bytearray(H(T['s1']))
    schunks = [smlod, bytes(smatd), struct.pack('<4sIII', b'VBUF', 0x101, 0, SR(6)) + bytes(svb), ibuf(idx),
               H(T['s4']), H(T['s5']), struct.pack('<4I', 1, 8, len(pts), 0) + struct.pack('<2I', 2, 2)]
    sik = [ikey(MODEL, T_MLOD, 0x10000), ikey(fnv64('LinearScrumboard:shadowmatd'), 0x01D0E75D),
           ikey(0, 0x0229684B), ikey(0, 0x0229684F), ikey(0, 0x01D0E76B), ikey(0, 0x01D0E75D), ikey(0, 0)]
    shadow_res = rcol(spub, sik, [], schunks)
    return modl_res, shadow_res, box


# ---------------------------------------------------------------------------------------------------------------
# OBJD, COBJ, STBL.
def lstr(s):
    b = s.encode()
    return struct.pack('<I', len(b)) + b


def okey(inst, typ, grp=0):
    return struct.pack('<IIIII', 4, inst >> 32, inst & 0xFFFFFFFF, typ, grp)


def build_objd(name='linearScrumboard_GLAsims4vr_01_set1', model=None):
    t = H(T['objd'])
    tp = struct.unpack_from('<I', t, 2)[0]
    cnt = struct.unpack_from('<H', t, tp)[0]
    ents = [struct.unpack_from('<II', t, tp + 2 + 8 * k) for k in range(cnt)]
    offs = sorted(o for _, o in ents) + [tp]
    raw = {tg: t[o:min(x for x in offs if x > o)] for tg, o in ents}
    raw[0xE7F07786] = lstr(name)
    if model is not None:  # None keeps the etagere's own model (the placement test object)
        raw[0x8D20ACC6] = okey(model, T_MODL)
    raw[0xE4F4FAA4] = struct.pack('<I', PRICE)
    body, table = bytearray(), []
    pos = 6
    for tg, _ in ents:
        table.append((tg, pos))
        body += raw[tg]
        pos += len(raw[tg])
    out = struct.pack('<HI', 2, 6 + len(body)) + bytes(body) + struct.pack('<H', len(table))
    out += b''.join(struct.pack('<II', tg, o) for tg, o in table)
    return out


def build_cobj(name_key=None, desc_key=None):
    c = bytearray(H(T['cobj']))
    assert struct.unpack_from('<II', c, 0) == (0x1A, 0x0B)
    struct.pack_into('<III', c, 8, name_key or NAME_KEY, desc_key or DESC_KEY, PRICE)
    return bytes(c)


def build_stbl(entries):
    body = b''
    total = 0
    for k, s in entries:
        b = s.encode('utf-8')
        body += struct.pack('<IBH', k, 0, len(b)) + b
        total += len(b) + 1
    return struct.pack('<4sHBQHI', b'STBL', 5, 0, len(entries), 0, total) + body


# ---------------------------------------------------------------------------------------------------------------
# Thumbnails: a small flat-shaded render, JPEG with the game's 'ALFA' segment (a grayscale PNG alpha).
def render(m, size, tex):
    S = size * 3
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    yaw, pitch = math.radians(-30), math.radians(14)
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)

    def view(p):
        x, y, z = p
        x, z = x * cy + z * sy, -x * sy + z * cy
        y, z = y * cp - z * sp, y * sp + z * cp
        return x, y, z

    cols = {'cork': tex[CORK[1]:CORK[3], :].reshape(-1, 3).mean(0),
            'oak': tex[OAK[1]:OAK[3], :].reshape(-1, 3).mean(0),
            'metal': tex[METAL[1]:METAL[3], :].reshape(-1, 3).mean(0)}
    light = np.array([0.4, 0.7, 0.6])
    light /= np.linalg.norm(light)
    faces = []
    for c, n, mat in m.faces:
        vc = [view(p) for p in c]
        vn = view(n)
        if vn[2] <= 0:
            continue
        depth = sum(p[2] for p in vc) / 4
        shade = 0.55 + 0.45 * max(0.0, float(np.dot(n, light)))
        faces.append((depth, vc, tuple(int(min(255, v * shade)) for v in cols[mat])))
    faces.sort(key=lambda f: f[0])
    scale = S / 1.7
    for depth, vc, col in faces:
        pts = [(S / 2 + p[0] * scale, S * 0.93 - p[1] * scale) for p in vc]
        d.polygon(pts, fill=col + (255,))
    # The cork texture on the front panel would be lost at this size: keep the flat colour.
    return img.resize((size, size), Image.LANCZOS)


def thumb_bytes(img):
    rgb = Image.new('RGB', img.size, (0, 0, 0))
    rgb.paste(img, mask=img.split()[3])
    j = io.BytesIO()
    rgb.save(j, 'JPEG', quality=92)
    p = io.BytesIO()
    img.split()[3].save(p, 'PNG')
    png = p.getvalue()
    seg = b'ALFA' + struct.pack('>I', len(png)) + png
    jb = j.getvalue()
    app = b'\xff\xe0' + struct.pack('>H', len(seg) + 2) + seg
    # After SOI + the JFIF APP0, like the game's own thumbnails.
    jfif_len = struct.unpack('>H', jb[4:6])[0]
    cut = 4 + jfif_len
    return jb[:cut] + app + jb[cut:]


# ---------------------------------------------------------------------------------------------------------------
def dbpf(resources):
    """resources: list of (type, group, instance, bytes). zlib-compressed like the game's packages."""
    body = bytearray(b'\0' * 96)
    index = struct.pack('<I', 0)
    for t, g, i, data in resources:
        comp = zlib.compress(data, 9)
        use = comp if len(comp) < len(data) else data
        off = len(body)
        body += use
        index += struct.pack('<IIIIIIIHH', t, g, i >> 32, i & 0xFFFFFFFF, off, len(use) | 0x80000000, len(data),
                             0x5A42 if use is comp else 0, 1)
    ipos = len(body)
    body += index
    hdr = struct.pack('<4sIIIIIIIIIII', b'DBPF', 2, 1, 0, 0, 0, 0, 0, 0, len(resources), 0, len(index))
    hdr += struct.pack('<III', 0, 0, 0) + struct.pack('<I', 3) + struct.pack('<Q', ipos)
    hdr = hdr.ljust(96, b'\0')
    body[:96] = hdr
    return bytes(body)


def main():
    out = sys.argv[1]
    mesh = build_mesh()
    tex = make_texture()
    modl, shadow, box = build_model(mesh)
    shadowmap = dds_dxt1(np.zeros((32, 32, 3), np.uint8))
    res = [
        (T_OBJD, 0, OBJ, build_objd(model=MODEL)),
        (T_COBJ, 0, OBJ, build_cobj()),
        (T_OBJD, 0, TEST_OBJ, build_objd('scrumBoardPlacementTest_GLAsims4vr_01_set1')),
        (T_COBJ, 0, TEST_OBJ, build_cobj(TEST_NAME_KEY, DESC_KEY)),
        (T_MODL, 0, MODEL, modl),
        (T_MLOD, 0x10000, MODEL, shadow),
        (T_IMG, 0, TEX_DIFFUSE, dds_dxt1(tex)),
        (T_IMG, 0, TEX_SHADOW, shadowmap),
        (T_STBL, 0x80000000, STBL, build_stbl([(NAME_KEY, NAME), (DESC_KEY, DESC), (TEST_NAME_KEY, TEST_NAME)])),
    ]
    big = render(mesh, 232, tex)
    for inst in (OBJ, TEST_OBJ):
        for n, size in ((1, 64), (2, 78), (3, 116)):
            grp = (((inst & 0x0FFFFFFF) << 4) | n) & 0xFFFFFFFF
            res.append((T_THUMB, grp, inst, thumb_bytes(big.resize((size, size), Image.LANCZOS))))
    data = dbpf(res)
    open(out, 'wb').write(data)
    if len(sys.argv) > 2:
        big.save(sys.argv[2])
        Image.fromarray(tex).save(sys.argv[2].replace('.png', '_texture.png'))
    print('wrote', out, len(data), 'bytes;', len(mesh.v), 'vertices', len(mesh.i) // 3, 'triangles; box',
          [round(b, 3) for b in box])
    print('OBJD/COBJ (definition id) 0x%016X  model 0x%016X' % (OBJ, MODEL))


if __name__ == '__main__':
    main()
