"""First person: "Open Discord" on a TV, a computer or any other object with a
screen (user 2026-10-28: a friend's stream on the TV at home, at the same time
as Claude on a computer).

The pie option is claude.py's (make_affordance), so a screen object with
several screens lists them in a sub menu named "Open Discord"; one screen: the
plain option, "Close Discord" while it is on. Picking it puts the Discord
desktop app's window on that screen (claudescreen.cpp slot 1): the window stays
where it is on your desktop (start the stream, or pop it out, as usual; a
minimised Discord is brought back from the taskbar without taking the front),
and its picture is drawn on the screen. Discord's sound stays on your desktop
(Virtual Desktop streams it to the headset). The laser clicks into the window
unless [discord] interactive is turned off (default on, user 2026-10-28: as Claude's screen).

Costs: pictures are taken on the capture thread at [discord] fps (30) and
scaled down to [discord] max_width (1920); nothing is taken while the screen
isn't drawn. Claude and Discord can be on two screens (or two screens of one
object) at once; not on the same screen.

Switch: [discord] on in sims4vr.ini (debug menu: Menus pickers). The window is
the largest Discord window, or the one whose title contains [discord] title.
"""
from sims4vr import claude, native

SLOT = 1
GUID = 0x5653434C44495343

_open = None  # the screen object's id while Discord is on its screen
_open_index = 0
_affordance = None
_last = None  # the options last sent to the DLL


def _log(message):
    claude._log('discord: ' + message)


def shown_on(obj, index=0):
    """Discord is on this screen of this object."""
    return _open is not None and _open == getattr(claude._owner(obj), 'id', None) and _open_index == index


def _options():
    return (int(float(native.setting('discord', 'fps', 30) or 30)), int(float(native.setting('discord', 'max_width', 1920) or 1920)),
            1 if native.setting_flag('discord', 'interactive', True) else 0, native.setting('discord', 'title', '') or '',
            native.setting('discord', 'exe', '') or '')


_audio = None


def _push_audio():
    """The sound options ([discord] mute_off_screen, sound_3d), passed on when they change."""
    global _audio
    mode = (1 if native.setting_flag('discord', 'mute_off_screen', False) else 0) | (
        2 if native.setting_flag('discord', 'sound_3d', False) else 0)
    if mode != _audio:
        _audio = mode
        native.discord_audio(mode)
        _log('sound: {}'.format({0: 'left alone', 1: 'muted while not on a screen', 2: 'placed at the screen',
                                 3: 'muted while not on a screen, placed at the screen'}[mode]))


def _push(centre, normal, up, width, height):
    global _last
    fps, max_width, interactive, title, exe = _last = _options()
    native.window_screen(SLOT, True, centre, normal, up, width, height, exe, title, fps, max_width, interactive)


def _show(obj, index=0):
    global _open, _open_index
    from sims4vr import simsforever
    centre, normal, up, width, height, _video = simsforever._screen(obj, index)
    _push(centre, normal, up, width, height)
    _open = getattr(obj, 'id', None)
    _open_index = index
    _log('open on {} (screen {}): at {}, {:.2f} x {:.2f}, {} pictures a second up to {} wide, {}'.format(
        obj, index + 1, tuple(round(v, 2) for v in centre), width, height, _last[0], _last[1],
        'laser clicks' if _last[2] else 'view only'))


def close(why):
    global _open
    if _open is None:
        return
    _open = None
    native.window_screen(SLOT, False)
    _log('closed ({})'.format(why))


def toggle(obj, index=None):
    obj = claude._owner(obj)
    if obj is None:
        return
    index = index or 0
    if _open is not None and _open == getattr(obj, 'id', None) and index == _open_index:
        close('you picked Close Discord')
        return
    if claude._open is not None and claude._open == getattr(obj, 'id', None) and claude._open_index == index:
        _log('Claude is on that screen; Discord not opened')
        return
    try:
        from sims4vr import linearboard
        if linearboard.shown_on(obj, index):
            _log('the Linear board is on that screen; Discord not opened')
            return
    except Exception:
        pass
    _show(obj, index)


def tick(sim):
    """claude.tick's (every 0.5 s in first person): closes Discord when you walked away from the
    screen or it's gone, and passes on the debug menu's changed options."""
    _push_audio()
    if _open is None:
        return
    import services
    obj = services.object_manager().get(_open)
    if obj is None or sim is None:
        close('the screen is gone' if obj is None else 'not in first person')
        return
    d = claude.too_far(sim, obj)
    if d is not None:
        close('you walked {:.1f} m away from the screen'.format(d))
        return
    if _options() != _last:
        try:
            from sims4vr import simsforever
            _push(*simsforever._screen(obj, _open_index)[:5])
        except Exception as e:
            _log('options not passed on: {!r}'.format(e))


def spec():
    return {
        'label': 'Discord', 'name': 'vr_OpenDiscord', 'guid': GUID,
        'category_idle': 0x5653434C44490001, 'category_open': 0x5653434C44490002,
        'logo_id': 0x4C4F474F434C4401,
        'offered': lambda obj: native.setting_flag('discord', 'on', True) and claude.any_screen(obj),
        'toggle': toggle,
        'state': lambda obj: (_open is not None and getattr(obj, 'id', None) == _open, _open_index),
    }


def start():
    global _affordance
    if not native.setting_flag('discord', 'on', True) or native._optional('window_screen') is None:
        return
    _push_audio()
    if _affordance is None:
        _affordance = claude.make_affordance(spec())


def stop():
    global _audio
    close('first person ended')
    if _audio:
        _audio = None
        native.discord_audio(0)  # Discord's sound given back
