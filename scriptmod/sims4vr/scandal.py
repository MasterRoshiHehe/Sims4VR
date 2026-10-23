# Your Sim's scandal aura in first person (user 2026-10-23 19:46Z: the purple "!" is a scandal from Royalty &
# Legacy). It is no UI piece but a world effect: buff_ScandalSecret_ScandalVFX (493236) plays ep21_scandal_aura on
# the b__head__ slot (buffs/buff.py _start_vfx, vfx.PlayEffect), so in first person it hangs in front of your eyes.
# Effects have no offset of their own (VFXStart: a joint, or a fixed transform), so the menu picks the joint it
# plays on ([bubbles] scandal_spot: 0 head as the game, 1 chest, 2 hips, 3 left hand, 4 right hand, 5 hidden).
# The buff keeps our effect in its _vfx, so the game stops it as its own. Other Sims and god view stay as the game.
import time

from sims4vr import native

EFFECT = 'ep21_scandal_aura'
JOINTS = (None, 'b__Spine2__', 'b__Pelvis__', 'b__L_Hand__', 'b__R_Hand__')
EVERY = 0.5

_last = 0.0
_placed = {}  # sim id: the spot its aura was put on
_ours = {}  # id() of an aura we played: its spot
_errors = 0
_logs = 0


def _spot():
    try:
        spot = int(native.setting('bubbles', 'scandal_spot', '0'))
    except (TypeError, ValueError):
        spot = 0
    return spot if 0 <= spot <= 5 else 0


def _auras(sim):
    try:
        for buff in list(sim.sim_info.Buffs._active_buffs.values()):
            vfx = getattr(buff, '_vfx', None)
            if vfx is not None and getattr(vfx, 'effect_name', None) == EFFECT:
                yield buff, vfx
    except Exception:
        return


def _replay(buff, sim, spot):
    """The buff's aura stopped and played again on the spot's joint (none: hidden)."""
    from vfx import PlayEffect
    old = buff._vfx
    old.stop(immediate=True)
    if spot == 0:
        new = buff.vfx(sim)  # the game's own, on the head
    else:
        import sims4.hash_util
        joint = sims4.hash_util.hash32(JOINTS[spot]) if spot < len(JOINTS) else 0
        new = PlayEffect(sim, effect_name=EFFECT, joint_name=joint)
    buff._vfx = new
    _ours.pop(id(old), None)
    if spot != 0:
        _ours[id(new)] = spot
    if spot != 5:
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
                wanted[sim_id] = 0  # out of first person (or another Sim now): the game's head again
        for sim_id, want in wanted.items():
            sim = services.object_manager().get(sim_id)
            if sim is None:
                _placed.pop(sim_id, None)
                continue
            for buff, vfx in _auras(sim):
                if _ours.get(id(vfx), 0) != want or (want not in (0, 5) and not vfx.is_attached):
                    _replay(buff, sim, want)
                    _logs += 1
                    if _logs <= 30:
                        native._log('scandal: your aura {}'.format(
                            ('on your head (game)', 'on your chest', 'on your hips', 'on your left hand',
                             'on your right hand', 'hidden')[want]))
            if want:
                _placed[sim_id] = want
            else:
                _placed.pop(sim_id, None)
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('scandal: moving your aura failed: {!r}'.format(e))
