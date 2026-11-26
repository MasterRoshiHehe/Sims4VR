"""Swimming, step 1 (thread "Integrate swimming in pools and ocean", user 2026-10-28 02:26Z): into and out of a
pool by the stick.

Walking into water deeper than walk.py's limit is closed by the wall check, and a pool's edge is closed by the
navmesh. Standing at the edge and pushing the stick toward the water, the game takes your Sim in with its own
route (a pool ladder, steps or the edge, ending in the swim posture on the pool surface), as carried.py does for
ladders. Swimming in the pool, pushing toward land does the same the other way. Nothing is chosen for you: you
only push the stick toward the water or the shore. Moving in the water is step 2 (walk.py lets you move while
the posture is mobile; the log says what the swim posture is).
Switch: [swim] enter_exit in sims4vr.ini. Logs: "swim:".
"""
import math
import time

import build_buy
import routing
import services
import sims4.math
import terrain

from sims4vr import native

REACH = 0.6  # metres ahead of you that must hold deep water (or land) for the stick to start a trip
FAR = (1.5, 2.5, 3.5)  # metres ahead tried for the trip's goal
DEEP = 0.2  # metres of water that closes a spot to walking (walk.py WATER_DEEP)
TOWARD = 0.3
TRIP_LIMIT = 30.0
STRAY = 6.0  # metres from the goal and from you at the start before the trip is called off
COOLDOWN = 3.0
DWELL = 0.8  # seconds the stick must keep pushing at the water or the shore before the trip starts (user 2026-10-28: thrown in and out)
AFTER_TRIP = 6.0  # seconds after a trip ends with no new one
REACH_OUT = 1.1  # metres ahead that must be land for the stick to climb out (the swim mask stops you SWIM_EDGE short of it)
SWIM_EDGE = 0.7  # metres from the shore at which the wall check closes the way on (walk.py)
PUSH = 0.5  # stick strength that counts
SWIMHERE = 103790  # terrain-Swimhere (re/tuning/index.tsv)

_trip = None  # (interaction, goal, started)
_pushing = False  # the game plans the route inside push_super_affordance, before _trip is set (trail.py would hold it)
_last_start = 0.0
_last_end = -1e9
_hold = None  # (what, since)
_enabled = None
_logs = 0
_probe_at = 0.0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 80:
        native._log('swim: ' + message)


def _on():
    global _enabled
    if _enabled is None:
        _enabled = native.setting_flag('swim', 'enter_exit', True)
    return _enabled


def active():
    return _trip is not None or _pushing


def is_swim_interaction(interaction):
    """The game's Swim Here (from the pool's menu): the game takes you in, as for a ladder, rather than leaving
    you to walk onto the water to its spot (user 2026-10-29: ran around on the water)."""
    try:
        return 'swim' in type(interaction).__name__.lower() or 'swim' in str(getattr(interaction, 'affordance', '')).lower()
    except Exception:
        return False


def forget():
    global _trip
    _trip = None


def _depth(x, z, level):
    try:
        return terrain.get_water_depth(x, z, level)
    except Exception:
        return 0.0


def _is_pool(x, y, z, level):
    try:
        return bool(build_buy.is_location_pool(sims4.math.Vector3(x, y, z), level))
    except Exception:
        return False


def _surface(level, kind):
    return routing.SurfaceIdentifier(services.current_zone_id(), level, kind)


def in_water(x, z, level):
    """True where there is water to swim in (the depth is -100 off the pool)."""
    return _depth(x, z, level) > 0.05


def swimming(sim):
    return sim.routing_surface.type == routing.SurfaceType.SURFACETYPE_POOL


def floor_height(x, z, surface, fallback):
    """Where the water carries you while you swim: the game's height on the pool surface (the water's top), else
    the bottom plus the depth. Logged every 2 s (what the game says) to confirm."""
    global _probe_at
    level = surface.secondary_id
    try:
        top = terrain.get_terrain_height(x, z, routing_surface=surface)
        bottom = terrain.get_terrain_height(x, z, routing_surface=_surface(level, routing.SurfaceType.SURFACETYPE_WORLD))
    except Exception:
        return fallback
    depth = _depth(x, z, level)
    now = time.monotonic()
    if now - _probe_at > 2.0:
        _probe_at = now
        _log('swimming at {:.1f},{:.1f}: pool surface height {:.2f}, ground {:.2f}, depth {:.2f}'.format(x, z, top, bottom, depth))
    # The pool surface height is the water's top (log 2026-10-28: 214.89 with the ground at 215.02 and 2.88 m of
    # depth: ground + depth floated you 3 m up). Off the water (depth -100) it is the ground.
    return top


def _toward_ok(stick):
    return stick is not None and stick[2] >= 0.25


def try_start(sim, x, y, z, stick, surface):
    """Pushing the stick (world direction stick[0], stick[1]): at the water's edge go in, in the water toward
    the shore come out. Returns True when a trip began."""
    global _trip, _last_start
    global _hold, _pushing
    now = time.monotonic()
    if _trip is not None or not _on() or now - _last_start < COOLDOWN or now - _last_end < AFTER_TRIP:
        return False
    if stick is None or stick[2] < PUSH:
        _hold = None
        return False
    dx, dz = stick[0], stick[1]
    n = math.hypot(dx, dz)
    if n < 1e-3:
        return False
    dx, dz = dx / n, dz / n
    level = surface.secondary_id
    goal = None
    if surface.type == routing.SurfaceType.SURFACETYPE_WORLD:
        if _depth(x + dx * REACH, z + dz * REACH, level) <= DEEP:
            _hold = None
            return False
        for d in FAR:
            gx, gz = x + dx * d, z + dz * d
            gy = terrain.get_terrain_height(gx, gz, routing_surface=_surface(level, routing.SurfaceType.SURFACETYPE_POOL))
            if _is_pool(gx, gy, gz, level) and _depth(gx, gz, level) > DEEP * 2:
                goal = (gx, gy, gz, _surface(level, routing.SurfaceType.SURFACETYPE_POOL))
        what = 'into the water'
    elif surface.type == routing.SurfaceType.SURFACETYPE_POOL:
        if _depth(x + dx * REACH_OUT, z + dz * REACH_OUT, level) > DEEP:
            _hold = None
            return False
        world = _surface(level, routing.SurfaceType.SURFACETYPE_WORLD)
        for d in reversed(FAR):
            gx, gz = x + dx * d, z + dz * d
            if _depth(gx, gz, level) <= 0.05:
                goal = (gx, terrain.get_terrain_height(gx, gz, routing_surface=world), gz, world)
        what = 'out of the water'
    else:
        return False
    if goal is None:
        _hold = None
        return False
    if _hold is None or _hold[0] != what:
        _hold = (what, now)
    if now - _hold[1] < DWELL:
        return False
    _hold = None
    _last_start = time.monotonic()
    _pushing = True
    try:
        import objects.terrain
        from server.pick_info import PickType
        from server_commands.sim_commands import CommandTuning, _build_terrain_interaction_target_and_context
        target, context = _build_terrain_interaction_target_and_context(
            sim, sims4.math.Vector3(goal[0], goal[1], goal[2]), goal[3], PickType.PICK_TERRAIN, objects.terrain.TerrainPoint)
        affordance = CommandTuning.TERRAIN_GOHERE_AFFORDANCE
        if goal[3].type == routing.SurfaceType.SURFACETYPE_POOL:
            # GoHere refuses swimmable water ("can swim here, but the test is negated", log 2026-10-28 04:33Z):
            # the game's own way in is terrain-Swimhere, which the pool's menu runs.
            from sims4.resources import Types
            found = services.get_instance_manager(Types.INTERACTION).get(SWIMHERE)
            if found is not None:
                affordance = found
        result = sim.push_super_affordance(affordance, target, context)
    except Exception as e:
        _pushing = False
        native._log('swim: starting the way {} failed: {!r}'.format(what, e))
        return False
    if not result:
        _pushing = False
        _log('the game would not take you {}: {}'.format(what, result))
        return False
    _trip = (result.interaction, goal, time.monotonic())
    _pushing = False
    _log('taking you {} (goal {:.1f},{:.1f},{:.1f}; posture now {})'.format(what, goal[0], goal[1], goal[2], sim.posture))
    return True


def update(sim):
    """Every tick: end the trip when its interaction is done; call it off if it takes too long."""
    global _trip, _last_end
    if _trip is None:
        return
    interaction, goal, started = _trip
    try:
        done = interaction is None or interaction.is_finishing or getattr(interaction, 'has_been_reset', False)
        if done:
            _trip = None
            _last_end = time.monotonic()
            posture = sim.posture
            _log('trip ended: posture {} (mobile {}), surface {}, at {:.1f},{:.1f},{:.1f}'.format(
                posture, getattr(posture, 'mobile', '?'), getattr(sim.routing_surface, 'type', '?'),
                sim.position.x, sim.position.y, sim.position.z))
            return
        if time.monotonic() - started > TRIP_LIMIT:
            _trip = None
            from interactions.interaction_finisher import FinishingType
            interaction.cancel(FinishingType.USER_CANCEL, 'VR: the swim trip took too long')
            _log('called the trip off (too long)')
    except Exception as e:
        _trip = None
        native._log('swim: following the trip failed: {!r}'.format(e))
