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

_cache = {}  # model instance -> (nx, ny, depths) or None
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
    if _logs <= 40:
        native._log('screenmesh: ' + message)


def _vrtf(data, pos, size):
    """(stride, position offset, position format) of a VRTF chunk, or None."""
    _tag, _version, stride, count, extended = struct.unpack_from('<4sIiiI', data, pos)
    if not (4 <= stride <= 256 and 1 <= count <= 32):
        return None
    for wide in ((True, False) if extended else (False, True)):
        at, found, ok = pos + 20, None, True
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
        if ok and found is not None:
            return stride, found[0], found[1]
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


def _inside(p, box, slack):
    return all(box[i] - slack <= p[i] <= box[3 + i] + slack for i in range(3))


def _mesh_points(data, public, chunks, entry, box):
    """Positions of one MLOD mesh, or None when they don't fit its bounds."""
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
    stride, offset, fmt = layout
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
        inside = sum(1 for p in points if _inside(p, box, slack))
        tried.append(inside)
        if inside >= 0.95 * count and inside >= 3:
            return [p for p in points if _inside(p, box, slack)], None
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


def _frame(points):
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
    # Outward: the model's front (+z); a screen lying flat faces up (+y).
    if abs(normal[2]) >= 0.2:
        if normal[2] < 0:
            normal = tuple(-v for v in normal)
    elif normal[1] < 0:
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


def _cached(obj):
    """The object's screen frame (model space, unscaled; _frame), or None."""
    try:
        instance = getattr(obj.definition.get_model(0), 'instance', None)
    except Exception:
        instance = None
    if instance is None:
        return None
    if instance in _cache:
        return _cache[instance]
    result = None
    try:
        found = native.model_bytes(instance)
        if found is None or found[0] == 1:
            return None  # no reader, or still indexing: next time
        if found[0] != 0:
            _log('model {:#018x} of {}: not readable ({})'.format(instance, obj, found[0]))
        else:
            from sims4vr import objmenu
            data = objmenu._unpack(found[1], found[2], found[3])
            result = _read(data, instance, obj)
    except Exception as e:
        _log('reading the screen of {} failed: {!r}'.format(obj, e))
    _cache[instance] = result
    return result


def surface(obj):
    """(GRID_X, GRID_Y, depths in world units, row by row from the bottom left)
    of the object's video screen, behind the screen() plane, or None (no
    screen mesh, or unreadable)."""
    frame = _cached(obj)
    if frame is None:
        return None
    scale = getattr(obj, 'scale', 1.0) or 1.0
    return GRID_X, GRID_Y, [d * scale for d in frame[5]]


def screen(obj):
    """The exact area the game plays its screen video on (user 2026-10-14
    22:54Z: TVs, computers and laptops alike): centre of its front, outward
    normal, up, width, height in world units, or None."""
    frame = _cached(obj)
    if frame is None:
        return None
    from sims4vr import objmenu
    t = obj.transform
    p, o = t.translation, t.orientation
    q = (o.x, o.y, o.z, o.w)
    scale = getattr(obj, 'scale', 1.0) or 1.0
    centre, normal, up, width, height = frame[:5]
    r = objmenu._rotate(q, tuple(v * scale for v in centre))
    return (p.x + r[0], p.y + r[1], p.z + r[2]), objmenu._rotate(q, normal), objmenu._rotate(q, up), \
        width * scale, height * scale


def _read(data, instance, obj):
    from sims4vr import objmenu
    public, chunks = objmenu._rcol_chunks(data)
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

    for tag, pos, size in chunks:
        if tag != b'MLOD':
            continue
        count = struct.unpack_from('<I', data, pos + 8)[0]
        at, end = pos + 12, pos + size
        points, meshes = [], 0
        for _ in range(min(count, 256)):
            if at + 76 > end:
                break
            length = struct.unpack_from('<I', data, at)[0]
            entry = at + 4
            if is_video(struct.unpack_from('<I', data, entry + 4)[0]):
                meshes += 1
                bounds = struct.unpack_from('<6f', data, entry + 48)
                found, why = _mesh_points(data, public, chunks, entry, bounds)
                if found is None:
                    _log('model {:#018x} of {}: screen mesh not read: {}'.format(instance, obj, why))
                    return None
                points += found
            at = entry + length
        if meshes:
            frame = _frame(points) if len(points) >= 3 else None
            if frame is None:
                _log('model {:#018x} of {}: {} screen vertices make no screen'.format(instance, obj, len(points)))
                return None
            centre, normal, up, width, height, depths = frame
            _log('model {:#018x} of {}: screen from {} vertices in {} mesh(es): {:.3f} x {:.3f}, facing {}, '
                 '{:.3f} deep at most'.format(instance, obj, len(points), meshes, width, height,
                                              tuple(round(v, 2) for v in normal), -min(depths)))
            return frame
    return None
