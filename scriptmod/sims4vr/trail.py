"""First person: the guiding trail. You walk to what you picked yourself.

User 2026-10-15 21:27Z: "The guided pathing system in the new roadmap sounds
great! Build it" (docs/GAMEPLAY-PLAN.md, stage C; Hogwarts Legacy style
trail instead of the game walking your Sim).

Step C1 (this build): hold the action instead of walking or cancelling it.
You pick something far away; the game plans its route as usual (the posture
graph's TransitionSpec.get_transition_route -> routing.get_route_element_for_path
-> do_route, which would make the FollowPath that walks the Sim). For your
Sim in first person, do_route doesn't walk: the action stays in the game's
queue, in its transition, waiting for you, and the path the game planned is
kept (its nodes and the spot where the action starts) for the trail.

How the game itself does it (decompiled 1.121):
- do_route is looked up as a module global by route_gen at call time
  (interactions/utils/routing.py get_route_element_for_path), so wrapping it
  covers every posture transition route; Go Here and the like make their
  FollowPath elsewhere and are untouched.
- The game's own route callback (posture_graph.py get_transition_route)
  stops a route by derailing the transition and returning
  FollowPath.Action.CANCEL, so do_route returns False and the transition is
  planned again from where the Sim is (TransitionSequence.derail,
  reset_derailed_transitions). The hold ends the same way when you leave
  first person, so the game carries on its usual way in god view.
- A cancel (the queue's X, the wrist queue, Stop everything: halt.py cancels
  the transition) shows on the transition (canceled) or the interaction
  (is_finishing); the hold then returns False like a cancelled route.

Not held: a walk shorter than LINE_UP (the line-up step), route failures
(the game's failure balloon plays as before), socials (the other Sim comes to
you, scripted.py), ladders and spiral stairs (carried.py), and anything you
didn't pick (halt.guard stops those).

Switch: [body] trail in sims4vr.ini (1 = on). First person only.
Step C2 (v0.13.167, user 2026-10-16 09:56Z after C1 passed; footprints, user
2026-10-15 21:28Z): the held route goes to the DLL as floor points
(_send_trail), and trail.cpp draws it as glowing footprints ending in a pair
where the action starts.

Step C3 (v0.13.170, user 2026-10-16 10:43Z): within ARRIVE of the spot (and on
its level) the Sim is put exactly where you stand and the transition is
derailed (CONSTRAINTS_CHANGED), the way the game's own route callback stops a
route: the interaction queue runs the transition again (run_transitions ->
reset_derailed_transitions), the posture graph plans from where you are, that
walk is a line-up under LINE_UP so it isn't held, and the action plays. No
facing check: at a seat the spot faces away from it, while you face the seat.

Step C4 (v0.13.171, user 2026-10-16 10:58Z): the spot taken while you walk
(an NPC sits in your chair). The game's walk calls the transition's
route_callback (posture_graph.py TransitionSpec.get_transition_route) once the
Sim is within FollowPath.DISTANCE_TO_RECHECK_INUSE (5 m) of the end: it checks
the stand slot, then do_reservation reserves the spot (the seat, the bed
side), and if that fails (RESERVATION failure reason) it returns CANCEL and
do_route returns False. The hold calls that same callback, the same way, with
the distance you still have to go: it reserves the spot for you at 5 m as the
game does (so an NPC can't take it any more), and if it is already taken the
hold returns False like the game's walk; the transition derails
(TRANSITION_FAILED adds that spot to _tried_destinations), so the game picks
its next valid spot (a new trail) or ends the action with its own reason.
Walking off the trail is covered since v0.13.168 (planned again every 1 m).
Not called for actions the game fades out or that lock socials (travel):
those side effects belong to the walk itself.

Step C6 (v0.13.177, user 2026-10-16 12:19Z): a far pick on a Sim that isn't
a social (socials still bring the other Sim to you, v0.13.122) is no longer
a call over (reach.py): the game plans the walk to them and it waits here
like any other. When they move more than FOLLOW_MOVE from where the trail was
planned, the transition is derailed (CONSTRAINTS_CHANGED, as for leaving
first person) so the game plans again to where they are now, from where you
stand, and a new trail is drawn: it follows them.
"""
import math
import time

import sims4.math
import services

from sims4vr import native

LINE_UP = 1.0  # m: a walk this short is the game's line-up step and plays as usual
TICK = 0.25  # s (game time at normal speed) between checks while an action waits
SAMPLE = 1.0  # m between floor samples along the trail
STEP_RISE = 0.25  # m: one stair step (the game's straight stairs: rise = tread, 16 steps per 4 m level)
REPLAN_MOVE = 1.0  # m you move before the game plans the trail again from where you are
REPLAN_EVERY = 1.0  # s at least between plans
ARRIVE = 0.5  # m from the spot where the action starts at which it carries on (C3)
ARRIVE_HEIGHT = 1.0  # m: and on its level
ARRIVE_CONTACT = 0.2  # m: for a social with hand contact (scripted.contact): the game's own
                      # line-up from further was cut short and left you a metre off (v0.13.218)
ARRIVE_PUSH_MAX = 5.0  # s: the stick pushed from walking onto the spot is ignored this long at most
FOLLOW_MOVE = 1.0  # m the Sim you walk to moves before the game plans the way to them again (C6)
RECHECK = 5.0  # m: FollowPath.DISTANCE_TO_RECHECK_INUSE (its tuned value is read when there)
MAX_POINTS = 256  # trail.cpp kMaxPoints

_holds = {}  # sim id -> dict(interaction, goal, level, nodes, started)
_arrived_at = None  # when you last walked onto a spot (the stick may still be pushed)
_logs = 0
_errors = 0
_original = None
_walk_to = None  # a social mixer waiting for you to walk to its Sim (walk_to)
MIXER_NEAR = 1.5  # m: unused since v0.13.197: the mixer plays within scripted.talk_distance()
MIXER_TEST_EVERY = 1.0  # s: the waiting mixer pick is tested again this often
MIXER_GROUP_GRACE = 2.0  # s: no conversation with them left after this: the walk ends (and its footprints)
MIXER_WAIT = 120.0  # s a walk to a mixer's Sim waits at most
STALE = 2.0  # s: footprints nothing has updated this long (game running) are taken away (_watch)
_shown = False  # footprints are drawn
_watcher = None  # the real-time alarm of _watch
_owner = type('_Owner', (), {})()


def _log(message, limit=200):
    global _logs
    _logs += 1
    if _logs <= limit:
        native._log('trail: ' + message)


def _error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('trail: {} failed: {!r}'.format(what, e))


def _on():
    return native.setting_flag('body', 'trail', True)


def holding(sim):
    """walk.py: an action of yours waits for you to walk there (you move freely)."""
    return sim is not None and getattr(sim, 'id', None) in _holds


def waiting(sim):
    """The waiting action's hold (dict) or None."""
    return _holds.get(getattr(sim, 'id', None))


def _nodes(path):
    out = []
    try:
        for node in path.nodes:
            p = node.position
            surface = node.routing_surface_id
            out.append((p[0], p[1], p[2], getattr(surface, 'secondary_id', 0)))
    except Exception as e:
        _error('reading the path', e)
    return out


def _floor(x, z, surface, fallback):
    try:
        import terrain
        return terrain.get_terrain_height(x, z, routing_surface=surface)
    except Exception:
        return fallback


def _stair_points(px, pz, x, z, y0, y1):
    """A stretch between levels: a point at the front and back of every step
    it crosses, at that step's tread height (stairs.py flights), in walking
    order; [] if it crosses no flight. The flight's ramp (stairs.py) runs from
    the foot of the bottom riser to the top, under the treads: on it the
    footprints sank into the steps (user 2026-10-16 10:14Z)."""
    try:
        from sims4vr import stairs
        found = []
        lo, hi = min(y0, y1), max(y0, y1)
        for f in stairs.flights():
            if f.h0 < lo - 0.6 or f.h1 > hi + 0.6:
                continue
            sa, la = f.coords(px, pz)
            sb, lb = f.coords(x, z)
            if max(abs(la), abs(lb)) > f.half_w + 0.5 or abs(sb - sa) < 1e-3:
                continue
            if max(sa, sb) < 0 or min(sa, sb) > f.run:
                continue
            steps = max(1, int(round((f.h1 - f.h0) / STEP_RISE)))
            tread = f.run / steps
            rise = (f.h1 - f.h0) / steps
            for k in range(steps):
                height = f.h0 + (k + 1) * rise
                for along in (k * tread + 0.02, (k + 1) * tread - 0.02):
                    u = (along - sa) / (sb - sa)
                    if 0.0 < u < 1.0:
                        found.append((u, px + (x - px) * u, height, pz + (z - pz) * u))
        found.sort()
        return [(fx, fy, fz) for _, fx, fy, fz in found]
    except Exception as e:
        _error('reading the stairs', e)
        return []


def _send_trail(path, nodes):
    """C2: the route to the DLL as floor points (trail.cpp draws the footprints).
    Path nodes are where the route turns; a stretch on one level is filled in
    every SAMPLE m with the floor's height there (slopes, steps outside), a
    stretch between levels (stairs) gets the height of each step it crosses."""
    import struct
    try:
        if len(nodes) < 2:
            _clear()
            return 0
        surfaces = []
        for node in path.nodes:
            surfaces.append(node.routing_surface_id)
        points = []
        for i, (x, y, z, level) in enumerate(nodes):
            surface = surfaces[i] if i < len(surfaces) else None
            if i > 0:
                px, py, pz, plevel = nodes[i - 1]
                if plevel == level and surface is not None:
                    d = math.hypot(x - px, z - pz)
                    steps = int(d / SAMPLE)
                    for k in range(1, steps + 1):
                        t = k / (steps + 1)
                        sx, sz = px + (x - px) * t, pz + (z - pz) * t
                        points.append((sx, _floor(sx, sz, surface, py + (y - py) * t), sz))
                elif plevel != level:
                    points.extend(_stair_points(px, pz, x, z, py, y))
            points.append((x, _floor(x, z, surface, y) if surface is not None else y, z))
            if len(points) >= MAX_POINTS - 1:
                break
        end = path.final_location
        goal = end.transform.translation
        facing = end.transform.orientation.transform_vector(sims4.math.Vector3.Z_AXIS())
        gy = _floor(goal.x, goal.z, end.routing_surface, goal.y)
        packed = struct.pack('<5f', goal.x, gy, goal.z, facing.x, facing.z)
        packed += b''.join(struct.pack('<3f', *p) for p in points)
        native.trail_set(packed)
        _mark_shown()
        return len(points)
    except Exception as e:
        _error('sending the trail', e)
        return 0


def _clear():
    global _shown
    _shown = False
    native.trail_set(b'')


def _mark_shown():
    global _shown, _watcher
    _shown = True
    if _watcher is None:
        import alarms
        import clock
        _watcher = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(0.5), _watch, repeating=True)


def _watch(_handle):
    """Footprints never outlive what they lead to (user 2026-10-17 02:35Z: a
    hand massage on a Sim walking to the pool never happened, the queue
    emptied when she jumped in, and the trail to the pool edge stayed). What
    draws them (a hold, a mixer walk) clears them when it ends; this catches
    the case where it ended without getting to (its action finished, or the
    game dropped its timeline element): nothing has updated them for STALE s
    while the game runs, or the held action is finishing."""
    global _watcher, _walk_to
    try:
        if not _shown:
            alarm, _watcher = _watcher, None
            if alarm is not None:
                import alarms
                alarms.cancel_alarm(alarm)
            return
        import clock
        clock_service = services.game_clock_service()
        paused = clock_service is None or clock_service.clock_speed == clock.ClockSpeedMode.PAUSED
        now = time.monotonic()
        alive = False
        for sim_id, hold in list(_holds.items()):
            interaction = hold['interaction']
            if getattr(interaction, 'is_finishing', False) or \
                    (not paused and now - hold.get('seen', hold['started']) > STALE):
                del _holds[sim_id]
                _log('{}: footprints left behind (the action {}): taken away'.format(
                    interaction, 'ended' if getattr(interaction, 'is_finishing', False) else 'stopped waiting'))
            else:
                alive = True
        w = _walk_to
        if w is not None:
            if paused or now - w.get('seen', w['started']) <= STALE:
                alive = True
            else:
                _walk_to = None
                _log('{} on {}: mixer walk stopped waiting: footprints taken away'.format(
                    getattr(w['aop'].affordance, '__name__', '?'), w['target']))
        if not alive:
            _clear()
    except Exception as e:
        _error('checking the footprints', e)


def _should_hold(agent, path, interaction):
    """The reason to hold (text) or None: walk as the game would."""
    if interaction is None or not getattr(agent, 'is_sim', False) or not _on():
        return None
    from sims4vr import halt
    if not halt.first_person(agent) or not halt._picked(interaction):
        return None
    try:
        from sims4vr import carried
        if carried.active():
            return None
    except Exception:
        pass
    try:
        from sims4vr import scripted
        if scripted.you_walk(interaction):
            pass  # you walk to them, they wait (scripted._you_walk)
        elif scripted._is_social(interaction) or scripted.is_social_with_you(interaction):
            return None
    except Exception:
        pass
    import routing
    if path is None or path.status != routing.Path.PLANSTATUS_READY or path.is_route_fail():
        return None
    try:
        length = path.length()
    except Exception:
        length = 0.0
    end = path.final_location.transform.translation
    here = agent.position
    straight = math.hypot(end.x - here.x, end.z - here.z)
    # Hand contact: even the short line-up is yours to walk (the game's own was
    # cut short on the client and left you a metre off, v0.13.218).
    if max(length, straight) <= (ARRIVE_CONTACT if getattr(interaction, '_sims4vr_touch', None) else LINE_UP):
        return None
    return '{:.1f} m'.format(max(length, straight))


def _you(sim):
    """(x, y, z) of your body (free movement), else the Sim's."""
    pose = native.walk_pose()
    if pose is not None and pose[5]:
        return pose[0], pose[1], pose[2]
    p = sim.position
    return p.x, p.y, p.z


def _where(obj):
    """(x, z) of a Sim, or None."""
    try:
        p = obj.position
        return p.x, p.z
    except Exception:
        return None


def _length(path):
    try:
        return path.length()
    except Exception:
        return 0.0


def _replan_gen(timeline, sim, goal_location, interaction, start):
    """The game's route from where you are now to the same spot (PlanRoute,
    as routing_elements.py and the routing.debug.follow command plan one), or
    None. Only drawn: nothing is reserved and nothing walks."""
    import routing
    from element_utils import run_child
    from interactions.utils.routing import PlanRoute
    x, y, z = start
    here = routing.Location(sims4.math.Vector3(x, y, z), sim.orientation, sim.routing_surface)
    route = routing.Route(here, (routing.Goal(goal_location),), routing_context=sim.routing_context)
    plan = PlanRoute(route, sim, reserve_final_location=False, interaction=interaction)
    result = yield from run_child(timeline, plan)
    nodes = plan.path.nodes if result else None
    if not (nodes and nodes.plan_success) or plan.path.is_route_fail():
        return None
    return plan.path


def arrival_push(pushing):
    """walk.py: True while the stick is still pushed from walking onto the
    spot (user 2026-10-16 11:24Z: at the counter the action just stopped; the
    log showed the stick's hard cancel right after "you arrived": you were
    still walking when the game took over). Ends once you let go, or after
    ARRIVE_PUSH_MAX; pushing again after that cancels as usual."""
    global _arrived_at
    if _arrived_at is None:
        return False
    if not pushing or time.monotonic() - _arrived_at > ARRIVE_PUSH_MAX:
        _arrived_at = None
        return False
    return True


def _arrive(sim):
    """The simulation's Sim goes exactly where you stand before the game plans
    from there (walk.py teleports it only now and then while you walk)."""
    try:
        pose = native.walk_pose()
        if pose is not None and pose[5]:
            from sims4vr import walk
            walk._sync(sim, pose, False, force=True)
    except Exception as e:
        _error('putting the Sim where you are', e)


def _recheck_distance():
    try:
        from interactions.utils.routing import FollowPath
        return float(FollowPath.DISTANCE_TO_RECHECK_INUSE)
    except Exception:
        return RECHECK


def _may_check(interaction):
    """C4: the route callback may run while you walk (not for walks the game
    fades out or that lock socials: travel)."""
    try:
        if interaction.should_fade_sim_out():
            return False
        import sims.self_interactions
        if isinstance(interaction, sims.self_interactions.TravelInteraction):
            return False
    except Exception:
        return False
    return True


def _hold_gen(timeline, sim, path, interaction, why, callback_fn=None):
    global _arrived_at
    import clock
    import elements
    from element_utils import run_child
    from postures import DerailReason
    end = path.final_location
    goal = end.transform.translation
    level = getattr(end.routing_surface, 'secondary_id', 0)
    hold = dict(interaction=interaction, goal=(goal.x, goal.y, goal.z), level=level,
                nodes=_nodes(path), started=time.monotonic())
    drop_walk_to('another action walks you')
    _holds[sim.id] = hold
    drawn = _send_trail(path, hold['nodes'])
    _log('waiting {} goal {:.2f},{:.2f},{:.2f} level {} ({} away, {} nodes, trail of {} points)'.format(
        interaction, goal.x, goal.y, goal.z, level, why, len(hold['nodes']), drawn))
    outcome = 'ended'
    goal_location = end
    drawn_from = _you(sim)
    planned_at = time.monotonic()
    replans = 0
    planned_length = _length(path)
    recheck = _recheck_distance()
    check = callback_fn if callback_fn is not None and _may_check(interaction) else None
    other = getattr(interaction, 'target', None)
    if not getattr(other, 'is_sim', False) or other is sim:
        other = None
    other_at = _where(other)
    try:
        while True:
            hold['seen'] = time.monotonic()
            controller = sim.transition_controller
            if controller is None or getattr(controller, 'canceled', False) or interaction.is_finishing:
                outcome = 'cancelled'
                return False
            if controller.is_derailed(sim):
                outcome = 'derailed by the game'
                return False
            from sims4vr import halt
            if not halt.first_person(sim) or not _on():
                # Out of first person (or switched off): the game plans the walk again its own way.
                controller.derail(DerailReason.CONSTRAINTS_CHANGED, sim)
                outcome = 'handed back to the game'
                return False
            # C3: you reached the spot: the game plans again from where you stand
            # (the line-up there is under LINE_UP, so it plays) and the action goes on.
            you = _you(sim)
            gx, gy, gz = hold['goal']
            arrive = ARRIVE_CONTACT if getattr(interaction, '_sims4vr_touch', None) else ARRIVE
            if math.hypot(you[0] - gx, you[2] - gz) <= arrive and abs(you[1] - gy) <= ARRIVE_HEIGHT:
                _arrived_at = time.monotonic()
                _arrive(sim)
                controller.derail(DerailReason.CONSTRAINTS_CHANGED, sim)
                outcome = 'you arrived: carried on'
                return False
            # C6: the Sim you walk to has moved: the game plans the way to them again.
            if other_at is not None and time.monotonic() - planned_at > REPLAN_EVERY:
                now_at = _where(other)
                if now_at is not None and math.hypot(now_at[0] - other_at[0], now_at[1] - other_at[1]) > FOLLOW_MOVE:
                    controller.derail(DerailReason.CONSTRAINTS_CHANGED, sim)
                    outcome = '{} moved: planned again to them'.format(other)
                    return False
            # C4: within the game's recheck distance, its own route callback
            # reserves the spot, or says it is taken (the game then picks again).
            if check is not None:
                moved = math.hypot(you[0] - drawn_from[0], you[2] - drawn_from[2])
                left = max(math.hypot(you[0] - gx, you[2] - gz), planned_length - moved)
                if left < recheck:
                    from interactions.utils.routing import FollowPath
                    if check(left) == FollowPath.Action.CANCEL:
                        outcome = 'spot taken {:.1f} m away: the game picks again'.format(left)
                        return False
            # The trail starts where you are (user 2026-10-16 10:15Z: it stayed where
            # the action was picked): walked off by REPLAN_MOVE, the game plans it again.
            you = _you(sim)
            if math.hypot(you[0] - drawn_from[0], you[2] - drawn_from[2]) > REPLAN_MOVE and \
                    time.monotonic() - planned_at > REPLAN_EVERY:
                planned_at = time.monotonic()
                new_path = None
                try:
                    new_path = yield from _replan_gen(timeline, sim, goal_location, interaction, you)
                except Exception as e:
                    _error('planning the trail again', e)
                if new_path is not None:
                    drawn_from = you
                    planned_length = _length(new_path)
                    replans += 1
                    points = _send_trail(new_path, _nodes(new_path))
                    if replans <= 5 or replans % 20 == 0:
                        _log('trail planned again from {:.1f},{:.1f} ({} points; {} so far)'.format(
                            you[0], you[2], points, replans))
                if interaction.is_finishing:
                    continue
            slept = yield from run_child(timeline, elements.SoftSleepElement(clock.interval_in_real_seconds(TICK)))
            if not slept:
                outcome = 'stopped by the game'  # soft stopped: never wait in a loop that can't sleep
                return False
    finally:
        if _holds.get(sim.id) is hold:
            del _holds[sim.id]
            _clear()  # the trail goes with the hold
        _log('{} {} after {:.0f} s'.format(outcome, interaction, time.monotonic() - hold['started']))


def _do_route(timeline, agent, path, *args, **kwargs):
    why = None
    try:
        why = _should_hold(agent, path, kwargs.get('interaction'))
    except Exception as e:
        _error('the hold check', e)
    if why is None:
        result = yield from _original(timeline, agent, path, *args, **kwargs)
        return result
    result = yield from _hold_gen(timeline, agent, path, kwargs.get('interaction'), why,
                                  kwargs.get('callback_fn'))
    return result


def _wrap_do_route():
    global _original
    import interactions.utils.routing as game_routing
    original = getattr(game_routing, 'do_route', None)
    if original is None:
        native._log('trail: routing.do_route not found')
        return
    if getattr(original, '_sims4vr_trail', False):
        return
    _original = original
    _do_route._sims4vr_trail = True
    game_routing.do_route = _do_route


def walk_to(sim, target, aop, context, front=False):
    """reach.py: a social mixer picked with a Sim further away (user
    2026-10-16 15:18Z: the buzzer prank still played from afar). A mixer runs
    inside a conversation you are already in, and the game's social group
    lets that span the room, so no walk is planned and there is nothing to
    hold: the pick waits here instead. The footprints lead to them (planned
    by the game from where you are, again as you or they move), and the
    mixer is picked for you once you're within MIXER_NEAR."""
    global _walk_to
    import elements
    token = object()
    try:
        grouped = any(target in group for group in sim.get_groups_for_sim_gen())
    except Exception:
        grouped = False
    _walk_to = dict(sim=sim, target=target, aop=aop, context=context, token=token, started=time.monotonic(),
                    grouped=grouped, tested=0.0, failing=None, front=front)
    services.time_service().sim_timeline.schedule(
        elements.GeneratorElement(lambda timeline: _walk_to_gen(timeline, token)))
    _log('{} on {}: footprints to them; it plays when you get there'.format(
        getattr(aop.affordance, '__name__', '?'), target))


def drop_walk_to(why):
    """A new pick or a hard stop ends a waiting mixer walk."""
    global _walk_to
    if _walk_to is not None:
        _log('{} on {}: dropped ({})'.format(
            getattr(_walk_to['aop'].affordance, '__name__', '?'), _walk_to['target'], why))
        _walk_to = None
        if not _holds:
            _clear()


def _walk_to_gen(timeline, token):
    global _walk_to
    import clock
    import elements
    import routing
    from element_utils import run_child
    from sims4vr import halt
    outcome = 'ended'
    drawn_from = None
    drawn_to = None
    planned_at = 0.0
    try:
        while _walk_to is not None and _walk_to['token'] is token:
            w = _walk_to
            w['seen'] = time.monotonic()
            sim, target = w['sim'], w['target']
            if not halt.first_person(sim) or not _on():
                outcome = 'out of first person'
                return
            if time.monotonic() - w['started'] > MIXER_WAIT:
                outcome = 'waited {:.0f} s'.format(MIXER_WAIT)
                return
            there = _where(target)
            if there is None or getattr(target, 'is_sim', False) is False:
                outcome = 'they are gone'
                return
            # v0.13.197 (user 2026-10-17 02:35Z: asked for a hand massage while
            # she walked to the pool; she jumped in, the queue emptied, the
            # footprints stayed): a mixer needs the conversation; when you
            # and they are no longer in one, the walk ends.
            # v0.13.202 (user 2026-10-17 03:53Z: talking from a distance didn't
            # work): a mixer picked with no conversation yet starts one when it
            # plays (the game pushes the chat first), so the group check above
            # only holds when you were already talking; otherwise the game's
            # own test of the pick decides (she jumped in the pool: it fails).
            now = time.monotonic()
            if w['grouped'] and now - w['started'] > MIXER_GROUP_GRACE and \
                    not any(target in group for group in sim.get_groups_for_sim_gen()):
                outcome = 'the conversation with them ended'
                return
            if now - w['tested'] > MIXER_TEST_EVERY:
                w['tested'] = now
                result = w['aop'].test(w['context'])
                if result:
                    w['failing'] = None
                elif w['failing'] is None:
                    w['failing'] = now
                elif now - w['failing'] > MIXER_GROUP_GRACE:
                    outcome = 'the game no longer allows it: {}'.format(getattr(result, 'reason', result))
                    return
            you = _you(sim)
            far = math.hypot(there[0] - you[0], there[1] - you[2])
            from sims4vr import scripted
            if w['front']:
                # Hand contact (user 2026-10-17 05:42Z): right in front of them
                # (scripted.contact); the trail leads there and follows them.
                there = scripted.front_of(target)
                far = math.hypot(there[0] - you[0], there[1] - you[2])
                arrived = scripted.in_front(sim, target)  # and facing them
            else:
                arrived = far <= scripted.talk_distance()
            if arrived:
                _walk_to = None
                _clear()
                result = w['aop'].test_and_execute(w['context'])
                outcome = 'you got there ({:.1f} m): {}'.format(
                    far, 'picked' if result else 'refused by the game: {}'.format(result))
                return
            now = time.monotonic()
            if (drawn_from is None or math.hypot(you[0] - drawn_from[0], you[2] - drawn_from[2]) > REPLAN_MOVE
                    or math.hypot(there[0] - drawn_to[0], there[1] - drawn_to[1]) > FOLLOW_MOVE) and \
                    now - planned_at > REPLAN_EVERY and not _holds:
                planned_at = now
                if w['front']:
                    gx, gz = there  # the spot in front of them
                else:
                    # A spot 1 m short of them on your side, on their floor.
                    k = min(1.0, far) / far
                    gx, gz = there[0] + (you[0] - there[0]) * k, there[1] + (you[2] - there[1]) * k
                tp = target.position
                facing = target.orientation
                if w['front']:  # the footprints end facing them
                    yaw = math.atan2(tp.x - gx, tp.z - gz)
                    facing = sims4.math.Quaternion(0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2))
                goal = routing.Location(sims4.math.Vector3(gx, tp.y, gz), facing, target.routing_surface)
                path = None
                try:
                    path = yield from _replan_gen(timeline, sim, goal, None, you)
                except Exception as e:
                    _error('planning the footprints to {}'.format(target), e)
                if _walk_to is not w:
                    return
                if path is not None:
                    drawn_from, drawn_to = you, there
                    _send_trail(path, _nodes(path))
            slept = yield from run_child(timeline, elements.SoftSleepElement(clock.interval_in_real_seconds(TICK)))
            if not slept:
                outcome = 'stopped by the game'
                return
        outcome = 'replaced'
    except Exception as e:
        _error('walking to a Sim', e)
    finally:
        if _walk_to is not None and _walk_to['token'] is token:
            _walk_to = None
            if not _holds:
                _clear()
        _log('mixer walk: {}'.format(outcome))


def forget():
    _holds.clear()


try:
    _wrap_do_route()
except Exception as e:
    native._log('trail: wrapping do_route failed: {!r}'.format(e))
