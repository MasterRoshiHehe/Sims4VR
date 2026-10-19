"""Your Sim's phone in first person, the start of the phone as the menu hub.

Close your right hand at your right hip and your Sim holds its phone in that
hand; open the hand and the phone goes away. While it is out the phone's
screen shows the game's own phone menu (phoneapp.py; the DLL draws it), and
its Options app the game menu (the Escape menu; the DLL opens it and shows it
on the phone); the game keeps running.

The DLL (vr.cpp) sees the gesture and sends control events 3 (out) and 4
(away); controls.py passes them here. This module:
- makes the game's own cell phone prop (for mirrors and other Sims; [phone]
  game_phone) and hangs it off the Sim's right hand joint inside the phone the
  DLL draws (phone_mount), its longest side up and its thinnest across;
- wraps GameClock.push_speed: a menu's pause request (a UI modal pause) gets a
  validity check that is false while the phone is out, so the game skips it
  and runs on; put the phone away and the pause counts again if the menu is
  still open;
- tells the DLL whether a UI modal (the menu) is open, so it knows whether
  putting the phone away has to close the menu;
- tells the DLL whether the game is showing a popup (a modal dialog, from
  UiDialogService), so your phone buzzes and shows it when you take it out.
Switch: [phone] phone in sims4vr.ini.
"""
import services
import sims4.math

from clock import ClockSpeedMode, GameClock, GameSpeedChangeSource
from objects.system import create_prop

from sims4vr import native

PHONE_DEFINITIONS = (0x285C, 0x23819)  # the cell phone prop; a newer one if that is missing
HAND_JOINT = 'b__R_Hand__'

_out = False
_phone = None
_sim = None
_logs = 0
_modal_open = None
_popup_open = None


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 200:
        native._log('phone: ' + message)


def _joint_hash(name):
    h = 0x811c9dc5
    for c in name.lower().encode('ascii'):
        h = ((h * 0x1000193) & 0xffffffff) ^ c
    return h


def _setting_floats(key, default):
    text = native.setting('phone', key, None)
    try:
        values = tuple(float(v) for v in text.replace(',', ' ').split())
        if len(values) == len(default):
            return values
    except Exception:
        pass
    return default


def _quaternion_from_degrees(x, y, z):
    """Rotation about X, then Y, then Z (degrees), as x, y, z, w."""
    import math
    hx, hy, hz = math.radians(x) / 2, math.radians(y) / 2, math.radians(z) / 2
    qx = (math.sin(hx), 0.0, 0.0, math.cos(hx))
    qy = (0.0, math.sin(hy), 0.0, math.cos(hy))
    qz = (0.0, 0.0, math.sin(hz), math.cos(hz))

    def mul(a, b):
        ax, ay, az, aw = a
        bx, by, bz, bw = b
        return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)
    return mul(qz, mul(qy, qx))


def is_out():
    return _out


def take_out():
    global _out
    if _out:
        return
    _out = True
    _refresh_clock()
    _make_prop()
    try:
        from sims4vr import phoneapp
        phoneapp.send()
    except Exception as e:
        _log('the phone menu failed: {!r}'.format(e))


def _mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _rotate(q, v):
    x, y, z, w = q
    r = _mul(_mul(q, (v[0], v[1], v[2], 0.0)), (-x, -y, -z, w))
    return r[:3]


def _from_rows(rows):
    """The rotation whose matrix has these rows (each a unit axis), as x, y, z, w."""
    import math
    (m00, m01, m02), (m10, m11, m12), (m20, m21, m22) = rows
    t = m00 + m11 + m22
    if t > 0:
        k = math.sqrt(t + 1) * 2
        return ((m21 - m12) / k, (m02 - m20) / k, (m10 - m01) / k, k / 4)
    if m00 > m11 and m00 > m22:
        k = math.sqrt(1 + m00 - m11 - m22) * 2
        return (k / 4, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k)
    if m11 > m22:
        k = math.sqrt(1 + m11 - m00 - m22) * 2
        return ((m01 + m10) / k, k / 4, (m12 + m21) / k, (m02 - m20) / k)
    k = math.sqrt(1 + m22 - m00 - m11) * 2
    return ((m02 + m20) / k, (m12 + m21) / k, k / 4, (m10 - m01) / k)


def _model_bounds(phone):
    """The phone model's bounding box (min xyz, max xyz) from its MODL, or None."""
    import struct
    try:
        import sims4.resources
        key = phone.definition.get_model(0)
        data = bytes(sims4.resources.load(key))
    except Exception as e:
        _log('no model for the phone: {!r}'.format(e))
        return None
    at = data.find(b'MODL')
    while at >= 0:
        for skip in (8, 12):
            if at + skip + 24 <= len(data):
                box = struct.unpack_from('<6f', data, at + skip)
                size = [box[3 + i] - box[i] for i in range(3)]
                if all(0.001 < v < 1.0 for v in size):
                    return box
        at = data.find(b'MODL', at + 4)
    _log('no bounding box in the phone model ({} bytes)'.format(len(data)))
    return None


def _placement(phone):
    """Where the game's phone goes on the hand joint: inside our drawn phone,
    its longest side up and its thinnest across the screen."""
    mount = native.phone_mount()
    if mount is None:
        return None
    (mx, my, mz), mq = mount
    box = _model_bounds(phone)
    if box is None:
        rows, centre = ((1, 0, 0), (0, 1, 0), (0, 0, 1)), (0.0, 0.0, 0.0)
    else:
        size = [box[3 + i] - box[i] for i in range(3)]
        centre = tuple((box[i] + box[3 + i]) / 2 for i in range(3))
        order = sorted(range(3), key=lambda i: size[i])  # thinnest, middle, longest
        axis = lambda i: tuple(1.0 if j == i else 0.0 for j in range(3))
        x, y, z = axis(order[1]), axis(order[2]), axis(order[0])
        # Right-handed: x = y cross z.
        cross = (y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0])
        rows = (cross, y, z)
        native.set_phone_prop(size[order[1]], size[order[2]], size[order[0]])
        _log('phone model box {} (centre {}, size {})'.format(
            tuple(round(v, 4) for v in box), tuple(round(v, 4) for v in centre), tuple(round(v, 4) for v in size)))
    model_to_phone = _from_rows(rows)
    turn = _quaternion_from_degrees(*_setting_floats('prop_rotation', (0.0, 0.0, 0.0)))
    model_to_phone = _mul(turn, model_to_phone)
    shift = _setting_floats('prop_offset', (0.0, 0.0, 0.0))
    inside = _rotate(model_to_phone, centre)
    in_phone = tuple(shift[i] - inside[i] for i in range(3))
    offset = _rotate(mq, in_phone)
    position = (mx + offset[0], my + offset[1], mz + offset[2])
    return position, _mul(mq, model_to_phone)


def _make_prop():
    global _phone, _sim
    if _phone is not None:
        return
    if not native.setting_flag('phone', 'game_phone', False):
        return
    sim = services.get_active_sim()
    if sim is None:
        _log('out, but there is no active Sim to hold it')
        return
    phone = None
    for definition in PHONE_DEFINITIONS:
        try:
            phone = create_prop(definition)
        except Exception as e:
            _log('making phone {:#x} failed: {!r}'.format(definition, e))
            phone = None
        if phone is not None:
            break
    if phone is None:
        _log('out (no phone prop: the menu still shows)')
        return
    placed = _placement(phone)
    if placed is None:
        _log('no hand frame from the DLL yet; the game phone sits on the joint as is')
        placed = ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))
    position, rotation = placed
    try:
        phone.set_parent(sim, sims4.math.Transform(sims4.math.Vector3(*position), sims4.math.Quaternion(*rotation)),
                         joint_name_or_hash=_joint_hash(HAND_JOINT))
    except Exception as e:
        _log('putting the phone in the hand failed: {!r}'.format(e))
        try:
            phone.destroy(source=phone, cause='Sims4VR phone: no hand')
        except Exception:
            pass
        return
    _phone, _sim = phone, sim
    _log('out: {} in {} (position {}, rotation {})'.format(
        phone.definition.id, sim, tuple(round(v, 4) for v in position), tuple(round(v, 4) for v in rotation)))


def _remove_prop():
    global _phone, _sim
    phone, _phone, _sim = _phone, None, None
    if phone is not None:
        try:
            phone.destroy(source=phone, cause='Sims4VR phone put away')
        except Exception as e:
            _log('putting the phone away failed: {!r}'.format(e))


def put_away(why='hand opened'):
    global _out
    was_out = _out
    _out = False
    _remove_prop()
    if was_out:
        try:
            from sims4vr import phoneapp
            phoneapp.clear()
        except Exception as e:
            _log('clearing the phone menu failed: {!r}'.format(e))
        _refresh_clock()
        _log('away ({})'.format(why))


def before_save():
    """The prop never goes into a save; the phone stays out."""
    _remove_prop()


def after_save():
    if _out:
        _make_prop()


# --- the menu's pause ---------------------------------------------------------

def _refresh_clock():
    try:
        services.game_clock_service()._update_speed(immediate=True)
    except Exception as e:
        _log('clock refresh failed: {!r}'.format(e))


def _report_modal(clock_service):
    global _modal_open
    try:
        open_ = bool(clock_service.speed_controllers.get(GameSpeedChangeSource.UI_MODAL))
    except Exception:
        return
    if open_ != _modal_open:
        _modal_open = open_
        native.set_game_menu(open_)


def _wrap_clock():
    original_push = GameClock.push_speed
    original_pop = GameClock.pop_speed
    original_remove = GameClock.remove_request

    def push_speed(self, speed, source=GameSpeedChangeSource.GAMEPLAY, validity_check=None, reason='',
                   immediate=False):
        if speed == ClockSpeedMode.PAUSED and source == GameSpeedChangeSource.UI_MODAL:
            check = validity_check

            def not_while_phone_out():
                return not _out and (check is None or check())
            validity_check = not_while_phone_out
            if _out:
                _log('kept the game running: menu pause "{}"'.format(reason))
        elif speed == ClockSpeedMode.PAUSED and _out:
            _log('a pause while the phone is out, not from a menu: source {}, "{}"'.format(source, reason))
        request = original_push(self, speed, source=source, validity_check=validity_check, reason=reason,
                                immediate=immediate)
        if source == GameSpeedChangeSource.UI_MODAL:
            _report_modal(self)
        return request

    def pop_speed(self, speed=None, source=GameSpeedChangeSource.GAMEPLAY, reason='', immediate=False):
        request = original_pop(self, speed=speed, source=source, reason=reason, immediate=immediate)
        if source == GameSpeedChangeSource.UI_MODAL:
            _report_modal(self)
        return request

    def remove_request(self, request, source=GameSpeedChangeSource.GAMEPLAY, reason='', immediate=False):
        result = original_remove(self, request, source=source, reason=reason, immediate=immediate)
        if source == GameSpeedChangeSource.UI_MODAL:
            _report_modal(self)
        return result

    GameClock.push_speed = push_speed
    GameClock.pop_speed = pop_speed
    GameClock.remove_request = remove_request


try:
    _wrap_clock()
except Exception as e:
    native._log('phone: wrapping the game clock failed: {!r}'.format(e))


# --- the game's popups ----------------------------------------------------------

def _report_popup(service):
    global _popup_open
    try:
        open_ = bool(service.has_active_modal_dialogs())
    except Exception as e:
        _log('popup check failed: {!r}'.format(e))
        return
    if open_ != _popup_open:
        _popup_open = open_
        _log('a game popup is {}'.format('open' if open_ else 'gone'))
        native.set_popup(open_)


def _wrap_dialogs():
    from ui.ui_dialog_service import UiDialogService

    def wrap(name):
        original = getattr(UiDialogService, name)

        def wrapped(self, *args, **kwargs):
            try:
                return original(self, *args, **kwargs)
            finally:
                _report_popup(self)
        wrapped.__name__ = name
        setattr(UiDialogService, name, wrapped)

    for name in ('dialog_show', 'dialog_respond', 'dialog_cancel', '_dialog_cancel_internal',
                 'disable_on_teardown'):
        if hasattr(UiDialogService, name):
            wrap(name)


try:
    _wrap_dialogs()
except Exception as e:
    native._log('phone: wrapping the game popups failed: {!r}'.format(e))


# The game clock paused or running, for the pause symbol in first person
# (vr.cpp). The game sets GameClock._clock_speed in _update_speed whenever a
# speed request changes (simulation/clock.py), so it is read back from there.
_paused = None


def _wrap_clock():
    from clock import GameClock, ClockSpeedMode
    original = GameClock._update_speed

    def _update_speed(self, *args, **kwargs):
        try:
            return original(self, *args, **kwargs)
        finally:
            global _paused
            try:
                paused = self._clock_speed == ClockSpeedMode.PAUSED
                if paused != _paused:
                    _paused = paused
                    native.set_paused(paused)
            except Exception:
                pass
    _update_speed.__name__ = '_update_speed'
    GameClock._update_speed = _update_speed


try:
    _wrap_clock()
except Exception as e:
    native._log('phone: wrapping the game clock failed: {!r}'.format(e))
