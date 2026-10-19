"""First person: "Open Claude" on a computer (user 2026-10-14 16:35Z: write
bug reports and ideas without taking the headset off).

Computers get a pie option "Open Claude" (only for the Sim you are in, only
in first person; god view never shows it). Picking it puts the Claude desktop
app on that computer's screen (claudescreen.cpp): its window comes to the
front so your real mouse and keyboard type into it, and its picture is drawn
on the screen the same way as Sims Forever's (shaped like the screen,
screenmesh.py). The laser or a finger on the screen is the mouse. The option
reads "Close Claude" while it is open; walking away from the computer
(farther than CLOSE_DISTANCE), Sims Forever starting, or leaving first person
also closes it. The app itself keeps running; its window gets its old place
back.

While Claude is on a screen, no other Sim gets options on that object
(autonomy included): it is yours alone until it closes.

TVs offer it too while [claude] tvs is on (debug menu: Menus > Open Claude
on TVs too; user 2026-10-14 23:53Z), from wherever you are.

Switch: [claude] on in sims4vr.ini; [claude] exe = where claude.exe is when
it isn't in the usual place (empty: %LOCALAPPDATA%\\AnthropicClaude).
"""
from sims4vr import body, hooks, native

CLOSE_DISTANCE = 2.0  # metres from the computer's nearest edge
TV_CLOSE_DISTANCE = 10.0  # a TV is used from the sofa
GUID = 0x5653434C41554445  # our interaction's id (not in the game's tuning)

_open = None  # the computer's id while Claude is on its screen
_affordance = None
_wrapped = False
_logs = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 60:
        native._log('claude: ' + message)


_notes = {}  # what the pie did with Open Claude, logged a few times each (user 20:18Z: still not shown)


def _note(key, message, most=3):
    n = _notes.get(key, 0)
    if n < most:
        _notes[key] = n + 1
        _log(message)


def _tested(result, target):
    _note('test', 'tested on {}: {}'.format(target, 'passed' if result else getattr(result, 'reason', result)), 6)
    return result


def _owner(target):
    if target is not None and getattr(target, 'is_part', False):
        return getattr(target, 'part_owner', target)
    return target


def _fp_sim():
    from sims4vr import simsforever
    return simsforever._first_person_sim()


def _is_computer(obj):
    from sims4vr import simsforever
    return simsforever._is_computer(obj)


_asking = False  # remote._is_tv lists the TV's options itself: no asking again from inside that


def _is_tv(obj):
    """A TV, when the debug menu's "Open Claude on TVs too" is on ([claude] tvs;
    user 2026-10-14 23:53Z)."""
    global _asking
    if _asking or obj is None or not native.setting_flag('claude', 'tvs', False):
        return False
    _asking = True
    try:
        from sims4vr import remote
        return remote._is_tv(obj)
    except Exception:
        return False
    finally:
        _asking = False


def _offered_on(obj):
    return _is_computer(obj) or _is_tv(obj)


def _show(obj):
    global _open
    from sims4vr import simsforever
    centre, normal, up, width, height, video = simsforever._screen(obj)
    shape = None
    if video:
        try:
            from sims4vr import screenmesh
            shape = screenmesh.surface(obj)
        except Exception as e:
            _log('screen shape failed: {!r}'.format(e))
    native.sims_forever_surface(*(shape or (0, 0, ())))
    native.claude_screen(True, centre, normal, up, width, height,
                         native.setting('claude', 'exe', '') or '')
    _open = getattr(obj, 'id', None)
    _log('open on {}: screen at {}, {:.2f} x {:.2f}'.format(obj, tuple(round(v, 2) for v in centre), width, height))


def close(why):
    global _open
    if _open is None:
        return
    _open = None
    native.claude_screen(False)
    _log('closed ({})'.format(why))


def toggle(obj):
    obj = _owner(obj)
    if obj is None:
        return
    if _open is not None and _open == getattr(obj, 'id', None):
        close('you picked Close Claude')
        return
    from sims4vr import simsforever
    if simsforever._shown is not None:
        _log('Sims Forever is on the screen; Claude not opened')
        return
    _show(obj)


def tick(sim):
    """simsforever.py's tick (every 0.5 s in first person): closes Claude when
    you walked away from the computer or it's gone."""
    if _open is None:
        return
    import services
    obj = services.object_manager().get(_open)
    if obj is None or sim is None:
        close('the computer is gone' if obj is None else 'not in first person')
        return
    try:
        from sims4vr import reach
        d = reach._distance(sim, obj)
    except Exception:
        d = None
    if d is None:
        p, q = sim.position, obj.position
        d = ((p.x - q.x) ** 2 + (p.z - q.z) ** 2) ** 0.5
    limit = CLOSE_DISTANCE if _is_computer(obj) else TV_CLOSE_DISTANCE
    if d > limit:
        close('you walked {:.1f} m away from the screen'.format(d))


def _make_affordance():
    """Our "Open Claude" interaction: a copy of one of the game's immediate
    interactions (so every tuned value it needs exists) with its own test,
    name and action."""
    import services
    import sims4.resources
    from event_testing.results import TestResult
    from interactions.base.immediate_interaction import ImmediateSuperInteraction
    from interactions.utils.outcome import InteractionOutcomeNone
    from sims4.localization import LocalizationHelperTuning
    from sims4.utils import flexmethod
    from singletons import DEFAULT

    manager = services.get_instance_manager(sims4.resources.Types.INTERACTION)
    # A plain one (tuned straight from ImmediateSuperInteraction, no script class of
    # its own that lists its options its own way: v0.13.87 took a computer
    # Locking one, which offers no option unless a lock is set up).
    base = fallback = None
    for cls in list(manager.types.values()):
        try:
            if cls.__bases__[0] is not ImmediateSuperInteraction or getattr(cls, 'debug', False) or \
                    getattr(cls, 'cheat', False):
                continue
        except (TypeError, AttributeError, IndexError):
            continue
        name = cls.__name__.lower()
        if 'lock' in name:
            continue
        if 'computer' in name:
            base = cls
            break
        if fallback is None:
            fallback = cls
    base = base or fallback
    if base is None:
        _log('no immediate interaction to build on; no Open Claude option')
        return None

    @flexmethod
    def test(cls, inst, target=DEFAULT, context=DEFAULT, **kwargs):
        try:
            if target is DEFAULT:
                target = inst.target
            if context is DEFAULT:
                context = inst.context
            sim = _fp_sim()
            if sim is None or context is None or context.sim is not sim:
                return _tested(TestResult(False, 'VR first person only'), target)
            if not _offered_on(_owner(target)):
                return _tested(TestResult(False, 'not a computer'), target)
            return _tested(TestResult.TRUE, target)
        except Exception as e:
            return _tested(TestResult(False, 'Open Claude test failed: {!r}'.format(e)), target)

    @flexmethod
    def _get_name(cls, inst, target=DEFAULT, context=DEFAULT, **kwargs):
        _note('named', 'pie asked for its name')
        if target is DEFAULT and inst is not None:
            target = inst.target
        obj = _owner(target)
        shown = _open is not None and obj is not None and getattr(obj, 'id', None) == _open
        return LocalizationHelperTuning.get_raw_text('Close Claude' if shown else 'Open Claude')

    def _run_interaction_gen(self, timeline):
        try:
            toggle(self.target)
        except Exception as e:
            _log('opening failed: {!r}'.format(e))
        return True
        yield

    attrs = {
        'test': test,
        '_get_name': _get_name,
        '_run_interaction_gen': _run_interaction_gen,
        'allow_autonomous': False,
        'allow_user_directed': True,
        # The pie's category is `category` (v0.13.87-v0.13.99 set an unused name and
        # kept the base's Locking category: Open Claude sat in that submenu, if it showed at all).
        'category': None,
        'category_on_forwarded': None,
        'guid64': GUID,
        # Nothing of the base's own results: v0.13.101-v0.13.131 kept the outcome of
        # Computer_Locking_LockForEveryone_ButButler, so every Open Claude locked the
        # computer for everyone but a butler (your Sim too: the pie shrank to a few options).
        'outcome': InteractionOutcomeNone(),
        'basic_extras': (),
    }
    affordance = type('vr_OpenClaude', (base,), attrs)
    _log('Open Claude built on {} (its pie category was {})'.format(base.__name__, getattr(base, 'category', None)))
    return affordance


def _wrap_affordances():
    """Computers list Open Claude among their pie options."""
    global _wrapped
    if _wrapped:
        return
    from objects.game_object import GameObject
    from sims4.utils import flexmethod
    # The game's super_affordances is a flexmethod (cls, inst, context=None):
    # keep it one, or every object's options break (no highlight, no pie).
    raw = next((k.__dict__['super_affordances'] for k in GameObject.__mro__ if 'super_affordances' in k.__dict__), None)
    if raw is None:
        _log('GameObject.super_affordances not found; no Open Claude option')
        return

    @flexmethod
    def super_affordances(cls, inst, *args, **kwargs):
        if inst is not None and _open is not None and getattr(inst, 'id', None) == _open:
            # The screen showing Claude is yours alone: other Sims get no options
            # on it (autonomy included) while it's open (user 2026-10-15 00:53Z).
            context = args[0] if args else kwargs.get('context')
            sim = getattr(context, 'sim', None)
            if sim is not None and sim is not _fp_sim():
                _note('locked', '{} kept off {} (Claude is on it)'.format(sim, inst), 10)
                return
        yield from raw.__get__(inst, cls)(*args, **kwargs)
        try:
            # Only the Sim you are in can pick it (its test says so), so every
            # other Sim's autonomy, which asks all objects all the time, stops
            # here before the computer check (perf, coordinator 2026-10-17).
            if inst is None or _affordance is None:
                return
            context = args[0] if args else kwargs.get('context')
            fp = body.following()
            if not fp or getattr(getattr(context, 'sim', None), 'id', None) != fp:
                return
            if _offered_on(inst):
                _note('offered', 'offered on {} (for {}, shift {})'.format(
                    inst, getattr(context, 'sim', None), getattr(context, 'shift_held', None)), 6)
                yield _affordance
        except Exception:
            pass

    GameObject.super_affordances = super_affordances
    _wrapped = True
    try:
        from interactions.choices import ChoiceMenu

        def add_aop_logged(add_aop, self, aop, *args, **kwargs):
            before = len(self)
            result = add_aop(self, aop, *args, **kwargs)
            try:
                if _affordance is not None and aop.affordance is _affordance:
                    _note('added', 'pie on {}: {} ({}; menu has {} options)'.format(
                        aop.target, 'added' if len(self) > before else 'left out',
                        getattr(result, 'reason', result), len(self)))
            except Exception:
                pass
            return result

        hooks.hook('claude', ChoiceMenu, 'add_aop', add_aop_logged)

        # A computer's options left out of its pie, with why (user 20:50Z: the
        # computer's pie lost most of its options; 185 one time, 22 the next).
        state = {'context': None, 'target': None, 'tested': 0, 'left_out': 0}

        def is_valid_aop(plain, aop, context, *args, **kwargs):
            result = plain(aop, context, *args, **kwargs)
            try:
                target = _owner(aop.target)
                if _is_computer(target):
                    if context is not state['context']:
                        if state['tested']:
                            _note('count', '  {}: {} options tested, {} left out'.format(
                                state['target'], state['tested'], state['left_out']), 20)
                        state.update(context=context, target=target, tested=0, left_out=0)
                        sim = getattr(context, 'sim', None)
                        posture = getattr(getattr(sim, 'posture_state', None), 'body', None)
                        _note('pie', 'computer pie for {}: shift {}, posture {}'.format(
                            target, getattr(context, 'shift_held', None), posture), 20)
                    state['tested'] += 1
                    if not result:
                        state['left_out'] += 1
                        _note('hidden', '  left out on the computer: {} ({})'.format(
                            getattr(aop.affordance, '__name__', aop.affordance), getattr(result, 'reason', result)), 80)
            except Exception:
                pass
            return result

        hooks.hook('claude', ChoiceMenu, 'is_valid_aop', is_valid_aop)
    except Exception as e:
        _log('pie logging not set up: {!r}'.format(e))


_unlocked = False


def _unlock_computers():
    """Once per game run: take off the "everyone but the butler" player locks
    that older Open Claude builds put on computers."""
    global _unlocked
    if _unlocked:
        return
    import services
    from objects.components.portal_locking_enums import LockPriority, LockType
    manager = services.object_manager()
    if manager is None:
        return
    _unlocked = True
    ours = (LockType.LOCK_ALL_WITH_SIMID_EXCEPTION, LockType.LOCK_ALL_WITH_SITUATION_JOB_EXCEPTION)
    for obj in list(manager.values()):
        try:
            if not _is_computer(obj) or not obj.has_locking_component():
                continue
            locks = obj.get_locking_component().lock_datas
            for lock_type, data in list(locks.items()):
                if lock_type in ours and data.lock_priority == LockPriority.PLAYER_LOCK:
                    obj.remove_locks(lock_type=lock_type, lock_priority=LockPriority.PLAYER_LOCK)
                    _log('unlocked {} ({} left by Open Claude)'.format(obj, LockType(lock_type).name))
        except Exception as e:
            _log('could not unlock {}: {!r}'.format(obj, e))


def start():
    global _affordance
    if not native.setting_flag('claude', 'on', True) or native._optional('claude_screen') is None:
        return
    try:
        if _affordance is None:
            _affordance = _make_affordance()
        _wrap_affordances()
    except Exception as e:
        _log('no Open Claude option: {!r}'.format(e))
    try:
        _unlock_computers()
    except Exception as e:
        _log('computer unlock failed: {!r}'.format(e))


def stop():
    close('first person ended')
