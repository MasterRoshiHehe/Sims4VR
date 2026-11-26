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
DROP = 1.5  # metres: walls and low walls reach this far below the floor, so a step down of a metre or more beside them
#   (the floor under you reads lower there) doesn't leave you below them (user 2026-11-03: walked out over a half wall)
HIP = 0.9  # metres above the floor: objects starting lower can stop your body
DOOR_THICK = 0.05  # metres: half the thickness of a closed door's solid panel
DOOR_HEIGHT = 2.0  # metres
MIN_GAP_HALF = 0.55  # metres: a doorway is at least this wide on each side of its centre
GAP_DEPTH = 0.3  # metres either side of a door's strip cut out of the wall
DEFAULT_STOREY = 3.0  # metres when the level above has no height
COMPARE_EVERY = 1.0  # seconds between comparisons with the ray check
PROBE = 0.4 + 0.123  # metres: walk.PROBE_DISTANCE plus the navmesh's agent radius (it keeps Sims that far off)
RAYS = 24

walls = []    # (ax, az, bx, bz, y0, y1[, fence])
hand_shapes = []  # (polygon, y0, y1, flags): what your hands meet of an object (its visible shape or meshes' boxes;
                  # flags None: hands only, else as the footprint's: its body too)
_hand_skip = set()  # id() of the footprint polygons hand_shapes stand in for
_meshes = {}  # definition id: [(min xyz, max xyz)] of its model's meshes, () for a stand-in model, or None
_shapes = {}  # definition id: [(corners, y0, y1)] of its model's visible shape (model space), () stand-in, None
_placed = {}  # object id: (where it stands, its visible shape there): turned and moved once while it stays
_body_skip = set()  # id() of the footprint polygons whose body part the visible shape took over
_step = set()  # id() of the footprint polygons of low objects you walk over (flag 64: collide.cpp stepHeight)
_step_check = [0.0]
_step_all = [False]  # [body] step_all: every low object, not only rocks
_step_max = [0.0]  # metres: objects up to this tall are stepped on (0: off); [body] step_over / step_height
_soft = set()  # id() of the footprint polygons of plants (flag 16: they slow you, collide.cpp softSlow)
_step_logged = set()
_soft_logs = [0, set()]  # bushes logged with their solid and leaf layers
_scenery = {'zone': None, 'lot': None, 'status': None, 'area': '', 'cells': {}, 'count': 0}  # the area's scenery
_scenery_shapes = {}  # model instance: (prisms, reach, height, kind) in model space, or None (no model)
_scenery_on = [True, -1e9]  # [body] world_scenery, when last read
_scenery_logs = [0, None]
SCENERY_CELL = 16.0  # metres: the area's scenery is kept in a grid of these
SCENERY_BIG = 40.0  # metres: wider models (backdrops, terrain pieces) are left out
_soft_kinds = {}  # definition id: 2 bushes, flowers, garden plants (soft); 1 other plants and trees (leaves soft); 0
_SOFT_TAGS = ('Build_Shrub', 'Build_Flower', 'Func_Bush', 'Func_OutdoorPlant')  # trees and potted plants stay solid
_LEAFY_TAGS = ('BuyCatLD_Plant', 'Build_Tree', 'Func_Tree', 'Func_Grass', 'Func_Gardening', 'BuyCatEE_Gardening')
_LEAFY_WORDS = ('plant', 'tree', 'fern', 'palm', 'flower', 'bush', 'shrub', 'hedge', 'grass', 'ivy', 'vine', 'cactus')
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
_occ_at = [0.0, 0.0, 0]  # x, z, level of the last build: where the floors are read for muffling
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
        data.extend(w[:6])
        data.append(w[6] if len(w) > 6 else 0.0)  # 1: a fence or railing, kept clear of by furniture_radius
    shapes = [o for o in objects if 3 <= len(o[0]) < 256]
    # Your hands meet an object at its model's box where there is one (collide.cpp
    # stopHand: flag 2, hands only); its footprint then gets flag 4 (not for hands).
    with_box = _hand_skip
    data.append(float(len(shapes) + len(hand_shapes)))
    for o in shapes:
        pts, y0, y1, blocks = o[:4]
        material = o[5] if len(o) > 5 else 0  # touch sounds (foley.py)
        flags = (1 if blocks and id(pts) not in _body_skip else 0) | material << 8 | (4 if id(pts) in with_box else 0)
        flags |= 16 | 32 if id(pts) in _soft else 0  # 32: this footprint slows you (collide.cpp plantSpeed)
        flags |= 64 if id(pts) in _step else 0  # 64: you walk over it (collide.cpp stepHeight)
        data.extend((float(len(pts)), y0, y1, float(flags)))
        for x, z in pts:
            data.extend((x, z))
    for pts, y0, y1, f in hand_shapes:
        data.extend((float(len(pts)), y0, y1, 2.0 if f is None else float(f)))
        for x, z in pts:
            data.extend((x, z))
    fn(struct.pack('<{}f'.format(len(data)), *data))
    _send_occlusion()


def _centre_line(pts):
    """The middle line of a thin footprint [(x, z), ...]: ((ax, az), (bx, bz)), along its longest edge."""
    n = len(pts)
    best = max(range(n), key=lambda i: math.hypot(pts[(i + 1) % n][0] - pts[i][0], pts[(i + 1) % n][1] - pts[i][1]))
    ux, uz = pts[(best + 1) % n][0] - pts[best][0], pts[(best + 1) % n][1] - pts[best][1]
    length = math.hypot(ux, uz) or 1.0
    ux, uz = ux / length, uz / length
    nx, nz = -uz, ux
    along = [x * ux + z * uz for x, z in pts]
    across = [x * nx + z * nz for x, z in pts]
    c = (min(across) + max(across)) / 2
    a0, a1 = min(along), max(along)
    return (a0 * ux + c * nx, a0 * uz + c * nz), (a1 * ux + c * nx, a1 * uz + c * nz)


def _send_occlusion():
    """What muffles sound, to the DLL (collide.cpp occlusion; it hands Extended Sound Control the amount for each
    sound from camera.cpp hookedPan). [audio] muffle_feed switches it; the amounts are the debug menu's Sound page:
    a wall side lets through 1 - wall (two faces to cross), a closed door, a half wall piece and a floor each block
    their share."""
    fn = native._optional('occlusion_scene')  # absent in a DLL older than the script
    if fn is None:
        return
    try:
        if not native.setting_flag('audio', 'muffle_feed', True):
            fn(struct.pack('<f', 0.0))
            return
        wall = min(max(float(native.setting('audio', 'muffle_wall', 0.5) or 0.0), 0.0), 0.95)
        door = min(max(float(native.setting('audio', 'muffle_door', 0.35) or 0.0), 0.0), 0.95)
        half = min(max(float(native.setting('audio', 'muffle_half_wall', 0.25) or 0.0), 0.0), 0.95)
        slab = min(max(float(native.setting('audio', 'muffle_floor', 0.6) or 0.0), 0.0), 0.95)
        face = 1.0 - math.sqrt(1.0 - wall)  # a wall is two sides: both together block `wall`
        data = []
        count = 0
        for w in walls:
            if len(w) > 6 and w[6]:
                continue  # a fence or railing: sound passes
            if len(w) > 7 and w[7]:
                continue  # a half wall piece: counted from its object below
            data.extend((w[0], w[1], w[2], w[3], w[4], w[5], face))
            count += 1
        for o in objects:
            if len(o) < 7 or len(o[0]) < 3:
                continue
            name = o[4]
            if name == 'door':
                if o[6] not in _door_panels or doors_mod.is_open(o[6]):
                    continue
                weight = door
            elif str(name).startswith('object_Half_Wall'):
                weight = half
            else:
                continue
            if weight <= 0:
                continue
            (ax, az), (bx, bz) = _centre_line(o[0])
            data.extend((ax, az, bx, bz, o[1], o[2], weight))
            count += 1
        x, z, level = _occ_at
        floors = []
        for lv in range(level - 2, level + 4):
            h = _floor(lv, x, z)
            if h is not None and not any(abs(h - f) < 0.3 for f in floors):
                floors.append(h)
        out = [float(count)] + data + [float(len(floors) if slab > 0 else 0)]
        if slab > 0:
            for h in floors:
                out.extend((h, slab))
        fn(struct.pack('<{}f'.format(len(out)), *out))
    except Exception as e:
        _log('muffle scene: {!r}'.format(e))


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
    _soft.clear()
    _placed.clear()
    _door_panels.clear()
    fn = native._optional('occlusion_scene')
    if fn is not None:
        fn(struct.pack('<f', 0.0))  # no scene: no muffling
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
    half = max(half, MIN_GAP_HALF)  # user 2026-10-28: a narrow door was too slim to walk through
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


def _plant_kind(obj):
    """2: a bush, flower or garden plant (the game's own build/buy tags): you walk
    through it, slowed (user 2026-10-28: no collision with bushes, slow you
    down instead). 1: another plant or a tree (tags, else its tuning name): its
    leaves (alpha cut-out meshes) are soft, its pot or trunk solid (user
    2026-10-28: a big fern's fronds held you in mid-air). 0: neither."""
    key = getattr(obj.definition, 'id', None)
    kind = _soft_kinds.get(key)
    if kind is None:
        kind = 0
        name = str(obj).split(':')[0]
        why = ''
        try:
            import tag
            tags = set(int(t) for t in obj.get_tags())
            for k, names in ((2, _SOFT_TAGS), (1, _LEAFY_TAGS)):
                hit = [n for n in names if getattr(tag.Tag, n, None) is not None and int(getattr(tag.Tag, n)) in tags]
                if hit:
                    kind, why = k, hit[0]
                    break
        except Exception as e:
            _log('plant check of {}: {!r}'.format(obj, e))
        if not kind:
            word = next((w for w in _LEAFY_WORDS if w in name.lower()), None)
            if word:
                kind, why = 1, 'name: ' + word
        _soft_kinds[key] = kind
        if kind and sum(1 for v in _soft_kinds.values() if v) <= 40:
            _log('{} ({}) {}'.format(name, why, 'is a plant: it slows you instead of stopping you' if kind == 2
                                      else 'has leaves: they let you through, the rest stops you'))
    return kind


def _step_height():
    """[body] step_over / step_height: how tall an object can be for you to walk over it (0: off)."""
    if not native.setting_flag('body', 'step_over', True):
        return 0.0
    try:
        return max(0.0, min(1.0, float(native.setting('body', 'step_height', '0.5'))))
    except ValueError:
        return 0.5


_ROCK_WORDS = ('rock', 'stone', 'boulder', 'pebble', 'cobble')  # + [body] step_words (comma list, sims4vr.ini)
_NOT_ROCK = ('planter', 'fence', 'wall', 'edge', 'border', 'bush', 'hedge', 'raised', 'curb', 'railing', 'brick')
_rocks = {}  # definition id: True for rock-like decor (tags or tuning name)


def _is_rock(obj):
    """Rock-like decor: a game tag or the tuning name has rock/stone/boulder in it (user 2026-10-28: planter edges and
    low garden walls stay solid, only the rocks are stepped over); anything with a planter/edge/border/fence/wall word,
    and plants, never."""
    key = getattr(obj.definition, 'id', None)
    hit = _rocks.get(key)
    if hit is None:
        hit = False
        name = str(obj).split(':')[0].lower()
        extra = tuple(w.strip().lower() for w in (native.setting('body', 'step_words', '') or '').split(',') if w.strip())
        why = ''
        try:
            import tag
            names = [n for n in dir(tag.Tag) if not n.startswith('_')]
            have = set(int(t) for t in obj.get_tags())
            tags = [n for n in names if any(w in n.lower() for w in _ROCK_WORDS) and not any(w in n.lower() for w in _NOT_ROCK)
                    and isinstance(getattr(tag.Tag, n, None), int) and int(getattr(tag.Tag, n)) in have]
            if tags:
                why = 'tag ' + tags[0]
        except Exception as e:
            _log('rock check of {}: {!r}'.format(obj, e))
        if not why:
            word = next((w for w in _ROCK_WORDS + extra if w in name), None)
            if word:
                why = 'name: ' + word
        hit = bool(why) and not any(w in name for w in _NOT_ROCK) and _plant_kind(obj) == 0
        _rocks[key] = hit
        if hit and len(_rocks) < 80:
            _log('{} is rock-like decor ({})'.format(name, why))
    return hit


_WALL_WORDS = ('half_wall', 'halfwall', 'low_wall', 'lowwall', 'railing', 'parapet', 'balustrade', 'ledge')  # + [body] wall_words
_WALL_MORE = ('fence', 'barrier', 'divider', 'planter', 'banister', 'guardrail', 'handrail', 'balcony', 'terrace', 'wall')  # looser hints, only with a low thin shape
_WALL_NOT = ('painting', 'prototype', 'wallobject', 'retail', 'frame', 'picture', 'screen', 'light', 'art', 'deco', 'shelf', 'clock', 'mirror', 'hang', 'poster', 'tv', 'sconce', 'sofa', 'bench', 'table', 'bed',
             'desk', 'counter', 'cabinet', 'dresser', 'chair', 'plant', 'bookcase', 'fireplace', 'window', 'door', 'column', 'pillar')
_WALL_HEIGHT = 0.5  # metres: a low wall whose model/footprint gives no height ("Tiny half wall 0.5")
_wall_logs = [0, set()]
_tall_logs = [0, set()]
_low_walls = {}  # (definition id, wall_words text): _is_low_wall answer


_HALF_WALL_ID = 0x17B94  # object_Half_Wall (halfWallTrimSlots_GEN), read from the user's save (field 30 of 290 of them)
_HALF_WALL_THICK = 0.1  # metres either side of the centre line
_HALF_WALL_HEIGHT = 1.0  # metres: the drawn wall; the helper object itself claims 2 m


class _P(object):
    __slots__ = ('x', 'y', 'z')

    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z


def _half_wall_polygons(obj):
    """The half walls the build tools draw are a wall type the game's wall list (get_wall_contours) leaves out; each
    1 m piece only leaves an invisible helper object (class HalfWall) at its centre, with a 1 cm footprint, facing across
    the wall (the game stands Sims 0.25 m either side of it along forward). The save holds 290 of them on a 1 m grid, a
    piece 1 m long (1.41 m on a diagonal). Build the piece from that: [(x, z) polygon] as vector-like points, or None."""
    try:
        if obj.definition.id != _HALF_WALL_ID and type(obj).__name__ != 'HalfWall':
            return None
        f = obj.forward
        n = math.hypot(f.x, f.z)
        if n < 1e-3:
            return None
        fx, fz = f.x / n, f.z / n
    except Exception:
        return None
    half = 0.5 * (1.4143 if min(abs(fx), abs(fz)) > 0.3 else 1.0)
    ux, uz = -fz, fx  # along the wall
    p = obj.position
    out = []
    for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        out.append(_P(p.x + ux * half * a + fx * _HALF_WALL_THICK * b, p.y, p.z + uz * half * a + fz * _HALF_WALL_THICK * b))
    return [out]


def _is_low_wall(obj):
    """A low wall or railing object (user 2026-10-29: "tiny half wall 0.5" had no collision): its tuning name has a
    half wall / railing word. Walls drawn by the build tools are not objects and stay with the wall code."""
    key = getattr(getattr(obj, 'definition', None), 'id', None)
    raw = native.setting('body', 'wall_words', '') or ''
    hit = _low_walls.get((key, raw)) if key is not None else None
    if hit is None:
        name = str(obj).split(':')[0].lower()
        extra = tuple(w.strip().lower() for w in raw.split(',') if w.strip())
        hit = any(w in name for w in _WALL_WORDS + extra)
        if key is not None:
            _low_walls[(key, raw)] = hit  # per definition: the tuning name never changes
    return hit


def _looks_low_wall(obj, polygons, height):
    """A low, thin, long object with a wall-ish name that the strict word list missed (user 2026-11-04: two half walls
    in the 880 marks had no collision and no "low wall" log line)."""
    if not 0.25 <= height <= 1.35:
        return False
    name = str(obj).split(':')[0].lower()
    model = str(getattr(getattr(obj, 'definition', None), 'name', '') or '').lower()  # the catalog name "Half Wall" is a string table entry, not this
    both = name + ' ' + model
    if any(w in both for w in _WALL_NOT):
        return False
    try:
        xs = [v.x for poly in polygons for v in poly]
        zs = [v.z for poly in polygons for v in poly]
        dx, dz = max(xs) - min(xs), max(zs) - min(zs)
    except Exception:
        return False
    thin, long_ = min(dx, dz), max(dx, dz)
    named = any(w in both for w in _WALL_MORE)
    hit = (thin <= 0.6 and long_ >= 0.9) if named else (thin <= 0.35 and long_ >= 1.0)  # no wall word: only a plain thin slab
    if hit and len(_tall_logs[1]) < 40 and obj.id not in _tall_logs[1]:
        _tall_logs[1].add(obj.id)
        _log('{} / {} taken for a low wall by shape ({:.2f} x {:.2f} m, {:.2f} m tall)'.format(name, model, thin, long_, height))
    return hit


def _wall_sides(polygon):
    """(centre, across unit, half thickness, along unit, half length) of a thin footprint, from its longest edge."""
    pts = [(v.x, v.z) for v in polygon]
    n = len(pts)
    best = max(range(n), key=lambda i: math.hypot(pts[(i + 1) % n][0] - pts[i][0], pts[(i + 1) % n][1] - pts[i][1]))
    a, b = pts[best], pts[(best + 1) % n]
    length = math.hypot(b[0] - a[0], b[1] - a[1]) or 1.0
    ux, uz = (b[0] - a[0]) / length, (b[1] - a[1]) / length
    nx, nz = -uz, ux
    along = [(x * ux + z * uz) for x, z in pts]
    across = [(x * nx + z * nz) for x, z in pts]
    ca, cc = (min(along) + max(along)) / 2, (min(across) + max(across)) / 2
    return ((ca * ux + cc * nx, ca * uz + cc * nz), (nx, nz), (max(across) - min(across)) / 2,
            (ux, uz), (max(along) - min(along)) / 2)


def _wall_crossable(obj, polygons, level, surface):
    """True when walkable floor lies on both sides of the low wall (about the same height and on the navmesh):
    you step over it. A drop or nothing on a side (it guards a ledge): False, it stays an invisible wall."""
    try:
        polygon = max(polygons, key=lambda poly: len(poly))
        (cx, cz), (nx, nz), half, (ux, uz), length = _wall_sides(polygon)
        ground = _floor(level, cx, cz)
        if ground is None:
            return False
        for side in (-1, 1):
            for t in (-0.5, 0.0, 0.5):
                d = half + 0.45
                x = cx + nx * d * side + ux * length * t
                z = cz + nz * d * side + uz * length * t
                f = _floor(level, x, z)
                if f is None or abs(f - ground) > 0.2:
                    return False
                if surface is not None and not routing.test_point_placement_in_navmesh(surface, sims4.math.Vector3(x, f, z)):
                    return False
        return True
    except Exception as e:
        _log('{}: wall sides check failed {!r}'.format(str(obj).split(':')[0], e))
        return False


def _steppable(obj, level, y0, y1, shape, wall=False):
    """A low rock-like thing lying on the ground (a garden rock; every low object with step_all): its top (the visible shape's where
    known) no higher than step_height over the ground there, standing on it (not an overhang). Seats and beds
    (objects with parts) stay solid."""
    try:
        if getattr(obj, 'parts', None) and not wall:
            return False
        if not wall and not _step_all[0] and not _is_rock(obj):
            return False
        ground = _floor(level, obj.position.x, obj.position.z)
        if ground is None:
            return False
        bottom, top = (min(a for _, a, _, _ in shape), max(b for _, _, b, _ in shape)) if shape else (y0, y1)
        if bottom > ground + 0.15 or top - ground > _step_max[0] + (0.1 if wall else 0.0):
            return False
        if len(_step_logged) < 40 and obj.definition.id not in _step_logged:
            _step_logged.add(obj.definition.id)
            _log('{}: {:.2f} m tall: you step over it'.format(str(obj).split(':')[0], top - ground))
        return True
    except Exception:
        return False


def _wall_note(obj, what):
    key = (obj.definition.id, what)
    if _wall_logs[0] < 40 and key not in _wall_logs[1]:
        _wall_logs[0] += 1
        _wall_logs[1].add(key)
        _log('{} (low wall): {}'.format(str(obj).split(':')[0], what))


def _is_soft(obj):
    return _plant_kind(obj) == 2


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
    for corners, y0, y1, leaves in prisms:
        y0, y1 = p.y + y0 * scale, min(p.y + y1 * scale, top)
        if y1 - y0 > 0.0005:
            shapes.append(([world(x, z) for x, z in corners], y0, y1, leaves))
    if len(_placed) > 2000:
        _placed.clear()
    _placed[obj.id] = (where, shapes)
    return shapes


def _scenery_retry():
    """Build again shortly (the DLL is still reading the area or the models)."""
    global _dirty
    _dirty = time.monotonic()


def _scenery_load():
    """The area's placed scenery into _scenery['cells']: True once known (found or
    not), False while the DLL still reads the .world files."""
    s = _scenery
    zone = services.current_zone_id()
    if s['zone'] != zone:
        s.update(zone=zone, lot=None, status=None, area='', cells={}, count=0)
    if s['status'] not in (None, 1):
        return True
    if s['lot'] is None:
        try:
            z = services.current_zone()
            world = services.get_world_description_id(z.world_id)
            s['lot'] = int(services.get_lot_description_id(z.lot.lot_id, world) or 0)
        except Exception as e:
            _log('scenery: no lot description id: {!r}'.format(e))
            s['status'] = -1
            return True
    found = native.world_scenery(s['lot'])
    if found is None:
        s['status'] = -1  # an older DLL
        return True
    status, placed, area = found
    s['status'], s['area'] = status, area
    if status == 1:
        return False
    cells = {}
    for p in placed:
        cells.setdefault((int(p[1] // SCENERY_CELL), int(p[3] // SCENERY_CELL)), []).append(p)
    s['cells'], s['count'] = cells, len(placed)
    _log('scenery: lot {:016X}: {} ({} placed objects)'.format(
        s['lot'], {0: 'area ' + area, 2: 'no area has this lot', 3: 'reading ' + area + ' failed'}.get(status, status),
        len(placed)))
    return True


def _scenery_shape(model):
    """A scenery model's visible shape: (prisms, reach, height, kind) in model space
    (kind: 'plant' leaves soft, 'bush' all leaves and low: slows you, 'thin' alpha
    cut-out fence or lattice: solid, 'solid'); None without a model, False while the
    game's models are still being indexed."""
    if model in _scenery_shapes:
        return _scenery_shapes[model]
    shape, note = None, ''
    for key in (model, (model >> 32) | (model & 0xFFFFFFFF) << 32):  # the record's key, then its halves swapped
        found = native.model_shape(key)
        if found is None:
            return None
        if found[0] == 1:
            return False
        if found[0] == 0 and found[1]:
            shape, note = found[1], found[2] + ('' if key == model else ' (instance halves swapped)')
            break
        note = note or found[2]
    if shape:
        pts = [c for corners, _, _, _ in shape for c in corners]
        reach = max(math.hypot(px, pz) for px, pz in pts)
        height = max(p[2] for p in shape) - min(p[1] for p in shape)
        leaf = [c for corners, _, _, leaves in shape if leaves for c in corners]
        kind = 'solid'
        if leaf:
            wx = max(c[0] for c in leaf) - min(c[0] for c in leaf)
            wz = max(c[1] for c in leaf) - min(c[1] for c in leaf)
            if min(wx, wz) < 0.25 and max(wx, wz) > 0.8:
                kind = 'thin'  # a chain-link fence or lattice, drawn with an alpha cut-out like leaves
            elif len(leaf) == len(pts) and height < 2.5:
                kind = 'bush'
            else:
                kind = 'plant'
        shape = (shape, reach, height, kind)
    _scenery_shapes[model] = shape
    if len(_scenery_shapes) <= 60:
        _log('scenery model {:016X}: {}'.format(model, '{}, {}'.format(shape[3], note) if shape
                                                else 'no shape ({})'.format(note or 'not in the game\'s packages')))
    return shape


def _scenery_near(x, z):
    """Generator: yields between batches; ends returning [(polygon, y0, y1, flags)] of the
    neighbourhood scenery near (x, z) (flags as hand_shapes: 1 stops you, 16 leaves, 32 slows)."""
    out = []
    if not _scenery_load():
        _scenery_retry()
        return out
    cx, cz = int(x // SCENERY_CELL), int(z // SCENERY_CELL)
    near = [p for i in (cx - 1, cx, cx + 1) for j in (cz - 1, cz, cz + 1) for p in _scenery['cells'].get((i, j), ())]
    started = time.perf_counter()
    waiting = big = used = 0
    for model, px, py, pz, qx, qy, qz, qw, sc in near:
        if time.perf_counter() - started > 0.004:
            yield
            started = time.perf_counter()
        found = _scenery_shape(model)
        if found is False:
            waiting += 1
            continue
        if not found:
            continue
        prisms, reach, height, kind = found
        sc = sc or 1.0
        if reach * sc > SCENERY_BIG / 2:
            big += 1
            continue
        if math.hypot(px - x, pz - z) - reach * sc > RADIUS:
            continue
        used += 1

        def world(lx, lz):
            vx, vz = lx * sc, lz * sc
            tx, ty, tz = 2 * qy * vz, 2 * (qz * vx - qx * vz), -2 * qy * vx
            return (px + vx + qw * tx + (qy * tz - qz * ty), pz + vz + qw * tz + (qx * ty - qy * tx))

        for corners, y0, y1, leaves in prisms:
            if not leaves or kind in ('solid', 'thin'):
                flags = 1
            else:
                flags = 16 | (32 if kind == 'bush' else 0)
            out.append(([world(lx, lz) for lx, lz in corners], py + y0 * sc, py + y1 * sc, flags))
    if waiting:
        _scenery_retry()
    key = (_scenery['area'], used, big, waiting)
    if _scenery_logs[0] < 30 and key != _scenery_logs[1]:
        _scenery_logs[0] += 1
        _scenery_logs[1] = key
        _log('scenery near {:.1f} {:.1f}: {} objects, {} shapes{}{}'.format(
            x, z, used, len(out), ', {} too big left out'.format(big) if big else '',
            ', {} models still loading'.format(waiting) if waiting else ''))
    return out


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


def _hand_polygon(obj, pts, whole=False):
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
    if whole:
        return rect  # a placeholder footprint (a CC half wall's 1 cm square): the model box is the object
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


def _area(polygon):
    pts = [(v.x, v.z) for v in polygon]
    return abs(sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1]
                   for i in range(len(pts)))) / 2 if len(pts) >= 3 else 0.0


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
    global walls, objects, hand_shapes, _hand_skip, _body_skip, _soft, _step
    _step_max[0] = _step_height()
    _step_all[0] = native.setting_flag('body', 'step_all', False)
    spent = 0.0
    started = time.perf_counter()
    level = surface.secondary_id
    floor = _floor(level, x, z)
    if floor is None:
        return
    _occ_at[:] = [x, z, level]
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
        fence = 1.0 if all(abs(v.y) < 0.001 for v in contour) else 0.0  # fences and railings come back at height 0 (outdoors.py)
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
                    new_walls.append(s + (floor - DROP, top, fence))
                    _jamb_ends(jambs, gaps, s, a, b, top)
    new_walls.extend(_jambs(jambs, floor - DROP))
    spent += time.perf_counter() - started
    yield
    started = time.perf_counter()
    # 3. Objects with their height ranges.
    new_objects = []
    new_hands = []
    new_skip = set()
    new_body_skip = set()
    new_soft = set()
    new_step = set()
    tight = native.setting_flag('body', 'tight_body', True)  # [body] tight_body: bodies meet the model, not its wider footprint
    for i, obj in enumerate(near):
        if i and i % BATCH == 0:
            spent += time.perf_counter() - started
            yield
            started = time.perf_counter()
        try:
            if obj.parent is not None:
                continue  # carried or held
            lowwall = _is_low_wall(obj)
            footprint = getattr(obj, 'footprint', None)
            if footprint is None or not obj.footprint_component.footprints_enabled:
                if lowwall:
                    _wall_note(obj, 'no footprint')
                continue
            polygons = obj.footprint_polygon
        except Exception:
            continue
        if polygons is None:
            if lowwall:
                _wall_note(obj, 'no footprint polygon')
            continue
        span = _y_range(obj, footprint)
        piece = _half_wall_polygons(obj)
        if piece is not None:
            polygons = piece  # its footprint is a 1 cm point; the piece itself is a 1 m wall (see _half_wall_polygons)
            span = (obj.position.y, obj.position.y + _HALF_WALL_HEIGHT)
            lowwall = True
            # Also as a wall side: bodies keep body_radius from walls but only furniture_radius (0.12 m) from an object, and
            # against a 0.2 m thick piece that is a keep-out band you can step through; 8th field marks it (muffling counts it once).
            (ax, az), (bx, bz) = _centre_line([(v.x, v.z) for v in piece[0]])
            new_walls.append((ax, az, bx, bz, obj.position.y - DROP, obj.position.y + _HALF_WALL_HEIGHT, 0.0, 1))
        if lowwall and (span is None or span[1] - span[0] < MIN_HEIGHT):
            span = (obj.position.y, obj.position.y + _WALL_HEIGHT)
        if span is None:
            continue
        y0, y1 = span
        base = y0
        if not lowwall and base <= floor + 0.2 and _looks_low_wall(obj, polygons, y1 - y0):
            lowwall = True
        if lowwall:
            y0 -= DROP  # down to the lower floor beside it; its top isn't cut by a slab a metre up
        else:
            y1 = min(y1, _top(level, obj.position.x, obj.position.z, floor))
        if y1 - y0 < MIN_HEIGHT or y1 <= floor:
            if lowwall:
                _wall_note(obj, 'height {:.2f}..{:.2f} over floor {:.2f}'.format(y0, y1, floor))
            continue
        cross = _wall_crossable(obj, polygons, level, surface) if lowwall else None
        # Standing on the floor: reach DROP below it too, so a floor reading a metre lower beside it (the ground
        # outside a terrace edge) doesn't make it "above your hips" (user 2026-11-03: floor 156.17 -> 155.17, walked through)
        ground = None if lowwall else _floor(level, obj.position.x, obj.position.z)
        grounded = ground is not None and base <= ground + 0.15
        if grounded:
            y0 -= DROP
        if lowwall:
            _wall_note(obj, '{:.2f} m tall: {}'.format(y1 - y0, 'you step over it' if cross else
                                                       'nothing to walk on beyond it, it stays a wall'))
        name = str(obj)[:48]
        material = _material(obj)
        shape = _shape_of(obj, y1)
        meshes = None if shape else _mesh_shapes(obj, y1)
        first = len(new_objects)
        hands_from = len(new_hands)
        body_meshes = False
        if time.perf_counter() - started > 0.004:
            spent += time.perf_counter() - started
            yield
            started = time.perf_counter()
        # A tall thing with several footprints and no model data to cut them down (a column): the big ones are
        # clearance around it, not the thing; keep the small ones for the body. [body] tight_body
        areas = [_area(poly) for poly in polygons]
        small = min((a for a in areas if a > 1e-4), default=0.0)
        loose = tight and not shape and not meshes and y1 - y0 > 1.5 and len(areas) > 1 and small > 0
        for polygon in polygons:
            pts = [(v.x, v.z) for v in polygon]
            if len(pts) >= 3:
                if loose and _area(polygon) > small * 2.5:
                    continue
                blocks = y0 < floor + HIP and (cross is False or _blocks_navmesh(surface, pts, floor))
                new_objects.append((pts, y0, y1, blocks, name, material, obj.id))
                if shape:  # its visible shape stands in for the footprint, for hands and body
                    new_skip.add(id(pts))
                    new_body_skip.add(id(pts))
                elif meshes is not None:
                    new_skip.add(id(pts))
                    if blocks and tight and meshes:  # the meshes stop your body too, not the footprint (columns)
                        new_body_skip.add(id(pts))
                        body_meshes = True
                else:
                    pa = abs(sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1]
                                 for i in range(len(pts)))) / 2
                    tiny = lowwall and pa < 0.05
                    hand = _hand_polygon(obj, pts, whole=tiny)
                    if tiny and hand is not None:
                        _log('{} has a placeholder footprint ({:.3f} m2): the model box stands in, {:.2f} x {:.2f} m'.format(
                            name, pa, math.hypot(hand[1][0] - hand[0][0], hand[1][1] - hand[0][1]),
                            math.hypot(hand[2][0] - hand[1][0], hand[2][1] - hand[1][1])))
                    if hand is not None:
                        # the model's own box stops your body too: a column's footprint is wider than the column
                        # (user 2026-11-01: could not get close to columns)
                        new_hands.append((hand, y0, y1, (1 | material << 8) if blocks and tight else None))
                        new_skip.add(id(pts))
                        if blocks and tight:
                            new_body_skip.add(id(pts))
        if y1 - y0 > 1.5 and len(_tall_logs[1]) < 60 and obj.definition.id not in _tall_logs[1]:
            _tall_logs[1].add(obj.definition.id)  # tall things like columns: what stops you, how wide
            try:
                xs = [v.x for poly in polygons for v in poly]
                zs = [v.z for poly in polygons for v in poly]
                _log('{} tall {:.2f} m: footprint {:.2f} x {:.2f} m, body from {}, hands from {}'.format(
                    str(obj).split(':')[0], y1 - y0, max(xs) - min(xs), max(zs) - min(zs),
                    'shape' if shape else ('meshes' if body_meshes else ('model box' if tight and any(id(o[0]) in new_body_skip for o in new_objects[first:]) else 'footprint')),
                    'shape' if shape else ('meshes' if meshes else 'model box or footprint')))
            except Exception:
                pass
        if shape:
            mine = new_objects[first:]
            flags = (1 if any(o[3] for o in mine) else 0) | next((o[5] for o in mine if len(o) > 5), 0) << 8
            # Leaves and fronds are soft like bushes (plant flag 16): no stop, they slow you.
            leafy = _plant_kind(obj) > 0  # alpha cut-outs elsewhere are solid (a laptop's body has one)
            new_hands.extend((pts, a, b, flags | (16 if leaves and leafy else 0)) for pts, a, b, leaves in shape)
        if body_meshes:
            meshes = [(pts, a, b, 1 | material << 8) for pts, a, b, _ in meshes]
        new_hands.extend(meshes or ())
        if _is_soft(obj):
            new_soft.update(id(o[0]) for o in new_objects[first:])
            # Its leaves are soft already (above); the opaque rest (a planter box, an edge) stops you, unless
            # the model has no leaf meshes to tell them apart: then all of it is soft.
            leafy_layers = sum(1 for _, _, _, leaves in shape or () if leaves)
            if _soft_logs[0] < 40 and obj.definition.id not in _soft_logs[1]:
                _soft_logs[0] += 1
                _soft_logs[1].add(obj.definition.id)
                _log('{}: {} leaf layers soft, {} other layers {}'.format(
                    str(obj).split(':')[0], leafy_layers, len(shape or ()) - leafy_layers,
                    'stop you' if leafy_layers else 'soft too (no leaf meshes to tell apart)'))
            if not leafy_layers:
                new_hands[hands_from:] = [(p, a, b, (2 if f is None else f) | 16) for p, a, b, f in new_hands[hands_from:]]
        elif _step_max[0] > 0 and cross is not False and _steppable(obj, level, y0, y1, shape, lowwall):
            new_step.update(id(o[0]) for o in new_objects[first:])
            new_hands[hands_from:] = [(p, a, b, (2 if f is None else f) | 64) for p, a, b, f in new_hands[hands_from:]]
        if grounded:
            new_hands[hands_from:] = [(p, a - DROP if a <= ground + 0.15 else a, b, f) for p, a, b, f in new_hands[hands_from:]]
    spent += time.perf_counter() - started
    walls, objects, hand_shapes, _hand_skip, _body_skip, _soft, _step = (new_walls, new_objects, new_hands, new_skip,
                                                                         new_body_skip, new_soft, new_step)
    # 4. The neighbourhood's own scenery (the area's .world file: trees, rocks, lamps, fences; drawn by the game,
    #    never live objects; user 2026-10-28: walked right through them). [body] world_scenery
    _scenery_on[0] = native.setting_flag('body', 'world_scenery', True)
    if _scenery_on[0]:
        spent += time.perf_counter() - started
        try:
            scenery = yield from _scenery_near(x, z)
        except Exception as e:
            scenery = []
            _log('scenery failed: {!r}'.format(e))
        started = time.perf_counter()
        hand_shapes = hand_shapes + scenery
    nav_added = 0
    try:
        polys = [o[0] for o in new_objects if o[3]] + [h[0] for h in hand_shapes if h[3] is not None and h[3] & 1 and not h[3] & (16 | 64)]
        polys += [p[0] for p in panels.values()]  # a closed door's panel: the navmesh may stop at it
        nav_added = yield from _navmesh_edges(sim, x, y, z, surface, floor, new_walls, polys)
    except Exception as e:
        _log('navmesh edges failed: {!r}'.format(e))
    _door_panels.clear()
    _door_panels.update(panels)
    _apply_doors()
    body = sum(1 for o in new_objects if o[3])
    high = sum(1 for o in new_objects if o[1] >= floor + HIP)
    _log('built at {:.2f} {:.2f} level {} (floor {:.2f}): {} wall sides ({} from the navmesh), {} doorways cut, {} object shapes '
         '({} stop your body, {} above your hips, {} you walk through); {:.1f} ms of work over the ticks, '
         '{} objects on the lot'.format(
             x, z, level, floor, len(new_walls), nav_added, len(gaps), len(new_objects), body, high,
             len(new_objects) - body - high, spent * 1000, len(everything)))


NAV_RAYS = 48  # [body] navmesh_edges: rays round you
NAV_REACH = 3.0  # metres the rays look out; a scene rebuild happens every ~3 m, so the edge is there before you reach it
NAV_EXPLAINED = 0.6  # metres: a navmesh edge this close to a scene wall or object is that (its margin), not a hole in the scene


def _seg_dist(px, pz, ax, az, bx, bz):
    dx, dz = bx - ax, bz - az
    l = dx * dx + dz * dz
    t = 0.0 if l < 1e-9 else min(max(((px - ax) * dx + (pz - az) * dz) / l, 0.0), 1.0)
    return math.hypot(px - (ax + dx * t), pz - (az + dz * t))


def _explained(px, pz, scene_walls, polys):
    """True when a wall side or a footprint in the scene lies within NAV_EXPLAINED of the point."""
    for w in scene_walls:
        if _seg_dist(px, pz, w[0], w[1], w[2], w[3]) < NAV_EXPLAINED:
            return True
    for pts in polys:
        n = len(pts)
        for i in range(n):
            if _seg_dist(px, pz, pts[i][0], pts[i][1], pts[(i + 1) % n][0], pts[(i + 1) % n][1]) < NAV_EXPLAINED:
                return True
    return False


def _navmesh_edges(sim, x, y, z, surface, floor, new_walls, polys):
    """Generator. What the game's navmesh stops Sims at but the scene doesn't know (user 2026-11-26, marks of half walls
    that are not object_Half_Wall pieces: the navmesh blocked 22 of 24 directions where the scene blocked none): rays all
    round you find the first spot the navmesh blocks; where nothing in the scene stands within NAV_EXPLAINED of it, the
    points joined up become wall sides (fence kind: your body keeps furniture_radius from them; the navmesh edge already
    lies a Sim's radius off the surface). Not in a pool, not at stairs. [body] navmesh_edges. 8th field 2: muffling skips it."""
    from sims4vr import stairs, walk
    if not native.setting_flag('body', 'navmesh_edges', True):
        return 0
    if surface.type == routing.SurfaceType.SURFACETYPE_POOL:
        return 0
    context = sim.routing_context
    origin = walk._location(x, y, z, surface)
    scene = list(new_walls)
    points = []
    for i in range(NAV_RAYS):
        if i and i % 8 == 0:
            yield
        a = i * 2 * math.pi / NAV_RAYS
        dx, dz = math.sin(a), math.cos(a)
        if routing.ray_test(origin, walk._location(x + dx * NAV_REACH, y, z + dz * NAV_REACH, surface), context):
            points.append(None)  # clear all the way
            continue
        lo, hi = 0.0, NAV_REACH
        for _ in range(6):
            mid = (lo + hi) / 2
            if routing.ray_test(origin, walk._location(x + dx * mid, y, z + dz * mid, surface), context):
                lo = mid
            else:
                hi = mid
        if hi < 0.1:
            points.append(None)  # you stand in it already: nothing to hold you back from
            continue
        px, pz = x + dx * hi, z + dz * hi
        level = surface.secondary_id
        if (_explained(px, pz, scene, polys) or stairs.near(px, y, pz, 0.8)
                or walk._water_depth(px + dx * 0.6, pz + dz * 0.6, level) > 0.05):  # water's edge: swim.py's, not a wall
            points.append(None)
        else:
            points.append((px, pz))
    added = 0
    for i, p in enumerate(points):
        if p is None:
            continue
        q = points[(i + 1) % NAV_RAYS]
        if q is not None and math.hypot(q[0] - p[0], q[1] - p[1]) < 1.2:
            new_walls.append((p[0], p[1], q[0], q[1], floor - DROP, floor + 2.0, 1.0, 2))
            added += 1
        elif points[i - 1] is None and (q is None or math.hypot(q[0] - p[0], q[1] - p[1]) >= 1.2):
            # a lone point: a short piece across the ray
            a = i * 2 * math.pi / NAV_RAYS
            tx, tz = math.cos(a), -math.sin(a)
            new_walls.append((p[0] - tx * 0.25, p[1] - tz * 0.25, p[0] + tx * 0.25, p[1] + tz * 0.25,
                              floor - DROP, floor + 2.0, 1.0, 2))
            added += 1
    return added


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
    for ax, az, bx, bz, _, _ in (w[:6] for w in walls):
        if _cross(x, z, ex, ez, ax, az, bx, bz):
            return 'wall'
    for pts, _, _, blocks, name, _, _ in objects:
        if not blocks or id(pts) in _body_skip:  # the visible shape stands in for its (wider) footprint
            continue
        n = len(pts)
        for i in range(n):
            if _cross(x, z, ex, ez, pts[i][0], pts[i][1], pts[(i + 1) % n][0], pts[(i + 1) % n][1]):
                return name
    for pts, _, _, flags in hand_shapes:
        if flags is None or not flags & 1 or flags & (16 | 64):
            continue  # hands only, plants (slow you) and low things you step over
        n = len(pts)
        for i in range(n):
            if _cross(x, z, ex, ez, pts[i][0], pts[i][1], pts[(i + 1) % n][0], pts[(i + 1) % n][1]):
                return 'shape'
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


_relaxed = [0, 0.0]


def relax(mask, x, y, z, surface):
    """The game's ray check (navmesh) keeps you ~0.5 m off furniture and leaves a wide hole around every column
    (user 2026-11-03: huge hit box; 24 directions blocked only by the ray check). Where the collision scene has
    nothing in that direction, the ground ahead is the same level and a body-stopping object is within 1 m (so
    it is that object's navmesh margin, not a drop, water or an unroutable area), the direction is opened:
    the scene's own pushOut stops you at the real edge. [body] scene_walk"""
    try:
        if (not mask or _state is None or _pending is not None or surface is None or not objects
                or _state[2] != surface.secondary_id or math.hypot(x - _state[0], z - _state[1]) > 1.5
                or surface.type == routing.SurfaceType.SURFACETYPE_POOL
                or not native.setting_flag('body', 'scene_walk', True)):
            return mask
        level = surface.secondary_id
        ground = _floor(level, x, z)
        if ground is None:
            return mask
        close = []  # the body-stopping objects within 1 m: the navmesh's margin is theirs
        for pts, _, _, blocks, _, _, _ in objects:
            if blocks and any(_near_segment(x, z, pts[i][0], pts[i][1], pts[(i + 1) % len(pts)][0],
                                            pts[(i + 1) % len(pts)][1], 1.0) for i in range(len(pts))):
                close.append(pts)
        if not close:
            return mask
        out = mask
        why = {'scene': 0, 'far': 0, 'floor': 0}
        for i in range(RAYS):
            if not mask & (1 << i):
                continue
            a = i * 2 * math.pi / RAYS
            if _ray_hit(x, z, a) is not None:
                why['scene'] += 1
                continue
            ex, ez = x + math.sin(a) * PROBE, z + math.cos(a) * PROBE
            if not any(_near_segment(ex, ez, pts[k][0], pts[k][1], pts[(k + 1) % len(pts)][0],
                                     pts[(k + 1) % len(pts)][1], 0.9) for pts in close for k in range(len(pts))):
                why['far'] += 1
                continue  # nothing of furniture that way: a ledge or wall the scene doesn't know stays closed
            f = _floor(level, ex, ez)
            if f is None or abs(f - ground) > 0.15:
                why['floor'] += 1
                continue
            out &= ~(1 << i)
        if out and time.monotonic() - _relaxed[1] > 3:
            _relaxed[1] = time.monotonic()
            _log('ray check still blocks {} directions at {:.2f} {:.2f} with furniture within 1 m: scene {}, no furniture that way {}, ground differs {}'.format(
                bin(out).count('1'), x, z, why['scene'], why['far'], why['floor']))
        if out != mask:
            _relaxed[0] += 1
            if _relaxed[0] <= 10 or time.monotonic() - _relaxed[1] > 30:
                _relaxed[1] = time.monotonic()
                _log('opened {} of the ray check\'s blocked directions at {:.2f} {:.2f}: only furniture margin, the scene stops you at the edge'.format(
                    bin(mask & ~out).count('1'), x, z))
        return out
    except Exception as e:
        if _relaxed[0] < 3:
            _relaxed[0] += 1
            _log('relax failed: {!r}'.format(e))
        return mask


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
    if now - _step_check[0] > SWITCH_EVERY:
        _step_check[0] = now
        if _step_height() != _step_max[0] or native.setting_flag('body', 'step_all', False) != _step_all[0]:
            _state = None  # the step switch or height changed: build the scene again
    if now - _scenery_on[1] > SWITCH_EVERY:
        _scenery_on[1] = now
        if native.setting_flag('body', 'world_scenery', True) != _scenery_on[0]:
            _state = None  # neighbourhood scenery switched: build the scene again
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
