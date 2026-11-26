"""First person: knock on doors and windows (user 2026-11-01).

Make a fist (grip + trigger together) and hit a door or window. The DLL does the hit
test and plays your own sound in 3D at the hit (armik.cpp objectTrack, knock.cpp,
foleyaudio.cpp; sounds\\knock\\door and sounds\\knock\\window). This script only tells
it where the doors and windows around you are: each as an upright box (centre, along
unit, half width, half depth, bottom, top, type), refreshed every second. The game has
no player knock interaction (only the Welcome Wagon NPCs' doorknocker loot), so
nothing is pushed on the Sim; it is a sound only.

Doors: every Door within RADIUS that is closed (or locked for you) and has something to
open (arches are left out). Windows: objects with a WINDOW tag (as gazewatch.py) that
have a footprint strip in the wall. Log lines "knock: ..." in sims4vr_python.log.
"""
import struct
import time

import alarms
import clock
import services

from sims4vr import native

TICK_SECONDS = 1.0
RESCAN_EVERY = 10.0  # seconds between looks through the lot's objects
RADIUS = 6.0  # metres around you
DOOR_HEIGHT = 2.0
WINDOW_LOW, WINDOW_HIGH = 0.8, 2.2  # metres above the floor when the model's box is unknown
MAX_HALF = 1.6  # metres: a wider strip is not a single door or window
HALF_DEPTH = 0.12  # metres either side of the wall's middle
TYPE_DOOR, TYPE_WINDOW = 0.0, 1.0

POLL_SECONDS = 0.1
_owner = type('_Owner', (), {})()
_alarm = None
_poll = None
_cache = []  # (object, is door) found in the last rescan
_scanned = -1e9
_last_count = -1
_logs = 0
_errors = 0


def _log(message):
    global _logs
    if native.setting_flag('knock', 'log', True):
        _logs += 1
        if _logs <= 200:
            native._log('knock: ' + message)


def _note_error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('knock: {} failed: {!r}'.format(what, e))


def _first_person():
    try:
        from sims4vr import body
        return body.enabled and body.following() is not None
    except Exception:
        return False


def start():
    global _alarm, _scanned
    _scanned = -1e9
    if _alarm is None and native.setting_flag('foley', 'knock', True):
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
        _poll_alarm()
        _log('on')


def _poll_alarm():
    global _poll
    if _poll is None:
        _poll = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(POLL_SECONDS), _take, repeating=True)


def stop():
    global _alarm, _cache, _poll
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    alarm, _poll = _poll, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    _cache = []
    _push([])


def _push(targets):
    fn = native._optional('knock_targets')  # absent in a DLL older than the script
    if fn is None:
        return
    data = [float(len(targets))]
    for t in targets:
        data.extend(t)
    fn(struct.pack('<{}f'.format(len(data)), *data))


def _rescan():
    """Doors and windows on the lot (a scan of every object: not each tick)."""
    global _cache
    from objects.doors.door import Door
    from sims4vr import gazewatch
    tags = gazewatch._window_tag_set()
    found = []
    for obj in list(services.object_manager().values()):
        try:
            if getattr(obj, 'is_sim', False):
                continue
            if isinstance(obj, Door):
                found.append((obj, True))
            elif tags and tags & obj.get_tags():
                found.append((obj, False))
        except Exception:
            pass
    _cache = found


def _targets(sim):
    from sims4vr import collide, doors as doors_mod
    p = sim.position
    level = getattr(sim, 'level', None)
    out = []
    for obj, is_door in _cache:
        try:
            if getattr(obj, 'level', None) != level:
                continue
            q = obj.position
            if (q.x - p.x) ** 2 + (q.z - p.z) ** 2 > RADIUS * RADIUS:
                continue
            if is_door and (doors_mod.is_open_passage(obj) or doors_mod.is_open(obj.id)):
                continue  # an arch, or a door standing open: nothing to knock on
            gap = collide._strip(obj)
            if gap is None:
                continue
            (cx, cz), (ux, uz), half, _normal, _depth = gap
            if is_door:
                half = min(half, 0.6)
                y0, y1 = q.y, q.y + DOOR_HEIGHT
            else:
                if half > MAX_HALF:
                    continue
                rng = None
                try:
                    rng = collide._y_range(obj, None)
                except Exception:
                    pass
                y0, y1 = rng if rng else (q.y + WINDOW_LOW, q.y + WINDOW_HIGH)
            glass = not is_door or collide._material(obj) == collide.GLASS  # a glass door sounds like a window
            oid = int(obj.id)
            out.append((cx, cz, ux, uz, half, HALF_DEPTH, y0, y1, TYPE_WINDOW if glass else TYPE_DOOR,
                        float(oid & 0xFFFFFF), float((oid >> 24) & 0xFFFFFF), float(oid >> 48)))
        except Exception as e:
            _note_error('target', e)
    return out


@native.timed('knock tick', 0.004)
def _tick(_handle):
    global _scanned, _last_count
    try:
        sim = services.get_active_sim()
        if sim is None or not _first_person():
            if _last_count:
                _push([])
                _last_count = 0
            return
        now = time.monotonic()
        if now - _scanned >= RESCAN_EVERY:
            _scanned = now
            _rescan()
        targets = _targets(sim)
        _push(targets)
        if len(targets) != _last_count:
            _last_count = len(targets)
            _log('{} door(s) and window(s) within {:.0f} m ({} doors, {} windows on the lot)'.format(
                len(targets), RADIUS, sum(1 for _, d in _cache if d), sum(1 for _, d in _cache if not d)))
    except Exception as e:
        _note_error('tick', e)


def _take(_handle):
    """A knock that played (the DLL queues them): hands it to the reactions."""
    try:
        for _ in range(4):
            e = native.knock_event()
            if e is None:
                return
            oid, kind, hand, into = e
            obj = services.object_manager().get(oid)
            _log('knocked on a {} ({}) with the {} fist at {:.2f} m/s'.format(
                'window' if kind else 'door', obj, 'right' if hand else 'left', into))
            on_knock(obj, kind, hand, into)
    except Exception as e:
        _note_error('take', e)


def on_knock(obj, kind, hand, into):
    """Sims react to a knock (steps 2-4 of the plan: windows look and wave, doors get answered)."""
    pass
