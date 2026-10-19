"""First person: "Sims Forever" on a computer plays your real Sims 1.

When your Sim runs the computer's own Sims Forever game (an interaction whose
tuning name has one of [simsforever] interactions in it, on a computer), the
DLL starts the Sims 1 in [simsforever] sims1_path (once; it keeps running in
the background afterwards, paused while it isn't on show) and draws its
picture on the computer's screen: the model's VideoSurface mesh, found the
same way as a TV's screen for the menus on objects (objmenu.py), shaped like
the screen's glass (screenmesh.py: curved screens too). The laser
or a finger on the screen is Sims 1's mouse (vr.cpp, simsforever.cpp). When
the game ends (the interaction stops, or first person ends) the screen goes
back to the game's own picture, and Sims 1 is closed outright by the DLL (no
save question). While Sims 1 is on the screen the computer's own sounds are
muted: the game's PlaySound on that computer or on your Sim (state and
interaction audio; your Sim's voice excepted) is stopped and kept from
starting; animation sound events aren't touched.

Only your Sim, only in first person (body.py starts and stops it). God view:
nothing changes. Switch: [simsforever] on in sims4vr.ini.
"""
import itertools
import weakref

import alarms
import clock

from sims4vr import hooks, native

TICK_SECONDS = 0.5

_owner = type('_Owner', (), {})()
_alarm = None
_shown = None   # (interaction id, object id) on the screen
_seen = set()   # computer interaction names already logged
_muted = None   # (computer id, your Sim's id) whose own sounds are muted
_sounds = weakref.WeakSet()  # the game's sounds playing (started through PlaySound)
_wrapped = False
_through = 0  # sounds logged as let through while Sims 1 is on
_logs = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 120:
        native._log('simsforever: ' + message)


def _settings():
    names = native.setting('simsforever', 'interactions', 'playgame_simsforever, playgametournament_simsforever, livestream_simsforever, game_stream_simsforever') or ''
    return {
        'on': native.setting_flag('simsforever', 'on', True),
        'exe': native.setting('simsforever', 'sims1_path', '') or '',
        'args': native.setting('simsforever', 'sims1_args', '-w') or '',
        'names': [n.strip().lower() for n in names.split(',') if n.strip()],
    }


_cfg = None


def _first_person_sim():
    try:
        from sims4vr import body
        if body.enabled and body.following():
            import services
            return services.object_manager().get(body.following())
    except Exception:
        pass
    return None


def _target(interaction):
    obj = getattr(interaction, 'target', None)
    if obj is not None and getattr(obj, 'is_part', False):
        obj = getattr(obj, 'part_owner', obj)
    return obj


def _is_computer(obj):
    if obj is None or getattr(obj, 'is_sim', False):
        return False
    names = (type(obj).__name__, getattr(getattr(obj, 'definition', None), 'name', '') or '')
    return any('computer' in (n or '').lower() or 'laptop' in (n or '').lower() for n in names)


def _sims_forever(sim):
    """The running interaction that is the Sims Forever game, or None. Logs
    each computer interaction's name once (to find the game's)."""
    try:
        running = itertools.chain((sim.queue.running,), sim.si_state)
    except Exception:
        return None
    for interaction in running:
        if interaction is None:
            continue
        name = getattr(getattr(interaction, 'affordance', None), '__name__', '') or type(interaction).__name__
        lower = name.lower()
        obj = _target(interaction)
        computer = _is_computer(obj)
        if computer and name not in _seen and len(_seen) < 60:
            _seen.add(name)
            _log('your Sim runs {} on {}'.format(name, obj))
        if obj is not None and any(n in lower for n in _cfg['names']):
            return interaction
    return None


def _screen(obj):
    """Centre, normal, up, width, height of the computer's screen (world units)."""
    from sims4vr import objmenu
    centre, normal, up, width, height, video = objmenu._front(obj, True)
    if not video:
        # No screen in the model: a 4:3 screen 55% of the front's width, near its top.
        w = width * 0.55
        h = min(w * 0.75, height * 0.6)
        lift = height / 2 - height * 0.08 - h / 2
        centre = tuple(centre[i] + up[i] * lift for i in range(3))
        width, height = w, h
    return centre, normal, up, width, height, video


def _owner_id(target):
    if target is not None and getattr(target, 'is_part', False):
        target = getattr(target, 'part_owner', target)
    return getattr(target, 'id', None)


def _sound_name(sound):
    name = getattr(sound, 'sound_name', None)
    sid = getattr(sound, 'sound_id', None)
    return '{} ({})'.format(name, sid) if name else str(sid)


def _silenced(sound):
    """A sound kept quiet while Sims 1 is on the screen: one on that computer,
    or on your Sim (the game's own computer game sounds play on either); your
    Sim's voice isn't."""
    if not _muted:
        return False
    owner = _owner_id(getattr(sound, 'target', None))
    if owner is None:
        return False
    if owner == _muted[0]:
        return True
    return owner == _muted[1] and not getattr(sound, 'is_vox', False)


def _wrap_sound():
    """The game's PlaySound: none starts on the computer Sims 1 is on, or on
    your Sim playing it (its voice excepted)."""
    global _wrapped
    if _wrapped:
        return
    from audio.primitive import PlaySound

    def start(original, self):
        global _through
        try:
            if _silenced(self):
                self._manually_distributed = True  # a later stop() sends a harmless stop, no error
                _log('kept quiet: {} on {}'.format(_sound_name(self), getattr(self, 'target', None)))
                return None
        except Exception as e:
            _log('sound wrapper failed: {!r}'.format(e))
        result = original(self)
        try:
            _sounds.add(self)
            if _muted and _through < 30:
                _through += 1
                _log('sound let through: {} on {}'.format(_sound_name(self), getattr(self, 'target', None)))
        except Exception:
            pass  # not weak-referenceable: not tracked
        return result

    hooks.hook('simsforever', PlaySound, 'start', start)
    _wrapped = True


def _mute(obj, sim):
    global _muted
    _muted = (getattr(obj, 'id', None), getattr(sim, 'id', None))
    stopped = []
    for sound in list(_sounds):
        try:
            if _silenced(sound):
                sound._stop_sound()  # stays attached: the game's own stop later is normal
                stopped.append(_sound_name(sound))
        except Exception as e:
            _log('stopping a sound failed: {!r}'.format(e))
    _log('the computer\'s own sounds muted ({} playing stopped: {})'.format(len(stopped), ', '.join(stopped)))


def _show(interaction, obj, sim=None):
    global _shown
    try:
        from sims4vr import claude
        claude.close('Sims Forever started on a computer')
    except Exception:
        pass
    centre, normal, up, width, height, video = _screen(obj)
    shape = None
    if video:
        try:
            from sims4vr import screenmesh
            shape = screenmesh.surface(obj)  # a curved screen's shape, so the picture lies on its glass
        except Exception as e:
            _log('screen shape failed: {!r}'.format(e))
    native.sims_forever_surface(*(shape or (0, 0, ())))
    native.sims_forever(True, centre, normal, up, width, height, _cfg['exe'], _cfg['args'])
    _shown = (getattr(interaction, 'id', None), getattr(obj, 'id', None))
    _mute(obj, sim)
    _log('{} on {}: Sims 1 on its {} at {}, {:.2f} x {:.2f}'.format(
        getattr(interaction.affordance, '__name__', interaction), obj,
        'screen' if video else 'screen (guessed, no screen mesh found)',
        tuple(round(v, 2) for v in centre), width, height))


def _hide(why):
    global _shown, _muted
    _muted = None
    if _shown is not None:
        _shown = None
        native.sims_forever(False)
        _log('off ({})'.format(why))


def _tick(_handle):
    try:
        sim = _first_person_sim()
        try:
            from sims4vr import claude
            claude.tick(sim)
        except Exception as e:
            _log('claude tick failed: {!r}'.format(e))
        if sim is None:
            _hide('not in first person')
            return
        interaction = _sims_forever(sim)
        if interaction is None:
            _hide('the game ended')
            return
        obj = _target(interaction)
        if _shown != (getattr(interaction, 'id', None), getattr(obj, 'id', None)):
            _show(interaction, obj, sim)
    except Exception as e:
        _log('tick failed: {!r}'.format(e))


def start():
    global _alarm, _cfg
    _cfg = _settings()
    if not _cfg['on'] or _alarm is not None:
        return
    if native._optional('sims_forever') is None:
        _log('the DLL is older than the script: off')
        return
    try:
        from sims4vr import claude
        claude.start()  # "Open Claude" on computers
    except Exception as e:
        _log('Open Claude failed to start: {!r}'.format(e))
    try:
        _wrap_sound()
    except Exception as e:
        _log('can\'t mute the computer\'s sounds: {!r}'.format(e))
    _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
    _log('on (Sims 1: {} {}; games named {})'.format(_cfg['exe'] or '(no path)', _cfg['args'], ', '.join(_cfg['names'])))


def stop():
    global _alarm
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    _hide('first person ended')
    try:
        from sims4vr import claude
        claude.stop()
    except Exception:
        pass
