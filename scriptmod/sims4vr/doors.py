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
import sims4.math
from objects.doors.door import Door

from sims4vr import native

REFRESH = 10.0  # seconds between looking up the lot's doors again
OPEN_REACH = 0.9  # metres from a door's centre line at which it opens for you
CLOSE_REACH = 1.4  # metres from it at which it closes again behind you
PUSH_SPEED = 0.9  # m/s a palm must move into a door to push it ([body] door_push_speed)
PUSH_TRAVEL = 0.12  # metres the palm must have travelled toward the door within PUSH_WINDOW
PUSH_WINDOW = 0.5  # seconds
PUSH_NEAR = 0.2  # metres from the door's plane within which a palm pushes it
PUSH_HALF = 0.5  # metres either side of the doorway's centre a palm still touches the door
PULL_GRIP = 0.6  # grip needed to take hold of a door
PULL_DISTANCE = 0.2  # metres you pull a held door toward you to open it
PUSH_HEIGHT = 2.0  # metres above the floor the door panel reaches
LEVEL = 1.0  # metres of height difference that count as another floor

_doorways = []  # (door id, swings, ax, az, bx, bz, y): every doorway's centre line on the lot
_doors_time = -1e9
_doors_sig = None
_doors_built = -1e9  # when the list was last built (rebuilt at least once a minute anyway)
_swing_logs = 0
_open = set()  # ids of the doors we hold open
_palms = [[], []]  # recent (time, x, y, z) of each palm
_push_logs = 0
_last_try = [0.0]
_held = [None, None]  # per hand: (door id, hand is on side a, distance from the plane when taken)
_others = [frozenset()]  # watched doors the game has open (others passing)
_errors = 0


def manual():
    """[body] manual_doors: doors stay shut until your hands open them. Not in
    build/buy mode: the game's clock is stopped there, so doors would never
    open (user 2026-10-27); they stay as the game has them."""
    try:
        zone = services.current_zone()
        if zone is not None and getattr(zone, 'is_in_build_buy', False):
            return False
    except Exception:
        pass
    return native.setting_flag('body', 'manual_doors', True)


def is_open(door_id):
    """A door we hold open (a closed one is a wall while manual_doors is on)."""
    return door_id in _open or native.door_open(door_id)


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
        doorways, missing = _build()
        if missing:
            # A door without its portal sides yet (placed in build mode: the
            # game makes them after leaving it, user 2026-10-26): look again
            # at the next refresh, not only when the portals move.
            _doors_sig = None
        if len(doorways) != len(_doorways):
            swinging = sum(1 for d in doorways if d[1])
            native._log('doors: {} doors and {} open passages on the lot'.format(swinging, len(doorways) - swinging))
        _doorways = doorways
    return _doorways


@native.timed('listing doors', 0.004)
def _build():
    doorways = []
    missing = 0
    try:
        for obj in services.object_manager().values():
            if not isinstance(obj, Door):
                continue
            sides = _sides(obj)
            if sides is None:
                missing += 1
            else:
                a, b = sides[0].position, sides[1].position
                doorways.append((obj.id, not is_open_passage(obj), a.x, a.z, b.x, b.z, (a.y + b.y) / 2))
    except Exception as e:
        native._log('doors: listing doors failed: {!r}'.format(e))
    return doorways, missing


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
    manual_on = manual()
    for door_id, swings, ax, az, bx, bz, dy in _doorway_list():
        if abs(dy - y) >= LEVEL or segment_distance(x, z, ax, az, bx, bz)[0] > reach:
            continue
        if swings and (_locked(door_id, sim) or (manual_on and not is_open(door_id))):
            continue
        found.append((ax, az, bx, bz))
    return found


def _describe(door_id, front):
    """Log the door's facing and which side its first portal location is on, so a door that
    swings the wrong way can be compared with the others."""
    global _push_logs
    if _push_logs >= 80:
        return
    _push_logs += 1
    try:
        door = services.object_manager().get(door_id)
        sides = _sides(door)
        fwd = door.transform.orientation.transform_vector(sims4.math.Vector3.Z_AXIS())
        a = sides[0].position
        side_a_front = (a.x - door.position.x) * fwd.x + (a.z - door.position.z) * fwd.z > 0
        native._log('doors: {:x} {} facing ({:.2f}, {:.2f}), side a is on its {}, you came from side {}, model {:x}'.format(
            door_id, type(door).__name__, fwd.x, fwd.z, 'front' if side_a_front else 'back', 'a' if front else 'b',
            getattr(door.definition, 'id', 0)))
    except Exception as e:
        native._log('doors: describe failed: {!r}'.format(e))


def set_open(door_id, open_, front=True):
    """Hold a door open (count you into it) or let it close; the wall gaps
    follow. front: you come from the side of the door's first portal location
    (it swings away from that side)."""
    from sims4vr import collide
    # User's marks 2026-10-27: flag 1 swung one test door TOWARD side a, so a push from a sends 0.
    # [body] door_flip reverses it (debug menu) while the rule per door is not known.
    native.door(door_id, open_, front if native.setting_flag('body', 'door_flip', False) else not front)
    if open_:
        _describe(door_id, front)
    if open_:
        _open.add(door_id)
    else:
        _open.discard(door_id)
    collide.mark_dirty()
    try:
        from sims4vr import walk
        walk._mask_cache = None  # the way through opens at once
    except Exception:
        pass
    _swing_log('opening' if open_ else 'closing', door_id)


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
        auto = not manual()
        near = set()
        nearest = sorted((segment_distance(x, z, d[2], d[3], d[4], d[5])[0], d[0]) for d in _doorway_list()
                         if d[1] and abs(d[6] - y) < LEVEL)
        watched = [i for dist, i in nearest[:8] if dist < 6.0]
        native.door_watch(watched)  # other Sims' open doors count too
        opened = frozenset(i for i in watched if native.door_open(i))
        if opened != _others[0]:
            _others[0] = opened
            from sims4vr import collide
            collide.mark_dirty()
        for door_id, swings, ax, az, bx, bz, dy in _doorway_list():
            if not swings or abs(dy - y) >= LEVEL:
                continue
            distance = segment_distance(x, z, ax, az, bx, bz)[0]
            if auto and distance <= OPEN_REACH and door_id not in _open and not _locked(door_id, sim):
                set_open(door_id, True)
            if distance < CLOSE_REACH:
                near.add(door_id)
        for door_id in list(_open):
            if door_id not in near:
                set_open(door_id, False)
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('doors: swing failed: {!r}'.format(e))


def _panel(door_id, px, pz):
    """(distance from the door's plane on the hand's side, hand is on side a) when (px, pz)
    is in front of the panel, else None."""
    for d_id, swings, ax, az, bx, bz, dy in _doorway_list():
        if d_id != door_id:
            continue
        nx, nz = bx - ax, bz - az
        length = math.hypot(nx, nz) or 1.0
        nx, nz = nx / length, nz / length
        s_now = (px - (ax + bx) / 2) * nx + (pz - (az + bz) / 2) * nz
        lateral = (px - (ax + bx) / 2) * -nz + (pz - (az + bz) / 2) * nx
        if abs(lateral) > PUSH_HALF:
            return None
        return abs(s_now), s_now < 0
    return None


def _rel_s(door_id, hand, head):
    """How far the hand is from the head along the door's across axis (positive toward side b)."""
    for d_id, swings, ax, az, bx, bz, dy in _doorway_list():
        if d_id == door_id:
            nx, nz = bx - ax, bz - az
            length = math.hypot(nx, nz) or 1.0
            return ((hand[1][0] - head[0]) * nx + (hand[1][2] - head[2]) * nz) / length
    return 0.0


def _pull(sim, side, hand, y, head):
    """Grip a closed door's panel and pull it toward you: it swings open toward you.
    True when a door was opened."""
    px, py, pz = hand[1]
    grip = hand[3]
    held = _held[side]
    if grip < PULL_GRIP * 0.7 or hand[5] or hand[4] > 0.5:  # trigger too: a fist knocks, it doesn't pull
        _held[side] = None
        return False
    if held is None:
        if grip < PULL_GRIP:
            return False
        for door_id, swings, ax, az, bx, bz, dy in _doorway_list():
            if not swings or is_open(door_id) or not (dy - 0.1 < py < dy + PUSH_HEIGHT) or abs(dy - y) >= LEVEL:
                continue
            found = _panel(door_id, px, pz)
            if found is not None and found[0] < PUSH_NEAR:
                _held[side] = (door_id, found[1], _rel_s(door_id, hand, head))
                return False
        return False
    door_id, from_a, start = held
    found = _panel(door_id, px, pz)
    if found is None or is_open(door_id):
        _held[side] = None
        return False
    # Your arm coming back toward you, measured from your head so walking away does not pull.
    back = (_rel_s(door_id, hand, head) - start) * (1 if from_a else -1)  # from side a: toward a is negative
    if -back < PULL_DISTANCE:
        return False
    _held[side] = None
    global _push_logs
    if _locked(door_id, sim):
        if _push_logs < 40:
            _push_logs += 1
            native._log('doors: pull on locked {:x} refused'.format(door_id))
        return False
    if _push_logs < 40:
        _push_logs += 1
        native._log('doors: hand {} pulls {:x} from side {}'.format('RL'[side], door_id, 'a' if from_a else 'b'))
    set_open(door_id, True, not from_a)  # swings toward the hand
    native.haptic(side, 0.3, 0.05)
    return True


def _push_speed():
    try:
        return max(0.2, float(native.setting('body', 'door_push_speed', PUSH_SPEED)))
    except Exception:
        return PUSH_SPEED


def push(sim, x, y, z):
    """Manual doors: a deliberate shove of a palm into a closed door's panel
    (fast, over a real distance, ending at the door) pushes it open, away from
    the hand. A hand resting or drifting against it does nothing. Doors locked
    for you stay shut."""
    global _push_logs
    if not manual():
        return
    snap = native.hands()
    now = time.monotonic()
    if snap is None:
        return
    need = _push_speed()
    head = snap[0]
    for side in (0, 1):
        hand = snap[3 + side]
        trail = _palms[side]
        if not hand[0]:
            del trail[:]
            continue
        px, py, pz = hand[1]
        trail.append((now, px, py, pz, px - head[0], pz - head[2]))
        while trail and now - trail[0][0] > PUSH_WINDOW:
            trail.pop(0)
        if _pull(sim, side, hand, y, head):
            return
        if len(trail) < 3 or hand[3] > 0.5 or hand[4] > 0.5:
            continue  # a fist (grip or trigger held) knocks; only an open hand pushes
        for door_id, swings, ax, az, bx, bz, dy in _doorway_list():
            if not swings or is_open(door_id) or not (dy - 0.1 < py < dy + PUSH_HEIGHT):
                continue
            nx, nz = bx - ax, bz - az
            length = math.hypot(nx, nz)
            if length < 1e-3 or abs(dy - y) >= LEVEL:
                continue
            nx, nz = nx / length, nz / length  # across the door, from side a to side b
            mx, mz = (ax + bx) / 2, (az + bz) / 2
            s_now = (px - mx) * nx + (pz - mz) * nz
            lateral = (px - mx) * -nz + (pz - mz) * nx
            if abs(lateral) > PUSH_HALF or abs(s_now) > PUSH_NEAR:
                continue
            sign = 1 if s_now > 0 else -1
            start = trail[0]
            # Arm motion only: the hand relative to your head, so walking up to a door is not a shove.
            rel_now = (trail[-1][4] * nx + trail[-1][5] * nz) * sign
            rel_start = (start[4] * nx + start[5] * nz) * sign
            travel = rel_start - rel_now  # metres the arm moved toward the plane
            if travel < 0.04:
                continue
            # Fastest step toward the door over the window (not the average: a shove is a burst).
            fastest = 0.0
            for a, b in zip(trail, trail[2:]):
                if b[0] > a[0]:
                    fastest = max(fastest, -(((b[4] - a[4]) * nx + (b[5] - a[5]) * nz) * sign) / (b[0] - a[0]))
            if travel < PUSH_TRAVEL or fastest < need:
                if _push_logs < 60 and now - _last_try[0] > 0.5:
                    _last_try[0] = now
                    _push_logs += 1
                    native._log('doors: hand {} at {:x} too weak: {:.2f} m/s (needs {:.2f}) over {:.2f} m (needs {:.2f})'.format(
                        'RL'[side], door_id, fastest, need, travel, PUSH_TRAVEL))
                continue
            if _locked(door_id, sim):
                if _push_logs < 40:
                    _push_logs += 1
                    native._log('doors: push on locked {:x} refused'.format(door_id))
                continue
            from_a = s_now < 0  # the hand is on side a
            if _push_logs < 40:
                _push_logs += 1
                native._log('doors: hand {} pushes {:x} from side {} at {:.2f} m/s over {:.2f} m'.format(
                    'RL'[side], door_id, 'a' if from_a else 'b', fastest, travel))
            set_open(door_id, True, from_a)
            native.haptic(side, 0.3, 0.05)
            del trail[:]
            return


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
