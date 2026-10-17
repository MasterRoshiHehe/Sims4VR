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
        'nearby': number('nearby', 4.0),
        'share': number('passive_share', 0.3),
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
        centre, normal = screen[0], screen[1]
        surface = sim.routing_surface
        y = sim.position.y
        aim = (centre[0] + normal[0] * SCREEN_CLEARANCE, centre[2] + normal[2] * SCREEN_CLEARANCE)
        origin = routing.Location(sims4.math.Vector3(head[0], y, head[2]), sims4.math.Quaternion.ZERO(), surface)
        end = routing.Location(sims4.math.Vector3(aim[0], y, aim[1]), sims4.math.Quaternion.ZERO(), surface)
        clear = bool(routing.ray_test(origin, end, sim.routing_context))
    except Exception as e:
        _log('wall check failed: {!r}'.format(e))
    _sight[tv.id] = (now, clear)
    return clear


def _looking_at(sim, snap, now):
    """The TV that is on and that you look at, or None."""
    head, forward = snap[0], snap[1]
    if not all(math.isfinite(v) for v in head) or not all(math.isfinite(v) for v in forward):
        return None
    flen = math.sqrt(sum(v * v for v in forward)) or 1.0
    best = None
    for tv in _tvs:
        try:
            if tv.level != sim.level or _channel(tv) is None:
                continue
            screen = _screen(tv, now)
            centre, normal = screen[0], screen[1]
            d = [centre[i] - head[i] for i in range(3)]
            dist = math.sqrt(sum(v * v for v in d))
            if dist < 0.1 or dist > FULL_RANGE:
                continue
            if sum(normal[i] * d[i] for i in range(3)) > 0:
                continue  # behind the screen
            cos = sum(forward[i] * d[i] for i in range(3)) / (flen * dist)
            angle = math.degrees(math.acos(max(-1.0, min(1.0, cos))))
            allowed = _settings['angle'] + math.degrees(math.atan(0.5 * max(screen[2], screen[3]) / dist))
            if angle > allowed:
                continue
            if best is not None and angle >= best[0]:
                continue
            if _clear_sight(sim, tv, head, screen, now):
                best = (angle, tv)
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
            return
        now = time.monotonic()
        if now - _scanned >= SCAN_EVERY:
            _scanned = now
            _scan()
        if not _tvs:
            _trickle(sim, None)
            return
        snap = native.hands()
        tv = _looking_at(sim, snap, now) if snap is not None else None
        if tv is not None:
            _looked_at = (tv, now)
        moving = _moving()
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
