"""Developer probe: runs sims4vr_probe.py from the mod folder whenever it changes.

Only with [dev] probe = 1 in sims4vr.ini. The script runs on the game's
simulation thread with `native`, `log` and the game modules available; its
output goes to sims4vr_probe.log next to the DLL (MO2: the overwrite folder).
"""
import os
import traceback

from sims4vr import native

class _Owner:
    pass


_owner = _Owner()
_handle = None
_mtime = None
_scope = {}


def _paths():
    folder = os.path.dirname(native._find_dll() or __file__)
    return os.path.join(folder, 'sims4vr_probe.py'), os.path.join(folder, 'sims4vr_probe.log')


def _tick(_):
    global _mtime
    script, log_path = _paths()
    try:
        mtime = os.path.getmtime(script)
    except OSError:
        return
    if mtime == _mtime:
        return
    first = _mtime is None
    _mtime = mtime
    try:
        with open(script) as f:
            code = f.read()
    except Exception:
        return
    if first and not code.strip():
        return
    lines = []

    def log(*parts):
        lines.append(' '.join(str(p) for p in parts))

    _scope.update(native=native, log=log)
    try:
        exec(compile(code, script, 'exec'), _scope)
    except Exception:
        lines.append(traceback.format_exc())
    try:
        with open(log_path, 'a') as f:
            f.write('--- run at mtime {}\n'.format(mtime))
            f.write('\n'.join(lines) + '\n')
    except Exception:
        pass


def start():
    global _handle
    if _handle is not None or not native.dev_probe():
        return
    import alarms
    import clock
    _handle = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(1.0), _tick, repeating=True)
    native._log('probe: watching sims4vr_probe.py')


def stop():
    global _handle
    if _handle is not None:
        import alarms
        alarms.cancel_alarm(_handle)
        _handle = None
