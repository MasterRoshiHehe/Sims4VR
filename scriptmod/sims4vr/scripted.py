"""In first person your Sim does not run the game's automatic follow-ups.

After cooking or making a drink the game serves it and then makes the cook
pick it up and eat or drink it; served food makes the Sim it was made for do
the same. In first person you do that yourself: you carry the plate to a seat
and eat with your hands (hands.py). Only the follow-up is skipped: the game
treats it as if the Sim were not hungry, and food that would have gone
straight into your hand is put down instead.

Two places push those follow-ups, both checked at call time:
- the crafting interactions' should_push_consume (after the last phase, and
  for objects made straight into the hand);
- serving_interactions.push_object_pick_up_and_consume (served food, and
  orders served to you).
Switch: [hands] no_auto_eat in sims4vr.ini.

Your Sim also never sets off on its own. Autonomy is off in first person
(body.py), but the game still pushes some things on a Sim with autonomy off:
a need in the red sends it to fix it ("distress": a bursting bladder walks it
to the nearest toilet), it shuffles to fit a conversation and sits down to get
comfortable. InteractionQueue.append refuses those for your Sim (always on in
first person). Needs failing (wetting itself, passing out, dying) are pushed by
script, not autonomy, and still happen; so do what you pick and other Sims'
socials with you.

Reactions don't stop you (user 2026-10-14, v0.13.67; [body] reactions_in_place):
your Sim's involuntary reactions, a reaction to something (smelling dirty
dishes or itself, disgust at a puddle or laundry) and a need in the red's
emote (hungry, dirty, bursting: the distress emote with no target), still
play with their sounds, but right where you are: no turning to face the thing
and no stepping toward it, and walk.py neither stops your walking for them nor
cancels them when you push the stick. Whatever such a need would send the Sim
off to do on its own (to a toilet, a fridge) is still refused.

Other Sims' conversations with you happen where you are (user 2026-10-14
13:26Z, v0.13.74): a Sim starting a chat or a flirt with you made your Sim walk
over to it (across the lot, through the house) and sit down beside it on the
couch. Now your Sim's part of a social you didn't pick yourself stays right
where you are, in the posture you are in, and doesn't stop your walking; the
other Sim comes to you and follows you when you walk off; you leave it by
cancelling it on your wrist's queue (hint.py, v0.13.185). Socials you pick
from the pie menu work as before (reach.py).

That never took effect (user 2026-10-14 15:09Z, v0.13.82): a Sim coming to
chat still sent your Sim running around (through walls) while she walked
over. The social's starter plans the walk for both Sims (its transition
carries your Sim too), and your part is made by her social directly, not
queued, so the queue check above never saw it. Now every social made with
your Sim in it, hers or your part of it, is planned with your Sim staying
within HERE of where it stands, in its posture (social_super_interaction's
__init__, so it doesn't matter how it is pushed). Should the game still give
your Sim a path for it, walk.py ends that social instead of following
(stop_walking_over).

The game doesn't send your Sim to things either (user 2026-10-14 14:51Z,
v0.13.81; [body] no_errands): a fire broke out and your Sim ran to it to
panic (fire_Panic, pushed by the fire service), and a sink got repaired
without being picked (object_Repair_Sink). Both are pushed by script, which
the autonomy check above lets through because needs failing come that way
too. Now a script push on your Sim that has a target other than your Sim
(an object, another Sim, a spot on the ground) and isn't the next step of
something already running is refused. What happens to your Sim itself
(wetting itself, passing out, catching fire, dying) has no such target and
still happens.

Socials you pick yourself are held the same way (user 2026-10-14 23:44Z,
v0.13.122): before, reach.py sent a far Sim a Go Here and waited for it; now
the game plans your pick for both of you with your Sim staying where it is, so
the other Sim walks over through the game's own group placement.

Sims you walked away from (hint.py) leave you alone for a while (user
2026-10-14 20:58Z, v0.13.108): their socials with you, and your part of them,
are refused while hint.leaves_you_alone says so, unless you picked a social
with that Sim yourself. Since v0.13.185 that is after you cancelled the
conversation on your wrist (walking away makes them follow you instead).
"""
import math
import time

import sims4.math

from sims4vr import native

HERE = 0.5  # m: how far from where it stands your Sim may be placed for another Sim's social

_logs = 0
_errors = 0
_enabled = None  # [hands] no_auto_eat, read once the DLL is loaded
_stopped = set()  # ids of socials ended because the game walked your Sim over anyway
_paths_seen = set()  # ids of the paths stop_walking_over logged
WAIT_FAR = 3.0  # m: default of [body] talk_distance (talk_distance())
WAIT_INSIDE = 0.3  # m: your walk ends this far inside the talk distance
WAIT_FOR = 60.0  # s: how long their part of it is planned the same way
_walk_to = {}  # Sim id: when you picked a far social with them (you walk, they wait)
FRONT = 0.6  # m in front of a Sim where you stand for a social with hand contact (contact())
FACING = 35.0  # degrees: and face them this squarely
FRONT_NEAR = 0.25  # m from that spot that counts as in front of them (v0.13.218: 0.8/0.45 left you
                   # about a metre away, kissing the air, user 2026-10-17 12:01Z)
CONTACT_WORDS = ('hand', 'hug', 'kiss', 'massage', 'embrace', 'cuddle', 'highfive', 'high_five', 'shake',
                 'touch', 'tickle', 'caress', 'snuggle', 'fistbump', 'fist_bump', 'slowdance', 'hold')


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 300:
        native._log('scripted: ' + message)


def _error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('scripted: {} failed: {!r}'.format(what, e))


def _on():
    global _enabled
    if _enabled is None:
        if native._optional('setting') is None:
            return True  # the DLL isn't loaded yet: the default
        _enabled = native.setting_flag('hands', 'no_auto_eat', True)
    return _enabled


def _yours(sim):
    """The Sim you are in, in first person (and the switch is on)."""
    return _on() and _first_person(sim)


def _first_person(sim):
    """The Sim you are in, in first person."""
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def _emote(interaction, source):
    """An involuntary reaction of your Sim's (see the module text)."""
    from interactions.context import InteractionSource
    if not getattr(interaction, 'is_super', False) or getattr(interaction, 'is_social', False):
        return False
    if source == InteractionSource.REACTION:
        return True
    name = getattr(getattr(interaction, 'affordance', None), '__name__', '').lower()
    target = getattr(interaction, 'target', None)
    return 'distress' in name and (target is None or target is getattr(interaction, 'sim', None))


def _in_place(interaction):
    """Done right where the Sim stands, in the posture it is in: the transition
    planner asks constraint_intersection where the Sim must be; for your Sim
    the answer becomes its current posture wherever it is now."""
    def constraint_intersection(*args, **kwargs):
        from interactions.constraints import ANYWHERE
        who = kwargs.get('sim', args[0] if args else None)
        try:
            if not getattr(who, 'is_sim', False) or who is interaction.sim:  # DEFAULT means its own Sim
                constraint = interaction.sim.posture_state.body_posture_state_constraint
                if constraint is not None and constraint.valid:
                    return constraint
        except Exception:
            pass
        return ANYWHERE

    interaction.constraint_intersection = constraint_intersection
    interaction._sims4vr_emote = True


def _here(sim):
    """Your Sim staying where it is: its posture, within HERE of where it
    stands (standing on its own allows anywhere)."""
    constraint = sim.posture_state.body_posture_state_constraint
    try:
        from interactions.constraints import Circle
        spot = Circle(sim.position, HERE, sim.routing_surface)
        if constraint is not None:
            spot = spot.intersect(constraint)
        if spot.valid:
            return spot
    except Exception as e:
        _error('placing your Sim where it stands', e)
    return constraint


def _pin(interaction, you):
    """A social with your Sim in it: whenever the game asks where your Sim
    must be for it, the answer is where it is now (_here). Everyone else gets
    the game's own answer."""
    original = interaction.constraint_intersection

    def constraint_intersection(*args, **kwargs):
        try:
            who = kwargs.get('sim', args[0] if args else None)
            if not getattr(who, 'is_sim', False):  # DEFAULT: its own Sim, or the participant asked for
                participant = kwargs.get('participant_type', args[2] if len(args) > 2 else None)
                who = interaction.sim
                if participant is not None and type(participant).__name__ == 'ParticipantType':
                    who = interaction.get_participant(participant)
            if getattr(who, 'id', None) == you.id and _first_person(you):
                constraint = _here(you)
                if constraint is not None and constraint.valid:
                    return constraint
        except Exception as e:
            _error('your place in {}'.format(interaction), e)
        return original(*args, **kwargs)

    interaction.constraint_intersection = constraint_intersection
    interaction._sims4vr_emote = True
    interaction._sims4vr_social = True


def _social_with_you(interaction):
    """Pin your Sim in a social it didn't start from the pie menu: another
    Sim's social with you, or your part of it (see the module text)."""
    from interactions.context import InteractionSource
    sim = getattr(interaction, 'sim', None)
    target = getattr(interaction, 'target', None)
    picked = False
    if _first_person(sim):
        source = getattr(getattr(interaction, 'context', None), 'source', None)
        if getattr(interaction, 'initiated', True) and \
                source in (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT):
            picked = True
            if getattr(target, 'is_sim', False):
                from sims4vr import hint
                hint.forget(target)  # you want to talk with that Sim after all
            # Yours, picked by you: pinned too (v0.13.122), so the other Sim walks over.
        you = sim
    elif getattr(target, 'is_sim', False) and _first_person(target):
        you = target  # their part of it (also of one you picked)
    else:
        return
    if not native.setting_flag('body', 'socials_in_place', True):
        return
    other = target if you is sim else sim
    if _you_walk(interaction, you, other, you is sim and picked):
        return
    _pin(interaction, you)
    _log('{} (by {}): planned with you staying where you are'.format(
        interaction, 'your Sim' if you is sim else 'another Sim'))


def _you_walk(interaction, you, other, picked, social=True):
    """You walk, they wait (user 2026-10-16, decision card after the prank
    played 3 m apart): a social you pick with a Sim further than talk_distance()
    away is planned with that Sim staying where it is (_here) and your Sim
    within talk_distance() of it. The game plans your walk there, trail.py holds it
    with footprints, and the social starts once you arrive. Their part of it
    (made for them right after yours) is planned the same way. The game's own
    social constraint alone lets a conversation play across its group radius,
    which is how the prank played far apart."""
    if not getattr(other, 'is_sim', False) or not native.setting_flag('body', 'trail', True):
        return False
    now = time.monotonic()
    talk = talk_distance()
    if not picked:
        entry = _walk_to.get(other.id)
        if entry is None or now - entry[0] > WAIT_FOR:
            return False
        stay = entry[1]
    try:
        from sims4vr import trail
        x, _, z = trail._you(you)
        there = other.position
        far = math.hypot(there.x - x, there.z - z)
    except Exception as e:
        _error('measuring {}'.format(interaction), e)
        return False
    touch = contact(getattr(interaction, 'affordance', type(interaction)))
    if picked:
        # v0.13.197 (user 2026-10-17 02:35Z): within the talk distance it just
        # starts, both of you where you are (a walking Sim stops for it);
        # further away you walk up to them (they wait) until you are that
        # close. (v0.13.196's "a walking Sim comes to you" is gone: the user
        # prefers walking up.)
        # Hand contact: only dead in front of them, then both stay where they are
        # (contact(), user 2026-10-17 12:03Z); elsewhere you walk there first.
        stay = in_front(you, other) if touch else far <= talk
        if stay and not social and not touch:
            return False  # anything else picked on a near Sim: the game places you both as before
        _walk_to[other.id] = (now, stay)
        _log('{}: {} is {:.1f} m away (talk distance {:.1f} m): {}'.format(
            interaction, other, far, talk, 'it starts where you both are' if stay else
            'hand contact ({}): you are in front of them, it starts there'.format(touch) if touch and stay else
            'hand contact ({}): you walk to right in front of them, they wait'.format(touch) if touch else
            'you walk to them, they wait'))
    else:
        _log('{}: the other part, planned with {} {}'.format(
            interaction, other, 'staying where they are' if stay else 'waiting for you'))
    original = interaction.constraint_intersection

    def constraint_intersection(*args, **kwargs):
        constraint = original(*args, **kwargs)
        try:
            who = kwargs.get('sim', args[0] if args else None)
            if not getattr(who, 'is_sim', False):
                participant = kwargs.get('participant_type', args[2] if len(args) > 2 else None)
                who = interaction.sim
                if participant is not None and type(participant).__name__ == 'ParticipantType':
                    who = interaction.get_participant(participant)
            if getattr(interaction, '_sims4vr_free', False):
                # Released (hint.py: the conversation follows you): they may
                # move again; you stay where you are, as in any social.
                if getattr(who, 'id', None) == you.id and _first_person(you):
                    spot = _here(you)
                    if spot is not None and spot.valid:
                        return spot
                return constraint
            if getattr(who, 'id', None) == other.id:
                spot = _here(other)
            elif getattr(who, 'id', None) == you.id and _first_person(you):
                if stay:
                    spot = _here(you)
                elif touch:
                    # Hand contact: right in front of them; a jig social's own
                    # slots already are, so where they don't meet the game's
                    # answer stands.
                    from interactions.constraints import Circle
                    fx, fz = front_of(other)
                    spot = Circle(sims4.math.Vector3(fx, other.position.y, fz), FRONT_NEAR, other.routing_surface)
                    if constraint is not None:
                        spot = spot.intersect(constraint)
                        if not spot.valid:
                            return constraint
                else:
                    from interactions.constraints import Circle
                    spot = Circle(other.position, max(0.8, talk - WAIT_INSIDE), other.routing_surface)
                    if constraint is not None:
                        spot = spot.intersect(constraint)
            else:
                return constraint
            if spot is not None and spot.valid:
                return spot
        except Exception as e:
            _error('places in {}'.format(interaction), e)
        return constraint

    interaction.constraint_intersection = constraint_intersection
    interaction._sims4vr_you_walk = True
    interaction._sims4vr_touch = touch  # trail.py: arrive closer (ARRIVE_CONTACT)
    return True


def contact(affordance, super_affordance=None):
    """Why a social has hand contact (hugs, kisses, holding hands, a hand
    massage), or None (user 2026-10-17 05:42Z: started from the side or a
    few metres away, their hands meet in the air, glitchy). The game's own
    sign is a jig social group (socials/jig_group.py JigGroup: the two Sims
    on fixed slots facing each other, which touching socials use); a mixer
    played inside a chat has none, so its name is checked too."""
    for a in (affordance, super_affordance):
        try:
            from socials.jig_group import JigGroup
            group = getattr(a, '_social_group_type', None)
            if isinstance(group, type) and issubclass(group, JigGroup):
                return 'jig'
        except Exception:
            pass
    name = getattr(affordance, '__name__', '').lower()
    for word in CONTACT_WORDS:
        if word in name:
            return word
    # WickedWhims starts (user 2026-10-17 12:03Z: "Make a move" started from the
    # side, her hand went into her body): its ask leads into a hand-holding start.
    if 'wickedwhims' in name and any(w in name for w in ('autostart', 'askfor', 'makeamove', 'make_move', 'makemove')):
        return 'wickedwhims start'
    return None


def in_front(you, other):
    """You stand dead in front of that Sim (within FRONT_NEAR of front_of) and
    face them (within FACING degrees), as a social with hand contact needs."""
    try:
        pose = native.walk_pose()
        if pose is not None and pose[5]:
            x, z, yaw = pose[0], pose[2], pose[3]
        else:
            p = you.position
            f = you.forward
            x, z, yaw = p.x, p.z, math.atan2(f.x, f.z)
        fx, fz = front_of(other)
        if math.hypot(x - fx, z - fz) > FRONT_NEAR:
            return False
        o = other.position
        dx, dz = o.x - x, o.z - z
        d = math.hypot(dx, dz)
        if d < 1e-3:
            return True
        return (math.sin(yaw) * dx + math.cos(yaw) * dz) / d >= math.cos(math.radians(FACING))
    except Exception as e:
        _error('checking you stand in front of {}'.format(other), e)
        return True


def front_of(sim):
    """(x, z) where you stand directly in front of that Sim."""
    p = sim.position
    f = sim.forward
    return p.x + f.x * FRONT, p.z + f.z * FRONT


def talk_distance():
    """[body] talk_distance (user 2026-10-17 02:35Z, B menu Other Sims "Talk
    starts within", default 3 m): within this of a Sim a social you pick with
    them starts where you both are; further away you walk to them (they
    wait) until you are this close. Mixers in a conversation the same
    (reach.py, trail.py)."""
    try:
        return max(1.0, float(native.setting('body', 'talk_distance', str(WAIT_FAR))))
    except (TypeError, ValueError):
        return WAIT_FAR


def _picked_on_sim(interaction, source):
    """Not only socials (user 2026-10-16 15:22Z: giving a gift sent you to a
    spot a couple of metres from her, and to another one each time you got
    there): something you pick on a Sim further than WAIT_FAR away that
    places you both (a jig like the gift's) is planned the same way, with
    them staying where they are and you next to them (_you_walk). Without it
    the game picks a meeting spot for the two of you that she never walks to."""
    from interactions.context import InteractionSource
    sim = getattr(interaction, 'sim', None)
    target = getattr(interaction, 'target', None)
    if not getattr(target, 'is_sim', False) or target is sim or not getattr(interaction, 'is_super', False) \
            or _is_social(interaction) or you_walk(interaction) or not _first_person(sim) \
            or source not in (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT):
        return
    _you_walk(interaction, sim, target, True, social=False)


def you_walk(interaction):
    """trail.py: a social you walk to with footprints (_you_walk)."""
    return bool(getattr(interaction, '_sims4vr_you_walk', False))


def _is_social(interaction):
    if getattr(interaction, 'is_social', False):
        return True
    from interactions.social.social_super_interaction import SocialSuperInteraction
    return isinstance(interaction, SocialSuperInteraction)


def _you_started(you, other):
    """You picked a social with that Sim from the pie menu (queued or running)."""
    from interactions.context import InteractionSource
    picked = (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT)
    for si in list(you.queue) + list(you.si_state):
        if _is_social(si) and getattr(si, 'target', None) is other and \
                getattr(getattr(si, 'context', None), 'source', None) in picked:
            return True
    return False


def stop_walking_over(interaction, sim, you_at=None):
    """walk.py, while a social pinned by _pin has your Sim in its transition:
    if the game gave your Sim a path anyway that goes further than a small
    step, end that social rather than run your Sim over (once each).

    v0.13.118 (user 2026-10-14 22:38Z: a conversation sent the Sim running off
    through walls again; this had never fired): the path is also measured
    against where you last walked (you_at, from walk.py) and by its own length,
    not only from the simulation's Sim to its end: that Sim's place lags behind
    yours, and a path ending next to it can still be a long run from you."""
    path = sim.routing_component.current_path
    key = id(interaction)
    if path is None or key in _stopped or getattr(interaction, 'is_finishing', True):
        return
    try:
        end = path.final_location.transform.translation
        here = sim.position
        far = math.hypot(end.x - here.x, end.z - here.z)
        if you_at is not None:
            far = max(far, math.hypot(end.x - you_at[0], end.z - you_at[1]))
        try:
            length = path.length()
        except Exception:
            length = -1.0
        far = max(far, length)
        if id(path) not in _paths_seen:
            _paths_seen.add(id(path))
            if len(_paths_seen) <= 30:
                native._log('scripted: {}: your Sim got a path {:.1f} m long to {:.2f} {:.2f}; the simulation has it at {:.2f} {:.2f}, '
                     'you were at {}'.format(interaction, length, end.x, end.z, here.x, here.z,
                                             '{:.2f} {:.2f}'.format(*you_at) if you_at is not None else '?'))
        if far <= 2 * HERE:
            return
    except Exception as e:
        _error('reading the path for {}'.format(interaction), e)
    _stopped.add(key)
    try:
        # v0.13.159: a hard stop of the walk (halt.py): the transition that walks
        # you is often the other Sim's, so ending only this social left it going.
        from sims4vr import halt
        from interactions.interaction_finisher import FinishingType
        halt.walking(sim, 'VR first person: they come to you ({} walked your Sim over)'.format(interaction))
        if not interaction.is_finishing:
            interaction.cancel(getattr(FinishingType, 'SOCIALS', FinishingType.USER_CANCEL),
                               cancel_reason_msg='VR first person: they come to you', ignore_must_run=True,
                               immediate=True)
        _log('{}: the game walked your Sim over anyway; ended it'.format(interaction))
    except Exception as e:
        _error('ending {}'.format(interaction), e)


def _their_social(interaction, source):
    """Another Sim's social with your Sim: your part of a conversation or
    flirt you didn't pick yourself (see the module text)."""
    from interactions.context import InteractionSource
    if not getattr(interaction, 'is_super', False) or not _is_social(interaction):
        return False
    return source not in (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT)


def _unwelcome(interaction, source):
    """A social between your Sim and a Sim you walked away from (hint.py),
    not picked by you, while that Sim is still out of earshot."""
    from interactions.context import InteractionSource
    if not getattr(interaction, 'is_super', False) or not _is_social(interaction) or \
            source in (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT):
        return False
    sim = getattr(interaction, 'sim', None)
    target = getattr(interaction, 'target', None)
    if not getattr(target, 'is_sim', False) or target is sim:
        return False
    if _first_person(sim):
        you, other = sim, target
    elif _first_person(target):
        you, other = target, sim
    else:
        return False
    if _you_started(you, other):
        return False
    from sims4vr import hint
    return hint.leaves_you_alone(other, you)


def _errand(interaction, source):
    """A script push that would send your Sim to something (see the module text)."""
    from interactions.context import InteractionSource
    if source != InteractionSource.SCRIPT or not getattr(interaction, 'is_super', False) or \
            _is_social(interaction):
        return False
    if getattr(getattr(interaction, 'context', None), 'continuation_id', None):
        return False  # the next step of something already running
    sim = getattr(interaction, 'sim', None)
    target = getattr(interaction, 'target', None)
    return target is not None and target is not sim and target is not getattr(sim, 'sim_info', None)


def is_emote(interaction):
    """walk.py: a reaction (or another Sim's social with you) that plays where
    you are without stopping you."""
    return bool(getattr(interaction, '_sims4vr_emote', False))


def is_social_with_you(interaction):
    """walk.py: a social pinned by _pin (see stop_walking_over)."""
    return bool(getattr(interaction, '_sims4vr_social', False))


def _wrap_queue_append():
    from event_testing.results import TestResult
    from interactions.context import InteractionSource
    from interactions.interaction_finisher import FinishingType
    from interactions.interaction_queue import InteractionQueue
    original = InteractionQueue.__dict__.get('append')
    if original is None:
        native._log('scripted: InteractionQueue.append not found')
        return
    on_its_own = {
        InteractionSource.AUTONOMY: 'on its own',
        InteractionSource.SOCIAL_ADJUSTMENT: 'to fit a conversation',
        InteractionSource.GET_COMFORTABLE: 'to get comfortable',
    }

    def append(self, interaction, *args, **kwargs):
        why = None
        unwelcome = False
        try:
            context = getattr(interaction, 'context', None)
            source = getattr(context, 'source', None)
            _picked_on_sim(interaction, source)
            if native.setting_flag('body', 'socials_in_place', True) and _unwelcome(interaction, source):
                unwelcome = True
            elif _first_person(getattr(interaction, 'sim', None)) and _emote(interaction, source) and \
                    native.setting_flag('body', 'reactions_in_place', True):
                _in_place(interaction)
                _log('{}: your Sim reacts where it is, without stopping you'.format(interaction))
            elif is_social_with_you(interaction):
                pass  # pinned when it was made (_wrap_social_init)
            elif _first_person(getattr(interaction, 'sim', None)) and _their_social(interaction, source) and \
                    native.setting_flag('body', 'socials_in_place', True):
                _pin(interaction, interaction.sim)
                _log('{} (source {}): another Sim talks with you where you are; you don\'t walk over'.format(
                    interaction, source))
            else:
                why = on_its_own.get(source)
                if why is None and _first_person(getattr(interaction, 'sim', None)) and \
                        _errand(interaction, source) and native.setting_flag('body', 'no_errands', True):
                    why = 'that the game sent it to'
            if why is not None and (not getattr(interaction, 'is_super', False) or _is_social(interaction)
                                    or not _first_person(getattr(interaction, 'sim', None))):
                why = None  # mixers, someone else's social with you, other Sims
        except Exception as e:
            _error('checking {}'.format(interaction), e)
            why = None
            unwelcome = False
        if unwelcome:
            _log('{}: refused; you walked away from them (hint.py)'.format(interaction))
            try:
                interaction.cancel(FinishingType.INTERACTION_QUEUE,
                                   cancel_reason_msg='VR first person: you walked away from them')
            except Exception:
                pass
            return TestResult(False, 'VR first person: you walked away from them')
        if why is None:
            return original(self, interaction, *args, **kwargs)
        _log('{}: your Sim does nothing {}'.format(interaction, why))
        try:
            interaction.cancel(FinishingType.INTERACTION_QUEUE, cancel_reason_msg='VR first person: nothing on its own')
        except Exception:
            pass
        return TestResult(False, 'VR first person: your Sim does nothing on its own')

    InteractionQueue.append = append


def _wrap_social_init():
    from interactions.social.social_super_interaction import SocialSuperInteraction
    original = SocialSuperInteraction.__init__

    def __init__(self, *args, **kwargs):
        original(self, *args, **kwargs)
        try:
            _social_with_you(self)
        except Exception as e:
            _error('a social with you', e)

    SocialSuperInteraction.__init__ = __init__


def _wrap_should_push_consume():
    from crafting import crafting_interactions
    cls = crafting_interactions.CraftingPhaseSuperInteractionMixin
    original = cls.__dict__.get('should_push_consume')
    if original is None:
        native._log('scripted: should_push_consume not found')
        return

    def should_push_consume(self, *args, **kwargs):
        if _yours(getattr(self, 'sim', None)):
            _log('{}: no automatic eating or drinking for you'.format(self))
            return False
        return original(self, *args, **kwargs)

    cls.should_push_consume = should_push_consume


def _wrap_pick_up_and_consume():
    from crafting import serving_interactions
    original = getattr(serving_interactions, 'push_object_pick_up_and_consume', None)
    if original is None:
        native._log('scripted: push_object_pick_up_and_consume not found')
        return

    def push_object_pick_up_and_consume(parent_crafting_interaction, order_sim, object_to_serve, *args, **kwargs):
        if _yours(order_sim):
            from event_testing.results import TestResult
            _log('{} served: yours to pick up'.format(object_to_serve))
            return TestResult(False, 'VR first person: you pick it up yourself')
        return original(parent_crafting_interaction, order_sim, object_to_serve, *args, **kwargs)

    serving_interactions.push_object_pick_up_and_consume = push_object_pick_up_and_consume


_balloon_logs = 0


def _wrap_balloon_distribute():
    """Diagnostics (bubbles thread): logs balloons the game sends for your Sim in first person."""
    from balloon.balloon_request import BalloonRequest
    original = BalloonRequest.distribute

    def distribute(self, *args, **kwargs):
        global _balloon_logs
        result = original(self, *args, **kwargs)
        try:
            sim = self._sim
            if _balloon_logs < 40 and _first_person(sim):
                _balloon_logs += 1
                native._log('scripted: balloon for your Sim {} (type {}, icon {}): {}'.format(
                    getattr(sim, 'id', None), self.balloon_type, getattr(self.icon, 'instance', None),
                    'sent' if result else 'not sent (Sim hidden: {})'.format(sim.is_hidden())))
        except Exception as e:
            _error('balloon log', e)
        return result

    BalloonRequest.distribute = distribute


for _wrap in (_wrap_should_push_consume, _wrap_pick_up_and_consume, _wrap_queue_append, _wrap_social_init,
              _wrap_balloon_distribute):
    try:
        _wrap()
    except Exception as e:
        native._log('scripted: {} failed: {!r}'.format(_wrap.__name__, e))
