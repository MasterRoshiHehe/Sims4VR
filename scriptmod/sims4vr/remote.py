"""First person: TVs and lights work from wherever you are.

Lights: their switches (on, off, set to auto, colour) run at once without
walking anywhere, so they are simply left out of the 1 m reach rule (reach.py).

TVs: picking a channel (Turn On..., Change Channel..., Watch ..., Channel
Surf) normally walks your Sim to the best seat, sits it down and only then
switches the TV on. In first person your Sim instead uses the TV remote right
where it is, standing or seated: the game's own remote animation, with the
remote in your hand, and the TV changes to that channel. Turn Off already
works that way and is only let through the reach rule. Nobody sits down to
watch; the TV stays on until it is turned off.

How: InteractionQueue.append (looked up at call time by every push) refuses a
watch interaction for the Sim you are in and queues the game's Turn Off
interaction (tv_turnoff: the remote animation, then a state change on its
xevt) in its place, with its state change pointed at the picked channel and
its own "TV is on" test skipped. If that can't run, the TV is switched
directly. The remote, Turn Off and the other watching picks on a TV (Watch TV
picks a channel first; other mods' movie or video pickers open their menu
first) are told to happen where your Sim is (_stay_put): the remote animation
alone would walk your Sim in front of the TV to face it.

The remote in your hand: opening a pie menu on a TV in first person puts the
game's own TV remote (the prop its remote animation uses) in the hand that
clicked it, held exactly the way the game's remote animation holds it
(v0.13.67; before, it stood upright in an open hand where the phone sits).
The DLL learns that grip from the game itself: while the game's remote
animation runs on your Sim (Turn Off, a channel), it reads where the remote's
joint sits in the hand and how the fingers close, keeps it in
sims4vr_grips.ini and mirrors it for the left hand. Our remote then hangs on
the hand joint at that place and the hand's fingers take that grip. Until
the game has shown it once, the remote stays hidden in the hand and appears
the moment the Sim's first remote animation takes it out. It stays while the pie menu is open,
while a channel or video picker is up, while a TV pick waits in the queue and
while the game's remote animation runs; the animation's own remote is hidden
meanwhile, so yours stays in the hand you used. It goes when the game puts its
remote away (the animation's props are destroyed), or a moment after nothing
to do with the TV is left.
"""
import time

from sims4vr import native

TURN_OFF = 'tv_turnoff'  # the game's remote-in-hand Turn Off
TV_PICKS = ('tv_turnoff', 'tv_turnoff_immediate')
TV_PICK_PREFIXES = ('tv_pickchannel',)  # Turn On... and Change Channel... (a channel picker)
TV_WORDS = ('watch', 'channel', 'turnoff', 'turn_off', 'movie', 'video')  # watching picks on a TV, also other mods' (not Upgrade or Repair)

PROP_JOINTS = ('b__L_Prop__', 'b__R_Prop__')
HAND_JOINTS = ('b__L_Hand__', 'b__R_Hand__')
CHECK_AFTER = 0.5  # seconds after the remote goes into a hand, where it is gets logged
HOLD_TICK = 0.1  # seconds between checks while the remote is out
PIE_GRACE = 1.5  # seconds a TV pie menu counts as open before the DLL shows it at your hand
LINGER = 1.0  # seconds the remote stays after the last reason to hold it

_logs = 0
_remote_affordance = None
_remote_def = None  # definition id of the game's TV remote prop (False: not found yet)
_held = None  # the remote in your hand: _Held
_game_offset = None  # (position, rotation) the game's own remote sits at on its prop joint, if not at the joint itself
_alarm = None
_owner = type('_Owner', (), {})()


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 150:
        native._log('remote: ' + message)


def _first_person(sim):
    """The Sim you are in, in first person."""
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def _watch_class():
    try:
        from objects.electronics.television import WatchSuperInteraction
        return WatchSuperInteraction
    except Exception:
        return None


def _is_light_switch(affordance):
    """On, off, auto, all lights, colour and brightness: all run at once."""
    try:
        from objects.lighting.lighting_interactions import SwitchLightImmediateInteraction
        if isinstance(affordance, type) and issubclass(affordance, SwitchLightImmediateInteraction):
            return True
    except Exception:
        pass
    return bool(getattr(affordance, 'immediate', False)) and \
        getattr(affordance, '__module__', '').startswith('objects.lighting')


def _is_tv_pick(affordance):
    watch = _watch_class()
    if watch is not None and isinstance(affordance, type) and issubclass(affordance, watch):
        return True
    name = getattr(affordance, '__name__', '').lower()
    return name in TV_PICKS or name.startswith(TV_PICK_PREFIXES)


_tv_kinds = {}


def _is_tv(target):
    """An object you can watch (it offers a watch interaction)."""
    watch = _watch_class()
    if target is None or watch is None:
        return False
    key = getattr(getattr(target, 'definition', None), 'id', None) or id(type(target))
    if key not in _tv_kinds:
        found = False
        try:
            for affordance in target.super_affordances():
                if isinstance(affordance, type) and issubclass(affordance, watch):
                    found = True
                    break
        except Exception:
            pass
        _tv_kinds[key] = found
    return _tv_kinds[key]


def _is_tv_use(affordance, target):
    """A watching pick on a TV: Watch TV, a channel, a movie or video picker, Turn Off."""
    if _is_tv_pick(affordance):
        return True
    name = getattr(affordance, '__name__', '').lower()
    return any(word in name for word in TV_WORDS) and _is_tv(target)


def from_afar(affordance, target=None):
    """True for picks that work from wherever you are (reach.py lets them through)."""
    if _is_light_switch(affordance) or _is_tv_use(affordance, target):
        return True
    if getattr(affordance, 'vr_from_afar', False):  # other mods' options that need no walking (Claude Jiggle outfits)
        return True
    try:  # Open Claude on a TV (claude.py, [claude] tvs)
        from sims4vr import claude
        return affordance is claude._affordance and claude._is_tv(target)
    except Exception:
        return False


def _stay_put(interaction):
    """Your Sim does it right where it is, in the posture it is in: no walking
    up to the TV, no turning to face it, no sitting down. The transition
    planner asks constraint_intersection where the Sim must be; for your Sim
    the answer becomes its current posture wherever it is now."""
    sim = interaction.sim

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
    _log('{}: done where you are ({})'.format(type(interaction).__name__, sim.posture_state if sim else '?'))


def _turn_off_affordance():
    global _remote_affordance
    if _remote_affordance is None:
        import services
        import sims4.resources
        manager = services.get_instance_manager(sims4.resources.Types.INTERACTION)
        found = [t for t in manager.types.values() if getattr(t, '__name__', '') == TURN_OFF]
        _remote_affordance = found[0] if found else False
        _log('the TV remote interaction is {}'.format(getattr(_remote_affordance, '__name__', 'missing')))
    return _remote_affordance or None


def _set_directly(tv, value, why):
    try:
        if tv.get_state(value.state) is not value:
            tv.set_state(value.state, value)
        _log('{} switched to {} directly ({})'.format(tv, value, why))
    except Exception as e:
        _log('{} could not be switched to {}: {!r}'.format(tv, value, e))


def _use_remote(sim, tv, value):
    """Queue the remote animation on your Sim, ending with the TV on `value`."""
    affordance = _turn_off_affordance()
    if affordance is None:
        _set_directly(tv, value, 'no remote interaction')
        return
    from event_testing.results import TestResult
    from interactions.aop import AffordanceObjectPair
    from interactions.context import InteractionContext, QueueInsertStrategy
    from interactions.priority import Priority
    context = InteractionContext(sim, InteractionContext.SOURCE_PIE_MENU, Priority.High,
                                 insert_strategy=QueueInsertStrategy.NEXT)
    aop = AffordanceObjectPair(affordance, tv, affordance, None, skip_test_on_execute=True)
    made = aop.interaction_factory(context)
    interaction = getattr(made, 'interaction', None)
    if not made or interaction is None:
        _set_directly(tv, value, 'remote interaction not made: {}'.format(made))
        return

    def to_channel(factory):
        def build(owner, *args, sequence=(), **kwargs):
            kwargs['new_value'] = lambda _owner: value
            return factory(owner, *args, sequence=sequence, **kwargs)
        return build

    try:
        interaction.basic_extras = tuple(to_channel(f) for f in affordance.basic_extras)
        interaction.test = lambda *args, **kwargs: TestResult.TRUE  # its own test wants the TV already on
        interaction._sims4vr_channel = value
        _stay_put(interaction)
    except Exception as e:
        _set_directly(tv, value, 'remote interaction not adjustable: {!r}'.format(e))
        return

    def finished(_interaction):
        # The channel is set on the remote's xevt (also when cut short); if it
        # never came (no animation, failed to start), switch it anyway.
        try:
            if tv.get_state(value.state) is not value:
                _set_directly(tv, value, 'the remote animation did not switch it')
        except Exception:
            pass

    try:
        interaction.register_on_finishing_callback(finished)
    except Exception:
        pass
    result = AffordanceObjectPair.execute_interaction(interaction)
    if result:
        _log('{}: remote in hand, switching {} to {}'.format(sim, tv, value))
    else:
        _set_directly(tv, value, 'remote interaction not queued: {}'.format(result))


class _Held:
    __slots__ = ('prop', 'sim', 'tv', 'hand', 'since', 'last', 'game', 'why', 'joint', 'check_at', 'placed',
                 'game_logged')

    def __init__(self, prop, sim, tv, hand):
        self.prop, self.sim, self.tv, self.hand = prop, sim, tv, hand
        self.joint = HAND_JOINTS[hand]
        self.check_at = None
        self.placed = False  # in the hand with the game's grip (False: hidden until the game shows how)
        self.game_logged = False
        self.since = self.last = time.monotonic()
        self.game = []  # the remote animation's own remotes, hidden while yours is out
        self.why = 'pie menu'


def _find_remote_definition():
    """The remote prop of the game's remote animation (tv_turnoff), read from its state machine."""
    affordance = _turn_off_affordance()
    if affordance is None:
        return None
    from animation import get_throwaway_animation_context
    from animation.asm import create_asm
    try:
        ref = getattr(affordance.basic_content, 'animation_ref', None)
        factory = getattr(ref, 'factory', ref)
        elements = list(factory.animation_element_gen()) if factory is not None else []
    except Exception as e:
        _log('remote animation not readable: {!r}'.format(e))
        return None
    for element in elements:
        try:
            asm_key = getattr(element, 'asm_key', None)
            if asm_key is None:
                continue
            asm = create_asm(asm_key, get_throwaway_animation_context())
            state = getattr(element, 'initial_state', None)
            if not isinstance(state, str):
                state = 'entry'
            for to_state in tuple(getattr(element, 'begin_states', ()) or ()):
                props = asm.get_props_in_traversal(state, to_state) or {}
                for name, definition_id in props.items():
                    _log('the game\'s remote is prop {} ({:#x}) of {}'.format(name, int(definition_id), asm.name))
                    return int(definition_id)
                state = to_state
            _log('no prop in {} {}'.format(asm.name, getattr(element, 'begin_states', ())))
        except Exception as e:
            _log('reading the remote animation failed: {!r}'.format(e))
    return None


def _remote_definition():
    global _remote_def
    if _remote_def is None:
        _remote_def = _find_remote_definition() or False
    return _remote_def or None


def _is_remote_interaction(interaction):
    return hasattr(interaction, '_sims4vr_channel') or \
        getattr(getattr(interaction, 'affordance', None), '__name__', '') == TURN_OFF


def _put_away(why):
    global _held, _alarm
    held, _held = _held, None
    if _alarm is not None:
        try:
            import alarms
            alarms.cancel_alarm(_alarm)
        except Exception:
            pass
        _alarm = None
    native.grip_learn(False)
    if held is None:
        return
    if held.placed:
        native.grip_hold(held.hand, False)
    try:
        if held.prop.id in held.prop.manager:
            held.prop.destroy(source=held.prop, cause='VR: TV remote put away')
    except Exception as e:
        _log('putting the remote away failed: {!r}'.format(e))
    _log('remote put away ({})'.format(why))


def _distance(a, b):
    return sum((a[i] - b[i]) ** 2 for i in range(3)) ** 0.5


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _rotate(q, v):
    x, y, z, _w = _qmul(_qmul(q, (v[0], v[1], v[2], 0.0)), (-q[0], -q[1], -q[2], q[3]))
    return x, y, z


def _mount(hand):
    """(joint, position, rotation) for the remote on that hand: the hand joint,
    at the place the game's own remote animation holds it (learned by the
    DLL; with the offset the game hangs its remote on the prop joint, if it
    has one). None until the game has shown how."""
    grip = native.grip(hand)
    if grip is None:
        return None
    position, rotation = tuple(grip[0]), tuple(grip[1])
    if _game_offset is not None:
        offset_position, offset_rotation = _game_offset
        moved = _rotate(rotation, offset_position)
        position = tuple(position[i] + moved[i] for i in range(3))
        rotation = _qmul(rotation, offset_rotation)
    return HAND_JOINTS[hand], position, rotation


def _attach(held, hand):
    import sims4.math
    from objects import VisibilityState
    from sims4vr.hands import _joint_hash
    mount = _mount(hand)
    if held.placed and held.hand != hand:
        native.grip_hold(held.hand, False)
    if mount is None:
        joint, position, rotation = HAND_JOINTS[hand], (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)
    else:
        joint, position, rotation = mount
    held.prop.set_parent(held.sim, sims4.math.Transform(sims4.math.Vector3(*position),
                                                        sims4.math.Quaternion(*rotation)),
                         joint_name_or_hash=_joint_hash(joint))
    # A prop made for an animation may wait for it to be shown: show it now,
    # or keep it hidden until the game has shown how it is held.
    try:
        held.prop.visibility = VisibilityState(mount is not None)
    except Exception as e:
        _log('showing the remote failed: {!r}'.format(e))
    held.hand = hand
    held.joint = joint
    held.placed = mount is not None
    native.grip_hold(hand, held.placed)
    if held.placed:
        held.check_at = time.monotonic() + CHECK_AFTER
        _log('remote in the game\'s grip at {} {} on {}'.format(
            tuple(round(v, 3) for v in position), tuple(round(v, 3) for v in rotation), joint))
    else:
        _log('the game hasn\'t shown its grip yet: the remote waits hidden in your hand for its first remote '
             'animation')


def _log_game_remote(held):
    """Once per remote animation: where the game hangs its own (hidden) remote."""
    global _game_offset
    held.game_logged = True
    from sims4vr.hands import _joint_hash
    prop_hashes = {_joint_hash(name): name for name in PROP_JOINTS}
    hand_hashes = {_joint_hash(name): name for name in HAND_JOINTS}
    for prop in held.game:
        try:
            location = prop.location
            joint = getattr(location, 'joint_name_or_hash', None)
            transform = getattr(location, 'transform', None)
            t, q = transform.translation, transform.orientation
            position, rotation = (t.x, t.y, t.z), (q.x, q.y, q.z, q.w)
            name = prop_hashes.get(joint) or hand_hashes.get(joint) or joint
            _log('the game\'s own remote hangs on {} of {} at {} {}'.format(
                name, getattr(prop, 'parent', None), tuple(round(v, 3) for v in position),
                tuple(round(v, 3) for v in rotation)))
            if joint in prop_hashes and getattr(prop, 'parent', None) is held.sim:
                identity = max(abs(v) for v in position) < 0.002 and abs(abs(rotation[3]) - 1) < 1e-4
                offset = None if identity else (position, rotation)
                if offset != _game_offset:
                    _game_offset = offset
                    if offset is not None:
                        _log('the game\'s remote sits off its prop joint: our remote follows that')
                        if held.placed:
                            _attach(held, held.hand)
        except Exception as e:
            _log('reading where the game hangs its remote failed: {!r}'.format(e))


def _check_place(held):
    """Log once where the remote is against the hand, to tell a hidden remote
    from one that sits somewhere else."""
    held.check_at = None
    try:
        p = held.prop.position
        at = (p.x, p.y, p.z)
        hand_joint = native.joint(held.sim.id, HAND_JOINTS[held.hand])
        prop_joint = native.joint(held.sim.id, PROP_JOINTS[held.hand])
        visibility = getattr(held.prop, 'visibility', None)
        _log('remote at {} on {}: {} from the hand joint, {} from the prop joint; visible {}, parent {}'.format(
            tuple(round(v, 2) for v in at), held.joint,
            '{:.2f} m'.format(_distance(at, hand_joint[0])) if hand_joint else 'unknown',
            '{:.2f} m'.format(_distance(at, prop_joint[0])) if prop_joint else 'unknown',
            getattr(visibility, 'visibility', visibility), getattr(held.prop, 'parent', None)))
    except Exception as e:
        _log('remote place check failed: {!r}'.format(e))


def pie_opened(target, sim):
    """pie.py: the game built a pie menu for a click on target. A TV, in first
    person: the remote goes into the hand that clicked it."""
    global _held, _alarm
    if not _is_tv(target) or not _first_person(sim):
        return
    hand = native.click_hand()
    if hand not in (0, 1):
        hand = 1
    if _held is not None and _held.sim is sim and _held.prop.id in _held.prop.manager:
        _held.tv = target
        _held.since = _held.last = time.monotonic()
        if _held.hand != hand:
            _attach(_held, hand)
            _log('remote moved to your {} hand'.format('right' if hand else 'left'))
        return
    _put_away('another remote')
    definition = _remote_definition()
    if definition is None:
        return
    from objects.system import create_prop
    prop = create_prop(definition)
    if prop is None:
        _log('the remote prop {:#x} could not be made'.format(definition))
        return
    held = _Held(prop, sim, target, hand)
    try:
        _attach(held, hand)
    except Exception as e:
        _log('the remote could not go into your hand: {!r}'.format(e))
        prop.destroy(source=prop, cause='VR: TV remote not attached')
        return
    _held = held
    import alarms
    import clock
    _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(HOLD_TICK), _tick, repeating=True)
    _log('remote in your {} hand for {}'.format('right' if hand else 'left', target))


def _reasons(held):
    """Why the remote stays out now (empty: no reason)."""
    if time.monotonic() - held.since < PIE_GRACE:
        return 'pie menu opening'
    shown = native.pie_shown()
    if shown:
        return 'pie menu'
    try:
        from sims4vr import phone
        if phone._popup_open:
            return 'picker'
    except Exception:
        pass
    queue = held.sim.queue
    running = getattr(queue, 'running', None)
    for interaction in list(queue):
        if interaction.target is not held.tv:
            continue
        if interaction is running:
            if _is_remote_interaction(interaction):
                return 'remote animation'
        elif _is_tv_use(interaction.affordance, interaction.target):
            return 'TV pick waiting'
    return None


def _tick(_handle):
    held = _held
    if held is None:
        _put_away('gone')
        return
    try:
        if not _first_person(held.sim) or held.prop.id not in held.prop.manager:
            _put_away('left first person' if held.prop.id in held.prop.manager else 'the remote went')
            return
        if held.game and not any(p.id in p.manager for p in held.game):
            _put_away('the game put its remote away')
            return
        now = time.monotonic()
        if not held.placed and native.grip(held.hand) is not None:
            _log('the game showed how it holds the remote: it is in your hand now')
            _attach(held, held.hand)
        if held.game and not held.game_logged:
            _log_game_remote(held)
        if held.check_at is not None and now >= held.check_at:
            _check_place(held)
        why = _reasons(held)
        if why is not None:
            held.last = now
            if why != held.why:
                held.why = why
                _log('remote kept: {}'.format(why))
        elif now - held.last > LINGER:
            _put_away('nothing to do with the TV')
    except Exception as e:
        _log('remote check failed: {!r}'.format(e))
        _put_away('error')


def _wrap_get_prop():
    """The game's remote animation makes its own remote: learn which prop that
    is, and hide it while yours is in your hand."""
    import animation
    original = animation.AnimationContext.__dict__.get('_get_prop')
    if original is None:
        native._log('remote: AnimationContext._get_prop not found; the remote stays the game\'s')
        return

    def _get_prop(self, asm, prop_name, definition_id, interaction=None):
        global _remote_def
        prop = original(self, asm, prop_name, definition_id, interaction)
        try:
            if prop is not None and interaction is not None and _is_remote_interaction(interaction):
                if _first_person(interaction.sim):
                    native.grip_learn(True)
                    _log('the game\'s remote animation starts: learning how it holds the remote')
                if not _remote_def:
                    _remote_def = prop.definition.id
                    _log('learned the remote prop from the animation: {} ({:#x})'.format(prop_name, _remote_def))
                held = _held
                if held is not None and interaction.sim is held.sim and prop is not held.prop and \
                        prop.definition.id == held.prop.definition.id and prop not in held.game:
                    from objects import VisibilityState
                    prop.visibility = VisibilityState(False)
                    held.game.append(prop)
                    _log('the animation\'s own remote hidden; yours stays in your {} hand'.format(
                        'right' if held.hand else 'left'))
        except Exception as e:
            _log('remote prop check failed: {!r}'.format(e))
        return prop

    animation.AnimationContext._get_prop = _get_prop


def _wrap_queue_append():
    from event_testing.results import TestResult
    from interactions.interaction_finisher import FinishingType
    from interactions.interaction_queue import InteractionQueue
    original = InteractionQueue.__dict__.get('append')
    watch = _watch_class()
    if original is None or watch is None:
        native._log('remote: InteractionQueue.append or WatchSuperInteraction not found; TVs keep the game\'s way')
        return

    def append(self, interaction, *args, **kwargs):
        value = None
        try:
            if _first_person(getattr(interaction, 'sim', None)):
                if getattr(interaction, '_sims4vr_gaze', False):
                    pass  # tvwatch.py: watching because you look at the TV, already where you are
                elif isinstance(interaction, watch):
                    value = getattr(interaction.affordance, 'required_channel', None)
                elif getattr(interaction, 'is_super', False) and not hasattr(interaction, '_sims4vr_channel') \
                        and _is_tv_use(interaction.affordance, interaction.target):
                    _stay_put(interaction)  # Turn Off, Watch TV (picks a channel, then a watch comes here)
        except Exception as e:
            _log('TV check failed: {!r}'.format(e))
            value = None
        if value is None:
            return original(self, interaction, *args, **kwargs)
        sim, tv = interaction.sim, interaction.target
        try:
            interaction.cancel(FinishingType.INTERACTION_QUEUE, cancel_reason_msg='VR first person: the remote instead')
        except Exception:
            pass
        try:
            if tv.get_state(value.state) is value:
                _log('{} already on {}: nothing to do'.format(tv, value))
                return TestResult(False, 'VR first person: already on that channel')
        except Exception:
            pass
        try:
            _use_remote(sim, tv, value)
        except Exception as e:
            _log('remote failed: {!r}'.format(e))
            _set_directly(tv, value, 'remote failed')
        return TestResult(False, 'VR first person: switched with the remote where you are')

    InteractionQueue.append = append


try:
    _wrap_queue_append()
except Exception as _e:
    native._log('remote: wrapping the queue failed: {!r}'.format(_e))
try:
    _wrap_get_prop()
except Exception as _e:
    native._log('remote: wrapping the animation props failed: {!r}'.format(_e))
