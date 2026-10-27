"""Facing base layer (user 2026-10-27 02:16Z): in an interaction the Sims face
each other. Features build on this module instead of turning Sims themselves
(scripted.py placement, hint.py conversations, trail.py line-up, the paired
animations).

What it does
- NPC side: a Sim in a conversation with you, or one a feature asked for
  (want), is turned to face you where it stands, the game's way: a social
  adjustment into a small circle around itself with a Facing constraint
  (the same as SocialGroup, hint._turn and follow._come). Not while it is
  seated or otherwise held in place, walking to you, carried, or in the
  middle of something that is not about you (never cancelled for a turn).
  Closer than 0.8 m counts too (hint._turn skipped that: hugs and kisses
  start close). Not during a touching social either (scripted.contact): the
  pair is placed for it already, and a turn would cancel it.
- Their part of a social that starts where you both stand is planned facing
  you (here), not just "where they are": that is what left them talking with
  their back to you.
- Your side: your headset is never turned. pair_gate says whether you and
  they face each other close enough for an action that needs it (hug, kiss,
  hand holding), and trail.py ends its footprints facing the Sim you walk to.

API (all take Sim objects; positions are x, y, z in the world)
  enabled()                  [body] face_you
  off_by(sim, x, z)          degrees between where sim looks and (x, z); None when closer than 5 cm
  faces(sim, x, z, deg=None) True when within deg (default [body] face_angle)
  you_at(you)                where you are (your body, not the simulation's Sim)
  turn(other, you_xyz, why)  turn it now if needed; True = sent
  want(other, seconds, why)  keep turning it to you for that long (update does it)
  here(other, you_xyz)       constraint: where it stands, facing you (scripted.py)
  pair_gate(you, other, reach=1.0, deg=35.0) -> (ok, why, dist, you_off, them_off)
  group(sim, group, others, px, py, pz, now), update(sim, px, py, pz, now): called by hint.update
Settings: [body] face_you (on), face_angle (degrees, 45): debug menu Options > Game settings > Other Sims.
"""
import math
import time

from sims4vr import native

ANGLE = 45.0  # degrees: facing further away than this, they turn (default of [body] face_angle)
EVERY = 1.0  # s between two turns of the same Sim
SPOT = 0.3  # m: radius of the circle they turn in
HALF = math.radians(20)  # the Facing constraint's half angle: they end up this squarely at you

_last = {}  # Sim id -> when it was last turned
_wanted = {}  # Sim id -> [until, why]
_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 80:
        native._log('facing: ' + message)


def _error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('facing: {} failed: {!r}'.format(what, e))


def enabled():
    return native.setting_flag('body', 'face_you', True)


def angle():
    try:
        return max(10.0, float(native.setting('body', 'face_angle', str(ANGLE))))
    except (TypeError, ValueError):
        return ANGLE


def you_at(you):
    from sims4vr import trail
    return trail._you(you)


def off_by(sim, x, z):
    p = sim.position
    dx, dz = x - p.x, z - p.z
    far = math.hypot(dx, dz)
    if far < 0.05:
        return None
    f = sim.forward
    cos = (f.x * dx + f.z * dz) / ((math.hypot(f.x, f.z) or 1.0) * far)
    return math.degrees(math.acos(max(-1.0, min(1.0, cos))))


def faces(sim, x, z, deg=None):
    off = off_by(sim, x, z)
    return off is None or off <= (deg if deg is not None else angle())


def _can_turn(other, you):
    """Free to be turned: standing, not carried, not walking over, and doing nothing or only things with you."""
    from sims4vr import hint, scripted
    if getattr(getattr(other, 'parent', None), 'is_sim', False) or hint._set_in_place(other):
        return False
    if other.id in hint._this_check or hint._adjusting(other):
        return False
    if other.queue.has_adjustment_interaction():
        return False
    if getattr(other.queue, 'transition_controller', None) is not None:
        return False
    running = other.queue.running
    if running is None:
        return True
    if scripted.contact(getattr(running, 'affordance', type(running))):
        return False
    group = getattr(running, 'social_group', None)
    if group is not None:
        try:
            return you in group
        except Exception:
            return False
    return getattr(running, 'target', None) is you or getattr(running, 'sim', None) is you


def turn(other, you_xyz, why=''):
    """Turn that Sim to face you where it stands, if it needs it. True when a turn went out."""
    now = time.monotonic()
    if now - _last.get(other.id, 0.0) < EVERY:
        return False
    x, y, z = you_xyz
    p = other.position
    if abs(p.y - y) >= 2.0:
        return False
    off = off_by(other, x, z)
    if off is None or off <= angle():
        return False
    from sims4vr import follow
    you = follow._fp_sim()
    if you is None or not _can_turn(other, you):
        return False
    import sims4.math
    from interactions.constraints import Circle, Facing
    from interactions.priority import Priority
    from socials.group import SocialGroup
    _last[other.id] = now
    spot = Circle(p, SPOT, other.routing_surface).intersect(
        Facing(target_position=sims4.math.Vector3(x, p.y, z), facing_range=HALF))
    result = other.execute_adjustment_interaction(SocialGroup.SOCIAL_ADJUSTMENT_AFFORDANCE, spot, Priority.High,
                                                  name_override='Satisfy[vr_face]')
    try:
        for group in you.get_groups_for_sim_gen():
            if other in group and result:
                group._adjustment_interactions.add(result.execute_result.interaction)
    except Exception:
        pass
    _log('{} turns to you ({:.0f} deg away{}){}'.format(
        follow._name(other), off, ', ' + why if why else '',
        '' if result else ' refused: {}'.format(getattr(result, 'reason', result))))
    return bool(result)


def want(other, seconds=10.0, why=''):
    """Keep that Sim facing you for a while (a feature that needs it: a paired animation)."""
    _wanted[other.id] = [time.monotonic() + seconds, why]


def here(other, you_xyz):
    """scripted.py: where that Sim must be for a social that starts where you both stand: where it
    stands, facing you. The plain place (scripted._here) when it cannot face you or facing is off."""
    from sims4vr import scripted
    base = scripted._here(other)
    if not enabled() or you_xyz is None:
        return base
    try:
        import sims4.math
        from interactions.constraints import Circle, Facing
        p = other.position
        spot = Circle(p, scripted.HERE, other.routing_surface).intersect(
            Facing(target_position=sims4.math.Vector3(you_xyz[0], p.y, you_xyz[2]), facing_range=HALF))
        constraint = other.posture_state.body_posture_state_constraint
        if constraint is not None:
            spot = spot.intersect(constraint)
        if spot.valid:
            return spot
    except Exception as e:
        _error('placing {} facing you'.format(other), e)
    return base


def pair_gate(you, other, reach=1.0, deg=35.0):
    """For an action that needs the two of you face to face and within reach (a hug, a kiss, held
    hands): (ok, why, distance, how far off you face them, how far off they face you). Nothing is
    turned here; the caller decides to wait, turn them (want) or refuse."""
    x, y, z = you_at(you)
    p = other.position
    far = math.hypot(p.x - x, p.z - z)
    you_off = None
    pose = native.walk_pose()
    if pose is not None and pose[5] and far > 0.05:
        yaw = pose[3]
        cos = (math.sin(yaw) * (p.x - x) + math.cos(yaw) * (p.z - z)) / far
        you_off = math.degrees(math.acos(max(-1.0, min(1.0, cos))))
    them_off = off_by(other, x, z)
    if far > reach:
        return False, 'too far ({:.2f} m)'.format(far), far, you_off, them_off
    if you_off is not None and you_off > deg:
        return False, 'you face {:.0f} deg away'.format(you_off), far, you_off, them_off
    if them_off is not None and them_off > deg:
        return False, 'they face {:.0f} deg away'.format(them_off), far, you_off, them_off
    return True, 'ok', far, you_off, them_off


def group(sim, grp, others, px, py, pz, now):
    """hint.update, for each conversation you are in."""
    if not enabled():
        return
    for other in others:
        try:
            turn(other, (px, py, pz), 'conversation')
        except Exception as e:
            _error('turning {}'.format(other), e)


def update(sim, px, py, pz, now):
    """hint.update, every check: the Sims a feature asked for."""
    if not _wanted or not enabled():
        return
    import services
    manager = services.object_manager()
    for sim_id, (until, why) in list(_wanted.items()):
        if now > until:
            del _wanted[sim_id]
            continue
        other = manager.get(sim_id)
        if other is None:
            del _wanted[sim_id]
            continue
        try:
            turn(other, (px, py, pz), why)
        except Exception as e:
            _error('turning {}'.format(other), e)
