"""Doors in first person: you walk through them yourself.

Every door is a portal through a wall to the game, so the navmesh check stops
you at it like at the wall. walk.py opens a rail along each doorway's centre
line in that check (see passages), so you can walk through arches, doorways
and doors with no interaction. A real door also swings: when you come close,
the DLL counts you into it the way a Sim's route does, and it plays its open
animation; when you have moved away it is counted out and closes. Doors
locked for your Sim stay walls.
"""
import math
import time

import services
from objects.doors.door import Door

from sims4vr import native

REFRESH = 10.0  # seconds between looking up the lot's doors again
OPEN_REACH = 0.9  # metres from a door's centre line at which it opens for you
CLOSE_REACH = 1.4  # metres from it at which it closes again behind you
LEVEL = 1.0  # metres of height difference that count as another floor

_doorways = []  # (door id, swings, ax, az, bx, bz, y): every doorway's centre line on the lot
_doors_time = -1e9
_doors_sig = None
_doors_built = -1e9  # when the list was last built (rebuilt at least once a minute anyway)
_swing_logs = 0
_open = set()  # ids of the doors we hold open
_errors = 0


def _doorway_list():
    """Every door on the lot, arches included, as its centre line. Rebuilt
    when the lot's portals change (going through every object on the lot
    every few seconds stalled the frame)."""
    global _doorways, _doors_time, _doors_sig, _doors_built
    now = time.monotonic()
    if now - _doors_time > REFRESH:
        _doors_time = now
        from sims4vr import stairs
        try:
            sig = stairs.portal_signature()
        except Exception:
            sig = None
        if sig is not None and sig == _doors_sig and now - _doors_built < 60.0:
            return _doorways
        _doors_sig = sig
        _doors_built = now
        doorways = _build()
        if len(doorways) != len(_doorways):
            swinging = sum(1 for d in doorways if d[1])
            native._log('doors: {} doors and {} open passages on the lot'.format(swinging, len(doorways) - swinging))
        _doorways = doorways
    return _doorways


@native.timed('listing doors', 0.004)
def _build():
    doorways = []
    try:
        for obj in services.object_manager().values():
            if not isinstance(obj, Door):
                continue
            sides = _sides(obj)
            if sides is not None:
                a, b = sides[0].position, sides[1].position
                doorways.append((obj.id, not is_open_passage(obj), a.x, a.z, b.x, b.z, (a.y + b.y) / 2))
    except Exception as e:
        native._log('doors: listing doors failed: {!r}'.format(e))
    return doorways


def is_open_passage(door):
    """Arches and other openings with nothing to open. Gates are tuned the
    same way but have a gate that swings, so they count as doors."""
    if getattr(door, 'is_door_portal', True):
        return False
    return 'gate' not in type(door).__name__.lower()


def _sides(door):
    """(location, location) on either side of the door, or None."""
    try:
        front, back = door.get_single_portal_locations()
    except Exception:
        return None
    if front is None or back is None:
        return None
    return front, back


def _locked(door_id, sim):
    door = services.object_manager().get(door_id)
    if door is None:
        return True
    try:
        return sim in door.get_disallowed_objects()
    except Exception:
        return False  # no lock on it


def segment_distance(x, z, ax, az, bx, bz):
    """(distance from (x, z) to the segment a-b, position along it 0..1)."""
    dx, dz = bx - ax, bz - az
    length2 = dx * dx + dz * dz
    t = 0.0 if length2 < 1e-6 else max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / length2))
    return math.hypot(x - (ax + dx * t), z - (az + dz * t)), t


def passages(sim, x, y, z, reach):
    """Centre lines (ax, az, bx, bz) of the doorways within reach you may walk through."""
    found = []
    for door_id, swings, ax, az, bx, bz, dy in _doorway_list():
        if abs(dy - y) >= LEVEL or segment_distance(x, z, ax, az, bx, bz)[0] > reach:
            continue
        if swings and _locked(door_id, sim):
            continue
        found.append((ax, az, bx, bz))
    return found


def _swing_log(what, door_id):
    global _swing_logs
    _swing_logs += 1
    if _swing_logs <= 40:
        native._log('doors: {} {:x}'.format(what, door_id))


def swing(sim, x, y, z):
    """While you walk freely: open the doors you come close to, close the
    ones you have left behind."""
    global _errors
    try:
        near = set()
        for door_id, swings, ax, az, bx, bz, dy in _doorway_list():
            if not swings or abs(dy - y) >= LEVEL:
                continue
            distance = segment_distance(x, z, ax, az, bx, bz)[0]
            if distance <= OPEN_REACH and door_id not in _open and not _locked(door_id, sim):
                native.door(door_id, True)
                _open.add(door_id)
                _swing_log('opening', door_id)
            if distance < CLOSE_REACH:
                near.add(door_id)
        for door_id in list(_open):
            if door_id not in near:
                native.door(door_id, False)
                _open.discard(door_id)
                _swing_log('closing', door_id)
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('doors: swing failed: {!r}'.format(e))


def close_all():
    """Free movement ended: let every door we hold open close."""
    for door_id in list(_open):
        try:
            native.door(door_id, False)
        except Exception:
            pass
    _open.clear()


def forget():
    """A new lot: its doors are new objects."""
    global _doors_time, _doors_sig
    _open.clear()
    _doors_time = -1e9
    _doors_sig = None
