"""Your hands in first person: pick things up, carry them, put them down, eat
and drink.

The DLL (grab.cpp) watches the controllers: a hand that closes within reach of
an object holds it at once. Twenty times a second this module:
- tells the DLL which objects near you can be picked up (carryable, nobody
  using them, not held by anyone), with their centre and reach;
- parents a grabbed object to the Sim's hand joint where it sits in your hand,
  so it moves with your hand (no carry animation, no posture);
- puts an object you let go of down: into the nearest free slot on a surface
  under it (a counter, a table), else on the floor below;
- eating and drinking: food or a drink in your hand brought to your mouth
  takes one bite, one of the game's own turns of eating it (hunger, calories,
  the plate empties). An empty hand that dipped into food (a fork) bites too.
Switches: [hands] grab / eat in sims4vr.ini.
"""
import math
import time

import alarms
import clock
import services
import sims4.math
import terrain

from sims4vr import native

TICK_SECONDS = 0.05
LIST_EVERY = 0.4  # seconds between looking for objects near you
NEAR = 1.6  # metres around you in which objects can be picked up
REACH = 0.18  # metres from an object's centre at which a closing hand takes it
CENTRE_UP = 0.05  # metres from an object's origin (its base) up to where you hold it
SLOT_REACH = 0.35  # metres sideways from where you let go to a free slot on a surface
SLOT_BELOW = 0.7  # metres an object may drop onto a surface below where you let go
SLOT_ABOVE = 0.15  # metres a slot may sit above it (your hand pushed into the counter)
HAND_JOINTS = ('b__L_Hand__', 'b__R_Hand__')
MOUTH_FORWARD = 0.06  # metres in front of your eyes and below them: your mouth
MOUTH_DOWN = 0.09
MOUTH_ENTER = 0.17  # metres between a palm and your mouth that count as eating
MOUTH_LEAVE = 0.26  # and that the hand must move away before the next bite
LOAD_REACH = 0.18  # metres from food at which an empty hand picks up a forkful
LOAD_KEEP = 4.0  # seconds a forkful stays on an empty hand
BITE_EVERY = 0.8  # seconds at least between bites

_owner = type('_Owner', (), {})()
_alarm = None
_held = [None, None]  # per hand: (object, (rel position), (rel orientation xyzw), Sim holding it)
_stuck = []  # objects a put-down failed for: (object, Sim, when), tried again
_grabbable_ids = ()
_food = []  # (object, centre) near you, for forkfuls
_last_list = 0.0
_at_mouth = [False, False]
_loaded = [None, None]  # per hand: (food, when)
_last_bite = 0.0
_grab_on = True
_eat_on = True
_errors = 0
_logs = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 200:
        native._log('hands: ' + message)


def _error(where, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('hands: {} failed: {!r}'.format(where, e))


# --- small quaternion helpers (x, y, z, w) -----------------------------------

def _mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _conj(q):
    return (-q[0], -q[1], -q[2], q[3])


def _rotate(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))


def _joint_hash(name):
    """The game's joint name hash (FNV-1 of the lower-case name), as the DLL uses it."""
    h = 0x811c9dc5
    for c in name.lower().encode('ascii'):
        h = ((h * 0x1000193) & 0xffffffff) ^ c
    return h


# --- life cycle ---------------------------------------------------------------

def start():
    global _alarm, _grab_on, _eat_on
    _grab_on = native.setting_flag('hands', 'grab', True)
    _eat_on = native.setting_flag('hands', 'eat', True)
    if _alarm is None and (_grab_on or _eat_on):
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
        _log('on (picking up {}, eating {})'.format('on' if _grab_on else 'off', 'on' if _eat_on else 'off'))


def stop():
    global _alarm
    if _alarm is not None:
        alarms.cancel_alarm(_alarm)
        _alarm = None
    release_all('first person ended')
    native.set_grabbables([])


def release_all(why):
    """Put down whatever your hands hold (leaving first person, saving,
    another Sim)."""
    for hand in (0, 1):
        entry = _held[hand]
        if entry is None:
            continue
        _held[hand] = None
        native.grab_set(hand, 0)
        obj, owner = entry[0], entry[3]
        if obj.parent is owner:
            where = _drop(owner, obj, hand, entry)
            _log('put {} down {}: {}'.format(obj, where, why))
    _retry_stuck(force=True)


def _drop(owner, obj, hand, entry):
    """Put a held object down; if every way fails it is tried again later."""
    try:
        return _put_down(owner, obj, hand, entry)
    except Exception as e:
        _error('putting {} down'.format(obj), e)
    _stuck.append((obj, owner, time.monotonic()))
    return 'later (it stuck)'


def _retry_stuck(force=False):
    now = time.monotonic()
    for item in list(_stuck):
        obj, owner, when = item
        if not force and now - when < 1.0:
            continue
        _stuck.remove(item)
        try:
            if getattr(obj, 'is_being_destroyed', False) or obj.parent is not owner:
                continue
            t = obj.transform
            surface = owner.routing_surface
            floor = terrain.get_terrain_height(t.translation.x, t.translation.z, routing_surface=surface)
            obj.clear_parent(sims4.math.Transform(sims4.math.Vector3(t.translation.x, floor, t.translation.z),
                                                  t.orientation), surface)
            _log('{} put down below where it was'.format(obj))
        except Exception as e:
            _error('putting {} down again'.format(obj), e)
            if len(_stuck) < 8:
                _stuck.append((obj, owner, now))


# --- what you can pick up -----------------------------------------------------

def _pickable(obj, sim):
    if obj.is_sim or getattr(obj, 'sim_info', None) is not None:  # Sims and babies are not things
        return False
    parent = obj.parent
    if parent is not None and parent.is_sim:
        return False  # in someone's hands already (yours too)
    if obj.in_use:
        return False
    return True


def _refresh_lists(sim, x, y, z):
    """The pick-up candidates and food near you, nearest first."""
    global _grabbable_ids, _food
    from objects.components.types import CARRYABLE_COMPONENT
    found = []
    food = []
    for obj in services.object_manager().get_all_objects_with_component_gen(CARRYABLE_COMPONENT):
        try:
            p = obj.position
            dx, dy, dz = p.x - x, p.y + CENTRE_UP - y, p.z - z
            if abs(dx) > NEAR or abs(dz) > NEAR or abs(dy) > 2.2:
                continue
            d = math.sqrt(dx * dx + dz * dz)
            if d > NEAR or not _pickable(obj, sim):
                continue
            centre = (p.x, p.y + CENTRE_UP, p.z)
            found.append((d, obj.id, centre))
            if getattr(obj, 'consumable_component', None) is not None:
                food.append((obj, centre))
        except Exception as e:
            _error('looking at {}'.format(obj), e)
    found.sort(key=lambda f: f[0])
    entries = [(oid, c[0], c[1], c[2], REACH) for _, oid, c in found[:12]] if _grab_on else []
    native.set_grabbables(entries)
    ids = tuple(e[0] for e in entries)
    if ids != _grabbable_ids:
        _grabbable_ids = ids
    # Food you hold counts for forkfuls too.
    for entry in _held:
        if entry is not None and getattr(entry[0], 'consumable_component', None) is not None:
            food.append((entry[0], None))
    _food = food


# --- grabbing and letting go --------------------------------------------------

def _hand_pose(sim, hand):
    """The Sim's hand joint in the world, as the game thread read it this
    frame (known for the Sim you are in only)."""
    joint = native.hand_joint(hand)
    if joint is None:
        return None
    sim_id, (px, py, pz), q = joint
    if sim_id != sim.id or not all(math.isfinite(v) for v in (px, py, pz) + tuple(q)):
        return None
    return (px, py, pz), tuple(q)


def _grab(sim, hand, object_id):
    obj = services.object_manager().get(object_id)
    if obj is None or not _pickable(obj, sim) or _held[hand] is not None:
        native.grab_set(hand, 0)
        _log('could not pick up {:x}: {}'.format(object_id, 'gone' if obj is None else 'in use or held'))
        return
    pose = _hand_pose(sim, hand)
    if pose is None:
        native.grab_set(hand, 0)
        _log('could not pick up {}: no hand joint'.format(obj))
        return
    hp, hq = pose
    t = obj.transform
    op, oq = t.translation, t.orientation
    rel_p = _rotate(_conj(hq), (op.x - hp[0], op.y - hp[1], op.z - hp[2]))
    rel_q = _mul(_conj(hq), (oq.x, oq.y, oq.z, oq.w))
    try:
        obj.set_parent(sim, sims4.math.Transform(sims4.math.Vector3(*rel_p), sims4.math.Quaternion(*rel_q)),
                       joint_name_or_hash=_joint_hash(HAND_JOINTS[hand]))
    except Exception as e:
        native.grab_set(hand, 0)
        _error('picking up {}'.format(obj), e)
        return
    _held[hand] = (obj, rel_p, rel_q, sim)
    native.grab_set(hand, obj.id)
    native.haptic(hand, 0.5, 0.04)
    _log('{} hand picked up {} ({:.2f} m from the wrist)'.format(
        'right' if hand else 'left', obj, math.sqrt(sum(v * v for v in rel_p))))


def _world_pose(sim, hand, entry):
    """Where a held object is now: your hand and its grip offset."""
    pose = _hand_pose(sim, hand)
    if pose is None:
        t = entry[0].transform
        return (t.translation.x, t.translation.y, t.translation.z), (
            t.orientation.x, t.orientation.y, t.orientation.z, t.orientation.w)
    hp, hq = pose
    r = _rotate(hq, entry[1])
    return (hp[0] + r[0], hp[1] + r[1], hp[2] + r[2]), _mul(hq, entry[2])


def _free_slot(obj, p, sim):
    """The nearest free slot on a surface that takes this object, under or
    beside where you let go."""
    candidates = []
    for other in services.object_manager().valid_objects():
        if other is obj or other.is_sim:
            continue
        try:
            q = other.position
            if abs(q.x - p[0]) > 2.5 or abs(q.z - p[2]) > 2.5:
                continue
            parent = other.parent
            if parent is not None and parent.is_sim:
                continue
            for slot in other.get_runtime_slots_gen():
                s = slot.position
                side = math.hypot(s.x - p[0], s.z - p[2])
                drop = p[1] - s.y
                if side < SLOT_REACH and -SLOT_ABOVE < drop < SLOT_BELOW:
                    candidates.append((side + abs(drop) * 0.3, len(candidates), slot))
        except Exception as e:
            _error('slots of {}'.format(other), e)
    candidates.sort(key=lambda c: (c[0], c[1]))
    for _, _, slot in candidates[:10]:
        try:
            if slot.empty and slot.is_valid_for_placement(obj=obj):
                return slot
        except Exception as e:
            _error('testing a slot', e)
    return None


def _put_down(sim, obj, hand, entry):
    p, q = _world_pose(sim, hand, entry)
    try:
        slot = _free_slot(obj, p, sim)
        if slot is not None:
            slot.add_child(obj)
            return 'on {}'.format(slot.owner)
    except Exception as e:
        _error('putting {} on a surface'.format(obj), e)
    from carry.carry_postures import CarryingObject
    surface = sim.routing_surface
    floor = terrain.get_terrain_height(p[0], p[2], routing_surface=surface)
    f = _rotate(q, (0.0, 0.0, 1.0))
    yaw = math.atan2(f[0], f[2]) if abs(f[0]) + abs(f[2]) > 1e-3 else 0.0
    start = sims4.math.Transform(sims4.math.Vector3(p[0], floor, p[2]),
                                 sims4.math.Quaternion(0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2)))
    CarryingObject.snap_to_good_location_on_floor(obj, starting_transform=start, starting_routing_surface=surface)
    return 'on the floor'


def _let_go(hand, object_id):
    entry = _held[hand]
    if entry is None or entry[0].id != object_id:
        return
    _held[hand] = None
    obj, owner = entry[0], entry[3]
    if obj.parent is not owner:
        _log('{} left your hand on its own'.format(obj))
        return
    where = _drop(owner, obj, hand, entry)
    native.haptic(hand, 0.25, 0.03)
    _log('{} hand put {} down {}'.format('right' if hand else 'left', obj, where))


def _check_held(sim):
    """Put down what another Sim's hands still hold (you switched Sims), and
    forget held objects the game moved or deleted."""
    for hand in (0, 1):
        entry = _held[hand]
        if entry is None:
            continue
        obj, owner = entry[0], entry[3]
        if owner is not sim:
            _held[hand] = None
            native.grab_set(hand, 0)
            if obj.parent is owner:
                _log('put {} down {}: you are in another Sim now'.format(obj, _drop(owner, obj, hand, entry)))
            continue
        gone = getattr(obj, 'is_being_destroyed', False) or services.object_manager().get(obj.id) is None
        if gone or obj.parent is not sim:
            _held[hand] = None
            native.grab_set(hand, 0)
            _log('{} is no longer in your {} hand'.format(obj, 'right' if hand else 'left'))


# --- eating and drinking ------------------------------------------------------

def _bite(sim, food, hand):
    global _last_bite
    component = getattr(food, 'consumable_component', None)
    if component is None:
        return
    try:
        left = component.bites_left()
    except Exception:
        left = 1
    if left <= 0.01:
        _log('{} is empty'.format(food))
        return
    from event_testing.resolver import SingleActorAndObjectResolver
    from interactions.utils.loot import LootOperationList
    resolver = SingleActorAndObjectResolver(sim.sim_info, food, source=None)
    LootOperationList(resolver, component.loot_list).apply_operations()
    _last_bite = time.monotonic()
    native.haptic(hand, 0.3, 0.06)
    try:
        left = component.bites_left()
    except Exception:
        left = -1
    _log('bite of {} ({:.0f} left)'.format(food, left))


def _food_in(hand):
    entry = _held[hand]
    if entry is not None and getattr(entry[0], 'consumable_component', None) is not None:
        return entry[0]
    return None


def _eat(sim, snap):
    head, forward, up, left, right = snap
    if not all(math.isfinite(v) for v in head):
        return
    mouth = tuple(head[i] + forward[i] * MOUTH_FORWARD - up[i] * MOUTH_DOWN for i in range(3))
    now = time.monotonic()
    for hand, state in ((0, left), (1, right)):
        valid, palm, _q, _grip, _trigger, _held_id = state
        if not valid:
            continue
        if _held[hand] is None:
            # An empty hand dipping into food takes a forkful.
            for food, centre in _food:
                c = centre
                if c is None:
                    continue
                if math.sqrt(sum((palm[i] - c[i]) ** 2 for i in range(3))) < LOAD_REACH:
                    _loaded[hand] = (food, now)
                    break
        d = math.sqrt(sum((palm[i] - mouth[i]) ** 2 for i in range(3)))
        if not _at_mouth[hand] and d < MOUTH_ENTER:
            _at_mouth[hand] = True
            food = _food_in(hand)
            if food is None and _held[hand] is None:
                food = _food_in(1 - hand)  # a fork hand while the other holds the plate
                if food is None and _loaded[hand] is not None and now - _loaded[hand][1] < LOAD_KEEP:
                    food = _loaded[hand][0]
            if food is not None and now - _last_bite >= BITE_EVERY:
                _loaded[hand] = None
                try:
                    _bite(sim, food, hand)
                except Exception as e:
                    _error('a bite of {}'.format(food), e)
        elif _at_mouth[hand] and d > MOUTH_LEAVE:
            _at_mouth[hand] = False


# --- the tick -----------------------------------------------------------------

@native.timed('hands tick')
def _tick(_handle):
    global _last_list
    try:
        from sims4vr import body
        if not body.enabled:
            return
        # The Sim you are in (not the selected one: a switch reaches body a
        # moment later, and until then your hands are still that Sim's).
        sim = services.object_manager().get(body.following())
        if sim is None:
            return
        _check_held(sim)
        if _stuck:
            _retry_stuck()
        while True:
            event = native.hand_event()
            if event is None:
                break
            kind, hand, object_id = event
            if kind == 1 and _grab_on:
                _grab(sim, hand, object_id)
            elif kind == 1:
                native.grab_set(hand, 0)
            elif kind == 2:
                _let_go(hand, object_id)
        try:
            from sims4vr import bodygrab
            bodygrab.tick(sim)
        except Exception as e:
            _error('body grab', e)
        snap = native.hands()
        now = time.monotonic()
        if now - _last_list >= LIST_EVERY:
            _last_list = now
            if snap is not None and all(math.isfinite(v) for v in snap[0]):
                x, y, z = snap[0]
            else:
                p = sim.position
                x, y, z = p.x, p.y + 1.0, p.z
            if _grab_on or _eat_on:
                _refresh_lists(sim, x, y - 0.6, z)
        if snap is not None and _eat_on:
            _eat(sim, snap)
    except Exception as e:
        _error('tick', e)
