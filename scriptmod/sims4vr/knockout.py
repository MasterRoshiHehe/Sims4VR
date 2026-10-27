"""Knocked out by your slaps and punches (user 2026-10-26 22:03Z, step 1 of docs/RAGDOLL-KNOCKOUT.md).

Every slap or punch the DLL sees (armik.cpp knockBack: a foley event of kind 5, into = m/s into them,
along = 1 for a punch) adds to that Sim's daze: speed squared, a punch 1.5 times that. The daze halves
every [knockout] recover seconds. Past [knockout] score, or one blow at [knockout] one_blow m/s or
faster, the Sim is knocked out: unconscious on the floor (Get to Work's collapsed patient,
collapsedSim_Collapse 113296: no sleep, no snoring, no energy change; onlookers react to it) for
[knockout] minutes Sim minutes, then it gets up. [knockout] unconscious off, or without that pack: the
game's Energy Failure (sim_EnergyFailure 14029, BASE: falls asleep on the floor 20 Sim minutes).
foley.py's tick hands the events over (hit()) and calls tick() to wake them when the time is up.
"""

import time

import services

from sims4vr import native

ENERGY_FAILURE = 14029  # sim_EnergyFailure (interaction, BASE): asleep on the floor, 20 Sim minutes
COLLAPSED = 113296      # collapsedSim_Collapse (interaction, EP01): unconscious on the floor (180 Sim minutes as tuned)
PUNCH_WEIGHT = 1.5
KO_HOLD = 30.0          # seconds after a knockout its hits add nothing (it's on the floor)

_daze = {}  # sim id -> (daze, when)
_out_until = {}  # sim id -> time.monotonic() its knockout stops ignoring hits
_down = {}  # sim id -> (its knockout interaction's tuning id, Sim time it wakes, time.monotonic() pushed)
_settings = {}
_settings_at = -10.0
_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 60:
        native._log('knockout: ' + message)


def _error(where, e):
    global _errors
    _errors += 1
    if _errors <= 8:
        native._log('knockout: {} failed: {!r}'.format(where, e))


def _setting(key, default):
    try:
        return float(native.setting('knockout', key, str(default)))
    except (TypeError, ValueError):
        return default


def _refresh_settings(now):
    """[knockout] values, read again every 2 s (the debug menu changes them live)."""
    global _settings_at
    if now - _settings_at < 2.0:
        return
    _settings_at = now
    _settings['on'] = native.setting_flag('knockout', 'on', True)
    _settings['score'] = max(1.0, _setting('score', 40.0))
    _settings['one_blow'] = _setting('one_blow', 6.0)
    _settings['recover'] = max(0.5, _setting('recover', 8.0))
    _settings['unconscious'] = native.setting_flag('knockout', 'unconscious', True)
    _settings['minutes'] = max(1.0, _setting('minutes', 20.0))


def _collapse(npc):
    """Knocks `npc` out (its knockout interaction, first in its queue); True when it went in."""
    import clock
    from interactions.context import InteractionContext, QueueInsertStrategy
    from interactions.priority import Priority
    from sims4.resources import Types
    manager = services.get_instance_manager(Types.INTERACTION)
    ids = (COLLAPSED, ENERGY_FAILURE) if _settings['unconscious'] else (ENERGY_FAILURE,)
    for si in npc.si_state:
        if si.affordance.guid64 in ids:
            return True  # already down
    for aff_id in ids:
        affordance = manager.get(aff_id)
        if affordance is None:
            _log('no interaction {} (pack not installed?)'.format(aff_id))
            continue
        context = InteractionContext(npc, InteractionContext.SOURCE_SCRIPT, Priority.Critical,
                                     insert_strategy=QueueInsertStrategy.NEXT)
        result = npc.push_super_affordance(affordance, npc, context)
        if not result:
            _log("{} can't be knocked out with {}: {}".format(npc.full_name, affordance.__name__,
                                                           getattr(result, 'reason', None) or result))
            continue
        wake = services.time_service().sim_now + clock.interval_in_sim_minutes(_settings['minutes'])
        _down[npc.id] = (aff_id, wake, time.monotonic())
        _log('{} is knocked out ({}, {:.0f} Sim minutes)'.format(npc.full_name, affordance.__name__,
                                                                _settings['minutes']))
        return True
    return False


def tick():
    """Wakes knocked-out Sims when their [knockout] minutes are up (foley.py's tick, every 20 ms)."""
    if not _down:
        return
    try:
        from interactions.interaction_finisher import FinishingType
        sim_now = services.time_service().sim_now
        now = time.monotonic()
        for sim_id, (aff_id, wake, pushed) in list(_down.items()):
            sim = services.object_manager().get(sim_id)
            si = None
            if sim is not None:
                si = next((i for i in sim.si_state if i.affordance.guid64 == aff_id), None)
                if si is None:
                    si = next((i for i in sim.queue if i.affordance.guid64 == aff_id), None)
            if si is None or si.is_finishing:
                if si is None and now - pushed < 10.0:
                    continue  # still on its way in
                del _down[sim_id]
                _out_until.pop(sim_id, None)
                _log('{}: knockout ended by the game'.format(sim.full_name if sim is not None else sim_id))
                continue
            if sim_now >= wake:
                del _down[sim_id]
                _out_until.pop(sim_id, None)
                si.cancel(FinishingType.NATURAL, cancel_reason_msg='sims4vr: knockout over')
                _log('{} comes round'.format(sim.full_name))
    except Exception as e:
        _error('tick', e)


def hit(npc, hand, part, into, punch):
    """A slap or punch on `npc` (from foley.py's tick)."""
    try:
        now = time.monotonic()
        _refresh_settings(now)
        if npc is None or not _settings['on']:
            return
        if npc.id in _down or now < _out_until.get(npc.id, 0):
            return
        daze, when = _daze.get(npc.id, (0.0, now))
        daze *= 0.5 ** ((now - when) / _settings['recover'])
        daze += into * into * (PUNCH_WEIGHT if punch else 1.0)
        one_blow = _settings['one_blow'] > 0 and into >= _settings['one_blow']
        out = one_blow or daze >= _settings['score']
        _log('{} hand {} {} at {:.2f} m/s: daze {:.0f} of {:.0f}{}'.format(
            'right' if hand else 'left', 'punches' if punch else 'slaps', npc.full_name, into, daze,
            _settings['score'], ' (one blow)' if one_blow else ''))
        if not out:
            _daze[npc.id] = (daze, now)
            return
        _daze.pop(npc.id, None)
        if _collapse(npc):
            _out_until[npc.id] = now + KO_HOLD
        else:
            _daze[npc.id] = (daze, now)
    except Exception as e:
        _error('hit', e)
