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
- Music: a stereo that plays (its StereoChannel state is not off) that
  you can hear gives a little fun, scaled by how well you hear it
  (passive_gain.hearing: volume state, distance against [passive] music_range, walls, floors),
  [passive] music_rate per game minute, the way the TV trickle in tvwatch.py
  does for a TV. The game gives nothing for only being near a playing stereo.
  Switch [passive] music.
- Engine ([passive] music_engine, default on; docs/PASSIVE-ACTIONS.md): instead
  of that flat fun, what the stereo's own listen interaction gives
  (passive_gain.py: fun, the music moodlet commodity, Music Lover need...) at
  strength = how well you hear it. Dancing in real life to it (native.dance,
  dance.cpp: head bob + hands moving to a beat, not walking; [dance] energy,
  full_energy, rhythm, hearing) gives the stereo's dance interaction instead
  (fun, Dancing skill, Dance Fever...) at strength = how well you dance.
  Nothing is queued or animated.

Only your Sim, only in first person (body.py starts and stops it). Nothing is
pushed onto your Sim: no interaction, no walking. Switch: [passive] on.
"""

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
    _engine_stop('first person ended')
    _listen_cache.clear()
    _failed.clear()


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


def _loudest(sim, state):
    """The playing stereo you hear best: (obj, hearing 0..1) or (None, 0)."""
    best, heard = None, 0.0
    for obj in _stereos:
        try:
            if _playing(obj, state):
                h = _heard(sim, obj)
                if h > heard:
                    best, heard = obj, h
        except Exception as e:
            _note_error('music near {}'.format(obj), e)
    return (best, heard) if heard >= 0.02 else (None, 0.0)

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
    obj, heard = _loudest(sim, state) if state is not None and _stereos else (None, 0.0)
    if obj is None:
        if _music_obj is not None:
            _log('no music playing nearby: it stops')
        _music_at = _music_obj = None
        _engine_stop('no music you can hear')
        return
    if obj is not _music_obj:
        _log('music from {} (heard {:.0%})'.format(obj, heard))
        _music_obj = obj
    if native.setting_flag('passive', 'music_engine', True) and _engine_music(sim, obj, state, heard):
        _music_at = None
        return
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
        sim.commodity_tracker.add_value(fun, _number('music_rate', 0.15) * heard * min(minutes, 60.0))


# --- engine (passive_gain.py): the stereo's own listen / dance interaction, no interaction --------

HEAR_STEP = 0.02
_listen_cache = {}  # (object id, channel) -> (listen affordance, dance affordance) or None
_engine_keys = set()
_failed = set()  # (object id, affordance) the engine could not use
_dance_ticks = 0  # + consecutive ticks that look like dancing, - that don't
_dancing = False


def _choices(sim, obj, channel):
    """(listen affordance, dance affordance) of the stereo on this channel, as the game offers them."""
    key = (obj.id, getattr(channel, 'guid64', id(channel)))
    if key in _listen_cache:
        return _listen_cache[key]
    listen = dance = None
    try:
        from interactions.context import InteractionContext
        from interactions.priority import Priority
        from objects.components.types import STEREO_COMPONENT
        from objects.electronics.stereo import CancelOnStateChangeInteraction
        context = InteractionContext(sim, InteractionContext.SOURCE_PIE_MENU, Priority.Low)
        component = obj.get_component(STEREO_COMPONENT)
        if component is not None:
            for aop in component.component_potential_interactions_gen(context):
                if aop.test(context):
                    listen = (aop.affordance, dict(aop._kwargs))
                    break
        for affordance in obj.super_affordances(context):
            if not (isinstance(affordance, type) and issubclass(affordance, CancelOnStateChangeInteraction)):
                continue
            if 'dance' not in affordance.__name__.lower():
                continue
            for aop in affordance.potential_interactions(obj, context, required_station=channel):
                if aop.test(context):
                    dance = (aop.affordance, dict(aop._kwargs))
                    break
            if dance:
                break
        _log('{} on {}: listen {}, dance {}'.format(obj, getattr(channel, '__name__', channel),
             listen[0].__name__ if listen else None, dance[0].__name__ if dance else None))
    except Exception as e:
        _note_error('choices of {}'.format(obj), e)
    _listen_cache[key] = (listen, dance)
    return listen, dance


def _dance_level():
    """0..1 from your real body (native.dance: dance.cpp), with [dance] energy / rhythm; 0 when walking."""
    d = native.dance()
    if not d or d[3]:
        return 0.0
    energy, rhythm = d[0], d[1]
    start = max(0.01, _dnumber('energy', 0.25))
    full = max(start + 0.05, _dnumber('full_energy', 0.7))
    min_rhythm = _dnumber('rhythm', 0.35)
    if energy < start or rhythm < min_rhythm:
        return 0.0
    e = min(1.0, (energy - start) / (full - start))
    r = min(1.0, (rhythm - min_rhythm) / max(0.05, 1.0 - min_rhythm) * 2.0)
    return max(0.1, e * r)


def _dnumber(key, default):
    try:
        return float(native.setting('dance', key, default))
    except (TypeError, ValueError):
        return default


def _engine_music(sim, obj, state, heard):
    """Listening (strength = how well you hear it), or dancing to it (strength = how you dance). True if handled."""
    global _dance_ticks, _dancing
    from sims4vr import passive_gain
    try:
        channel = obj.get_state(state)
    except Exception:
        return False
    listen, dance = _choices(sim, obj, channel)
    level = _dance_level() if dance and native.setting_flag('dance', 'on', True) and heard >= _dnumber('hearing', 0.3) else 0.0
    _dance_ticks = (max(_dance_ticks, 0) + 1) if level > 0 else (min(_dance_ticks, 0) - 1)
    if not _dancing and _dance_ticks >= 2:
        _dancing = True
        _log('you dance to {} (level {:.2f}, {})'.format(obj, level, native.dance()))
    elif _dancing and _dance_ticks <= -2:
        _dancing = False
        _log('you stopped dancing')
    if _dancing and dance:
        _engine_stop('dancing instead', only='music')
        strength = max(0.1, round(level / 0.1) * 0.1) if level > 0 else 0.1
        if passive_gain.start('dance', sim, dance[0], obj, min(1.0, strength), **dance[1]):
            _engine_keys.add('dance')
        return True
    _engine_stop('not dancing', only='dance')
    if not listen:
        return False
    strength = round(heard / HEAR_STEP) * HEAR_STEP
    if strength <= 0:
        _engine_stop('too quiet', only='music')
        return True
    failed = (obj.id, listen[0])
    if failed in _failed:
        return False
    if passive_gain.start('music', sim, listen[0], obj, min(1.0, strength), **listen[1]):
        _engine_keys.add('music')
        return True
    if 'music' in _engine_keys:
        return True  # it ended on its own (fun full): no trickle either, as the game's listen would have stopped
    _failed.add(failed)  # the engine could not read it: the old trickle for this stereo
    return False


def _engine_stop(why, only=None):
    global _dancing, _dance_ticks
    keys = [only] if only else list(_engine_keys)
    for key in keys:
        if key in _engine_keys:
            _engine_keys.discard(key)
            try:
                from sims4vr import passive_gain
                passive_gain.stop(key, why)
            except Exception as e:
                _note_error('engine stop', e)
    if only is None:
        _dancing, _dance_ticks = False, 0


def _heard(sim, obj):
    """How well you hear the stereo (passive_gain.hearing: volume, distance against music_range, walls)."""
    try:
        from sims4vr import passive_gain
        return passive_gain.hearing(sim, obj, _number('music_range', 12.0))
    except Exception as e:
        _note_error('hearing', e)
        return 1.0


@native.timed('passive tick', 0.006)
def _tick(_handle):
    global _ticks
    try:
        sim = services.get_active_sim()
        if not _first_person(sim):
            if _engine_keys:
                _engine_stop('not in first person')
            return
        _ticks += 1
        if native.setting_flag('passive', 'log', True):
            _probe(sim)
        if native.setting_flag('passive', 'music', True):
            state = _stereo_state()
            if state is not None and (_ticks % SCAN_EVERY == 1 or not _stereos and _ticks < 3):
                _scan(state)
            _music(sim)
        elif _engine_keys:
            _engine_stop('music switched off')
    except Exception as e:
        _note_error('tick', e)
