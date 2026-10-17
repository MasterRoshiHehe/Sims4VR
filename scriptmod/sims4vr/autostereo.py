"""Stereo while playing on a lot; the virtual screen in menus and loading screens.

Wraps Zone.on_loading_screen_animation_finished (the loading screen is gone and
play starts) and Zone.on_teardown (leaving the lot: main menu, world map,
travel). The DLL ignores both when auto_stereo = 0 in sims4vr.ini, and with
play_mode = body play starts in first person.
"""
import zone

from sims4vr import native


def _wrap(name, playing):
    original = getattr(zone.Zone, name, None)
    if original is None:
        native._log('autostereo: Zone.{} not found'.format(name))
        return

    def wrapper(self, *args, **kwargs):
        if not playing:
            steps = [('body', 'disable'), ('controls', 'stop')]
        else:
            steps = [('probe', 'start'), ('controls', 'start')]
        for module, call in steps:  # each on its own: one failing doesn't block the rest
            try:
                getattr(__import__('sims4vr.' + module, fromlist=[call]), call)()
            except Exception as e:
                native._log('autostereo {}: {}.{} failed: {!r}'.format(name, module, call, e))
        try:
            if native.on_gameplay(playing) == 2:
                from sims4vr import body
                native._log('play_mode body: first person on {}'.format(body.enable()))
        except Exception as e:
            native._log('autostereo {}: {!r}'.format(name, e))
        return original(self, *args, **kwargs)

    setattr(zone.Zone, name, wrapper)


_wrap('on_loading_screen_animation_finished', True)
_wrap('on_teardown', False)
