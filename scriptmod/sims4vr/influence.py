"""First person: Influence (user 2026-10-28 18:01Z). Have another household Sim do what you would
otherwise switch to them for.

On the pie of a household Sim you don't play (first person only): "Influence to...". It opens that
Sim's own pie menu as if you had selected them: the game builds the menu with that Sim as the actor
(interactions.choices asks _active_sim(client) for it; while we influence, that one answer is the
influenced Sim), and a pick comes back through Client.select_interaction with the choice menu of
that Sim, so the game queues it on them as a pie menu pick, like any Sim of yours.

The menu has one more entry at the top: "Try...". It makes the Sim answer first (below), and then,
for the next click on an object or Sim in the world, the same override is on: the pie that opens is
the influenced Sim's, for that target (cooking, the pool, a seat, a chat, a hug...). A pick (or
influence_timeout seconds, leaving first person, the Sim gone) ends it.

While choosing, the Sim looks where your laser ends (armik.cpp setInfluenceLook, the same eye and
head turning as when you touch a Sim): eyes only while the point is within look_eyes metres of their
eyes, the head from look_head metres. A point further round than body_angle degrees from where they
face turns their body (facing.turn, the game's social adjustment) - but never further than
face_keep degrees away from you, so they can always see you point (user: "how would they be able to
see where I am pointing"). The head follows the point as far as the head can turn; it is the body
that is held back.

The answer: when you press Try..., the Sim reacts by how they feel about you (friendship, plus half
of a positive romance, as reactions._feeling reads it) and their mood. Willing: a happy nod, and the
choosing starts. Unwilling (friendship below min_friendship, or a bad mood unless you are closer than
mood_override): an annoyed shake of the head, and nothing is chosen. Their autonomy is untouched; only
what you pick runs, like a pick on any Sim of yours (a follower's hold gives way for it).

No walking is scripted: whatever you pick is what the game does for that Sim (walking to the
object included, as for a Sim you play it is the Sim's own interaction). Nothing changes for your own
Sim. First person only.

Switches ([influence], debug menu Options > Game settings > Influencing Sims): on, min_friendship,
mood_override, bad_moods, look_eyes, look_head, body_angle, face_keep, timeout.
"""
import math
import time

import alarms
import clock
import services

from sims4vr import hooks, native

TICK_SECONDS = 0.25
GUID_INFLUENCE = 0x5653494E464C5545  # our interaction's id (not in the game's tuning)

# Reaction animations (tuning ids, first that plays): a happy nod, an annoyed shake of the head.
NODS = (458578,   # EP20World_HappyNod
        24413,    # Social_Responses_Compassion_UnderstandingNod
        28370)    # Socials_Responses_Friendly_LowIntensity
SHAKES = (11802,   # Social_Responses_Annoyed_HeadShake
          167310,  # discipline_TimeOut_ShakeHead
          28336)   # Socials_Responses_Angry_LowIntensity

_owner = type('_Owner', (), {})()
_alarm = None
_sim = None          # the influenced Sim
_opening = False     # building the influenced Sim's pie right now
_menu = False        # that pie is up (it carries Try...)
_choosing = False    # Try... was answered yes: the next click opens the Sim's pie
_since = 0.0
_seen = set()      # Sims whose pie option was logged
_body = {}           # Sim id -> [when last sent, yaw sent]
_aff = None
_wrapped = False
_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 200:
        native._log('influence: ' + message)


def _note_error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        _log('{} failed: {!r}'.format(what, e))


def _number(key, default):
    try:
        return float(native.setting('influence', key, default))
    except (TypeError, ValueError):
        return float(default)


def _on():
    return native.setting_flag('influence', 'on', True)


def _name(sim):
    try:
        return sim.sim_info.first_name
    except Exception:
        return str(sim)


# --- state ------------------------------------------------------------------------------

def opening():
    """pie.py: the pie being built is the influenced Sim's own (it gets the Try... entry)."""
    return _opening


def influenced():
    return _sim


def _override():
    """The Sim that answers for _active_sim(client) now, or None."""
    if _sim is None or not (_opening or _choosing):
        return None
    return _sim


def _end(why):
    global _sim, _opening, _menu, _choosing, _alarm
    sim = _sim
    if sim is None and not _choosing and _alarm is None:
        return
    _sim, _opening, _menu, _choosing = None, False, False, False
    alarm, _alarm = _alarm, None
    if alarm is not None:
        try:
            alarms.cancel_alarm(alarm)
        except Exception:
            pass
    _body.clear()
    native.influence_look(0)
    if sim is not None:
        _log('{}: influence ends ({})'.format(_name(sim), why))


def start():
    """body.py: first person started."""
    global _aff, _wrapped
    if _wrapped:
        return
    _wrapped = True
    try:
        from sims4vr import follow
        _aff = follow._make_affordance('Influence to...', GUID_INFLUENCE,
                                       lambda target, you: _can_influence(target, you), action=begin)
        if _aff is None or not _wrap_sim_affordances():
            return
        from server.client import Client
        from server_commands import interaction_commands
        hooks.hook('influence', interaction_commands, '_active_sim', _active_sim)
        hooks.hook('influence', Client, 'select_interaction', _select_interaction)
        _log('ready')
    except Exception as e:
        _note_error('start', e)


def stop():
    """body.py: first person ended."""
    _end('first person ended')


# --- the pie option ---------------------------------------------------------------------

def _can_influence(target, you):
    if not _on() or target is None or target is you or not getattr(target, 'is_sim', False):
        return False
    try:
        mine = target.sim_info.household_id == you.sim_info.household_id
        if not mine and not native.setting_flag('influence', 'household_only', True):
            mine = True
        if target.id not in _seen:  # once per Sim: why the option is or isn't on their pie
            _seen.add(target.id)
            _log('{}: option {} (household {} vs yours {})'.format(
                _name(target), 'offered' if mine else 'NOT offered, not in your household',
                target.sim_info.household_id, you.sim_info.household_id))
        return mine
    except Exception as e:
        _note_error('household check', e)
        return False


def _wrap_sim_affordances():
    """Household Sims list "Influence to..." among their pie options (as follow.py lists Follow me)."""
    from sims.sim import Sim
    from sims4.utils import flexmethod
    raw = Sim.__dict__.get('super_affordances')
    if raw is None:
        _log('Sim.super_affordances not found; no Influence option')
        return False

    @flexmethod
    def super_affordances(cls, inst, *args, **kwargs):
        yield from raw.__get__(inst, cls)(*args, **kwargs)
        try:
            if inst is None or _aff is None:
                return
            context = args[0] if args else kwargs.get('context')
            from sims4vr import body
            fp = body.following()
            if not fp or getattr(getattr(context, 'sim', None), 'id', None) != fp or inst.id == fp:
                return
            yield _aff
        except Exception:
            pass

    Sim.super_affordances = super_affordances
    return True


def begin(target):
    """"Influence to..." picked: that Sim's own pie, as if you had selected them."""
    global _sim, _opening, _menu, _choosing, _since
    try:
        if not _on():
            return
        _end('another Sim')
        from sims4vr import pie
        _sim, _opening, _menu, _choosing, _since = target, True, False, False, time.monotonic()
        try:
            count = pie.open_for(target)
        finally:
            _opening = False
        if not count:
            _log('{}: no pie menu to show'.format(_name(target)))
            _end('nothing to pick')
            return
        _menu = True
        _log('{}: their pie menu is up ({} options)'.format(_name(target), count))
    except Exception as e:
        _note_error('begin', e)
        _end('failed')


def _active_sim(original, client):
    sim = _override()
    if sim is not None:
        return sim
    return original(client)


def _select_interaction(original, self, choice_id, revision, *args, **kwargs):
    """Something picked from a pie: when it is the influenced Sim's, what comes of it passes the hold
    of a follower (follow.py) and the influence is over."""
    sim = _sim
    mine = False
    try:
        menu = self.choice_menu
        owner = menu.simref() if menu is not None and getattr(menu, 'simref', None) is not None else None
        mine = sim is not None and owner is sim and revision == menu.revision
        if mine:
            from sims4vr import follow
            follow._picked[sim.id] = time.monotonic()
            if not _gate(sim, menu, choice_id):
                self.set_choices(None)  # the pick is dropped; the answer is their reaction
                _end('refused')
                return None
    except Exception as e:
        _note_error('pick', e)
    try:
        return original(self, choice_id, revision, *args, **kwargs)
    finally:
        if mine:
            _log('{}: you picked something for them'.format(_name(sim)))
            _end('picked')


# --- social picks: accept or reject, with a reaction -----------------------------------

# Reactions by kind (tuning ids of animations, first that plays).
YES = {'intimate': (28366, 28365), 'romance': (28365, 28366), 'friendly': (28370,),
       'prank': (28375, 28370), 'prank_evil': (28391, 28375), 'mean': (28336,)}
NO = {'intimate': (11802, 28356), 'romance': (28356, 11802), 'friendly': (128766, 28353),
      'prank': (28336, 11802), 'mean': (28336,)}
NO_ANGRY = (28337, 28336)  # a strong refusal from a Sim in a temper
SAYS_NO = {'intimate': "No. I'm not into that.", 'romance': "Sorry, I don't feel that way.",
           'prank': "Not funny. Stop it.", 'friendly': "Not now, please."}  # what a refusing Sim says (first person)
EVIL_TRAITS = ('mischievous', 'evil', 'mean', 'goofball')
GOOD_FOR_ROMANCE = ('flirty', 'playful', 'confident', 'happy')

# Name pieces (affordance name + its category tags, lowercase) by kind, first match wins.
KINDS = (
    ('intimate', ('sex', 'woohoo', 'strip', 'nude', 'naked', 'masturb', 'orgasm', 'makeamove', 'make_a_move',
                  'makemove', 'ww_', 'wickedwhims', 'seduc', 'makeout', 'sext')),
    ('romance', ('flirt', 'romanc', 'kiss', 'embrace', 'caress', 'propose', 'smooch', 'sweettalk', 'cuddle',
                 'lovey', 'steamy', 'massage')),
    ('mean', ('insult', 'yell', 'slap', 'fight', 'chewout', 'argue', 'threat', 'mean_', '_mean', 'punch')),
    ('prank', ('prank', 'mischief', 'tease', 'trick', 'scare', 'whoopee')),
    ('friendly', ('hug', 'friendly', 'chat', 'joke', 'compliment', 'highfive', 'greet', 'tellstory', 'praise',
                  'handshake', 'cheer', 'laugh', 'thank')),
)


def _kind(aop):
    affordance = getattr(aop, 'affordance', None)
    text = getattr(affordance, '__name__', '').lower()
    try:
        text += ' ' + ' '.join(str(t).lower() for t in (getattr(affordance, 'interaction_category_tags', None) or ()))
    except Exception:
        pass
    for kind, pieces in KINDS:
        if any(p in text for p in pieces):
            return kind
    return None


def _decide(kind, responder, other):
    """(accepts, why, angry): how `responder` takes `kind` from `other`: relationship (theirs toward
    other), mood, traits, their orientation, ages and family for romance."""
    from sims4vr import bodygrab as bg
    friend, love = bg._scores(other, responder)
    friend, love = friend * 100.0, love * 100.0
    mood, traits = bg._mood(responder), bg._traits(responder)
    angry = 'angry' in mood or any(w in t for t in traits for w in ('hotheaded', 'mean'))
    bad = any(m in mood for m in ('angry', 'tense', 'sad', 'uncomfortable', 'embarrassed'))
    good = any(m in mood for m in GOOD_FOR_ROMANCE)
    if kind in ('romance', 'intimate'):
        if bg._too_young(other, responder):
            return False, 'too young', angry
        if bg._family(other, responder):
            return False, 'family', angry
        if not bg._attracted(other, responder):
            return False, 'not attracted to their gender', angry
        need = _number('intimate_min' if kind == 'intimate' else 'romance_min', 40 if kind == 'intimate' else 15)
        if good:
            need -= 10
        if bad:
            need += 30
        if love < need:
            return False, 'romance {:.0f} < {:.0f} (mood {})'.format(love, need, mood or '?'), angry
        return True, 'romance {:.0f}, mood {}'.format(love, mood or '?'), angry
    if kind == 'mean':
        return True, 'mean', angry
    need = _number('friendly_min', -10)
    if kind == 'prank' and any(w in t for t in traits for w in ('goofball', 'mischievous', 'playful')):
        need -= 20
    if bad and friend < _number('mood_override', 60):
        return False, 'mood {} at friendship {:.0f}'.format(mood, friend), angry
    if friend < need:
        return False, 'friendship {:.0f} < {:.0f}'.format(friend, need), angry
    return True, 'friendship {:.0f}, mood {}'.format(friend, mood or '?'), angry


def _gate(actor, menu, choice_id):
    """False: this pick is refused (a reaction was played). Judges a social the influenced Sim picks
    toward another Sim by its kind; true for everything else."""
    if not native.setting_flag('influence', 'gate_socials', True):
        return True
    selection = menu.menu_items.get(choice_id)
    aop = getattr(selection, 'aop', None)
    if aop is None:
        return True
    target = getattr(aop, 'target', None)
    kind = _kind(aop)
    if kind is None or target is None or target is actor or not getattr(target, 'is_sim', False):
        return True
    from sims4vr import follow
    you = follow._fp_sim()
    if target is you:
        judges = [(actor, you)]  # you are the target: the Sim you influence decides for themself
    elif kind in ('romance', 'intimate'):
        judges = [(target, actor), (actor, target)]
    else:
        judges = [(target, actor)]
    name = getattr(aop.affordance, '__name__', '?')
    for responder, other in judges:
        ok, why, angry = _decide(kind, responder, other)
        if ok or _ignore():
            continue
        _log('{} refuses {} ({}): {}'.format(_name(responder), name, kind, why))
        ids = NO_ANGRY if angry and kind in ('intimate', 'prank') else NO.get(kind, NO['friendly'])
        _react(responder, other, ids, 'refusal')
        _tell(responder, SAYS_NO.get(kind, "I'd rather not."))
        return False
    responder = actor if target is you else target
    other = you if target is you else actor
    ids = YES.get(kind, ())
    if kind == 'prank':
        from sims4vr import bodygrab as bg
        if any(w in t for t in bg._traits(responder) for w in EVIL_TRAITS):
            ids = YES['prank_evil']
    _log('{} accepts {} ({})'.format(_name(responder), name, kind))
    if ids:
        _react(responder, other, ids, 'acceptance')
    return True


# --- Try... ------------------------------------------------------------------------------

def _willing(you, sim):
    """(willing, why): friendship with you (and half a positive romance) and their mood."""
    from sims4vr import bodygrab as bg
    friend, love = bg._scores(you, sim)
    feel = (friend + max(love, 0.0) * 0.5 if friend >= 0 else friend) * 100.0
    mood = bg._mood(sim)
    bad = [m.strip().lower() for m in str(native.setting('influence', 'bad_moods', 'angry,tense,sad,uncomfortable')).split(',')
           if m.strip()]
    grumpy = any(m in mood for m in bad)
    if feel < _number('min_friendship', -20):
        return False, 'friendship {:.0f}'.format(feel)
    if grumpy and feel < _number('mood_override', 60):
        return False, 'mood {} at friendship {:.0f}'.format(mood, feel)
    return True, 'friendship {:.0f}, mood {}'.format(feel, mood or '?')


def _ignore():
    """Testing: willingness and consent are ignored ([influence] ignore_willingness, developer row)."""
    return native.setting_flag('influence', 'ignore_willingness', False)


def _tell(sim, text):
    """Their answer as a speech bubble over their head, in their words (the DLL's bubbles, [bubbles] own_balloons);
    a notification when the DLL has none, so a refusal is seen even when no reaction animation could play."""
    try:
        if native.hud_balloon_text(sim.id, text, 1, 9, 4500):
            _log('{} says: {}'.format(_name(sim), text))
            return
    except Exception as e:
        _note_error('bubble', e)
    try:
        from sims4.localization import LocalizationHelperTuning
        from ui.ui_dialog_notification import UiDialogNotification
        dialog = UiDialogNotification.TunableFactory().default(
            sim, text=lambda *_, **__: LocalizationHelperTuning.get_raw_text(text),
            title=lambda *_, **__: LocalizationHelperTuning.get_raw_text(_name(sim)))
        dialog.show_dialog()
    except Exception as e:
        _note_error('notification', e)


def _react(sim, you, ids, what):
    from sims4vr import bodygrab as bg
    for anim in ids:
        mixer = bg.mixer_for_animation(anim)
        if mixer is not None and bg._react(sim, you, mixer):
            return True
    _log('{}: no {} could play ({})'.format(_name(sim), what, getattr(bg, '_why_not', '?')))
    return False


def use_chosen():
    """"Try..." picked on the influenced Sim's pie (pie.py select_own)."""
    global _menu, _choosing, _since, _alarm
    sim = _sim
    try:
        if sim is None or sim.is_hidden():
            _end('gone')
            return
        from sims4vr import follow
        you = follow._fp_sim()
        if you is None:
            _end('no Sim of yours')
            return
        ok, why = _willing(you, sim)
        _menu = False
        if not ok and _ignore():
            _log('{} would not ({}), but willingness is ignored (testing)'.format(_name(sim), why))
            ok = True
        if not ok:
            _log('{} will not ({}): shakes their head'.format(_name(sim), why))
            played = _react(sim, you, SHAKES, 'shake of the head')
            _tell(sim, "I'm in no mood for this." if why.startswith('mood') else "I don't feel like doing what you say.")
            _end('unwilling')
            return
        _log('{} is willing ({}): nods'.format(_name(sim), why))
        _react(sim, you, NODS, 'nod')
        _choosing, _since = True, time.monotonic()
        native.influence_look(sim.id, _number('look_eyes', 0.6), _number('look_head', 1.5))
        if _alarm is None:
            _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
    except Exception as e:
        _note_error('use', e)
        _end('failed')


# --- looking and turning while choosing -------------------------------------------------

def _yaw(dx, dz):
    return math.atan2(dx, dz)


def _wrap(a):
    while a > math.pi:
        a -= 2 * math.pi
    while a < -math.pi:
        a += 2 * math.pi
    return a


def _tick(_handle=None):
    sim = _sim
    try:
        if sim is None or not _choosing:
            _end('stopped')
            return
        if time.monotonic() - _since > _number('timeout', 45):
            _end('timed out')
            return
        if sim.is_hidden() or services.object_manager().get(sim.id) is None:
            _end('gone')
            return
        point = native.laser_point()
        if point is None or point[3] > 400:
            return
        _turn_body(sim, point[0], point[1], point[2])
    except Exception as e:
        _note_error('tick', e)


def _turn_body(sim, lx, ly, lz):
    """The body follows a point well round the Sim, never so far that you leave their view."""
    from sims4vr import facing, follow
    you = follow._fp_sim()
    if you is None or not facing.enabled():
        return
    p, f = sim.position, sim.forward
    to_point = _yaw(lx - p.x, lz - p.z)
    if math.hypot(lx - p.x, lz - p.z) < 0.25:
        return  # at their feet or on them: eyes and head only
    own = _yaw(f.x, f.z)
    if abs(_wrap(to_point - own)) <= math.radians(_number('body_angle', 75)):
        return
    yx, _, yz = facing.you_at(you)
    to_you = _yaw(yx - p.x, yz - p.z)
    keep = math.radians(_number('face_keep', 60))
    # The wanted facing: toward the point, but within `keep` of you.
    side = _wrap(to_point - to_you)
    want = to_you + max(-keep, min(keep, side))
    if abs(_wrap(want - own)) < math.radians(20):
        return
    last = _body.get(sim.id)
    if last is not None and abs(_wrap(want - last)) < math.radians(15):
        return
    _body[sim.id] = want
    facing.turn(sim, (p.x + math.sin(want) * 2.0, p.y, p.z + math.cos(want) * 2.0), 'influence')
