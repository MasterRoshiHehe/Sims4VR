"""The shapes you collide with in first person (docs/COLLISION-PLAN.md).

Walls come from the game's wall outlines of your level (closed outlines,
both faces), with doorways and arches cut out where the door's thin
footprint strip sits (doors locked for you stay shut); each wall side goes
up to the floor of the level above at that spot. Objects are their
footprints (only enabled ones) with the height range their model really
covers: object y plus the model box (wall shelves and cabinets hang below or
above their anchor), else the footprint's height. Heights are read live
(shelves can be raised, ceilings changed).

An object stops your body only when the game's navmesh is cut there too
(you fit where a Sim fits); one starting above your hips (cabinets, shelves)
counts for your head and hands.

Phase 2: the DLL pushes your body out of it every frame ([body] collision);
it is still compared with walk.py's ray check in the log. Switch: [body] collision_scene
(debug menu: Controls > Moving), read every 2 s.
"""
import math
import struct
import time

import build_buy
import placement
import routing
import services
import sims4.math
import terrain

from indexed_manager import CallbackTypes
from objects.doors.door import Door

from sims4vr import doors as doors_mod, native

RADIUS = 8.0  # metres around you that walls and objects are collected
REBUILD_MOVE = 3.0  # metres from the last build centre before it is built again
REBUILD_GAP = 1.0  # seconds at least between builds for objects moved near you
SCAN = 150  # objects of the whole lot looked at per tick while building
BATCH = 15  # nearby objects measured per tick while building
SWITCH_EVERY = 2.0  # seconds between reads of the debug-menu switch ([body] collision_scene)
MIN_HEIGHT = 0.05  # metres: lower things (rugs, seed packs) are walked over
HIP = 0.9  # metres above the floor: objects starting lower can stop your body
DOOR_THICK = 0.05  # metres: half the thickness of a closed door's solid panel
DOOR_HEIGHT = 2.0  # metres
GAP_DEPTH = 0.3  # metres either side of a door's strip cut out of the wall
DEFAULT_STOREY = 3.0  # metres when the level above has no height
COMPARE_EVERY = 1.0  # seconds between comparisons with the ray check
PROBE = 0.4 + 0.123  # metres: walk.PROBE_DISTANCE plus the navmesh's agent radius (it keeps Sims that far off)
RAYS = 24

walls = []    # (ax, az, bx, bz, y0, y1)
hand_shapes = []  # (polygon, y0, y1, flags): what your hands meet of an object (its visible shape or meshes' boxes;
                  # flags None: hands only, else as the footprint's: its body too)
_hand_skip = set()  # id() of the footprint polygons hand_shapes stand in for
_meshes = {}  # definition id: [(min xyz, max xyz)] of its model's meshes, () for a stand-in model, or None
_shapes = {}  # definition id: [(corners, y0, y1)] of its model's visible shape (model space), () stand-in, None
_placed = {}  # object id: (where it stands, its visible shape there): turned and moved once while it stays
_body_skip = set()  # id() of the footprint polygons whose body part the visible shape took over
_model_users = {}  # model instance: {tuning name: definition id} of the objects using it
objects = []  # (polygon [(x, z), ...], y0, y1, blocks the navmesh, name, material, object id)

# What your hand hitting an object sounds like (foley.py, foleyaudio.cpp; touch-sounds thread): a
# guess from the object's tuning name. 0 hard (wood, stone, plastic), 1 metal, 2 soft, 3 glass.
HARD, METAL, SOFT, GLASS = range(4)
_MATERIAL_WORDS = (
    (SOFT, ('sofa', 'couch', 'loveseat', 'bed', 'armchair', 'chair_living', 'chairliving', 'livingchair', 'beanbag',
            'ottoman', 'pillow', 'cushion', 'mattress', 'hammock', 'futon', 'plush', 'teddy', 'pouf', 'blanket')),
    (METAL, ('fridge', 'refrigerator', 'stove', 'oven', 'microwave', 'dishwasher', 'locker', 'trash', 'garbage',
             'washer', 'dryer', 'grill', 'bbq', 'metal', 'mailbox', 'vending', 'freezer', 'radiator', 'safe_')),
    (GLASS, ('glass', 'aquarium', 'fishtank', 'fish_tank', 'terrarium', 'mirror', 'window')),
)
_state = None  # (centre x, z, level, planner build id)
_pending = None  # generator of a build in progress
_door_panels = {}  # door id -> the solid shape of its closed panel
_dirty = 0.0  # when an object near the scene was added, removed or moved (0: not since the last build)
_built_at = 0.0
_watching = False
_compared = 0.0
_logs = 0
_disagree = [0, 0]  # comparisons that disagreed, all comparisons (since the last log)
_switch = [True, -1e9]  # [body] collision_scene, when last read


def _log(message):
    native._log('collide: ' + message)


def _object_changed(obj):
    """Object manager callback (add, remove, location changed)."""
    global _dirty
    s = _state
    if s is None or _dirty:
        return
    try:
        if obj.is_sim or getattr(obj, 'footprint', None) is None or obj.parent is not None:
            return  # Sims, carried things and footprint-less objects don't change the scene
        p = obj.position
        if (p.x - s[0]) ** 2 + (p.z - s[1]) ** 2 < (RADIUS + 1) ** 2:
            _dirty = time.monotonic()
    except Exception:
        pass


def _watch(on):
    global _watching
    if on == _watching:
        return
    manager = services.object_manager()
    kinds = (CallbackTypes.ON_OBJECT_ADD, CallbackTypes.ON_OBJECT_REMOVE, CallbackTypes.ON_OBJECT_LOCATION_CHANGED)
    for kind in kinds:
        try:
            if on:
                manager.register_callback(kind, _object_changed)
            elif manager.is_callback_registered(kind, _object_changed):
                manager.unregister_callback(kind, _object_changed)
        except Exception as e:
            _log('object callbacks: {!r}'.format(e))
    _watching = on


def _send():
    """The scene to the DLL (collide.cpp; layout in collide.h): it pushes your body out of it every frame."""
    fn = native._optional('collision_scene')  # absent in a DLL older than the script
    if fn is None:
        return
    data = [float(len(walls))]
    for w in walls:
        data.extend(w)
    shapes = [o for o in objects if 3 <= len(o[0]) < 256]
    # Your hands meet an object at its model's box where there is one (collide.cpp
    # stopHand: flag 2, hands only); its footprint then gets flag 4 (not for hands).
    with_box = _hand_skip
    data.append(float(len(shapes) + len(hand_shapes)))
    for o in shapes:
        pts, y0, y1, blocks = o[:4]
        material = o[5] if len(o) > 5 else 0  # touch sounds (foley.py)
        flags = (1 if blocks and id(pts) not in _body_skip else 0) | material << 8 | (4 if id(pts) in with_box else 0)
        data.extend((float(len(pts)), y0, y1, float(flags)))
        for x, z in pts:
            data.extend((x, z))
    for pts, y0, y1, f in hand_shapes:
        data.extend((float(len(pts)), y0, y1, 2.0 if f is None else float(f)))
        for x, z in pts:
            data.extend((x, z))
    fn(struct.pack('<{}f'.format(len(data)), *data))


def forget():
    global _state, _pending, walls, objects, _dirty
    _state = None
    _pending = None
    _dirty = 0.0
    walls = []
    objects = []
    hand_shapes.clear()
    _hand_skip.clear()
    _body_skip.clear()
    _placed.clear()
    _door_panels.clear()
    try:
        _send()
    except Exception:
        pass
    try:
        _watch(False)
    except Exception:
        pass


def _floor(level, x, z):
    try:
        return terrain.get_lot_level_height(x, z, level, services.current_zone_id())
    except Exception:
        return None


def _top(level, x, z, floor):
    """Floor of the level above at (x, z): the walls' top there."""
    y = _floor(level + 1, x, z)
    if y is None or y <= floor + 0.5:
        return floor + DEFAULT_STOREY
    return y


def _near_segment(x, z, ax, az, bx, bz, reach):
    dx, dz = bx - ax, bz - az
    length = dx * dx + dz * dz
    t = 0.0 if length < 1e-9 else max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / length))
    ex, ez = ax + dx * t - x, az + dz * t - z
    return ex * ex + ez * ez < reach * reach


def _strip(door):
    """The door's footprint polygon with the least area (the strip in the
    wall), widened across the wall: (centre, along unit, half length, across
    unit, half depth)."""
    best = None
    try:
        polygons = door.footprint_polygon
    except Exception:
        return None
    if polygons is None:
        return None
    for polygon in polygons:
        pts = [(v.x, v.z) for v in polygon]
        if len(pts) < 3:
            continue
        area = 0.0
        for i in range(len(pts)):
            x0, z0 = pts[i]
            x1, z1 = pts[(i + 1) % len(pts)]
            area += x0 * z1 - x1 * z0
        area = abs(area) / 2
        if best is None or area < best[0]:
            best = (area, pts)
    if best is None:
        return None
    pts = best[1]
    longest = None
    for i in range(len(pts)):
        x0, z0 = pts[i]
        x1, z1 = pts[(i + 1) % len(pts)]
        d = math.hypot(x1 - x0, z1 - z0)
        if longest is None or d > longest[0]:
            longest = (d, (x1 - x0) / max(d, 1e-6), (z1 - z0) / max(d, 1e-6))
    cx = sum(p[0] for p in pts) / len(pts)
    cz = sum(p[1] for p in pts) / len(pts)
    _, ux, uz = longest
    half = max(abs((p[0] - cx) * ux + (p[1] - cz) * uz) for p in pts)
    return (cx, cz), (ux, uz), half, (-uz, ux), GAP_DEPTH


def _cut(segment, gap):
    """The parts of a wall segment outside a door's gap (an oriented box)."""
    ax, az, bx, bz = segment
    (cx, cz), (ux, uz), half, (nx, nz), depth = gap
    # Box coordinates of both ends; clip the line to the box (slab method).
    a = ((ax - cx) * ux + (az - cz) * uz, (ax - cx) * nx + (az - cz) * nz)
    b = ((bx - cx) * ux + (bz - cz) * uz, (bx - cx) * nx + (bz - cz) * nz)
    t0, t1 = 0.0, 1.0
    for axis, limit in ((0, half), (1, depth)):
        d = b[axis] - a[axis]
        if abs(d) < 1e-9:
            if abs(a[axis]) > limit:
                return [segment]
            continue
        s0, s1 = (-limit - a[axis]) / d, (limit - a[axis]) / d
        if s0 > s1:
            s0, s1 = s1, s0
        t0, t1 = max(t0, s0), min(t1, s1)
        if t0 >= t1:
            return [segment]
    parts = []
    if t0 > 1e-4:
        parts.append((ax, az, ax + (bx - ax) * t0, az + (bz - az) * t0))
    if t1 < 1 - 1e-4:
        parts.append((ax + (bx - ax) * t1, az + (bz - az) * t1, bx, bz))
    return parts


def _jamb_ends(jambs, gaps, piece, a, b, top):
    """Notes where a doorway's cut ended this wall piece: (gap, which end) ->
    [(across, x, z, top)]. The wall contours have no doorways, so cutting one
    leaves the wall open at its sides; _jambs closes them again."""
    for px, pz in ((piece[0], piece[1]), (piece[2], piece[3])):
        if math.hypot(px - a.x, pz - a.z) < 1e-3 or math.hypot(px - b.x, pz - b.z) < 1e-3:
            continue  # a corner of the wall, not a cut
        for i, ((cx, cz), (ux, uz), half, (nx, nz), depth) in enumerate(gaps):
            along = (px - cx) * ux + (pz - cz) * uz
            across = (px - cx) * nx + (pz - cz) * nz
            if abs(across) <= depth + 1e-3 and abs(abs(along) - half) < 0.01:
                jambs.setdefault((i, along > 0), []).append((across, px, pz, top))
                break


def _jambs(jambs, floor):
    """The sides of each doorway (its jambs): one wall side across the wall
    between the two faces cut at each end of the gap (user 2026-10-27: hands
    went into the wall between two arches from the arch side)."""
    sides = []
    for ends in jambs.values():
        ends.sort()
        for (_, ax, az, top0), (_, bx, bz, top1) in zip(ends[0::2], ends[1::2]):
            if math.hypot(bx - ax, bz - az) < 0.6:  # a wall's thickness, not two walls
                sides.append((ax, az, bx, bz, floor, min(top0, top1)))
    return sides


def _material(obj):
    name = (type(obj).__name__ + ' ' + str(obj)).lower()
    for material, words in _MATERIAL_WORDS:
        if any(w in name for w in words):
            return material
    return HARD


def _y_range(obj, footprint):
    """World height range the object covers, or None."""
    y = obj.position.y
    box = None
    try:
        from sims4vr import objmenu
        box, _ = objmenu._model_box(obj)
    except Exception:
        box = None
    if box is not None:
        scale = getattr(obj, 'scale', 1.0) or 1.0
        return y + box[1] * scale, y + box[4] * scale
    try:
        return y, y + placement.get_object_height(footprint)
    except Exception:
        return None


def _mesh_boxes(obj):
    """[(min xyz, max xyz)] of each mesh in the object's model, from its first
    detail level (MLOD; each mesh entry carries its bounds), flat ones (drop
    shadows) left out; None if unknown. The model's overall box (MODL) reaches
    far past the object (a counter's is a metre deep, a bookshelf's too: user
    2026-10-27, hands hovering short of objects)."""
    key = getattr(obj.definition, 'id', None)
    if key in _meshes:
        return _meshes[key]
    boxes = None
    try:
        from sims4vr import objmenu
        model = obj.definition.get_model(0)
        instance = getattr(model, 'instance', None)
        found = native.model_bytes(instance) if instance is not None else None
        if found is not None and found[0] == 1:
            return None  # the game's models are still being indexed: next time
        # One model under several unrelated objects (wild plants, spawners) is a
        # stand-in, not what you see: hands go through those. (Swatches share a
        # model too, but also their tuning.)
        users = _model_users.setdefault(instance, {})
        users[str(obj).split(':')[0]] = key
        if len(users) > 2:
            for k in users.values():
                _meshes[k] = ()
            return ()
        if found is not None and found[0] == 0:
            data = objmenu._unpack(found[1], found[2], found[3])
            _public, chunks = objmenu._rcol_chunks(data)
            for tag, pos, size in chunks:
                if tag != b'MLOD':
                    continue
                count = struct.unpack_from('<I', data, pos + 8)[0]
                at, end = pos + 12, pos + size
                got = []
                for _ in range(min(count, 64)):
                    if at + 76 > end:
                        break
                    length = struct.unpack_from('<I', data, at)[0]
                    b = struct.unpack_from('<6f', data, at + 4 + 48)
                    size3 = [b[3 + i] - b[i] for i in range(3)]
                    if all(math.isfinite(v) for v in b) and size3[1] >= 0.02 and all(0.0 <= v < 20.0 for v in size3):
                        got.append(b)
                    at += 4 + length
                if got:
                    boxes = got
                    break
    except Exception as e:
        _log('mesh boxes of {}: {!r}'.format(obj, e))
        boxes = None
    _meshes[key] = boxes
    if len(_meshes) <= 60:
        _log('hands meet {}: {}'.format(str(obj).split(':')[0], 'its model unknown (the footprint, cut to the model box)'
             if boxes is None else ', '.join('{:.2f} x {:.2f} x {:.2f} m'.format(b[3] - b[0], b[4] - b[1], b[5] - b[2])
                                             for b in boxes[:6]) + (' ...' if len(boxes) > 6 else '')))
    return boxes


def _model_shape(obj):
    """[(corners, y0, y1)] of the object's visible shape in its model's own space
    (the DLL's meshshape: each separate piece of the model sliced into 5 cm
    layers, each layer the piece's outline there; user 2026-10-27: a round
    parasol table and its pole met you as squares). () for a stand-in model,
    None if unknown (older DLL, model not in the game's packages)."""
    key = getattr(obj.definition, 'id', None)
    if key in _shapes:
        return _shapes[key]
    name = str(obj).split(':')[0]
    try:
        model = obj.definition.get_model(0)
        instance = getattr(model, 'instance', None)
        if instance is None:
            _shapes[key] = None
            return None
        users = _model_users.setdefault(instance, {})
        users[name] = key
        if len(users) > 2:  # a stand-in (see _mesh_boxes)
            for k in users.values():
                _shapes[k] = ()
            return ()
        found = native.model_shape(instance)
        if found is None:
            return None  # an older DLL: the meshes' boxes
        if found[0] == 1:
            return None  # the game's models are still being indexed: next time
        prisms = found[1] if found[0] == 0 and found[1] else None
        note = found[2]
    except Exception as e:
        prisms, note = None, repr(e)
    _shapes[key] = prisms
    if len(_shapes) <= 80:
        _log('{} meets you at {}'.format(name, 'its shape: ' + note if prisms else 'its footprint ({})'.format(note or 'no model')))
    return prisms


def _shape_of(obj, top):
    """The object's visible shape where it stands: [(polygon, y0, y1)] (world), cut
    at top; () for a stand-in, None if unknown."""
    prisms = _model_shape(obj)
    if not prisms:
        return prisms
    scale = getattr(obj, 'scale', 1.0) or 1.0
    p, q = obj.position, obj.orientation
    where = (p.x, p.y, p.z, q.x, q.y, q.z, q.w, scale, top)
    placed = _placed.get(obj.id)
    if placed is not None and placed[0] == where:
        return placed[1]

    def world(lx, lz):
        x, z = lx * scale, lz * scale
        tx, ty, tz = 2 * q.y * z, 2 * (q.z * x - q.x * z), -2 * q.y * x
        return (p.x + x + q.w * tx + (q.y * tz - q.z * ty), p.z + z + q.w * tz + (q.x * ty - q.y * tx))

    shapes = []
    for corners, y0, y1 in prisms:
        y0, y1 = p.y + y0 * scale, min(p.y + y1 * scale, top)
        if y1 - y0 > 0.0005:
            shapes.append(([world(x, z) for x, z in corners], y0, y1))
    if len(_placed) > 2000:
        _placed.clear()
    _placed[obj.id] = (where, shapes)
    return shapes


def _mesh_shapes(obj, top):
    """What your hands meet of an object: each mesh's box, turned, scaled and
    placed like the object, as (polygon, y0, y1, None); None if unknown, empty
    for a stand-in model (hands go through)."""
    boxes = _mesh_boxes(obj)
    if boxes is None or boxes == ():
        return boxes
    scale = getattr(obj, 'scale', 1.0) or 1.0
    p, q = obj.position, obj.orientation

    def world(lx, lz):
        x, z = lx * scale, lz * scale
        tx, ty, tz = 2 * q.y * z, 2 * (q.z * x - q.x * z), -2 * q.y * x
        return (p.x + x + q.w * tx + (q.y * tz - q.z * ty), p.z + z + q.w * tz + (q.x * ty - q.y * tx))

    shapes = []
    for b in boxes:
        y0, y1 = p.y + b[1] * scale, min(p.y + b[4] * scale, top)
        if y1 - y0 < 0.01:
            continue
        shapes.append(([world(b[0], b[2]), world(b[3], b[2]), world(b[3], b[5]), world(b[0], b[5])], y0, y1, None))
    return shapes


def _hand_polygon(obj, pts):
    """The footprint polygon cut down to the object's model box (its mesh's
    bounds, turned and scaled like the object): what your hand touches. A lamp's
    or bookshelf's footprint reaches well past it (user 2026-10-27: hands
    stopped 10 cm short). None without a model box."""
    try:
        from sims4vr import objmenu
        box, _ = objmenu._model_box(obj)
    except Exception:
        return None
    if box is None:
        return None
    scale = getattr(obj, 'scale', 1.0) or 1.0
    p, q = obj.position, obj.orientation

    def world(lx, lz):  # object space to the world: q * v * q^-1 about y and the rest
        x, y, z = lx * scale, 0.0, lz * scale
        tx = 2 * (q.y * z - q.z * y)
        ty = 2 * (q.z * x - q.x * z)
        tz = 2 * (q.x * y - q.y * x)
        return (p.x + x + q.w * tx + (q.y * tz - q.z * ty), p.z + z + q.w * tz + (q.x * ty - q.y * tx))

    rect = [world(box[0], box[2]), world(box[3], box[2]), world(box[3], box[5]), world(box[0], box[5])]
    # Clip the footprint to the box (Sutherland-Hodgman; the box is convex).
    area = sum(rect[i][0] * rect[(i + 1) % 4][1] - rect[(i + 1) % 4][0] * rect[i][1] for i in range(4))
    sign = 1.0 if area > 0 else -1.0
    out = list(pts)
    for i in range(4):
        if len(out) < 3:
            return None
        (ax, az), (bx, bz) = rect[i], rect[(i + 1) % 4]
        inside = lambda pt: sign * ((bx - ax) * (pt[1] - az) - (bz - az) * (pt[0] - ax)) >= 0
        src, out = out, []
        for j in range(len(src)):
            cur, nxt = src[j], src[(j + 1) % len(src)]
            if inside(cur):
                out.append(cur)
            if inside(cur) != inside(nxt):
                dx, dz = nxt[0] - cur[0], nxt[1] - cur[1]
                den = (bx - ax) * dz - (bz - az) * dx
                if abs(den) > 1e-12:
                    t = ((bz - az) * (cur[0] - ax) - (bx - ax) * (cur[1] - az)) / den
                    out.append((cur[0] + dx * t, cur[1] + dz * t))
    return out if 3 <= len(out) < 256 else None


def _blocks_navmesh(surface, pts, y):
    """True when the navmesh is cut at the middle of the footprint (Sims can't stand there)."""
    cx = sum(p[0] for p in pts) / len(pts)
    cz = sum(p[1] for p in pts) / len(pts)
    try:
        return not routing.test_point_placement_in_navmesh(surface, sims4.math.Vector3(cx, y, cz))
    except Exception:
        return True


def _build(sim, x, y, z, surface):
    """Generator: yields between batches; ends with the new scene in place."""
    global walls, objects, hand_shapes, _hand_skip, _body_skip
    spent = 0.0
    started = time.perf_counter()
    level = surface.secondary_id
    floor = _floor(level, x, z)
    if floor is None:
        return
    # 1. Nearby objects and doors, scanning the lot a slice per tick.
    everything = list(services.object_manager().values())
    near = []
    doors = []
    for i, obj in enumerate(everything):
        if i and i % SCAN == 0:
            spent += time.perf_counter() - started
            yield
            started = time.perf_counter()
        try:
            if obj.is_sim or getattr(obj, 'level', None) != level:
                continue
            p = obj.position
        except Exception:
            continue
        d = (p.x - x) ** 2 + (p.z - z) ** 2
        if isinstance(obj, Door):
            if d < (RADIUS + 2) ** 2:
                doors.append(obj)
        elif d < RADIUS * RADIUS:
            near.append(obj)
    # 2. Walls, with doorways cut out.
    gaps = []
    panels = {}
    for door in doors:
        try:
            if sim in door.get_disallowed_objects():
                continue  # locked for you: the wall stays shut
        except Exception:
            pass
        gap = _strip(door)
        if gap is not None:
            gaps.append(gap)
            if not doors_mod.is_open_passage(door):
                (cx, cz), (ux, uz), half, (nx, nz), _ = gap
                pts = [(cx + ux * sx * half + nx * sn * DOOR_THICK, cz + uz * sx * half + nz * sn * DOOR_THICK)
                       for sx, sn in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
                panels[door.id] = (pts, floor, floor + DOOR_HEIGHT, True, 'door', 0, door.id)
    new_walls = []
    tops = {}
    jambs = {}
    for contour in build_buy.get_wall_contours(x, z, surface, False):
        n = len(contour)
        for i in range(n):
            a, b = contour[i], contour[(i + 1) % n]
            if not _near_segment(x, z, a.x, a.z, b.x, b.z, RADIUS):
                continue
            pieces = [(a.x, a.z, b.x, b.z)]
            for gap in gaps:
                pieces = [q for s in pieces for q in _cut(s, gap)]
            for s in pieces:
                if math.hypot(s[2] - s[0], s[3] - s[1]) > 0.01:
                    cell = (round((s[0] + s[2]) / 2), round((s[1] + s[3]) / 2))
                    top = tops.get(cell)
                    if top is None:
                        top = tops[cell] = _top(level, (s[0] + s[2]) / 2, (s[1] + s[3]) / 2, floor)
                    new_walls.append(s + (floor, top))
                    _jamb_ends(jambs, gaps, s, a, b, top)
    new_walls.extend(_jambs(jambs, floor))
    spent += time.perf_counter() - started
    yield
    started = time.perf_counter()
    # 3. Objects with their height ranges.
    new_objects = []
    new_hands = []
    new_skip = set()
    new_body_skip = set()
    for i, obj in enumerate(near):
        if i and i % BATCH == 0:
            spent += time.perf_counter() - started
            yield
            started = time.perf_counter()
        try:
            if obj.parent is not None:
                continue  # carried or held
            footprint = getattr(obj, 'footprint', None)
            if footprint is None or not obj.footprint_component.footprints_enabled:
                continue
            polygons = obj.footprint_polygon
        except Exception:
            continue
        if polygons is None:
            continue
        span = _y_range(obj, footprint)
        if span is None:
            continue
        y0, y1 = span
        y1 = min(y1, _top(level, obj.position.x, obj.position.z, floor))
        if y1 - y0 < MIN_HEIGHT or y1 <= floor:
            continue
        name = str(obj)[:48]
        material = _material(obj)
        shape = _shape_of(obj, y1)
        meshes = None if shape else _mesh_shapes(obj, y1)
        first = len(new_objects)
        if time.perf_counter() - started > 0.004:
            spent += time.perf_counter() - started
            yield
            started = time.perf_counter()
        for polygon in polygons:
            pts = [(v.x, v.z) for v in polygon]
            if len(pts) >= 3:
                blocks = y0 < floor + HIP and _blocks_navmesh(surface, pts, floor)
                new_objects.append((pts, y0, y1, blocks, name, material, obj.id))
                if shape:  # its visible shape stands in for the footprint, for hands and body
                    new_skip.add(id(pts))
                    new_body_skip.add(id(pts))
                elif meshes is not None:
                    new_skip.add(id(pts))
                else:
                    hand = _hand_polygon(obj, pts)
                    if hand is not None:
                        new_hands.append((hand, y0, y1, None))
                        new_skip.add(id(pts))
        if shape:
            mine = new_objects[first:]
            flags = (1 if any(o[3] for o in mine) else 0) | next((o[5] for o in mine if len(o) > 5), 0) << 8
            new_hands.extend((pts, a, b, flags) for pts, a, b in shape)
        new_hands.extend(meshes or ())
    spent += time.perf_counter() - started
    walls, objects, hand_shapes, _hand_skip, _body_skip = new_walls, new_objects, new_hands, new_skip, new_body_skip
    _door_panels.clear()
    _door_panels.update(panels)
    _apply_doors()
    body = sum(1 for o in new_objects if o[3])
    high = sum(1 for o in new_objects if o[1] >= floor + HIP)
    _log('built at {:.2f} {:.2f} level {} (floor {:.2f}): {} wall sides, {} doorways cut, {} object shapes '
         '({} stop your body, {} above your hips, {} you walk through); {:.1f} ms of work over the ticks, '
         '{} objects on the lot'.format(
             x, z, level, floor, len(new_walls), len(gaps), len(new_objects), body, high,
             len(new_objects) - body - high, spent * 1000, len(everything)))


def _cross(px, pz, qx, qz, ax, az, bx, bz):
    def side(ox, oz, ux, uz, vx, vz):
        return (ux - ox) * (vz - oz) - (uz - oz) * (vx - ox)
    d1 = side(ax, az, bx, bz, px, pz)
    d2 = side(ax, az, bx, bz, qx, qz)
    d3 = side(px, pz, qx, qz, ax, az)
    d4 = side(px, pz, qx, qz, bx, bz)
    return (d1 > 0) != (d2 > 0) and (d3 > 0) != (d4 > 0)


def _ray_hit(x, z, angle):
    """What the PROBE-long ray from (x, z) hits (a wall or a body-stopping object), or None."""
    ex, ez = x + math.sin(angle) * PROBE, z + math.cos(angle) * PROBE
    for ax, az, bx, bz, _, _ in walls:
        if _cross(x, z, ex, ez, ax, az, bx, bz):
            return 'wall'
    for pts, _, _, blocks, name, _, _ in objects:
        if not blocks:
            continue
        n = len(pts)
        for i in range(n):
            if _cross(x, z, ex, ez, pts[i][0], pts[i][1], pts[(i + 1) % n][0], pts[(i + 1) % n][1]):
                return name
    return None


def _compare(x, z, mask):
    """Logs how the scene's 24 rays differ from walk.py's."""
    global _logs
    mine = 0
    hits = set()
    for i in range(RAYS):
        hit = _ray_hit(x, z, i * 2 * math.pi / RAYS)
        if hit is not None:
            mine |= 1 << i
            if not mask & (1 << i):
                hits.add(hit)
    _disagree[1] += 1
    if mine != mask:
        _disagree[0] += 1
        if _logs < 60:
            _logs += 1
            _log('at {:.2f} {:.2f}: {} directions blocked only by the ray check, {} only by the scene{}'.format(
                x, z, bin(mask & ~mine).count('1'), bin(mine & ~mask).count('1'),
                ' ({})'.format(', '.join(sorted(hits))) if hits else ''))
    if _disagree[1] >= 60:
        _log('last {} checks: {} disagreed with the ray check'.format(_disagree[1], _disagree[0]))
        _disagree[0] = _disagree[1] = 0


@native.timed('collide tick', 0.004)
def _apply_doors():
    """Closed doors (manual_doors on) are solid panels in the gaps cut for them; open ones are
    not. Re-sends the scene at once, no rebuild: the way through opens in the same tick."""
    global objects
    manual = doors_mod.manual()
    kept = [o for o in objects if o[6] not in _door_panels]
    if manual:
        kept.extend(p for door_id, p in _door_panels.items() if not doors_mod.is_open(door_id))
    objects = kept
    _send()


def mark_dirty():
    """A door opened or closed: the panels follow at once."""
    if _door_panels:
        try:
            _apply_doors()
        except Exception as e:
            _log('door panels: {!r}'.format(e))


def update(sim, x, y, z, surface, free, mask):
    """walk.py's tick: keeps the scene built around you; compares while you walk freely."""
    global _state, _pending, _compared, _dirty, _built_at
    now = time.monotonic()
    if now - _switch[1] > SWITCH_EVERY:
        _switch[1] = now
        on = native.setting_flag('body', 'collision_scene', True)
        if on != _switch[0]:
            _switch[0] = on
            _log('switched {} (debug menu: Moving)'.format('on' if on else 'off'))
            if not on:
                forget()
    if not _switch[0] or surface is None:
        return
    _watch(True)
    if _pending is not None:
        try:
            next(_pending)
        except StopIteration:
            _pending = None
            _built_at = time.monotonic()
        except Exception as e:
            _pending = None
            _log('build failed: {!r}'.format(e))
        return
    level = surface.secondary_id
    build_id = routing.planner_build_id()
    stale = (_state is None or _state[2] != level or _state[3] != build_id
             or math.hypot(x - _state[0], z - _state[1]) > REBUILD_MOVE
             or (_dirty and now - _built_at > REBUILD_GAP))
    if stale:
        _state = (x, z, level, build_id)
        _dirty = 0.0
        _pending = _build(sim, x, y, z, surface)
        return
    if free and now - _compared > COMPARE_EVERY:
        _compared = now
        _compare(x, z, mask)
