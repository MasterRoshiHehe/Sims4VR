"""How other Sims take your touch in first person: grabbing their bodies, and
touching them without holding on (user 2026-10-26: a body sensitivity "heat
map" weighed against how they feel about you; replaces the judging by game
socials, whose tests need a social group and never passed).

The DLL (armik.cpp) sends touch events naming the place: hand, forearm, upper
arm, shoulder, hair, face, neck, chest, belly, back, waist, butt, crotch, hips,
thigh, inner thigh, leg. Kinds: 1 your hand takes hold, 2 lets go, 3 pulls hard,
4 touches without holding on (value: m/s into the body). It also turns their
head to the hand that did it, then back to you ([physical] look_at_hand).

The judgement (user 2026-10-26 20:13Z answers):
- Each place has a sensitivity, 0 (a hand) to 1 (the crotch): the closeness
  it takes for the touch to be welcome, and how much it pays off if it is.
- Closeness: friendship counts fully on neutral places and less and less on
  sensitive ones (none at all past the chest); romance counts on every place,
  but only if romance can be: both adults, not family, and they are attracted
  to your Sim's gender. Disliking you counts against every place.
- Their mood and traits move it (angry, tense, flirty, playful; hot-headed,
  mean, loner, romantic, cheerful...).
- How (force_of, v0.13.580): a touch costs only the closeness the place
  needs; a pat, slap or hit also costs pain = how much that place hurts (HURT:
  face, crotch 1, neck .9, hair .7, chest .5, butt .2, hand .15) x how fast.
  Only enough pain counts as a hit (hard stop, hurt/scared moodlets, fights):
  a light slap on the chest or butt is forward (embarrassed, told off, maybe
  slapped), a slap in the face is a hit. A grab is a bit more forward than a
  touch; pulling hard on a held place hurts by the place (hair yes, arm no).
- Family: intimate places (sensitivity 0.55 and up) are always refused.
- Children, and a teen with an adult: the DLL sends only the hand, arms,
  shoulders, back and hair, and they react friendly or surprised at most.
Reactions are base-game reaction mixers that play where they stand or sit
(no routing), pushed as the game's ReactionMixer does (_react). Welcome
touches raise friendship (and romance on intimate places), unwelcome ones
lower it ([physical] touch_relationship). Your own Sim does nothing.
Later (user): repeated touches building up; escalation (a punch starting a
fight, a slap making them cry or slap back).
Switches: [physical] grab_reactions, touch_reactions, touch_relationship.
"""
import random
import time

import services
import sims4.resources

from sims4vr import native

REGIONS = ('hand', 'forearm', 'upper arm', 'shoulder', 'hair', 'face', 'neck', 'chest', 'belly', 'back', 'waist',
           'butt', 'crotch', 'hips', 'thigh', 'inner thigh', 'leg')
# How sensitive each place is: the closeness a touch there takes to be welcome.
SENSITIVITY = {
    'hand': 0.05, 'forearm': 0.1, 'upper arm': 0.15, 'shoulder': 0.15, 'back': 0.25, 'leg': 0.3, 'hair': 0.35,
    'waist': 0.4, 'face': 0.45, 'neck': 0.5, 'belly': 0.55, 'hips': 0.6, 'thigh': 0.6, 'butt': 0.75,
    'chest': 0.85, 'inner thigh': 0.9, 'crotch': 1.0,
}
INTIMATE = 0.55  # sensitivity from which a place is intimate (never for family)
# How much a strike there hurts or insults (user 2026-10-27 15:23Z: a light slap
# on the breasts counted the same as a hard slap in the face). Sensitivity
# above says how close you must be for a touch there to be welcome; this says
# how much force there hurts. A slap in the face is the insult; a light slap on
# the chest or butt is forward, not an assault.
HURT = {
    'face': 1.0, 'crotch': 1.0, 'neck': 0.9, 'hair': 0.7, 'chest': 0.5, 'inner thigh': 0.5, 'belly': 0.5,
    'back': 0.35, 'thigh': 0.3, 'waist': 0.3, 'shoulder': 0.3, 'leg': 0.3, 'hips': 0.25, 'upper arm': 0.25,
    'butt': 0.2, 'forearm': 0.2, 'hand': 0.15,
}

# Reaction mixers (base game; they play in place, from the Sim's posture).
GIGGLE = 123828  # reaction_Mixers_Giggle
LAUGH = 125532  # reaction_Generic_Funny_Mixer_Strong
SURPRISED = 120157  # reaction_Mixer_Socials_Mean_Surprised
SHOCKED = 125199  # reaction_Generic_Shocked_Mixer
DISGUSTED = 169085  # reaction_Generic_Disgusted_Mixer
HECKLE_LIGHT, HECKLE_LOW, HECKLE_STRONG = 125654, 125656, 125653  # reaction_Generic_Heckle_Mixer_*
# Response animations played through the giggle mixer (mixer_for_animation), clearer than the
# generic mixers (user 2026-10-26 21:30Z: disliked grabs all got the "you smell" disgust):
ANIM_SMILE = 28370  # Socials_Responses_Friendly_LowIntensity
ANIM_FLIRTY, ANIM_FLIRTY_HIGH = 28365, 28366  # Socials_Responses_Flirty_Low/HighIntensity
ANIM_EMBARRASSED, ANIM_EMBARRASSED_HIGH = 28356, 28357  # Socials_Responses_Embarrassed_Low/HighIntensity
ANIM_ANNOYED = 28336  # Socials_Responses_Angry_LowIntensity
ANIM_ANGRY = 28337  # Socials_Responses_Angry_HighIntensity
ANIM_YELL = 28338  # Socials_Responses_Angry_HighIntensity_YellAt
ANIM_HORRIFIED = 28379  # Socials_Responses_Miscellaneous_Horrified

# Their mood (mood tuning name contains) -> closeness change.
MOODS = (('angry', -0.2), ('tense', -0.1), ('sad', -0.1), ('embarrassed', -0.1), ('uncomfortable', -0.1),
         ('happy', 0.05), ('playful', 0.08), ('confident', 0.03))
FLIRTY = 0.15  # a flirty mood, on intimate places when romance can be (0.03 elsewhere)
# Their traits (trait tuning name contains) -> closeness change.
TRAITS = (('hotheaded', -0.1), ('mean', -0.1), ('evil', -0.05), ('snob', -0.05), ('loner', -0.08),
          ('gloomy', -0.03), ('cheerful', 0.05), ('goofball', 0.05), ('outgoing', 0.05))
ROMANTIC_TRAIT = 0.1  # trait_Romantic, on intimate places when romance can be
TEMPERED = ('hotheaded', 'mean')  # these heckle instead of looking shocked

PAT_SPEED, SLAP_SPEED = 0.5, 1.8  # m/s into the body, as the touch sounds tell them apart
WELCOME, SURPRISED_TIER, DISPLEASED = 0.1, -0.2, -0.45  # margins: at or above each

NEAR = 3.0  # metres: the Sims a touch event may be about
HOLD_ROUND = 6.0  # seconds between judgements while you keep holding on (while welcome)
COOLDOWN = 2.5  # seconds after a judgement before the same Sim is judged again for a new touch

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


def _scores(you, npc):
    """Their friendship and romance with you, -1..1."""
    from relationships.relationship_track import RelationshipTrack
    tracker = npc.sim_info.relationship_tracker
    out = []
    for track in (RelationshipTrack.FRIENDSHIP_TRACK, RelationshipTrack.ROMANCE_TRACK):
        try:
            out.append((tracker.get_relationship_score(you.sim_id, track) or 0) / 100.0)
        except Exception:
            out.append(0.0)
    return out


def _attracted(you, npc):
    """They are romantically attracted to your Sim's gender (true where the
    game has no orientation to ask)."""
    try:
        from sims.global_gender_preference_tuning import GenderPreferenceType
        genders = npc.sim_info.get_attracted_genders(GenderPreferenceType.ROMANTIC)
    except Exception:
        return True
    return you.sim_info.gender in genders


def _names(items):
    return [getattr(i, '__name__', type(i).__name__).lower() for i in items]


def _traits(npc):
    try:
        return _names(npc.sim_info.trait_tracker.equipped_traits)
    except Exception:
        return []


def _mood(npc):
    try:
        mood = npc.get_mood()
        return getattr(mood, '__name__', '').lower() if mood is not None else ''
    except Exception:
        return ''


def _change(you, npc, friendship, romance):
    from relationships.relationship_track import RelationshipTrack
    tracker = npc.sim_info.relationship_tracker
    for amount, track in ((friendship, RelationshipTrack.FRIENDSHIP_TRACK), (romance, RelationshipTrack.ROMANCE_TRACK)):
        if amount:
            try:
                tracker.add_relationship_score(you.sim_id, amount, track)
            except Exception as e:
                _error('relationship change', e)


def _number(key, default):
    try:
        return float(native.setting('physical', key, str(default)) or default)
    except (TypeError, ValueError):
        return default


def force_of(how, region, speed):
    """How hard a touch was, and what it does to them (debug menu page
    How hard a touch counts): force 'touch', 'pat', 'slap' or 'hit' by speed;
    pain = weight x how much that place hurts (HURT) x strength, strength 0 at
    PAT_SPEED, 1 at the hard-hit speed, at most 1.5; a hard pull hurts
    pull_pain x HURT. It counts as a hit (hard stop, hurt/scared moodlets,
    fights) from hit_from pain, as a hard hit from hard_pain."""
    slap_at = _number('slap_speed', SLAP_SPEED)
    hard_at = max(slap_at + 0.1, _number('hard_hit_speed', 3.0))
    hurt = HURT.get(region, 0.4)
    force, pain = 'touch', 0.0
    if how == 'pull':
        force, pain = 'pull', _number('pull_pain', 0.5) * hurt
    elif how == 'touch' and speed >= PAT_SPEED:
        force = 'hit' if speed >= hard_at else 'slap' if speed >= slap_at else 'pat'
        strength = min(1.5, (speed - PAT_SPEED) / (hard_at - PAT_SPEED))
        pain = _number('hit_weight', 0.9) * hurt * strength
    hit = pain > 0 and pain >= _number('hit_from', 0.3)
    hard = pain >= _number('hard_pain', 0.8)
    return force, pain, hit, hard


def _judge(you, npc, region, how, speed=0.0):
    """Judges one touch (how: 'touch', 'grab' or 'pull'), plays their reaction
    and changes your relationship. Returns True if they welcomed it."""
    s = SENSITIVITY.get(region, 0.5)
    intimate = s >= INTIMATE
    restricted = _too_young(you, npc)
    family = _family(you, npc)
    friend, love = _scores(you, npc)
    romance_ok = not restricted and not family and _attracted(you, npc)
    if not romance_ok:
        love = 0.0
    # Friendship counts fully up to sensitivity 0.3, none from 0.8.
    friend_weight = min(1.0, max(0.0, (0.8 - s) / 0.5))
    if friend >= 0:
        closeness = max(friend * friend_weight, love)
    else:
        closeness = friend + max(love, 0.0) * 0.5
    notes = []
    force, pain, hit, hard = force_of(how, region, speed)
    margin = closeness - _number('intimacy_weight', 0.6) * s - pain
    mood = _mood(npc)
    for word, value in MOODS:
        if word in mood:
            margin += value
            notes.append(word)
    if 'flirty' in mood:
        margin += FLIRTY if intimate and romance_ok else 0.03
        notes.append('flirty')
    traits = _traits(npc)
    for word, value in TRAITS:
        if any(word in t for t in traits):
            margin += value
            notes.append(word)
    if intimate and romance_ok and any('romantic' in t for t in traits):
        margin += ROMANTIC_TRAIT
        notes.append('romantic')
    slap = force in ('slap', 'hit')
    if how == 'grab':
        margin -= 0.05 * s
    elif how == 'pull':
        margin -= 0.1
    if force != 'touch':
        notes.append('{} pain {:.2f}{}'.format(force, pain, ', a hard hit' if hard else ', a hit' if hit else ''))
    # Again and again: each unwelcome touch or hit lately makes the next one worse.
    from sims4vr import reactions
    bad, hits = reactions.repeats(npc)
    if bad or hits:
        margin -= 0.1 * bad + 0.1 * hits
        notes.append('{} unwelcome, {} hits lately'.format(bad, hits))
    margin += random.gauss(0.0, 0.08)
    if family and intimate:
        margin = min(margin, DISPLEASED - 0.1)
        notes.append('family')
    if restricted:
        margin = max(margin, SURPRISED_TIER)  # friendly or surprised only
    tempered = any(any(word in t for t in traits) for word in TEMPERED)
    if margin >= WELCOME:
        tier = 'welcome'
        if intimate and romance_ok:
            mixer = (ANIM_FLIRTY_HIGH if love >= 0.6 else ANIM_FLIRTY), GIGGLE
        elif slap or speed >= PAT_SPEED:
            mixer = (None, LAUGH)
        else:
            mixer = ANIM_SMILE, GIGGLE
        friendship, romance = 1 + 2 * s, (4 * s if intimate and romance_ok else 0)
    elif margin >= SURPRISED_TIER:
        tier = 'surprised'
        mixer = (ANIM_EMBARRASSED if intimate else None), SURPRISED
        friendship, romance = 0, 0
    elif margin >= DISPLEASED:
        tier = 'displeased'
        mixer = (ANIM_ANGRY if tempered else ANIM_EMBARRASSED_HIGH if intimate else ANIM_ANNOYED), HECKLE_LIGHT
        friendship, romance = -(2 + 4 * s), 0
    else:
        tier = 'angry'
        mixer = (ANIM_YELL if tempered or hit else ANIM_HORRIFIED if intimate else ANIM_ANGRY), HECKLE_STRONG
        friendship, romance = -(4 + 8 * s), (-4 * s if love > 0 else 0)
    if tier != 'welcome':
        friendship *= 1.0 + pain
    if native.setting_flag('physical', 'touch_relationship', True):
        _change(you, npc, friendship, romance)
    # Violent or unwelcome: whatever they were doing stops first. A casual touch
    # from someone they really like doesn't stop their chat: no body reaction
    # then, only the look at your hand (user 2026-10-26 23:55Z).
    violent = hit or slap
    liked = friend >= 0.5 or love >= 0.5
    keep_on = not violent and liked and tier in ('welcome', 'surprised') and reactions.busy_talking(npc)
    consequence = {
        'tier': tier, 'region': region, 's': s, 'intimate': intimate, 'how': how, 'speed': speed,
        'slap': slap, 'force': force, 'pain': pain, 'hit': hit, 'hard': hard, 'friend': friend, 'love': love, 'romance_ok': romance_ok, 'family': family,
        'restricted': restricted, 'mood': mood, 'traits': traits, 'tempered': tempered}
    summary = '{} {} on the {} ({:.2f} m/s): friendship {:.0f}, romance {:.0f}{}{} -> margin {:.2f} {}, ' \
              'friendship {:+.1f} romance {:+.1f}'.format(
                  how, npc.sim_info.first_name, region, speed, friend * 100, love * 100,
                  '' if romance_ok else ' (no romance)', ' [{}]'.format(', '.join(notes)) if notes else '',
                  margin, tier, friendship, romance)

    def act(npc=npc, mixer=mixer):
        global _why_not
        reacted = False
        if keep_on:
            _why_not = 'keeps talking (likes you, casual touch)'
        else:
            try:
                anim, fallback = mixer
                made = mixer_for_animation(anim) if anim else None
                mixer = made if made is not None else fallback
                reacted = _react(npc, you, mixer)
            except Exception as e:
                _error('reaction', e)
        _log('{}, reaction {}'.format(summary, getattr(mixer, '__name__', mixer) if reacted
                                      else 'none ({})'.format(_why_not)))
        if _you_busy(you) and not hit:
            _log('{}: you are on your way to something: no follow-up'.format(npc.sim_info.first_name))
            return
        try:
            reactions.consequence(you, npc, consequence)
        except Exception as e:
            _error('consequence', e)

    # Violent or unwelcome: they stop dead first (as Stop everything stops
    # you), then react; act() runs once they are clear.
    if not keep_on and ((hit and tier != 'welcome') or tier == 'angry') and native.setting_flag('physical', 'touch_interrupts', True):
        try:
            reactions.interrupt(npc, '{} {}'.format(how, region), act, you, spare_you=not hit)
        except Exception as e:
            _error('interrupt', e)
            act()
    else:
        act()
    return tier == 'welcome'


_anim_mixers = {}  # animation id -> reaction mixer playing it (None if it can't be made)


def mixer_for_animation(anim_id):
    """The giggle reaction mixer playing another animation instead: the same
    in-place one-shot (no tests, no loot), its animation_ref pointed at another
    AnimationElement (the Socials_Responses_* family it already uses, or
    others). Made once per animation; None if the game won't have it."""
    import copy
    if anim_id in _anim_mixers:
        return _anim_mixers[anim_id]
    cls = None
    try:
        base = _affordance(GIGGLE)
        anim = services.get_instance_manager(sims4.resources.Types.ANIMATION).get(anim_id)
        if base is None or anim is None:
            _log('animation {}: {}'.format(anim_id, 'no giggle mixer' if base is None else 'not installed'))
        else:
            ref = copy.copy(base.basic_content.animation_ref)
            for attr in ('factory', '_factory'):
                try:
                    setattr(ref, attr, anim)
                except Exception:
                    continue
                if getattr(ref, 'factory', None) is anim:
                    break
            if getattr(ref, 'factory', None) is anim:
                content = copy.copy(base.basic_content)
                content.animation_ref = ref
                cls = type('s4vr_reaction_{}'.format(anim.__name__), (base,), {'basic_content': content})
            else:
                _log('animation {}: cannot point the reaction at it ({})'.format(anim_id, type(ref).__name__))
    except Exception as e:
        _error('reaction for animation {}'.format(anim_id), e)
    _anim_mixers[anim_id] = cls
    return cls


def _react(npc, you, mixer_id):
    """Pushes a reaction mixer (tuning id or class) on `npc` targeting you, as
    the game's ReactionMixer does (interactions/utils/reactions.py)."""
    from autonomy.content_sets import get_valid_aops_gen
    from interactions.context import InteractionContext, InteractionSource, QueueInsertStrategy
    from interactions.interaction_finisher import FinishingType
    from interactions.priority import Priority
    global _why_not
    mixer = mixer_id if isinstance(mixer_id, type) else _affordance(mixer_id)
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


def _meant(off, speed):
    """A touch you meant, not brushing past (user 2026-10-27 01:35Z: accidental
    touches kept breaking up a WickedWhims scene): you looked about where it
    was ([physical] touch_look_angle degrees) and it wasn't the lightest
    brush ([physical] touch_min_speed m/s)."""
    try:
        angle = float(native.setting('physical', 'touch_look_angle', '50') or 50)
        least = float(native.setting('physical', 'touch_min_speed', '0.15') or 0.15)
    except ValueError:
        angle, least = 50.0, 0.15
    return (angle >= 180 or off <= angle) and speed >= least


def _in_scene(sim):
    """A WickedWhims animation it plays, heads for or has queued."""
    try:
        from sims4vr import fades
        if fades.in_scene(sim):
            return True
    except Exception:
        pass
    try:
        for si in list(sim.si_state) + list(sim.queue):
            name = getattr(getattr(si, 'affordance', None), '__name__', '') or ''
            module = type(si).__module__ or ''
            if 'WickedWhims' in name or 'wickedwhims' in module.lower():
                return True
    except Exception:
        pass
    return False


def _you_busy(you):
    """Your Sim has something running or queued (a walk to an action, a
    chat): a follow-up social from them would push into it."""
    try:
        keep = getattr(getattr(you, 'posture', None), 'source_interaction', None)
        for si in list(you.si_state) + list(you.queue):
            if si is keep or not getattr(si, 'visible', True) or getattr(si, 'is_finishing', False):
                continue
            return True
    except Exception:
        pass
    return False


TAP_GAP = 1.2  # seconds: two light taps this close are a "hey"
_taps = {}  # (sim id, kind) -> time of the last light tap or poke


def _tap(you, npc, kind, region, speed, now):
    """Two light taps (no buttons) or two pokes with the index finger (grip
    held) on a Sim facing away (user 2026-10-27 00:51Z): they stop and turn
    to face you (trail.py's stop-and-face, as when you walk up to them)."""
    if not native.setting_flag('gestures', 'tap_to_turn', True) or speed >= SLAP_SPEED:
        return
    if SENSITIVITY.get(region, 0.5) >= INTIMATE:
        return
    key = (npc.id, kind)  # two taps, or two pokes
    last = _taps.get(key)
    _taps[key] = now
    if last is None or now - last > TAP_GAP:
        return
    _taps.pop(key, None)
    import math
    p, q = you.position, npc.position
    dx, dz = p.x - q.x, p.z - q.z
    d = math.hypot(dx, dz)
    f = npc.forward
    if d > 1e-3 and (f.x * dx + f.z * dz) / d > 0.5:
        _log('{} already faces you'.format(npc.sim_info.first_name))
        return
    try:
        from sims4vr import trail
        trail._stop_for(you, npc)
        _log('{} {}: turns to face you'.format(npc.sim_info.first_name, 'poked twice' if kind == 6 else 'tapped twice'))
    except Exception as e:
        _error('turning them to you', e)


def tick(you):
    """Called by hands.py twenty times a second in first person."""
    grabs = native.setting_flag('physical', 'grab_reactions', True)
    touches = native.setting_flag('physical', 'touch_reactions', True)
    now = time.monotonic()
    try:
        _send_restricted(you, now)
    except Exception as e:
        _error('restricted Sims', e)
    try:
        from sims4vr import reactions
        reactions.tick(you)
    except Exception as e:
        _error('reactions', e)
    while True:
        event = native.touch_event()
        if event is None:
            break
        kind, hand, region_index, actor, value = event
        region = REGIONS[region_index] if 0 <= region_index < len(REGIONS) else 'body'
        try:
            if kind == 7:
                npc = _sim_for_actor(you, actor)
                if npc is not None:
                    from sims4vr import gestures
                    gestures.high_five_hit(you, npc, hand, value)
                continue
            if kind in (4, 6):
                # value = degrees off your look * 10 + m/s (armik.cpp)
                off, value = int(value // 10), value % 10
                if not _meant(off, value):
                    continue
                npc = _sim_for_actor(you, actor)
                if npc is not None and npc.sim_info.is_human:
                    _tap(you, npc, kind, region, value, now)
                if kind == 6 and SENSITIVITY.get(region, 0.5) < INTIMATE and value < PAT_SPEED:
                    continue  # a light poke on a neutral place only asks for attention (a fast one is a hit)
                kind = 4
            if kind in (1, 4):
                if not (grabs if kind == 1 else touches):
                    continue
                npc = _sim_for_actor(you, actor)
                if npc is None or not npc.sim_info.is_human:
                    _log('the {} you {} is no Sim near you'.format(region, 'hold' if kind == 1 else 'touch'))
                    continue
                if _in_scene(you) or _in_scene(npc):
                    _log('{}: in a WickedWhims scene (or heading for one): no reaction'.format(npc.sim_info.first_name))
                    continue
                if region not in SAFE_ON_RESTRICTED and _too_young(you, npc):
                    continue  # never there (the DLL refuses it too)
                if kind == 1:
                    if native.setting_flag('physical', 'haptics', True):
                        native.haptic(hand, 0.35, 0.06)
                    hold = {'sim_id': npc.id, 'region': region, 'since': now, 'judged': now, 'welcome': True}
                    _holds[hand] = hold
                if now - _last_judged.get(npc.id, -COOLDOWN) < COOLDOWN:
                    continue
                _last_judged[npc.id] = now
                welcome = _judge(you, npc, region, 'grab' if kind == 1 else 'touch', value if kind == 4 else 0.0)
                if kind == 1:
                    hold['welcome'] = welcome
            elif kind == 2:
                hold = _holds.pop(hand, None)
                if hold is not None:
                    _log('let go of the {} after {:.1f} s'.format(hold['region'], value))
            elif kind == 3 and grabs:
                hold = _holds.get(hand)
                npc = services.object_manager().get(hold['sim_id']) if hold else None
                if npc is not None and not (_in_scene(you) or _in_scene(npc)):
                    if native.setting_flag('physical', 'haptics', True):
                        native.haptic(hand, 0.6, 0.1)
                    _last_judged[npc.id] = now
                    hold['judged'] = now
                    hold['welcome'] = _judge(you, npc, hold['region'], 'pull')
        except Exception as e:
            _error('touch event {}'.format(kind), e)
    # Holding on: judged again every HOLD_ROUND seconds while it's welcome.
    if not grabs:
        _holds.clear()
        return
    for hand, hold in list(_holds.items()):
        if not hold['welcome'] or now - hold['judged'] < HOLD_ROUND:
            continue
        npc = services.object_manager().get(hold['sim_id'])
        if npc is None:
            _holds.pop(hand, None)
            continue
        hold['judged'] = now
        if _in_scene(you) or _in_scene(npc):
            continue
        _last_judged[npc.id] = now
        try:
            hold['welcome'] = _judge(you, npc, hold['region'], 'grab')
        except Exception as e:
            _error('holding on', e)
