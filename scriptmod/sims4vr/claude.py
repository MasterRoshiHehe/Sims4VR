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

Objects with more than one screen (a DJ station's three monitors, a karaoke
system's big screen and lyrics screen; screenmesh.names) list their screens
in a sub pie menu under "Open Claude" (user 2026-10-28); with one screen there
is none. discord.py does the same for Discord, from the same factory
(make_affordance).

Switch: [claude] on in sims4vr.ini; [claude] exe = where claude.exe is when
it isn't in the usual place (empty: %LOCALAPPDATA%\\AnthropicClaude).
"""
from sims4vr import body, hooks, native

CLOSE_DISTANCE = 2.0  # metres from the computer's nearest edge
TV_CLOSE_DISTANCE = 10.0  # a TV is used from the sofa
GUID = 0x5653434C41554445  # our interaction's id (not in the game's tuning)

_open = None  # the computer's id while Claude is on its screen
_open_index = 0  # which of its screens
_affordance = None
_offered_logged = set()  # (app, object definition) already logged as offered
_specs = []  # every app made by make_affordance (Claude, Discord): their options are listed on screen objects
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


def tv_object(obj):
    """A TV (an object you can watch)."""
    global _asking
    if _asking or obj is None:
        return False
    _asking = True
    try:
        from sims4vr import remote
        return remote._is_tv(obj)
    except Exception:
        return False
    finally:
        _asking = False


def _is_tv(obj):
    """A TV, when the debug menu's "Open Claude on TVs too" is on ([claude] tvs;
    user 2026-10-14 23:53Z)."""
    return native.setting_flag('claude', 'tvs', False) and tv_object(obj)


def screen_count(obj):
    """How many screens the object's model has (screenmesh.py); 0 when it has none we can see."""
    if obj is None:
        return 0
    try:
        from sims4vr import screenmesh
        return screenmesh.count(obj)
    except Exception:
        return 0


def _offered_on(obj):
    """Claude: computers, TVs when [claude] tvs is on, and other objects with a screen (a DJ station, a karaoke system)."""
    if _is_computer(obj) or _is_tv(obj):
        _want_dump(obj)
        return True
    return not tv_object(obj) and screen_count(obj) > 0


def _want_dump(obj):
    """A computer or TV: if its model shows no screen mesh, the log lists the model's meshes (screenmesh._dump)."""
    try:
        from sims4vr import screenmesh
        screenmesh.want_dump(obj)
    except Exception:
        pass


def any_screen(obj):
    """Discord: every object with a screen."""
    if _is_computer(obj) or tv_object(obj):
        _want_dump(obj)
        return True
    return screen_count(obj) > 0


def _show(obj, index=0):
    global _open, _open_index
    from sims4vr import simsforever
    centre, normal, up, width, height, video = simsforever._screen(obj, index)
    shape = None
    if video:
        try:
            from sims4vr import screenmesh
            shape = screenmesh.surface(obj, index)
        except Exception as e:
            _log('screen shape failed: {!r}'.format(e))
    native.sims_forever_surface(*(shape or (0, 0, ())))
    native.claude_screen(True, centre, normal, up, width, height,
                         native.setting('claude', 'exe', '') or '')
    _open = getattr(obj, 'id', None)
    _open_index = index
    _log('open on {} (screen {}): screen at {}, {:.2f} x {:.2f}'.format(
        obj, index + 1, tuple(round(v, 2) for v in centre), width, height))


def close(why):
    global _open
    if _open is None:
        return
    _open = None
    native.claude_screen(False)
    _log('closed ({})'.format(why))


def toggle(obj, index=None):
    obj = _owner(obj)
    if obj is None:
        return
    shown = _open is not None and _open == getattr(obj, 'id', None)
    if shown and (index is None or index == _open_index):
        close('you picked Close Claude')
        return
    from sims4vr import simsforever
    if simsforever._shown is not None:
        _log('Sims Forever is on the screen; Claude not opened')
        return
    try:
        from sims4vr import discord, linearboard
        if discord.shown_on(obj, index or 0):
            _log('Discord is on that screen; Claude not opened')
            return
        if linearboard.shown_on(obj, index or 0):
            _log('the Linear board is on that screen; Claude not opened')
            return
    except Exception:
        pass
    _show(obj, index or 0)


def too_far(sim, obj):
    """True when `sim` is farther from the screen object than it stays open (computers: CLOSE_DISTANCE, else TV_CLOSE_DISTANCE)."""
    try:
        from sims4vr import reach
        d = reach._distance(sim, obj)
    except Exception:
        d = None
    if d is None:
        p, q = sim.position, obj.position
        d = ((p.x - q.x) ** 2 + (p.z - q.z) ** 2) ** 0.5
    return d if d > (CLOSE_DISTANCE if _is_computer(obj) else TV_CLOSE_DISTANCE) else None


def tick(sim):
    """simsforever.py's tick (every 0.5 s in first person): closes Claude (and Discord) when
    you walked away from the screen or it's gone."""
    try:
        from sims4vr import discord
        discord.tick(sim)
    except Exception as e:
        _log('discord tick failed: {!r}'.format(e))
    try:
        from sims4vr import linearboard
        linearboard.tick(sim)
    except Exception as e:
        _log('linear board tick failed: {!r}'.format(e))
    if _open is None:
        return
    import services
    obj = services.object_manager().get(_open)
    if obj is None or sim is None:
        close('the computer is gone' if obj is None else 'not in first person')
        return
    d = too_far(sim, obj)
    if d is not None:
        close('you walked {:.1f} m away from the screen'.format(d))


# The pie's categories of our own (a sub menu listing a screen object's screens): ids no tuned category uses;
# pie.py lists them with the name given here. Per app one while it is off and one while it is on.
class _Category:
    def __init__(self, key):
        self.guid64 = key


def claude_spec():
    return {
        'label': 'Claude', 'name': 'vr_OpenClaude', 'guid': GUID,
        'category_idle': 0x5653434C43410001, 'category_open': 0x5653434C43410002,
        'logo_id': 0x4C4F474F434C4400,
        'offered': _offered_on, 'toggle': toggle,
        'state': lambda obj: (_open is not None and getattr(obj, 'id', None) == _open, _open_index),
    }


def make_affordance(spec):
    """An "Open <app>" interaction: a copy of one of the game's immediate
    interactions (so every tuned value it needs exists) with its own test,
    name and action. A screen object with several screens gets one pick per
    screen in a sub menu named after the app; one screen: the plain pick.
    spec: label, name, guid, category_idle / category_open (pie category ids),
    offered(obj), toggle(obj, index), state(obj) -> (open here, which screen)."""
    import services
    import sims4.resources
    from event_testing.results import TestResult
    from interactions.base.immediate_interaction import ImmediateSuperInteraction
    from interactions.utils.outcome import InteractionOutcomeNone
    from sims4.localization import LocalizationHelperTuning
    from sims4.utils import flexmethod
    from singletons import DEFAULT

    from sims4vr import pie
    label = spec['label']
    pie.custom_categories[spec['category_idle']] = 'Open ' + label
    pie.custom_categories[spec['category_open']] = label + ' screen'
    pie.custom_category_icons[spec['category_idle']] = pie.custom_category_icons[spec['category_open']] = spec['logo_id']

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
        _log('no immediate interaction to build on; no Open {} option'.format(label))
        return None

    def screens(obj):
        return max(1, screen_count(obj)) if obj is not None else 1

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
            if not spec['offered'](_owner(target)):
                return _tested(TestResult(False, 'not a screen'), target)
            return _tested(TestResult.TRUE, target)
        except Exception as e:
            return _tested(TestResult(False, 'Open {} test failed: {!r}'.format(label, e)), target)

    @flexmethod
    def _get_name(cls, inst, target=DEFAULT, context=DEFAULT, **kwargs):
        _note('named', 'pie asked for its name')
        if target is DEFAULT and inst is not None:
            target = inst.target
        obj = _owner(target)
        shown, current = spec['state'](obj) if obj is not None else (False, 0)
        index = kwargs.get('screen')
        if index is None:
            return LocalizationHelperTuning.get_raw_text(('Close ' if shown else 'Open ') + label)
        try:
            from sims4vr import screenmesh
            names = screenmesh.names(obj)
            text = names[index] if index < len(names) else 'Screen {}'.format(index + 1)
        except Exception:
            text = 'Screen {}'.format(index + 1)
        return LocalizationHelperTuning.get_raw_text(('Close: ' if shown and index == current else '') + text)

    @classmethod
    def get_pie_menu_icon_info(cls, *args, **kwargs):
        """The app's logo in the pie: a made-up icon resource the VR pie draws from the HUD atlas (hud.cpp ownLogo)."""
        import sims4.resources
        from distributor.shared_messages import IconInfoData
        key = sims4.resources.Key(sims4.resources.Types.PNG, spec['logo_id'], 0)
        return IconInfoData(icon_resource=key)

    @classmethod
    def get_pie_menu_category(cls, **kwargs):
        if kwargs.get('screen') is None:
            return None
        return _Category(spec['category_open'] if kwargs.get('shown') else spec['category_idle'])

    @classmethod
    def potential_interactions(cls, target, context, **kwargs):
        obj = _owner(target)
        n = screens(obj)
        if n <= 1:
            yield cls.generate_aop(target, context, **kwargs)
            return
        shown = bool(spec['state'](obj)[0])
        for index in range(n):
            yield cls.generate_aop(target, context, screen=index, shown=shown, **kwargs)

    def _run_interaction_gen(self, timeline):
        try:
            index = (getattr(self, '_kwargs', None) or {}).get('screen')
            spec['toggle'](self.target, index)
        except Exception as e:
            _log('opening failed: {!r}'.format(e))
        return True
        yield

    attrs = {
        'test': test,
        '_get_name': _get_name,
        'get_pie_menu_category': get_pie_menu_category,
        'get_pie_menu_icon_info': get_pie_menu_icon_info,
        'potential_interactions': potential_interactions,
        '_run_interaction_gen': _run_interaction_gen,
        'allow_autonomous': False,
        'allow_user_directed': True,
        # The pie's category is `category` (v0.13.87-v0.13.99 set an unused name and
        # kept the base's Locking category: Open Claude sat in that submenu, if it showed at all).
        'category': None,
        'category_on_forwarded': None,
        'guid64': spec['guid'],
        # Nothing of the base's own results: v0.13.101-v0.13.131 kept the outcome of
        # Computer_Locking_LockForEveryone_ButButler, so every Open Claude locked the
        # computer for everyone but a butler (your Sim too: the pie shrank to a few options).
        'outcome': InteractionOutcomeNone(),
        'basic_extras': (),
    }
    affordance = type(spec['name'], (base,), attrs)
    affordance.vr_spec = spec
    _specs.append(spec)
    spec['affordance'] = affordance
    _log('Open {} built on {} (its pie category was {})'.format(label, base.__name__, getattr(base, 'category', None)))
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
        if inst is not None and any(spec['state'](inst)[0] for spec in _specs):
            # The screen showing Claude (or Discord) is yours alone: other Sims get no options
            # on it (autonomy included) while it's open (user 2026-10-15 00:53Z).
            context = args[0] if args else kwargs.get('context')
            sim = getattr(context, 'sim', None)
            if sim is not None and sim is not _fp_sim():
                _note('locked', '{} kept off {} (a screen app is on it)'.format(sim, inst), 10)
                return
        yield from raw.__get__(inst, cls)(*args, **kwargs)
        try:
            # Only the Sim you are in can pick it (its test says so), so every
            # other Sim's autonomy, which asks all objects all the time, stops
            # here before the computer check (perf, coordinator 2026-10-17).
            if inst is None or not _specs:
                return
            context = args[0] if args else kwargs.get('context')
            fp = body.following()
            if not fp or getattr(getattr(context, 'sim', None), 'id', None) != fp:
                return
            for spec in _specs:
                if not spec['offered'](inst):
                    text = str(inst).lower()
                    key = (spec['label'], text.split(':')[0], 'not')
                    if key not in _offered_logged and len(_offered_logged) < 80 and \
                            any(w in text for w in ('musicproduction', 'dj', 'videostation', 'video_station', 'karaoke', 'mixing')):
                        _offered_logged.add(key)
                        _log('{} NOT offered on {}: computer {}, tv {}, screens {}'.format(
                            spec['label'], inst, _is_computer(inst), tv_object(inst), screen_count(inst)))
                if spec['offered'](inst):
                    _note('offered', 'offered on {} (for {}, shift {})'.format(
                        inst, getattr(context, 'sim', None), getattr(context, 'shift_held', None)), 6)
                    key = (spec['label'], getattr(getattr(inst, 'definition', None), 'id', None))
                    if key not in _offered_logged and len(_offered_logged) < 60:
                        _offered_logged.add(key)
                        _log('{} offered on {} ({} screens)'.format(spec['label'], inst, screen_count(inst)))
                    yield spec['affordance']
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
                if getattr(aop.affordance, 'vr_spec', None) is not None:
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
    if native._optional('claude_screen') is None:
        return
    try:
        if native.setting_flag('claude', 'on', True) and _affordance is None:
            _affordance = make_affordance(claude_spec())
        try:
            from sims4vr import discord
            discord.start()
        except Exception as e:
            _log('Open Discord not set up: {!r}'.format(e))
        try:
            from sims4vr import linearboard
            linearboard.start()
        except Exception as e:
            _log('Open Linear board not set up: {!r}'.format(e))
        if _specs:
            _wrap_affordances()
    except Exception as e:
        _log('no Open Claude option: {!r}'.format(e))
    try:
        _unlock_computers()
    except Exception as e:
        _log('computer unlock failed: {!r}'.format(e))


def stop():
    close('first person ended')
    try:
        from sims4vr import discord
        discord.stop()
    except Exception:
        pass
    try:
        from sims4vr import linearboard
        linearboard.stop()
    except Exception:
        pass
