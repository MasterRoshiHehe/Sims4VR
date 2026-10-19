"""First person: "Follow me" / "Dismiss" on other Sims (user 2026-10-19 00:25Z).

A Sim's pie menu gets "Follow me" (only in first person, only for the Sim you
are in; god view never shows it). Picked, that Sim follows you everywhere
until you pick "Dismiss" on them, which takes its place in their pie.

While following they are held like Integrated Body Physics' "Freeze all
motor functions" (user 2026-10-19 01:11Z): between walks they stand still in
the same interaction (sim-standExclusive), everything else in their queue is
refused (mixers too) and the game's requests to leave wait until you dismiss
them. Nothing their autonomy or the game's situations would have them do runs; only what you do with them (a chat, a kiss, anything you pick
on them) does, and once that has played they follow you again. Needs failing
(Critical priority: wetting, passing out) still happen.

Built the game's own ways (checked 2026-10-19 in re/v121 and re/python128):
- Autonomy off: AutonomyComponent.skip_autonomy(si, True) puts a running
  interaction in _autonomy_skip_sis, and full autonomy doesn't run while that
  set has anything in it (_attempt_full_autonomy_gen: "Running SIs are
  preventing autonomy from running"). A follower gets our own token in it, so
  no autonomy setting is changed (nothing can stick in a save).
- Script pushes (situation jobs, errands) for a follower are refused in
  InteractionQueue.append, as scripted.py does for your own Sim.
- Walking to you: the game's social adjustment walk (the way a conversation
  brings its Sims together) into a circle FOLLOW_SPOT around you, standing,
  facing you, sent again as you walk on. v0.13.263 used Go Here, which the
  game refused every time (log 2026-10-19 00:5x: "Go Here refused").
- Follow me first stops everything they do (chats with others, seat, bed)
  and turns them to face you (user 00:54Z), then they walk with you. The routing formations the game uses for leashes
  (routing/formation/formation_type_follow.py) only move a slave while its
  master takes a game route, and your Sim never does in first person.

In a conversation with you, hint.py walks them with the group (seated
followers aren't left behind: they stand up and follow). Leaving first person
pauses following (god view plays as the game does); coming back resumes it.
Followers are kept for this game run only.

Switch: [body] follow_me (default on).
"""
import math
import time
import weakref

from sims4vr import hooks, native

CHECK_EVERY = 0.5  # seconds
FOLLOW_START = 2.2  # m: farther than this (or on another floor) they walk to you
FOLLOW_SPOT = 1.3  # m from you, on their side, where they walk to
RESEND_MOVED = 1.2  # m: you moved this far from where they were sent: sent again
RESEND_EVERY = 1.5  # s between two sends
LEVEL_APART = 2.0  # m up or down: another floor
GRACE = 20.0  # s after you pick something on a follower: everything they do passes
HOLD = (23835,)  # sim-standExclusive: what they "do" between walks (IBP's Freeze all motor functions holds the same)
GUID_FOLLOW = 0x5653464F4C4C4F57  # our interactions' ids (not in the game's tuning)
GUID_DISMISS = 0x56534449534D4953

_followers = {}  # Sim id -> dict(goal, sent, walk)
_picked = {}  # follower id -> when you last picked something on them
_allowed = weakref.WeakSet()  # their interactions let through because they were with you
_sending = False  # our own walk or hold is going into their queue
_leave_later = {}  # follower id -> 'leave' / 'now': the game asked them to go while following
_hold_aff = None
_follow_aff = None
_dismiss_aff = None
_wrapped = False
_paused = True
_last = 0.0
_logs = 0


class _Token:
    """What skip_autonomy keeps in the follower's _autonomy_skip_sis (it reads
    .source; the game's own entries are running interactions)."""

    def __init__(self):
        from interactions.context import InteractionSource
        self.source = InteractionSource.SCRIPT

    def __repr__(self):
        return 'VR Follow me'


_token = None


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 120:
        native._log('follow: ' + message)


def _on():
    return native.setting_flag('body', 'follow_me', True)


def _fp_sim():
    from sims4vr import body
    fp = body.following()
    if not fp:
        return None
    import services
    sim = services.object_manager().get(fp)
    return sim if getattr(sim, 'is_sim', False) else None


def _name(sim):
    try:
        info = sim.sim_info
        return '{} {}'.format(info.first_name, info.last_name).strip() or str(sim.id)
    except Exception:
        return str(getattr(sim, 'id', '?'))


def is_follower(sim):
    return getattr(sim, 'id', None) in _followers


def is_ours(affordance):
    return affordance is not None and affordance in (_follow_aff, _dismiss_aff)


def _can_follow(target, you):
    if not getattr(target, 'is_sim', False) or target is you:
        return False
    try:
        info = target.sim_info
        if not getattr(info, 'is_human', True):
            return False  # Go Here is a human walk
        from sims.sim_info_types import Age
        return info.age >= Age.CHILD
    except Exception:
        return True


# ---------------------------------------------------------------------------
# Following and dismissing


def _autonomy(sim, off):
    global _token
    try:
        if _token is None:
            _token = _Token()
        sim.skip_autonomy(_token, off)
    except Exception as e:
        _log('autonomy {} for {} failed: {!r}'.format('off' if off else 'back', _name(sim), e))


def _stop_what_they_do(sim, you, state=None):
    """What they are doing ends (user 2026-10-19 00:54Z: talking, sitting,
    sleeping, whatever it is): every interaction not with you, chats with
    other Sims included (they leave those groups), past must-run. The seat or
    bed they are in is left by the walk to you (a stand constraint with
    posture changes allowed, as the game's own social adjustment does)."""
    from interactions.interaction_finisher import FinishingType
    ended = []
    ours = (state or {}).get('walk')
    why = 'VR first person: they follow you'
    try:
        # v0.13.267 (user 01:11Z: a Sim with an autonomous interaction already
        # queued started it): everything queued and running ends at once, the
        # queue's walk too, the way IBP's Freeze all motor functions stops a Sim
        # (pose.py _stop_running).
        controller = getattr(sim.queue, 'transition_controller', None)
        owner = getattr(controller, 'interaction', None)
        if controller is not None and owner is not ours and not _with_you(owner, you):
            controller.cancel(FinishingType.USER_CANCEL, why, si_to_cancel=owner)
        items = list(sim.si_state) + list(sim.queue)
        if owner is not None:
            items.append(owner)
        seen = set()
        for si in items:
            if si is None or id(si) in seen or si.is_finishing or si is ours or si.affordance is _hold_aff:
                continue
            seen.add(id(si))
            if getattr(si, 'provided_posture_type', None) is not None or \
                    getattr(si.affordance, '_provided_posture_type', None) is not None or _with_you(si, you):
                continue
            si.cancel(FinishingType.USER_CANCEL, cancel_reason_msg=why, ignore_must_run=True, immediate=True)
            ended.append(getattr(si.affordance, '__name__', str(si)))
        for group in list(sim.get_groups_for_sim_gen()):
            if you not in group:
                group.remove(sim)
                ended.append('left {}'.format(str(group).split(' ')[0]))
    except Exception as e:
        _log('ending what {} does failed: {!r}'.format(_name(sim), e))
    return ended


def _source(name):
    from interactions.context import InteractionSource
    return getattr(InteractionSource, name, None)


def _come(sim, centre, radius, surface, face, state, now):
    """The game's social adjustment (Sim.execute_adjustment_interaction, as
    SocialGroup and hint._turn use it): a walk into a circle, standing, facing
    `face`. Returns the game's result."""
    import sims4.math
    from animation.posture_manifest_constants import STAND_CONSTRAINT_OUTER_PENALTY
    from interactions.constraints import Circle, Facing
    from interactions.priority import Priority
    from socials.group import SocialGroup
    _end_walk(state)
    spot = Circle(sims4.math.Vector3(*centre), radius, surface).intersect(
        Facing(target_position=sims4.math.Vector3(*face), facing_range=sims4.math.PI / 3)).intersect(
        STAND_CONSTRAINT_OUTER_PENALTY)
    _end_hold(sim)
    global _sending
    _sending = True
    try:
        result = sim.execute_adjustment_interaction(SocialGroup.SOCIAL_ADJUSTMENT_AFFORDANCE, spot, Priority.High,
                                                    name_override='Satisfy[vr_follow]', allow_posture_changes=True)
    finally:
        _sending = False
    walk = getattr(getattr(result, 'execute_result', None), 'interaction', None) if result else None
    if walk is not None:
        walk._sims4vr_follow = True
    state['walk'], state['sent'] = walk, now
    return result


def _hold_running(sim):
    try:
        return any(si.affordance is _hold_aff and not si.is_finishing
                   for si in sim.get_all_running_and_queued_interactions())
    except Exception:
        return False


def _hold(sim):
    """Between walks they stand still doing nothing (IBP's freeze: pose.py
    _push, the same interaction and context)."""
    global _sending
    if _hold_aff is None or _hold_running(sim):
        return
    from interactions.context import InteractionContext, QueueInsertStrategy
    from interactions.priority import Priority
    context = InteractionContext(sim, InteractionContext.SOURCE_SCRIPT_WITH_USER_INTENT, Priority.Critical,
                                 insert_strategy=QueueInsertStrategy.NEXT)
    _sending = True
    try:
        result = sim.push_super_affordance(_hold_aff, sim, context)
    finally:
        _sending = False
    if not result:
        _log('{}: holding still refused ({})'.format(_name(sim), getattr(result, 'reason', result)))


def _end_hold(sim):
    from interactions.interaction_finisher import FinishingType
    try:
        for si in list(sim.get_all_running_and_queued_interactions()):
            if si.affordance is _hold_aff and not si.is_finishing:
                si.cancel(FinishingType.USER_CANCEL, cancel_reason_msg='VR first person: follow walk')
    except Exception:
        pass


def _wrap_leaving():
    """While following, the game's requests to send them off are put off until
    you dismiss them (IBP pose.py wrap_leaving: SituationManager.make_sim_leave
    / make_sim_leave_now_must_run)."""
    from situations.situation_manager import SituationManager

    def wrap(name, how):
        def leave(original, self, sim, *args, **kwargs):
            try:
                if not _paused and getattr(sim, 'id', None) in _followers:
                    if _leave_later.get(sim.id) != 'now':
                        _leave_later[sim.id] = how
                    _log('{}: {} put off (they follow you)'.format(_name(sim), name))
                    return None
            except Exception as e:
                _log('leave check failed: {!r}'.format(e))
            return original(self, sim, *args, **kwargs)

        hooks.hook('follow', SituationManager, name, leave)

    wrap('make_sim_leave', 'leave')
    wrap('make_sim_leave_now_must_run', 'now')


def _leave_now_if_asked(sim):
    how = _leave_later.pop(sim.id, None)
    if how is None:
        return
    try:
        import services
        manager = services.get_zone_situation_manager()
        if how == 'now':
            manager.make_sim_leave_now_must_run(sim)
        else:
            manager.make_sim_leave(sim)
        _log('{}: leaves now (asked while following)'.format(_name(sim)))
    except Exception as e:
        _log('{}: leaving failed: {!r}'.format(_name(sim), e))


def start_following(target):
    you = _fp_sim()
    if target is None or you is None or not _can_follow(target, you):
        return
    _followers[target.id] = dict(goal=None, sent=0.0, walk=None)
    _picked.pop(target.id, None)
    _autonomy(target, True)
    state = _followers[target.id]
    ended = _stop_what_they_do(target, you, state)
    # They turn to face you where they are (standing up first), so you see they
    # follow you now; farther than FOLLOW_START, update() walks them over facing you.
    from sims4vr import reach
    x, y, z = reach._you(you)
    p = target.position
    result = _come(target, (p.x, p.y, p.z), 0.3, target.routing_surface, (x, y, z), state, time.monotonic())
    _log('{} follows you{}; turning to you: {}'.format(
        _name(target), ' (stopped: {})'.format(', '.join(ended)) if ended else '',
        'yes' if result else 'refused ({})'.format(getattr(result, 'reason', result))))


def dismiss(target, why='you dismissed them'):
    import services
    sim_id = getattr(target, 'id', target)
    state = _followers.pop(sim_id, None)
    _picked.pop(sim_id, None)
    sim = target if getattr(target, 'is_sim', False) else services.object_manager().get(sim_id)
    if sim is not None:
        _autonomy(sim, False)
        _end_walk(state)
        _end_hold(sim)
    _log('{} no longer follows you ({})'.format(_name(sim) if sim is not None else sim_id, why))
    if sim is not None:
        _leave_now_if_asked(sim)
    else:
        _leave_later.pop(sim_id, None)


def _end_walk(state):
    walk = (state or {}).get('walk')
    if walk is not None and not walk.is_finishing:
        try:
            from interactions.interaction_finisher import FinishingType
            walk.cancel(FinishingType.USER_CANCEL, cancel_reason_msg='VR first person: follow ended')
        except Exception:
            pass


# ---------------------------------------------------------------------------
# What a follower may do


def _with_you(si, you):
    """Something they do with you: you are its target, or it belongs to a
    social group you are in."""
    target = getattr(si, 'target', None)
    if target is you or getattr(target, 'sim_info', None) is getattr(you, 'sim_info', 0):
        return True
    group = getattr(si, 'social_group', None)
    try:
        return group is not None and you in group
    except Exception:
        return False


def _scene_with_you(you):
    """Your Sim is in a WickedWhims scene or between two of its steps: the
    scene moves its actors itself and followers are left to it. Holding a
    follower still and refusing the scene's pushes made WickedWhims find its
    actor off the animation and restart the scene every few seconds, with its
    fades (user 2026-10-19 02:15Z: eyes closed when an animation repeated; log:
    active_sex_instance reset by persistence._validate_sim_sex_interaction,
    only with a follower in the scene)."""
    from sims4vr import walk
    try:
        return walk._in_scene(you) is not None
    except Exception:
        return False


def _engaged(sim, you, now):
    """You picked something on them a moment ago: what comes of it passes."""
    return now - _picked.get(sim.id, -1e9) < GRACE


def _refused(interaction):
    """Why a follower may not do it, or None. Like IBP's freeze (user 01:11Z):
    nothing but our walk and hold, and what they do with you."""
    sim = getattr(interaction, 'sim', None)
    if _paused or _sending or getattr(sim, 'id', None) not in _followers:
        return None
    you = _fp_sim()
    if you is None or sim is you or _scene_with_you(you):
        return None
    affordance = getattr(interaction, 'affordance', None)
    if not getattr(interaction, 'is_super', True):
        # A mixer: only of what they do with you (sub-action autonomy picks
        # mixers for the Sim's running interaction: cleaning while standing).
        parent = getattr(interaction, 'super_interaction', None)
        if parent is not None and (parent in _allowed or _with_you(parent, you) or _with_you(interaction, you)):
            return None
        return 'they follow you (mixer)'
    if affordance is _hold_aff or getattr(interaction, '_sims4vr_follow', False):
        return None
    context = getattr(interaction, 'context', None)
    source = getattr(context, 'source', None)
    from interactions.context import InteractionSource
    from interactions.priority import Priority
    if getattr(context, 'priority', None) == Priority.Critical and source != InteractionSource.AUTONOMY:
        return None  # needs failing: involuntary
    if source in (InteractionSource.BODY_CANCEL_AOP, InteractionSource.CARRY_CANCEL_AOP,
                  InteractionSource.POSTURE_GRAPH, InteractionSource.VEHCILE_CANCEL_AOP):
        return None  # steps of a posture change the game plans for an allowed action
    if source == InteractionSource.SOCIAL_ADJUSTMENT:
        return None if _in_chat_with_you(sim, you) else 'they follow you (adjustment for another chat)'
    if _with_you(interaction, you) or _engaged(sim, you, time.monotonic()):
        _allowed.add(interaction)
        _end_hold(sim)
        return None
    if getattr(context, 'continuation_id', None) is not None and \
            any(getattr(si, 'id', None) == context.continuation_id for si in _allowed):
        _allowed.add(interaction)
        return None  # the next step of something they do with you
    return 'they follow you'


def _note_pick(interaction):
    """Something you picked on a follower: what comes of it passes."""
    from interactions.context import InteractionSource
    source = getattr(getattr(interaction, 'context', None), 'source', None)
    if source not in (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT):
        return
    target = getattr(interaction, 'target', None)
    if getattr(target, 'id', None) in _followers and getattr(interaction, 'sim', None) is _fp_sim() \
            and not is_ours(getattr(interaction, 'affordance', None)):
        _picked[target.id] = time.monotonic()


_refusals = [0]


def _wrap_queue_append():
    from event_testing.results import TestResult
    from interactions.interaction_finisher import FinishingType
    from interactions.interaction_queue import InteractionQueue

    def append(original, self, interaction, *args, **kwargs):
        why = None
        if _followers and not _sending:
            try:
                _note_pick(interaction)
                why = _refused(interaction)
            except Exception as e:
                _log('checking {} failed: {!r}'.format(interaction, e))
                why = None
        if why is None:
            return original(self, interaction, *args, **kwargs)
        _refusals[0] += 1
        if _refusals[0] <= 40:
            _log('{} for {}: refused ({})'.format(getattr(interaction.affordance, '__name__', interaction),
                                                 _name(interaction.sim), why))
        try:
            interaction.cancel(FinishingType.INTERACTION_QUEUE, cancel_reason_msg='VR first person: ' + why)
        except Exception:
            pass
        return TestResult(False, 'VR first person: ' + why)

    hooks.hook('follow', InteractionQueue, 'append', append)


# ---------------------------------------------------------------------------
# Walking to you


def _busy_with_you(sim, you):
    """Doing something with you right now: they stay for it."""
    for si in list(sim.get_all_running_and_queued_interactions()):
        if si.is_finishing or getattr(si, 'provided_posture_type', None) is not None:
            continue
        if si in _allowed or _with_you(si, you):
            return True
    try:
        for si in list(you.get_all_running_and_queued_interactions()):
            if not si.is_finishing and getattr(si, 'target', None) is sim and \
                    getattr(si, 'provided_posture_type', None) is None:
                return True
    except Exception:
        pass
    return False


def _in_chat_with_you(sim, you):
    try:
        return any(sim in group for group in you.get_groups_for_sim_gen())
    except Exception:
        return False


def _seated(sim):
    try:
        return not sim.posture.mobile
    except Exception:
        return False


def _walking(state):
    walk = state.get('walk')
    return walk is not None and not walk.is_finishing


def _follow(sim, you, px, py, pz, surface, now):
    state = _followers[sim.id]
    p = sim.position
    dx, dz = p.x - px, p.z - pz
    flat = math.hypot(dx, dz)
    far = flat if abs(p.y - py) < LEVEL_APART else float('inf')
    if getattr(getattr(sim, 'parent', None), 'is_sim', False):
        return  # carried
    if _busy_with_you(sim, you) or _engaged(sim, you, now) or _scene_with_you(you):
        _end_hold(sim)  # what you do with them plays; they follow again after
        return
    if far <= FOLLOW_START:
        if not _walking(state):
            _hold(sim)  # here with you: standing still, nothing else
        return
    if _in_chat_with_you(sim, you) and not _seated(sim):
        return  # hint.py walks the conversation to you
    goal = (px, py, pz)
    old = state.get('goal')
    if _walking(state) and old is not None and \
            (math.hypot(goal[0] - old[0], goal[2] - old[2]) < RESEND_MOVED or now - state['sent'] < RESEND_EVERY):
        return
    if not _walking(state) and now - state['sent'] < RESEND_EVERY:
        return
    ended = _stop_what_they_do(sim, you, state)
    result = _come(sim, goal, FOLLOW_SPOT, surface, goal, state, now)
    state['goal'] = goal
    if not result or ended or old is None:
        _log('{} walks to you ({:.1f} m{}){}{}'.format(
            _name(sim), flat, ', another floor' if far == float('inf') else '',
            '' if result else ': refused ({})'.format(getattr(result, 'reason', result)),
            ' (stopped: {})'.format(', '.join(ended)) if ended else ''))


def update(sim, px, py, pz):
    """walk.py, every tick in first person: the Sim you are in and where you are."""
    global _last
    if not _followers:
        return
    now = time.monotonic()
    if now - _last < CHECK_EVERY:
        return
    _last = now
    if not _on():
        for sim_id in list(_followers):
            dismiss(sim_id, 'Follow me switched off')
        return
    import services
    from sims4vr import walk
    manager = services.object_manager()
    surface = walk._surface_of(sim)
    for sim_id in list(_followers):
        try:
            other = manager.get(sim_id)
            if other is None:
                continue  # not on this lot (yet): kept
            if other is sim:
                dismiss(other, 'you are in that Sim now')
                continue
            _autonomy(other, True)  # again: a reset clears the game's skip list
            _follow(other, sim, px, py, pz, surface, now)
        except Exception as e:
            _log('following {} failed: {!r}'.format(sim_id, e))


# ---------------------------------------------------------------------------
# The pie options


def _make_affordance(label, guid, wanted):
    """Like claude.py's Open Claude: a copy of a plain immediate interaction
    with its own test, name and action. wanted(target): offered on it."""
    import services
    import sims4.resources
    from event_testing.results import TestResult
    from interactions.base.immediate_interaction import ImmediateSuperInteraction
    from interactions.utils.outcome import InteractionOutcomeNone
    from sims4.localization import LocalizationHelperTuning
    from sims4.utils import flexmethod
    from singletons import DEFAULT

    manager = services.get_instance_manager(sims4.resources.Types.INTERACTION)
    base = None
    for cls in list(manager.types.values()):
        try:
            if cls.__bases__[0] is not ImmediateSuperInteraction or getattr(cls, 'debug', False) or \
                    getattr(cls, 'cheat', False) or 'lock' in cls.__name__.lower():
                continue
        except (TypeError, AttributeError, IndexError):
            continue
        base = cls
        break
    if base is None:
        _log('no immediate interaction to build on; no {} option'.format(label))
        return None

    @flexmethod
    def test(cls, inst, target=DEFAULT, context=DEFAULT, **kwargs):
        try:
            if target is DEFAULT:
                target = inst.target
            if context is DEFAULT:
                context = inst.context
            you = _fp_sim()
            if you is None or context is None or context.sim is not you or not _on():
                return TestResult(False, 'VR first person only')
            if not wanted(target, you):
                return TestResult(False, 'not offered')
            return TestResult.TRUE
        except Exception as e:
            return TestResult(False, '{} test failed: {!r}'.format(label, e))

    @flexmethod
    def _get_name(cls, inst, target=DEFAULT, context=DEFAULT, **kwargs):
        return LocalizationHelperTuning.get_raw_text(label)

    def _run_interaction_gen(self, timeline):
        try:
            if guid == GUID_FOLLOW:
                start_following(self.target)
            else:
                dismiss(self.target)
        except Exception as e:
            _log('{} failed: {!r}'.format(label, e))
        return True
        yield

    attrs = {
        'test': test,
        '_get_name': _get_name,
        '_run_interaction_gen': _run_interaction_gen,
        'allow_autonomous': False,
        'allow_user_directed': True,
        'category': None,
        'category_on_forwarded': None,
        'guid64': guid,
        'outcome': InteractionOutcomeNone(),
        'basic_extras': (),
    }
    if hasattr(base, 'pie_menu_icon'):
        attrs['pie_menu_icon'] = None
    affordance = type('vr_' + label.replace(' ', ''), (base,), attrs)
    _log('{} built on {}'.format(label, base.__name__))
    return affordance


def _wrap_sim_affordances():
    """Sims list Follow me / Dismiss among their pie options. Sim has its own
    super_affordances (it skips GameObject's), a flexmethod (cls, inst, context)."""
    from sims.sim import Sim
    from sims4.utils import flexmethod
    raw = Sim.__dict__.get('super_affordances')
    if raw is None:
        _log('Sim.super_affordances not found; no Follow me option')
        return False

    @flexmethod
    def super_affordances(cls, inst, *args, **kwargs):
        yield from raw.__get__(inst, cls)(*args, **kwargs)
        try:
            if inst is None or _follow_aff is None:
                return
            # Only for the Sim you are in (autonomy asks every Sim all the time: stop early).
            context = args[0] if args else kwargs.get('context')
            from sims4vr import body
            fp = body.following()
            if not fp or getattr(getattr(context, 'sim', None), 'id', None) != fp or inst.id == fp:
                return
            yield _dismiss_aff if inst.id in _followers else _follow_aff
        except Exception:
            pass

    Sim.super_affordances = super_affordances
    return True


def start():
    """body.py: first person started."""
    global _follow_aff, _dismiss_aff, _wrapped, _paused, _hold_aff
    _paused = False
    if _hold_aff is None:
        try:
            import services
            import sims4.resources
            manager = services.get_instance_manager(sims4.resources.Types.INTERACTION)
            _hold_aff = next((a for a in (manager.get(i) for i in HOLD) if a is not None), None)
            _log('holding still with {}'.format(getattr(_hold_aff, '__name__', 'nothing (not found)')))
        except Exception as e:
            _log('hold interaction lookup failed: {!r}'.format(e))
    try:
        if not _wrapped:
            _follow_aff = _make_affordance('Follow me', GUID_FOLLOW,
                                           lambda t, you: _can_follow(t, you) and t.id not in _followers)
            _dismiss_aff = _make_affordance('Dismiss', GUID_DISMISS,
                                            lambda t, you: getattr(t, 'id', None) in _followers)
            if _follow_aff is not None and _dismiss_aff is not None and _wrap_sim_affordances():
                _wrap_queue_append()
                _wrap_leaving()
            _wrapped = True
    except Exception as e:
        _wrapped = True
        _log('no Follow me option: {!r}'.format(e))
    if _followers:
        import services
        manager = services.object_manager()
        for sim_id in _followers:
            other = manager.get(sim_id)
            if other is not None:
                _autonomy(other, True)
        _log('first person: {} follow you again'.format(len(_followers)))


def stop():
    """body.py: first person ended. Followers pause: god view plays as the game does."""
    global _paused
    _paused = True
    if not _followers:
        return
    import services
    manager = services.object_manager()
    for sim_id, state in _followers.items():
        other = manager.get(sim_id)
        if other is not None:
            _autonomy(other, False)
            _end_walk(state)
            _end_hold(other)
    _log('first person ended: {} followers paused'.format(len(_followers)))
