"""First person: touch a light switch and the lights of its room flip.

The game's placeable switches are decorative wall sculptures (tuning prototype_RetailCompatible_WallObject
116915, OBJD 0x51EA6/7 Clickity, 0x52053/4 Rocket, 0x6E78C-0x6E795 Delightful) with NO light interaction of
their own, so the flip is always made directly. They are found by model name containing "lightswitch" or
by catalog name (catalog names "Clickity Light Switch", "The Delightful Light
Switch", "Rocket Light Switch & Dimmer"; keys in SWITCH_NAME_KEYS, more in [lightswitch] name_keys)
are found by their catalog name (build_buy.get_object_catalog_name). A palm (either hand, any
grip/trigger state) that comes within [lightswitch] touch_radius metres of one flips the lights of
the room the switch is in, once per touch: the hand must leave 1.5 x that distance and
[lightswitch] cooldown seconds must pass before the same hand flips it again. A short buzz
confirms it ([lightswitch] haptics).

What flips: any light in the room that is on (dimmer above 0) -> all of them off; none on -> all
on, the way the game's own "This Room" lights do it (objects.lighting.lighting_utils
lights_in_target_room_gen, dimmer 1 / 0; lights tagged MANUAL_LIGHT_TAG are left alone). The
switch's own tuned room-light affordance runs when it has one (SwitchLightImmediateInteraction
with a room target and the wanted dimmer), as your own pick, no walking; otherwise the same
change is made through LightingHelper's per-light calls. Log: "lightswitch: ..." in
sims4vr_python.log ([lightswitch] log).

Only your Sim, only in first person (body.py starts and stops it). Nothing is queued while
you do something that is not an immediate interaction.
"""
import math
import time

import alarms
import clock
import services

from sims4vr import native

TICK_SECONDS = 0.05
SCAN_EVERY = 5.0  # seconds between looks for switches on the lot
NEAR = 3.0  # metres from the head a switch is looked at
APPROACH = 0.5  # metres: palms nearer than this get their closest pass logged
LEAVE_FACTOR = 1.5  # the hand must get this much further than touch_radius before a re-touch counts
AUTO_DIMMER = -1  # LightingComponent.LIGHT_AUTOMATION_DIMMER_VALUE
SWITCH_NAME_KEYS = {
    0xE0F898E2,  # Clickity Light Switch
    0x22A6DA71,  # The Delightful Light Switch
    0xA2214E16,  # Rocket Light Switch & Dimmer
}

_owner = type('_Owner', (), {})()
_alarm = None
_logs = 0
_errors = 0
_switches = []
_scanned = -1e9
_inside = set()  # (switch id, hand) of hands that touched and have not left yet
_reported = set()  # ids of nearby objects already listed
_closest = {}  # (switch id, hand) -> closest palm distance of this approach
_ready = {}  # (switch id, hand) -> time it may flip again
_keys = None


def _log(message):
    global _logs
    if native.setting_flag('lightswitch', 'log', True):
        _logs += 1
        if _logs <= 300:
            native._log('lightswitch: ' + message)


def _note_error(what, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('lightswitch: {} failed: {!r}'.format(what, e))


def _number(key, default):
    try:
        return float(native.setting('lightswitch', key, default))
    except (TypeError, ValueError):
        return default


def _dist(a, p):
    """Metres from the point a to the position p (the game's Python has no math.dist)."""
    return math.sqrt((a[0] - p.x) ** 2 + (a[1] - p.y) ** 2 + (a[2] - p.z) ** 2)


def _first_person(sim):
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def start():
    global _alarm, _scanned
    _scanned = -1e9
    _hook_interactions()
    if _alarm is None and native.setting_flag('lightswitch', 'on', True):
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
        _log('on')


def stop():
    global _alarm
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    _switches[:] = []
    _inside.clear()
    _closest.clear()
    _ready.clear()


# --- finding switches -------------------------------------------------------------------

def _name_keys():
    global _keys
    if _keys is None:
        _keys = set(SWITCH_NAME_KEYS)
        for text in str(native.setting('lightswitch', 'name_keys', '') or '').split(','):
            try:
                if text.strip():
                    _keys.add(int(text.strip(), 0))
            except ValueError:
                _log('bad name key {!r}'.format(text))
    return _keys


NAME_WORDS = ('lightswitch', 'light_switch', 'switch', 'dimmer')


def _is_switch(obj):
    """Any object that is a light switch: a model name with switch/dimmer in it, one of the catalog
    names, or an object that offers the game's own room-light interaction but is not a light itself."""
    try:
        if getattr(obj, 'is_sim', False):
            return False
        name = str(getattr(obj.definition, 'name', '') or '').lower()
        if any(word in name for word in NAME_WORDS) and not obj.is_lighting_enabled():
            return True  # model names sculptWall_SP39GENlightswitch_set1 ...
        from build_buy import get_object_catalog_name
        if (get_object_catalog_name(obj.definition.id) or 0) in _name_keys():
            return True
        if not obj.is_lighting_enabled() and _room_affordance(obj, True) is not None:
            return True
    except Exception:
        pass
    return False


def _nearby_report(sim):
    """Log once each wall-ish object within 2.5 m of your head (what a switch might be called)."""
    head = sim.position
    for obj in list(services.object_manager().values()):
        try:
            if getattr(obj, 'is_sim', False) or obj.id in _reported:
                continue
            if _dist((head.x, head.y, head.z), obj.position) > 2.5 or obj.level != sim.level:
                continue
            name = str(getattr(obj.definition, 'name', '') or '')
            low = name.lower()
            if not any(word in low for word in ('wall', 'switch', 'sculpt', 'light', 'lamp', 'plug', 'socket', 'outlet', 'dimmer')):
                continue
            from build_buy import get_object_catalog_name
            _reported.add(obj.id)
            if len(_reported) <= 60:
                _log('near you: {} {} catalog {:#x} ({:.2f}, {:.2f}, {:.2f}) {}'.format(
                    name, obj.id, get_object_catalog_name(obj.definition.id) or 0,
                    obj.position.x, obj.position.y, obj.position.z,
                    'light' if obj.is_lighting_enabled() else ''))
        except Exception:
            pass


def _scan():
    found = [obj for obj in list(services.object_manager().values()) if _is_switch(obj)]
    if [o.id for o in found] != [o.id for o in _switches]:
        _log('{} light switch(es) on the lot: {}'.format(
            len(found), ['{} {} ({:.1f}, {:.1f}, {:.1f})'.format(
                getattr(o.definition, 'name', '?'), o.id, o.position.x, o.position.y, o.position.z) for o in found[:12]]))
    _switches[:] = found


# --- flipping the room's lights ----------------------------------------------------------

def _lights_in_room(zone_id, room_id, level):
    from build_buy import get_object_has_tag, get_room_id
    from objects.components.types import LIGHTING_COMPONENT
    from objects.components.lighting_component import LightingComponent
    found = []
    manual_too = native.setting_flag('lightswitch', 'include_manual', True)
    for obj in services.object_manager().get_all_objects_with_component_gen(LIGHTING_COMPONENT):
        if not manual_too and get_object_has_tag(obj.definition.id, LightingComponent.MANUAL_LIGHT_TAG):
            continue
        if get_room_id(zone_id, obj.position, obj.level) == room_id and obj.level == level:
            found.append(obj)
    return found


def _room_lights(switch, sim=None):
    """(lights, own) of the room the switch is in. A wall switch sits on the wall between two rooms, so
    when its own spot gives none, rooms 0.5 m either side of it and your own room are tried
    (own False then: the switch's own affordance would not match)."""
    from build_buy import get_room_id
    zone_id = services.current_zone_id()
    p, level = switch.position, switch.level
    lights = [light for light in _lights_in_room(zone_id, get_room_id(zone_id, p, level), level)
              if light is not switch and light.is_lighting_enabled()]
    if lights:
        return lights, True
    tried = {get_room_id(zone_id, p, level)}
    spots = []
    if sim is not None:
        spots.append(sim.position)
    import sims4.math
    spots += [sims4.math.Vector3(p.x + dx, p.y, p.z + dz) for dx, dz in ((0.5, 0), (-0.5, 0), (0, 0.5), (0, -0.5))]
    for spot in spots:
        room = get_room_id(zone_id, spot, level)
        if room in tried:
            continue
        tried.add(room)
        lights = [light for light in _lights_in_room(zone_id, room, level)
                  if light is not switch and light.is_lighting_enabled()]
        if lights:
            _log('switch {}: its own spot has no lights, using room {} next to it'.format(switch.id, room))
            return lights, False
    _log('switch {}: rooms tried {}, none with lights'.format(switch.id, sorted(tried)))
    return [], True


def _lit(light):
    try:
        return light.get_light_dimmer_value() != 0
    except Exception:
        return False


def _room_affordance(switch, want_on):
    """The switch's own tuned room-light affordance that sets the dimmer you want, or None."""
    try:
        from objects.lighting.lighting_interactions import SwitchLightImmediateInteraction
        from objects.lighting.lighting_utils import _LightTargetRoom, _DimmerValueSpecified
        for affordance in switch.super_affordances():
            if not (isinstance(affordance, type) and issubclass(affordance, SwitchLightImmediateInteraction)):
                continue
            op = getattr(affordance, 'lighting_setting_operation', None)
            settings = getattr(op, 'lighting_settings', None)
            if settings is None or not isinstance(settings.light_target, _LightTargetRoom):
                continue
            dimmer = settings.dimmer_value
            if isinstance(dimmer, _DimmerValueSpecified) and (dimmer.dimmer_value > 0) == want_on:
                return affordance
    except Exception as e:
        _note_error('affordance lookup', e)
    return None


def _run_affordance(sim, switch, affordance):
    from interactions.aop import AffordanceObjectPair
    from interactions.context import InteractionContext, QueueInsertStrategy
    from interactions.priority import Priority
    context = InteractionContext(sim, InteractionContext.SOURCE_PIE_MENU, Priority.High,
                                 insert_strategy=QueueInsertStrategy.NEXT)
    aop = AffordanceObjectPair(affordance, switch, affordance, None)
    tested = aop.test(context)
    if not tested:
        return 'test: {}'.format(getattr(tested, 'reason', tested))
    result = AffordanceObjectPair.execute_interaction(aop.interaction_factory(context).interaction)
    return None if result else 'not queued: {}'.format(result)


def _click(snap, palm):
    """The click, from a place in the world, in the headset's frame."""
    head, fwd, up = snap[0], snap[1], snap[2]
    right = (fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2], fwd[0] * up[1] - fwd[1] * up[0])
    d = [palm[i] - head[i] for i in range(3)]
    return native.switch_click((sum(d[i] * right[i] for i in range(3)),
                                sum(d[i] * up[i] for i in range(3)),
                                -sum(d[i] * fwd[i] for i in range(3))))


_touch_click = -1e9  # when a touch last clicked: its queued affordance must not click again
_hooked = False


_last_click = -1e9


def _click_for(interaction, where):  # (a hook on _run_interaction_gen broke every light pick: the game rejects a wrapped generator)
    """Your own on/off light interaction (from afar, the pie menu, the remote): the click, in 3D at the light."""
    global _last_click
    try:
        now = time.monotonic()
        if now - _touch_click < 2.0 or now - _last_click < 0.5 or not _first_person(getattr(interaction, 'sim', None)):
            return
        settings = getattr(getattr(interaction, 'lighting_setting_operation', None), 'lighting_settings', None)
        target = interaction.target
        if settings is None or target is None:
            return
        dimmer = settings.dimmer_value.get_dimmer_value(target)
        if dimmer == AUTO_DIMMER:
            return
        snap = native.hands()
        p = target.position
        _last_click = now
        played = snap is not None and _click(snap, (p.x, p.y, p.z))
        _log('click from {}: {} on {} ({}): {}'.format(
            where, type(interaction).__name__, target.id, 'on' if dimmer else 'off', 'played' if played else 'not played'))
    except Exception as e:
        _note_error('click', e)


def _append_with_click(original, self, interaction, *args, **kwargs):
    """The interaction going into your queue: clicks when it is a light on/off, whichever way it runs."""
    try:
        from objects.lighting.lighting_interactions import SwitchLightImmediateInteraction
        if isinstance(interaction, SwitchLightImmediateInteraction):
            _click_for(interaction, 'queue')
    except Exception as e:
        _note_error('queue click', e)
    return original(self, interaction, *args, **kwargs)


def _hook_interactions():
    global _hooked
    if _hooked:
        return
    _hooked = True
    try:
        from sims4vr import hooks
        from interactions.interaction_queue import InteractionQueue
        hooks.hook('lightswitch', InteractionQueue, 'append', _append_with_click, first_person_only=True)
    except Exception as e:
        _note_error('click hook', e)


def _set_manual(lights, want_on):
    """Lights the game's own room option skips (MANUAL_LIGHT_TAG: most table and floor lamps) set directly."""
    try:
        from build_buy import get_object_has_tag
        from objects.components.lighting_component import LightingComponent
        for light in lights:
            if get_object_has_tag(light.definition.id, LightingComponent.MANUAL_LIGHT_TAG):
                light.set_light_dimmer_value(1.0 if want_on else 0.0)
    except Exception as e:
        _note_error('manual lights', e)


def _flip(sim, switch):
    lights, own = _room_lights(switch, sim)
    if not lights:
        _log('{} {}: no lights in its room'.format(getattr(switch.definition, 'name', '?'), switch.id))
        return False
    want_on = not any(_lit(light) for light in lights)
    how = 'direct'
    affordance = _room_affordance(switch, want_on) if own else None
    if affordance is not None:
        failed = _run_affordance(sim, switch, affordance)
        if failed is None:
            _set_manual(lights, want_on)  # the game's room option leaves lamps tagged manual alone
        how = affordance.__name__ if failed is None else 'direct (affordance {}: {})'.format(affordance.__name__, failed)
        if failed is None:
            _log('{} {} -> {} via {} ({} light(s))'.format(
                getattr(switch.definition, 'name', '?'), switch.id, 'on' if want_on else 'off', how, len(lights)))
            return True
    for light in lights:
        light.set_light_dimmer_value(1.0 if want_on else 0.0)
    _log('{} {} -> {} via {} ({} light(s))'.format(
        getattr(switch.definition, 'name', '?'), switch.id, 'on' if want_on else 'off', how, len(lights)))
    return True


# --- touching ---------------------------------------------------------------------------

@native.timed('light switch tick', 0.004)
def _tick(_handle):
    global _scanned, _touch_click
    try:
        sim = services.get_active_sim()
        if not _first_person(sim):
            _inside.clear()
            return
        now = time.monotonic()
        if now - _scanned >= SCAN_EVERY:
            _scanned = now
            _scan()
            if native.setting_flag('lightswitch', 'log', True):
                _nearby_report(sim)
        if not _switches:
            return
        snap = native.hands()
        if snap is None:
            return
        head = snap[0]
        radius, cooldown = _number('touch_radius', 0.15), _number('cooldown', 0.8)
        leave = radius * LEAVE_FACTOR
        for switch in _switches:
            try:
                if switch.level != sim.level:
                    continue
                p = switch.position
                if _dist(head, p) > NEAR:
                    continue
                for hand in (0, 1):
                    palm = snap[3 + hand]
                    key = (switch.id, hand)
                    if not palm[0]:
                        continue
                    d = _dist(palm[1], p)
                    if not math.isfinite(d):
                        continue
                    if d <= APPROACH:  # log the closest a palm got to a switch, to tune touch_radius
                        _closest[key] = min(d, _closest.get(key, d))
                    elif key in _closest:
                        _log('palm {} came within {:.2f} m of switch {} (touch at {:.2f} m)'.format(
                            hand, _closest.pop(key), switch.id, radius))
                    if d > leave:
                        _inside.discard(key)
                    elif d <= radius and key not in _inside and now >= _ready.get(key, 0.0):
                        _inside.add(key)  # one touch = one flip: the hand must leave before it counts again
                        _ready[key] = now + cooldown
                        if _flip(sim, switch):
                            _touch_click = now
                            _click(snap, palm[1])
                            if native.setting_flag('lightswitch', 'haptics', True):
                                native.haptic(hand, 0.6, 0.08)
            except Exception as e:
                _note_error('switch {}'.format(switch), e)
    except Exception as e:
        _note_error('tick', e)
