"""First person: switch the game's auto lights off for you alone (user 2026-10-29).

Lights set to Auto turn on when a Sim is in their room. Other Sims can't use your light switches, so
you want them to keep that, but not light a room only you are in. Every light's pie menu (in the
game's lights category) gets, for each scope the game's own light settings have (this lamp, this room,
all lights as a sub pie: All, Interiors, Exteriors), "Auto lights: ignore me (scope)" or, once set, "Auto lights: include me (scope)".

Ignoring: every ALARM_SECONDS the auto lights (dimmer LIGHT_AUTOMATION_DIMMER_VALUE, -1) in every
ignoring room with no other Sim are set to 0 (pre-held, so entering a room doesn't flash them on; the
game's automation is client side), and set back to auto when another Sim comes in, you switch the
option off or leave first person. A light you (or the light
switch) changed in the meantime is left alone. Only lights this module turned off are touched.
Switch: [lights] auto_ignore_me (starting state, default 0). Log: "autolight: ..." lines.
"""
import time

import alarms
import clock
import services

from sims4vr import follow, native

ALARM_SECONDS = 0.25
SCOPES = ("this lamp", "this room", "all lights", "interior lights", "exterior lights")  # index = GUID offset
# Scopes 2-4 are one sub pie each way: "Auto lights: ignore me (all lights)" / "include me": All, Interiors, Exteriors.
SUB_LABELS = {2: 'All', 3: 'Interiors', 4: 'Exteriors'}
SUB_CATEGORY = {False: 0x5653414C49470101, True: 0x5653414C49470102}  # ignore menu, include menu
SUB_NAMES = {False: 'Auto lights: ignore me (all lights)', True: 'Auto lights: include me (all lights)'}
GUID_BASE = 0x5653414C49470000  # + scope for ignore, + 16 + scope for include
AUTO = -1

_owner = type('_Owner', (), {})()
_alarm = None
_all = None  # all lights ignore you: None until first person starts, then [lights] auto_ignore_me
_inside = False  # all interior lights ignore you
_outside = False  # all exterior lights ignore you
_rooms = set()  # (room id, level) of rooms whose lights ignore you
_lamps = set()  # ids of lamps that ignore you
_held = {}  # light id -> light, set to 0 by us
_toggle = None  # "Turn on/off" on a light switch, from wherever you are
_aff = None  # [scope] -> (ignore, include)
_wrapped = False
_logs = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 120:
        native._log('autolight: ' + message)


def _is_light(obj):
    try:
        return obj is not None and not getattr(obj, 'is_sim', False) and obj.is_lighting_enabled()
    except Exception:
        return False


def _is_switch(obj):
    """A light switch (lightswitch.py finds them): clicked with the laser it gets the same options."""
    try:
        from sims4vr import lightswitch
        return obj is not None and any(s.id == obj.id for s in lightswitch._switches)
    except Exception:
        return False


def _is_target(obj):
    return _is_light(obj) or _is_switch(obj)


def _room_of(obj):
    from build_buy import get_room_id
    if _is_switch(obj):  # the room of the lights it flips (it sits on a wall between two rooms)
        try:
            from sims4vr import lightswitch
            lights, _own = lightswitch._room_lights(obj, follow._fp_sim())
            if lights:
                obj = lights[0]
        except Exception:
            pass
    return (get_room_id(services.current_zone_id(), obj.position, obj.level), obj.level)


def _scope_on(scope, target):
    if scope == 0:
        return target.id in _lamps
    if scope == 1:
        return _room_of(target) in _rooms
    if scope == 3:
        return _inside
    if scope == 4:
        return _outside
    return bool(_all)


def _set_scope(scope, target, value):
    global _all, _inside, _outside
    if scope == 0:
        (_lamps.add if value else _lamps.discard)(target.id)
    elif scope == 1:
        (_rooms.add if value else _rooms.discard)(_room_of(target))
    elif scope == 3:
        _inside = bool(value)
    elif scope == 4:
        _outside = bool(value)
    else:
        _all = bool(value)
    _log('auto lights ignore you ({}): {}'.format(SCOPES[scope], bool(value)))


def _is_outside(light):
    try:
        import build_buy
        return bool(build_buy.is_location_outside(light.position, light.level))
    except Exception:
        return False


def _ignored(light, room):
    if _all or room in _rooms or light.id in _lamps:
        return True
    if _inside or _outside:
        return _outside if _is_outside(light) else _inside
    return False


def _release(only=None):
    for light_id, light in list(_held.items()):
        if only is not None and light_id not in only:
            continue
        del _held[light_id]
        try:
            if light.get_light_dimmer_value() == 0:  # still how we left it
                light.set_light_dimmer_value(AUTO)
        except Exception as e:
            _log('giving light {} back failed: {!r}'.format(light_id, e))


def _tick(_handle):
    try:
        sim = follow._fp_sim()
        if sim is None or not (_all or _inside or _outside or _rooms or _lamps):
            if _held:
                _release()
            return
        from build_buy import get_room_id
        from objects.components.types import LIGHTING_COMPONENT
        zone_id = services.current_zone_id()
        mine = (get_room_id(zone_id, sim.position, sim.level), sim.level)
        others = set()
        for other in services.sim_info_manager().instanced_sims_gen():
            if other is not sim and getattr(other, 'is_on_active_lot', lambda: True)():
                others.add((get_room_id(zone_id, other.position, other.level), other.level))
        busy = others
        # The game lights auto lights client side the moment a Sim enters, so waiting until you are in
        # the room flashes them on. Hold every ignored auto light in any room no other Sim is in (an
        # empty room's auto lights are off anyway); give back when another Sim is in that room.
        back = set()
        for light_id, light in _held.items():
            room = (get_room_id(zone_id, light.position, light.level), light.level)
            if room in busy or not _ignored(light, room):
                back.add(light_id)
        if back:
            _release(back)
        for light in services.object_manager().get_all_objects_with_component_gen(LIGHTING_COMPONENT):
            if light.id in _held or light.get_light_dimmer_value() != AUTO:
                continue
            room = (get_room_id(zone_id, light.position, light.level), light.level)
            if room in busy or not _ignored(light, room):
                continue
            _held[light.id] = light
            light.set_light_dimmer_value(0)
            _log('light {} held off, no other Sim in room {}'.format(light.id, room[0]))
    except Exception as e:
        _log('tick failed: {!r}'.format(e))


def _wrap_object_affordances():
    from objects.game_object import GameObject
    from sims4.utils import flexmethod
    raw = next((k.__dict__['super_affordances'] for k in GameObject.__mro__ if 'super_affordances' in k.__dict__), None)
    if raw is None:
        _log('GameObject.super_affordances not found; no auto light option')
        return False

    @flexmethod
    def super_affordances(cls, inst, *args, **kwargs):
        yield from raw.__get__(inst, cls)(*args, **kwargs)
        try:
            if inst is None or not _aff:
                return
            context = args[0] if args else kwargs.get('context')
            from sims4vr import body
            fp = body.following()
            # The Sim you are in is the only one offered these, so every other Sim's autonomy (which asks
            # every object) stops before the per-object light/switch checks (perf, 2026-11-03 profile).
            if not fp or getattr(getattr(context, 'sim', None), 'id', None) != fp:
                return
            if not (hasattr(inst, 'get_light_dimmer_value') or _is_switch(inst)):
                return
            if _toggle is not None and _is_switch(inst):
                yield _toggle
            for scope in range(5):
                yield _aff[scope][1 if _scope_on(scope, inst) else 0]
        except Exception:
            pass

    GameObject.super_affordances = super_affordances
    return True


def _lights_category():
    """The pie category of the game's own light options (Set to Auto, This Room ...), or None."""
    try:
        import sims4.resources
        manager = services.get_instance_manager(sims4.resources.Types.PIE_MENU_CATEGORY)
        for name in ('LightsParentCategory', 'AllLights'):
            for cls in list(manager.types.values()):
                if cls.__name__ == name:
                    return cls
    except Exception as e:
        _log('lights category lookup failed: {!r}'.format(e))
    return None


class _Category:
    def __init__(self, key):
        self.guid64 = key


def _flip_switch(switch):
    from sims4vr import lightswitch
    lightswitch._flip(follow._fp_sim(), switch)


def _build(scope):
    label = SCOPES[scope]
    sub = SUB_LABELS.get(scope)
    ignore = follow._make_affordance(
        sub or 'Auto lights: ignore me ({})'.format(label), GUID_BASE + scope,
        lambda t, you: _is_target(t) and (scope or _is_light(t)) and not _scope_on(scope, t), lambda t: _set_scope(scope, t, True))
    include = follow._make_affordance(
        sub or 'Auto lights: include me ({})'.format(label), GUID_BASE + 16 + scope,
        lambda t, you: _is_target(t) and (scope or _is_light(t)) and _scope_on(scope, t), lambda t: _set_scope(scope, t, False))
    if ignore is None or include is None:
        return None
    if sub:  # in our own sub pie (pie.py custom_categories)
        for aff, included in ((ignore, False), (include, True)):
            key = SUB_CATEGORY[included]
            aff.category = _Category(key)
            aff.get_pie_menu_category = classmethod(lambda cls, _c=_Category(key), **kw: _c)
    return ignore, include


def start():
    global _alarm, _all, _aff, _wrapped, _toggle
    if _all is None:
        _all = native.setting_flag('lights', 'auto_ignore_me', False)
    if not _wrapped:
        _wrapped = True
        try:
            try:
                from sims4vr import pie
                for included, key in SUB_CATEGORY.items():
                    pie.custom_categories[key] = SUB_NAMES[included]
                    pie.custom_category_icons[key] = pie.VR_ICON_ID
            except Exception as e:
                _log('sub pie names not made: {!r}'.format(e))
            built = [_build(scope) for scope in range(5)]
            if all(built):
                category = _lights_category()
                for scope, pair in enumerate(built):
                    for aff in pair:
                        if category is not None and scope not in SUB_LABELS:
                            aff.category = category
                _aff = built
                _toggle = follow._make_affordance(
                    'Turn on/off', GUID_BASE + 32, lambda t, you: _is_switch(t), _flip_switch)
                if _toggle is not None:
                    _toggle.vr_from_afar = True  # reach.py lets it through
                _log('pie options built (category {})'.format(getattr(category, '__name__', None)))
                _wrap_object_affordances()
        except Exception as e:
            _log('no auto light option: {!r}'.format(e))
    if _alarm is None:
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(ALARM_SECONDS), _tick, repeating=True)


def stop():
    global _alarm
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    _release()
