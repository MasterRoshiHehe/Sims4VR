"""First person: what an interaction gives, without the interaction (docs/PASSIVE-ACTIONS.md).

The engine of passive actions. A source (tvwatch.py: looking at a TV) says
"the player is doing <affordance> with <object>, this strongly"; the engine
gives your Sim what that interaction's tuning gives while it runs, the way the
game does it, but nothing is queued, animated, routed or reserved:

- basic_content.periodic_stat_change (interactions/utils/statistic_element.py
  PeriodicStatisticChangeElement._start_statistic_gains / _add_operation_if_valid):
  continuous stats (needs, skills, most commodities) become one
  AutonomyModifier(statistic_modifiers={stat: rate}) on your Sim
  (sim.add_statistic_modifier), rates from the op's own get_value (traits,
  buffs and skill speed included; dynamic skill ops x 1/Skill.DYNAMIC_SKILL_INTERVAL).
  The rest (non-continuous stats, APPLY_AT_INTERVAL_ONLY / RETEST_ON_INTERVAL
  ops, operation_actions loot) is applied on a game-time alarm every
  alarm_interval sim minutes, as StatisticChangeHelper.apply does.
- basic_content.statistic_reduction_by_category (StatisticDecayByCategory):
  decay modifier on the commodity categories (TV: stressed moodlets fade faster).
- basic_content.conditional_actions: the stat-based exit conditions
  (StatisticCondition thresholds, TimeBasedCondition minimum time) end the
  activity like the game ends the interaction (fun full, a need near failing);
  it stays ended until the source lets go of it (you look away, then back).

The ops need an interaction to read participants, the resolver and
skill_loot_data from: a shadow instance of the affordance, made by
AffordanceObjectPair.interaction_factory and never queued or run.
PeriodicStatisticChangeElement itself is not used: it sends the interaction's
progress bar (send_current_progress) and suppresses skill bars.

Strength: 1.0 = doing it (all of it); below 1 = nearby (continuous rates
scaled, no alarm loot, no decay change, no skill unless [passive] nearby_skill).
Only your Sim gains (ops on other participants are skipped). Only in first
person: every activity ends when you leave it or follow another Sim.
Not done: whims/aspirations that count the interaction running, basic_extras
(buffs while running), mixer outcome loot.
Log: "passive_gain: ..." in sims4vr_python.log.
"""
import alarms
import clock
import services

from sims4vr import native

TICK_SECONDS = 1.0
FULL = 0.99  # strength from which an activity counts as really doing it

_owner = type('_Owner', (), {})()
_alarm = None
_logs = 0
_errors = 0
_activities = {}  # key -> _Activity


class _Activity:
    __slots__ = ('key', 'sim', 'affordance', 'target', 'strength', 'shadow', 'handle', 'rates',
                 'interval_ops', 'loot', 'loot_interval', 'loot_alarm', 'decayed', 'exits', 'started', 'done')

    def __init__(self, key, sim, affordance, target, strength):
        self.key, self.sim, self.affordance, self.target, self.strength = key, sim, affordance, target, strength
        self.shadow = None
        self.handle = None
        self.rates = {}
        self.interval_ops = []
        self.loot = []
        self.loot_interval = 1
        self.loot_alarm = None
        self.decayed = []
        self.exits = []  # [(conditions [(stat, threshold) or minutes], tests)]
        self.started = _minutes()
        self.done = None  # why it ended (exit condition), or None


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 300:
        native._log('passive_gain: ' + message)


def _note_error(what, e):
    global _errors
    _errors += 1
    if _errors <= 20:
        _log('{} failed: {!r}'.format(what, e))


def _number(key, default):
    try:
        return float(native.setting('passive', key, default))
    except (TypeError, ValueError):
        return default


def _minutes():
    try:
        return services.time_service().sim_now.absolute_minutes()
    except Exception:
        return 0.0


def _first_person(sim):
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def _name(thing):
    return getattr(thing, '__name__', None) or type(thing).__name__


def _tuned(factory, name, default=None):
    """A tuned value of a TunableFactory wrapper (factory._tuned_values.<name>)."""
    values = getattr(factory, '_tuned_values', None)
    if values is None:
        return default
    return getattr(values, name, default)


# --- public -----------------------------------------------------------------------------

def active(key):
    """True while the activity runs and has not ended on its own."""
    a = _activities.get(key)
    return a is not None and a.done is None


def start(key, sim, affordance, target, strength=1.0, **kwargs):
    """Begin (or update) the activity <key>: <sim> doing <affordance> on <target>.

    kwargs go to the shadow interaction (e.g. required_station for a stereo).
    Same key, affordance and target again: only the strength is updated.
    After the activity ended on its own (exit condition) it stays ended until
    stop(key)."""
    global _alarm
    if not _first_person(sim) or affordance is None:
        return False
    a = _activities.get(key)
    if a is not None and (a.affordance is not affordance or a.target is not target or a.sim is not sim):
        stop(key, 'changed to {}'.format(_name(affordance)))
        a = None
    if a is not None:
        if a.done is None and abs(a.strength - strength) > 0.01:
            a.strength = strength
            _apply(a)
        return a.done is None
    a = _Activity(key, sim, affordance, target, strength)
    try:
        a.shadow = _shadow(sim, affordance, target, kwargs)
        if a.shadow is None:
            return False
        _read(a)
    except Exception as e:
        _note_error('reading {}'.format(_name(affordance)), e)
        return False
    _activities[key] = a
    _apply(a)
    if _alarm is None:
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
    return True


def stop(key, why='stopped'):
    a = _activities.pop(key, None)
    if a is None:
        return
    _remove(a)
    _log('{}: {} on {} ends ({})'.format(key, _name(a.affordance), a.target, why))
    _stop_alarm_if_idle()


def stop_all(prefix='', why='stopped'):
    for key in [k for k in _activities if k.startswith(prefix)]:
        stop(key, why)


# --- hearing: volume x distance x walls -----------------------------------------------------

VOLUME_STATE = 15021  # AudioVolume (re/tuning): Low 15023, Medium 15024, High 15022, LoudMusic 163341
VOLUME_LOUDNESS = {15023: 0.6, 15024: 0.8, 15022: 1.0, 163341: 1.3}
DEFAULT_LOUDNESS = 0.8  # no volume state: as Medium
FULL_PART = 0.25  # share of the reach heard at full strength
_volume_state = None


def _volume(obj):
    global _volume_state
    if _volume_state is None:
        _volume_state = False
        try:
            import sims4.resources
            _volume_state = services.get_instance_manager(sims4.resources.Types.OBJECT_STATE).get(VOLUME_STATE) or False
        except Exception as e:
            _note_error('volume state', e)
    try:
        if _volume_state and obj.has_state(_volume_state):
            value = obj.get_state(_volume_state)
            return VOLUME_LOUDNESS.get(getattr(value, 'guid64', None), DEFAULT_LOUDNESS)
    except Exception:
        pass
    return DEFAULT_LOUDNESS


def _room(position, level):
    try:
        import build_buy
        return build_buy.get_room_id(services.current_zone_id(), position, level) or 0
    except Exception:
        return 0


FRONT = 0.4  # metres in front of the object its sound is taken from (a wall-mounted one's own spot is in the wall)


def _front_point(obj):
    """A point FRONT metres in front of the object, on its level (for distance and its room)."""
    try:
        import sims4.math
        t = obj.transform
        o = t.orientation
        f = o.transform_vector(sims4.math.Vector3.Z_AXIS())
        p = t.translation
        return sims4.math.Vector3(p.x + f.x * FRONT, p.y, p.z + f.z * FRONT)
    except Exception:
        return obj.position


def hearing(sim, obj, reach):
    """How well you hear obj, 0..1: its volume state x distance falloff x walls/floors between.

    reach = metres at which a High-volume source fades out (scaled by the volume:
    Low 0.6, Medium 0.8, High 1.0, Loud Music 1.3). Full strength within the
    first quarter of that, then a smooth (squared) fade. Another room on your
    level: x [passive] wall_factor; another level: x [passive] floor_factor."""
    try:
        import math
        loud = _volume(obj)
        p, t = sim.position, _front_point(obj)
        d = math.hypot(t.x - p.x, t.z - p.z)  # over the floor: a screen high on a wall is not farther
        x = d / max(0.1, reach * loud)
        if x >= 1.0:
            return 0.0
        fade = 1.0 if x <= FULL_PART else (1.0 - (x - FULL_PART) / (1.0 - FULL_PART)) ** 2
        if obj.level != sim.level:
            fade *= max(0.0, _number('floor_factor', 0.2))
        else:
            if _room(p, sim.level) != _room(t, obj.level):  # both outdoors (0, 0) = open air
                fade *= max(0.0, _number('wall_factor', 0.4))
        return max(0.0, min(1.0, fade * min(loud, 1.0)))
    except Exception as e:
        _note_error('hearing {}'.format(obj), e)
        return 0.0


# --- reading the affordance ------------------------------------------------------------------

def _shadow(sim, affordance, target, kwargs):
    """An instance of the affordance that is never queued: participants, resolver, skill data."""
    from interactions.aop import AffordanceObjectPair
    from interactions.context import InteractionContext
    from interactions.priority import Priority
    context = InteractionContext(sim, InteractionContext.SOURCE_PIE_MENU, Priority.Low)
    made = AffordanceObjectPair(affordance, target, affordance, None, **kwargs).interaction_factory(context)
    interaction = getattr(made, 'interaction', None)
    if not made or interaction is None:
        _log('{} on {}: no shadow interaction ({})'.format(_name(affordance), target, made))
        return None
    interaction._sims4vr_shadow = True
    return interaction


def _read(a):
    from interactions import ParticipantType
    from statistics.skill import Skill
    from statistics.statistic_ops import StatisticChangeOp, DynamicSkillLootOp
    from statistics.statistic_enums import PeriodicStatisticBehavior
    import snippets
    content = getattr(a.affordance, 'basic_content', None)
    if content is None:
        return
    shadow = a.shadow
    resolver = shadow.get_resolver()
    periodic = getattr(content, 'periodic_stat_change', None)
    if periodic is not None:
        for op in _tuned(periodic, 'operations', ()) or ():
            if isinstance(op, snippets.SnippetInstanceMetaclass):
                op = op.value
            _add_op(a, op, resolver, ParticipantType, Skill, StatisticChangeOp, DynamicSkillLootOp, PeriodicStatisticBehavior)
        actions = _tuned(periodic, 'operation_actions')
        if actions is not None:
            a.loot = list(getattr(actions, 'actions', ()) or ())
            a.loot_interval = getattr(actions, 'alarm_interval', None) or 1
    decay = getattr(content, 'statistic_reduction_by_category', None)
    if decay is not None:
        a.decayed = [(set(_tuned(decay, 'categories', ()) or ()), _tuned(decay, 'rate', 1))]
    for exit in getattr(content, 'conditional_actions', None) or ():
        _add_exit(a, exit)
    _log('{}: {} on {}: {} ({} at interval, {} loot every {} min, {} exit condition(s))'.format(
        a.key, _name(a.affordance), a.target,
        ', '.join('{} {:+.3f}/min'.format(_name(s), r) for s, r in a.rates.items()) or 'no rates',
        len(a.interval_ops), len(a.loot), a.loot_interval, len(a.exits)))



def _add_op(a, op, resolver, ParticipantType, Skill, StatisticChangeOp, DynamicSkillLootOp, PeriodicStatisticBehavior):
    """As PeriodicStatisticChangeElement._add_operation_if_valid, for your Sim only."""
    try:
        dynamic = isinstance(op, DynamicSkillLootOp)
        stat = op.get_stat(a.shadow)
        if stat is not None and not stat.continuous and (dynamic or isinstance(op, StatisticChangeOp)):
            a.interval_ops.append(op)
            return
        behavior = getattr(op, 'periodic_change_behavior', None)
        if behavior == PeriodicStatisticBehavior.APPLY_AT_INTERVAL_ONLY:
            a.interval_ops.append(op)
            return
        if not op.test_resolver(resolver):
            if behavior == PeriodicStatisticBehavior.RETEST_ON_INTERVAL:
                a.interval_ops.append(op)
            return
        if stat is None:
            return
        participants = a.shadow.get_participants(op.subject)
        if a.sim not in participants and a.sim.sim_info not in participants:
            return  # only your Sim gains
        rate = op.get_value(obj=a.sim, interaction=a.shadow, sims={a.sim})
        if dynamic:
            rate *= 1.0 / Skill.DYNAMIC_SKILL_INTERVAL
        if rate:
            a.rates[stat] = a.rates.get(stat, 0.0) + rate
    except Exception as e:
        _note_error('op {}'.format(op), e)


def _add_exit(a, exit):
    """Stat-based exit conditions that end the interaction (EXIT_NATURALLY / EXIT_CANCEL)."""
    try:
        from interactions import ParticipantType
        from interactions.utils.statistic_element import ConditionalInteractionAction, ConditionalActionRestriction
        from statistics.statistic_conditions import StatisticCondition, TimeBasedCondition
        import snippets
        exits = exit.value if isinstance(exit, snippets.SnippetInstanceMetaclass) else exit
        if not isinstance(exits, (list, tuple)):
            exits = (exits,)
        for ec in exits:
            if ec.interaction_action not in (ConditionalInteractionAction.EXIT_NATURALLY, ConditionalInteractionAction.EXIT_CANCEL):
                continue
            if ec.restrictions == ConditionalActionRestriction.AUTONOMOUS_ONLY:
                continue  # you chose to do it
            parts = []
            for factory in ec.conditions:
                condition = factory()
                if isinstance(condition, StatisticCondition):
                    if condition.who != ParticipantType.Actor or condition.stat is None:
                        parts = None
                        break
                    tracker = a.sim.get_tracker(condition.stat)
                    if tracker is None:
                        parts = None
                        break
                    parts.append((condition.stat, condition.create_threshold(tracker)))
                elif isinstance(condition, TimeBasedCondition):
                    parts.append(float(getattr(condition, '_interval', 0) or 0))
                else:
                    parts = None  # a kind we don't follow: this exit never fires here
                    break
            if parts and any(not isinstance(p, float) for p in parts):
                a.exits.append((parts, ec.tests))
    except Exception as e:
        _note_error('exit condition', e)


# --- applying --------------------------------------------------------------------------------

def _apply(a):
    """(Re)place the modifier for the current strength; loot alarm and decay at full strength."""
    from autonomy.autonomy_modifier import AutonomyModifier
    from statistics.skill import Skill
    _remove(a, keep_done=True)
    scale = max(0.0, _number('gain_scale', 1.0)) * a.strength
    full = a.strength >= FULL
    skills = full or native.setting_flag('passive', 'nearby_skill', False)
    mods = {}
    for stat, rate in a.rates.items():
        if not skills and isinstance(stat, type) and issubclass(stat, Skill):
            continue
        mods[stat] = rate * scale
    if mods:
        try:
            a.handle = a.sim.add_statistic_modifier(AutonomyModifier(statistic_modifiers=mods))
        except Exception as e:
            _note_error('modifier for {}'.format(a.key), e)
    if full:
        if (a.interval_ops or a.loot) and a.loot_alarm is None:
            a.loot_alarm = alarms.add_alarm(_owner, clock.interval_in_sim_minutes(a.loot_interval),
                                            lambda _h, a=a: _interval(a), repeating=True)
        for categories, rate in a.decayed:
            try:
                for commodity in a.sim.commodity_tracker.get_all_commodities():
                    if categories.intersection(set(commodity.get_categories())):
                        commodity.add_decay_rate_modifier(rate)
                        _decay_on.setdefault(a.key, []).append((commodity, rate))
            except Exception as e:
                _note_error('decay for {}'.format(a.key), e)
    _log('{}: strength {:.2f}: {}'.format(a.key, a.strength,
         ', '.join('{} {:+.3f}/min'.format(_name(s), r) for s, r in mods.items()) or 'nothing'))


_decay_on = {}  # key -> [(commodity, rate)]


def _remove(a, keep_done=False):
    if a.handle is not None:
        try:
            a.sim.remove_statistic_modifier(a.handle)
        except Exception as e:
            _note_error('removing modifier of {}'.format(a.key), e)
        a.handle = None
    if a.loot_alarm is not None:
        alarms.cancel_alarm(a.loot_alarm)
        a.loot_alarm = None
    for commodity, rate in _decay_on.pop(a.key, ()):
        try:
            commodity.remove_decay_rate_modifier(rate)
        except Exception:
            pass


def _interval(a):
    """As StatisticChangeHelper.apply plus the operation_actions loot, every alarm_interval."""
    try:
        if not _first_person(a.sim) or a.done is not None:
            return
        resolver = a.shadow.get_resolver()
        for op in a.interval_ops:
            if op.test_resolver(resolver):
                op.apply_to_resolver(resolver, skip_test=True)
        for loot in a.loot:
            loot.apply_to_resolver(resolver)
    except Exception as e:
        _note_error('interval of {}'.format(a.key), e)


def _exit_reached(a):
    minutes = _minutes() - a.started
    for parts, tests in a.exits:
        try:
            met = True
            for p in parts:
                if isinstance(p, float):
                    met = minutes >= p
                else:
                    stat, threshold = p
                    value = a.sim.get_stat_value(stat)
                    met = value is not None and threshold.compare(value)
                if not met:
                    break
            if met and tests and not tests.run_tests(a.shadow.get_resolver()):
                met = False
            if met:
                return ', '.join(_name(p[0]) for p in parts if not isinstance(p, float))
        except Exception as e:
            _note_error('exit check of {}'.format(a.key), e)
    return None


def _stop_alarm_if_idle():
    global _alarm
    if not _activities and _alarm is not None:
        alarms.cancel_alarm(_alarm)
        _alarm = None


@native.timed('passive gain tick', 0.004)
def _tick(_handle):
    for key, a in list(_activities.items()):
        try:
            if not _first_person(a.sim):
                stop(key, 'not in first person')
                continue
            if a.done is None:
                why = _exit_reached(a)
                if why:
                    a.done = why
                    _remove(a)
                    _log('{}: {} done ({} reached; again after you stop and start it)'.format(key, _name(a.affordance), why))
        except Exception as e:
            _note_error('tick {}'.format(key), e)
    _stop_alarm_if_idle()
