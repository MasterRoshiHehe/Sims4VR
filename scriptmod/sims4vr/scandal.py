# Your Sim's scandal aura in first person (user 2026-10-23 19:46Z: the purple "!" is a scandal from Royalty &
# Legacy). It is no UI piece but a world effect: buff_ScandalSecret_ScandalVFX (493236) plays ep21_scandal_aura on
# the b__head__ slot (buffs/buff.py _start_vfx, vfx.PlayEffect), so in first person it hangs in front of your eyes.
# Placement = [bubbles] scandal_position (right / up / ahead, m) sent as VFXStart.target_joint_offset on the head
# joint (vfx/__init__.py write); scandal_hide hides it. The buff keeps our effect in its _vfx, so the game stops it
# as its own. Other Sims and god view stay as the game.
import time

from sims4vr import native

EFFECT = 'ep21_scandal_aura'
EVERY = 0.5

_last = 0.0
_placed = {}  # sim id: the spot its aura was put on
_ours = {}  # id() of an aura we played: its spot
_errors = 0
_logs = 0


def _spot():
    """(hidden, (right, up, ahead)); (False, None) = the game's own place."""
    hide = native.setting('bubbles', 'scandal_hide', '0') not in ('0', 'false', 'off', 'no')
    try:
        v = tuple(float(x) for x in native.setting('bubbles', 'scandal_position', '0 0 0').split())
        if len(v) != 3 or not any(v):
            v = None
        else:
            order = (0, 2, 1)  # right/up/ahead -> head joint axes x, z, y (user test 2026-10-23)
            out = [0.0, 0.0, 0.0]
            for i, axis in enumerate(order):
                out[axis] = v[i]  # right/up/ahead -> joint axis
            v = (tuple(out), order)
    except (TypeError, ValueError):
        v = None
    return hide, v


def _auras(sim):
    try:
        for buff in list(sim.sim_info.Buffs._active_buffs.values()):
            vfx = getattr(buff, '_vfx', None)
            if vfx is not None and getattr(vfx, 'effect_name', None) == EFFECT:
                yield buff, vfx
    except Exception:
        return


def _replay(buff, sim, spot):
    """The buff's aura stopped and played again with the spot's offset (hidden: not played)."""
    old = buff._vfx
    old.stop(immediate=True)
    hide, off = spot
    off = off[0] if off is not None else None
    if off is None:
        new = buff.vfx(sim)  # the game's own
    else:
        from vfx import PlayEffect
        from sims4.math import Vector3
        import sims4.hash_util
        new = PlayEffect(sim, effect_name=EFFECT, joint_name=sims4.hash_util.hash32('b__head__'),
                         target_joint_offset=Vector3(off[0], off[1], off[2]))
    buff._vfx = new
    _ours.pop(id(old), None)
    _ours[id(new)] = spot
    if not hide:
        new.start()


def tick():
    """controls.py, every tick: your Sim's aura where the menu says, back on the head outside first person."""
    global _last, _errors, _logs
    now = time.monotonic()
    if now - _last < EVERY:
        return
    _last = now
    try:
        from sims4vr import objmenu
        import services
        you = objmenu._first_person_sim()
        spot = _spot()
        wanted = {}
        if you is not None:
            wanted[you.id] = spot
        for sim_id in list(_placed):
            if sim_id not in wanted:
                wanted[sim_id] = (False, None)  # out of first person (or another Sim now): the game's head again
        for sim_id, want in wanted.items():
            sim = services.object_manager().get(sim_id)
            if sim is None:
                _placed.pop(sim_id, None)
                continue
            for buff, vfx in _auras(sim):
                if _ours.get(id(vfx), (False, None)) != want or (not want[0] and not vfx.is_attached):
                    _replay(buff, sim, want)
                    _logs += 1
                    if _logs <= 30:
                        native._log('scandal: your aura {}'.format('hidden' if want[0] else want[1] or 'as the game'))
            if want != (False, None):
                _placed[sim_id] = want
            else:
                _placed.pop(sim_id, None)
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('scandal: moving your aura failed: {!r}'.format(e))
