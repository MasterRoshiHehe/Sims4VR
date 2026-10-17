"""Conversations in first person: the Sims you talk with follow you, and
you leave a conversation by cancelling it on your wrist's queue.

NPCs follow you (user 2026-10-16, replaces "take a hint"): while your Sim is
in a conversation, whoever started it, the other Sims in it follow you when
you walk off, whatever they are doing (standing, eating...), for as long as
it lasts. Built the game's own way (checked 2026-10-16 in the 1.128 decompile:
socials/group.py, interactions/base/rally_interaction.py, carry/
put_down_interactions.py, sims/sim.py of 1.121): when the nearest of them is
farther than [body] follow_distance metres (default 2.5, 0 = off; B menu,
Other Sims), the group is moved to you with SocialGroup.
try_relocate_around_focus(your Sim) (what a rally interaction does to bring
its group along: the group's spot becomes the space in front of you, on your
level) and each other Sim in it gets the game's social adjustment
(SocialGroup.execute_adjustment_interaction with posture changes allowed, as
putting a carried Sim down does), which walks them into the moved group.
Again every FOLLOW_EVERY seconds while you keep walking away. Your own Sim is
never walked: its part stays pinned where you are (scripted.py) and its own
social adjustments are refused. Anchored groups (a jig or an object the
group is placed around) stay where they are. Not while you walk to a Sim on
purpose (trail.py: a far social or mixer waits for you).

Leaving: only by cancelling the conversation on your wrist's queue
(hudinfo.cancel -> leave()): everything social between you and the Sims in
it ends, the way it did when you walked away before (below), and those Sims
leave you alone for LEAVE_ALONE seconds unless you pick a social with one of
them yourself. Walking away no longer ends a conversation. God view keeps
the game's conversations.

History (take a hint, now gone): v0.13.67 ended a conversation when you
walked [body] hint_distance away from everyone in it; v0.13.84, v0.13.108
(everyone on another floor) and v0.13.118 refined when. What ending one does
is still used for leaving:

v0.13.84 (Sims Forever thread): "you walked" now needs you to have moved at
least WALKED metres since someone was last close (a Sim walking off from you
while you sit no longer ends it), and nothing is ended while Sims 1 is on a
computer screen (cancelling the chat there stood your Sim up from the game).

v0.13.108 (user 2026-10-14 20:58Z: caught in a group chat without noticing,
then going upstairs to the computer the game walked the Sim around downstairs
and teleported it): the Sims in it were never near you, the chat was only ended
30 s later, and three direct leaves after that did nothing: the other Sims'
parts of it, with you as their target, kept you in the group and started it
again. Now everyone in it being on another floor for FLOOR_GRACE seconds ends
it (out of earshot, whoever started it); ending it also ends the other Sims'
parts with you; and the Sims you walked away from don't start a conversation
with you again for LEAVE_ALONE seconds unless they are within the distance
(scripted.py refuses it). A conversation you pick from the pie menu is yours.

v0.13.118 (user 2026-10-14 22:38Z: pulled into a conversation again): the same
far-away group (a protest chat with a toddler in it, 10 to 20 m off) started
it over three times, a minute or more apart; LEAVE_ALONE is now 5 minutes.

v0.13.130 (user 2026-10-15 01:33Z: caught in a group chat again, cancelling
did nothing; checked against the game's own code, socials/group.py,
interactions/social/social_super_interaction.py, interactions/base/
super_interaction.py and interaction_queue.py): ending it now removes every
social of yours with a Sim in it, running, queued (mixers too) or being walked
to (the queue's transition controller, which iterating the queue leaves out),
not only what is registered with the group. Yours end as a user cancel, so the
game itself locks those Sims out of starting another social with you (its
AutonomyModesTuning.LOCKOUT_TIME, 240 Sim minutes, set both ways here as well);
must-run ones too. Whatever is still on after GIVE_UP_AFTER is stopped hard
(kill, skipped where the game would reset the Sim to do it) and you leave the
group; still in it after another GIVE_UP_AFTER, the group itself is shut
down. While LEAVE_ALONE runs, socials between you and a Sim you walked away
from that slip through anyway (queued before, or made directly by its social,
which never passes the queue check) are ended while that Sim is out of
earshot (stopped hard if the cancel isn't taken), unless you picked one with
it yourself.

v0.13.159 (user 2026-10-15 18:54Z, hard cancel): "stopped hard" is now a
cancel that isn't held back (immediate), never kill(): kill closed the route
on the server without telling the client, so your Sim was left in a walk
animation. Still walked by the conversation after that, the walk itself is
stopped (halt.py).
"""
import time

from sims4vr import native

CHECK_EVERY = 0.5  # seconds
LEVEL_APART = 2.0  # metres up or down: on another floor (they follow you up or down)
GIVE_UP_AFTER = 2.0  # seconds between the steps of leaving: cancel, stop what is left hard, shut the group down
LEAVE_ALONE = 300.0  # seconds the Sims you left don't start talking with you again
FOLLOW_EVERY = 3.0  # seconds between moving a group to you while they are still on their way
SETTLE = 5.0  # seconds after a conversation starts before it follows you (they are still joining)
FOLLOW_MAX_EVERY = 12.0  # seconds: a group that didn't get closer is moved again at most this rarely
STUCK_AFTER = 8.0  # seconds: still not here after this, the group is moved and they are sent again
ON_THEIR_WAY = 1.0  # m: the group's spot this close to where it would be moved: they are coming already

_last = 0.0
_leaving = {}  # id(group) -> [when the last step went out, steps taken]
_swept = {}  # id(interaction) -> when _sweep cancelled it
_followed = {}  # id(group) -> when it was last moved to you
_first_seen = {}  # id(group) -> when your Sim was first seen in it
_every = {}  # id(group) -> [seconds between moves now, nearest when last moved]
_sent = {}  # Sim id -> [its adjustment walk to you, when, how far it was, group name] (outcome logged)
_this_check = set()  # Sim ids sent already in this check (a Sim in two groups with you goes once)
_skipped = set()  # (group id, why) logged once
_anchored = set()  # ids of anchored groups already logged (they stay where they are)
_left = {}  # Sim id -> when you left a conversation with it (leaves_you_alone)
_you = None  # where you were at the last check
_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 80:
        native._log('hint: ' + message)


def _limit():
    """[body] follow_distance: the Sims you talk with follow you once the
    nearest of them is farther than this (0 = off)."""
    try:
        return float(native.setting('body', 'follow_distance', '2.5'))
    except (TypeError, ValueError):
        return 2.5


def _apart(px, py, pz, other):
    p = other.position
    dx, dz = p.x - px, p.z - pz
    flat = (dx * dx + dz * dz) ** 0.5
    return flat if abs(p.y - py) < LEVEL_APART else float('inf')


def _social(si):
    from sims4vr import scripted
    try:
        return scripted._is_social(si)
    except Exception:
        return False


def _picked(si):
    """Picked by you from the pie menu."""
    from interactions.context import InteractionSource
    source = getattr(getattr(si, 'context', None), 'source', None)
    return source in (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT)


def _doing(sim):
    """Everything a Sim has on: running and queued interactions, mixers too,
    and the one its transition is walking it to (InteractionQueue.__iter__
    yields the running one and the buckets, not the transition controller's)."""
    items = list(sim.si_state) + list(sim.queue)
    controller = getattr(sim.queue, 'transition_controller', None)
    items.append(getattr(controller, 'interaction', None))
    seen = set()
    for si in items:
        if si is not None and id(si) not in seen:
            seen.add(id(si))
            yield si


def _mine(sim, group, others):
    """Your Sim's part in the conversation: what is registered with the group,
    whatever runs in it, and every social of yours with a Sim in it (running,
    queued or being walked to; mixers too)."""
    found = list(group.get_sis_registered_for_sim(sim) or ())
    ids = {id(si) for si in found}
    for si in _doing(sim):
        if id(si) in ids:
            continue
        parent = getattr(si, 'super_interaction', None)
        if getattr(si, 'social_group', None) is group or \
                (_social(si) and getattr(si, 'target', None) in others) or \
                (parent is not si and id(parent) in ids):
            found.append(si)
            ids.add(id(si))
    return found


def _theirs(sim, others):
    """The other Sims' socials with you (running, queued or being walked to):
    their part of it keeps you in the group and starts it again."""
    for other in others:
        try:
            for si in _doing(other):
                if getattr(si, 'target', None) is sim and _social(si):
                    yield si
        except Exception as e:
            _log('reading what {} does failed: {!r}'.format(other, e))


def _end(si, ended, finishing, hard=False):
    """Cancel it, past must-run (SuperInteraction._cancel's ignore_must_run);
    hard: at once, not held back while a must-run posture change or a social
    mixer plays (_cancel_eventually's immediate). v0.13.159: no kill() any
    more: it closed a route on the server without telling the client, which
    left the walk animation playing (halt.py)."""
    try:
        if si.is_finishing:
            return
        done = si.cancel(finishing, cancel_reason_msg='VR first person: you walked away', ignore_must_run=True,
                         immediate=hard)
        ended.append('{}{}'.format(si, (' (at once)' if hard else '') if done else ' (cancel not taken yet)'))
    except Exception as e:
        _log('ending {} failed: {!r}'.format(si, e))


def _lock_out(sim, others):
    """What the game does when you cancel a social yourself
    (SocialSuperInteraction._cancel): the two Sims are locked out of each other
    for autonomy (autonomy_modes: "Sim has lockout for this object")."""
    try:
        from autonomy.autonomy_modes_tuning import AutonomyModesTuning
        minutes = AutonomyModesTuning.LOCKOUT_TIME
    except Exception:
        minutes = 240
    for other in others:
        try:
            other.add_lockout(sim, minutes)
            sim.add_lockout(other, minutes)
        except Exception as e:
            _log('locking {} out failed: {!r}'.format(other, e))


def _end_all(sim, group, others, hard=False):
    from interactions.interaction_finisher import FinishingType
    socials = getattr(FinishingType, 'SOCIALS', FinishingType.USER_CANCEL)
    ended = []
    for si in _mine(sim, group, others):
        _end(si, ended, FinishingType.USER_CANCEL, hard)
    for si in list(_theirs(sim, others)):
        _end(si, ended, socials, hard)
    return ended


def _leave(sim, group, others, now, why):
    """Out of the conversation for good, step by step (GIVE_UP_AFTER apart)
    while you are still in the group: cancel everything social between you
    and its Sims; then stop what is left hard and leave the group; then shut
    the group down."""
    key = id(group)
    step = _leaving.get(key)
    if step is not None:
        if now - step[0] < GIVE_UP_AFTER or step[1] >= 3:
            return
        step[0] = now
        step[1] += 1
        if step[1] == 2:
            ended = _end_all(sim, group, others, hard=True)
            try:
                group.remove(sim)
            except Exception as e:
                _log('leaving {} failed: {!r}'.format(group, e))
            from sims4vr import halt
            if halt.walked(sim):
                halt.walking(sim, 'VR first person: still walked by {} after you walked away'.format(group))
            _log('still in {} after the cancel: stopped what was left and left it ({})'.format(
                group, ', '.join(ended) or 'nothing left running'))
        else:
            try:
                from interactions.interaction_finisher import FinishingType
                group.shutdown(getattr(FinishingType, 'SOCIALS', FinishingType.USER_CANCEL))
                _log('still in {}: shut the group down'.format(group))
            except Exception as e:
                _log('shutting {} down failed: {!r}'.format(group, e))
        return
    _leaving[key] = [now, 1]
    for other in others:
        _left[other.id] = now
    _lock_out(sim, others)
    ended = _end_all(sim, group, others)
    _log('{}: the conversation ends ({})'.format(why, ', '.join(ended) or 'nothing to cancel'))


def _sweep(sim, now):
    """Socials between you and a Sim you left that got through anyway are
    ended (LEAVE_ALONE), unless you picked one with it yourself (forget()
    then took it off the list)."""
    if not _left:
        return
    import services
    from interactions.interaction_finisher import FinishingType
    socials = getattr(FinishingType, 'SOCIALS', FinishingType.USER_CANCEL)
    manager = services.object_manager()
    ended = []

    def end(si, finishing):
        tried = _swept.get(id(si))
        if tried is None:
            _swept[id(si)] = now
            _end(si, ended, finishing)
        elif now - tried >= GIVE_UP_AFTER:
            _swept[id(si)] = float('inf')  # once
            _end(si, ended, finishing, hard=True)

    for other_id, when in list(_left.items()):
        if now - when > LEAVE_ALONE:
            continue
        other = manager.get(other_id)
        if other is None:
            continue
        for si in list(_doing(sim)):
            if getattr(si, 'target', None) is other and _social(si) and not _picked(si):
                end(si, FinishingType.USER_CANCEL)
        for si in list(_doing(other)):
            if getattr(si, 'target', None) is sim and _social(si):
                end(si, socials)
    if len(_swept) > 200:
        _swept.clear()
    if ended:
        _log('Sims you left: ended {}'.format(', '.join(ended)))


def leaves_you_alone(other, you):
    """scripted.py, for a social between you and another Sim that you didn't
    pick: True for LEAVE_ALONE seconds after you left a conversation with
    that Sim (wherever it is now; v0.13.185: was only while out of earshot)."""
    key = getattr(other, 'id', None)
    when = _left.get(key)
    if when is None:
        return False
    if time.monotonic() - when > LEAVE_ALONE:
        del _left[key]
        return False
    return True


def forget(other):
    """scripted.py: you picked a social with that Sim yourself."""
    _left.pop(getattr(other, 'id', None), None)


def leave(sim, interaction, why):
    """hudinfo.py, first person: you cancelled a social on your wrist's queue.
    That is how you leave a conversation: every conversation it belongs to
    (its group, or the groups with its target in them) ends for you the way
    _leave does it. True when it was one."""
    try:
        if interaction is None or not _social(interaction):
            parent = getattr(interaction, 'super_interaction', None)
            if parent is None or not _social(parent):
                return False
            interaction = parent
        target = getattr(interaction, 'target', None)
        own = getattr(interaction, 'social_group', None)
        now = time.monotonic()
        found = False
        for group in list(sim.get_groups_for_sim_gen()):
            others = [other for other in group if other is not sim]
            if group is own or (target is not None and target in others):
                found = True
                _leaving.pop(id(group), None)
                _leave(sim, group, others, now, why)
        if not found and getattr(target, 'is_sim', False) and target is not sim:
            found = True  # not in a group yet (still on its way): end it with that Sim
            _left[target.id] = now
            _lock_out(sim, [target])
            ended = []
            from interactions.interaction_finisher import FinishingType
            _end(interaction, ended, FinishingType.USER_CANCEL)
            for si in list(_theirs(sim, [target])):
                _end(si, ended, getattr(FinishingType, 'SOCIALS', FinishingType.USER_CANCEL))
            _log('{}: the conversation with {} ends ({})'.format(why, target, ', '.join(ended) or 'nothing to cancel'))
        return found
    except Exception as e:
        _log('leaving after {} failed: {!r}'.format(interaction, e))
        return False


def _on_purpose(sim):
    """You are walking to a Sim on purpose (trail.py: a far social or mixer
    waits for you): the conversation doesn't come to you meanwhile."""
    try:
        from sims4vr import trail
        return trail.holding(sim) or trail._walk_to is not None
    except Exception:
        return False


def _name(sim):
    try:
        info = sim.sim_info
        name = '{} {}'.format(info.first_name, info.last_name).strip()
        return name or str(sim.id)
    except Exception:
        return str(getattr(sim, 'id', '?'))


def _release(sim, others):
    """v0.13.189 (log 2026-10-17: they never moved): a social you walked to
    (scripted._you_walk) keeps the other Sim where it stood for as long as it
    runs, so the adjustment had nowhere to take them. Let them go."""
    for other in others:
        for si in list(_doing(other)) + list(_doing(sim)):
            if getattr(si, '_sims4vr_you_walk', False) and not getattr(si, '_sims4vr_free', False):
                si._sims4vr_free = True
                try:
                    si.refresh_constraints()
                    if si.running:
                        # The posture state keeps the constraint it was given
                        # (SuperInteraction.apply_posture_state); give it the new one.
                        si.apply_posture_state(si.sim.posture_state)
                except Exception as e:
                    _log('refreshing {} failed: {!r}'.format(si, e))
                _log('{}: {} may move again (the conversation follows you)'.format(si, _name(other)))


def _adjusting(other):
    """A social adjustment runs on that Sim (walking into the group)."""
    try:
        from interactions.context import InteractionContext
        running = other.queue.running
        return running is not None and running.context.source == InteractionContext.SOURCE_SOCIAL_ADJUSTMENT
    except Exception:
        return False


def _stale_adjustments(other):
    """Social adjustments still waiting in their queue for the group's old
    spot (the queue holds one, and it never ran in the 2026-10-17 log): cancel
    them so the new one goes in. Returns how many."""
    count = 0
    try:
        from interactions.interaction_finisher import FinishingType
        for si in list(getattr(other.queue, '_social_adjustment', ()) or ()):
            if not si.is_finishing:
                si.cancel(FinishingType.SOCIALS, cancel_reason_msg='VR first person: the conversation moved to you')
                count += 1
    except Exception as e:
        _log('clearing the old adjustment of {} failed: {!r}'.format(_name(other), e))
    return count


def _set_in_place(other):
    """Seated, on a treadmill or any other posture that holds them in place
    (not carried: that one comes along with whoever carries it)."""
    try:
        if getattr(getattr(other, 'parent', None), 'is_sim', False):
            return False
        return not other.posture.mobile
    except Exception:
        return False


def _posture(other):
    try:
        return getattr(other.posture, 'posture_type', other.posture).__name__
    except Exception:
        return str(getattr(other, 'posture', '?'))


def _cut_mixers(group, sims):
    """v0.13.202 (user 2026-10-17 03:53Z: they often play out their current
    animation before they start walking): a social mixer isn't interruptible
    (MixerInteraction.interruptible is False unless tuned), so the walk to you
    waited in their queue until it ended. The game itself ends a group's
    interactions with FinishingType.SOCIALS when the group moves
    (SocialGroup.try_relocate_around_focus); here the mixer playing in the
    group is ended the same way. The conversation (its super interaction)
    stays, so it goes on once they are with you. Returns their names."""
    cut = []
    try:
        from interactions.interaction_finisher import FinishingType
        for s in sims:
            running = getattr(s.queue, 'running', None)
            if running is None or running.is_super or running.is_finishing:
                continue
            if getattr(running, 'social_group', None) is not group:
                continue
            running.cancel(FinishingType.SOCIALS, cancel_reason_msg='VR first person: the conversation walks with you')
            cut.append('{} ({})'.format(_name(s), getattr(running.affordance, '__name__', '?')))
    except Exception as e:
        _log('ending the mixer playing in {} failed: {!r}'.format(group, e))
    return cut


def _follow(sim, group, others, px, py, pz, now, limit):
    """The Sims in a conversation with you follow you (see the module text)."""
    nearest = min(_apart(px, py, pz, other) for other in others)
    every = _every.setdefault(id(group), [FOLLOW_EVERY, None])
    if nearest <= limit:
        every[0], every[1] = FOLLOW_EVERY, None
        return
    if now - _first_seen.setdefault(id(group), now) < SETTLE:
        return
    if now - _followed.get(id(group), 0.0) < every[0]:
        return
    # v0.13.196 (user 2026-10-17 02:30Z: after a chat the Sim ran through
    # walls in a walk animation): the group was moved while the game was still
    # lining your Sim up for the chat, so its transition was planned again
    # every move and each plan sent the client another walk for your Sim.
    # Never while the game moves your Sim.
    try:
        from sims4vr import halt
        if getattr(sim.queue, 'transition_controller', None) is not None or halt.walked(sim):
            return
    except Exception:
        pass
    if _on_purpose(sim):
        _skip(group, 'you are walking to a Sim you picked something with')
        return
    # v0.13.210 (user 2026-10-17 04:56Z): a Sim held in place by what they are
    # doing (seated, on a treadmill: a body posture that isn't mobile, the
    # game's own test in SocialGroup._could_sim_route) doesn't follow; walking
    # away from them ends the conversation, as cancelling it does.
    # Not while you are seated yourself (a long dining table).
    stay = [other for other in others if _set_in_place(other)]
    if stay and len(stay) == len(others) and not _set_in_place(sim):
        _leave(sim, group, others, now, 'you walked away from {} ({})'.format(
            ', '.join(_name(other) for other in stay), ', '.join(_posture(other) for other in stay)))
        return
    if getattr(group, 'anchor', None) is not None:
        _skip(group, 'placed around {}'.format(group.anchor))
        return
    if all(other.id in _this_check for other in others):
        return  # sent along with another conversation they share with you
    try:
        spot = group.position
        radius = group.group_radius
    except Exception:
        spot, radius = None, 1.0
    if spot is not None and abs(spot.y - py) < LEVEL_APART and \
            ((spot.x - px) ** 2 + (spot.z - pz) ** 2) ** 0.5 <= radius + ON_THEIR_WAY and \
            any(other.queue.has_adjustment_interaction() or _adjusting(other) for other in others) and \
            now - _followed.get(id(group), 0.0) < STUCK_AFTER:
        return  # moved to you already; they are on their way
    if every[1] is not None and nearest >= every[1] - 1.0:
        every[0] = min(every[0] * 2, FOLLOW_MAX_EVERY)  # didn't get closer: back off (each move costs a frame hitch)
    elif every[1] is not None:
        every[0] = FOLLOW_EVERY
    every[1] = nearest
    _followed[id(group)] = now
    _release(sim, others)
    from interactions.priority import Priority
    if not group.try_relocate_around_focus(sim, priority=Priority.High):
        # It only says True when the group's spot is next to a Sim or an object
        # (used_object); the spot is moved either way.
        _log('{}: moved to you without a Sim or object to stand around'.format(group))
    cut = _cut_mixers(group, [sim] + list(others))
    coming = []
    for other in others:
        try:
            if getattr(getattr(other, 'parent', None), 'is_sim', False):
                continue  # carried: comes along with whoever carries it
            if other.id in _this_check:
                continue
            if other in stay:
                coming.append('{} stays ({})'.format(_name(other), _posture(other)))
                continue
            _this_check.add(other.id)
            stale = _stale_adjustments(other)
            result = group.execute_adjustment_interaction(other, force_allow_posture_changes=True)
            if result:
                # Registered with the group as the game does (_consider_adjusting_sim),
                # so the next move to you cancels it (try_relocate_around_focus).
                try:
                    walk = result.execute_result.interaction
                    group._adjustment_interactions.add(walk)
                    _sent[other.id] = [walk, now, _apart(px, py, pz, other), str(group).split(' ')[0]]
                except Exception:
                    pass
            coming.append('{}{}{}'.format(_name(other), ' (replaced {} stuck)'.format(stale) if stale else '',
                                          '' if result else ' (refused: {})'.format(getattr(result, 'reason', result))))
        except Exception as e:
            _log('bringing {} along failed: {!r}'.format(other, e))
    _log('{} follows you (nearest {:.1f} m, limit {:.1f} m): {}{}'.format(
        group, nearest, limit, ', '.join(coming) or 'nobody to send',
        '; mixer ended so they walk at once: {}'.format(', '.join(cut)) if cut else ''))


TURN_ANGLE = 60.0  # degrees: facing further away from you than this, they turn to you
TURN_NEAR = 0.8  # m: closer than this they don't turn (you are right at them)
TURN_EVERY = 2.0  # s between two turns of the same Sim
_turned = {}  # Sim id -> when they were last turned to you
_turn_logs = 0


def _busy_elsewhere(other, group):
    """Doing something other than this conversation right now (a turn
    would cancel it): only the chat itself or nothing may be running."""
    running = getattr(other.queue, 'running', None)
    return running is not None and getattr(running, 'social_group', None) is not group


def _turn(sim, group, others, px, py, pz, now):
    """v0.13.219 (user 2026-10-17 12:01Z: with no talking while walking,
    at least turn towards me when I walk away): a Sim in the conversation who
    stays where they are turns to face you, the game's way: a social
    adjustment (Sim.execute_adjustment_interaction, as SocialGroup does) to a
    spot right where they stand, facing you (Facing, as the group's own
    constraint uses). Not while they walk to you, sit or do something else,
    and their head already follows you (the game's social focus)."""
    global _turn_logs
    import math
    import sims4.math
    from interactions.constraints import Circle, Facing
    from interactions.priority import Priority
    for other in others:
        try:
            if other.id in _this_check or now - _turned.get(other.id, 0.0) < TURN_EVERY:
                continue
            if getattr(getattr(other, 'parent', None), 'is_sim', False) or _set_in_place(other):
                continue
            p = other.position
            dx, dz = px - p.x, pz - p.z
            far = math.hypot(dx, dz)
            if far < TURN_NEAR or abs(p.y - py) >= LEVEL_APART:
                continue
            f = other.forward
            cos = (f.x * dx + f.z * dz) / (math.hypot(f.x, f.z) * far or 1.0)
            off = math.degrees(math.acos(max(-1.0, min(1.0, cos))))
            if off <= TURN_ANGLE:
                continue
            if other.queue.has_adjustment_interaction() or _adjusting(other) or _busy_elsewhere(other, group):
                continue
            if getattr(other.queue, 'transition_controller', None) is not None:
                continue
            _turned[other.id] = now
            spot = Circle(p, 0.3, other.routing_surface).intersect(
                Facing(target_position=sims4.math.Vector3(px, p.y, pz), facing_range=sims4.math.PI / 3))
            result = other.execute_adjustment_interaction(group.SOCIAL_ADJUSTMENT_AFFORDANCE, spot, Priority.High)
            if result:
                try:
                    group._adjustment_interactions.add(result.execute_result.interaction)
                except Exception:
                    pass
            _turn_logs += 1
            if _turn_logs <= 30:
                native._log('hint: {} turns to you ({:.0f} deg away, {:.1f} m){}'.format(
                    _name(other), off, far, '' if result else ' refused: {}'.format(getattr(result, 'reason', result))))
        except Exception as e:
            _log('turning {} to you failed: {!r}'.format(other, e))


def _skip(group, why):
    key = (id(group), why)
    if key not in _skipped:
        _skipped.add(key)
        _log('{}: not following you ({})'.format(group, why))


def _outcomes(px, py, pz, now):
    """v0.13.190 (user 2026-10-17 01:52Z: follows, but far from with every
    Sim or every time): log how each walk to you ended, for the next fix."""
    if not _sent:
        return
    import services
    manager = services.object_manager()
    for other_id, (walk, when, was, group) in list(_sent.items()):
        other = manager.get(other_id)
        finished = getattr(walk, 'is_finishing', True) or getattr(walk, 'has_been_canceled', False)
        if other is None:
            del _sent[other_id]
            continue
        if not finished and now - when < 20.0:
            continue
        del _sent[other_id]
        how = getattr(walk, 'finishing_type', None)
        reason = getattr(walk, '_cancel_reason_msg', None) or getattr(walk, 'cancel_reason_msg', None)
        _log('{} ({}): walk to you {} after {:.1f} s, {}{}; was {:.1f} m away, now {:.1f} m'.format(
            _name(other), group, 'ended' if finished else 'still not done', now - when,
            how, ' ({})'.format(reason) if reason else '', was, _apart(px, py, pz, other)))


def update(sim, px, py, pz):
    """walk.py, every tick in first person: the Sim you are in and where you are."""
    global _last, _errors, _you
    now = time.monotonic()
    if now - _last < CHECK_EVERY:
        return
    _last = now
    _you = (px, py, pz)
    try:
        try:
            _sweep(sim, now)
        except Exception as e:
            _errors += 1
            if _errors <= 5:
                _log('ending socials with Sims you left failed: {!r}'.format(e))
        try:
            from sims4vr import simsforever
            if simsforever._shown is not None:
                return  # playing Sims 1 at a computer: nobody follows you around
        except Exception:
            pass
        try:
            _outcomes(px, py, pz, now)
        except Exception as e:
            _errors += 1
            if _errors <= 5:
                _log('reading how the walks ended failed: {!r}'.format(e))
        _this_check.clear()
        groups = list(sim.get_groups_for_sim_gen())
        if not groups:
            _leaving.clear()
            _followed.clear()
            _first_seen.clear()
            _every.clear()
            return
        limit = _limit()
        live = set()
        for group in groups:
            live.add(id(group))
            others = [other for other in group if other is not sim]
            if not others:
                continue
            if id(group) in _leaving:
                _leave(sim, group, others, now, 'you cancelled it')  # the next step while you are still in it
            elif limit > 0:
                try:
                    _follow(sim, group, others, px, py, pz, now, limit)
                except Exception as e:
                    _errors += 1
                    if _errors <= 5:
                        _log('following you failed: {!r}'.format(e))
                if native.setting_flag('body', 'turn_to_you', True) and id(group) not in _leaving:
                    _turn(sim, group, others, px, py, pz, now)
        for table in (_leaving, _followed, _first_seen, _every):
            for key in list(table):
                if key not in live:
                    del table[key]
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            _log('check failed: {!r}'.format(e))
