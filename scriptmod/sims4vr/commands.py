"""Cheat console commands (Ctrl+Shift+C): vr.start, vr.recenter, vr.mode, vr.status."""
import sims4.commands

from sims4vr import native

try:
    from sims4vr import autostereo  # noqa: F401
except Exception as e:
    native._log('autostereo unavailable: {!r}'.format(e))

try:
    from sims4vr import pie  # noqa: F401
except Exception as e:
    native._log('pie menu at hand unavailable: {!r}'.format(e))

try:
    from sims4vr import scripted  # noqa: F401
except Exception as e:
    native._log('no-auto-eat unavailable: {!r}'.format(e))

try:
    from sims4vr import onesim  # noqa: F401
except Exception as e:
    native._log('one Sim in first person unavailable: {!r}'.format(e))

try:
    from sims4vr import reach  # noqa: F401
except Exception as e:
    native._log('reach-only picks unavailable: {!r}'.format(e))

try:
    from sims4vr import remote  # noqa: F401
except Exception as e:
    native._log('far TVs/lights unavailable: {!r}'.format(e))

try:
    from sims4vr import saving  # noqa: F401
except Exception as e:
    native._log('save hook unavailable: {!r}'.format(e))

try:
    from sims4vr import phone  # noqa: F401
except Exception as e:
    native._log('phone unavailable: {!r}'.format(e))

try:
    from sims4vr import buildbuy  # noqa: F401
except Exception:
    import traceback
    native._log(traceback.format_exc())

try:
    from sims4vr import objmenu  # noqa: F401
except Exception as e:
    native._log('menus on objects unavailable: {!r}'.format(e))

try:
    from sims4vr import camwatch
except Exception as e:  # the camera finder is optional; VR still works without it
    camwatch = None
    native._log('camwatch unavailable: {!r}'.format(e))

Live = sims4.commands.CommandType.Live


@sims4.commands.Command('vr.start', command_type=Live)
def vr_start(_connection=None):
    output = sims4.commands.CheatOutput(_connection)
    error = native.start()
    output(error or 'Sims4VR started. Put on your headset.')


@sims4.commands.Command('vr.recenter', command_type=Live)
def vr_recenter(_connection=None):
    output = sims4.commands.CheatOutput(_connection)
    if not native.loaded():
        output(native.status())
        return
    native.recenter()
    output('Recentered.')


@sims4.commands.Command('vr.mode', command_type=Live)
def vr_mode(mode: str = None, _connection=None):
    output = sims4.commands.CheatOutput(_connection)
    if mode not in ('quad', 'stereo', 'body'):
        output('Usage: vr.mode quad|stereo|body')
        return
    if not native.loaded():
        output(native.status())
        return
    native.set_mode(mode != 'quad')
    if mode == 'body':
        from sims4vr import body
        name = body.enable()
        output('Mode: body, seeing through {}. Recenter (vr.recenter) while looking straight ahead.'.format(name)
               if name else 'Mode: body. No active Sim yet; the view joins one when you select a Sim.')
    else:
        try:
            from sims4vr import body
            body.disable()
        except Exception:
            pass
        output('Mode: ' + mode)


@sims4.commands.Command('vr.scale', command_type=Live)
def vr_scale(scale: float = None, _connection=None):
    output = sims4.commands.CheatOutput(_connection)
    if scale is None or not 0.01 < scale < 100:
        output('Usage: vr.scale 1.0  (bigger = you move further for each step, so the world feels smaller; '
               'set world_scale in sims4vr.ini to keep it)')
        return
    native.set_scale(scale)
    output('World scale: {}'.format(scale))


@sims4.commands.Command('vr.status', command_type=Live)
def vr_status(_connection=None):
    sims4.commands.CheatOutput(_connection)(native.status())


@sims4.commands.Command('vr.findcamera', command_type=Live)
def vr_findcamera(_connection=None):
    output = sims4.commands.CheatOutput(_connection)
    if camwatch is None:
        output('Camera watcher failed to load, see sims4vr_python.log.')
        return
    reported = camwatch.current()
    if reported is None:
        output('The game has not reported a camera position yet. Move the camera a little and try again.')
        return
    position, target, age = reported
    output('Camera position {:.2f} {:.2f} {:.2f}, target {:.2f} {:.2f} {:.2f}{}'.format(
        *position, *target, '' if age is None else ' ({:.1f} s old)'.format(age)))
    output('Searching memory, the game may pause for a few seconds...')
    output(native.find_camera(position, target))
