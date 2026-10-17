"""Outdoor sounds come in through doors and arches (user 2026-10-14, v0.13.68).

In first person the DLL fades the outdoor ambience (birds, wind, street) in
over [audio] outdoor_fade metres around the outside walls (ambience.cpp).
Inside, this says how much of it actually reaches you: it follows the way
out from the room you are in, through the lot's doorways on your level, up
to MAX_DOORWAYS of them and outdoor_fade metres long. Every doorway on the
way passes on part of the sound: an arch or open doorway all of it, a door
that is open right now (doors.py swings it open as you come near) [audio]
open_door, a closed door [audio] closed_door. The farther the way, the
less arrives. The best way out counts; plain walls let through [audio]
wall_leak of what the wall distance alone would give.

A porch, gazebo or balcony with only railings or fences around it is a room
to the game, and under its roof the game counts it as inside (user
2026-10-14, v0.13.79). A room whose edge is at least OPEN_SHARE open to the
outdoors (railings, fences or nothing instead of walls) counts as outside
here, and so does a doorway into one; so does a spot under a roof that is in
no room at all (eaves, a pergola). The game's wall contours tell real walls
from fences: fences and railings come back at height 0.

The DLL eases the change, so a door swinging open fades the outdoors in.
Switches: [audio] doors_matter (0 = the wall distance alone, v0.13.67),
[audio] railings_outside (0 = as before: a railed porch is inside).
Only your Sim, only in first person; god view keeps the game's ambience.
"""
import time

import build_buy
import routing
import services
import sims4.math

from sims4vr import doors, native

CHECK_EVERY = 0.2  # seconds
MAX_DOORWAYS = 4  # on one way out
STEP = 0.5  # metres between the points a room's floor is looked at
MAX_CELLS = 900  # points (225 m2): a bigger room is no porch
NEAR_WALL = 0.4  # metres from a wall's outline at which a room's edge is that wall
OPEN_SHARE = 0.25  # of a room's edge open to the outdoors makes it count as outside

_last = 0.0
_doorways = None  # the doors.py list the openings were worked out from
_openings = []  # (door id, swings, ax, az, bx, bz, cx, cz, level, (block, outside) for side a, same for side b)
_logs = 0
_errors = 0
_last_log = 0.0
_walls = {}  # level -> {(x, z) 2 m cell: [wall edges (ax, az, bx, bz) near it]}, None: not known
_open_rooms = {}  # (block, level) -> True when the room counts as outside


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 80:
        native._log('outdoors: ' + message)


def _number(key, default, low, high):
    try:
        value = float(native.setting('audio', key, str(default)))
    except (TypeError, ValueError):
        return default
    return min(high, max(low, value))


def _side(zone, location):
    level = location.routing_surface.secondary_id
    position = location.position
    return (build_buy.get_block_id(zone, position, level), bool(build_buy.is_location_outside(position, level)), level,
            (position.x, position.y, position.z))


def _wall_cells(zone, level):
    """The real walls on a level (not fences or railings), sorted into 2 m cells."""
    if level in _walls:
        return _walls[level]
    cells = {}
    found = False
    try:
        surface = routing.SurfaceIdentifier(zone, level, routing.SurfaceType.SURFACETYPE_WORLD)
        contours = build_buy.get_wall_contours(0.0, 0.0, surface, 0)
    except Exception as e:
        _log('the walls on level {} are not known: {!r}'.format(level, e))
        contours = ()
    walls = fences = 0
    for contour in contours:
        points = [(float(v.x), float(v.y), float(v.z)) for v in contour]
        if not points:
            continue
        if all(abs(y) < 0.001 for x, y, z in points):
            fences += 1  # fences and railings have no height here
            continue
        walls += 1
        for i in range(len(points)):
            a, b = points[i], points[(i + 1) % len(points)]
            edge = (a[0], a[2], b[0], b[2])
            for cx in range(int((min(a[0], b[0]) - NEAR_WALL) // 2), int((max(a[0], b[0]) + NEAR_WALL) // 2) + 1):
                for cz in range(int((min(a[2], b[2]) - NEAR_WALL) // 2), int((max(a[2], b[2]) + NEAR_WALL) // 2) + 1):
                    cells.setdefault((cx, cz), []).append(edge)
            found = True
    _log('level {}: {} walls, {} fences and railings'.format(level, walls, fences))
    _walls[level] = cells if found else None  # no walls at all: nothing is judged open
    return _walls[level]


def _on_wall(cells, x, z):
    for edge in cells.get((int(x // 2), int(z // 2)), ()):
        if doors.segment_distance(x, z, *edge)[0] < NEAR_WALL:
            return True
    return False


@native.timed('looking for open sides', 0.004)
def _is_open(zone, block, level, x, y, z):
    """Whether the room (block) around (x, z) counts as outside: in no room,
    or its edge is at least OPEN_SHARE open to the outdoors (railings, fences
    or nothing instead of real walls)."""
    if not native.setting_flag('audio', 'railings_outside', True):
        return False
    if block == 0:
        return True
    key = (block, level)
    if key in _open_rooms:
        return _open_rooms[key]
    cells = _wall_cells(zone, level)
    result = False
    if cells is not None:
        blocks = {}

        def at(i, j):
            got = blocks.get((i, j))
            if got is None:
                got = blocks[i, j] = build_buy.get_block_id(zone, sims4.math.Vector3(x + i * STEP, y, z + j * STEP), level)
            return got

        stack = [(0, 0)]
        seen = {(0, 0)}
        edge = opened = 0
        while stack and len(seen) <= MAX_CELLS:
            i, j = stack.pop()
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                n = (i + di, j + dj)
                other = at(*n)
                if other == block:
                    if n not in seen:
                        seen.add(n)
                        stack.append(n)
                    continue
                edge += 1
                mx, mz = x + (i + di / 2) * STEP, z + (j + dj / 2) * STEP
                if _on_wall(cells, mx, mz):
                    continue
                if other == 0 or build_buy.is_location_outside(sims4.math.Vector3(x + n[0] * STEP, y, z + n[1] * STEP),
                                                               level):
                    opened += 1
        if len(seen) > MAX_CELLS:
            _log('room {} on level {}: over {:.0f} m2, not a porch'.format(block, level, MAX_CELLS * STEP * STEP))
        else:
            result = edge > 0 and opened >= OPEN_SHARE * edge
            _log('room {} on level {}: {:.0f} m2, {:.0f}% of its edge open to the outdoors: {}'.format(
                block, level, len(seen) * STEP * STEP, 100.0 * opened / max(1, edge),
                'counts as outside' if result else 'inside'))
    _open_rooms[key] = result
    return result


@native.timed('finding the ways out', 0.004)
def _build(doorways):
    """Which rooms each doorway joins, and whether a side is outdoors."""
    zone = services.current_zone_id()
    manager = services.object_manager()
    openings = []
    inside = outside = 0
    for door_id, swings, ax, az, bx, bz, y in doorways:
        door = manager.get(door_id)
        sides = doors._sides(door) if door is not None else None
        if sides is None:
            continue
        try:
            a, b = _side(zone, sides[0]), _side(zone, sides[1])
        except Exception:
            continue
        if a[1] and b[1]:
            continue  # a gate in a fence: outdoors on both sides
        if a[1] or b[1]:
            outside += 1
        else:
            inside += 1
        openings.append((door_id, swings, ax, az, bx, bz, (ax + bx) / 2, (az + bz) / 2, a[2], a[:2] + a[3:],
                         b[:2] + b[3:]))
    _log('{} doorways to the outside, {} between rooms'.format(outside, inside))
    return openings


def _passes(opening, open_door, closed_door):
    """How much of the sound a doorway passes on: arch 1, door open or closed."""
    if not opening[1]:
        return 1.0
    return open_door if opening[0] in doors._open else closed_door


def _outdoors(zone, side, level):
    """A doorway's side is outdoors: no roof over it, or a room open to the outdoors."""
    return side[1] or _is_open(zone, side[0], level, *side[2])


def _through(zone, block, level, px, pz, fade, open_door, closed_door):
    """How much of the outdoors reaches (px, pz) in that room: the best way out."""
    best = 0.0
    best_way = None
    stack = [(block, px, pz, 1.0, 0.0, ())]
    while stack:
        room, x, z, gain, walked, used = stack.pop()
        for o in _openings:
            if o[8] != level or o[0] in used:
                continue
            if o[9][0] == room and not o[9][1]:
                beyond = o[10]
            elif o[10][0] == room and not o[10][1]:
                beyond = o[9]
            else:
                continue
            if used:
                far = walked + ((o[6] - x) ** 2 + (o[7] - z) ** 2) ** 0.5
            else:
                far = walked + doors.segment_distance(x, z, o[2], o[3], o[4], o[5])[0]
            if far >= fade:
                continue
            passed = gain * _passes(o, open_door, closed_door)
            heard = passed * (1 - far / fade)
            if heard <= best:
                continue  # every way on from here only loses more
            if _outdoors(zone, beyond, level):
                best = heard
                best_way = used + (o[0],)
            elif len(used) + 1 < MAX_DOORWAYS:
                stack.append((beyond[0], o[6], o[7], passed, far, used + (o[0],)))
    return best, best_way


def update(px, py, pz, surface):
    """walk.py, every tick in first person: where you are and on which level."""
    global _last, _doorways, _openings, _errors, _last_log, _walls, _open_rooms
    now = time.monotonic()
    if now - _last < CHECK_EVERY:
        return
    _last = now
    try:
        walls = _number('wall_leak', 0.25, 0, 1)
        fade = _number('outdoor_fade', 4.0, 0, 20)
        if not native.setting_flag('audio', 'doors_matter', True) or not fade > 0:
            native.outdoors(-1, walls)
            return
        listed = doors._doorway_list()
        if listed is not _doorways:
            _doorways = listed
            _openings = _build(listed)
            _walls = {}  # the lot may have been rebuilt: walls and open rooms are looked at again
            _open_rooms = {}
        level = getattr(surface, 'secondary_id', 0)
        zone = services.current_zone_id()
        position = sims4.math.Vector3(px, py, pz)
        if build_buy.is_location_outside(position, level):
            native.outdoors(1, walls)
            return
        block = build_buy.get_block_id(zone, position, level)
        if _is_open(zone, block, level, px, py, pz):
            native.outdoors(1, walls)
            if now - _last_log > 10.0:
                _last_log = now
                _log('in room {} on level {}: open to the outdoors (railings, fences or open sides), counts as '
                     'outside'.format(block, level))
            return
        heard, way = _through(zone, block, level, px, pz, fade, _number('open_door', 1.0, 0, 1),
                              _number('closed_door', 0.35, 0, 1))
        native.outdoors(heard, walls)
        if now - _last_log > 10.0:
            _last_log = now
            _log('in room {} on level {}: {:.2f} of the outdoors comes in{}'.format(
                block, level, heard, ' through doorways {}'.format(', '.join('{:x}'.format(i) for i in way))
                if way else ' (no way out within {:.1f} m)'.format(fade)))
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            _log('check failed: {!r}'.format(e))

