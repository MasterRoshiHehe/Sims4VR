"""Paired animations in first person: hugs, kisses, held hands (thread "Fix broken paired animations").

The game authors a two-person animation against a jig (socials/jig_group.py): two
slots with a fixed distance and facing, and the animation's arms, heads and
hands only meet when both Sims stand on them. In first person your Sim stays
wherever your body is (scripted._pin), so the jig was placed away from your
body or turned another way: the partner hugged the air a few metres off, from
behind, with arms bent the wrong way, or kissed the air in front of you.

Now the jig is fitted to your body, not the other way round (your headset is
never turned or moved):
- anchor: every jig transform pair the game works out with your Sim in it
  (JigGroup._get_jig_transforms_gen: the planning constraints and the group's
  slots) is moved as one rigid piece so your slot is where your body is, facing
  where your body faces. The partner's slot follows: right in front of you,
  facing you, at the distance the animation was made for. Beyond NEAR m from
  your body the jig is left to the game (a jig on a bed or a dance floor).
- snap: when the animation of a jig social is set up (SocialInteractionMixin.
  setup_asm_default) and you have drifted since the slots were planned, the
  partner is put on its slot again, measured from your body now. A kiss has the
  partner step in by [pair] kiss_in cm more, because your Sim doesn't lean in
  (your head is your headset's).
Facing of the partner before and between animations is facing.py's.

Switches (sims4vr.ini [pair], debug menu Options > Game settings > Other Sims):
anchor (on), snap (on), kiss_in (cm, default 10), lock (on: the stick does not walk you during one).
"""
import math

import sims4.math

from sims4vr import hooks, native

NEAR = 3.0  # m: a jig whose slot for you is this far from your body or less is moved onto it
SNAP_MIN = 0.03  # m: the partner closer than this to its slot stays
SNAP_MAX = 2.0  # m: further than this it is not teleported (the social is not ready)
KISS_IN_DEFAULT = 10.0  # cm

_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 80:
        native._log('pair: ' + message)


def _error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('pair: {} failed: {!r}'.format(what, e))


def _on(key):
    return native.setting_flag('pair', key, True)


def _kiss_in():
    try:
        return max(0.0, float(native.setting('pair', 'kiss_in', str(KISS_IN_DEFAULT)))) / 100.0
    except (TypeError, ValueError):
        return KISS_IN_DEFAULT / 100.0


def _yours(sim):
    from sims4vr import scripted
    return scripted._first_person(sim)


def _pose():
    """(x, z, yaw) of your body while you move freely, else None."""
    pose = native.walk_pose()
    if pose is not None and pose[5]:
        return pose[0], pose[2], pose[3]
    return None


def _yaw(transform):
    f = transform.orientation.transform_vector(sims4.math.FORWARD_AXIS)
    return math.atan2(f.x, f.z)


def _quat(yaw):
    return sims4.math.Quaternion(0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2))


def _fit(mine, other, pose):
    """(your slot, the partner's slot) moved as one piece so that yours is where your body is, facing where
    it faces; heights stay the jig's."""
    px, pz, yaw = pose
    turn = yaw - _yaw(mine)
    c, s = math.cos(turn), math.sin(turn)
    dx = other.translation.x - mine.translation.x
    dz = other.translation.z - mine.translation.z
    mine_new = sims4.math.Transform(sims4.math.Vector3(px, mine.translation.y, pz), _quat(yaw))
    other_new = sims4.math.Transform(
        sims4.math.Vector3(px + dx * c + dz * s, other.translation.y, pz + dz * c - dx * s),
        _quat(_yaw(other) + turn))
    return mine_new, other_new


def _picked_jig(cls, args, kwargs):
    """The jig is an object's (a bed, a dance floor): its slots are that object's."""
    picked = kwargs.get('picked_object', args[0] if args else None)
    try:
        return bool(cls._can_picked_object_be_jig(picked))
    except Exception:
        return False


def _anchored(item, pose, you_are_actor):
    actor, target = item[0], item[1]
    mine, other = (actor, target) if you_are_actor else (target, actor)
    if mine is None or other is None:
        return item
    if math.hypot(mine.translation.x - pose[0], mine.translation.z - pose[1]) > NEAR:
        return item
    mine_new, other_new = _fit(mine, other, pose)
    pair = (mine_new, other_new) if you_are_actor else (other_new, mine_new)
    return pair + tuple(item[2:])


def _wrap_jig(owner):
    raw = owner.__dict__.get('_get_jig_transforms_gen')
    if not isinstance(raw, classmethod):
        return False
    original = raw.__func__

    def _get_jig_transforms_gen(cls, initiating_sim, target_sim, *args, **kwargs):
        pose = None
        you_are_actor = None
        try:
            if _on('anchor'):
                if _yours(initiating_sim):
                    you_are_actor = True
                elif _yours(target_sim):
                    you_are_actor = False
                if you_are_actor is not None and not _picked_jig(cls, args, kwargs):
                    pose = _pose()
        except Exception as e:
            _error('reading your body for the jig', e)
            pose = None
        for item in original(cls, initiating_sim, target_sim, *args, **kwargs):
            if pose is not None:
                try:
                    item = _anchored(item, pose, you_are_actor)
                except Exception as e:
                    _error('fitting the jig to your body', e)
            yield item

    owner._get_jig_transforms_gen = classmethod(_get_jig_transforms_gen)
    return True


def _slot(group, sim):
    mapped = getattr(group, '_sim_routing_slot_map', {}).get(sim)
    if mapped:
        return mapped[0]
    slots = getattr(group, '_sim_transform_map', {}).get(sim)
    return slots[0][0] if slots else None


def _snap(interaction, asm):
    if not _on('snap'):
        return
    group = getattr(interaction, 'social_group', None)
    if group is None or not getattr(group, 'is_jig_group', False):
        return
    from interactions import ParticipantType
    sim = interaction.sim
    target = interaction.get_participant(ParticipantType.TargetSim)
    if sim is None or target is None or sim is target:
        return
    if _yours(sim):
        you, partner = sim, target
    elif _yours(target):
        you, partner = target, sim
    else:
        return
    pose = _pose()
    mine, theirs = _slot(group, you), _slot(group, partner)
    if pose is None or mine is None or theirs is None:
        return
    if math.hypot(mine.translation.x - pose[0], mine.translation.z - pose[1]) > NEAR:
        return
    _, goal = _fit(mine, theirs, pose)
    name = getattr(getattr(interaction, 'affordance', None), '__name__', '') or ''
    kiss = 'kiss' in name.lower() or 'kiss' in (getattr(asm, 'name', '') or '').lower()
    gx, gz = goal.translation.x, goal.translation.z
    if kiss:
        dx, dz = pose[0] - gx, pose[1] - gz
        far = math.hypot(dx, dz)
        step = min(_kiss_in(), max(0.0, far - 0.2))
        if far > 1e-3 and step > 0:
            gx += dx / far * step
            gz += dz / far * step
    p = partner.position
    moved = math.hypot(gx - p.x, gz - p.z)
    f = partner.forward
    off = abs(math.degrees((_yaw(goal) - math.atan2(f.x, f.z) + math.pi) % (2 * math.pi) - math.pi))
    _log('{} (asm {}): partner {:.2f} m from its slot, {:.0f} deg off{}'.format(
        name, getattr(asm, 'name', None), moved, off, ', kiss: steps in {:.0f} cm'.format(_kiss_in() * 100) if kiss else ''))
    if moved < SNAP_MIN and off < 3.0:
        return
    if moved > SNAP_MAX:
        return
    partner.move_to(translation=sims4.math.Vector3(gx, p.y, gz), orientation=goal.orientation,
                    routing_surface=partner.routing_surface)


def _wrap_asm():
    from interactions.social import SocialInteractionMixin

    def setup_asm_default(original, self, asm, *args, **kwargs):
        result = original(self, asm, *args, **kwargs)
        try:
            _snap(self, asm)
        except Exception as e:
            _error('snapping the partner', e)
        return result

    hooks.hook('pair', SocialInteractionMixin, 'setup_asm_default', setup_asm_default)


LEAVE_AFTER = 1.2  # s: the stick held this long ends the animation (the way out of the lock)
_pushed_since = None


def locked(sim, pushing):
    """walk.py, while you move freely: True while a jig social (hug, kiss, held hands) plays with you, so the
    stick doesn't walk you out of it and leave the partner posed in the air (user 2026-10-27 12:53Z). Your
    head stays free. Holding the stick LEAVE_AFTER s ends the animation (cancelling it on the wrist works too)."""
    global _pushed_since
    import time
    if not _on('lock'):
        return False
    from sims4vr import halt
    si = halt.social_running(sim)
    group = getattr(si, 'social_group', None) if si is not None else None
    if group is None or not getattr(group, 'is_jig_group', False):
        _pushed_since = None
        return False
    now = time.monotonic()
    if not pushing:
        _pushed_since = None
    elif _pushed_since is None:
        _pushed_since = now
    elif now - _pushed_since >= LEAVE_AFTER:
        _pushed_since = None
        try:
            from interactions.interaction_finisher import FinishingType
            si.cancel(FinishingType.USER_CANCEL, cancel_reason_msg='VR first person: you walked out of it')
            _log('{}: stick held, the animation ends'.format(si))
        except Exception as e:
            _error('ending the animation', e)
        return False
    return True


def anchoring():
    """scripted.py: a jig social with you is fitted to your body, so its partner keeps the game's own slot."""
    return _on('anchor')


def _install():
    from socials import jig_group
    done = [name for name, owner in (('JigGroup', jig_group.JigGroup),
                                     ('MultiSimPostureJigGroup', jig_group.MultiSimPostureJigGroup))
            if _wrap_jig(owner)]
    native._log('pair: jig slots fitted to your body ({})'.format(', '.join(done) or 'nothing found'))
    _wrap_asm()


try:
    _install()
except Exception as e:
    native._log('pair: unavailable: {!r}'.format(e))
