"""Walk animation for free movement: the game's own walk cycle on the body.

Nothing walks on the simulation side. We send the client the same route
messages the game sends when a Sim walks (FOLLOW_ROUTE, ROUTE_UPDATE),
describing where the player is walking right now, and the client plays its
walk and turn animations along that path. The DLL keeps putting the body
exactly where the player is, so only the legs come from the route. No
interaction runs and the server never thinks the Sim is routing, so nothing
walks back afterwards (the game sends a route this way itself in
distance_placement_interaction).

Stopping: the client only leaves a route when its locomotion controller picks a
stop clip, and that needs the Sim to have walked the path itself. Ours is
pinned under the player, so the legs kept walking in place (and a route that
never ends also holds back every later route and teleport). So the DLL stops
the client's controller the way the engine's own stuck-route watchdog does
(native.walk_stop), right when you stop. Cutting the path short and waiting
for it made the client build ever shorter walk cycles on the last few
centimetres, which flickered, and a route started meanwhile queued behind it.

When the walk ends: as you let go of the stick and the body is nearly still,
or after a moment of pushing into a wall or a Sim. Only pushing the stick
starts one, so the body still slowing down never restarts the walk.

Stairs: the client walks a routing Sim on its level's floor (it follows the
floor under the route itself, ignoring the heights we send), so on a flight
the walking body stayed at the floor you started from: under the steps going
up, over your head going down. So there is no walk route on the steps or
right before them: the body goes up and down them in its standing pose, which
the DLL holds at the steps' height.

Running (a tap of the left stick; the DLL moves you at the run speed): the
route gets the Sim's own run walkstyle, the one the game gives long outdoor
routes, everywhere. Switching mid-walk ends the route and starts a new one.
"""
import math
import time

import clock
import id_generator
import services
from distributor.ops import GenericProtocolBufferOp
from distributor.system import Distributor
from protocolbuffers import DistributorOps_pb2, Routing_pb2

from sims4vr import native

UPDATE_EVERY = 0.2  # route seconds between path updates while walking
TURN_UPDATE = math.radians(20)  # update at once when the direction turns this much
LOOKAHEAD = 2.0  # route seconds of path ahead of the player
STEP = 0.25  # route seconds between path points
HISTORY = 1.0  # route seconds of walked path kept in each update
PUSHED = 0.2  # stick strength the DLL walks at (player.cpp kDeadZone)
RELEASE_SPEED = 0.35  # units per second: with the stick let go, the walk ends below this
MIN_SPEED = 0.1  # units per second: slower than this while pushing is being held back
BLOCKED_STOP = 0.3  # real seconds held back (a wall, a Sim) before the walk ends
START_DELAY = 0.1  # seconds after sending that the client starts a route (the game's own buffer)
# The game's own start and stop clips ([body] walk_clips, v0.13.194; user
# 2026-10-17 01:37Z: "it goes directly from a walking animation to a standing
# still animation"). The client's walk controller (game 1.128 WalkLogic) picks
# its clips by distance, not by a flag: the start clip when a route begins
# with the Sim standing and a long first segment within ~30 deg of its facing;
# the stop clip at a walk cycle's end once the path left is no longer than
# the stop clip's distance (the DLL reads it from the controller). So a walk
# begins with a long straight first segment and no updates while the start
# clip plays, and ends with one last update whose path ends where the DLL
# slows you down to (over that same distance) instead of the stuck-route stop.
CLIPS_LOOKAHEAD = 3.0  # route seconds ahead with clips on (the client completes a route ending < 2 s ahead)
START_SEGMENT = 2.5  # metres of the first segment, so the start clip can play
START_HOLD = 1.0  # real seconds at most without updates while the start clip plays
STOP_TIMEOUT = 1.5  # real seconds for the stop clip to finish the route before the old stop ends it
# Pushing again while the walk ends (user 2026-10-17 11:53Z: "still getting
# that jittering when I stop walking"; v0.13.215 log: 8 of 30 stops were cut
# with the old stop, from a walk cycle, the moment the stick came back past
# its dead zone): a push counts once it holds REPUSH_HOLD; before the stop
# clip starts the same route walks on, and while it plays it finishes first.
REPUSH_HOLD = 0.15  # real seconds the stick stays pushed before it counts
REPUSH_WAIT = 0.6  # real seconds at most to let a playing stop clip finish
STATE_START, STATE_STOP = 2, 4  # the controller's states


class _Route:
    __slots__ = ('id', 'number', 'start', 'walkstyle', 'past', 'last_update', 'direction', 'end', 'sent_at',
                 'stopping', 'clip_start', 'retried', 'final')

    def __init__(self, walkstyle):
        self.id = id_generator.generate_object_id()
        # Route time 0 is when the client starts it, a moment after we send.
        self.start = services.game_clock_service().monotonic_time() + clock.interval_in_real_seconds(START_DELAY)
        self.walkstyle = walkstyle
        self.past = []  # (t, x, y, z, yaw) where the player was
        self.last_update = 0.0
        self.direction = None
        self.end = 0.0  # route time of the last point sent
        self.number = 0
        self.sent_at = time.monotonic()
        self.stopping = None  # real time the last update (ending at your rest point) went out
        self.clip_start = False  # sent with a long first segment for the start clip
        self.retried = False  # the last update sent a second time (the client kept the old path)
        self.final = None  # (points, t) of the last update


_route = None
_recent = []  # ids of our last routes (halt.py: a client route not among them is the game's)
_ending = []  # (real time, route): routes the DLL is to end on the client then
_rate = 1.0  # route seconds per real second (game speed)
_last_clock = None  # (real time, game time) for measuring _rate
_errors = 0
_surface = None  # the level walk.py says you walk on
_routes = 0
_ended = 0
_blocked_since = None  # when pushing stopped moving you (a wall, a Sim)
_stops = 0
_clips_logs = 0
_pushed_since = None  # when the stick was pushed again while the walk ends


_clips_cache = (0.0, True)


def _clips():
    """[body] walk_clips, read at most every 2 s (ini reads are slow under MO2)."""
    global _clips_cache
    now = time.monotonic()
    if now - _clips_cache[0] > 2.0:
        _clips_cache = (now, native.setting_flag('body', 'walk_clips', True))
    return _clips_cache[1]


def _info(sim):
    """(state, path ahead, stop clip m, start clip m) of the client's walk, or None."""
    fn = native._optional('walk_info')
    try:
        return fn(int(sim.id)) if fn is not None else None
    except Exception:
        return None


def _route_time(route):
    """Where the client is in the route now, by our clock (from when we sent it)."""
    return (services.game_clock_service().monotonic_time() - route.start).in_real_world_seconds()


def _end_later(route, delay):
    _ending.append((time.monotonic() + delay, route))


def _end_due(sim, everything=False):
    """Have the DLL end the client routes whose time has come."""
    global _ending, _ended
    if not _ending:
        return
    now = time.monotonic()
    due = [r for at, r in _ending if everything or now >= at]
    if not due:
        return
    _ending = [(at, r) for at, r in _ending if not (everything or now >= at)]
    for route in due:
        native.walk_stop(sim.id, route.id)
        _ended += 1
        if _ended <= 5 or _ended % 50 == 0:
            native._log('stride: ending route #{} on the client'.format(route.number))


def _measure_rate():
    global _rate, _last_clock
    real = time.monotonic()
    game = services.game_clock_service().monotonic_time()
    if _last_clock is not None:
        dt = real - _last_clock[0]
        if dt > 0.05:
            sample = (game - _last_clock[1]).in_real_world_seconds() / dt
            _rate += (max(0.0, min(sample, 10.0)) - _rate) * 0.5
    _last_clock = (real, game)


def _message(sim, route, points, elapsed):
    pb = Routing_pb2.Route(id=route.id)
    surface = _surface if _surface is not None else sim.routing_surface
    for t, x, y, z, yaw in points:
        node = pb.nodes.add()
        location = node.location
        location.translation.x, location.translation.y, location.translation.z = x, y, z
        location.orientation.x, location.orientation.y = 0.0, math.sin(yaw / 2)
        location.orientation.z, location.orientation.w = 0.0, math.cos(yaw / 2)
        node.time = t
        node.action = 0
        node.walkstyle = route.walkstyle
        node.routing_surface_id.primary_id = surface.primary_id
        node.routing_surface_id.secondary_id = surface.secondary_id
        node.routing_surface_id.type = surface.type
    pb.time = elapsed
    pb.absolute_time_ms = int(services.game_clock_service().monotonic_time().absolute_ticks() + 100)
    return pb


def _send(sim, op_type, pb):
    Distributor.instance().add_op(sim, GenericProtocolBufferOp(op_type, pb, block_on_task_owner=False))


def _ahead(t, x, y, z, vx, vz, yaw, lookahead=LOOKAHEAD):
    """Points along where the player is heading, in route time."""
    points = []
    scale = 1.0 / _rate if _rate > 0.05 else 1.0  # route seconds go faster at higher game speeds
    k = 1
    while k * STEP <= lookahead:
        dt = k * STEP
        points.append((t + dt, x + vx * dt * scale, y, z + vz * dt * scale, yaw))
        k += 1
    return points


def _walkstyle(sim):
    """The Sim's walk, or its run while the player runs (the game's own choice
    of run for its posture and what it carries; the walk if it has none)."""
    global _errors
    if native.running():
        try:
            behavior = sim.get_walkstyle_behavior()
            run = behavior._try_get_posture_compatible_run_walkstyle(sim)
            if run is not None:
                carried = behavior._try_get_carry_compatible_walkstyle_override(run, sim)
                return carried if carried is not None else run
        except Exception as e:
            _errors += 1
            if _errors <= 5:
                native._log('stride: no run walkstyle: {!r}'.format(e))
    return sim.get_walkstyle()


def _walk(sim, pose):
    global _route
    x, y, z = pose[0], pose[1], pose[2]
    vx, vz = pose[6], pose[7]
    speed = math.hypot(vx, vz)
    yaw = math.atan2(vx, vz)
    direction = (vx / speed, vz / speed)
    route = _route
    walkstyle = _walkstyle(sim)
    if route is not None and _route_time(route) > route.end - STEP:
        # The client ran out of path (updates came too late): it would walk in
        # place on it forever, so end it and start a new route.
        _end_later(route, 0.0)
        route = None
    elif route is not None and route.walkstyle != walkstyle:
        # Running started or stopped: a new route in the other walkstyle.
        native._log('stride: route #{} switches walkstyle {} -> {}'.format(route.number, route.walkstyle, walkstyle))
        _end_later(route, 0.0)
        route = None
    if route is None:
        global _routes
        route = _Route(walkstyle)
        route.number = _routes + 1
        # Start where the player will be when the client starts the route.
        lead = START_DELAY / _rate if _rate > 0.05 else START_DELAY
        x, z = x + vx * lead, z + vz * lead
        route.past = [(0.0, x, y, z, yaw)]
        route.direction = direction
        if _clips():
            # One long straight segment first: room for the start clip.
            scale = 1.0 / _rate if _rate > 0.05 else 1.0
            pace = max(speed, 1.0) * scale
            end_t = max(CLIPS_LOOKAHEAD, START_SEGMENT / pace)
            points = route.past + [(end_t, x + direction[0] * pace * end_t, y, z + direction[1] * pace * end_t, yaw)]
            route.clip_start = True
        else:
            points = route.past + _ahead(0.0, x, y, z, vx, vz, yaw)
        route.end = points[-1][0]
        _recent.append(route.id)
        del _recent[:-8]
        _send(sim, DistributorOps_pb2.Operation.FOLLOW_ROUTE, _message(sim, route, points, 0.0))
        _route = route
        _routes += 1
        if _routes <= 5 or _routes % 50 == 0:
            native._log('stride: walk route #{} (walkstyle {}, speed {:.2f}, game speed x{:.2f})'.format(
                _routes, route.walkstyle, speed, _rate))
        return
    t = _route_time(route)
    if route.clip_start:
        # No updates while the start clip plays (an update restarts the walk).
        info = _info(sim)
        held = time.monotonic() - route.sent_at
        if held < START_HOLD and (held < START_DELAY + 0.15 or (info is not None and info[0] == STATE_START)):
            return
        route.clip_start = False
        _log_clips('route #{} start clip {} after {:.2f} s'.format(
            route.number, 'played' if info is not None and info[0] != STATE_START else 'not seen', held))
    turned = math.acos(max(-1.0, min(1.0, direction[0] * route.direction[0] + direction[1] * route.direction[1])))
    if t - route.last_update < UPDATE_EVERY and turned < TURN_UPDATE:
        return
    if t <= route.past[-1][0] + 0.02:
        return
    route.past.append((t, x, y, z, yaw))
    # Keep the start and the last second of walked path; times keep rising.
    route.past = [route.past[0]] + [p for p in route.past[1:] if p[0] >= t - HISTORY]
    route.last_update = t
    route.direction = direction
    points = route.past + _ahead(t, x, y, z, vx, vz, yaw, CLIPS_LOOKAHEAD if _clips() else LOOKAHEAD)
    route.end = points[-1][0]
    _send(sim, DistributorOps_pb2.Operation.ROUTE_UPDATE, _message(sim, route, points, t))


def _log_clips(text):
    global _clips_logs
    if _clips_logs < 200:
        _clips_logs += 1
        native._log('stride: ' + text)
        native.dll_log('stride: ' + text)


def _finish(sim, pose):
    """You let go of the stick: one last update, its path ending where the DLL
    brings you to rest (over the stop clip's distance), facing the way you
    face, for the client's stop clip. Returns False when it can't (then the
    old stop)."""
    route = _route
    info = _info(sim)
    rest = native._optional('walk_rest')
    rest = rest() if rest is not None else None
    if route is None or info is None or rest is None or not info[2] > 0.05:
        return False
    t = _route_time(route)
    x, y, z, facing = pose[0], pose[1], pose[2], pose[3]
    rx, rz, metres = rest
    speed = math.hypot(pose[6], pose[7])
    if metres < 0.02:
        return False
    if t <= route.past[-1][0] + 0.02:
        t = route.past[-1][0] + 0.02
    scale = 1.0 / _rate if _rate > 0.05 else 1.0
    duration = max(0.3, 2.0 * metres / max(speed * scale, 0.3))  # it slows to a stop: half the speed on average
    route.past.append((t, x, y, z, facing))
    route.past = [route.past[0]] + [p for p in route.past[1:] if p[0] >= t - HISTORY]
    points = route.past + [(t + duration, rx, y, rz, facing)]
    route.end = points[-1][0]
    route.stopping = time.monotonic()
    route.clip_start = False
    route.final = (points, t)
    _send(sim, DistributorOps_pb2.Operation.ROUTE_UPDATE, _message(sim, route, points, t))
    _log_clips('route #{} ends with the stop clip: {:.2f} m to rest (clip {:.2f} m) over {:.2f} s, '
               'controller state {}'.format(route.number, metres, info[2], duration, info[0]))
    return True


def _stopping(sim):
    """The route ending with the stop clip: done once the client left it;
    the old stop if the clip didn't finish it."""
    global _route
    route = _route
    waited = time.monotonic() - route.stopping
    on_client = native.client_route(sim.id) == route.id
    if not on_client and waited > START_DELAY + 0.2:
        _route = None
        _log_clips('route #{} finished by the game ({:.2f} s)'.format(route.number, waited))
    elif waited > STOP_TIMEOUT and not route.retried and route.final is not None:
        # Still far from its end: the client kept the old path. Once more,
        # from where the route is now, before the old stop.
        info = _info(sim)
        route.retried = True
        if info is not None and info[2] > 0.05 and info[1] > info[2] * 1.5:
            points, sent = route.final
            t = max(_route_time(route), points[-2][0] + 0.02)
            shift = t - sent
            points = points[:-1] + [(points[-1][0] + shift,) + tuple(points[-1][1:])]
            route.end = points[-1][0]
            route.stopping = time.monotonic() - (STOP_TIMEOUT - 1.0)  # one more second
            _send(sim, DistributorOps_pb2.Operation.ROUTE_UPDATE, _message(sim, route, points, t))
            _log_clips('route #{} kept its path ({:.2f} m left, state {}): last update sent again'.format(
                route.number, info[1], info[0]))
        else:
            route.stopping = time.monotonic() - (STOP_TIMEOUT - 0.5)
    elif waited > STOP_TIMEOUT:
        info = _info(sim)
        _log_clips('route #{} not finished by the stop clip after {:.2f} s (state {}, {:.2f} m left): stopped'.format(
            route.number, waited, info[0] if info else '?', info[1] if info else 0.0))
        _stop(sim, 'stop clip timed out')


def _walk_on(sim, pose, held):
    """Pushed again while the walk ends: before the stop clip, the same route
    walks on (no stop); while it plays, it finishes first (the game's own
    stop and start), unless that takes too long."""
    global _pushed_since
    route = _route
    info = _info(sim)
    state = info[0] if info is not None else -1
    on_client = native.client_route(sim.id) == route.id
    if on_client and state != STATE_STOP:
        route.stopping = None
        route.retried = False
        route.last_update = -1.0  # update at once
        _pushed_since = None
        _log_clips('route #{} walks on (pushed again before the stop clip, state {})'.format(route.number, state))
        _walk(sim, pose)
    elif not on_client:
        _pushed_since = None
        _log_clips('route #{} finished; pushed again: a new walk'.format(route.number))
        _stop(sim, 'finished, pushed again')
        _walk(sim, pose)
    elif held >= REPUSH_HOLD + REPUSH_WAIT:
        _pushed_since = None
        _log_clips('route #{} stop clip still playing after {:.2f} s pushed: stopped for a new walk'.format(
            route.number, held))
        _stop(sim, 'pushed again while stopping')
        _walk(sim, pose)
    else:
        _stopping(sim)


def _stop(sim, why):
    """The walk is over: the DLL ends the route on the client now (no more
    updates after this)."""
    global _route, _blocked_since, _stops
    _blocked_since = None
    route = _route
    _route = None
    if route is None:
        return
    _end_later(route, 0.0)
    _stops += 1
    if _stops <= 10 or _stops % 50 == 0:
        native._log('stride: route #{} stops ({}) {:.2f} s in'.format(route.number, why, _route_time(route)))


def update(sim, pose, surface=None, stick=0.0, on_stairs=False):
    """Every walk tick: pose is the DLL's (x, y, z, yaw, moving, free, vx, vz),
    or None when the player isn't moving freely; surface is the level you
    walk on (walk.py tracks it on stairs); stick is how far the left stick is
    pushed (0 to 1); on_stairs: you are on a flight of stairs or about to
    step onto one (no walk route there)."""
    global _errors, _surface, _blocked_since, _pushed_since
    _surface = surface
    try:
        _measure_rate()
        speed = math.hypot(pose[6], pose[7]) if pose is not None else 0.0
        pushing = stick >= PUSHED
        if pose is None or not pose[5] or _rate < 0.05:
            _stop(sim, 'free movement paused')
        elif on_stairs:
            _stop(sim, 'on the stairs')
        elif _route is None:
            if pushing and speed >= MIN_SPEED:
                _walk(sim, pose)
        elif _route.stopping is not None:
            now = time.monotonic()
            if pushing and speed >= MIN_SPEED:
                if _pushed_since is None:
                    _pushed_since = now
            else:
                _pushed_since = None
            if _pushed_since is not None and now - _pushed_since >= REPUSH_HOLD:
                _walk_on(sim, pose, now - _pushed_since)
            else:
                _stopping(sim)
        elif not pushing:
            if _clips() and _finish(sim, pose):
                pass
            elif speed < RELEASE_SPEED:
                _stop(sim, 'stick let go')
            else:
                _walk(sim, pose)  # still slowing down
        elif speed < MIN_SPEED:
            # Held back: the path already sent carries the legs for a moment
            # (an update now would be a path of no length).
            now = time.monotonic()
            if _blocked_since is None:
                _blocked_since = now
            elif now - _blocked_since >= BLOCKED_STOP:
                _stop(sim, 'held back')
        else:
            _blocked_since = None
            _walk(sim, pose)
        _end_due(sim)
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('stride: {!r}'.format(e))


def ours(route_id):
    """halt.py: a route on the client that is one of our walk routes."""
    return route_id in _recent


def playing(route_id):
    """halt.py: the route is the walk we are sending now, or one we have yet
    to end on the client (anything else of ours there is left over)."""
    return (_route is not None and _route.id == route_id) or any(r.id == route_id for _, r in _ending)


def stop(sim):
    """Free movement is over (the game takes the Sim, or first person ends)."""
    try:
        _stop(sim, 'the game has the Sim')
        _end_due(sim, everything=True)
    except Exception:
        pass
