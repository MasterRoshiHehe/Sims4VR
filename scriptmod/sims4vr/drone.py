"""Fly the Streaming Drone yourself in first person (thread "Add first-person drone control", user 2026-10-28).

How the game's drone works (decompiled tuning, re/tuning, EP06 / Get Famous): object_StreamingDrone_HighEnd / _LowEnd
is a routing object. Its interactions (streamingDrone_RecordNeighborhood, _Streaming, _RecordingSim, _CharityStream;
started by the streamingDrone_Launch_* pie entries) set its RoutingBehavior state (streamingDrone_RoutingBehavior_State,
object state 192970), and the object routing component starts a behaviour for that value: Neighborhood (194728) flies
it to spawn points, GetClose (194729) puts it in a routing formation behind a Sim, Land (193269) lands it by the Sim
(which also ends the interaction: exitConditionList_StreamingDrone_Landing). Stop (192975) is Stop_All_Behavior.

What this does, once the first-person Sim runs one of those interactions on a drone (and [drone] enabled):
  1. after START_DELAY (the take-off animation) the drone's routing state is set to Stop, so the game flies it no more;
  2. the DLL (drone.cpp) takes the camera: it hangs off a free flight point; left stick flies where you look, the right
     stick turns and lifts, A / X lands; your Sim stands where it is;
  3. here, ten times a second, the drone object is moved to the flight point (move_to; the game sends it to the client);
     the flight point is kept between the ground + min_height and + max_height;
  4. landing (A / X), the interaction ending, the drone breaking or running flat (the game sets its state to Land) ends
     it: the view returns to your Sim and the state is set to Land so the drone comes down by you the game's way.
Nothing here runs outside first person. Walls are not checked yet (the drone flies through them).
Switch: [drone] enabled. Logs: "drone:".
"""
import math
import time

import services
import sims4.math
import sims4.resources
import terrain

from sims4vr import native

TICK_MIN = 0.1  # seconds between runs (walk.py calls update about ten times a second)
START_DELAY = 1.5  # seconds after the interaction is seen before the drone is taken over (the take-off)
PILOT_AFFORDANCES = ('streamingdrone_recordneighborhood', 'streamingdrone_streaming', 'streamingdrone_recordingsim')
ROUTING_STATE = 192970    # object_state streamingDrone_RoutingBehavior_State
STOP = 192975             # streamingDrone_RoutingBehavior_Stop
LAND = 193269             # streamingDrone_RoutingBehavior_Land
START_RISE = 1.0  # metres above the drone the camera starts
MOVE_MIN = 0.05  # metres the flight point must move before the drone object follows
LOG_LIMIT = 60

_si = None          # the interaction being flown
_drone = None       # its drone object
_seen = 0.0         # when the interaction was first seen (monotonic)
_last_move = None   # (x, y, z) the drone object was last put at
_heading = 0.0
_active = False
_logs = 0
_last_run = 0.0
_errors = 0
_unmatched = set()
_scan_at = 0.0
_known = set()    # ids of drones already logged
_near_last = None  # the Sim's interaction names last logged while a drone was near


def _log(message):
    global _logs
    _logs += 1
    if _logs <= LOG_LIMIT:
        native._log('drone: ' + message)


def active():
    return _active


def _on():
    return native.setting_flag('drone', 'enabled', True)


def _num(key, default):
    try:
        return float(native.setting('drone', key, default))
    except (TypeError, ValueError):
        return default


def _state(value_id):
    manager = services.get_instance_manager(sims4.resources.Types.OBJECT_STATE)
    return manager.get(value_id) if manager is not None else None


def _is_drone(obj):
    return obj is not None and 'streamingdrone' in type(obj).__name__.lower()


def _find(sim):
    """The drone interaction the Sim runs: (interaction, drone) or None."""
    for si in list(sim.si_state):
        name = getattr(getattr(si, 'affordance', None), '__name__', '').lower()
        if name in PILOT_AFFORDANCES and _is_drone(si.target):
            return si, si.target
        if name not in _unmatched and ('drone' in name or _is_drone(getattr(si, 'target', None))):
            _unmatched.add(name)
            _log('interaction {} (target {}) is not a flyable drone interaction'.format(name, type(si.target).__name__))
    return None


def _names(items):
    return sorted(getattr(getattr(i, 'affordance', None), '__name__', type(i).__name__) for i in items)


def _scan(sim, now):
    """Diagnostic (every 3 s): log each drone object once, and what the Sim runs / queues while one is within 8 m."""
    global _scan_at, _near_last
    if now - _scan_at < 3.0:
        return
    _scan_at = now
    p = sim.position
    near = None
    for obj in list(services.object_manager().values()):
        if not _is_drone(obj):
            continue
        d = math.hypot(obj.position.x - p.x, obj.position.z - p.z)
        if obj.id not in _known:
            _known.add(obj.id)
            _log('drone object {} id {} at {:.1f} m, routing state {}'.format(type(obj).__name__, obj.id, d, _routing_value(obj)))
        if d < 8.0 and (near is None or d < near[0]):
            near = (d, obj)
    if near is None:
        _near_last = None
        return
    controller = getattr(sim, 'transition_controller', None)
    info = (tuple(_names(sim.si_state)), tuple(_names(sim.queue)),
            getattr(getattr(controller, 'interaction', None), '__name__', None) if controller is not None else None,
            _routing_value(near[1]))
    if info != _near_last:
        _near_last = info
        _log('drone {:.1f} m away: running {}, queued {}, transition {}, routing state {}'.format(near[0], *info))


def _set_routing(drone, value_id):
    value = _state(value_id)
    if value is None:
        _log('state {} not found'.format(value_id))
        return False
    drone.set_state(value.state, value)
    return True


def _routing_value(drone):
    state = _state(ROUTING_STATE)
    try:
        value = drone.get_state(state) if state is not None else None
    except Exception:
        return None
    return getattr(value, 'guid64', None)


def _ground(drone, surface):
    p = drone.position
    try:
        return terrain.get_terrain_height(p.x, p.z, surface)
    except Exception:
        return None


def _start(sim, si, drone):
    global _si, _drone, _active, _last_move, _heading
    if not _set_routing(drone, STOP):
        return
    p = drone.position
    base = _ground(drone, drone.routing_surface)
    y = max(p.y, base if base is not None else p.y) + START_RISE
    native.drone_configure(_num('speed', 6.0), _num('lift_speed', 3.0), _num('smoothing', 0.25), _num('boost', 2.0))
    if not native.drone_start(p.x, y, p.z):
        _log('the DLL has no drone flight')
        return
    _si, _drone, _active, _last_move = si, drone, True, (p.x, y, p.z)
    _heading = 0.0
    _log('flying {} for {} (start {:.1f} {:.1f} {:.1f}, ground {})'.format(drone, si, p.x, y, p.z, base))


def _end(sim, why, land=True):
    global _si, _drone, _active
    drone = _drone
    native.drone_stop()
    _active = False
    _si = _drone = None
    _log('ended: {}'.format(why))
    if land and drone is not None:
        try:
            _set_routing(drone, LAND)
        except Exception as e:
            _log('landing failed: {!r}'.format(e))


def _follow(drone, x, y, z):
    """Put the drone object at the flight point, turned the way it flies."""
    global _last_move, _heading
    last = _last_move
    if last is not None and math.sqrt((x - last[0]) ** 2 + (y - last[1]) ** 2 + (z - last[2]) ** 2) < MOVE_MIN:
        return
    if last is not None and math.hypot(x - last[0], z - last[2]) > 0.1:
        _heading = math.atan2(x - last[0], z - last[2])
    _last_move = (x, y, z)
    kwargs = {'translation': sims4.math.Vector3(x, y, z)}
    try:
        kwargs['orientation'] = sims4.math.angle_to_yaw_quaternion(_heading)
    except Exception:
        pass
    drone.move_to(**kwargs)


def update(sim):
    """Called by walk.py's tick for the first-person Sim. True while the drone is flown (walk.py then leaves the Sim be)."""
    global _seen, _last_run
    now = time.monotonic()
    if now - _last_run < TICK_MIN * 0.5:
        return _active
    _last_run = now
    try:
        if not _active:
            if not _on():
                _seen = 0.0
                return False
            found = _find(sim)
            if found is None:
                _seen = 0.0
                _scan(sim, now)
                return False
            if not _seen:
                _seen = now
            if now - _seen >= _num('start_delay', START_DELAY):
                _start(sim, found[0], found[1])
            return _active
        drone = _drone
        if drone is None or services.object_manager().get(drone.id) is None:
            _end(sim, 'the drone is gone', land=False)
            return False
        if _find(sim) is None:
            _end(sim, 'the interaction ended')
            return False
        value = _routing_value(drone)
        if value is not None and value not in (STOP,):
            _end(sim, 'the game changed the drone (state {})'.format(value), land=False)
            return False
        pose = native.drone_pose()
        if pose is None:
            _end(sim, 'the DLL stopped the flight', land=False)
            return False
        x, y, z, speed, land = pose
        if land:
            _end(sim, 'you landed it')
            return False
        ground = None
        try:
            ground = terrain.get_terrain_height(x, z, drone.routing_surface)
        except Exception:
            pass
        if ground is not None:
            native.drone_limits(ground + _num('min_height', 1.0), ground + _num('max_height', 60.0))
        _follow(drone, x, y, z)
        return True
    except Exception as e:
        global _errors
        _errors += 1
        if _errors <= 5:
            native._log('drone: update failed: {!r}'.format(e))
        if _active:
            try:
                _end(sim, 'error', land=False)
            except Exception:
                pass
        return False


def forget():
    """First person ended or the game stopped: nothing is flown any more."""
    global _active, _si, _drone, _seen
    if _active:
        native.drone_stop()
    _active = False
    _si = _drone = None
    _seen = 0.0
