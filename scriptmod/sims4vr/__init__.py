# Sims4VR script mod. Importing the package loads the native DLL and registers
# the vr.* cheat console commands. Target: the game's embedded Python 3.7.
from sims4vr import native

try:
    from sims4vr import commands  # noqa: F401  (registers commands)
except Exception:
    import traceback
    native._log(traceback.format_exc())

native.load()

try:
    from sims4vr import hooks
    hooks.start()  # [hooks] switches from sims4vr.ini, one log line per wrapped game function
except Exception:
    import traceback
    native._log(traceback.format_exc())
