"""Body mode: the VR view sits at the active Sim's eyes.

The DLL reads the head joint every frame; Python tells it which Sim to follow
and where that Sim stands (for a sanity check on the joint's space). tick() runs
whenever the client reports the camera, and follows the active Sim when the
player switches Sims.
"""
import services

from distributor.ops import OverrideWallsUp, SetWallsUpOrDown
from distributor.system import Distributor

from sims4vr import hands, native, walk

enabled = False
_following = 0
_still = None  # the Sim whose autonomy is off for first person
_saved_autonomy = {}  # Sim id: its own autonomy setting from before first person


def _autonomy(sim, off):
    """In first person your Sim does nothing on its own: you decide what it
    does. Needs failing (wetting itself, passing out) still happen, the game
    forces those. Afterwards the Sim gets back exactly the setting it had
    (so with the game's "selected Sim autonomy" off it stays off), unless the
    game changed it meanwhile (switching Sims with that option off).

    LIMITED_ONLY, the game's own "selected Sim autonomy off" level (v0.13.173;
    DISABLED before): full autonomy never runs at that level
    (autonomy_modes.py FullAutonomy.test), but sub-action autonomy does
    (_MixerAutonomy.test refuses only DISABLED), and that is what picks the
    mixers of what you are doing. Cooking's steps are mixers
    (crafting_interactions.py CraftingStepInteraction: each one finished
    advances the recipe), so with DISABLED the Sim stood at the counter
    doing nothing while the skill still rose (user 2026-10-16 11:36Z)."""
    try:
        from autonomy.settings import AutonomyState
        still = AutonomyState.LIMITED_ONLY
        group = sim.get_autonomy_settings_group()
        current = sim.autonomy_settings.get_setting(AutonomyState, group)
        if off:
            if current not in (still, AutonomyState.DISABLED) or (
                    current == still and sim.id not in _saved_autonomy):
                # DISABLED is ours from an older build, never the Sim's own setting.
                _saved_autonomy[sim.id] = current
            sim.autonomy_settings.set_setting(still, group)
            native._log('autonomy limited for Sim {} (it was {})'.format(sim.id, current))
            return
        saved = _saved_autonomy.pop(sim.id, None)
        if current not in (still, AutonomyState.DISABLED):
            native._log('autonomy of Sim {} left at {} (the game changed it)'.format(sim.id, current))
            return
        if saved is None:  # never saved (an older session): the game's own choice
            service = services.autonomy_service()
            enabled_ = getattr(service, '_selected_sim_autonomy_enabled', True)
            saved = AutonomyState.UNDEFINED if enabled_ else AutonomyState.LIMITED_ONLY
        sim.autonomy_settings.set_setting(saved, group)
        native._log('autonomy of Sim {} back to {}'.format(sim.id, saved))
    except Exception as e:
        native._log('autonomy setting failed: {!r}'.format(e))


def _keep_still(sim):
    global _still
    if _still is sim:
        return
    if _still is not None:
        _autonomy(_still, False)
    _still = sim
    if sim is not None:
        _autonomy(sim, True)


def following():
    """Id of the Sim you are in (0: not in first person)."""
    return _following if enabled else 0


def _active_sim():
    try:
        return services.get_active_sim()
    except Exception:
        return None


def _walls_up(up):
    """Full walls (no cutaway) while in first person; the game's own choice otherwise.

    Never send SetWallsUpOrDown(True): that is the game's "ungreeted visitor"
    state, which hides the inside of the house (furniture, upper floors).
    Sending False clears it if an older build left it on.
    """
    try:
        distributor = Distributor.instance()
        distributor.add_op_with_no_owner(SetWallsUpOrDown(False))
        distributor.add_op_with_no_owner(OverrideWallsUp(override=up))
    except Exception as e:
        native._log('walls up failed: {}'.format(e))


def _tv(what):
    """Watching a TV by looking at it (tvwatch.py)."""
    try:
        from sims4vr import tvwatch
        getattr(tvwatch, what)()
    except Exception as e:
        native._log('tvwatch {} failed: {!r}'.format(what, e))
    try:  # Sims Forever on a computer (simsforever.py), started and stopped with it
        from sims4vr import simsforever
        getattr(simsforever, what)()
    except Exception as e:
        native._log('simsforever {} failed: {!r}'.format(what, e))


def enable():
    """Follow the active Sim. Returns its name, or None if there is no active Sim."""
    global enabled
    enabled = True
    _walls_up(True)
    walk.start()
    hands.start()
    _tv('start')
    try:  # who plays which sound, and on what (soundlog.py)
        from sims4vr import soundlog
        soundlog.install()
    except Exception as e:
        native._log('sound log failed to start: {!r}'.format(e))
    sim = _active_sim()
    if sim is None:
        return None
    _follow(sim)
    return '{} {}'.format(getattr(sim, 'first_name', ''), getattr(sim, 'last_name', '')).strip() or str(sim.id)


def disable():
    global enabled, _following
    was_enabled = enabled
    enabled = False
    _following = 0
    hands.stop()
    _tv('stop')
    native.set_body(0)
    walk.stop()
    walk.reset_sleep()
    _keep_still(None)
    if was_enabled:
        _walls_up(False)


def _follow(sim):
    global _following
    if sim.id != _following:
        if _following:
            hands.release_all('switched Sims')
        _following = sim.id
        native.set_body(sim.id)
        native._log('body mode following Sim {}'.format(sim.id))
    _keep_still(sim)
    native.set_body_root(sim.position, sim.orientation)


def tick():
    if enabled:
        sim = _active_sim()
        if sim is not None:
            _follow(sim)
