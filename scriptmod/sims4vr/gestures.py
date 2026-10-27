"""Hand gestures at other Sims in first person (thread "Hand gesture
interactions with Sims", user 2026-10-24). The DLL (gesture.cpp) reads them
from the posed Sim hand, so a controller and a tracked hand work the same.
Your Sim plays nothing (no scripted actions): your arm is the gesture.

Wave: the Sim you wave at (the one nearest the middle of where you look, or
of where your hand is, within MAX_ANGLE and MAX_DISTANCE) waves back. Done as
the game greets: the reaction loot of its greetings,
loot_Reaction_Greeting_Wave (123491, interactions/utils/reactions.py
ReactionLootOp), applied with you as the Actor and them as the TargetSim: it
pushes greetingReactionMixer_Wave (123489) on them, a REACTION mixer at High
priority on their posture's interaction, looking at you.
Come hither (open palm, no buttons, user 2026-10-24 16:16Z): the Sim you
beckon (MIN_CALL..MAX_CALL away) walks to you. Done as the game's call over
(sim_CallOver 13995) ends: its outcome continuation, terrain-gonear (77365)
with them as the actor and you as the target (stand within 2 m of you). The
call over's own one-shot (your Sim's beckon animation) is not played: your
arm is the gesture.
While walking over they nod at you (user 2026-10-26 16:44Z: a nod they can do
while walking): a route event, the game's way of playing an animation on a
Sim during a route without stopping it (routing/route_events). Base game has
no walking nod; the listener's nod-along of the Snowy Escape hiking trail
(routeEventPaired_HikingTrailListen 252062, animation element
HikingTrail_RouteEvent_Listen_Walk 251053, the listen half only; EP10) is
played as a single HIGH_SINGLE event at the start of their walk, provided by
our own route event provider on them (as RouteEventProviderRequest does:
routing_component.add_route_event_provider; the walk's path gathers it).
Switch: [gestures] nod (default on).

Follow me (finger come hither, user 2026-10-24 16:06Z / 2026-10-26 17:22Z):
hand upright, palm toward your face, grip held and the trigger pressed twice
(a crooked finger), at a Sim within FOLLOW_RANGE: the pie's "Follow me"
(follow.start_following), and they nod as they start (their first walk or
turn to you gets the same route event nod).

Shoo (user 2026-10-27 15:01Z): both palms down, both hands flicked up together
(fingers swing up from the wrist), twice: the Sim you face (within SHOO_ANGLE,
SHOO_RANGE) goes away. They are dismissed if they follow you, what they do with
you is cancelled (interactions with you as target, chats with you: the game's
SocialGroup.remove, as follow.py does) and for SHOO_TIME seconds the queue
refuses their own interactions with you (your own pie picks still pass); they
then walk off the way they face away from you, the game's social adjustment
walk (follow._come) into a circle 3.5 to 7 m away. Switch: [gestures] shoo,
shoo_angle, shoo_flicks.

Only a Sim who can see you answers (user 2026-10-25 01:40Z): you are within
SIGHT_ANGLE of where their body faces, and you can see each other (user
2026-10-26 17:55Z: across rooms too, through windows, open doors and arches).
Sight is the game's own GPU pick (the one the mouse and our finger use) from
your eye through their head, chest and hips (native.ask_sight): seen if it
lands on them; window glass in the way still counts as seen. If none of those
points is in your view, the game's wall line of sight (LineOfSight, wall
contours around them, same room only) decides instead.
Switches (debug menu Options > Game settings > Other Sims > Hand gestures):
[gestures] wave, come_hither, nod, follow_me, wave_dismiss (waving at a Sim who
follows you dismisses them; default on), haptics (off).
"""
import math
import time

import services
import sims4.resources

from sims4vr import native

WAVE_LOOT = 123491  # loot_Reaction_Greeting_Wave
WAVE_MIXER = 123489  # greetingReactionMixer_Wave
MAX_DISTANCE = 10.0  # metres
NOD_PAIRED = 252062  # routeEventPaired_HikingTrailListen (EP10)
NOD_ANIMATION = 251053  # HikingTrail_RouteEvent_Listen_Walk
GO_NEAR = 77365  # terrain-gonear, sim_CallOver's continuation
MIN_CALL = 1.5  # metres; closer is the finger come hither (Follow me)
FOLLOW_RANGE = 5.0  # metres, Follow me
MAX_CALL = 15.0
MAX_ANGLE = math.radians(22)
SIGHT_ANGLE = math.radians(100)  # either side of where their body faces
SHOO_ANGLE = math.radians(40)
SHOO_RANGE = 6.0
SHOO_TIME = 60.0  # seconds they keep away from you
COOLDOWN = 8.0
NOD_WAIT = 20.0  # seconds a nod waits for their walk
# How they take Follow me (base game Socials_Responses_* animations, played as
# an in-place reaction; eye roll from Movie Hangout, else a bored sigh).
ANIM_SMILE = 28370  # Socials_Responses_Friendly_LowIntensity
ANIM_FLIRTY = 28365  # Socials_Responses_Flirty_LowIntensity
ANIM_FLIRTY_HIGH = 28366  # Socials_Responses_Flirty_HighIntensity
ANIM_ROLL_EYES = 128766  # movie_Reactions_RollEyes (SP05)
ANIM_BORED = 28353  # Socials_Responses_Bored_LowIntensity
ANIM_REFUSE = 28339  # Socials_Responses_Angry_HighIntensity_Footstomp
HIGH_FIVE_RANGE = 1.8  # metres: a Sim this close (and facing you) answers your raised hand
HIGH_FIVE_WAIT = 6.0  # seconds their hand stays up for you to slap
ANIM_CELEBRATE = 28360  # Socials_Responses_Excited_HighIntensity_FistPump
CHEER = 125646  # reaction_Generic_Cheer_Mixer_Light, if the fist pump can't be made
ANIM_NO = 28336  # Socials_Responses_Angry_LowIntensity: an angry no
ANIM_NO_MEH = 28353  # Socials_Responses_Bored_LowIntensity: a bored no
_high_fives = {}  # sim id -> (until, hand)
FOLLOW_REACT_WAIT = 2.0  # seconds their reaction plays before they start following
_pending_follow = []  # (when, sim id, side, distance, angle, what)

_last = {}
_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 300:
        native._log('gestures: ' + message)


def _error(where, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('gestures: {} failed: {!r}'.format(where, e))


def _angle(dx, dz, fx, fz):
    d = math.hypot(dx, dz)
    f = math.hypot(fx, fz)
    if d < 1e-4 or f < 1e-4:
        return math.pi
    c = (dx * fx + dz * fz) / (d * f)
    return math.acos(max(-1.0, min(1.0, c)))


def _faces_you(npc, you):
    f = npc.forward
    p, q = npc.position, you.position
    return _angle(q.x - p.x, q.z - p.z, f.x, f.z) <= SIGHT_ANGLE


def _walls_clear(npc, you, distance):
    """The game's line of sight from them (wall contours): no wall between you.
    None when it could not run (counts as clear)."""
    try:
        from objects.components.line_of_sight_component import LineOfSight
        los = LineOfSight(distance + 1.0, 30, 0.35, 0.01)
        los.generate(npc.position, npc.routing_surface)
        return bool(los.constraint.geometry.contains_point(you.position))
    except Exception as e:
        _error('line of sight', e)
        return None


def _body_points(npc):
    """Head, chest and hips in the world, for the sight pick."""
    head = None
    try:
        found = native.joint(npc.id, 'b__Head__')
        if found is not None:
            head = tuple(found[0])
    except Exception:
        pass
    if head is None:
        p = npc.position
        head = (p.x, p.y + 1.65, p.z)
    x, y, z = head
    return [(x, y, z), (x, y - 0.4, z), (x, y - 0.75, z)]


def _is_window(object_id):
    obj = services.object_manager().get(object_id)
    if obj is None:
        return False, 'unknown {:x}'.format(object_id)
    name = '{}'.format(getattr(getattr(obj, 'definition', None), 'name', None) or type(obj).__name__)
    return 'window' in name.lower(), name


_pending = None  # the gesture waiting for its sight answer
SIGHT_WAIT = 2.5  # seconds


def _when_seen(you, npc, distance, label, act, now):
    """Runs act() once you and npc can see each other (asked of the DLL)."""
    global _pending
    ask = getattr(native, 'ask_sight', None)  # older native.py: walls only
    if ask is not None and ask(npc.id, _body_points(npc)):
        _pending = dict(npc_id=npc.id, distance=distance, label=label, act=act, asked=now)
        return
    if _walls_clear(npc, you, distance) is not False:
        act()
    else:
        _log('{}: a wall is between you'.format(label))


def _resolve_sight(you, now):
    global _pending
    p = _pending
    if p is None:
        return
    npc = services.object_manager().get(p['npc_id'])
    if npc is None:
        _pending = None
        return
    answer = getattr(native, 'sight', lambda: None)()
    state = 0
    if answer is not None and answer[0] == p['npc_id']:
        _, state, hit_type, hit_id, hit_distance, target_distance = answer
    if state == 0 and now - p['asked'] < SIGHT_WAIT:
        return
    _pending = None
    if state == 1:
        p['act']()
    elif state == 2:
        window, name = _is_window(hit_id) if hit_id else (False, 'nothing')
        if window:
            _log('{}: seen through a window ({})'.format(p['label'], name))
            p['act']()
        else:
            _log('{}: you cannot see each other ({} in the way, {:.1f} of {:.1f} from your eyes)'.format(
                p['label'], name, hit_distance, target_distance))
    else:
        # Not in your view (or no answer): the game's walls decide.
        clear = _walls_clear(npc, you, p['distance'])
        _log('{}: not in your view, walls {}'.format(p['label'], 'clear' if clear is not False else 'in the way'))
        if clear is not False:
            p['act']()


def _buzz(hand):
    if native.setting_flag('gestures', 'haptics', False):
        native.haptic(hand, 0.25, 0.05)


def _waved_at(you, head, forward, palm, near=0.3, far=MAX_DISTANCE, limit=MAX_ANGLE):
    """The Sim nearest the middle of your look (or of the line from your eyes
    through the waving hand), and how far off it is."""
    hx, _, hz = head
    fx, _, fz = forward
    px, _, pz = palm
    best, best_angle, best_d = None, limit, 0.0
    for sim in services.sim_info_manager().instanced_sims_gen():
        if sim is you or not sim.sim_info.is_human:
            continue
        q = sim.position
        dx, dz = q.x - hx, q.z - hz
        d = math.hypot(dx, dz)
        if d > far or d < near:
            continue
        a = min(_angle(dx, dz, fx, fz), _angle(dx, dz, px - hx, pz - hz))
        if a < best_angle and _faces_you(sim, you):
            best, best_angle, best_d = sim, a, d
    return best, best_angle, best_d


def _wave_back(you, npc):
    from event_testing.resolver import DoubleSimResolver
    from interactions.utils.loot import LootOperationList
    loot = services.get_instance_manager(sims4.resources.Types.ACTION).get(WAVE_LOOT)
    if loot is None:
        return 'no wave loot in this game'
    LootOperationList(DoubleSimResolver(you.sim_info, npc.sim_info), (loot,)).apply_operations()
    for interaction in tuple(npc.queue) + ((npc.queue.running,) if npc.queue.running is not None else ()):
        if getattr(interaction, 'guid64', None) == WAVE_MIXER:
            return 'waves back'
    return 'no wave queued (posture {}, {})'.format(
        npc.posture, 'walking' if getattr(npc.routing_component, 'is_moving', False) else 'standing still')


_nod_event = None  # our RouteEvent class (made once), False if the game lacks it
_nod_providers = {}  # sim id -> provider waiting for their walk


def _nod_event_class():
    """A plain RouteEvent playing only the listener's walking nod: the
    paired event's animation tuning cut to its listen element, always played."""
    global _nod_event
    if _nod_event is not None:
        return _nod_event or None
    _nod_event = False
    from interactions.utils.success_chance import SuccessChance
    from routing.route_events.route_event import RouteEvent
    paired = services.get_instance_manager(sims4.resources.Types.SNIPPET).get(NOD_PAIRED)
    element = services.get_instance_manager(sims4.resources.Types.ANIMATION).get(NOD_ANIMATION)
    if paired is None or element is None:
        _log('no walking nod in this game (Snowy Escape tuning {} / {} missing)'.format(NOD_PAIRED, NOD_ANIMATION))
        return None
    wrapper = paired.event_type
    values = wrapper._tuned_values.clone_with_overrides(animation_elements=(element,))
    event_type = type(wrapper)(values, wrapper._name, wrapper.factory)
    _nod_event = type('S4VR_RouteEvent_NodWalking', (RouteEvent,), {
        'event_type': event_type, 'chance': SuccessChance.ONE, 'tests': RouteEvent.tests,
        'priority': paired.priority, 'skippable': True, 'loot_actions': (), 'broadcaster': None})
    return _nod_event


class _NodProvider:
    """Gives their next walk one nod at its start (route event provider)."""

    def __init__(self, npc, event_cls, now, why):
        self.why = why
        self.npc_id = npc.id
        self.name = npc.sim_info.first_name
        self.event_cls = event_cls
        self.since = now
        self.done = False

    def provide_route_events(self, route_event_context, sim, path, failed_types=None, start_time=0, end_time=None,
                             **kwargs):
        from routing.route_enums import RouteEventType
        if self.done or (failed_types and self.event_cls in failed_types):
            return
        if not route_event_context.route_event_already_scheduled(self.event_cls):
            route_event_context.add_route_event(RouteEventType.HIGH_SINGLE,
                                                self.event_cls(provider=self, provider_required=True))

    def can_provide_route_event(self, route_event_cls, failed_types, sim_resolver):
        return not self.done

    def is_route_event_valid(self, route_event, time, sim, path):
        return not self.done

    def on_event_executed(self, route_event, sim):
        self.done = True
        _log('{} nods at you {}'.format(self.name, self.why))
        _drop_nod(sim)


def _drop_nod(npc, why=None):
    provider = _nod_providers.pop(npc.id, None)
    if provider is None:
        return
    try:
        npc.routing_component.remove_route_event_provider(provider)
    except Exception as e:
        _error('removing the nod', e)
    if why:
        _log('{} did not nod: {}'.format(provider.name, why))


def _nod_on_walk(npc, now, why='while walking over'):
    if not native.setting_flag('gestures', 'nod', True):
        return
    event_cls = _nod_event_class()
    if event_cls is None:
        return
    _drop_nod(npc)
    provider = _NodProvider(npc, event_cls, now, why)
    _nod_providers[npc.id] = provider
    npc.routing_component.add_route_event_provider(provider)


def _expire_nods(now):
    for sim_id, provider in list(_nod_providers.items()):
        if now - provider.since > NOD_WAIT:
            npc = services.object_manager().get(sim_id)
            if npc is None:
                _nod_providers.pop(sim_id, None)
            else:
                _drop_nod(npc, 'no walk played it within {:.0f} s'.format(NOD_WAIT))


def _call_over(you, npc, now):
    from interactions.context import InteractionContext, InteractionSource, QueueInsertStrategy
    from interactions.priority import Priority
    affordance = services.get_instance_manager(sims4.resources.Types.INTERACTION).get(GO_NEAR)
    if affordance is None:
        return 'no terrain-gonear in this game'
    try:
        _nod_on_walk(npc, now)
    except Exception as e:
        _error('nod', e)
    context = InteractionContext(npc, InteractionSource.SCRIPT_WITH_USER_INTENT, Priority.High,
                                 insert_strategy=QueueInsertStrategy.NEXT)
    result = npc.push_super_affordance(affordance, you, context)
    if not result:
        _drop_nod(npc)
    return 'comes over' if result else 'refused: {}'.format(result)


def tick(you):
    """Called by hands.py twenty times a second in first person."""
    on = native.setting_flag('gestures', 'wave', True)
    call_on = native.setting_flag('gestures', 'come_hither', True)
    now = time.monotonic()
    if _nod_providers:
        _expire_nods(now)
    if _pending_follow:
        try:
            _start_pending_follows()
        except Exception as e:
            _error('follow', e)
    try:
        _resolve_sight(you, now)
    except Exception as e:
        _error('sight', e)
    while True:
        event = native.gesture_event()
        if event is None:
            break
        kind, hand, head, forward, palm = event
        try:
            if kind == 2:
                if call_on:
                    _come_hither(you, hand, head, forward, palm, now)
                continue
            if kind == 5:
                if native.setting_flag('gestures', 'shoo', True):
                    _shoo(you, hand, head, forward, palm, now)
                continue
            if kind == 4:
                if native.setting_flag('gestures', 'high_five', True):
                    _high_five(you, hand, head, forward, palm, now)
                continue
            if kind == 3:
                if native.setting_flag('gestures', 'follow_me', True):
                    _follow_me(you, hand, head, forward, palm, now)
                continue
            if kind != 1 or not on:
                continue
            npc, angle, distance = _waved_at(you, head, forward, palm)
            side = 'right' if hand else 'left'
            if npc is None:
                _log('{} hand wave: nobody there facing you'.format(side))
                continue
            name = npc.sim_info.first_name
            if now - _last.get(npc.id, -COOLDOWN) < COOLDOWN:
                _log('{} hand wave at {}: answered {:.0f} s ago'.format(side, name, now - _last[npc.id]))
                continue
            _when_seen(you, npc, distance, '{} hand wave at {}'.format(side, name),
                       lambda npc=npc, side=side, name=name, angle=angle, distance=distance, hand=hand:
                       _wave(you, npc, side, name, angle, distance, hand, now), now)
        except Exception as e:
            _error('gesture {}'.format(kind), e)


_last_call = {}


def _wave(you, npc, side, name, angle, distance, hand, now):
    _last[npc.id] = now
    _buzz(hand)
    goodbye = ''
    if native.setting_flag('gestures', 'wave_dismiss', True):
        from sims4vr import follow
        if follow.is_follower(npc):
            # Waving at a follower: goodbye. Dismissed first (a follower's
            # queue takes no reactions), then they wave back.
            follow.dismiss(npc, 'you waved goodbye')
            goodbye = ', no longer follows you'
    _log('{} hand wave at {} ({:.1f} m, {:.0f} deg off): {}{}'.format(
        side, name, distance, math.degrees(angle), _wave_back(you, npc), goodbye))


def _come_hither(you, hand, head, forward, palm, now):
    npc, angle, distance = _waved_at(you, head, forward, palm, MIN_CALL, MAX_CALL)
    side = 'right' if hand else 'left'
    if npc is None:
        _log('{} hand come hither: nobody {:.1f}-{:.0f} m away there facing you'.format(side, MIN_CALL, MAX_CALL))
        return
    name = npc.sim_info.first_name
    if now - _last_call.get(npc.id, -COOLDOWN) < COOLDOWN:
        _log('{} hand come hither at {}: called {:.0f} s ago'.format(side, name, now - _last_call[npc.id]))
        return

    def act():
        _last_call[npc.id] = now
        _buzz(hand)
        _log('{} hand come hither at {} ({:.1f} m, {:.0f} deg off): {}'.format(
            side, name, distance, math.degrees(angle), _call_over(you, npc, now)))
    _when_seen(you, npc, distance, '{} hand come hither at {}'.format(side, name), act, now)


def _follow_me(you, hand, head, forward, palm, now):
    from sims4vr import follow
    side = 'right' if hand else 'left'
    if not follow._on():
        _log('{} hand follow me: Follow me is switched off'.format(side))
        return
    npc, angle, distance = _waved_at(you, head, forward, palm, 0.3, FOLLOW_RANGE)
    if npc is None:
        _log('{} hand follow me: nobody within {:.0f} m there facing you'.format(side, FOLLOW_RANGE))
        return
    name = npc.sim_info.first_name
    if follow.is_follower(npc):
        _log('{} hand follow me at {}: already follows you'.format(side, name))
        return

    def act():
        _buzz(hand)
        what, anim = _follow_mood(you, npc) if native.setting_flag('gestures', 'follow_reactions', True) else ('', None)
        played = _follow_react(you, npc, anim) if anim else False
        if what == 'refuse':
            _log('{} hand follow me at {}: refuses ({})'.format(side, name, 'reacts' if played else 'no reaction'))
            return
        if played:  # the reaction first, then they come along
            _pending_follow.append((time.monotonic() + FOLLOW_REACT_WAIT, npc.id, side, distance, angle, what))
        else:
            _start_follow(npc, side, distance, angle, what or 'no reaction')
    _when_seen(you, npc, distance, '{} hand follow me at {}'.format(side, name), act, now)


def _follow_mood(you, npc):
    """How they take being asked to follow you (user 2026-10-26 20:31Z): by
    their mood, traits and how they feel about you. (what, animation id)."""
    from sims4vr import bodygrab as bg
    friend, love = bg._scores(you, npc)
    romance_ok = not bg._too_young(you, npc) and not bg._family(you, npc) and bg._attracted(you, npc)
    if not romance_ok:
        love = 0.0
    mood, traits = bg._mood(npc), bg._traits(npc)
    regard = friend + max(love, 0.0) * 0.5
    for word, value in bg.MOODS:
        if word in mood:
            regard += value
    for word, value in bg.TRAITS:
        if any(word in t for t in traits):
            regard += value
    tempered = any(any(word in t for t in traits) for word in bg.TEMPERED)
    if native.setting_flag('gestures', 'follow_refuse', True) and (friend <= -0.4 or regard <= -0.45):
        return 'refuse', ANIM_REFUSE
    if 'angry' in mood or (tempered and 'tense' in mood):
        return 'eye roll', ANIM_ROLL_EYES
    if romance_ok and ('flirty' in mood or love >= 0.5):
        return 'flirty', ANIM_FLIRTY_HIGH if love >= 0.6 or ('flirty' in mood and love >= 0.3) else ANIM_FLIRTY
    if friend >= 0.2 or any(word in mood for word in ('happy', 'playful', 'confident')):
        return 'smile', ANIM_SMILE
    if 'bored' in mood:
        return 'bored', ANIM_BORED
    return 'neutral', None


def _follow_react(you, npc, anim):
    from sims4vr import bodygrab as bg
    mixer = bg.mixer_for_animation(anim)
    if mixer is None and anim == ANIM_ROLL_EYES:
        mixer = bg.mixer_for_animation(ANIM_BORED)  # Movie Hangout not installed
    if mixer is None:
        return False
    try:
        if bg._react(npc, you, mixer):
            return True
        _log('{}: reaction {} not played ({})'.format(npc.sim_info.first_name, mixer.__name__, bg._why_not))
    except Exception as e:
        _error('follow reaction', e)
    return False


def _start_follow(npc, side, distance, angle, what):
    from sims4vr import follow
    name = npc.sim_info.first_name
    try:
        _nod_on_walk(npc, time.monotonic(), 'as they start following you')
    except Exception as e:
        _error('nod', e)
    follow.start_following(npc)
    if not follow.is_follower(npc):
        _drop_nod(npc, 'they cannot follow you')
    _log('{} hand follow me at {} ({:.1f} m, {:.0f} deg off, {}): {}'.format(
        side, name, distance, math.degrees(angle), what, 'follows you' if follow.is_follower(npc) else 'refused'))


def _start_pending_follows():
    now = time.monotonic()
    for entry in list(_pending_follow):
        if now < entry[0]:
            continue
        _pending_follow.remove(entry)
        npc = services.object_manager().get(entry[1])
        if npc is not None:
            _start_follow(npc, *entry[2:])




def _high_five_mood(you, npc):
    """True if they want to (like you, or are in a good mood and don't dislike
    you), else the 'no' animation for why not."""
    from sims4vr import bodygrab as bg
    friend, love = bg._scores(you, npc)
    mood, traits = bg._mood(npc), bg._traits(npc)
    good = any(word in mood for word in ('happy', 'playful', 'confident', 'energized', 'flirty'))
    if 'angry' in mood or friend <= -0.3:
        return False, ANIM_NO
    if friend >= 0.1 or love >= 0.2 or (good and friend > -0.15):
        return True, None
    return False, ANIM_NO_MEH


def _high_five(you, hand, head, forward, palm, now):
    """Your open hand raised and held (user 2026-10-26 23:55Z): the Sim in
    front of you raises theirs (DLL armik: arm IK, smiling) if they want to,
    and your slap on it is touch event kind 7 (high_five_hit); if not, a no."""
    from sims4vr import bodygrab as bg
    side = 'right' if hand else 'left'
    npc, angle, distance = _waved_at(you, head, forward, palm, 0.2, HIGH_FIVE_RANGE)
    if npc is None:
        _log('{} hand raised for a high five: nobody close facing you'.format(side))
        return
    name = npc.sim_info.first_name
    yes, no_anim = _high_five_mood(you, npc)
    if yes:
        native.high_five(npc.id, hand, HIGH_FIVE_WAIT)
        _high_fives[npc.id] = (now + HIGH_FIVE_WAIT, hand)
        mixer = bg.mixer_for_animation(ANIM_SMILE)
        if mixer is not None:
            bg._react(npc, you, mixer)
        _log('{} hand high five: {} raises a hand ({:.1f} m)'.format(side, name, distance))
    else:
        mixer = bg.mixer_for_animation(no_anim)
        played = mixer is not None and bg._react(npc, you, mixer)
        _log('{} hand high five: {} says no ({})'.format(side, name, 'plays it' if played else bg._why_not))


def high_five_hit(you, npc, hand, speed):
    """bodygrab.tick: your palm met their raised one."""
    from sims4vr import bodygrab as bg
    _high_fives.pop(npc.id, None)
    mixer = bg.mixer_for_animation(ANIM_CELEBRATE) or CHEER
    played = bg._react(npc, you, mixer)
    if native.setting_flag('physical', 'touch_relationship', True):
        bg._change(you, npc, 2.0, 0)
    _log('high five with {} ({:.1f} m/s): {}'.format(npc.sim_info.first_name, speed,
                                                      'celebrates' if played else bg._why_not))


_shooed = {}  # sim id -> time.monotonic() until which their own interactions with you are refused
_shoo_wrapped = False
_shoo_logs = [0]


def _wrap_shoo_queue():
    global _shoo_wrapped
    if _shoo_wrapped:
        return
    _shoo_wrapped = True
    from event_testing.results import TestResult
    from interactions.interaction_finisher import FinishingType
    from interactions.interaction_queue import InteractionQueue
    from sims4vr import hooks

    def append(original, self, interaction, *args, **kwargs):
        try:
            if _shooed:
                until = _shooed.get(getattr(interaction.sim, 'id', None))
                if until is not None:
                    if time.monotonic() > until:
                        _shooed.pop(interaction.sim.id, None)
                    else:
                        from interactions.context import InteractionSource
                        from sims4vr import follow
                        you = follow._fp_sim()
                        picked = interaction.context.source in (InteractionSource.PIE_MENU,
                                                                getattr(InteractionSource, 'SCRIPT_WITH_USER_INTENT', None))
                        if you is not None and not picked and follow._with_you(interaction, you):
                            _shoo_logs[0] += 1
                            if _shoo_logs[0] <= 30:
                                _log('{}: {} with you refused (you shooed them)'.format(
                                    interaction.sim.sim_info.first_name, getattr(interaction.affordance, '__name__', '?')))
                            try:
                                interaction.cancel(FinishingType.INTERACTION_QUEUE, cancel_reason_msg='VR first person: shooed')
                            except Exception:
                                pass
                            return TestResult(False, 'VR first person: shooed')
        except Exception as e:
            _error('shoo queue', e)
        return original(self, interaction, *args, **kwargs)

    hooks.hook('shoo', InteractionQueue, 'append', append)


def _shoo(you, hand, head, forward, palm, now):
    npc, angle, distance = _waved_at(you, head, forward, palm, 0.3, SHOO_RANGE, SHOO_ANGLE)
    if npc is None:
        _log('shoo: nobody within {:.0f} m there facing you'.format(SHOO_RANGE))
        return
    name = npc.sim_info.first_name

    def act():
        _buzz(hand)
        _log('shoo at {} ({:.1f} m, {:.0f} deg off): {}'.format(name, distance, math.degrees(angle), _send_away(you, npc)))
    _when_seen(you, npc, distance, 'shoo at ' + name, act, now)


def _send_away(you, npc):
    from interactions.interaction_finisher import FinishingType
    from sims4vr import follow, reach
    notes = []
    if follow.is_follower(npc):
        follow.dismiss(npc, 'you shooed them')
        notes.append('no longer follows you')
    try:
        _wrap_shoo_queue()
        _shooed[npc.id] = time.monotonic() + SHOO_TIME
        items = list(npc.si_state) + list(npc.queue)
        seen = set()
        for si in items:
            if si is None or id(si) in seen or si.is_finishing:
                continue
            seen.add(id(si))
            if getattr(si, 'provided_posture_type', None) is not None or \
                    getattr(si.affordance, '_provided_posture_type', None) is not None or not follow._with_you(si, you):
                continue
            si.cancel(FinishingType.USER_CANCEL, cancel_reason_msg='VR first person: you shooed them', ignore_must_run=True,
                      immediate=True)
            notes.append('ended ' + getattr(si.affordance, '__name__', str(si)))
        for group in list(npc.get_groups_for_sim_gen()):
            if you in group:
                group.remove(npc)
                notes.append('left your chat')
    except Exception as e:
        _error('shoo cancel', e)
    try:
        x, y, z = reach._you(you)
        p = npc.position
        dx, dz = p.x - x, p.z - z
        d = math.hypot(dx, dz)
        if d < 0.1:
            f = npc.forward
            dx, dz, d = f.x, f.z, math.hypot(f.x, f.z) or 1.0
        dx, dz = dx / d, dz / d
        result = None
        for far in (7.0, 5.0, 3.5):
            state = {'walk': None, 'sent': 0.0}
            result = follow._come(npc, (x + dx * (d + far), p.y, z + dz * (d + far)), 2.0, npc.routing_surface,
                                  (p.x + dx * 10, p.y, p.z + dz * 10), state, time.monotonic(), math.pi)
            if result:
                notes.append('walks away ({:.0f} m)'.format(far))
                break
        if not result:
            notes.append('could not walk away ({})'.format(getattr(result, 'reason', result)))
    except Exception as e:
        _error('shoo walk', e)
        notes.append('walk failed')
    return ', '.join(notes) or 'nothing to do'
