"""First person: looking is doing. Look through a window for a while and you peep;
look at a Sim doing something worth watching for a while and you watch.

Both are WickedWhims interactions (it is the mod that has them; without it
nothing happens): the pie menu's Peep Through Window on a window, and Watch
on a Sim in a sex animation. Looking steadily at the target (head forward within
[gazewatch] peep_angle / watch_angle degrees, no wall between, standing still)
for peep_seconds / watch_seconds starts that interaction right where you stand
(remote._stay_put: no walking to the window, no turning). Looking away for
[gazewatch] grace seconds, walking or the interaction ending stops it.

Which interaction runs comes from names in sims4vr.ini ([gazewatch] peep_names,
watch_names): each is tried on the target in order until one passes its own
test; the game's/WickedWhims' test decides whether it is allowed (a Sim must be
visible through the window, the Sim must really be watchable...). Nothing is
pushed when none passes, and a target that failed is not retried for
RETRY_AFTER seconds. Log lines: "gazewatch: ..." in sims4vr_python.log.

Only your Sim, only in first person (body.py starts and stops it); nothing is
pushed onto it while it does something else. docs/PASSIVE-OBJECTS.md.
"""
import math
import time

import alarms
import clock
import services

from sims4vr import native

TICK_SECONDS = 0.25
SCAN_EVERY = 10.0  # seconds between looks for windows on the lot
RETRY_AFTER = 20.0  # seconds a target that failed its test is left alone
BEHIND_WINDOW = 12.0  # metres behind a window a Sim still counts as seen through it

_owner = type('_Owner', (), {})()
_alarm = None
_logs = 0
_errors = 0
_windows = []
_scanned = 0.0
_dwell = {}  # target key -> [seconds looked, last time looked]
_failed = {}  # target key -> time it failed
_active = None  # _Active: the interaction we run
_types = {}  # interaction name -> affordance (False: not found)
_window_tags = None


class _Active:
    __slots__ = ('interaction', 'key', 'target', 'kind', 'looked')

    def __init__(self, interaction, key, target, kind):
        self.interaction, self.key, self.target, self.kind = interaction, key, target, kind
        self.looked = time.monotonic()


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 200:
        native._log('gazewatch: ' + message)


def _note_error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        _log('{} failed: {!r}'.format(what, e))


def _number(key, default):
    try:
        return float(native.setting('gazewatch', key, default))
    except (TypeError, ValueError):
        return default


def _names(key, default):
    text = native.setting('gazewatch', key, default) or ''
    return [n.strip() for n in str(text).split(',') if n.strip()]


def _first_person(sim):
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def start():
    global _alarm, _scanned
    _scanned = 0.0
    if _alarm is None and native.setting_flag('gazewatch', 'on', True):
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
        _log('on')


def stop():
    global _alarm
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    _end('first person ended')
    _windows[:] = []
    _dwell.clear()
    _failed.clear()


# --- finding things ---------------------------------------------------------------------

def _affordance(name):
    if name not in _types:
        found = None
        try:
            import sims4.resources
            manager = services.get_instance_manager(sims4.resources.Types.INTERACTION)
            for t in manager.types.values():
                if getattr(t, '__name__', '') == name:
                    found = t
                    break
        except Exception as e:
            _note_error('looking up ' + name, e)
        _types[name] = found or False
        _log('{}: {}'.format(name, 'found' if found else 'not in this game'))
    return _types[name] or None


def _window_tag_set():
    global _window_tags
    if _window_tags is None:
        _window_tags = set()
        try:
            from tag import Tag
            _window_tags = {t for t in Tag if 'WINDOW' in t.name.upper()}
        except Exception as e:
            _note_error('window tags', e)
        _log('window tags: {}'.format(sorted(t.name for t in _window_tags)))
    return _window_tags


def _scan():
    tags = _window_tag_set()
    found = []
    if tags:
        for obj in list(services.object_manager().values()):
            try:
                if not getattr(obj, 'is_sim', False) and tags & obj.get_tags():
                    found.append(obj)
            except Exception:
                pass
    if [w.id for w in found] != [w.id for w in _windows]:
        _log('{} window(s) on the lot'.format(len(found)))
    _windows[:] = found


def _angle(head, forward, point):
    d = [point[i] - head[i] for i in range(3)]
    dist = math.sqrt(sum(v * v for v in d))
    flen = math.sqrt(sum(v * v for v in forward)) or 1.0
    if dist < 0.05:
        return 0.0, dist
    cos = sum(forward[i] * d[i] for i in range(3)) / (flen * dist)
    return math.degrees(math.acos(max(-1.0, min(1.0, cos)))), dist


def _centre(obj):
    p = obj.position
    return (p.x, p.y + 1.0, p.z)


def _room(position, level):
    try:
        import build_buy
        return build_buy.get_room_id(services.current_zone_id(), position, level) or 0
    except Exception:
        return 0


def _seen_through(sim, window, head):
    """Another Sim on the far side of the window (a different room from yours), in line with it."""
    wc = _centre(window)
    d = [wc[i] - head[i] for i in range(3)]
    dist = math.hypot(d[0], d[2]) or 1.0
    mine = _room(sim.position, sim.level)
    for other in services.sim_info_manager().instanced_sims_gen():
        try:
            if other is sim or other.level != window.level:
                continue
            p = other.position
            along = ((p.x - wc[0]) * d[0] + (p.z - wc[2]) * d[2]) / dist
            if along < 0.2 or along > BEHIND_WINDOW:
                continue
            side = abs((p.x - wc[0]) * d[2] - (p.z - wc[2]) * d[0]) / dist
            if side > 0.6 * along + 1.0:
                continue
            if _room(p, other.level) != mine:
                return True
        except Exception:
            pass
    return False


def _clear_sight(sim, target, head, centre, now):
    try:
        from sims4vr import tvwatch
        return tvwatch._clear_sight(sim, target, head, (centre, (0.0, 0.0, 0.0)), now)
    except Exception:
        return True


def _candidates(sim, head, forward, now):
    """(key, kind, target, names) of everything you look at right now."""
    out = []
    if native.setting_flag('gazewatch', 'peep', True):
        reach, allowed = _number('peep_range', 3.0), _number('peep_angle', 25.0)
        for window in _windows:
            try:
                if window.level != sim.level:
                    continue
                angle, dist = _angle(head, forward, _centre(window))
                if dist <= reach and angle <= allowed and _seen_through(sim, window, head):
                    out.append((('peep', window.id), 'peep', window, _names('peep_names', 'WW_PEEP_THROUGH_WINDOW')))
            except Exception as e:
                _note_error('window {}'.format(window), e)
    if native.setting_flag('gazewatch', 'watch', True):
        reach, allowed = _number('watch_range', 12.0), _number('watch_angle', 12.0)
        for other in services.sim_info_manager().instanced_sims_gen():
            try:
                if other is sim or other.level != sim.level:
                    continue
                p = other.position
                angle, dist = _angle(head, forward, (p.x, p.y + 1.0, p.z))
                if dist <= reach and angle <= allowed + math.degrees(math.atan(0.4 / max(dist, 0.5))):
                    out.append((('watch', other.id), 'watch', other,
                                _names('watch_names', 'WW_SEX_WATCH_DEFAULT_TRANSITION,WW_SEX_WATCH_DEFAULT')))
            except Exception as e:
                _note_error('sim {}'.format(other), e)
    return out


# --- starting and stopping ----------------------------------------------------------------

def _free(sim):
    queue = sim.queue
    running = getattr(queue, 'running', None)
    for interaction in list(queue):
        if interaction is not running:
            return False
    if running is None or _active is not None and running is _active.interaction:
        return True
    return getattr(running, 'provided_posture_type', None) is not None  # a seat


def _try_start(sim, key, kind, target, names):
    global _active
    from interactions.aop import AffordanceObjectPair
    from interactions.context import InteractionContext, QueueInsertStrategy
    from interactions.priority import Priority
    from sims4vr import remote
    context = InteractionContext(sim, InteractionContext.SOURCE_PIE_MENU, Priority.High,
                                 insert_strategy=QueueInsertStrategy.LAST)
    last = 'no such interaction'
    for name in names:
        affordance = _affordance(name)
        if affordance is None:
            continue
        aop = AffordanceObjectPair(affordance, target, affordance, None)
        tested = aop.test(context)
        if not tested:
            last = '{}: {}'.format(name, getattr(tested, 'reason', tested))
            continue
        made = aop.interaction_factory(context)
        interaction = getattr(made, 'interaction', None)
        if not made or interaction is None:
            last = '{} not made: {}'.format(name, made)
            continue
        interaction._sims4vr_gaze = True  # remote.py lets it through
        remote._stay_put(interaction)
        result = AffordanceObjectPair.execute_interaction(interaction)
        if not result:
            last = '{} not queued: {}'.format(name, result)
            continue
        _active = _Active(interaction, key, target, kind)
        _log('{} {}: {} ({} s of looking)'.format(kind, target, name, round(_dwell[key][0], 1)))
        return True
    _failed[key] = time.monotonic()
    _log('{} {} not started: {}'.format(kind, target, last))
    return False


def _end(why):
    global _active
    active, _active = _active, None
    if active is None:
        return
    try:
        if not active.interaction.is_finishing:
            active.interaction.cancel_user(cancel_reason_msg='VR: ' + why)
    except Exception as e:
        _log('stopping failed: {!r}'.format(e))
    _log('stopped {} {} ({})'.format(active.kind, active.target, why))


def _moving():
    pose = native.walk_pose()
    return bool(pose and pose[4])


@native.timed('gaze watch tick', 0.006)
def _tick(_handle):
    global _scanned
    try:
        sim = services.get_active_sim()
        if not _first_person(sim):
            _end('not in first person')
            return
        now = time.monotonic()
        if now - _scanned >= SCAN_EVERY:
            _scanned = now
            _scan()
        snap = native.hands()
        if snap is None:
            return
        head, forward = snap[0], snap[1]
        if not all(math.isfinite(v) for v in head) or not all(math.isfinite(v) for v in forward):
            return
        moving = _moving()
        looked = {}
        for key, kind, target, names in _candidates(sim, head, forward, now):
            if _clear_sight(sim, target, head, _centre(target) if kind == 'peep' else
                            (target.position.x, target.position.y + 1.0, target.position.z), now):
                looked[key] = (kind, target, names)
        grace = _number('grace', 2.0)
        for key in list(_dwell):
            if key not in looked and now - _dwell[key][1] > grace:
                del _dwell[key]
        for key in looked:
            entry = _dwell.setdefault(key, [0.0, now])
            entry[0] += TICK_SECONDS if now - entry[1] < 1.0 else 0.0
            entry[1] = now
        active = _active
        if active is not None:
            if active.interaction.is_finishing or active.interaction.has_been_canceled:
                _end('it ended')
            elif moving:
                _end('you walked')
            elif active.key in looked:
                active.looked = now
            elif now - active.looked > grace:
                _end('you looked away')
        if _active is None and not moving and looked and _free(sim):
            for key, (kind, target, names) in looked.items():
                need = _number('peep_seconds', 4.0) if kind == 'peep' else _number('watch_seconds', 1.5)
                if _dwell[key][0] < need or now - _failed.get(key, -1e9) < RETRY_AFTER:
                    continue
                if _try_start(sim, key, kind, target, names):
                    break
    except Exception as e:
        _note_error('tick', e)
