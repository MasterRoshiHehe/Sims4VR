"""Grabbing other Sims' bodies in first person, and how they take it
(user 2026-10-17: the breast grab of Claude Jiggle extended to the whole body,
with different reactions depending on where and how they are grabbed).

The DLL (armik.cpp) holds the place your hand squeezes (grip and trigger
together, or the grip alone on their hand) and sends a touch event naming it:
hand, forearm, upper arm, shoulder, hair, face, neck, chest, belly, back,
waist, butt, crotch, hips, thigh, inner thigh, leg.

Checked against the game (re/python128): a social's outcome decides whether
it lands (interactions/utils/outcome.py: InteractionOutcomeDual success chance,
InteractionOutcomeTestBased tested and weighted outcomes) and its loot carries
the relationship changes (a rejected kiss: Rel_Loss_Medium, romance loss,
embarrassment, jealousy). A touch is judged the same way, by the game social it
is closest to, with you as the actor and them as the target:
- hand, arms, shoulder, hair, back, waist, leg: Hug (friendly);
- face, neck: Caress Cheek (romantic);
- chest, belly, butt, hips, crotch, thighs: Kiss (romantic).
Its own tests run first (age, family, romance allowed); if they fail the touch
is inappropriate (the game's reaction_Inappropriate loot, Rel_Loss_Medium).
The chosen outcome's loot list is applied as the social would
(LootOperationList). Their reaction is one of the game's reaction mixers,
pushed the way a ReactionLootOp does (interactions/utils/reactions.py
ReactionMixer: on their posture's interaction, REACTION source, High priority,
targeting you): a giggle when welcome, flinching when a friendly touch isn't,
shock or disgust when an intimate one isn't (the hot-headed and mean flinch
angrily instead of looking shocked).
How: holding on keeps being judged every HOLD_ROUND seconds (a held hug or
caress is welcome until it isn't); pulling hard on any place counts as rough
(a friendly touch they had welcomed is judged again, as a failure flinch if it
fails). Your own Sim does nothing (no scripted actions): no animation, only the
loot the social would give you.
Switches: [physical] grab_bodies (DLL) / grab_reactions in sims4vr.ini.
"""
import time

import services
import sims4.resources

from sims4vr import native

REGIONS = ('hand', 'forearm', 'upper arm', 'shoulder', 'hair', 'face', 'neck', 'chest', 'belly', 'back', 'waist',
           'butt', 'crotch', 'hips', 'thigh', 'inner thigh', 'leg')
INTIMATE = frozenset(('chest', 'belly', 'butt', 'hips', 'crotch', 'thigh', 'inner thigh'))
TENDER = frozenset(('face', 'neck'))

# The game socials a touch is judged by (base game tuning ids).
HUG = 26914  # mixer_social_Hug_targeted_Friendly_MiddleScore
CARESS_CHEEK = 26167  # mixer_social_CaressCheek_targeted_romance_highScore
KISS = 28612  # mixer_social_Kiss_targeted_romance_middleScore
INAPPROPRIATE_LOOT = (11040,)  # Rel_Loss_Medium, as reaction_Inappropriate gives

# Reaction mixers (base game).
GIGGLE = 123828  # reaction_Mixers_Giggle
FLINCH = 120157  # reaction_Mixer_Socials_Mean_Surprised
SHOCKED = 125199  # reaction_Generic_Shocked_Mixer
DISGUSTED = 169085  # reaction_Generic_Disgusted_Mixer

HOT_HEADED, MEAN = 16845, 16857  # trait_HotHeaded, trait_Mean
NEAR = 3.0  # metres: the Sims a touch event may be about
HOLD_ROUND = 6.0  # seconds between judgements while you keep holding on
COOLDOWN = 4.0  # seconds after a judgement before the same Sim is judged again for a new touch

SAFE_ON_RESTRICTED = frozenset(('hand', 'forearm', 'upper arm', 'shoulder', 'back', 'hair'))
RESTRICT_EVERY = 0.5  # seconds between telling the DLL which Sims near you are restricted

_holds = {}  # hand -> {'sim_id', 'region', 'since', 'judged', 'welcome'}
_restricted_sent = None
_last_restrict = -RESTRICT_EVERY
_last_judged = {}  # sim id -> time
_logs = 0
_errors = 0
_why_not = ''


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 300:
        native._log('bodygrab: ' + message)


def _error(where, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('bodygrab: {} failed: {!r}'.format(where, e))


def _affordance(instance):
    return services.get_instance_manager(sims4.resources.Types.INTERACTION).get(instance)


def _sim_for_actor(you, actor):
    """The instanced Sim near you whose client object is `actor`."""
    p = you.position
    for sim in services.sim_info_manager().instanced_sims_gen():
        if sim is you:
            continue
        q = sim.position
        if (q.x - p.x) ** 2 + (q.z - p.z) ** 2 > NEAR * NEAR:
            continue
        try:
            if native.object_ptr(sim.id) == actor:
                return sim
        except Exception:
            pass
    return None


def _social_for(region):
    if region in INTIMATE:
        return KISS
    if region in TENDER:
        return CARESS_CHEEK
    return HUG


def _too_young(you, npc):
    """Children and younger, and a teen with an adult: their bodies may only be
    held at the hand, arms, shoulders, back and hair (the DLL refuses any other
    place; this is the script's own check)."""
    from sims.sim_info_types import Age
    young = {Age.BABY, Age.TODDLER, Age.CHILD}
    if hasattr(Age, 'INFANT'):
        young.add(Age.INFANT)
    a, b = you.sim_info.age, npc.sim_info.age
    if a in young or b in young:
        return True
    return (a == Age.TEEN) != (b == Age.TEEN)


def _family(you, npc):
    try:
        return npc.sim_id in set(you.sim_info.genealogy.get_family_sim_ids_gen())
    except Exception:
        return False


def _likes(you, npc, romantic):
    from relationships.relationship_track import RelationshipTrack
    tracker = npc.sim_info.relationship_tracker
    track = RelationshipTrack.ROMANCE_TRACK if romantic else RelationshipTrack.FRIENDSHIP_TRACK
    try:
        score = tracker.get_relationship_score(you.sim_id, track)
    except Exception:
        score = 0
    return (score or 0) >= (20 if romantic else 0)


def _choose_outcome(affordance, resolver, you):
    """The social's outcome for this resolver, decided as the game decides it:
    (outcome actions, success) or (None, False) when its tests reject it."""
    import random
    from interactions.utils.outcome import InteractionOutcomeDual, InteractionOutcomeTestBased
    from interactions.utils.outcome_enums import OutcomeResult
    outcome = affordance.outcome
    if isinstance(outcome, InteractionOutcomeDual):
        chance = outcome._success_chance.get_chance(resolver)
        try:
            chance += you.get_actor_success_modifier(affordance, resolver)
            chance += you.get_success_chance_modifier()
        except Exception:
            pass
        chance = min(1.0, max(0.0, chance))
        success = random.random() <= chance
        return (outcome._success_actions if success else outcome._failure_actions), success
    if isinstance(outcome, InteractionOutcomeTestBased):
        for entry in outcome._prioritized_tested_outcomes:
            if entry.tests.run_tests(resolver):
                return entry.outcome, entry.outcome.outcome_result == OutcomeResult.SUCCESS
        weights = []
        for entry in outcome._tested_outcomes:
            if not entry.tests.run_tests(resolver):
                continue
            for potential in entry.potential_outcomes:
                weight = potential.weight.get_multiplier(resolver)
                if weight > 0:
                    weights.append((weight, potential.outcome))
        if outcome._use_fallback_as_default or not weights:
            for potential in outcome._fallback_outcomes:
                weight = potential.weight.get_multiplier(resolver)
                if weight > 0:
                    weights.append((weight, potential.outcome))
        if not weights:
            return None, False
        import sims4.random
        chosen = sims4.random.weighted_random_item(weights)
        return chosen, chosen.outcome_result == OutcomeResult.SUCCESS
    return None, True


def _apply_loot(loot_list, resolver):
    from interactions.utils.loot import LootOperationList
    for loot in loot_list or ():
        try:
            LootOperationList(resolver, (loot,)).apply_operations()
        except Exception as e:
            _error('loot {}'.format(loot), e)


def _react(npc, you, mixer_id):
    """Pushes a reaction mixer on `npc` targeting you, as the game's
    ReactionMixer does (interactions/utils/reactions.py)."""
    from autonomy.content_sets import get_valid_aops_gen
    from interactions.context import InteractionContext, InteractionSource, QueueInsertStrategy
    from interactions.interaction_finisher import FinishingType
    from interactions.priority import Priority
    global _why_not
    mixer = _affordance(mixer_id)
    if mixer is None:
        _why_not = 'no such mixer'
        return False
    si = npc.posture.source_interaction if npc.posture is not None else None
    if si is None:
        _why_not = 'no posture interaction'
        return False
    for group in npc.get_groups_for_sim_gen():
        if group.disallow_reaction_mixers:
            _why_not = 'their social group allows no reactions'
            return False
    _why_not = 'no valid way to play it in posture {}'.format(npc.posture)
    context = InteractionContext(npc, InteractionSource.REACTION, Priority.High,
                                 insert_strategy=QueueInsertStrategy.NEXT)
    for aop, test_result in get_valid_aops_gen(you, mixer, si.super_affordance, si, context, False,
                                               push_super_on_prepare=False):
        if not test_result:
            _why_not = 'test: {}'.format(getattr(test_result, 'reason', test_result))
            continue
        constraint = aop.constraint_intersection(sim=npc, posture_state=None)
        if not constraint.intersect(npc.posture_state.posture_constraint_strict).valid:
            continue
        result = aop.execute(context)
        if not result:
            continue
        if npc.queue.always_start_inertial:
            running = npc.queue.running
            if running is not None and not running.is_super:
                running.cancel(FinishingType.DISPLACED, cancel_reason_msg='Reaction displaced mixer.')
        return True
    return False


def _has_trait(sim, trait_id):
    trait = services.get_instance_manager(sims4.resources.Types.TRAIT).get(trait_id)
    return trait is not None and sim.sim_info.has_trait(trait)


def _judge(you, npc, region, rough=False):
    """Judges one touch: applies the social's loot, plays their reaction.
    Returns True if they welcomed it."""
    from event_testing.resolver import DoubleSimResolver
    resolver = DoubleSimResolver(you.sim_info, npc.sim_info)
    social_id = _social_for(region)
    affordance = _affordance(social_id)
    romantic = social_id != HUG
    allowed = affordance is not None
    if allowed and romantic and _too_young(you, npc):
        allowed = False
    # The social's own availability tests are not used: outside a conversation
    # they fail on what a touch doesn't have (a social group, the pie menu),
    # which made every touch inappropriate (user test 2026-10-17 05:45Z).
    # (They need SocialGroup / PickedSim participants a DoubleSimResolver
    # doesn't have, so they are not run at all.)
    if allowed and romantic and _family(you, npc):
        allowed = False
    if allowed:
        actions, welcome = _choose_outcome(affordance, resolver, you)
        if actions is None and not welcome:
            # No outcome applies to a touch: their feelings for you decide.
            actions, welcome = None, _likes(you, npc, romantic)
    if rough and allowed and welcome:
        # A hard pull on a place they had welcomed: judged once more, and it
        # takes the failure reaction if it fails.
        actions, welcome = _choose_outcome(affordance, resolver, you)
    if not allowed:
        actions, welcome = None, False
        _apply_loot([services.get_instance_manager(sims4.resources.Types.ACTION).get(i)
                     for i in INAPPROPRIATE_LOOT], resolver)
    elif actions is not None:
        _apply_loot(actions.loot_list, resolver)
    if welcome:
        mixer = GIGGLE
    elif not romantic:
        mixer = FLINCH
    elif _has_trait(npc, HOT_HEADED) or _has_trait(npc, MEAN):
        mixer = FLINCH
    elif region in ('crotch', 'chest') or not allowed:
        mixer = DISGUSTED
    else:
        mixer = SHOCKED
    reacted = False
    try:
        reacted = _react(npc, you, mixer)
    except Exception as e:
        _error('reaction', e)
    name = affordance.__name__ if affordance is not None else social_id
    _log('{} {} by the {} ({}): {}{}, reaction {}'.format(
        'pulled' if rough else 'held', npc.sim_info.first_name, region, name,
        'welcome' if welcome else ('not allowed' if not allowed else 'not welcome'),
        '' if actions is None else ' (outcome loot applied)', mixer if reacted else 'none ({})'.format(_why_not)))
    return welcome


def _send_restricted(you, now):
    global _restricted_sent, _last_restrict
    if now - _last_restrict < RESTRICT_EVERY:
        return
    _last_restrict = now
    p = you.position
    ids = []
    for sim in services.sim_info_manager().instanced_sims_gen():
        if sim is you:
            continue
        q = sim.position
        if (q.x - p.x) ** 2 + (q.z - p.z) ** 2 > NEAR * NEAR:
            continue
        try:
            if _too_young(you, sim):
                ids.append(sim.id)
        except Exception:
            pass
    ids = tuple(sorted(ids)[:16])
    if ids != _restricted_sent:
        _restricted_sent = ids
        native.set_touch_restricted(ids)
        if ids:
            _log('{} Sims near you may only be held at the hand, arms, shoulders, back and hair'.format(len(ids)))


def tick(you):
    """Called by hands.py twenty times a second in first person."""
    if not native.setting_flag('physical', 'grab_reactions', True):
        while native.touch_event() is not None:
            pass
        _holds.clear()
        return
    now = time.monotonic()
    try:
        _send_restricted(you, now)
    except Exception as e:
        _error('restricted Sims', e)
    while True:
        event = native.touch_event()
        if event is None:
            break
        kind, hand, region_index, actor, value = event
        region = REGIONS[region_index] if 0 <= region_index < len(REGIONS) else 'body'
        try:
            if kind == 1:
                npc = _sim_for_actor(you, actor)
                if npc is None or not npc.sim_info.is_human:
                    _log('the {} you hold is no Sim near you'.format(region))
                    continue
                if region not in SAFE_ON_RESTRICTED and _too_young(you, npc):
                    continue  # never held there (the DLL refuses it too)
                native.haptic(hand, 0.35, 0.06)
                hold = {'sim_id': npc.id, 'region': region, 'since': now, 'judged': now, 'welcome': True}
                _holds[hand] = hold
                if now - _last_judged.get(npc.id, -COOLDOWN) < COOLDOWN:
                    continue
                _last_judged[npc.id] = now
                hold['welcome'] = _judge(you, npc, region)
            elif kind == 2:
                hold = _holds.pop(hand, None)
                if hold is not None:
                    _log('let go of the {} after {:.1f} s'.format(hold['region'], value))
            elif kind == 3:
                hold = _holds.get(hand)
                npc = services.object_manager().get(hold['sim_id']) if hold else None
                if npc is not None:
                    native.haptic(hand, 0.6, 0.1)
                    _last_judged[npc.id] = now
                    hold['judged'] = now
                    hold['welcome'] = _judge(you, npc, hold['region'], rough=True)
        except Exception as e:
            _error('touch event {}'.format(kind), e)
    # Holding on: judged again every HOLD_ROUND seconds while it's welcome.
    for hand, hold in list(_holds.items()):
        if not hold['welcome'] or now - hold['judged'] < HOLD_ROUND:
            continue
        npc = services.object_manager().get(hold['sim_id'])
        if npc is None:
            _holds.pop(hand, None)
            continue
        hold['judged'] = now
        _last_judged[npc.id] = now
        try:
            hold['welcome'] = _judge(you, npc, hold['region'])
        except Exception as e:
            _error('holding on', e)
