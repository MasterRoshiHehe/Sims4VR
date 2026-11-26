"""The real shape of a screen (Sims Forever, v0.13.86): how far each part of
an object's VideoSurface mesh sits behind the front of its box, so Sims 1's
picture lies on the screen itself, curved screens too, instead of on a flat
panel in front of it.

The model's VideoSurface meshes (objmenu._video_screen finds the same ones)
are read from the game's packages: their vertex format (VRTF) and positions
(VBUF). The positions become a grid of depths over the screen's box (front
view, x across, y up) from a smooth surface fitted through them (_grid). A
flat screen comes out all (nearly) zeros. Anything that
doesn't add up (positions outside the mesh's own bounds) gives None and the
picture stays flat on the front, as before.
"""
import struct

from sims4vr import native

GRID_X = 40
GRID_Y = 16

_cache = {}  # model instance -> its screens (_screens) or None
_logs = 0

# VRTF element formats: (struct format, count, normalise) for positions.
_USAGE_POSITION = 0
_FLOAT3 = 2
_FLOAT4 = 3
_SHORT4 = 7
_SHORT4N = 10
_USHORT4N = 12


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 160:
        native._log('screenmesh: ' + message)


def _vrtf(data, pos, size):
    """(stride, position offset, position format, (normal offset, format) or None) of a VRTF chunk, or None."""
    _tag, _version, stride, count, extended = struct.unpack_from('<4sIiiI', data, pos)
    if not (4 <= stride <= 256 and 1 <= count <= 32):
        return None
    for wide in ((True, False) if extended else (False, True)):
        at, found, ok, normal = pos + 20, None, True, None
        for _ in range(count):
            if wide:
                if at + 16 > pos + size:
                    ok = False
                    break
                usage, _index, fmt, offset = struct.unpack_from('<4I', data, at)
                at += 16
            else:
                if at + 4 > pos + size:
                    ok = False
                    break
                usage, _index, fmt, offset = struct.unpack_from('<4B', data, at)
                at += 4
            if fmt > 16 or offset >= stride:
                ok = False
                break
            if usage == _USAGE_POSITION and found is None:
                found = (offset, fmt)
            if usage == 1 and normal is None:
                normal = (offset, fmt)
        if ok and found is not None:
            return stride, found[0], found[1], normal
    return None


def _position(data, at, fmt):
    if fmt in (_FLOAT3, _FLOAT4):
        return struct.unpack_from('<3f', data, at)
    if fmt in (_SHORT4, _SHORT4N, _USHORT4N):
        x, y, z = struct.unpack_from('<3h', data, at)
        w = struct.unpack_from('<H', data, at + 6)[0]
        s = float(w) if w else 32767.0
        return x / s, y / s, z / s
    return None


def _normal(data, at, fmt):
    """A vertex normal (as modelparse.cpp decodeNormal), or None."""
    if fmt in (_FLOAT3, _FLOAT4):
        return struct.unpack_from('<3f', data, at)
    if fmt in (8, 4):
        return tuple(b / 127.5 - 1 for b in struct.unpack_from('<3B', data, at))
    if fmt == 5:
        b, g, r = struct.unpack_from('<3B', data, at)
        return r / 127.5 - 1, g / 127.5 - 1, b / 127.5 - 1
    if fmt in (13, 14):
        v, = struct.unpack_from('<I', data, at)
        return tuple(((v >> (10 * k)) & 1023) / 511.5 - 1 for k in range(3))
    if fmt in (_SHORT4, _SHORT4N):
        return tuple(x / 32767.0 for x in struct.unpack_from('<3h', data, at))
    return None


def _inside(p, box, slack):
    return all(box[i] - slack <= p[i] <= box[3 + i] + slack for i in range(3))


def _indices(data, public, chunks, entry, count, min_vertex):
    """The triangle corners of one MLOD mesh as vertex numbers 0..count-1 (IBUF: 16 bytes of header, flags at +8:
    bit 1 = 32-bit, bit 0 = each the difference to the one before; checked on real packages in modelparse.cpp), or
    None."""
    from sims4vr import objmenu
    flags, = struct.unpack_from('<I', data, entry + 20)
    ibuf_ref = struct.unpack_from('<I', data, entry + 16)[0]
    start_index, = struct.unpack_from('<i', data, entry + 32)
    primitives, = struct.unpack_from('<i', data, entry + 44)
    if (flags & 0xFF) != 3 or primitives <= 0 or primitives > 400000 or start_index < 0:
        return None
    ibuf = objmenu._chunk(ibuf_ref, public, chunks, (b'IBUF',))
    if ibuf is None:
        return None
    iflags, = struct.unpack_from('<I', data, ibuf[1] + 8)
    wide, differenced = bool(iflags & 2), bool(iflags & 1)
    size = 4 if wide else 2
    total = (ibuf[2] - 16) // size
    take = primitives * 3
    if start_index + take > total:
        return None
    fmt = '<{}{}'.format(start_index + take, 'i' if wide else 'h')
    raw = struct.unpack_from(fmt, data, ibuf[1] + 16)
    out, acc = [], 0
    for k, v in enumerate(raw):
        acc = acc + v if differenced else (v if wide else v & 0xFFFF)
        if not wide:
            acc &= 0xFFFF
        if k >= start_index:
            out.append(acc)
    low, high = min(out), max(out)
    shift = 0
    if high >= count:
        if low >= min_vertex and high - min_vertex < count:
            shift = min_vertex
        else:
            return None
    return [i - shift for i in out]


def _islands(points, corners, normals):
    """The vertices (their positions) in groups that share triangles, each with the sum of its vertex normals."""
    parent = list(range(len(points)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for k in range(0, len(corners) - 2, 3):
        a, b, c = corners[k], corners[k + 1], corners[k + 2]
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    groups = {}
    for i in set(corners):
        groups.setdefault(find(i), []).append(i)
    return [([points[i] for i in g], _sum_normals([normals[i] for i in g])) for g in groups.values()]


def _sum_normals(vectors):
    total = [0.0, 0.0, 0.0]
    for v in vectors:
        if v is not None:
            for k in range(3):
                total[k] += v[k]
    return tuple(total)


def _mesh_points(data, public, chunks, entry, box):
    """Positions of one MLOD mesh as islands ((points, summed vertex normals); one per group of
    vertices sharing triangles, a single one when the indices can't be read),
    or None when they don't fit its bounds."""
    from sims4vr import objmenu
    vrtf_ref, vbuf_ref = struct.unpack_from('<2I', data, entry + 8)
    stream, start_vertex, _start_index, min_vertex, count = struct.unpack_from('<5i', data, entry + 24)
    vrtf = objmenu._chunk(vrtf_ref, public, chunks, (b'VRTF',))
    vbuf = objmenu._chunk(vbuf_ref, public, chunks, (b'VBUF',))
    if vrtf is None or vbuf is None or not 0 < count <= 65536:
        return None, 'no vertex format or buffer ({} vertices)'.format(count)
    layout = _vrtf(data, vrtf[1], vrtf[2])
    if layout is None:
        return None, 'unreadable vertex format'
    stride, offset, fmt, nrm = layout
    slack = 0.01 + 0.02 * max(box[3 + i] - box[i] for i in range(3))
    tried = []
    # Where the vertices start: past the VBUF header, at the mesh's stream offset
    # (the start vertex too, in some layouts).
    for base in (vbuf[1] + 16 + stream, vbuf[1] + 16 + stream + start_vertex * stride,
                 vbuf[1] + 16 + stream + min_vertex * stride, vbuf[1] + stream):
        if base < vbuf[1] or base + count * stride > vbuf[1] + vbuf[2]:
            continue
        points = []
        for i in range(count):
            p = _position(data, base + i * stride + offset, fmt)
            if p is None:
                return None, 'position format {}'.format(fmt)
            points.append(p)
        normals = [_normal(data, base + i * stride + nrm[0], nrm[1]) if nrm else None for i in range(count)]
        inside = sum(1 for p in points if _inside(p, box, slack))
        tried.append(inside)
        if inside >= 0.95 * count and inside >= 3:
            try:
                corners = _indices(data, public, chunks, entry, count, min_vertex)
            except Exception:
                corners = None
            if corners is not None:
                islands = [([p for p in pts if _inside(p, box, slack)], n) for pts, n in _islands(points, corners, normals)]
                islands = [island for island in islands if len(island[0]) >= 3]
                if islands:
                    return islands, None
            return [([p for p in points if _inside(p, box, slack)], _sum_normals(normals))], None
    return None, 'positions outside the mesh bounds (format {}, stride {}, {} vertices, inside {})'.format(
        fmt, stride, count, tried)


def _terms(u, v):
    return (1.0, u, u * u, u * u * u, u * u * u * u, v, v * v, u * v)


def _solve(m, r):
    """m x = r by Gaussian elimination (partial pivoting); None when singular."""
    n = len(r)
    m = [row[:] + [r[i]] for i, row in enumerate(m)]
    for c in range(n):
        k = max(range(c, n), key=lambda i: abs(m[i][c]))
        if abs(m[k][c]) < 1e-12:
            return None
        m[c], m[k] = m[k], m[c]
        for i in range(c + 1, n):
            f = m[i][c] / m[c][c]
            for j in range(c, n + 1):
                m[i][j] -= f * m[c][j]
    x = [0.0] * n
    for i in range(n - 1, -1, -1):
        x[i] = (m[i][n] - sum(m[i][j] * x[j] for j in range(i + 1, n))) / m[i][i]
    return x


def _grid(points, extent):
    """Depths behind the front (<= 0) on a GRID_X x GRID_Y grid over extent
    (a0, b0, a1, b1): a smooth surface (least squares, up to 4th power across
    and 2nd up) through the points (a across, b up, c along the normal, 0 at
    the front), moved forward until no point is in front of it (and a little
    more), so the game's own picture never shows through. v0.13.87 took the
    nearest vertices' mean per grid point: bumpy, and behind the glass in
    places (user 2026-10-14 18:30Z: "extremely bulgey and badly cut out")."""
    a0, b0, a1, b1 = extent
    w, h = a1 - a0, b1 - b0
    if not (w > 0 and h > 0):
        return [0.0] * (GRID_X * GRID_Y)

    def uv(x, y):
        return 2.0 * (x - a0) / w - 1.0, 2.0 * (y - b0) / h - 1.0

    n = len(_terms(0.0, 0.0))
    m = [[0.0] * n for _ in range(n)]
    r = [0.0] * n
    for p in points:
        t = _terms(*uv(p[0], p[1]))
        for i in range(n):
            r[i] += t[i] * p[2]
            for j in range(n):
                m[i][j] += t[i] * t[j]
    for i in range(n):
        m[i][i] += 1e-6 * len(points)  # a few vertices only: stay smooth
    k = _solve(m, r)
    if k is None:
        k = [min(p[2] for p in points)] + [0.0] * (n - 1)

    def fit(u, v):
        return sum(c * t for c, t in zip(k, _terms(u, v)))

    ahead = max(p[2] - fit(*uv(p[0], p[1])) for p in points)
    lift = max(0.0, ahead) + 0.002
    depths = []
    for j in range(GRID_Y):
        v = 2.0 * j / (GRID_Y - 1) - 1.0
        for i in range(GRID_X):
            u = 2.0 * i / (GRID_X - 1) - 1.0
            depths.append(min(0.002, fit(u, v) + lift))
    return depths


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _unit(a):
    d = _dot(a, a) ** 0.5
    return (a[0] / d, a[1] / d, a[2] / d) if d > 1e-9 else None


def _eigen(c):
    """Eigenvalues and eigenvectors (columns) of a symmetric 3x3 (Jacobi)."""
    a = [row[:] for row in c]
    v = [[1.0 if i == j else 0.0 for j in range(3)] for i in range(3)]
    for _ in range(50):
        p, q = max(((0, 1), (0, 2), (1, 2)), key=lambda ij: abs(a[ij[0]][ij[1]]))
        if abs(a[p][q]) < 1e-14:
            break
        theta = (a[q][q] - a[p][p]) / (2 * a[p][q])
        t = (1.0 if theta >= 0 else -1.0) / (abs(theta) + (theta * theta + 1) ** 0.5)
        cs = 1 / (t * t + 1) ** 0.5
        sn = t * cs
        for k in range(3):
            akp, akq = a[k][p], a[k][q]
            a[k][p], a[k][q] = cs * akp - sn * akq, sn * akp + cs * akq
        for k in range(3):
            apk, aqk = a[p][k], a[q][k]
            a[p][k], a[q][k] = cs * apk - sn * aqk, sn * apk + cs * aqk
        for k in range(3):
            vkp, vkq = v[k][p], v[k][q]
            v[k][p], v[k][q] = cs * vkp - sn * vkq, sn * vkp + cs * vkq
    return [a[i][i] for i in range(3)], [tuple(v[k][i] for k in range(3)) for i in range(3)]


def _frame(points, hint=None):
    """The screen's own frame from its vertices (model space): centre of its
    front, outward normal, up, width, height, and the depth grid. The plane is
    the best fit through the vertices, so a tilted screen (a laptop's lid)
    gets its own tilt; up is the model's up laid onto that plane."""
    n = len(points)
    mean = tuple(sum(p[i] for p in points) / n for i in range(3))
    cov = [[sum((p[i] - mean[i]) * (p[j] - mean[j]) for p in points) / n for j in range(3)] for i in range(3)]
    values, vectors = _eigen(cov)
    order = sorted(range(3), key=lambda i: values[i])
    normal = _unit(vectors[order[0]])
    if normal is None:
        return None
    # Outward: the way the mesh's own vertex normals point (when they agree with the plane); else the
    # model's front (+z), a screen lying flat facing up (+y). A screen facing the other way from the front
    # (a karaoke system's lyrics screen, thread "Show Discord stream on in-game TV") is drawn on its own side.
    hinted = _unit(hint) if hint else None
    if hinted is not None and abs(_dot(hinted, normal)) > 0.7:
        if _dot(hinted, normal) < 0:
            normal = tuple(-v for v in normal)
    elif abs(normal[2]) >= max(0.2, abs(normal[1])):
        if normal[2] < 0:
            normal = tuple(-v for v in normal)
    elif normal[1] < 0:  # lying flat, or tilted back more than up: faces up (a karaoke lyrics screen was drawn facing down)
        normal = tuple(-v for v in normal)
    up = _unit(tuple((0.0, 1.0, 0.0)[i] - normal[i] * normal[1] for i in range(3)))
    if up is None:
        up = _unit(vectors[order[2]])
    right = _cross(up, normal)
    local = [(_dot(p, right), _dot(p, up), _dot(p, normal)) for p in points]
    a0, a1 = min(q[0] for q in local), max(q[0] for q in local)
    b0, b1 = min(q[1] for q in local), max(q[1] for q in local)
    front = max(q[2] for q in local)
    if not (a1 - a0 > 0.02 and b1 - b0 > 0.02):
        return None
    depths = _grid([(q[0], q[1], q[2] - front) for q in local], (a0, b0, a1, b1))
    ca, cb = (a0 + a1) / 2, (b0 + b1) / 2
    centre = tuple(right[i] * ca + up[i] * cb + normal[i] * front for i in range(3))
    return centre, normal, up, a1 - a0, b1 - b0, depths


_wanted = set()  # model instances to list in the log when no screen is found in them (computers and TVs)


def want_dump(obj):
    for instance in _instances(obj):
        _wanted.add(instance)


def _instances(obj):
    """The model resources the object can show, its current one first (geostate variants are separate models)."""
    found = []

    def add(key):
        instance = getattr(key, 'instance', None)
        if instance is not None and instance not in found:
            found.append(instance)

    try:
        add(getattr(obj, 'model', None))
    except Exception:
        pass
    try:
        definition = obj.definition
        models = getattr(definition, '_model', None)
        if isinstance(models, tuple):
            for key in models:
                add(key)
        else:
            add(definition.get_model(0))
    except Exception:
        pass
    return found[:6]


def _cached(obj):
    """The object's screens (model space, unscaled; each a _screens result), best
    first, or None. Most objects have one. Tries its current model, then its others."""
    pending = False
    for instance in _instances(obj):
        if instance in _cache:
            if _cache[instance]:
                return _cache[instance]
            continue
        result = None
        try:
            found = native.model_bytes(instance)
            if found is None or found[0] == 1:
                pending = True  # no reader, or still indexing: next time
                continue
            if found[0] != 0:
                _log('model {:#018x} of {}: not readable ({})'.format(instance, obj, found[0]))
            else:
                from sims4vr import objmenu
                data = objmenu._unpack(found[1], found[2], found[3])
                result = _read(data, instance, obj)
        except Exception as e:
            _log('reading the screen of {} failed: {!r}'.format(obj, e))
        _cache[instance] = result
        if result:
            return result
    return None


def count(obj):
    """How many separate screens the object's model has (0: no screen mesh found)."""
    frames = _cached(obj)
    return len(frames) if frames else 0


def surface(obj, index=0):
    """(GRID_X, GRID_Y, depths in world units, row by row from the bottom left)
    of the object's video screen number `index`, behind the screen() plane, or
    None (no screen mesh, or unreadable)."""
    frames = _cached(obj)
    if not frames or index >= len(frames):
        return None
    scale = getattr(obj, 'scale', 1.0) or 1.0
    return GRID_X, GRID_Y, [d * scale for d in frames[index][5]]


def screen(obj, index=0):
    """The exact area the game plays its screen video on (user 2026-10-14
    22:54Z: TVs, computers and laptops alike): centre of its front, outward
    normal, up, width, height in world units, or None. `index`: which screen
    of a model with several (names(): their labels)."""
    frames = _cached(obj)
    if not frames or index >= len(frames):
        return None
    frame = frames[index]
    from sims4vr import objmenu
    t = obj.transform
    p, o = t.translation, t.orientation
    q = (o.x, o.y, o.z, o.w)
    scale = getattr(obj, 'scale', 1.0) or 1.0
    centre, normal, up, width, height = frame[:5]
    r = objmenu._rotate(q, tuple(v * scale for v in centre))
    return (p.x + r[0], p.y + r[1], p.z + r[2]), objmenu._rotate(q, normal), objmenu._rotate(q, up), \
        width * scale, height * scale


# Models whose detected screens are corrected (user 2026-10-28 22:05Z, Music Production Station): the ones facing straight
# up are not screens (the drum pad), and the rest are named. The first detected (the large one) first, then the small ones left to right
# as seen from the front.
_MODEL_LABELS = {0xeebb0699dc95d072: ['Large', 'Small (Left)', 'Small (Right)']}
# Left = the lower x (user 2026-10-28 23:32Z: the first guess, the higher x, had the streamer's computer swapped).
_MODEL_NAMED = {}


def _tidy(instance, frames):
    labels = _MODEL_LABELS.get(instance)
    if not labels or not frames:
        return frames
    frames = [f for f in frames if f[1][1] <= 0.94]
    if len(frames) != len(labels):
        return frames  # not the shape known: left as detected
    big = frames[0]  # screen 1 as detected
    small = sorted((f for f in frames if f is not big), key=lambda f: f[0][0])
    _MODEL_NAMED[instance] = labels
    return [big] + small


def names(obj):
    """A pie label for each of the object's screens, in screen() order: Left / Middle / Right (as you see them
    standing in front of the object), Top / Bottom, or Large / Small when they differ a lot in size."""
    frames = _cached(obj) or []
    n = len(frames)
    if n < 2:
        return ['Screen'] * n
    for instance in _instances(obj):
        if _MODEL_NAMED.get(instance) and len(_MODEL_NAMED[instance]) == n:
            return list(_MODEL_NAMED[instance])
    areas = [f[3] * f[4] for f in frames]
    if max(areas) > 2.5 * min(areas):
        if n == 2:
            return ['Large screen' if a == max(areas) else 'Small screen' for a in areas]
        return ['Screen {}{}'.format(i + 1, ' (large)' if a == max(areas) else '') for i, a in enumerate(areas)]
    xs = [f[0][0] for f in frames]
    ys = [f[0][1] for f in frames]
    if max(ys) - min(ys) > max(xs) - min(xs):  # stacked: top first
        words = ['Top', 'Middle', 'Bottom'] if n == 3 else ['Top', 'Bottom'] if n == 2 else None
    else:
        # frames run from the highest x down; the lower x is the left one
        words = ['Right', 'Middle', 'Left'] if n == 3 else ['Right', 'Left'] if n == 2 else None
    if words is None:
        return ['Screen {}'.format(i + 1) for i in range(n)]
    return ['{} screen'.format(w) for w in words]


def _bounds(points):
    return [min(p[i] for p in points) for i in range(3)] + [max(p[i] for p in points) for i in range(3)]


def _touching(a, b, gap=0.005):
    return all(a[i] - gap <= b[3 + i] and b[i] - gap <= a[3 + i] for i in range(3))


def _screens(islands):
    """Groups of islands that are one screen (touching, facing the same way), each as one _frame; the largest
    first when they differ much in size, else left to right as seen from the front (model +x is the viewer's left)."""
    groups = []
    for points, hint in islands:
        frame = _frame(points, hint) if len(points) >= 3 else None
        if frame is None:
            continue
        groups.append({'points': list(points), 'box': _bounds(points), 'normal': frame[1], 'hint': hint})
    merged = True
    while merged:
        merged = False
        for i in range(len(groups)):
            for j in range(i + 1, len(groups)):
                a, b = groups[i], groups[j]
                if _touching(a['box'], b['box']) and _dot(a['normal'], b['normal']) > 0.7:
                    points = a['points'] + b['points']
                    hint = tuple(a['hint'][k] + b['hint'][k] for k in range(3))
                    frame = _frame(points, hint)
                    groups[i] = {'points': points, 'box': _bounds(points), 'normal': frame[1] if frame else a['normal'],
                                 'hint': hint}
                    del groups[j]
                    merged = True
                    break
            if merged:
                break
    frames = [f for f in (_frame(g['points'], g['hint']) for g in groups) if f is not None]
    if len(frames) > 2:
        # Scraps (a screen made of many loose pieces, user 2026-10-29: "Comfy gaming computer" offered ~100): only the
        # real-sized ones stay, at most 6, the biggest.
        areas = [f[3] * f[4] for f in frames]
        floor = max(0.01, 0.08 * max(areas))
        kept = sorted((f for f in frames if f[3] * f[4] >= floor), key=lambda f: -f[3] * f[4])[:6]
        if len(kept) != len(frames):
            _log('{} screen piece(s) too small to be a screen left out ({} kept)'.format(len(frames) - len(kept), len(kept)))
        frames = kept or frames[:1]
    if len(frames) > 1:
        areas = [f[3] * f[4] for f in frames]
        if max(areas) > 2.5 * min(areas):
            frames.sort(key=lambda f: -f[3] * f[4])
        else:
            frames.sort(key=lambda f: (-round(f[0][0], 2), -f[0][1]))
    return frames


_dumped = set()
# Screens that aren't VideoSurface meshes: object name word -> mesh numbers of the first LOD (as the log's model lists
# them). MusicProductionStation: its scrolling-light screens (mesh 4, 104 vertices; PC dump 2026-10-28).
EXTRA_SCREENS = {'musicproductionstation': (4,)}
DUMP_WORDS = ('sinerotui', 'dj', 'karaoke', 'mixing', 'lyric', 'stage', 'videostation', 'video_station', 'moreviews')  # [screens] dump = words: models of objects named so are listed


def _dump(data, instance, obj, public, chunks, failed=False):
    """Every mesh of the model with its material's shader and size, once per model (logged, 60 lines): to find
    out what a screen is made of when it isn't a VideoSurface mesh (DJ station, karaoke system)."""
    from sims4vr import objmenu
    words = native.setting('screens', 'dump', None)
    words = DUMP_WORDS if words is None else tuple(w.strip().lower() for w in str(words).split(',') if w.strip())
    text = '{} {}'.format(obj, getattr(obj, 'definition', '')).lower()
    if instance in _dumped or not (any(w in text for w in words) or (failed and instance in _wanted)):
        return
    _dumped.add(instance)
    lines = []
    for tag, pos, size in chunks:
        if tag != b'MLOD':
            continue
        count = struct.unpack_from('<I', data, pos + 8)[0]
        at, end = pos + 12, pos + size
        for number in range(min(count, 256)):
            if at + 76 > end:
                break
            length = struct.unpack_from('<I', data, at)[0]
            entry = at + 4
            material = struct.unpack_from('<I', data, entry + 4)[0]
            found = objmenu._chunk(material, public, chunks, (b'MATD', b'MTST'))
            if found is None:
                shaders = 'no material'
            elif found[0] == b'MATD':
                shaders = '{:#010x}'.format(struct.unpack_from('<I', data, found[1] + 12)[0])
            else:
                shaders = 'set:' + ','.join(
                    '{:#010x}'.format(struct.unpack_from('<I', data, r[1] + 12)[0])
                    for r in (objmenu._chunk(struct.unpack_from('<I', data, a)[0], public, chunks, (b'MATD',))
                              for a in range(found[1] + 8, found[1] + found[2] - 3, 4)) if r is not None)
            verts, prims = struct.unpack_from('<2i', data, entry + 40)
            box = struct.unpack_from('<6f', data, entry + 48)
            lines.append('  mesh {}: shader {} {} verts {} tris, box x {:.2f}..{:.2f} y {:.2f}..{:.2f} z {:.2f}..{:.2f}'.format(
                number, shaders, verts, prims, box[0], box[3], box[1], box[4], box[2], box[5]))
            at = entry + length
        break  # the first (best) LOD
    _log('model {:#018x} of {}: {} meshes'.format(instance, obj, len(lines)))
    for line in lines[:60]:
        _log(line)


def _extra_numbers(obj):
    """Mesh numbers that count as screens in this object's model without a VideoSurface material
    (EXTRA_SCREENS, and [screens] extra = word:mesh,word:mesh)."""
    table = dict(EXTRA_SCREENS)
    try:
        for item in str(native.setting('screens', 'extra', '') or '').split(','):
            word, _sep, number = item.partition(':')
            if word.strip() and number.strip().isdigit():
                table[word.strip().lower()] = table.get(word.strip().lower(), ()) + (int(number),)
    except Exception:
        pass
    text = '{} {}'.format(obj, getattr(obj, 'definition', '')).lower()
    out = set()
    for word, numbers in table.items():
        if word in text:
            out.update(numbers)
    return out


def _read(data, instance, obj):
    from sims4vr import objmenu
    public, chunks = objmenu._rcol_chunks(data)
    result = _tidy(instance, _read_screens(data, instance, obj, public, chunks))
    try:
        _dump(data, instance, obj, public, chunks, failed=not result)
    except Exception as e:
        _log('model dump failed: {!r}'.format(e))
    return result


def _read_screens(data, instance, obj, public, chunks):
    from sims4vr import objmenu
    video = set()
    for tag, pos, size in chunks:
        if tag == b'MATD' and size >= 16 and struct.unpack_from('<I', data, pos + 12)[0] in objmenu._VIDEO_SHADERS:
            video.add(pos)

    def is_video(material):
        found = objmenu._chunk(material, public, chunks, (b'MATD', b'MTST'))
        if found is None:
            return False
        if found[0] == b'MATD':
            return found[1] in video
        for at in range(found[1] + 8, found[1] + found[2] - 3, 4):
            ref = objmenu._chunk(struct.unpack_from('<I', data, at)[0], public, chunks, (b'MATD',))
            if ref is not None and ref[1] in video:
                return True
        return False

    extra = _extra_numbers(obj)
    for tag, pos, size in chunks:
        if tag != b'MLOD':
            continue
        count = struct.unpack_from('<I', data, pos + 8)[0]
        at, end = pos + 12, pos + size
        islands, meshes = [], 0
        for number in range(min(count, 256)):
            if at + 76 > end:
                break
            length = struct.unpack_from('<I', data, at)[0]
            entry = at + 4
            if is_video(struct.unpack_from('<I', data, entry + 4)[0]) or number in extra:
                meshes += 1
                bounds = struct.unpack_from('<6f', data, entry + 48)
                found, why = _mesh_points(data, public, chunks, entry, bounds)
                if found is None:
                    _log('model {:#018x} of {}: screen mesh not read: {}'.format(instance, obj, why))
                    return None
                islands += found
            at = entry + length
        if meshes:
            frames = _screens(islands)
            if not frames:
                _log('model {:#018x} of {}: {} screen vertices make no screen'.format(
                    instance, obj, sum(len(i[0]) for i in islands)))
                return None
            for number, frame in enumerate(frames):
                centre, normal, up, width, height, depths = frame
                _log('model {:#018x} of {}: screen {} of {} from {} mesh(es), {} island(s): {:.3f} x {:.3f} at {}, '
                     'facing {}, {:.3f} deep at most'.format(
                         instance, obj, number + 1, len(frames), meshes, len(islands), width, height,
                         tuple(round(v, 2) for v in centre), tuple(round(v, 2) for v in normal), -min(depths)))
            return frames
    return None
