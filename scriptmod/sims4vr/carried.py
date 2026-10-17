"""Ladders, spiral stairs and stairs that are objects, in first person: the
game carries you.

Straight stairs are a ramp you walk yourself (stairs.py). Anything else that
joins two levels (a ladder, spiral or other curved stairs, stairs that are an
object) you walk up to: standing at its foot or its top, push the stick
toward it and the game takes your Sim through it with its own animation, the
view in your Sim's eyes (walk.py lets the game have the Sim meanwhile). At the
far end you walk on yourself.

The trip is a go-here to the far end on the other level, which the game's
router takes through the portal. If it heads somewhere else instead (another
way up was shorter), it is cancelled.
Switch: [body] carried_stairs in sims4vr.ini.
"""
import math
import time

import services

from sims4vr import native, stairs

REFRESH = 10.0  # seconds between looking up the lot's ways up again
REACH = 0.8  # metres from a way's start at which pushing the stick toward it starts it
TOWARD = 0.3  # the stick must point at least this much (cosine) toward the way
LEVEL = 0.6  # metres between you and a start's floor that count as being at it
STRAY = 3.0  # metres the Sim may get from both ends before the trip is called off
TRIP_LIMIT = 30.0  # seconds at most a trip may take

_ways = []
_ways_time = -1e9
_ways_sig = None
_trip = None  # (interaction, way, started)
_last_start = 0.0
_enabled = None
_errors = 0
_logs = 0


class Way:
    __slots__ = ('kind', 'start', 'end', 'centre', 'h0')

    def __repr__(self):
        return '{} from {:.1f},{:.1f},{:.1f} to {:.1f},{:.1f},{:.1f}'.format(
            self.kind, self.start.position.x, self.h0, self.start.position.z,
            self.end.position.x, self.end.position.y, self.end.position.z)


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 60:
        native._log('carried: ' + message)


def _on():
    global _enabled
    if _enabled is None:
        _enabled = native.setting_flag('body', 'carried_stairs', True)
    return _enabled


def _kind(portal):
    from routing.portals.portal_data_dynamic_stairs import _PortalTypeDataDynamicStairs
    from routing.portals.portal_data_ladders import _PortalTypeDataLadders
    from routing.portals.portal_data_stairs import _PortalTypeDataStairs
    t = portal.traversal_type
    if isinstance(t, _PortalTypeDataLadders):
        return 'ladder'
    if isinstance(t, _PortalTypeDataDynamicStairs):
        return 'object stairs'
    if type(t) is _PortalTypeDataStairs:
        a, b = portal.there_entry, portal.there_exit
        if a is None or b is None:
            return None
        run = math.hypot(b.position.x - a.position.x, b.position.z - a.position.z) - \
            stairs.UP_START_OUT - stairs.UP_END_OUT
        rise = abs(stairs._height(b) - stairs._height(a))
        if run < 0.2 or rise > stairs.STEEP * run:  # what stairs.py leaves out
            return 'curved stairs'
    return None


@native.timed('listing ladders and stairs', 0.004)
def _build():
    found = []
    for obj in services.object_manager().portal_cache_gen():
        try:
            portals = obj.get_portal_instances()
        except Exception:
            continue
        for portal in portals:
            try:
                kind = _kind(portal)
            except Exception:
                kind = None
            if kind is None:
                continue
            for start, end in ((portal.there_entry, portal.there_exit), (portal.back_entry, portal.back_exit)):
                if start is None or end is None:
                    continue
                w = Way()
                w.kind = kind
                w.start, w.end = start, end
                p = obj.position
                w.centre = (p.x, p.z)
                w.h0 = stairs._height(start)
                found.append(w)
    return found


def ways():
    global _ways, _ways_time, _ways_sig, _errors
    now = time.monotonic()
    if now - _ways_time > REFRESH:
        _ways_time = now
        try:
            sig = stairs.portal_signature()
            if sig == _ways_sig:
                return _ways
            _ways_sig = sig
            found = _build()
            if len(found) != len(_ways):
                _log('{} ways between levels on the lot{}'.format(
                    len(found), ': ' + '; '.join(repr(w) for w in found[:8]) if found else ''))
            _ways = found
        except Exception as e:
            _errors += 1
            if _errors <= 5:
                native._log('carried: listing ladders and stairs failed: {!r}'.format(e))
    return _ways


def forget():
    global _ways, _ways_time, _ways_sig, _trip
    _ways = []
    _ways_time = -1e9
    _ways_sig = None
    _trip = None


def active():
    """A trip is under way: walk.py must not cancel it when you push the stick."""
    return _trip is not None


def _toward(stick, x, z, tx, tz):
    dx, dz = tx - x, tz - z
    d = math.hypot(dx, dz)
    if d < 0.05:
        return True
    return (stick[0] * dx + stick[1] * dz) / d >= TOWARD


def try_start(sim, x, y, z, stick, surface):
    """You push the stick (world direction stick[0], stick[1]) at (x, y, z):
    if that is toward a way up or down whose start you stand at, go."""
    global _trip, _last_start
    if _trip is not None or not _on() or time.monotonic() - _last_start < 3.0:
        return False
    best, best_d = None, REACH
    for w in ways():
        p = w.start.position
        d = math.hypot(p.x - x, p.z - z)
        if d >= best_d or abs(w.h0 - y) > LEVEL:
            continue
        if stairs.surface_key(w.start.routing_surface) != stairs.surface_key(surface):
            continue
        e = w.end.position
        if not (_toward(stick, x, z, e.x, e.z) or _toward(stick, x, z, w.centre[0], w.centre[1]) or
                _toward(stick, x, z, p.x, p.z)):
            continue
        best, best_d = w, d
    if best is None:
        return False
    _last_start = time.monotonic()
    try:
        import objects.terrain
        from server.pick_info import PickType
        from server_commands.sim_commands import CommandTuning, _build_terrain_interaction_target_and_context
        e = best.end
        target, context = _build_terrain_interaction_target_and_context(
            sim, e.position, e.routing_surface, PickType.PICK_TERRAIN, objects.terrain.TerrainPoint)
        result = sim.push_super_affordance(CommandTuning.TERRAIN_GOHERE_AFFORDANCE, target, context)
    except Exception as e:
        native._log('carried: starting {} failed: {!r}'.format(best, e))
        return False
    if not result:
        _log('the game would not take you along the {}: {}'.format(best.kind, result))
        return False
    _trip = (result.interaction, best, time.monotonic())
    _log('taking the {} ({})'.format(best.kind, best))
    return True


def update(sim):
    """Every tick: end the trip when its interaction is done; call it off if
    it heads somewhere else."""
    global _trip
    if _trip is None:
        return
    interaction, way, started = _trip
    try:
        done = interaction is None or interaction.is_finishing or getattr(interaction, 'has_been_reset', False)
        if done:
            _trip = None
            _log('trip ended at level {}'.format(getattr(sim.routing_surface, 'secondary_id', '?')))
            return
        p = sim.position
        a, b = way.start.position, way.end.position
        away = min(math.hypot(p.x - a.x, p.z - a.z), math.hypot(p.x - b.x, p.z - b.z))
        late = time.monotonic() - started > TRIP_LIMIT
        if away > STRAY or late:
            _trip = None
            interaction.cancel(_finishing_type(), 'VR: the trip went another way')
            _log('called the trip off ({})'.format('too long' if late else 'it went another way'))
    except Exception as e:
        _trip = None
        native._log('carried: following the trip failed: {!r}'.format(e))


def _finishing_type():
    from interactions.interaction_finisher import FinishingType
    return FinishingType.USER_CANCEL
