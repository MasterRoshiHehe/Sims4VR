"""First person: a hard cancel. What your Sim is doing stops at once.

User 2026-10-15 18:54Z: "a hard cancel of any interaction when playing first
person mode, I still keep getting bugs where my character starts running
around aimlessly and then gets stuck in a walking animation ... fast, smooth
and doesn't break anything". Built the way the game itself stops things
(decompiled 1.121 sources and the older full decompile, checked 2026-10-15):

- What walks your Sim can be another Sim's action. A social's transition is
  handed to every Sim it needs (interaction_queue.py:
  required_sim.queue.transition_controller = head.transition), so cancelling
  your own actions left a chat partner's walk running: the cancels "never
  processed". That transition is cancelled itself (TransitionSequence.cancel
  soft-stops its routes; the client gets RouteCancel) and so is the action
  that owns it, whoever's it is.
- Every action of yours, running or queued (mixers too), is cancelled with
  ignore_must_run and immediate: SuperInteraction._cancel_eventually holds a
  cancel back while a must-run posture change or a social mixer runs, unless
  immediate. Getting up from a seat or putting something down still plays its
  way out (the game's cancel replacement), so it stays smooth.
- A stopped route on the game's side waits for the client to say it is over
  (FollowPath: up to twice the route's length in time + 5 s), and the client
  never ends a route while the DLL holds your body (walkstop.h): the legs kept
  walking. native.walk_stop ends it on the client at once, as for our own
  walk routes; the client's route_complete then lets the server's route end
  where it was cancelled. Routes the game ran while it had your Sim get the
  same when you get control back (free_again), so no walk is left playing.
- Still being walked a second later, or still busy three seconds later (a
  stand-up plays in that time), the Sim gets the game's own reset (Sim.reset,
  RESET_EXPECTED, as the game's resetsim command and its stuck-route handling
  do): everything stops on both sides and the Sim stands where it is (moved
  only if that spot isn't valid; reset_and_delete_service, sim.py
  _validate_location). Never kill(): it closes the route on the server
  without telling the client (the walk animation kept going), and a Sim with
  a way out of its action gets reset by it anyway.
- What nobody may cancel is left alone (UNCANCELABLE_LIABILITY,
  CancelablePhase.NEVER): wetting itself, passing out (user rule 2026-10-12:
  the involuntary still happens), unless it is what walks you; so is a
  reaction playing where you are (scripted.py: reactions don't stop you).

v0.13.165 (user 2026-10-15 20:45Z: in a group conversation the Sim walked in
place and Stop everything left the actions in the game's queue):
- Socials with you are pinned where you are (scripted.py) and so counted as
  in-place reactions, which a hard cancel spares: Stop everything cancelled
  only an idle stand and left the conversation running. The debug menu's
  Stop everything (full) now ends every action and takes your Sim out of its
  social groups the game's way (SocialGroup.remove; the Sims in them get the
  lockout a cancelled social gives, SocialSuperInteraction._cancel), ends any
  route the client has on your Sim, and resets the Sim when anything is
  still in the game's action queue (UIManager) 3 s later: a reset clears it
  (Sim.on_reset_internal_state: ui_manager.remove_all_interactions). Pressed
  again while one is under way: the reset at once.
- A route the server had finished kept playing on the client (the DLL's walk
  stops found "another route is active" for minutes): the legs walked in
  place and blocked our own walk routes. In free movement a client route on
  your Sim that isn't ours (stride.py) and that the game isn't running is
  ended after STALE (native.client_route, walk_stop).

Route guard ([body] route_guard, debug menu Reach and game control): a route
the game gives your Sim that you didn't ask for (not picked by you, not the
next step of something you picked) and that goes further than LINE_UP is
stopped this way at once, and logged, so the cause shows in the log.

First person only (god view is untouched). Used by walk.py (pushing the stick
while the game moves or seats you), hudinfo.py (an action pressed on your
wrist's queue), hint.py, scripted.py (a conversation walking you over), and
the debug menu's "Stop everything" (controls.py).
"""
import math
import time

from sims4vr import native

ESCALATE_WALKING = 1.0  # s: still walked this long after a hard cancel: the game's reset
ESCALATE_BUSY = 3.0  # s: still busy this long (a way out like standing up plays first): the reset
GIVE_UP = 10.0  # s: stop watching (the reset was refused or didn't take)
LINE_UP = 1.0  # m: a route you didn't ask for may go this far (a step to line up)
STALE = 0.5  # s: in free movement a game route still on the client this long is ended
STALE_RETRY = 2.0  # s: and again this often while it stays

_pending = {}  # sim id -> [started, why, cancelled interactions, reset done, full, spared interaction ids]
_stale = None  # (route id, when it was first seen or last stopped) of a game route left on the client
_routes = {}  # sim id -> ids of the game's routes seen while it had your Sim
_guarded = set()  # ids of paths the route guard stopped (once each)
_logs = 0
_errors = 0


def _log(message, limit=200):
    global _logs
    _logs += 1
    if _logs <= limit:
        native._log('halt: ' + message)


def _error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('halt: {} failed: {!r}'.format(what, e))


def first_person(sim):
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def _finishing(name):
    from interactions.interaction_finisher import FinishingType
    return getattr(FinishingType, name, FinishingType.USER_CANCEL)


def _involuntary(si):
    """Nobody may cancel it (no cancel X in the game's queue either)."""
    try:
        from interactions.utils.interaction_liabilities import UNCANCELABLE_LIABILITY
        if si.get_liability(UNCANCELABLE_LIABILITY) is not None:
            return True
    except Exception:
        pass
    return getattr(getattr(si, '_cancelable_by_user', None), 'name', None) == 'NEVER'


def _doing(sim):
    """Everything on your Sim: running and queued (mixers too) and the action
    whose transition moves it (InteractionQueue.__iter__ leaves that out)."""
    items = list(sim.si_state) + list(sim.queue)
    controller = getattr(sim.queue, 'transition_controller', None)
    items.append(getattr(controller, 'interaction', None))
    seen = set()
    for si in items:
        if si is not None and id(si) not in seen:
            seen.add(id(si))
            yield si


def _routes_on(sim):
    """The game's routes (FollowPath) running on the Sim now."""
    found = []
    try:
        component = sim.routing_component
        get = getattr(component, '_get_active_follow_path', None)
        fp = get() if get is not None else None
        if fp is not None:
            found.append(fp)
        for primitive in tuple(getattr(sim, 'primitives', ()) or ()):
            if primitive is not fp and hasattr(primitive, 'route_finished') and hasattr(primitive, 'path'):
                found.append(primitive)
    except Exception as e:
        _error('reading the routes', e)
    return found


def _game_walking(sim):
    """The game walks the Sim right now (not a stopped route still waiting for the client)."""
    try:
        component = sim.routing_component
        return component.current_path is not None or component.is_moving
    except Exception:
        return False


def walked(sim):
    """True while the game walks the Sim."""
    try:
        component = sim.routing_component
        return component.current_path is not None or component.is_moving or bool(_routes_on(sim))
    except Exception:
        return False


def _end_routes_on_client(sim):
    """The client ends the game's routes on your Sim now (route_complete goes back)."""
    for fp in _routes_on(sim):
        try:
            native.walk_stop(sim.id, fp.id)
            _routes.setdefault(sim.id, set()).discard(fp.id)
        except Exception as e:
            _error('ending route {} on the client'.format(getattr(fp, 'id', '?')), e)


def _cancel(si, finishing, why, out):
    try:
        if si is None or si.is_finishing:
            return
        if not si.cancel(finishing, cancel_reason_msg=why, ignore_must_run=True, immediate=True):
            _log('{}: cancel not taken at once'.format(si))
        out.append(si)  # watched either way: still there later means the reset
    except Exception as e:
        _error('cancelling {}'.format(si), e)


def _stop_walk(sim, why, out):
    """Stop what walks the Sim: the transition it is in (maybe another Sim's)
    and the action that owns it, then the route on the client."""
    controller = getattr(sim.queue, 'transition_controller', None)
    if controller is not None:
        owner = getattr(controller, 'interaction', None)
        try:
            controller.cancel(_finishing('USER_CANCEL'), why, si_to_cancel=owner)
        except Exception as e:
            _error('cancelling the transition of {}'.format(owner), e)
        if owner is not None:
            mine = getattr(owner, 'sim', None) is sim
            _cancel(owner, _finishing('USER_CANCEL' if mine else 'SOCIALS'), why, out)
    _end_routes_on_client(sim)


def _watch(sim, why, cancelled, full=False, spared=()):
    entry = _pending.get(sim.id)
    if entry is None:
        _pending[sim.id] = [time.monotonic(), why, list(cancelled), False, full, set(spared)]
    else:
        entry[2].extend(si for si in cancelled if si not in entry[2])
        entry[4] = entry[4] or full
        entry[5].update(spared)


def _leave_groups(sim, why, cancelled):
    """Out of every social group, the game's way: SocialGroup.remove cancels
    your Sim's part in it; the other Sims' socials with you are cancelled
    too, and you and they get the autonomy lockout the game gives when you
    cancel a social yourself (SocialSuperInteraction._cancel), so they don't
    pull you straight back in."""
    from sims4vr import hint
    socials = _finishing('SOCIALS')
    for group in list(sim.get_groups_for_sim_gen()):
        try:
            others = [other for other in group if other is not sim]
            hint._lock_out(sim, others)
            for si in list(hint._theirs(sim, others)):
                _cancel(si, socials, why, cancelled)
            group.remove(sim)
            _log('{}: left {}'.format(why, group))
        except Exception as e:
            _error('leaving {}'.format(group), e)


def everything(sim, why, full=False):
    """Hard cancel: everything your Sim does stops (first person only).
    Returns what was cancelled. Again while one is under way: nothing new;
    full (the debug menu's Stop everything) then resets the Sim at once.

    full: conversations end too (socials with you sit where you are and
    otherwise count as reactions that play on), your Sim leaves its social
    groups and the client's route on it ends, whatever it is; anything still
    in the game's action queue 3 s later gets the reset."""
    if not first_person(sim):
        return []
    try:
        from sims4vr import trail
        trail.drop_walk_to(why)
    except Exception as e:
        _error('dropping a mixer walk', e)
    entry = _pending.get(sim.id)
    if entry is not None:
        if full and not entry[3]:
            entry[3] = True
            _reset(sim, why + ' (pressed again)')
        return []
    cancelled = []
    spared = []
    try:
        _stop_walk(sim, why, cancelled)
        from sims4vr import scripted
        posture = sim.posture
        keep = posture.source_interaction if posture is not None and posture.mobile else None
        in_place = not walked(sim)
        for si in list(_doing(sim)):
            if si in cancelled or getattr(si, 'is_finishing', True):
                continue
            if si is keep or _involuntary(si):
                spared.append(si.id)
                continue
            if in_place and not getattr(si, 'visible', True):
                continue  # the game's own idles (standing) while nothing walks you
            if in_place and scripted.is_emote(si) and not (full and scripted.is_social_with_you(si)):
                spared.append(si.id)
                continue  # a reaction where you are plays on (scripted.py: reactions don't stop you)
            _cancel(si, _finishing('USER_CANCEL'), why, cancelled)
        if full:
            _leave_groups(sim, why, cancelled)
            native.walk_stop(sim.id, 0)  # whatever route the client still plays on your Sim
    except Exception as e:
        _error('the hard cancel', e)
    _watch(sim, why, cancelled, full, spared)
    _log('{}: stopped {}'.format(why, ', '.join(str(si) for si in cancelled) or 'nothing running'))
    return cancelled


def social_running(sim):
    """walk.py: the social (or a mixer of one) the game is lining your Sim up
    for or playing, with another Sim; None otherwise."""
    try:
        from sims4vr import scripted
        controller = getattr(sim.queue, 'transition_controller', None)
        for si in (getattr(controller, 'interaction', None), sim.queue.running):
            if si is None or getattr(si, 'is_finishing', True):
                continue
            parent = si if getattr(si, 'is_super', False) else getattr(si, 'super_interaction', None)
            for candidate in (si, parent):
                if candidate is not None and scripted._is_social(candidate):
                    return si
    except Exception as e:
        _error('looking for a conversation', e)
    return None


def walking(sim, why):
    """Hard cancel of only what walks your Sim (the rest goes on)."""
    if not first_person(sim):
        return []
    cancelled = []
    try:
        _stop_walk(sim, why, cancelled)
    except Exception as e:
        _error('stopping the walk', e)
    _watch(sim, why, cancelled)
    _log('{}: stopped the walk ({})'.format(why, ', '.join(str(si) for si in cancelled) or 'route only'))
    return cancelled


def one(sim, interaction, why):
    """Hard cancel of one action (pressed on the wrist's queue). False when
    not first person (the caller cancels it the usual way)."""
    if not first_person(sim) or interaction is None:
        return False
    si = interaction
    if not getattr(si, 'is_super', True) and getattr(si, 'super_interaction', None) is not None:
        si = si.super_interaction
    controller = getattr(sim.queue, 'transition_controller', None)
    from sims4vr import trail
    held = trail.holding(sim) and si is trail.waiting(sim)['interaction']  # waits for you, walks nothing
    if not held and (si is getattr(controller, 'interaction', None) or (si is sim.queue.running and walked(sim))):
        everything(sim, why)  # what walks you: everything stops
        return True
    cancelled = []
    _cancel(si, _finishing('USER_CANCEL'), why, cancelled)
    if cancelled and si.running:
        _watch(sim, why, cancelled)
    _log('{}: stopped {}'.format(why, si))
    return True


def _queue_left(sim, entry):
    """What the game's action queue (UIManager) still shows, besides what was
    spared and the posture: a full stop isn't done while anything is there."""
    try:
        spared = entry[5]
        left = []
        for info in sim.ui_manager.get_interactions_gen():
            if info.interaction_id in spared or info.is_visual_type_posture():
                continue
            left.append(info)
        return left
    except Exception as e:
        _error('reading the action queue', e)
        return []


def _busy(sim, entry):
    if walked(sim) or getattr(sim.queue, 'transition_controller', None) is not None:
        return True
    try:
        si_state = sim.si_state
        running = sim.queue.running
        if any(si is running or si in si_state for si in entry[2]):
            return True
    except Exception:
        return False
    return bool(entry[4] and _queue_left(sim, entry))


def _reset(sim, why):
    try:
        entry = _pending.get(sim.id)
        left = _queue_left(sim, entry) if entry is not None else []
        from objects.object_enums import ResetReason
        sim.reset(ResetReason.RESET_EXPECTED, None, why)
        _log('{}: still busy, so the game reset the Sim (it stands where it is){}'.format(
            why, '; the queue still showed {}'.format(', '.join(str(i.display_name or i.interaction_id) for i in left))
            if left else ''))
    except Exception as e:
        _error('resetting the Sim', e)


def _sweep_client(sim):
    """Free movement: a route the client still plays on your Sim that isn't
    one of our walk routes and that the game isn't running (the server
    finished it, the client never did) is ended."""
    global _stale
    route = native.client_route(sim.id)
    # Only a walk the game is running now counts (a path, or moving). A
    # FollowPath the game stopped stays among the Sim's primitives until the
    # client says it finished (up to 2x its time + 5 s), and while it did,
    # walked() held this sweep off: that route sat on the client's queue, every
    # walk of ours queued behind it (their stops: "another route is active"),
    # the carried bowl and the next action's animations waited too, and then
    # it all played out at once: the Sim ran around through walls and did the
    # action where it ended up (user 2026-10-16 12:01Z, log 13:58-13:59).
    if not route or _game_walking(sim):
        _stale = None
        return
    from sims4vr import stride
    # One of ours that we no longer send (user 2026-10-16 11:24Z, walking
    # through walls again: its stop came while a game route was still on the
    # client, "another route is active", so it started after that one ended
    # and played to its end) is left over like a game route.
    if stride.ours(route) and stride.playing(route):
        _stale = None
        return
    now = time.monotonic()
    if _stale is None or _stale[0] != route:
        _stale = (route, now)
        return
    if now - _stale[1] < STALE:
        return
    native.walk_stop(sim.id, route)
    _stale = (route, now + STALE_RETRY - STALE)
    _log('a walk the game left playing on the client (route {:016x}) ended'.format(route), limit=400)


def update(sim, free=False):
    """walk.py, every tick in first person (free: you move, not the game):
    the reset when a hard cancel hasn't taken, the routes the game ran while
    it had the Sim, and a game route left playing on the client."""
    try:
        if walked(sim):
            seen = _routes.setdefault(sim.id, set())
            for fp in _routes_on(sim):
                seen.add(fp.id)
        if free:
            _sweep_client(sim)
        entry = _pending.get(sim.id)
        if entry is None:
            return
        age = time.monotonic() - entry[0]
        if not _busy(sim, entry):
            del _pending[sim.id]
            _log('{}: done in {:.1f} s'.format(entry[1], age))
            return
        if walked(sim):
            _end_routes_on_client(sim)
        if not entry[3] and (age >= ESCALATE_BUSY or (age >= ESCALATE_WALKING and walked(sim))):
            entry[3] = True
            _reset(sim, entry[1])
        elif age >= GIVE_UP:
            del _pending[sim.id]
            _log('{}: still busy after {:.0f} s; left to the game'.format(entry[1], age))
    except Exception as e:
        _error('watching the hard cancel', e)


def free_again(sim):
    """walk.py, when you get control back: the client ends the game's routes
    it ran on your Sim meanwhile, in case one is still playing its walk."""
    seen = _routes.pop(sim.id, None)
    for route_id in seen or ():
        try:
            native.walk_stop(sim.id, route_id)
        except Exception as e:
            _error('ending route {} on the client'.format(route_id), e)
    if seen:
        _log('free again: ended {} game route(s) on the client'.format(len(seen)), limit=400)


def _picked(interaction):
    """You asked for it: picked from the pie menu, or the next step of an
    action you picked (cooking, then putting the plate down)."""
    try:
        from interactions.context import InteractionSource
        context = interaction.context
        if context.source in (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT):
            return True
        return getattr(context, 'continuation_id', None) is not None
    except Exception:
        return False


def guard(sim, interaction, you_at=None):
    """walk.py, while the game has your Sim in first person: a route you
    didn't ask for that goes further than LINE_UP is stopped at once."""
    try:
        if not first_person(sim) or not native.setting_flag('body', 'route_guard', True):
            return
        path = sim.routing_component.current_path
        if path is None or id(path) in _guarded or sim.id in _pending:
            return
        if interaction is not None and _picked(interaction):
            return
        end = path.final_location.transform.translation
        here = sim.position
        far = math.hypot(end.x - here.x, end.z - here.z)
        if you_at is not None:
            far = max(far, math.hypot(end.x - you_at[0], end.z - you_at[1]))
        try:
            far = max(far, path.length())
        except Exception:
            pass
        if far <= LINE_UP:
            return
        _guarded.add(id(path))
        if len(_guarded) > 200:
            _guarded.clear()
            _guarded.add(id(path))
        walking(sim, 'route guard: {} would walk your Sim {:.1f} m (not asked for)'.format(interaction, far))
    except Exception as e:
        _error('the route guard', e)


def forget():
    global _stale
    _stale = None
    _pending.clear()
    _routes.clear()
    _guarded.clear()
