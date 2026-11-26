"""First person: you watch a TV by looking at it.

Looking: while a TV is on and its screen is where your head points (within
[tv] look_angle degrees of straight ahead, plus the screen's own size), with
no wall between you, and you stand still or sit, your Sim runs the game's own
watch interaction for the channel the TV is on, right where it is (no walking
to a seat, no sitting down; remote._stay_put). That gives everything the
channel gives: fun, its moodlets, skill, whims. Looking away for longer than
[tv] look_away seconds, walking off or the TV going off ends it; the TV stays
on (the game turns it off when its last watcher stops: not here).

Nearby: a TV that is on in the same room, or within [tv] nearby metres on
your level, gives a small fun trickle while you are not watching it:
[tv] passive_share of what watching gave the last time it was measured (a
watch of at least MEASURE_MINUTES game minutes), at first DEFAULT_WATCH_RATE.
No moodlets, no skill. Game time: nothing while paused, faster at speed 3.

Engine ([tv] engine, default on; docs/PASSIVE-ACTIONS.md): no watch
interaction at all. Looking gives what the channel's watch interaction gives
(passive_gain.py reads its tuning: fun, skill, moodlet commodities, stressed
moodlets fading faster, its loot), at full strength; a TV on you only hear
gives the same rates at [tv] passive_share x how well you hear it
(passive_gain.hearing: its volume, the distance against [tv] nearby, walls and
floors between; no skill unless [passive] nearby_skill).
Nothing is queued, no remote, no seat. Off = the old way above.

Only your Sim, only in first person (body.py starts and stops it); other
Sims and god view keep the game's way. Switch: [tv] gaze in sims4vr.ini.
"""
import math
import time

import alarms
import clock
import services

from sims4vr import native

TICK_SECONDS = 0.25
SCAN_EVERY = 10.0  # seconds between looks for TVs on the lot (new or moved ones)
SIGHT_EVERY = 0.5  # seconds a wall check between your head and a screen is reused
FULL_RANGE = 10.0  # metres: farther than this, looking doesn't count
SCREEN_CLEARANCE = 0.45  # metres in front of the screen the wall check aims at (past the TV's own stand)
MEASURE_MINUTES = 15.0  # game minutes of watching before its fun rate is learned
DEFAULT_WATCH_RATE = 0.7  # fun per game minute while watching, until measured
FUN_NAMES = ('motive_fun',)

_owner = type('_Owner', (), {})()
_alarm = None
_logs = 0
_errors = 0
_tvs = []  # TV objects on the lot
_tv_info = {}  # definition id -> (channel state, off value) or None
_screens = {}  # object id -> (time, position key, (centre, normal, width, height))
_sight = {}  # object id -> (time, clear)
_scanned = 0.0
_watch = None  # _Watch: the watch interaction we run
_looked_at = None  # (tv, time) the TV you last looked at
_fun_type = None  # the fun need's statistic type (False: not found)
_watch_rate = None  # learned fun per game minute while watching
_passive_at = None  # game minute of the last passive trickle
_passive_tv = None  # the TV the trickle comes from (for the log)
_settings = None


class _Watch:
    __slots__ = ('interaction', 'tv', 'channel', 'since', 'minute', 'fun')

    def __init__(self, interaction, tv, channel, minute, fun):
        self.interaction, self.tv, self.channel = interaction, tv, channel
        self.since = time.monotonic()
        self.minute, self.fun = minute, fun


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 200:
        native._log('tvwatch: ' + message)


def _read_settings():
    def number(key, default):
        try:
            return float(native.setting('tv', key, default))
        except (TypeError, ValueError):
            return default
    return {
        'on': native.setting_flag('tv', 'gaze', True),
        'angle': number('look_angle', 20.0),
        'grace': number('look_away', 3.0),
        'nearby': number('nearby', 10.0),
        'share': number('passive_share', 0.3),
        'engine': native.setting_flag('tv', 'engine', True),
    }


def _first_person(sim):
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


# --- life cycle ---------------------------------------------------------------

def start():
    global _alarm, _settings, _scanned, _passive_at
    _settings = _read_settings()
    _scanned = 0.0
    _passive_at = None
    if _alarm is None and _settings['on']:
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
        _log('on (look within {:.0f} degrees, {:.0f} s to look away, nearby {:.1f} m, passive {:.0f}%)'.format(
            _settings['angle'], _settings['grace'], _settings['nearby'], _settings['share'] * 100))


def stop():
    global _alarm, _passive_at
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    _end_watch('first person ended')
    _engine_stop('first person ended')
    _tvs[:] = []
    _screens.clear()
    _sight.clear()
    _passive_at = None


# --- TVs ------------------------------------------------------------------------

def _info(tv):
    """(channel state, off value) of a TV, from its watch interactions."""
    key = getattr(getattr(tv, 'definition', None), 'id', None) or id(type(tv))
    if key not in _tv_info:
        found = None
        try:
            from objects.electronics.television import WatchSuperInteraction
            for affordance in tv.super_affordances():
                if isinstance(affordance, type) and issubclass(affordance, WatchSuperInteraction):
                    channel = getattr(affordance, 'required_channel', None)
                    off = getattr(affordance, 'off_channel', None)
                    if channel is not None and off is not None:
                        found = (channel.state, off)
                        break
        except Exception as e:
            _log('reading {} failed: {!r}'.format(tv, e))
        _tv_info[key] = found
    return _tv_info[key]


def _channel(tv):
    """The channel value the TV is on (it has a watch interaction), or None if off."""
    info = _info(tv)
    if info is None:
        return None
    try:
        value = tv.get_state(info[0])
    except Exception:
        return None
    if value is None or value is info[1] or getattr(value, 'affordance', None) is None:
        return None
    return value


@native.timed('tv scan', 0.01)
def _scan():
    from sims4vr import remote
    found = []
    for obj in list(services.object_manager().values()):
        try:
            if not getattr(obj, 'is_sim', False) and remote._is_tv(obj) and _info(obj) is not None:
                found.append(obj)
        except Exception:
            pass
    if [t.id for t in found] != [t.id for t in _tvs]:
        _log('{} TV(s) on the lot'.format(len(found)))
    _tvs[:] = found


def _screen(tv, now):
    """(centre, normal, width, height) of the TV's screen in the world."""
    p = tv.position
    key = (round(p.x, 2), round(p.y, 2), round(p.z, 2), getattr(tv.orientation, 'y', 0.0))
    cached = _screens.get(tv.id)
    if cached is not None and cached[1] == key:
        return cached[2]
    from sims4vr import objmenu
    centre, normal, _up, width, height, _video = objmenu._front(tv, True)
    screen = (centre, normal, width, height)
    _screens[tv.id] = (now, key, screen)
    return screen


def _clear_sight(sim, tv, head, screen, now):
    """No wall between your head and the screen (the navmesh, on your level)."""
    cached = _sight.get(tv.id)
    if cached is not None and now - cached[0] < SIGHT_EVERY:
        return cached[1]
    clear = True
    try:
        import routing
        import sims4.math
        centre = screen[0]
        surface = sim.routing_surface
        y = sim.position.y
        origin = routing.Location(sims4.math.Vector3(head[0], y, head[2]), sims4.math.Quaternion.ZERO(), surface)
        clear = False
        for normal in (screen[1], _object_forward(tv)):  # the screen mesh's normal can point into a wall-mounted screen's wall
            if normal is None:
                continue
            aim = (centre[0] + normal[0] * SCREEN_CLEARANCE, centre[2] + normal[2] * SCREEN_CLEARANCE)
            end = routing.Location(sims4.math.Vector3(aim[0], y, aim[1]), sims4.math.Quaternion.ZERO(), surface)
            if routing.ray_test(origin, end, sim.routing_context):
                clear = True
                break
    except Exception as e:
        _log('wall check failed: {!r}'.format(e))
    _sight[tv.id] = (now, clear)
    return clear


_why_said = {}  # TV id -> (reason, time) last logged why looking at it did not count


def _why(tv, reason):
    """Logs (on a change, at most every 5 s per TV) why looking at a TV that is on does not count."""
    now = time.monotonic()
    said = _why_said.get(tv.id)
    if said is not None and (said[0] == reason or now - said[1] < 5.0):
        return
    _why_said[tv.id] = (reason, now)
    _log('{}: not watching: {}'.format(tv, reason))


def _object_forward(obj):
    """The object's own front (its local +Z) in the world, or None."""
    try:
        from sims4vr import objmenu
        o = obj.transform.orientation
        return objmenu._rotate((o.x, o.y, o.z, o.w), (0.0, 0.0, 1.0))
    except Exception:
        return None


def _looking_at(sim, snap, now):
    """The TV that is on and that you look at, or None."""
    head, forward = snap[0], snap[1]
    if not all(math.isfinite(v) for v in head) or not all(math.isfinite(v) for v in forward):
        return None
    flen = math.sqrt(sum(v * v for v in forward)) or 1.0
    best = None
    for tv in _tvs:
        try:
            if _channel(tv) is None:
                continue
            if tv.level != sim.level:
                _why(tv, 'on another level')
                continue
            screen = _screen(tv, now)
            centre, normal = screen[0], screen[1]
            d = [centre[i] - head[i] for i in range(3)]
            dist = math.sqrt(sum(v * v for v in d))
            if dist < 0.1 or dist > FULL_RANGE:
                continue
            forward = _object_forward(tv)
            if sum(normal[i] * d[i] for i in range(3)) > 0 and (forward is None or sum(forward[i] * d[i] for i in range(3)) > 0):
                _why(tv, 'behind the screen')
                continue  # behind the screen (its mesh normal and the object's own front both say so)
            cos = sum(forward[i] * d[i] for i in range(3)) / (flen * dist)
            angle = math.degrees(math.acos(max(-1.0, min(1.0, cos))))
            allowed = _settings['angle'] + math.degrees(math.atan(0.5 * max(screen[2], screen[3]) / dist))
            if angle > allowed:
                if angle < 45:
                    _why(tv, 'looking {:.0f} degrees off (allowed {:.0f})'.format(angle, allowed))
                continue
            if best is not None and angle >= best[0]:
                continue
            if _clear_sight(sim, tv, head, screen, now):
                best = (angle, tv)
            else:
                _why(tv, 'a wall in the way (navmesh)')
        except Exception as e:
            _note_error('looking at {}'.format(tv), e)
    return best[1] if best else None


def _nearby(sim):
    """A TV that is on in your room or within [tv] nearby metres on your level."""
    p = sim.position
    reach = _settings['nearby']
    room = None
    for tv in _tvs:
        try:
            if tv.level != sim.level or _channel(tv) is None:
                continue
            t = tv.position
            if math.hypot(t.x - p.x, t.z - p.z) <= reach:
                return tv
            if room is None:
                room = _room(p, sim.level)
            if room and _room(t, tv.level) == room:
                return tv
        except Exception as e:
            _note_error('nearby {}'.format(tv), e)
    return None


def _room(position, level):
    try:
        import build_buy
        return build_buy.get_room_id(services.current_zone_id(), position, level) or 0
    except Exception:
        return 0


# --- fun --------------------------------------------------------------------------

def _fun():
    global _fun_type
    if _fun_type is None:
        _fun_type = False
        try:
            import sims4.resources
            manager = services.get_instance_manager(sims4.resources.Types.STATISTIC)
            for t in manager.types.values():
                if getattr(t, '__name__', '').lower() in FUN_NAMES:
                    _fun_type = t
                    break
        except Exception as e:
            _log('the fun need not found: {!r}'.format(e))
        _log('fun need: {}'.format(getattr(_fun_type, '__name__', 'not found')))
    return _fun_type or None


def _fun_value(sim):
    fun = _fun()
    if fun is None:
        return None
    try:
        return sim.commodity_tracker.get_value(fun)
    except Exception:
        return None


def _minutes():
    return services.time_service().sim_now.absolute_minutes()


def _trickle(sim, tv):
    """Passive watching: a share of the watching fun rate, per game minute."""
    global _passive_at, _passive_tv
    now = _minutes()
    if tv is None:
        if _passive_tv is not None:
            _log('no TV on nearby: passive watching stops')
        _passive_at = _passive_tv = None
        return
    if tv is not _passive_tv:
        _log('passive watching {} ({:.2f} fun a game minute)'.format(
            tv, (_watch_rate or DEFAULT_WATCH_RATE) * _settings['share']))
        _passive_tv = tv
    if _passive_at is None or now < _passive_at:
        _passive_at = now
        return
    minutes = now - _passive_at
    if minutes < 1.0:
        return
    _passive_at = now
    fun = _fun()
    if fun is None:
        return
    try:
        sim.commodity_tracker.add_value(fun, (_watch_rate or DEFAULT_WATCH_RATE) * _settings['share'] * min(minutes, 60.0))
    except Exception as e:
        _note_error('passive fun', e)


# --- watching -------------------------------------------------------------------

def _free(sim):
    """Nothing else for your Sim to do (sitting is fine)."""
    queue = sim.queue
    running = getattr(queue, 'running', None)
    for interaction in list(queue):
        if interaction is running:
            continue
        return False
    if running is None or running is getattr(_watch, 'interaction', None):
        return True
    return getattr(running, 'provided_posture_type', None) is not None  # a seat (the sit interaction)


def _start_watch(sim, tv, channel):
    global _watch
    from interactions.aop import AffordanceObjectPair
    from interactions.context import InteractionContext, QueueInsertStrategy
    from interactions.priority import Priority
    from sims4vr import remote
    affordance = channel.affordance
    context = InteractionContext(sim, InteractionContext.SOURCE_PIE_MENU, Priority.High,
                                 insert_strategy=QueueInsertStrategy.LAST)
    aop = AffordanceObjectPair(affordance, tv, affordance, None)
    made = aop.interaction_factory(context)
    interaction = getattr(made, 'interaction', None)
    if not made or interaction is None:
        _log('{} on {} not made: {}'.format(affordance.__name__, tv, made))
        return
    interaction._sims4vr_gaze = True  # remote.py lets it through
    interaction.off_channel = channel  # stopping leaves the TV on
    remote._stay_put(interaction)
    result = AffordanceObjectPair.execute_interaction(interaction)
    if not result:
        _log('{} on {} not queued: {}'.format(affordance.__name__, tv, result))
        return
    _watch = _Watch(interaction, tv, channel, _minutes(), _fun_value(sim))
    _log('watching {} ({}): you look at it'.format(tv, affordance.__name__))


def _learn_rate(watch, sim):
    global _watch_rate
    fun = _fun_value(sim)
    minutes = _minutes() - watch.minute
    if fun is None or watch.fun is None or minutes < MEASURE_MINUTES:
        return
    rate = (fun - watch.fun) / minutes
    if fun >= 99.0:
        return  # full: the rate is cut off
    if rate > 0:
        _watch_rate = rate
        _log('watching gave {:.2f} fun a game minute over {:.0f} minutes'.format(rate, minutes))


def _end_watch(why):
    global _watch
    watch, _watch = _watch, None
    if watch is None:
        return
    try:
        _learn_rate(watch, watch.interaction.sim)
    except Exception:
        pass
    try:
        if not watch.interaction.is_finishing:
            watch.interaction.cancel_user(cancel_reason_msg='VR: ' + why)
    except Exception as e:
        _log('stopping the watch failed: {!r}'.format(e))
    _log('stopped watching {} ({})'.format(watch.tv, why))


def _moving():
    pose = native.walk_pose()
    return bool(pose and pose[4])


def _note_error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        _log('{} failed: {!r}'.format(what, e))


@native.timed('tv watch tick', 0.006)
def _tick(_handle):
    global _scanned, _looked_at
    try:
        sim = services.get_active_sim()
        if not _first_person(sim):
            _end_watch('not in first person')
            _engine_stop('not in first person')
            return
        now = time.monotonic()
        if now - _scanned >= SCAN_EVERY:
            _scanned = now
            _scan()
        if not _tvs:
            _trickle(sim, None)
            _engine_stop('no TV on the lot')
            return
        snap = native.hands()
        tv = _looking_at(sim, snap, now) if snap is not None else None
        if tv is not None:
            _looked_at = (tv, now)
        moving = _moving()
        if _settings['engine']:
            _end_watch('the engine watches instead')
            _engine(sim, now, moving)
            return
        watch = _watch
        if watch is not None:
            if watch.interaction.is_finishing or watch.interaction.has_been_canceled:
                _end_watch('it ended')
            elif _channel(watch.tv) is not watch.channel:
                _end_watch('the channel changed or the TV went off')
            elif moving:
                _end_watch('you walked')
            elif _looked_at is None or _looked_at[0] is not watch.tv or now - _looked_at[1] > _settings['grace']:
                _end_watch('you looked away')
        if _watch is None and tv is not None and not moving and _free(sim):
            _start_watch(sim, tv, _channel(tv))
        _trickle(sim, None if _watch is not None else _nearby(sim))
    except Exception as e:
        _note_error('tick', e)


# --- engine (passive_gain.py): the watch interaction's gains, no interaction ------------------

ENGINE_KEY = 'tv'
HEAR_STEP = 0.02  # strength steps for a TV you only hear (the modifier is re-placed only on a step)


def _heard(sim, passive_gain):
    """The TV that is on you hear best: (tv, hearing 0..1) or (None, 0); [tv] nearby = its reach at High volume."""
    best, heard = None, 0.0
    for tv in _tvs:
        try:
            if _channel(tv) is None:
                continue
            h = passive_gain.hearing(sim, tv, _settings['nearby'])
            if h > heard:
                best, heard = tv, h
        except Exception as e:
            _note_error('hearing {}'.format(tv), e)
    return (best, heard) if heard >= HEAR_STEP else (None, 0.0)
_engine_on = None  # (tv, channel, strength) last given to the engine


def _engine(sim, now, moving):
    """Looking (within the grace, standing still): full; else a TV on nearby: passive_share."""
    global _engine_on
    from sims4vr import passive_gain
    tv = strength = None
    looked = _looked_at
    if looked is not None and now - looked[1] <= _settings['grace'] and not moving and _channel(looked[0]) is not None:
        tv, strength = looked[0], 1.0
    else:
        tv, heard = _heard(sim, passive_gain)
        strength = round(_settings['share'] * heard / HEAR_STEP) * HEAR_STEP if tv is not None else None
    channel = _channel(tv) if tv is not None else None
    if channel is None or not strength:
        _engine_stop('no TV on near you' if tv is None else 'nothing to give')
        return
    state = (tv, channel, strength)
    if state == _engine_on:
        return
    if _engine_on is not None and (_engine_on[0] is not tv or _engine_on[1] is not channel):
        passive_gain.stop(ENGINE_KEY, 'other TV or channel')
    _engine_on = state
    passive_gain.start(ENGINE_KEY, sim, channel.affordance, tv, strength)
    _log('{} {} ({}), strength {:.2f}'.format('watching' if strength >= 1.0 else 'TV nearby:', tv,
                                              channel.affordance.__name__, strength))


def _engine_stop(why):
    global _engine_on
    if _engine_on is None:
        return
    _engine_on = None
    try:
        from sims4vr import passive_gain
        passive_gain.stop(ENGINE_KEY, why)
    except Exception as e:
        _note_error('engine stop', e)
