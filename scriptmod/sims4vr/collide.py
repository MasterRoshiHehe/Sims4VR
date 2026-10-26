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

Phase 1: the scene is built and logged, and compared with walk.py's ray
check; nothing moves you by it yet. Switch: [body] collision_scene
(debug menu: Controls > Moving), read every 2 s.
"""
import math
import time

import build_buy
import placement
import routing
import services
import sims4.math
import terrain

from indexed_manager import CallbackTypes
from objects.doors.door import Door

from sims4vr import native

RADIUS = 8.0  # metres around you that walls and objects are collected
REBUILD_MOVE = 3.0  # metres from the last build centre before it is built again
REBUILD_GAP = 1.0  # seconds at least between builds for objects moved near you
SCAN = 150  # objects of the whole lot looked at per tick while building
BATCH = 15  # nearby objects measured per tick while building
SWITCH_EVERY = 2.0  # seconds between reads of the debug-menu switch ([body] collision_scene)
MIN_HEIGHT = 0.05  # metres: lower things (rugs, seed packs) are walked over
HIP = 0.9  # metres above the floor: objects starting lower can stop your body
GAP_DEPTH = 0.3  # metres either side of a door's strip cut out of the wall
DEFAULT_STOREY = 3.0  # metres when the level above has no height
COMPARE_EVERY = 1.0  # seconds between comparisons with the ray check
PROBE = 0.4 + 0.123  # metres: walk.PROBE_DISTANCE plus the navmesh's agent radius (it keeps Sims that far off)
RAYS = 24

walls = []    # (ax, az, bx, bz, y0, y1)
objects = []  # (polygon [(x, z), ...], y0, y1, blocks the navmesh, name)
_state = None  # (centre x, z, level, planner build id)
_pending = None  # generator of a build in progress
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


def forget():
    global _state, _pending, walls, objects, _dirty
    _state = None
    _pending = None
    _dirty = 0.0
    walls = []
    objects = []
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
    global walls, objects
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
    for door in doors:
        try:
            if sim in door.get_disallowed_objects():
                continue  # locked for you: the wall stays shut
        except Exception:
            pass
        gap = _strip(door)
        if gap is not None:
            gaps.append(gap)
    new_walls = []
    tops = {}
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
    spent += time.perf_counter() - started
    yield
    started = time.perf_counter()
    # 3. Objects with their height ranges.
    new_objects = []
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
        for polygon in polygons:
            pts = [(v.x, v.z) for v in polygon]
            if len(pts) >= 3:
                blocks = y0 < floor + HIP and _blocks_navmesh(surface, pts, floor)
                new_objects.append((pts, y0, y1, blocks, name))
    spent += time.perf_counter() - started
    walls, objects = new_walls, new_objects
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
    for pts, _, _, blocks, name in objects:
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
