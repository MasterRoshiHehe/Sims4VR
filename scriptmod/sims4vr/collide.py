"""The shapes you collide with in first person (docs/COLLISION-PLAN.md).

Walls come from the game's wall outlines of your level (closed outlines,
both faces), with doorways and arches cut out where the door's thin
footprint strip sits (doors locked for you stay shut). Objects are their
routing footprints (only enabled ones) with the height range their model
really covers: object y plus the model box (wall shelves and cabinets hang
below or above their anchor), else the footprint's height. Heights are
read live (shelves can be raised, ceilings changed).

Phase 1: the scene is built and logged, and compared with walk.py's ray
check; nothing moves you by it yet.
"""
import math
import time

import build_buy
import placement
import services
import terrain

from objects.doors.door import Door

from sims4vr import native

RADIUS = 8.0  # metres around you that walls and objects are collected
REBUILD_MOVE = 3.0  # metres from the last build centre before it is built again
CHECK_EVERY = 2.0  # seconds between checks whether nearby objects moved
BATCH = 25  # objects looked at per tick (spreads the build over ticks)
MIN_HEIGHT = 0.05  # metres: lower things (rugs, seed packs) are walked over
HIP = 0.9  # metres above the floor: objects starting lower stop your body
GAP_DEPTH = 0.3  # metres either side of a door's strip cut out of the wall
DEFAULT_STOREY = 3.0  # metres when the level above has no height
COMPARE_EVERY = 1.0  # seconds between comparisons with the ray check
PROBE = 0.4 + 0.123  # metres: walk.PROBE_DISTANCE plus the navmesh's agent radius (it keeps Sims that far off)
RAYS = 24

walls = []    # (ax, az, bx, bz, y0, y1)
objects = []  # (polygon [(x, z), ...], y0, y1, name)
_state = None  # (centre x, z, level, planner build id, objects key)
_pending = None  # generator of a build in progress
_checked = 0.0
_compared = 0.0
_logs = 0
_disagree = [0, 0]  # comparisons that disagreed, all comparisons (since the last log)


def _log(message):
    native._log('collide: ' + message)


def forget():
    global _state, _pending, walls, objects
    _state = None
    _pending = None
    walls = []
    objects = []


def _floor(level, x, z):
    zone_id = services.current_zone_id()
    try:
        return terrain.get_lot_level_height(x, z, level, zone_id)
    except Exception:
        return None


def _storey(level, x, z):
    """Floor of this level and of the one above (the walls' top)."""
    y0 = _floor(level, x, z)
    if y0 is None:
        return None, None
    y1 = _floor(level + 1, x, z)
    if y1 is None or y1 <= y0 + 0.5:
        y1 = y0 + DEFAULT_STOREY
    return y0, y1


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


def _build(sim, x, y, z, surface):
    """Generator: yields between batches; ends with the new scene in place."""
    global walls, objects
    started = time.perf_counter()
    spent = 0.0
    level = surface.secondary_id
    floor, top = _storey(level, x, z)
    if floor is None:
        return
    gaps = []
    doors = []
    for obj in services.object_manager().values():
        if isinstance(obj, Door) and getattr(obj, 'level', None) == level:
            p = obj.position
            if (p.x - x) ** 2 + (p.z - z) ** 2 < (RADIUS + 2) ** 2:
                doors.append(obj)
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
                    new_walls.append(s + (floor, top))
    spent += time.perf_counter() - started
    yield
    new_objects = []
    door_ids = set(d.id for d in doors)
    batch = 0
    for obj in list(services.object_manager().values()):
        if obj.is_sim or obj.id in door_ids or isinstance(obj, Door):
            continue
        p = obj.position
        if (p.x - x) ** 2 + (p.z - z) ** 2 > RADIUS * RADIUS:
            continue
        if getattr(obj, 'level', None) != level:
            continue
        batch += 1
        if batch % BATCH == 0:
            spent += time.perf_counter() - started
            yield
            started = time.perf_counter()
        footprint = getattr(obj, 'footprint', None)
        if footprint is None:
            continue
        try:
            if not obj.footprint_component.footprints_enabled:
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
        y1 = min(y1, top)
        if y1 - y0 < MIN_HEIGHT or y1 <= floor:
            continue
        name = str(obj)[:48]
        for polygon in polygons:
            pts = [(v.x, v.z) for v in polygon]
            if len(pts) >= 3:
                new_objects.append((pts, y0, y1, name))
    spent += time.perf_counter() - started
    walls, objects = new_walls, new_objects
    body = sum(1 for o in new_objects if o[1] < floor + HIP)
    _log('built at {:.2f} {:.2f} level {} (floor {:.2f}, walls to {:.2f}): {} wall sides, {} doorways cut, '
         '{} object shapes ({} stop your body, {} head height only), {:.1f} ms over the ticks'.format(
             x, z, level, floor, top, len(new_walls), len(gaps), len(new_objects), body,
             len(new_objects) - body, spent * 1000))


def _objects_key(x, z, level):
    """Changes when an object near you is added, removed or moved (raised too)."""
    total = 0
    for obj in services.object_manager().values():
        p = obj.position
        if (p.x - x) ** 2 + (p.z - z) ** 2 < RADIUS * RADIUS and not obj.is_sim:
            total = (total * 31 + hash((obj.id, round(p.x, 2), round(p.y, 2), round(p.z, 2)))) & 0xFFFFFFFF
    return total


def _hits_ray(x, z, angle, floor):
    """True when the PROBE-long ray from (x, z) hits a wall side or a body-height object."""
    ex, ez = x + math.sin(angle) * PROBE, z + math.cos(angle) * PROBE
    for ax, az, bx, bz, _, _ in walls:
        if _cross(x, z, ex, ez, ax, az, bx, bz):
            return True
    for pts, y0, _, _ in objects:
        if y0 >= floor + HIP:
            continue
        n = len(pts)
        for i in range(n):
            if _cross(x, z, ex, ez, pts[i][0], pts[i][1], pts[(i + 1) % n][0], pts[(i + 1) % n][1]):
                return True
    return False


def _cross(px, pz, qx, qz, ax, az, bx, bz):
    def side(ox, oz, ux, uz, vx, vz):
        return (ux - ox) * (vz - oz) - (uz - oz) * (vx - ox)
    d1 = side(ax, az, bx, bz, px, pz)
    d2 = side(ax, az, bx, bz, qx, qz)
    d3 = side(px, pz, qx, qz, ax, az)
    d4 = side(px, pz, qx, qz, bx, bz)
    return (d1 > 0) != (d2 > 0) and (d3 > 0) != (d4 > 0)


def _compare(x, z, level, mask):
    """Logs how the scene's 24 rays differ from walk.py's (both 0.4 m)."""
    global _logs
    floor = _floor(level, x, z)
    if floor is None:
        return
    mine = 0
    for i in range(RAYS):
        if _hits_ray(x, z, i * 2 * math.pi / RAYS, floor):
            mine |= 1 << i
    _disagree[1] += 1
    if mine != mask:
        _disagree[0] += 1
        if _logs < 40:
            _logs += 1
            only_rays = bin(mask & ~mine).count('1')
            only_scene = bin(mine & ~mask).count('1')
            _log('at {:.2f} {:.2f}: {} directions blocked only by the ray check, {} only by the scene'.format(
                x, z, only_rays, only_scene))
    if _disagree[1] >= 60:
        _log('last {} checks: {} disagreed with the ray check'.format(_disagree[1], _disagree[0]))
        _disagree[0] = _disagree[1] = 0


@native.timed('collide tick', 0.006)
def update(sim, x, y, z, surface, free, mask):
    """walk.py's tick: keeps the scene built around you; compares while you walk freely."""
    global _state, _pending, _checked, _compared
    if surface is None:
        return
    if _pending is not None:
        try:
            next(_pending)
        except StopIteration:
            _pending = None
        except Exception as e:
            _pending = None
            _log('build failed: {!r}'.format(e))
        return
    now = time.monotonic()
    level = surface.secondary_id
    import routing
    build_id = routing.planner_build_id()
    stale = (_state is None or _state[2] != level or _state[3] != build_id
             or math.hypot(x - _state[0], z - _state[1]) > REBUILD_MOVE)
    key = None
    if not stale and now - _checked > CHECK_EVERY:
        _checked = now
        key = _objects_key(_state[0], _state[1], level)
        stale = key != _state[4]
    if stale:
        if key is None:
            key = _objects_key(x, z, level)
        _state = (x, z, level, build_id, key)
        _pending = _build(sim, x, y, z, surface)
        return
    if free and now - _compared > COMPARE_EVERY:
        _compared = now
        _compare(x, z, level, mask)
