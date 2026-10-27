"""What other Sims do about your touches, and how they show what they feel
about you (user 2026-10-26 22:29Z: "they just stand around and take it";
a Sim at -100 friendship stood smiling at you because it was happy).

Consequences (bodygrab._judge hands every judged touch to consequence()):
the game's own socials, done BY them TO you, picked by how they took the
touch, where, how hard, how they feel about you, their mood and traits,
family, age, and how often you did it lately:
- slapped or hit: slap back, start a fight (a hard hit, a hot-head, or the
  third hit), cry (sad, gloomy, children), run away (scared after repeats),
  or yell at you;
- an unwelcome intimate touch: chew you out (angry mood) or yell, insult;
  Sims around who see it react to it (reaction_Inappropriate) and judge you;
- welcome: a hug back (friends, family), flirt back, a flirty joke, a kiss
  on the cheek or a kiss (high romance), an embrace; a playful slap back
  from a friend you slapped in fun.
Children (and a teen with an adult) never get romance or fights: they cry,
tell you off or hug. Your own Sim is only on the receiving end (involuntary).
How: a social mixer needs them in a conversation with you, so if they aren't
in one the chat (sim_Chat) is pushed first and the mixer follows when it
runs; other reactions are super interactions pushed on them targeting you.

Feelings: a Sim close to you that looks at you ([physical] look_at_you)
shows what it feels about you now and then with a response animation
(angry, an eye roll, a smile, a flirty look), through bodygrab's reaction
mixers. Switches: [physical] touch_consequences, touch_fights,
touch_witnesses, show_feelings; [physical] consequence_chance.
"""
import math
import random
import time

import services
import sims4.resources

from sims4vr import native

# Socials they do to you (base game mixers).
SLAP = 25582  # mixer_social_SlapEmSilly_targeted_mischief_skills
YELL = 26151  # mixer_social_YellAT_targeted_mean
CHEW_OUT = 25885  # mixer_social_ChewOut_targeted_mean_emotionSpecific (angry)
FIGHT = 26138  # mixer_social_Fight_targeted_mean
INSULT = 25674  # mixer_social_Insult_Mean_STC
HUG = 316225  # mixer_social_Hug_Friendly_Middlescore_NoMoodTest
HUG_TESTED = 26914  # mixer_social_Hug_targeted_Friendly_MiddleScore
KISS = 28612  # mixer_social_Kiss_targeted_romance_middleScore
KISS_CHEEK = 97070  # mixer_social_KissCheek_targeted_romance_alwaysOn
EMBRACE = 26161  # mixer_social_Embrace_targeted_romance_middleScore_STC
FLIRT = 26162  # mixer_social_Flirt_targeted_romance_alwaysOn
FLIRTY_JOKE = 25876  # mixer_social_MakeAFlirtatiousJoke_targeted_romance_emotionSpecific
CHAT = 13998  # sim_Chat: the conversation a social needs
# Super interactions pushed on them, targeting you.
INAPPROPRIATE = 24338  # reaction_Inappropriate: a witness yells at you
SHOCK_REL_LOSS = 130397  # reaction_InappropriateBehavior_Shock_RelLoss
RUN_AWAY = 318284  # generic_RunAway_Scared
CRY = 139912  # reaction_Generic_Cry_WithAnims (a reaction mixer, bodygrab._react)

# Feelings shown when they look at you (Socials_Responses_* animations).
FEEL_HATE = 28336  # Angry_LowIntensity
FEEL_HATE_STRONG = 28337  # Angry_HighIntensity
FEEL_DISLIKE = 128766  # movie_Reactions_RollEyes (SP05), else Bored_LowIntensity 28353
FEEL_BORED = 28353
FEEL_LIKE = 28370  # Friendly_LowIntensity
FEEL_LOVE = 28365  # Flirty_LowIntensity

HARD_HIT = 3.0  # m/s into them: a hard hit (a slap is 1.8, bodygrab.SLAP_SPEED)
MEMORY = 90.0  # seconds a touch is remembered for repeats
COOLDOWN = 8.0  # seconds between consequences from one Sim
SOCIAL_WAIT = 6.0  # seconds a social waits for their chat with you to start
WITNESS_RANGE = 8.0
FEEL_RANGE = 2.2  # metres: close enough that they look at you
FEEL_EVERY = (9.0, 16.0)  # seconds between shown feelings, random in this range

_history = {}  # sim id -> [(time, tier, hit)]
_last_consequence = {}  # sim id -> time
_pending = []  # {'npc', 'mixers', 'why', 'until', 'chat'}
_feel_next = {}  # sim id -> time
_stopping = {}  # sim id -> {'at', 'reset_at', 'then', 'why'}
STOP_WAIT = 0.25  # seconds the cancels get before the reset
RESET_SETTLE = 0.3  # seconds after a reset before the reaction is pushed
# v0.13.578 crash (user 2026-10-27 13:07Z, slapping Amos): the hard stop
# cancelled his stand_Passive (his standing posture's interaction) and a chat
# was pushed on him in the same tick; the game's planner then crashed in its
# native goal code (placement.generate_routing_goals_for_polygon). Now postures
# are never cancelled and nothing new is pushed on a Sim until SETTLE seconds
# after its stop and its cancels have finished.
SETTLE = 0.75
_stopped_at = {}  # sim id -> time of its last hard stop
_later = []  # {'npc', 'run', 'why', 'until'}: pushes held until the Sim settles
_logs = 0
_errors = 0
_why_not = ''


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 400:
        native._log('reactions: ' + message)


def _error(where, e):
    global _errors
    _errors += 1
    if _errors <= 12:
        native._log('reactions: {} failed: {!r}'.format(where, e))


def _affordance(instance):
    return services.get_instance_manager(sims4.resources.Types.INTERACTION).get(instance)


def _chance():
    try:
        return max(0.0, float(native.setting('physical', 'consequence_chance', '1') or 1))
    except ValueError:
        return 1.0


def _roll(p):
    return random.random() < min(1.0, p * _chance())


def _context(npc):
    from interactions.context import InteractionContext, InteractionSource, QueueInsertStrategy
    from interactions.priority import Priority
    return InteractionContext(npc, InteractionSource.SCRIPT_WITH_USER_INTENT, Priority.High,
                              insert_strategy=QueueInsertStrategy.NEXT)


def _social_si(npc, you):
    """Their conversation with you, if they are in one."""
    for si in list(npc.si_state):
        group = getattr(si, 'social_group', None)
        if group is None:
            continue
        try:
            if you in list(group):
                return si
        except Exception:
            pass
    return None


def _try_mixer(npc, you, si, mixer_id):
    global _why_not
    from autonomy.content_sets import get_valid_aops_gen
    mixer = _affordance(mixer_id)
    if mixer is None:
        _why_not = 'no such social {}'.format(mixer_id)
        return False
    context = _context(npc)
    for aop, test_result in get_valid_aops_gen(you, mixer, si.super_affordance, si, context, False,
                                               push_super_on_prepare=False):
        if not test_result:
            _why_not = '{}: {}'.format(mixer.__name__, getattr(test_result, 'reason', test_result))
            continue
        if aop.execute(context):
            return True
    return False


def push_social(npc, you, mixers, why):
    """Queues one of `mixers` (social mixer ids, first that passes its tests)
    done by `npc` to you. Starts their chat with you if they aren't in one."""
    _pending.append({'npc': npc.id, 'mixers': tuple(mixers), 'why': why,
                     'until': time.monotonic() + SOCIAL_WAIT, 'chat': False})


def settling(npc):
    """Too soon after a hard stop to push anything new on them: SETTLE
    seconds, and until what was cancelled has finished."""
    at = _stopped_at.get(npc.id)
    if at is None:
        return False
    if time.monotonic() - at < SETTLE:
        return True
    try:
        if any(getattr(si, 'is_finishing', False) and not _is_posture(npc, si) for si in list(npc.si_state)):
            return time.monotonic() - at < SOCIAL_WAIT
    except Exception:
        pass
    _stopped_at.pop(npc.id, None)
    return False


def push_si(npc, target, si_id, why):
    if settling(npc):
        _later.append({'npc': npc.id, 'target': target.id, 'si': si_id, 'why': why,
                       'until': time.monotonic() + SOCIAL_WAIT})
        return True
    affordance = _affordance(si_id)
    if affordance is None:
        _log('{}: no interaction {}'.format(why, si_id))
        return False
    try:
        result = npc.push_super_affordance(affordance, target, _context(npc))
    except Exception as e:
        _error(why, e)
        return False
    _log('{} {}: {}'.format(npc.sim_info.first_name, why, 'pushed' if result else 'refused ({})'.format(
        getattr(result, 'reason', result))))
    return bool(result)


def _run_pending(you):
    now = time.monotonic()
    for job in list(_pending):
        npc = services.object_manager().get(job['npc'])
        if npc is None or now > job['until']:
            _pending.remove(job)
            if npc is not None:
                _log('{} {}: never happened ({})'.format(npc.sim_info.first_name, job['why'], _why_not))
            continue
        if settling(npc):
            continue
        si = _social_si(npc, you)
        if si is None:
            if not job['chat']:
                job['chat'] = True
                chat = _affordance(CHAT)
                if chat is not None:
                    try:
                        npc.push_super_affordance(chat, you, _context(npc))
                    except Exception as e:
                        _error('chat', e)
            continue
        for mixer_id in job['mixers']:
            try:
                if _try_mixer(npc, you, si, mixer_id):
                    _pending.remove(job)
                    _log('{} {} ({})'.format(npc.sim_info.first_name, job['why'],
                                             getattr(_affordance(mixer_id), '__name__', mixer_id)))
                    break
            except Exception as e:
                _error(job['why'], e)
        else:
            _pending.remove(job)
            _log('{} {}: no social passed ({})'.format(npc.sim_info.first_name, job['why'], _why_not))


def interrupt(npc, why, then, you=None, spare_you=True):
    """Stops them dead, then runs then() (user 2026-10-27 00:50Z: a hard stop
    like the debug menu's Stop everything, not a cancel request): every action
    running or queued is cancelled at once (halt._cancel: ignore_must_run,
    immediate; socials too), they leave their social groups the game's way
    (SocialGroup.remove, as halt's full stop), and if anything still runs a
    moment later the game resets them (Sim.reset, RESET_EXPECTED: they stand
    where they are, as halt does for you). What nobody may cancel (passing
    out, wetting themselves) is left to play."""
    from sims4vr import halt
    cancelled = []
    # Never your own actions (user 2026-10-27 01:41Z: brushing past someone on
    # the way to something cancelled your queue): only what is theirs, and
    # unless you hit them, nothing they do with you either (a shared transition
    # or social would take yours down with it), and no reset.
    for si in list(halt._doing(npc)):
        if si is None or getattr(si, 'is_finishing', False) or halt._involuntary(si) or _is_posture(npc, si):
            continue
        if getattr(si, 'sim', npc) is not npc:
            continue
        if you is not None and spare_you and _involves(si, you):
            continue
        halt._cancel(si, halt._finishing('USER_CANCEL'), 'stopped by your touch', cancelled)
    for group in list(npc.get_groups_for_sim_gen()):
        try:
            if you is not None and spare_you and you in list(group):
                continue
            group.remove(npc)
        except Exception as e:
            _error('leaving {}'.format(group), e)
    _stopped_at[npc.id] = time.monotonic()
    _stopping[npc.id] = {'at': time.monotonic(), 'reset_at': None, 'then': then, 'why': why,
                         'no_reset': you is not None and (spare_you or _tied(npc, you))}
    _log('{} stops dead ({}): cancelled {}'.format(
        npc.sim_info.first_name, why, ', '.join(type(si).__name__ for si in cancelled[:5]) or 'nothing'))


def _involves(si, you):
    if getattr(si, 'target', None) is you:
        return True
    group = getattr(si, 'social_group', None)
    try:
        if group is not None and you in list(group):
            return True
    except Exception:
        pass
    try:
        return you in tuple(si.required_sims())
    except Exception:
        return False


def _tied(npc, you):
    """You are in something with them (a group, an action of yours on them):
    a reset of theirs would reset you too."""
    try:
        for group in npc.get_groups_for_sim_gen():
            if you in list(group):
                return True
        for si in list(you.si_state) + list(you.queue):
            if getattr(si, 'target', None) is npc:
                return True
    except Exception:
        return True
    return False


def _is_posture(npc, si):
    """An interaction holding one of their postures (standing included:
    stand_Passive, sim-stand): cancelling it leaves them in a posture with no
    owner."""
    try:
        from postures.stand import StandSuperInteraction
        if isinstance(si, StandSuperInteraction):
            return True
    except Exception:
        pass
    if getattr(getattr(si, 'affordance', None), 'provided_posture_type', None) is not None:
        return True
    state = getattr(npc, 'posture_state', None)
    for name in ('body', 'left', 'right'):
        aspect = getattr(state, name, None)
        if aspect is not None and si is getattr(aspect, 'source_interaction', None):
            return True
    return False


def _run_later():
    now = time.monotonic()
    for job in list(_later):
        npc = services.object_manager().get(job['npc'])
        target = services.object_manager().get(job['target'])
        if npc is None or target is None or now > job['until']:
            _later.remove(job)
            continue
        if settling(npc):
            continue
        _later.remove(job)
        push_si(npc, target, job['si'], job['why'])


def _still_busy(npc):
    from sims4vr import halt
    for si in list(halt._doing(npc)):
        if si is None or getattr(si, 'is_finishing', False) or halt._involuntary(si) or _is_posture(npc, si):
            continue
        if not getattr(si, 'visible', True):
            continue  # the game's own idles while standing
        return True
    return False


def _run_stops():
    """Each stopped Sim: clear -> its reaction; still busy after STOP_WAIT ->
    the reset, and its reaction RESET_SETTLE after that."""
    now = time.monotonic()
    for sim_id, entry in list(_stopping.items()):
        npc = services.object_manager().get(sim_id)
        if npc is None:
            _stopping.pop(sim_id, None)
            continue
        if entry['reset_at'] is None:
            if _still_busy(npc) and now - entry['at'] < STOP_WAIT:
                continue
            if _still_busy(npc) and not entry.get('no_reset'):
                try:
                    from objects.object_enums import ResetReason
                    npc.reset(ResetReason.RESET_EXPECTED, None, 'stopped by your touch')
                    _log('{} still busy: reset (stands where they are)'.format(npc.sim_info.first_name))
                except Exception as e:
                    _error('reset', e)
                entry['reset_at'] = now
                continue
        elif now - entry['reset_at'] < RESET_SETTLE:
            continue
        _stopping.pop(sim_id, None)
        try:
            entry['then']()
        except Exception as e:
            _error('reaction after the stop', e)


def busy_talking(npc):
    for si in list(npc.si_state):
        if getattr(si, 'social_group', None) is not None:
            return True
    return False


def _remember(npc, tier, hit, now):
    entries = [e for e in _history.get(npc.id, ()) if now - e[0] < MEMORY]
    entries.append((now, tier, hit))
    _history[npc.id] = entries
    bad = sum(1 for e in entries if e[1] in ('displeased', 'angry'))
    hits = sum(1 for e in entries if e[2])
    return bad, hits


def repeats(npc):
    """Unwelcome touches and hits in the last MEMORY seconds, before this one."""
    now = time.monotonic()
    entries = [e for e in _history.get(npc.id, ()) if now - e[0] < MEMORY]
    return sum(1 for e in entries if e[1] in ('displeased', 'angry')), sum(1 for e in entries if e[2])


def _has(words, traits):
    return any(any(w in t for t in traits) for w in words)


def _witnesses(you, npc, now):
    """Adults nearby facing the scene react to an unwelcome intimate touch."""
    if not native.setting_flag('physical', 'touch_witnesses', True):
        return
    from sims.sim_info_types import Age
    p = npc.position
    count = 0
    for sim in services.sim_info_manager().instanced_sims_gen():
        if sim is you or sim is npc or not sim.sim_info.is_human or sim.sim_info.age in (Age.BABY, Age.TODDLER):
            continue
        q = sim.position
        d = math.hypot(q.x - p.x, q.z - p.z)
        if d > WITNESS_RANGE or d < 0.3:
            continue
        f = sim.forward
        if (f.x * (p.x - q.x) + f.z * (p.z - q.z)) / d < 0.3:  # not looking that way
            continue
        if push_si(sim, you, INAPPROPRIATE, 'saw it and calls you out'):
            count += 1
        if count >= 2:
            break


# Moodlets a hit or an unwelcome touch gives (base game buffs with a timer,
# shown with the reason "<what> by <you>"): user 2026-10-27 13:03Z wants
# them scared, angry and/or sad by relationship and personality.
BUFF_ANGRY = 296363  # buff_Scenario_InTheMoodlet_Mood_Angry "Angered" (Angry 2, 2 h)
BUFF_HURT = 247583  # buff_Sentiment_Hurt_Sad_Primary_generic "Feeling Hurt" (Sad 2, 2 h)
BUFF_SCARED = 253288  # buff_Sim_Scared_ScreamIncoherently_IrrationalDanger "Irrational Danger!" (Scared 2)
BUFF_EMBARRASSED = 296362  # buff_Scenario_InTheMoodlet_Mood_Embarrassed "Embarrassed" (2 h)
BUFF_TENSE = 296365  # buff_Scenario_InTheMoodlet_Mood_Tense "Stressed" (2 h)
TIMID = ('squeamish', 'loner', 'paranoid', 'insecure', 'cowardly', 'shy')
SOFT = ('sensitive', 'gloomy', 'romantic', 'family', 'loyal')


def _buff(npc, buff_id, reason):
    from sims4.localization import LocalizationHelperTuning
    buff = services.get_instance_manager(sims4.resources.Types.BUFF).get(buff_id)
    if buff is None:
        return None
    npc.sim_info.add_buff_from_op(buff, buff_reason=LocalizationHelperTuning.get_raw_text(reason))
    return buff.__name__


def _moodlets(you, npc, c, hit, hits):
    """Angry, sad (hurt) and/or scared after a hit; embarrassed (and angry)
    after an unwelcome intimate touch. Who they are decides which:
    - hot-headed, mean or already angry, or they dislike you: angry;
    - they like or love you, or are sensitive/gloomy, or a child: hurt (sad),
      and angry too if hot-headed;
    - timid (squeamish, loner, paranoid...), or hit hard or again by someone
      they don't like and they aren't hot-headed: scared."""
    if not native.setting_flag('physical', 'touch_moodlets', True):
        return
    tier = c['tier']
    if not (hit and tier != 'welcome') and tier not in ('displeased', 'angry'):
        return
    traits, mood = c['traits'], c['mood']
    you_name = you.sim_info.first_name
    what = 'Yanked' if c['how'] == 'pull' and hit else 'Hit' if c['hard'] else 'Slapped' if hit else \
        'Groped' if c['intimate'] else 'Touched'
    reason = '{} by {}'.format(what, you_name)
    close = c['friend'] >= 0.4 or c['love'] >= 0.4 or c['family']
    tempered = c['tempered'] or 'angry' in mood
    timid = _has(TIMID, traits) or 'tense' in mood or 'scared' in mood
    given = []
    if hit:
        if c['restricted'] or close or _has(SOFT, traits):
            given.append(_buff(npc, BUFF_HURT, reason))
        if tempered or c['friend'] < 0 or not given:
            given.append(_buff(npc, BUFF_ANGRY, reason))
        if not c['tempered'] and (timid or (c['friend'] < 0.1 and (c['hard'] or hits >= 2))):
            given.append(_buff(npc, BUFF_SCARED, reason))
    else:
        if c['intimate']:
            given.append(_buff(npc, BUFF_EMBARRASSED, reason))
        if tier == 'angry' or tempered:
            given.append(_buff(npc, BUFF_ANGRY, reason))
        elif not c['intimate']:
            given.append(_buff(npc, BUFF_TENSE, reason))
    given = [g for g in given if g]
    if given:
        _log('{} gets {} ({})'.format(npc.sim_info.first_name, ', '.join(given), reason))


def consequence(you, npc, c):
    """What they do about a judged touch. c: tier, region, s, intimate, how,
    speed, slap, friend, love, romance_ok, family, restricted, mood, traits,
    tempered."""
    if not native.setting_flag('physical', 'touch_consequences', True):
        return
    now = time.monotonic()
    hit = c['hit']  # by pain (bodygrab.force_of), not speed alone
    bad, hits = _remember(npc, c['tier'], hit, now)
    try:
        _moodlets(you, npc, c, hit, hits)
    except Exception as e:
        _error('moodlets', e)
    if now - _last_consequence.get(npc.id, -COOLDOWN) < COOLDOWN:
        return
    traits, mood, tier = c['traits'], c['mood'], c['tier']
    hard = c['hard']
    sad = 'sad' in mood or _has(('gloomy', 'sensitive'), traits)
    scared = 'scared' in mood or 'tense' in mood
    fights = native.setting_flag('physical', 'touch_fights', True) and not c['restricted']
    did = None
    if tier in ('angry', 'displeased') and hit:
        if c['restricted'] or (sad and not c['tempered']):
            did = 'cries'
            from sims4vr import bodygrab
            bodygrab._react(npc, you, CRY)
        elif fights and (hard or hits >= 3 or (c['tempered'] and hits >= 2)) and (c['friend'] < 0.3 or c['tempered']):
            did = 'fights you'
            push_social(npc, you, (FIGHT, SLAP, YELL), 'fights you')
        elif scared and hits >= 2 and not c['tempered']:
            did = 'runs away'
            push_si(npc, you, RUN_AWAY, 'runs away from you')
        elif tier == 'angry' and _roll(0.6 if c['tempered'] or c['friend'] < 0 else 0.35):
            did = 'slaps back'
            push_social(npc, you, (SLAP, YELL), 'slaps you back')
        elif _roll(0.7):
            did = 'yells'
            push_social(npc, you, (CHEW_OUT, YELL) if 'angry' in mood else (YELL,), 'yells at you')
    elif tier == 'angry':
        if c['intimate'] and not c['restricted']:
            if c['slap'] and not c['family'] and _roll(0.4):
                # A slap there isn't an assault but it is forward: the classic answer.
                did = 'slaps back'
                push_social(npc, you, (SLAP, YELL), 'slaps you for it')
            elif c['family'] or _roll(0.8):
                did = 'chews out'
                push_social(npc, you, (CHEW_OUT, YELL, INSULT) if 'angry' in mood else (YELL, INSULT),
                            'tells you off')
            if not c['restricted']:
                _witnesses(you, npc, now)
        elif bad >= 2 and _roll(0.7):
            did = 'yells'
            push_social(npc, you, (YELL, INSULT), 'has had enough of you')
        elif _roll(0.4):
            did = 'insults'
            push_social(npc, you, (INSULT, YELL), 'insults you')
    elif tier == 'displeased':
        if bad >= 2 and _roll(0.6):
            did = 'yells'
            push_social(npc, you, (YELL, INSULT), 'asks you to stop')
        elif c['intimate'] and not c['restricted'] and _roll(0.4):
            did = 'shocked'
            push_si(npc, you, SHOCK_REL_LOSS, 'is shocked at you')
    elif tier == 'welcome':
        playful = _has(('goofball', 'cheerful', 'outgoing'), traits) or 'playful' in mood
        if c['slap'] and playful and c['friend'] >= 0.3 and not c['restricted'] and _roll(0.35):
            did = 'playful slap'
            push_social(npc, you, (SLAP,), 'slaps you back for fun')
        elif c['romance_ok'] and c['love'] >= 0.6 and c['intimate'] and _roll(0.35):
            did = 'kisses'
            push_social(npc, you, (KISS, EMBRACE, FLIRT), 'kisses you')
        elif c['romance_ok'] and c['love'] >= 0.3 and c['region'] in ('face', 'neck', 'hair') and _roll(0.4):
            did = 'kiss on the cheek'
            push_social(npc, you, (KISS_CHEEK, EMBRACE, FLIRT), 'kisses your cheek')
        elif c['romance_ok'] and (c['love'] >= 0.3 or 'flirty' in mood) and c['s'] >= 0.4 and _roll(0.4):
            did = 'flirts'
            push_social(npc, you, (FLIRT, FLIRTY_JOKE), 'flirts back')
        elif not c['slap'] and (c['friend'] >= 0.5 or c['love'] >= 0.3 or (c['family'] and c['friend'] >= 0.2)) and \
                c['region'] in ('shoulder', 'back', 'upper arm', 'waist', 'hand') and _roll(0.25):
            did = 'hugs'
            push_social(npc, you, (HUG, HUG_TESTED), 'hugs you')
    if did:
        _last_consequence[npc.id] = now
        _log('{} {} ({} on the {}, {:.1f} m/s; {} unwelcome, {} hits lately)'.format(
            npc.sim_info.first_name, did, tier, c['region'], c['speed'], bad, hits))


def _feeling(you, npc):
    """The animation that shows how they feel about you now, or None."""
    from sims4vr import bodygrab as bg
    friend, love = bg._scores(you, npc)
    romance_ok = not bg._too_young(you, npc) and not bg._family(you, npc) and bg._attracted(you, npc)
    if not romance_ok:
        love = 0.0
    feel = friend + max(love, 0.0) * 0.5 if friend >= 0 else friend
    mood, traits = bg._mood(npc), bg._traits(npc)
    bad, hits = repeats(npc)
    feel -= 0.15 * bad
    if feel <= -0.5:
        strong = feel <= -0.8 and ('angry' in mood or _has(bg.TEMPERED, traits))
        return 'hates you', FEEL_HATE_STRONG if strong else FEEL_HATE
    if feel <= -0.15:
        return 'dislikes you', FEEL_DISLIKE
    if romance_ok and (love >= 0.4 or ('flirty' in mood and love >= 0.15)):
        return 'loves you', FEEL_LOVE
    if friend >= 0.3:
        return 'likes you', FEEL_LIKE
    return None, None


def _show_feelings(you, now):
    if not (native.setting_flag('physical', 'show_feelings', True) and
            native.setting_flag('physical', 'look_at_you', True)):
        return
    from sims4vr import bodygrab as bg
    p, eye = you.position, you.position
    for sim in services.sim_info_manager().instanced_sims_gen():
        if sim is you or not sim.sim_info.is_human:
            continue
        q = sim.position
        d = math.hypot(q.x - p.x, q.z - p.z)
        if d > FEEL_RANGE or d < 0.2:
            _feel_next.pop(sim.id, None)
            continue
        f = sim.forward
        if (f.x * (eye.x - q.x) + f.z * (eye.z - q.z)) / d < 0.5:  # not looking at you
            continue
        due = _feel_next.get(sim.id)
        if due is None:
            _feel_next[sim.id] = now + random.uniform(0.8, 2.0)  # soon after they first look at you
            continue
        if now < due:
            continue
        _feel_next[sim.id] = now + random.uniform(*FEEL_EVERY)
        try:
            what, anim = _feeling(you, sim)
            if anim is None:
                continue
            mixer = bg.mixer_for_animation(anim)
            if mixer is None and anim == FEEL_DISLIKE:
                mixer = bg.mixer_for_animation(FEEL_BORED)
            if mixer is not None and bg._react(sim, you, mixer):
                _log('{} {} (shows it)'.format(sim.sim_info.first_name, what))
        except Exception as e:
            _error('feelings', e)


def tick(you):
    """bodygrab.tick calls this twenty times a second in first person."""
    now = time.monotonic()
    if _stopping:
        try:
            _run_stops()
        except Exception as e:
            _error('stops', e)
    if _later:
        try:
            _run_later()
        except Exception as e:
            _error('held pushes', e)
    if _pending:
        try:
            _run_pending(you)
        except Exception as e:
            _error('socials', e)
    try:
        _show_feelings(you, now)
    except Exception as e:
        _error('feelings', e)
