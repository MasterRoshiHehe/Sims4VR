"""Records the camera position and target the game client reports to Python.

The client sends them with the 'update.camera.information' command, which calls
camera.update(). Wrapping that function gives the mod the latest values.
"""
import time

import camera

from sims4vr import body, hooks, native

latest = None  # (time, (x, y, z) position, (x, y, z) target)
_calls = 0


def _update(original, *args, **kwargs):
    global latest, _calls
    try:
        position = kwargs.get('camera_position')
        target = kwargs.get('target_position')
        if position is not None and target is not None:
            latest = (time.time(), (position.x, position.y, position.z), (target.x, target.y, target.z))
            native.set_floor(target.y)
            _calls += 1
            if _calls <= 20 or _calls % 100 == 0:
                native._log('camera.update #{} at {:.2f}: position {} target {}'.format(_calls, latest[0], latest[1], latest[2]))
    except Exception:
        pass
    try:
        body.tick()
    except Exception:
        pass
    return original(*args, **kwargs)


hooks.hook('camwatch', camera, 'update', _update)


def current():
    """(position, target, age in seconds) or None if the client never reported."""
    if latest is not None:
        return latest[1], latest[2], time.time() - latest[0]
    position = getattr(camera, '_camera_position', None)
    target = getattr(camera, '_target_position', None)
    if position is None or target is None:
        return None
    return (position.x, position.y, position.z), (target.x, target.y, target.z), None
