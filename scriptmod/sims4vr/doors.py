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
CLOSE_REACH = 1.4  # metres from it at which it closes again behind you (at least; wider doors more)
CLOSE_DELAY = 1.5  # seconds you must have been clear of an open door before it closes
PUSH_SPEED = 0.7  # m/s a palm must move into a door to push it ([body] door_push_speed)
PUSH_TRAVEL = 0.10  # metres the palm must have travelled toward the door within PUSH_WINDOW
PUSH_WINDOW = 0.5  # seconds
PUSH_NEAR = 0.2  # metres from the door's plane within which a palm pushes it
PUSH_HALF = 0.5  # metres either side of the doorway's centre a palm still touches the door
PULL_GRIP = 0.6  # grip needed to take hold of a door
HANDLE_PUSH = 0.1  # metres your arm moves toward a door holding its handle to push it open
PULL_DISTANCE = 0.2  # metres you pull a held door toward you to open it
PUSH_HEIGHT = 2.0  # metres above the floor the door panel reaches
LEVEL = 1.0  # metres of height difference that count as another floor

_far_since = {}  # door id -> when you left its reach
_doorways = []  # (door id, swings, ax, az, bx, bz, y): every doorway's centre line on the lot
_doors_time = -1e9
_doors_sig = None
_doors_built = -1e9  # when the list was last built (rebuilt at least once a minute anyway)
_swing_logs = 0
_open = set()  # ids of the doors we hold open
_palms = [[], []]  # recent (time, x, y, z) of each palm
_push_logs = 0
_noside_logged = set()
_sliding = {}  # door model id -> slides
_halves = {}  # door id -> half the panel's width
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
                # No portal sides (yet): use the door's own facing so it can still be opened.
                try:
                    fwd = obj.transform.orientation.transform_vector(sims4.math.Vector3.Z_AXIS())
                    p = obj.position
                    doorways.append((obj.id, not is_open_passage(obj), p.x + fwd.x * 0.6, p.z + fwd.z * 0.6,
                                     p.x - fwd.x * 0.6, p.z - fwd.z * 0.6, p.y))
                    if obj.id not in _noside_logged:
                        _noside_logged.add(obj.id)
                        native._log('doors: {} {:x} has no portal sides, using its facing ({:.2f}, {:.2f}), model {:x}'.format(
                            type(obj).__name__, obj.id, fwd.x, fwd.z, getattr(obj.definition, 'id', 0)))
                except Exception:
                    pass
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


def _facing(door_id):
    """(is side a (the first portal location) on the door's front, facing x, facing z), or None."""
    door = services.object_manager().get(door_id)
    sides = _sides(door)
    fwd = door.transform.orientation.transform_vector(sims4.math.Vector3.Z_AXIS())
    a = sides[0].position
    return (a.x - door.position.x) * fwd.x + (a.z - door.position.z) * fwd.z > 0, fwd.x, fwd.z, door


def _flag(door_id, toward_a):
    """The game's swing flag that opens the door toward side a (toward_a) or toward side b.
    The game swings a door by one flag (1 DoorOpenBackward, 0 DoorOpenForward) that is fixed
    to the door's own facing, not to the side a Sim comes from (every Sim route sends 1).
    Assumption, switched by [body] door_flip: flag 1 swings toward the door's front. Logged
    per open (user 2026-10-28: a single flip was right for some doors, wrong for others)."""
    global _push_logs
    try:
        a_front, fx, fz, door = _facing(door_id)
    except Exception as e:
        native._log('doors: facing of {:x} failed: {!r}'.format(door_id, e))
        return 1
    toward_front = (toward_a == a_front)
    # User 2026-10-28: all tests on v0.13.625 (push from a sent 0, pull from a sent 1) swung inverted,
    # so by side: toward a = flag 0, toward b = flag 1. The facing above is only logged, to confirm
    # the rule holds for every door; [body] door_flip inverts it.
    flag = 0 if toward_a else 1
    if native.setting_flag('body', 'door_flip', False):
        flag = 1 - flag
    if _push_logs < 100:
        _push_logs += 1
        native._log('doors: {:x} {} facing ({:.2f}, {:.2f}), side a is on its {}; asked to swing toward side {} '
                    '(its {}): flag {} (by side, probe only: facing), model {:x}'.format(
                        door_id, type(door).__name__, fx, fz, 'front' if a_front else 'back', 'a' if toward_a else 'b',
                        'front' if toward_front else 'back', flag, getattr(door.definition, 'id', 0)))
    return flag


def set_open(door_id, open_, toward_a=None, flag=None):
    """Hold a door open (count you into it) or let it close; the wall gaps
    follow. toward_a: swing it toward side a (True) or side b (False) of the
    door; None: as the game's own routes do (flag 1)."""
    from sims4vr import collide
    if open_:
        if flag is None:
            flag = 1 if toward_a is None else _flag(door_id, toward_a)
        native.door(door_id, True, flag == 1)
    else:
        native.door(door_id, False)
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
            if distance < max(CLOSE_REACH, _half_width(door_id) + 0.9):
                near.add(door_id)
        now = time.monotonic()
        for door_id in list(_open):
            if door_id in near:
                _far_since.pop(door_id, None)
                continue
            # A door closes only once you have been clear of it for a while: a sliding door's
            # animation is nearly instant, and it must not shut on you in the doorway.
            since = _far_since.setdefault(door_id, now)
            if now - since >= CLOSE_DELAY:
                _far_since.pop(door_id, None)
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
        if abs(lateral) > max(PUSH_HALF, _half_width(door_id) + 0.1):  # a wide door's handle is far from its centre
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


def _rel_s_head(door_id, head):
    """How far the head is from the door's plane along its across axis (signed)."""
    for d_id, swings, ax, az, bx, bz, dy in _doorway_list():
        if d_id == door_id:
            nx, nz = bx - ax, bz - az
            length = math.hypot(nx, nz) or 1.0
            return ((head[0] - (ax + bx) / 2) * nx + (head[2] - (az + bz) / 2) * nz) / length
    return 1.0


def _half_width(door_id):
    """Half the width of the door's panel (the wall strip cut for it), cached."""
    half = _halves.get(door_id)
    if half is None:
        half = 0.45
        try:
            from sims4vr import collide
            gap = collide._strip(services.object_manager().get(door_id))
            if gap is not None:
                half = max(0.3, min(1.2, gap[2]))
        except Exception:
            pass
        _halves[door_id] = half
    return half


def _handle_needed():
    return native.setting_flag('body', 'door_handle', True)


def _has_handle(door_id):
    """Only doors with a turnable knob need a handle grab: models listed in [body]
    door_knob_models (user 2026-10-28: most doors have a fixed vertical handle and are just
    pushed). Sliding doors are handled by _is_sliding. Models in door_no_handle_models and
    double doors never need one."""
    if _is_double(door_id) and not _is_sliding(door_id):
        return False  # user 2026-10-28: object_door_double has no handles, a push opens it
    try:
        door = services.object_manager().get(door_id)
        model = '{:x}'.format(getattr(door.definition, 'id', 0))
        def ids(key):
            return [m.strip().lower().replace('0x', '') for m in (native.setting('body', key, '') or '').split(',') if m.strip()]
        return model in ids('door_knob_models') and model not in ids('door_no_handle_models')
    except Exception:
        return False


def _is_sliding(door_id):
    """Sliding or shutter doors: a model in [body] door_sliding_models, or one whose name says so
    (logged once per model so the list can be filled in)."""
    try:
        door = services.object_manager().get(door_id)
        model = getattr(door.definition, 'id', 0)
        known = _sliding.get(model)
        if known is None:
            listed = native.setting('body', 'door_sliding_models', '') or ''
            ids = [m.strip().lower().lstrip('0x') for m in listed.split(',') if m.strip()]
            names = ' '.join(str(x) for x in (door, getattr(door.definition, 'name', ''),
                                              getattr(door.definition, 'tuning_name', ''))).lower()
            known = '{:x}'.format(model) in ids or any(w in names for w in ('slid', 'pocket', 'shutter', 'roll'))
            _sliding[model] = known
            native._log('doors: model {:x} is {} ({})'.format(model, 'sliding' if known else 'swinging', names[:80]))
        return known
    except Exception:
        return False


def _is_double(door_id):
    """Double doors (tuning names like object_door_double, object_sliding_door_double): two leaves
    that meet in the middle, where the handles are."""
    door = services.object_manager().get(door_id)
    return door is not None and 'double' in str(door).lower()


def _slide_distance():
    try:
        return max(0.05, float(native.setting('body', 'door_slide', 0.15)))
    except Exception:
        return 0.15


def _rel_u(door_id, hand, head):
    """How far the hand is from the head along the door's width (the way it slides)."""
    for d_id, swings, ax, az, bx, bz, dy in _doorway_list():
        if d_id == door_id:
            nx, nz = bx - ax, bz - az
            length = math.hypot(nx, nz) or 1.0
            return ((hand[1][0] - head[0]) * -nz + (hand[1][2] - head[2]) * nx) / length
    return 0.0


def _handle_reach():
    try:
        return max(0.1, float(native.setting('body', 'door_handle_reach', 0.5)))
    except Exception:
        return 0.5


def _at_handle(door_id, px, py, pz, dy):
    """The hand is at the door's handle: within reach of either edge of the panel (which side
    the hinge is on is not known, so both count), at hand height."""
    for d_id, swings, ax, az, bx, bz, d_y in _doorway_list():
        if d_id != door_id:
            continue
        nx, nz = bx - ax, bz - az
        length = math.hypot(nx, nz) or 1.0
        nx, nz = nx / length, nz / length
        lateral = abs((px - (ax + bx) / 2) * -nz + (pz - (az + bz) / 2) * nx)
        half = _half_width(door_id)
        if not 0.3 < py - dy < 1.8:
            return False
        if _is_double(door_id) and lateral <= _handle_reach():
            return True  # double doors meet, and have their handles, in the middle
        return half - _handle_reach() <= lateral <= half + 0.2
    return False


_miss_logged = {}


def _miss_log(door_id, px, py, pz, dy):
    """A fist on a handled door away from its handle: say where, once in a while."""
    global _push_logs
    now = time.monotonic()
    if _push_logs >= 80 or now - _miss_logged.get(door_id, -9.0) < 3.0:
        return
    _miss_logged[door_id] = now
    _push_logs += 1
    for d_id, swings, ax, az, bx, bz, d_y in _doorway_list():
        if d_id == door_id:
            nx, nz = bx - ax, bz - az
            length = math.hypot(nx, nz) or 1.0
            lateral = abs((px - (ax + bx) / 2) * -nz / length + (pz - (az + bz) / 2) * nx / length)
            native._log('doors: grab on {:x} missed its handle: {:.2f} m along the panel (handle zone {:.2f} to {:.2f}), '
                        '{:.2f} m up (needs 0.3 to 1.8)'.format(door_id, lateral, _half_width(door_id) - _handle_reach(),
                                                              _half_width(door_id) + 0.1, py - dy))
            return


def _pull(sim, side, hand, y, head):
    """Take hold of a closed door and move your arm: toward the door pushes it open (it swings
    away from you), back toward you pulls it open (it swings toward you). With
    [body] door_handle on that needs grip and trigger held at the door's handle (a door
    model without a handle is only shoved, see push); off, a grip alone anywhere on the
    panel pulls. True when a door was opened."""
    px, py, pz = hand[1]
    grip, trigger = hand[3], hand[4]
    held = _held[side]
    need_handle = _handle_needed()
    if grip < PULL_GRIP * 0.7 or hand[5] or (need_handle and trigger < 0.35) or (not need_handle and trigger > 0.5):
        _held[side] = None
        return False
    if held is None:
        if grip < PULL_GRIP or (need_handle and trigger < 0.5):
            return False
        for door_id, swings, ax, az, bx, bz, dy in _doorway_list():
            if not swings or is_open(door_id) or not (dy - 0.1 < py < dy + PUSH_HEIGHT) or abs(dy - y) >= LEVEL:
                continue
            if need_handle and (not (_has_handle(door_id) or _is_sliding(door_id)) or not _at_handle(door_id, px, py, pz, dy)):
                if need_handle and (_has_handle(door_id) or _is_sliding(door_id)):
                    near = _panel(door_id, px, pz)
                    if near is not None and near[0] < PUSH_NEAR + 0.1:
                        _miss_log(door_id, px, py, pz, dy)
                continue
            found = _panel(door_id, px, pz)
            if found is not None and found[0] < PUSH_NEAR + (0.2 if need_handle else 0.0):
                _held[side] = (door_id, found[1], _rel_s(door_id, hand, head), _rel_u(door_id, hand, head))
                native.haptic(side, 0.15, 0.04)
                return False
        return False
    door_id, from_a, start, start_u = held
    found = _panel(door_id, px, pz)
    if found is None or is_open(door_id):
        _held[side] = None
        return False
    if need_handle and _is_sliding(door_id):
        slid = _rel_u(door_id, hand, head) - start_u
        if abs(slid) < _slide_distance():
            return False
        _held[side] = None
        if _locked(door_id, sim):
            native._log('doors: slide on locked {:x} refused'.format(door_id))
            return False
        flag = 1 if slid > 0 else 0
        if native.setting_flag('body', 'door_flip', False):
            flag = 1 - flag
        native._log('doors: hand {} slides {:x} by its handle {:+.2f} m along its width: flag {}'.format(
            'RL'[side], door_id, slid, flag))
        set_open(door_id, True, flag=flag)
        native.haptic(side, 0.3, 0.05)
        return True
    # Your arm moving, measured from your head so walking does not count.
    toward_door = (_rel_s(door_id, hand, head) - start) * (-1 if from_a else 1)
    toward_door = -toward_door  # positive: hand moving toward the plane
    pulling = -toward_door >= PULL_DISTANCE
    head_dist = abs(_rel_s_head(door_id, head))
    pushing = need_handle and toward_door >= min(HANDLE_PUSH, max(0.04, head_dist * 0.4))
    if not (pulling or pushing):
        return False
    _held[side] = None
    global _push_logs
    verb = 'pulls' if pulling else 'pushes'
    if _locked(door_id, sim):
        if _push_logs < 40:
            _push_logs += 1
            native._log('doors: {} on locked {:x} refused'.format(verb[:-1], door_id))
        return False
    if _push_logs < 40:
        _push_logs += 1
        native._log('doors: hand {} {} {:x} by its handle from side {}'.format(
            'RL'[side], verb, door_id, 'a' if from_a else 'b'))
    set_open(door_id, True, from_a if pulling else not from_a)  # toward the hand on a pull, away on a push
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
    handle_needed = _handle_needed()
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
            if handle_needed and (_has_handle(door_id) or _is_sliding(door_id)):
                continue  # a door with a handle is opened by its handle (_pull)
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
            head_dist = abs((head[0] - mx) * nx + (head[2] - mz) * nz)  # a face against the door leaves little arm room
            if travel < min(PUSH_TRAVEL, max(0.04, head_dist * 0.4)) or fastest < need:
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
            set_open(door_id, True, not from_a)  # away from the hand
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
