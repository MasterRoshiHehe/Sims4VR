"""First person: you only use what you can reach.

Your Sim never walks itself to things in first person. A pie menu pick on an
object farther than REACH from you (measured to the object's nearest edge, its
footprint) is not queued: the menu still lists everything, a far pick simply
does nothing. Within reach the game may still take its short step to line up
(sitting down, standing at the stove).

A social you pick on a Sim farther away goes through: the game plans it for
both of you with your Sim held where it stands (scripted.py), so the other Sim
walks over the game's own way (v0.13.122). Other picks on a far Sim call them
over: they are sent (Go Here) to a spot in front of you, and your pick runs
once they are within reach. If they haven't come after WAIT seconds it is dropped.

A computer (a desktop, not a laptop) is used only while you sit in front of
it: seated, facing it, its nearest edge within reach.

Pie menus show what you can't use greyed out, with a note saying why ("Too
far away", "Sit at the computer to use it"); Sims you can call over stay as
they are. ChoiceMenu.is_valid_aop gives each option its test result while the
menu is built (looked up on the class at call time by add_aop).

With the guiding trail on ([body] trail, trail.py, 2026-10-15), a far pick on
an object goes through: the game plans its walk, your Sim doesn't take it, and
the action waits in the queue for you to walk there yourself.

Always in reach: your own Sim, what you hold or carry, your inventory.
A ladder or stairs is reached from its end on your level (_portal_ends).
From afar: lights and watching a TV (remote.py: the TV remote in your hand where you are).
Switch: [body] reach_only in sims4vr.ini (debug menu: Moving page).

ChoiceMenu.select runs every pick from a pie menu (interactions.select via
Client.select_interaction); it is looked up on the class at call time.
"""
import math
import time

import alarms
import clock
import services
import sims4.math

from sims4vr import hooks, native

REACH = 1.0  # metres from you to the nearest edge of what you use
SIM_RADIUS = 0.25  # metres: a Sim's edge from its centre
CALLED_SPOT = 0.9  # metres in front of you where a called Sim walks to
WAIT = 20.0  # seconds a called Sim has to come
TICK_SECONDS = 0.25
MIXER_FAR = 2.0  # m: a social mixer picked with a Sim further than this waits for you to walk there

_logs = 0
_owner = type('_Owner', (), {})()
_alarm = None
_pending = None  # a Sim called over: dict(sim, target, aop, context, goal, started, pushed)
_go_here = None


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 40:
        native._log('reach: ' + message)


def _on():
    return native.setting_flag('body', 'reach_only', True)


def _yours(sim):
    """The Sim you are in, in first person."""
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def _seated(sim):
    """Your Sim sits (a seat's posture: not mobile)."""
    try:
        posture = sim.posture
        return posture is not None and not posture.mobile and 'sit' in str(getattr(posture, 'posture_type', posture)).lower()
    except Exception:
        return False


def _is_computer(obj):
    """A desktop computer (laptops go anywhere with you)."""
    if obj is None or getattr(obj, 'is_sim', False):
        return False
    obj = getattr(obj, 'part_owner', None) or obj
    names = ' '.join((type(obj).__name__, getattr(getattr(obj, 'definition', None), 'name', '') or '')).lower()
    return 'computer' in names and 'laptop' not in names


def _facing(sim, target):
    """Your Sim faces target (within about 60 degrees)."""
    try:
        f, p, t = sim.forward, sim.position, target.position
        dx, dz = t.x - p.x, t.z - p.z
        length = math.hypot(dx, dz)
        return length < 0.05 or (f.x * dx + f.z * dz) / length > 0.5
    except Exception:
        return True


def _you(sim):
    """Where you are: the player's body on the client (the simulation's Sim lags while you walk);
    the Sim itself while it sits (the walking body stays where you stood)."""
    if _seated(sim):
        p = sim.position
        return p.x, p.y, p.z
    pose = native.walk_pose()
    if pose is not None and all(math.isfinite(v) for v in pose[:3]):
        return pose[0], pose[1], pose[2]
    p = sim.position
    return p.x, p.y, p.z


def _carried_by(obj, sim):
    for _ in range(8):
        if obj is None:
            return False
        if obj is sim:
            return True
        obj = getattr(obj, 'parent', None)
    return False


def _vertices(polygon):
    """Outlines of a Polygon or a CompoundPolygon, as lists of (x, z)."""
    outlines, points = [], []
    for item in polygon:
        if hasattr(item, 'x'):
            points.append((item.x, item.z))
        else:
            outlines.extend(_vertices(item))
    if len(points) >= 2:
        outlines.append(points)
    return outlines


def _segment_distance(px, pz, ax, az, bx, bz):
    dx, dz = bx - ax, bz - az
    length = dx * dx + dz * dz
    t = 0.0 if length < 1e-9 else max(0.0, min(1.0, ((px - ax) * dx + (pz - az) * dz) / length))
    cx, cz = ax + dx * t, az + dz * t
    return math.hypot(px - cx, pz - cz)


def _edge_distance(x, z, obj):
    """Metres from (x, z) to obj's footprint (0 inside it), or None without one."""
    polygon = getattr(obj, 'footprint_polygon', None)
    if polygon is None:
        return None
    try:
        if polygon.contains(sims4.math.Vector2(x, z)):
            return 0.0
    except Exception:
        pass
    best = None
    for outline in _vertices(polygon):
        for i in range(len(outline)):
            a, b = outline[i], outline[(i + 1) % len(outline)]
            d = _segment_distance(x, z, a[0], a[1], b[0], b[1])
            best = d if best is None else min(best, d)
    return best


def _portal_ends(obj):
    """(x, z, level) of each end of the ways through obj (a ladder, stairs):
    such an object stands on its bottom level, so it is reached from the end
    on your level (v0.13.121, user 2026-10-14 23:20Z: stuck at the top of a
    ladder, its climb down was too far away)."""
    get = getattr(obj, 'get_portal_instances', None)
    if get is None:
        return []
    ends = []
    try:
        for portal in get():
            for end in (portal.there_entry, portal.there_exit, portal.back_entry, portal.back_exit):
                if end is None:
                    continue
                p = end.position
                ends.append((p.x, p.z, getattr(end.routing_surface, 'secondary_id', None)))
    except Exception as e:
        _log('reading the ends of {} failed: {!r}'.format(obj, e))
    return ends


def _distance(sim, target):
    """Metres from you to target's nearest edge; None: always in reach."""
    if target is None or target is sim or _carried_by(target, sim):
        return None
    in_inventory = getattr(target, 'is_in_inventory', None)
    if callable(in_inventory) and in_inventory():
        return None
    x, _, z = _you(sim)
    ends = _portal_ends(target)
    if ends:
        # A way between levels: its nearest end on your level (or its footprint, on its own level).
        level = getattr(sim, 'level', None)
        near = [math.hypot(ex - x, ez - z) for ex, ez, end_level in ends if end_level == level]
        if getattr(target, 'level', level) == level:
            foot = _edge_distance(x, z, target)
            if foot is not None:
                near.append(foot)
        return min(near) if near else float('inf')
    try:
        if getattr(target, 'level', sim.level) != sim.level:
            return float('inf')  # another floor
    except Exception:
        pass
    if getattr(target, 'is_sim', False):
        p = target.position
        return max(0.0, math.hypot(p.x - x, p.z - z) - SIM_RADIUS)
    d = _edge_distance(x, z, target)
    if d is None:
        p = getattr(target, 'position', None)
        if p is None:
            return None
        d = math.hypot(p.x - x, p.z - z)
    return d


FAR = 'Too far away'
SIT = 'Sit at the computer to use it'


def _verdict(sim, aop, quiet=False):
    """None: usable; 'call': a Sim to call over; else why not (FAR or SIT), with the distance."""
    target = aop.target
    d = _distance(sim, target)
    name = getattr(aop.affordance, '__name__', '?')
    try:
        from sims4vr import follow
        if follow.is_ours(aop.affordance):
            return None, d  # Follow me / Dismiss: from wherever you are
    except Exception:
        pass
    if d is not None and _is_computer(target):
        try:
            from sims4vr import claude
            if claude._affordance is not None and aop.affordance is claude._affordance:
                # Open Claude works standing (user 2026-10-15 01:56Z), as near as it stays open.
                return (None, d) if d <= claude.CLOSE_DISTANCE else (FAR, d)
        except Exception as e:
            _log('Open Claude reach check failed: {!r}'.format(e))
        if _seated(sim) and _facing(sim, target) and d <= REACH:
            return None, d
        return SIT, d
    if getattr(target, 'is_sim', False) and target is not sim and d is not None and d != float('inf') and \
            native.setting_flag('body', 'trail', True):
        from sims4vr import scripted
        if scripted.contact(aop.affordance, getattr(aop, 'super_affordance', None)) and \
                not _carried_by(target, sim) and not scripted.in_front(sim, target):
            # Hand contact (kiss, hug, holding hands, WickedWhims starts): dead in
            # front of them first, facing them, also when close (user 2026-10-17 12:03Z).
            return 'walk_to', d
    if d is None or d <= REACH:
        return None, d
    try:
        from sims4vr import remote
        if remote.from_afar(aop.affordance, target):
            if not quiet:
                _log('{} on {} {:.1f} m away: works from where you are'.format(name, target, d))
            return None, d
    except Exception as e:
        _log('far-use check failed: {!r}'.format(e))
    if getattr(target, 'is_sim', False) and d != float('inf'):
        if _social_pick(aop.affordance):
            from sims4vr import scripted
            if not getattr(aop.affordance, 'is_super', True) and native.setting_flag('body', 'trail', True):
                if d > scripted.talk_distance():
                    return 'walk_to', d  # a mixer: you walk to them first (trail.walk_to)
            return None, d  # the game brings them to you (scripted.py pins your Sim)
        if native.setting_flag('body', 'trail', True):
            # C6 (user 2026-10-16 12:19Z): anything else on a Sim gets a trail to
            # them that follows them as they move (trail.py), not a call over.
            if not quiet:
                _log('{} on {} {:.1f} m away: you walk to them yourself (trail.py)'.format(name, target, d))
            return None, d
        return 'call', d
    if native.setting_flag('body', 'trail', True):
        if not quiet:
            _log('{} on {} {:.1f} m away: you walk there yourself (trail.py)'.format(name, target, d))
        return None, d  # the game plans the walk and the action waits for you (trail.py)
    return FAR, d


def _social_pick(affordance):
    """A social (a chat, a flirt, any of their mixers): planned for both Sims
    together, so with your Sim held where it stands (scripted._pin) the game
    walks the other Sim over itself (v0.13.122, user 2026-10-14 23:44Z)."""
    if not native.setting_flag('body', 'socials_in_place', True):
        return False
    try:
        if getattr(affordance, 'is_social', False):
            return True
        from interactions.social.social_super_interaction import SocialSuperInteraction
        from interactions.social.social_mixer_interaction import SocialMixerInteraction
        return isinstance(affordance, type) and issubclass(affordance, (SocialSuperInteraction, SocialMixerInteraction))
    except Exception:
        return False


def _check(menu, choice_id):
    """None to let the pick through, or the result to give instead."""
    selection = menu.menu_items.get(choice_id)
    if selection is None:
        return None
    context = selection.context
    sim = getattr(context, 'sim', None)
    if not _yours(sim) or not _on():
        return None
    aop = selection.aop
    target = aop.target
    from sims4vr import trail
    trail.drop_walk_to('you picked something else')
    why, d = _verdict(sim, aop)
    if why is None:
        return None
    name = getattr(aop.affordance, '__name__', '?')
    from event_testing.results import EnqueueResult
    if why == 'walk_to':
        from sims4vr import scripted
        touch = scripted.contact(aop.affordance, getattr(aop, 'super_affordance', None))
        _log('{} on {} {:.1f} m away: you walk to {} first'.format(
            name, target, d, 'right in front of them (hand contact: {})'.format(touch) if touch else 'them'))
        trail.walk_to(sim, target, aop, context, front=bool(touch))
        return EnqueueResult.NONE
    if why == 'call':
        _call_over(sim, target, aop, context)
        _log('{} on {} {:.1f} m away: calling them over'.format(name, target, d))
        return EnqueueResult.NONE
    _log('{} on {} is {:.1f} m away: {}, not queued'.format(name, target, d, why.lower()))
    return EnqueueResult.NONE


def _greyed(why):
    from event_testing.results import TestResult
    from sims4.localization import LocalizationHelperTuning
    return TestResult(False, 'VR: ' + why, tooltip=lambda *_, **__: LocalizationHelperTuning.get_raw_text(why))


def _wrap_menu():
    """Options you can't use show greyed out with why, instead of doing nothing."""
    from interactions.choices import ChoiceMenu
    counts = {}

    def is_valid_aop(original, aop, context, *args, **kwargs):
        result = original(aop, context, *args, **kwargs)
        try:
            if not result or not native.setting_flag('body', 'grey_far', True) or not _on():
                return result
            sim = getattr(context, 'sim', None)
            if not _yours(sim):
                return result
            why, d = _verdict(sim, aop, quiet=True)
            if why is None or why in ('call', 'walk_to'):
                return result
            key = (getattr(aop.target, 'id', None), why)
            if counts.get(key, 0) < 1 and len(counts) < 40:
                counts[key] = 1
                _log('{} {:.1f} m away: options greyed out ({})'.format(aop.target, d, why))
            return _greyed(why)
        except Exception as e:
            _log('grey check failed: {!r}'.format(e))
            return result

    hooks.hook('reach', ChoiceMenu, 'is_valid_aop', is_valid_aop, first_person_only=True)


def _wrap_select():
    from interactions.choices import ChoiceMenu

    def select(original, self, choice_id, *args, **kwargs):
        try:
            result = _check(self, choice_id)
        except Exception as e:
            _log('check failed: {!r}'.format(e))
            result = None
        if result is not None:
            return result
        return original(self, choice_id, *args, **kwargs)

    hooks.hook('reach', ChoiceMenu, 'select', select, first_person_only=True)


# ---------------------------------------------------------------------------
# Calling a Sim over


def _go_here_affordance():
    global _go_here
    if _go_here is None:
        from objects.terrain import GoHereSuperInteraction
        import sims4.resources
        manager = services.get_instance_manager(sims4.resources.Types.INTERACTION)
        found = [t for t in manager.types.values()
                 if isinstance(t, type) and issubclass(t, GoHereSuperInteraction)]
        exact = [t for t in found if t.__name__ == 'terrain-GoHere']
        plain = [t for t in found if not any(w in t.__name__.lower() for w in ('toddler', 'swim', 'ocean', 'pet', 'pool'))]
        _go_here = (exact or plain or found or [False])[0]
        _log('Go Here is {}'.format(getattr(_go_here, '__name__', 'missing')))
    return _go_here or None


def _spot(sim, target):
    """A spot CALLED_SPOT in front of you, toward the Sim you called."""
    x, y, z = _you(sim)
    p = target.position
    dx, dz = p.x - x, p.z - z
    length = math.hypot(dx, dz)
    if length < 1e-3:
        return x, y, z
    return x + dx / length * CALLED_SPOT, y, z + dz / length * CALLED_SPOT


def _send(target, goal, surface):
    affordance = _go_here_affordance()
    if affordance is None:
        return False
    from interactions.context import InteractionContext, QueueInsertStrategy
    from interactions.priority import Priority
    from objects.terrain import TerrainPoint
    location = sims4.math.Location(sims4.math.Transform(sims4.math.Vector3(*goal)), surface)
    context = InteractionContext(target, InteractionContext.SOURCE_SCRIPT, Priority.High,
                                 insert_strategy=QueueInsertStrategy.NEXT)
    result = target.push_super_affordance(affordance, TerrainPoint(location), context)
    if not result:
        _log('{} could not be called over: {}'.format(target, result))
    return bool(result)


def _call_over(sim, target, aop, context):
    global _pending, _alarm
    goal = _spot(sim, target)
    now = time.monotonic()
    _pending = dict(sim=sim, target=target, aop=aop, context=context, goal=goal, started=now, pushed=now)
    if not _send(target, goal, sim.routing_surface):
        _pending = None
        return
    if _alarm is None:
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)


def _stop_ticking():
    global _alarm
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)


def _tick(_handle):
    global _pending
    try:
        p = _pending
        if p is None:
            _stop_ticking()
            return
        sim, target = p['sim'], p['target']
        now = time.monotonic()
        if not _yours(sim) or getattr(target, 'is_sim', False) is False or target.position is None:
            _pending = None
            return
        d = _distance(sim, target)
        if d is not None and d <= REACH + 0.2:
            _pending = None
            result = p['aop'].test_and_execute(p['context'])
            _log('{} came over: {} {}'.format(target, getattr(p['aop'].affordance, '__name__', '?'),
                                              'queued' if result else 'failed: {}'.format(result)))
            return
        if now - p['started'] > WAIT:
            _pending = None
            _log('{} did not come over in {:.0f} s; pick dropped'.format(target, WAIT))
            return
        # You walked on: send them to where you are now.
        goal = _spot(sim, target)
        if math.hypot(goal[0] - p['goal'][0], goal[2] - p['goal'][2]) > 1.0 and now - p['pushed'] > 2.0:
            p['goal'], p['pushed'] = goal, now
            _send(target, goal, sim.routing_surface)
    except Exception as e:
        _pending = None
        _log('calling over failed: {!r}'.format(e))


_wrap_select()
_wrap_menu()
