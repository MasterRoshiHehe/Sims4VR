"""First person: passive effects of objects around you (music playing; probe of the game's own).

The game already affects any Sim near objects through broadcasters: the
environment score (a TV or stereo that is on, plants, fire... change the mood
of Sims in line of sight) and the reaction broadcasters (loud TV/stereo, music
you like). Your Sim is a real Sim at your body, so those reach it by themselves.
This module (docs/PASSIVE-OBJECTS.md):

- Probe: logs what the game's environment score currently says about your Sim
  (the objects that score on you, the leading mood, positive/negative score)
  whenever it changes, so every case can be checked in sims4vr_python.log
  ("passive: ..."). Switch [passive] log.
- Music: a stereo that plays (its StereoChannel state is not off) in your room
  or within [passive] music_range metres on your level gives a little fun,
  [passive] music_rate per game minute, the way the TV trickle in tvwatch.py
  does for a TV. The game gives nothing for only being near a playing stereo.
  Switch [passive] music.

Only your Sim, only in first person (body.py starts and stops it). Nothing is
pushed onto your Sim: no interaction, no walking. Switch: [passive] on.
"""
import math

import alarms
import clock
import services

from sims4vr import native

TICK_SECONDS = 2.0
SCAN_EVERY = 5  # ticks between looks for stereos on the lot
STEREO_STATE = 15337  # the StereoChannel object state (re/tuning)
FUN_NAMES = ('motive_fun',)

_owner = type('_Owner', (), {})()
_alarm = None
_ticks = 0
_logs = 0
_errors = 0
_stereos = []
_state = None  # the StereoChannel state (False: not found)
_fun_type = None
_last_probe = None
_music_at = None  # game minute of the last music trickle
_music_obj = None


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 300:
        native._log('passive: ' + message)


def _note_error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        _log('{} failed: {!r}'.format(what, e))


def _number(key, default):
    try:
        return float(native.setting('passive', key, default))
    except (TypeError, ValueError):
        return default


def _first_person(sim):
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def start():
    global _alarm, _ticks, _music_at
    _ticks = 0
    _music_at = None
    if _alarm is None and native.setting_flag('passive', 'on', True):
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
        _log('on')


def stop():
    global _alarm, _last_probe, _music_at, _music_obj
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    _stereos[:] = []
    _last_probe = _music_at = _music_obj = None


# --- the game's own effects (probe) ---------------------------------------------------

def _probe(sim):
    global _last_probe
    try:
        from broadcasters.environment_score.environment_score_tuning import EnvironmentScoreTuning
        objects = sorted({str(getattr(getattr(o, 'definition', None), 'name', None) or o)
                          for o in sim._get_broadcasting_environment_score_objects_gen()})
        mood = getattr(sim._environment_score_commodity, '__name__', None)
        tracker = sim.commodity_tracker
        negative = tracker.get_value(EnvironmentScoreTuning.NEGATIVE_ENVIRONMENT_SCORING)
        positive = tracker.get_value(EnvironmentScoreTuning.POSITIVE_ENVIRONMENT_SCORING)
    except Exception as e:
        _note_error('probe', e)
        return
    snap = (tuple(objects), mood, round(negative or 0), round(positive or 0))
    if snap != _last_probe:
        _last_probe = snap
        _log('environment score on you: {} object(s) {}; mood {}; negative {} positive {}'.format(
            len(objects), objects[:8], mood, snap[2], snap[3]))


# --- music ----------------------------------------------------------------------------

def _stereo_state():
    global _state
    if _state is None:
        _state = False
        try:
            import sims4.resources
            _state = services.get_instance_manager(sims4.resources.Types.OBJECT_STATE).get(STEREO_STATE) or False
        except Exception as e:
            _note_error('stereo state', e)
        _log('stereo state: {}'.format(getattr(_state, '__name__', 'not found')))
    return _state or None


def _playing(obj, state):
    try:
        if not obj.has_state(state):
            return False
        value = obj.get_state(state)
        name = getattr(value, '__name__', '').lower()
        return bool(name) and '_off' not in name
    except Exception:
        return False


def _scan(state):
    found = []
    for obj in list(services.object_manager().values()):
        try:
            if not getattr(obj, 'is_sim', False) and obj.has_state(state):
                found.append(obj)
        except Exception:
            pass
    if [o.id for o in found] != [o.id for o in _stereos]:
        _log('{} stereo(s) on the lot'.format(len(found)))
    _stereos[:] = found


def _room(position, level):
    try:
        import build_buy
        return build_buy.get_room_id(services.current_zone_id(), position, level) or 0
    except Exception:
        return 0


def _music_near(sim, state, reach):
    p = sim.position
    room = None
    for obj in _stereos:
        try:
            if obj.level != sim.level or not _playing(obj, state):
                continue
            t = obj.position
            if math.hypot(t.x - p.x, t.z - p.z) <= reach:
                return obj
            if room is None:
                room = _room(p, sim.level)
            if room and _room(t, obj.level) == room:
                return obj
        except Exception as e:
            _note_error('music near {}'.format(obj), e)
    return None


def _fun():
    global _fun_type
    if _fun_type is None:
        _fun_type = False
        try:
            import sims4.resources
            manager = services.get_instance_manager(sims4.resources.Types.STATISTIC)
            for t in manager.types.values():
                if getattr(t, '__name__', '').lower() in FUN_NAMES:
                    _fun_type = t
                    break
        except Exception as e:
            _note_error('fun need', e)
    return _fun_type or None


def _music(sim):
    global _music_at, _music_obj
    state = _stereo_state()
    obj = _music_near(sim, state, _number('music_range', 6.0)) if state is not None and _stereos else None
    if obj is None:
        if _music_obj is not None:
            _log('no music playing nearby: it stops')
        _music_at = _music_obj = None
        return
    if obj is not _music_obj:
        _log('music from {} ({:.2f} fun a game minute)'.format(obj, _number('music_rate', 0.15)))
        _music_obj = obj
    now = services.time_service().sim_now.absolute_minutes()
    if _music_at is None or now < _music_at:
        _music_at = now
        return
    minutes = now - _music_at
    if minutes < 1.0:
        return
    _music_at = now
    fun = _fun()
    if fun is not None:
        sim.commodity_tracker.add_value(fun, _number('music_rate', 0.15) * min(minutes, 60.0))


@native.timed('passive tick', 0.006)
def _tick(_handle):
    global _ticks
    try:
        sim = services.get_active_sim()
        if not _first_person(sim):
            return
        _ticks += 1
        if native.setting_flag('passive', 'log', True):
            _probe(sim)
        if native.setting_flag('passive', 'music', True):
            state = _stereo_state()
            if state is not None and (_ticks % SCAN_EVERY == 1 or not _stereos and _ticks < 3):
                _scan(state)
            _music(sim)
    except Exception as e:
        _note_error('tick', e)
