"""First person: the simulation side of free movement.

The player is the DLL's own object (player.cpp): the sticks and your head move
it, and the DLL puts the Sim's body under you every frame. Nothing here routes
the Sim. Ten times a second this module:
- says whether free movement is allowed: the Sim is standing and the game
  isn't taking it somewhere (a route to an object, a posture change);
- checks the navmesh around the player for walls and furniture, and the floor
  height around it, so the DLL slides along walls and follows slopes;
- teleports the simulation's Sim to the player now and then, so the game knows
  where you are (no route, no interaction, no sound);
- when you push the stick while the game is moving or seating the Sim,
  stops what it is doing with a hard cancel (halt.py, v0.13.159, user
  2026-10-15 18:54Z): a walk stops at once, a seat or a held thing still
  plays its way out (the camera keeps following it), and the game resets the
  Sim if it is still busy a few seconds later; then you get control back;
- stops routes the game gives your Sim that you didn't ask for (halt.guard,
  [body] route_guard), and ends on the client the game's routes it ran while
  it had your Sim, so no walk animation is left playing (halt.free_again);
- opens a way through doorways and swings doors open as you reach them
  (doors.py);
- lets you walk freely while an action you picked waits for you to walk
  there yourself (trail.py: the guiding trail);
- at a ladder or stairs it can't walk you up (spiral, object stairs), lets the
  game carry you through when you push toward it (carried.py);
- keeps the game in charge of your Sim for a whole WickedWhims scene: its
  animations, the wait at its spot and the moment between two of them
  (v0.13.195, user 2026-10-17 02:16Z: moving a scene to another object ended
  it; free movement came on between the old animation and the new one).
"""
import math
import time
import traceback
import weakref

import alarms
import clock
import routing
import services
import sims4.math
import terrain

from interactions.utils.interaction_liabilities import ANIMATION_CONTEXT_LIABILITY

from sims4vr import carried, doors, follow, halt, hint, native, outdoors, scripted, stairs, stride, trail
from sims4vr import livedrag  # noqa: F401  (no live-mode pick-up in first person)

TICK_SECONDS = 0.1
DEAD_ZONE = 0.25
PROBE_DISTANCE = 0.4  # metres: how close walls and furniture may come
RAYS = 24  # directions checked around the player (the DLL expects 24)
ALL_RAYS = (1 << RAYS) - 1
PASSAGE_REACH = 0.8  # metres from a doorway's centre line at which it opens up for you
PASSAGE_RAIL = 0.3  # metres either side of the centre line you can walk through it
PASSAGE_ALONG = math.radians(50)  # directions inside the doorway that count as along it
FLOOR_STEP = 0.5  # metres between the floor samples for the slope
SYNC_WHILE_MOVING = 1.0  # seconds between teleports while you walk
SYNC_DISTANCE = 0.05  # metres the Sim must be off before it is teleported
SYNC_STILL = 0.5  # seconds between teleports while you stand (your head moves the body a little)
SYNC_STILL_DISTANCE = 0.15  # metres off that teleport the standing Sim at once
MASK_STILL = (0.10, 1.0)  # standing: the wall check is reused this close (metres) and this fresh (seconds)
MASK_PUSHING = (0.02, 0.3)  # pushing the stick: the same, while it doesn't move you (a wall)
MASK_WHILE_PLANNING = 0.3  # seconds a wall check is reused wherever you are while the game plans a route
TAKE_OVER_EVERY = 1.0  # seconds between attempts to end what the Sim is doing
CANCEL_WAIT = 4.0  # seconds at most that the game keeps the Sim to finish a cancelled action (halt.py resets it at 3)
SCENE_STEPS = ('WickedWhims_Sex_Animation', 'WickedWhims_Routing_WaitAtSexDestination')  # affordance names
SCENE_GAP = 2.0  # seconds the game keeps your Sim after a scene step ends, for the next one to start
BODY_VIEW_ANIMS = 'WickedWhims_Sex_Animation'  # [body] body_view_anims when it isn't set
BODY_VIEW_GRACE = 1.5  # seconds the view keeps lying with the body between two listed animations
BODY_VIEW_READ_EVERY = 10.0  # seconds between reads of the list (the debug menu can change it)


class _Owner:
    pass


_owner = _Owner()
_alarm = None
_last_sync = 0.0
_last_take_over = 0.0
_social_logs = 0
_cancelled = []  # (interaction, when): cancelled with the stick, still winding down
_sleeping = False
_body_view_on = False
_body_view_seen = 0.0  # when a listed animation last played
_body_view_list = (None, (), -1e9)  # (setting text, name parts, read at)
_body_view_kinds = {}  # affordance: listed or not
_held = ()
_surface = None  # the level you walk on (the game's idea of it lags while you climb)
_flight = None  # the flight of stairs you were on last tick
_follow = [0.0, None]  # on the steps: when the simulation's Sim last followed you, and its level then
STAIR_FOLLOW_EVERY = 0.25  # seconds between moves of the simulation's Sim along a flight
STAIR_FOLLOW_DISTANCE = 0.1  # metres you move on the steps before it follows
STAIR_RAIL_MARGIN = 0.3  # metres to keep off a stair's sides
STAIR_MOUTH_HEIGHT = 0.35  # metres between you and the ramp at a stair's end that still let you on
STAIR_EXIT = 0.6  # metres past a flight's ends where the way along it stays open
STAIR_WALK_REACH = 0.25  # metres before a flight's end where the walk route stops (stride.py: it can't climb)
NO_CLAMP = (-1e9, 1e9)
NO_FLIGHT = (0.0, 0.0, 0.0, 0.0, 0.0, 0.0)  # no stairs: run 0
_was_free = None
_was_moving = False
_was_pushing = False
_mask_cache = None  # (x, y, z, level and flight, mask, time) of the last wall check
_floor_cache = None  # ((x, z in cm, level), floor tuple)
_errors = 0
_game_why = None  # why the game has the Sim now (None: it doesn't)
_you_at = None  # (x, z) where you last walked freely: a social's path is checked against it (scripted.py)
_game_logs = 0
_scene_seen = 0.0  # when a WickedWhims scene step was last on your Sim
_scene_kinds = {}  # affordance -> True for a scene step
_scene_watched = set()  # ids of the scene steps whose end is logged
_scene_logs = 0
_sync_logs = 0


def start():
    global _alarm
    if _alarm is None:
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)


def stop():
    global _alarm, _was_free
    if _alarm is not None:
        alarms.cancel_alarm(_alarm)
        _alarm = None
    _was_free = None
    native.set_player_state(False, 0, (0.0, 0.0, 0.0, 0.0, 0.0))
    doors.close_all()
    doors.forget()
    stairs.forget()
    carried.forget()
    sim = services.get_active_sim()
    if sim is not None:
        stride.stop(sim)


def _location(x, y, z, surface):
    return routing.Location(sims4.math.Vector3(x, y, z), sims4.math.Quaternion.ZERO(), surface)


def _surface_of(sim):
    return _surface if _surface is not None else sim.routing_surface


_plans = weakref.WeakSet()  # the game's PlanRoute elements that have run (see _wrap_plan_route)


def _game_planning():
    """True while the game's path planner works on a route (its own thread;
    PlanRoute.generate_path waits for nodes.plan_in_progress). routing.ray_test
    then waits for it to finish: the 23-67 ms wall checks of v0.13.182 came
    with "trail planned again" and the game planning socials (inferred from
    the log, not proven in the exe)."""
    for element in list(_plans):
        try:
            if element.path.nodes.plan_in_progress:
                return True
        except Exception:
            pass
    return False


def _wrap_plan_route():
    from interactions.utils.routing import PlanRoute
    original = PlanRoute.generate_path
    if getattr(original, '_sims4vr_walk', False):
        return

    def generate_path(self, timeline):
        _plans.add(self)
        result = yield from original(self, timeline)
        return result

    generate_path._sims4vr_walk = True
    PlanRoute.generate_path = generate_path


@native.timed('walk wall check reused (the game was planning a route)', 0.006)
def _reused_while_planning(c):
    return c[4]


def _cached_mask(sim, x, y, z, on, pushing, moving):
    """The wall check, reused while you stay where it was made: standing
    still it is only redone every second (the DLL only uses it for where the
    stick takes you), and pushing into a wall every 0.3 s. While the game
    plans a route the last one is reused for up to 0.3 s wherever you are,
    so the tick doesn't wait for the planner."""
    global _mask_cache
    now = time.monotonic()
    key = (stairs.surface_key(_surface_of(sim)), id(on))
    c = _mask_cache
    if c is not None and c[3] == key and not (pushing and not _was_pushing):
        reach, fresh = MASK_PUSHING if pushing or moving else MASK_STILL
        if now - c[5] < fresh and math.hypot(x - c[0], z - c[2]) < reach and abs(y - c[1]) < 0.05:
            return c[4]
        if on is None and now - c[5] < MASK_WHILE_PLANNING and _game_planning():
            return _reused_while_planning(c)
    mask = _blocked_mask(sim, x, y, z, on)
    _mask_cache = (x, y, z, key, mask, now)
    return mask


@native.timed('walk wall check', 0.006)
def _blocked_mask(sim, x, y, z, on):
    if on is not None:
        return _stair_mask(ALL_RAYS, on, x, y, z)
    surface = _surface_of(sim)
    origin = _location(x, y, z, surface)
    context = sim.routing_context
    mask = 0
    for i in range(RAYS):
        a = i * 2 * math.pi / RAYS
        end = _location(x + math.sin(a) * PROBE_DISTANCE, y, z + math.cos(a) * PROBE_DISTANCE, surface)
        if not routing.ray_test(origin, end, context):
            mask |= 1 << i
    for flight in stairs.near(x, y, z, PROBE_DISTANCE + 1.0):
        mask = _stair_mask(mask, flight, x, y, z)
    for passage in doors.passages(sim, x, y, z, PASSAGE_REACH):
        mask = _open_passage(mask, x, z, *passage)
    return mask


def _stair_mask(mask, f, x, y, z):
    """Stairs are a portal to the game, so the navmesh check stops you at
    them. On the steps you can go up or down the run (and back toward the
    middle from the sides); at either end the directions into the run are
    clear, but only from the floor level with that end, so the top can't be
    reached from under it (the game's stair portals likewise start on the
    routing surface of each end). That level is the ramp's height at the end
    nearest you, not at the probe's end: on a 45 degree flight the ramp 0.4 m
    in is already 0.4 m up, which blocked the way straight in right at the
    mouth (v0.13.129: smooth sliding stopped dead there, the old slide had
    sidestepped to a 30 degree ray)."""
    s, l = f.coords(x, z)
    rail = max(0.15, f.half_w - STAIR_RAIL_MARGIN)
    along = math.atan2(f.dx, f.dz)
    if f.on(x, y, z):
        mask = ALL_RAYS
        inward = along + (-math.pi / 2 if l > 0 else math.pi / 2)
        for i in range(RAYS):
            a = i * 2 * math.pi / RAYS
            off = abs(math.remainder(a - along, 2 * math.pi))
            if off < PASSAGE_ALONG or math.pi - off < PASSAGE_ALONG:
                mask &= ~(1 << i)
            elif abs(l) > rail and abs(math.remainder(a - inward, 2 * math.pi)) < PASSAGE_ALONG:
                mask &= ~(1 << i)
        return mask
    if abs(f.height(s) - y) >= STAIR_MOUTH_HEIGHT:  # height() clamps s to the run's ends
        return mask
    if abs(l) < rail and (f.run - 0.05 < s < f.run + STAIR_EXIT or -STAIR_EXIT < s < 0.05):
        # Just off an end, level with it: the game keeps the floor next to
        # a flight's ends off its navmesh (the stairs' portal), so every ray
        # from there failed and the top of the steps was an invisible wall
        # (user 2026-10-17 13:39Z; only the sides, outside that patch, let
        # you through). Both ways along the run are clear there, like a
        # doorway.
        along = math.atan2(f.dx, f.dz)
        for i in range(RAYS):
            off = abs(math.remainder(i * 2 * math.pi / RAYS - along, 2 * math.pi))
            if off < PASSAGE_ALONG or math.pi - off < PASSAGE_ALONG:
                mask &= ~(1 << i)
    for i in range(RAYS):
        if mask & (1 << i):
            a = i * 2 * math.pi / RAYS
            se, le = f.coords(x + math.sin(a) * PROBE_DISTANCE, z + math.cos(a) * PROBE_DISTANCE)
            if -0.3 < se < f.run + 0.3 and abs(le) < rail:
                mask &= ~(1 << i)
    return mask


def _open_passage(mask, x, z, ax, az, bx, bz):
    """Doorways are portals through a wall to the game, so the navmesh check
    stops you at them. Open a rail along the doorway's centre line: inside
    it you can walk along it (the walls either side still hold), and from
    the room the directions into it are clear."""
    distance, t = doors.segment_distance(x, z, ax, az, bx, bz)
    if distance < PASSAGE_RAIL and 0.0 < t < 1.0:
        along = math.atan2(bx - ax, bz - az)
        mask = ALL_RAYS
        for i in range(RAYS):
            off = abs(math.remainder(i * 2 * math.pi / RAYS - along, 2 * math.pi))
            if off < PASSAGE_ALONG or math.pi - off < PASSAGE_ALONG:
                mask &= ~(1 << i)
        return mask
    for i in range(RAYS):
        if mask & (1 << i):
            a = i * 2 * math.pi / RAYS
            ex, ez = x + math.sin(a) * PROBE_DISTANCE, z + math.cos(a) * PROBE_DISTANCE
            d, t = doors.segment_distance(ex, ez, ax, az, bx, bz)
            if d < PASSAGE_RAIL and 0.0 < t < 1.0:
                mask &= ~(1 << i)
    return mask


_stairs_logs = [0, 0.0]  # lines so far, when the last was written


def _log_stairs(sim, px, py, pz, on, ramp):
    """Once a second on the stairs (the first 60): the flight carrying you,
    where you are on it, and where the simulation's Sim is."""
    now = time.monotonic()
    if _stairs_logs[0] >= 60 or now - _stairs_logs[1] < 1.0:
        return
    _stairs_logs[0] += 1
    _stairs_logs[1] = now
    try:
        f = ramp if ramp is not None else on
        s, l = f.coords(px, pz)
        p = sim.position
        low, high = _level_floors(f, px, pz)
        native._log('walk: stairs: you at {:.2f} {:.2f} {:.2f}, {:.2f} m along {} (obj {:x}, from {:.2f} {:.2f} '
                    'toward {:.2f} {:.2f}), {:.2f} to its side, its ramp {:.2f} high here; the game\'s floors here: '
                    'lower level {}, upper level {}; on the steps: {}; the simulation\'s Sim at {:.2f} {:.2f} {:.2f} '
                    'on level {}'.format(
                        px, py, pz, s, f, f.obj_id, f.bx, f.bz, f.dx, f.dz, l, f.height(s),
                        'none' if low is None else '{:.2f}'.format(low),
                        'none' if high is None else '{:.2f}'.format(high),
                        'yes' if on is not None else 'no', p.x, p.y, p.z,
                        getattr(sim.routing_surface, 'secondary_id', '?')))
    except Exception as e:
        native._log('walk: stairs log failed: {!r}'.format(e))


def _flight_sides(f):
    """The flight you are on or at (stairs.ramp), for the DLL to hold you
    between its sides."""
    if f is None:
        return NO_FLIGHT
    return (f.bx, f.bz, f.dx, f.dz, f.run, f.half_w)


def _floor(sim, x, y, z, f):
    """(x, z, height, rise per unit x, rise per unit z, lowest, highest)
    around the player; f is the flight whose ramp carries you (stairs.ramp)."""
    global _floor_cache
    if f is not None:
        # The ramp as a plane through the bottom step under you, held between
        # the two floors: exact on the steps and flat past either end.
        s, _ = f.coords(x, z)
        gx, gz = f.slope()
        return (x - f.dx * s, z - f.dz * s, f.h0, gx, gz, min(f.h0, f.h1), max(f.h0, f.h1))
    surface = _surface_of(sim)
    key = (round(x * 100), round(z * 100), stairs.surface_key(surface))
    if _floor_cache is not None and _floor_cache[0] == key:
        return _floor_cache[1]
    try:
        h = terrain.get_terrain_height(x, z, routing_surface=surface)
        hx = terrain.get_terrain_height(x + FLOOR_STEP, z, routing_surface=surface)
        hz = terrain.get_terrain_height(x, z + FLOOR_STEP, routing_surface=surface)
        floor = (x, z, h, (hx - h) / FLOOR_STEP, (hz - h) / FLOOR_STEP) + NO_CLAMP
        _floor_cache = (key, floor)
        return floor
    except Exception:
        return (x, z, y, 0.0, 0.0) + NO_CLAMP


def _level_floors(f, x, z):
    """The game's floor height at (x, z) on the flight's lower and upper
    level (None if it can't say)."""
    try:
        return (terrain.get_terrain_height(x, z, routing_surface=f.low),
                terrain.get_terrain_height(x, z, routing_surface=f.high))
    except Exception:
        return None, None


def _stair_level(f, x, y, z):
    """The level whose floor at (x, z) is nearest your height on the steps.
    The game draws a Sim that isn't walking a route on its level's floor, so
    on the lower level the body stayed down at the bottom floor while you
    climbed (2026-10-14 test)."""
    low, high = _level_floors(f, x, z)
    if low is None or high is None:
        return f.high if f.coords(x, z)[0] > f.run / 2 else f.low
    return f.high if abs(high - y) < abs(low - y) else f.low


def _follow_on_steps(sim, pose, f):
    """On the steps the simulation's Sim goes along with you, on the level
    whose floor is nearest your height there."""
    now = time.monotonic()
    x, y, z = pose[0], pose[1], pose[2]
    level = _stair_level(f, x, y, z)
    key = stairs.surface_key(level)
    p = sim.position
    moved = math.hypot(x - p.x, z - p.z) > STAIR_FOLLOW_DISTANCE or abs(y - p.y) > STAIR_FOLLOW_DISTANCE
    if key == _follow[1] and (not moved or now - _follow[0] < STAIR_FOLLOW_EVERY):
        return
    if key != _follow[1]:
        low, high = _level_floors(f, x, z)
        native._log('walk: stairs: the simulation\'s Sim goes with you on level {} (you {:.2f} high; floors here: '
                    'lower level {}, upper level {})'.format(
                        getattr(level, 'secondary_id', '?'), y,
                        'none' if low is None else '{:.2f}'.format(low),
                        'none' if high is None else '{:.2f}'.format(high)))
    new_level = key != stairs.surface_key(sim.routing_surface)
    _follow[0] = now
    _follow[1] = key
    orientation = sims4.math.Quaternion(0.0, math.sin(pose[3] / 2), 0.0, math.cos(pose[3] / 2))
    _move(sim, sims4.math.Vector3(x, y, z), orientation, level)
    if new_level:
        _location_now(sim)


def _climb(sim, pose, on):
    """Your level follows the stairs: on the steps the simulation's Sim goes
    along with you; stepping off a flight puts you on the floor (or landing)
    at that end."""
    global _flight, _surface
    x, y, z = pose[0], pose[1], pose[2]
    if on is not None:
        _flight = on
        _follow_on_steps(sim, pose, on)
        return True
    if _flight is not None:
        f = _flight
        _flight = None
        _follow[1] = None
        s, _ = f.coords(x, z)
        if s >= f.run:
            new = f.high
        elif s <= 0.0:
            new = f.low
        else:
            new = f.high if abs(f.h1 - y) < abs(f.h0 - y) else f.low
        changed = stairs.surface_key(new) != stairs.surface_key(_surface_of(sim))
        if changed:
            _surface = new
            native._log('walk: now on level {} ({:.2f} high)'.format(getattr(new, 'secondary_id', '?'), y))
        _sync(sim, pose, False, force=True)
        if changed:
            _location_now(sim)
    return False


def _sleep_state(sim):
    """Asleep or napping: the game's own sleep state, which only the sleep and
    nap interactions set (blinks never touch it)."""
    global _sleeping
    try:
        sleeping = bool(sim.sleeping)
    except Exception:
        sleeping = False
    if sleeping != _sleeping:
        _sleeping = sleeping
        native.set_sleeping(sleeping)
        try:
            running = sim.queue.running
            posture = sim.posture
            detail = 'during {} (posture {})'.format(running, getattr(posture, 'posture_type', posture))
        except Exception:
            detail = ''
        native._log('walk: {} {}'.format('asleep' if sleeping else 'awake', detail))


def _held_candidates(sim):
    """Objects the Sim may hold: what it carries and the props of its current
    animations (a fork, a phone, a book)."""
    global _held, _errors
    ids = []

    def add(obj):
        if obj is not None and len(ids) < 8 and obj.id not in ids:
            ids.append(obj.id)

    contexts = []
    try:
        posture_state = sim.posture_state
        if posture_state is not None:
            for aspect in (posture_state.left, posture_state.right):
                if aspect is not None:
                    add(aspect.target)
            for aspect in (posture_state.left, posture_state.right, posture_state.body):
                if aspect is not None and aspect.animation_context is not None:
                    contexts.append(aspect.animation_context)
        for si in list(sim.si_state):
            # get_liability only reads; get_animation_context_liability would create one.
            liability = si.get_liability(ANIMATION_CONTEXT_LIABILITY)
            if liability is not None and liability.animation_context is not None:
                contexts.append(liability.animation_context)
        for context in contexts:
            for prop in list(getattr(context, '_props', {}).values()):
                add(prop)
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('walk: held objects check failed: {!r}'.format(e))
    ids = tuple(ids)
    if ids != _held:
        _held = ids
        native.set_held(list(ids))


def reset_sleep():
    global _sleeping, _body_view_on
    _sleeping = False
    native.set_sleeping(False)
    _body_view_on = False
    native.set_body_view(False)


def _body_view_parts():
    """Parts of affordance names from [body] body_view_anims, comma separated."""
    global _body_view_list
    text, parts, read = _body_view_list
    now = time.monotonic()
    if now - read < BODY_VIEW_READ_EVERY:
        return parts
    new = native.setting('body', 'body_view_anims', BODY_VIEW_ANIMS) or ''
    if new != text:
        parts = tuple(part.strip() for part in new.split(',') if part.strip())
        _body_view_kinds.clear()
    _body_view_list = (new, parts, now)
    return parts


def _body_view(sim, game):
    """While an animation listed in [body] body_view_anims plays, straight ahead
    lies the way your Sim's upper body does (on its back: looking straight
    ahead looks up); the headset still looks around freely (user 2026-10-17
    03:55Z). The camera does the turning (camera.cpp viewBase)."""
    global _body_view_on, _body_view_seen
    found = None
    if game:
        parts = _body_view_parts()
        if parts:
            for si in tuple(sim.si_state) + (sim.queue.running,):
                if si is None or getattr(si, 'is_finishing', False):
                    continue
                affordance = getattr(si, 'affordance', None)
                listed = _body_view_kinds.get(affordance)
                if listed is None:
                    name = getattr(affordance, '__name__', '') or ''
                    listed = _body_view_kinds[affordance] = any(part in name for part in parts)
                if listed:
                    found = si
                    break
    now = time.monotonic()
    if found is not None:
        _body_view_seen = now
    on = found is not None or (game and now - _body_view_seen < BODY_VIEW_GRACE)
    if on != _body_view_on:
        _body_view_on = on
        native.set_body_view(on)
        native._log('walk: {}'.format(
            "view lies with your Sim's body ({})".format(found) if on else 'view level again'))


def _winding_down(sim):
    """True while an action you cancelled with the stick is still finishing:
    the Sim plays its way out of it first, as with the game's cancel button."""
    now = time.monotonic()
    running = sim.queue.running
    si_state = sim.si_state
    still = []
    for interaction, when in _cancelled:
        if now - when > CANCEL_WAIT:
            continue
        if interaction is running or interaction in si_state:
            still.append((interaction, when))
    _cancelled[:] = still
    return bool(still)


def _scene_log(message):
    global _scene_logs
    _scene_logs += 1
    if _scene_logs <= 60:
        native._log('walk: ' + message)


def _watch_scene(si):
    """Log how a scene step ends and who ended it (a cancel with its caller)."""
    if id(si) in _scene_watched:
        return
    if len(_scene_watched) > 200:
        _scene_watched.clear()
    _scene_watched.add(id(si))
    try:
        si.register_on_finishing_callback(
            lambda i: _scene_log('scene step {} ended ({})'.format(i, getattr(i, 'finishing_type', None))))
        cancel = si.cancel

        def logged_cancel(*args, **kwargs):
            if not getattr(si, 'is_finishing', True):
                how = args[0] if args else kwargs.get('finishing_type')
                why = args[1] if len(args) > 1 else kwargs.get('cancel_reason_msg')
                callers = ' < '.join('{}:{}'.format(f.filename.replace('\\', '/').rsplit('/', 1)[-1], f.name)
                                     for f in reversed(traceback.extract_stack(limit=8)[:-1]))
                _scene_log('scene step {} cancelled ({}, {}) by {}'.format(si, how, why, callers))
            return cancel(*args, **kwargs)

        si.cancel = logged_cancel
    except Exception as e:
        _scene_log('watching {} failed: {!r}'.format(si, e))


def _scene_step(si):
    """True for a WickedWhims scene step (an animation, the wait at its spot)
    that isn't ending; notes when one was last seen."""
    global _scene_seen
    if si is None:
        return False
    affordance = getattr(si, 'affordance', None)
    step = _scene_kinds.get(affordance)
    if step is None:
        name = getattr(affordance, '__name__', '') or ''
        step = _scene_kinds[affordance] = any(kind in name for kind in SCENE_STEPS)
    if not step or getattr(si, 'is_finishing', False):
        return False
    _scene_seen = time.monotonic()
    _watch_scene(si)
    return True


def _in_scene(sim):
    """Why the game keeps your Sim for a WickedWhims scene, or None. Its steps
    follow each other (moving it to another object ends one animation and
    starts the next); free movement came on between them and the new one
    ended a second later (user 2026-10-17 02:16Z)."""
    for si in tuple(sim.si_state) + tuple(sim.queue):
        if _scene_step(si):
            return 'a WickedWhims scene: {}'.format(si)
    if time.monotonic() - _scene_seen < SCENE_GAP:
        return 'between two steps of a WickedWhims scene'
    return None


def _game_has_sim(sim):
    """True while the game moves or seats the Sim; the camera follows it then."""
    global _game_why, _game_logs
    why = _game_reason(sim)
    if why != _game_why:
        if _game_why and not why:
            halt.free_again(sim)  # no walk of the game's left playing on the client
        _game_why = why
        if why and _game_logs < 40:
            _game_logs += 1
            native._log('walk: the game has the Sim ({})'.format(why))
    return bool(why)


def _game_reason(sim):
    try:
        if _cancelled and _winding_down(sim):
            return 'winding down what the stick cancelled'
        posture = sim.posture
        if posture is None or not posture.mobile or posture.is_vehicle:
            return 'posture {}'.format(posture)
        controller = sim.transition_controller
        if controller is not None:
            if trail.holding(sim):
                return None  # the action waits for you to walk there (trail.py): you move freely
            interaction = getattr(controller, 'interaction', None)
            if scripted.is_social_with_you(interaction):
                scripted.stop_walking_over(interaction, sim, _you_at)  # only if the game walks you over anyway
            elif not carried.active():
                halt.guard(sim, interaction, _you_at)  # a route you didn't ask for
            if scripted.is_emote(interaction) and sim.routing_component.current_path is None:
                return None  # a reaction where you are (scripted.py): it doesn't stop you
            _scene_step(interaction)
            return 'transition for {}'.format(interaction)
        component = sim.routing_component
        if component.current_path is not None:
            if not carried.active():
                halt.guard(sim, sim.queue.running, _you_at)
            return 'routing a path'
        if component.is_moving:
            return 'moving'
        return _in_scene(sim)
    except Exception as e:
        global _errors
        _errors += 1
        if _errors <= 5:
            native._log('walk: posture check failed: {!r}'.format(e))
        return None


def _take_over(sim):
    """You pushed the stick while the game had the Sim: a hard cancel of what
    it's doing, except a conversation (v0.13.189, user 2026-10-17 01:37Z:
    reaching the Sim you picked a chat with and nudging the stick cancelled
    the chat and even reset the Sim): while the game lines your Sim up for a
    social or plays one, the stick does nothing; you leave a conversation
    only by cancelling it on your wrist (hint.py). Long walks for one are
    still ended by scripted.stop_walking_over and the route guard.

    Otherwise a hard cancel of what
    it's doing (halt.py; v0.13.159, user 2026-10-15 18:54Z: the cancel button's
    way, cancel_user, could leave a walk running that belonged to another
    Sim's action, and a walk animation playing). A walk stops at once; a seat
    or a held thing plays its way out first (the camera follows), then you get
    control. In a WickedWhims scene the eyes close first (fades.py: its end
    moves your Sim at once) and the cancel waits until they are shut."""
    global _last_take_over
    now = time.monotonic()
    if now - _last_take_over < TAKE_OVER_EVERY:
        return
    social = halt.social_running(sim)
    if social is not None:
        _last_take_over = now
        global _social_logs
        _social_logs += 1
        if _social_logs <= 20:
            native._log('walk: stick while the game lines you up for {}: kept (cancel it on your wrist to leave)'.format(social))
        return
    from sims4vr import fades
    if not fades.stick_ready(sim):
        return
    _last_take_over = now
    try:
        for interaction in halt.everything(sim, 'VR: you walked away (stick)'):
            _cancelled.append((interaction, now))
    except Exception as e:
        native._log('walk: taking over failed: {!r}'.format(e))


def _sync(sim, pose, moving, force=False, surface=None):
    """Teleport the simulation's Sim to the player's body, on your level."""
    global _last_sync, _sync_logs
    now = time.monotonic()
    if moving and not force and now - _last_sync < SYNC_WHILE_MOVING:
        return
    x, y, z, yaw = pose[0], pose[1], pose[2], pose[3]
    p = sim.position
    off = math.hypot(x - p.x, z - p.z)
    if off < SYNC_DISTANCE and abs(y - p.y) < 0.2:
        return
    if not moving and not force and now - _last_sync < SYNC_STILL and off < SYNC_STILL_DISTANCE:
        return
    _last_sync = now
    if off > 0.5:
        _sync_logs += 1
        if _sync_logs <= 20:
            native._log('walk: the simulation\'s Sim moved {:.2f} m to you (from {:.2f} {:.2f})'.format(off, p.x, p.z))
    orientation = sims4.math.Quaternion(0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2))
    _move(sim, sims4.math.Vector3(x, y, z), orientation, surface if surface is not None else _surface_of(sim))


@native.timed('walk teleport', 0.004)
def _move(sim, translation, orientation, surface):
    if _move_logs[0] < 60:
        _move_logs[0] += 1
        p = sim.position
        native.dll_log('walk: simulation Sim teleported {:.2f} m'.format(math.hypot(translation.x - p.x, translation.z - p.z)))
    sim.move_to(translation=translation, orientation=orientation, routing_surface=surface)


_move_logs = [0]


_location_logs = 0


def _location_now(sim):
    """Your level to the client at once (v0.13.108). The game sends a Sim's
    new place in line behind the Sim's animations, and an animation waiting on
    another Sim (a group chat with Sims on another floor) held it back: after
    you went upstairs the client kept your Sim downstairs for two minutes, so
    the game walked it to the computer chair through the rooms below and it
    popped up into the chair (user 2026-10-14 20:58Z; floor snap log -4.00).
    This sends the Sim's place again as the game's immediate ops do (not on
    the Sim's channel), each time your level changes."""
    global _location_logs
    try:
        from distributor.ops import SetLocation
        from distributor.system import Distributor
        op = SetLocation(sim)
        op._primary_channel_mask_override = 0  # Op(immediate=True): not behind the Sim's animations
        Distributor.instance().add_op(sim, op)
        if _location_logs < 20:
            _location_logs += 1
            native._log('walk: level {} sent to the game at once'.format(
                getattr(sim.routing_surface, 'secondary_id', '?')))
    except Exception as e:
        global _errors
        _errors += 1
        if _errors <= 5:
            native._log('walk: sending your level failed: {!r}'.format(e))


@native.timed('walk tick')
def _tick(_handle):
    global _errors, _was_free, _surface, _flight, _was_moving, _was_pushing, _you_at, _scene_seen
    try:
        from sims4vr import body
        if not body.enabled:
            return
        sim = services.get_active_sim()
        if sim is None:
            return
        _sleep_state(sim)
        from sims4vr import fades
        fades.scene_tick(sim)  # eyes closed while a WickedWhims scene switches animations
        _held_candidates(sim)
        stick = native.walk_input()
        pushing = stick is not None and stick[2] >= DEAD_ZONE
        if pushing:
            _scene_seen = 0.0  # the stick ends the wait for a scene's next step
        game = _game_has_sim(sim)
        _body_view(sim, game)
        carried.update(sim)
        # The stick still pushed from walking onto a trail's spot isn't a cancel (trail.py).
        late = trail.arrival_push(pushing)
        if game and pushing and not carried.active() and not late:
            _take_over(sim)
        pose = native.walk_pose()
        free = bool(pose is not None and pose[5])
        if free:
            px, py, pz = pose[0], pose[1], pose[2]
        else:
            p = sim.position
            px, py, pz = p.x, p.y, p.z
        if not (free and not game):
            _surface = sim.routing_surface  # the game moves the Sim: its level is right
            _flight = None
        on = stairs.flight_under(px, py, pz) if not game else None
        moving = bool(free and pose[4])
        mask = 0 if game else _cached_mask(sim, px, py, pz, on, pushing, moving)
        ramp = stairs.ramp(px, py, pz)
        native.set_player_state(not game, mask, _floor(sim, px, py, pz, ramp), _flight_sides(ramp))
        if free and not game:
            _you_at = (px, pz)
            if not _climb(sim, pose, on):
                # Coming to a stop: the Sim goes where you stopped at once.
                _sync(sim, pose, pose[4], force=_was_moving and not moving)
            doors.swing(sim, px, py, pz)
            if pushing and on is None:
                carried.try_start(sim, px, py, pz, stick, _surface_of(sim))
        else:
            doors.close_all()  # the game's own routes swing doors as it walks the Sim
        on_stairs = on is not None or (
            ramp is not None and -STAIR_WALK_REACH < ramp.coords(px, pz)[0] < ramp.run + STAIR_WALK_REACH)
        if on_stairs:
            _log_stairs(sim, px, py, pz, on, ramp)
        hint.update(sim, px, py, pz)
        follow.update(sim, px, py, pz)
        halt.update(sim, free and not game)
        outdoors.update(px, py, pz, _surface_of(sim))
        _was_moving = moving
        _was_pushing = pushing
        stride.update(sim, pose if free and not game else None, _surface_of(sim), stick[2] if stick is not None else 0.0,
                      on_stairs)
        if free != _was_free:
            _was_free = free
            native._log('walk: {}'.format('free movement' if free else 'following the Sim'))
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('walk tick failed: {!r}'.format(e))


try:
    _wrap_plan_route()
except Exception as e:
    native._log('walk: wrapping PlanRoute.generate_path failed: {!r}'.format(e))
