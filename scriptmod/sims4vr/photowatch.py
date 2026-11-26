"""Probe: logs the game's photo / selfie session start and the camera updates after it.

Wraps _BasePhotoMode.create_take_photo_op (the server builds the TAKE_PHOTO op
the client turns into its photography camera). No behaviour change. Log lines
start with "Photo probe:".
"""
import json
import time

import camera
from crafting.take_photo import TakePhoto, _BasePhotoMode
import services
from event_testing.event_manager_service import EventManagerService
from event_testing import test_events

from sims4vr import hooks, native

_after = 0  # camera.update calls still to log after a photo op
_selfie = False  # the session's op is a selfie (the phone lens looks out the glass)


def _create(original, self, sims, interaction, *args, **kwargs):
    global _after, _selfie
    _selfie = type(self).__name__ == '_SelfiePhotoMode'
    try:
        native._log('Photo probe: op {} mode {} sims {} interaction {} zoom {} orient {} hide_photographer {} bone {!r} number {}'.format(
            type(self).__name__, self._get_camera_mode(), [getattr(s, 'sim_id', s) for s in sims],
            type(interaction).__name__, self.zoom_capability, self.orientation, self.hide_photographer,
            self.camera_position_bone_name, self.number_of_photos_override))
        _after = 40
        native.photo_probe(600)
    except Exception as e:
        native._log('Photo probe: op log failed {!r}'.format(e))
    return original(self, sims, interaction, *args, **kwargs)


def _update(original, *args, **kwargs):
    global _after
    if _after > 0:
        _after -= 1
        try:
            p = kwargs.get('camera_position')
            t = kwargs.get('target_position')
            native._log('Photo probe: camera.update at {:.2f} position {} target {} extra {}'.format(
                time.time(), None if p is None else (round(p.x, 3), round(p.y, 3), round(p.z, 3)),
                None if t is None else (round(t.x, 3), round(t.y, 3), round(t.z, 3)),
                sorted(k for k in kwargs if k not in ('camera_position', 'target_position'))))
        except Exception:
            pass
    return original(*args, **kwargs)


def _do_behavior(original, self, *args, **kwargs):
    # [photo] game_camera = 0 (debug menu, Phone): no photo session in first person, to test the view
    # without the game's photography camera. Default 1: the game's own session.
    try:
        if not native.setting_flag('photo', 'game_camera', True):
            native._log('Photo probe: Take Photo skipped in first person (no game photography camera) {}'.format(
                type(self.photo_mode).__name__))
            services.get_photography_service().cleanup()
            return True
    except Exception as e:
        native._log('Photo probe: skip failed {!r}'.format(e))
    result = original(self, *args, **kwargs)
    _session(True)
    return result


_session_on = False
_phones_at = 0.0
_phones_logged = 0


def _attached(sim):
    """What hangs on your Sim: its children and the props of its animations (walk.py _held_candidates finds
    the held ones the same way: the selfie's phone is in an animation context's props, not a child)."""
    found = []

    def add(obj):
        if obj is not None and all(obj is not other for other in found):
            found.append(obj)

    for child in list(getattr(sim, 'children', None) or ()):
        add(child)
    try:
        from interactions.utils.interaction_liabilities import ANIMATION_CONTEXT_LIABILITY
        contexts = []
        posture_state = sim.posture_state
        if posture_state is not None:
            for aspect in (posture_state.left, posture_state.right, posture_state.body):
                if aspect is not None and aspect.animation_context is not None:
                    contexts.append(aspect.animation_context)
        for si in list(sim.si_state):
            liability = si.get_liability(ANIMATION_CONTEXT_LIABILITY)
            if liability is not None and liability.animation_context is not None:
                contexts.append(liability.animation_context)
        for context in contexts:
            for prop in list(getattr(context, '_props', {}).values()):
                add(prop)
    except Exception as e:
        _clog('listing the animation props failed: {!r}'.format(e))
    return found


def _hide_phones():
    """The game's own phone prop in your Sim's hand (the selfie's), out of every picture: hidden, as nophone.py does
    for the props it sees (nophone never got this one). Our own phone (phone.py) stays."""
    global _phones_at, _phones_logged
    now = time.monotonic()
    if now - _phones_at < 0.25:
        return
    _phones_at = now
    from sims4vr import nophone, phone
    sim = services.get_active_sim()
    if sim is None:
        return
    from objects import VisibilityState
    ours = getattr(phone, '_phone', None)
    for child in _attached(sim):
        definition = getattr(child, 'definition', None)
        ident = getattr(definition, 'id', 0)
        name = str(getattr(definition, 'name', '') or type(child).__name__).lower()
        if _phones_logged < 12:
            _phones_logged += 1
            native._log('Photo probe: attached to your Sim: {!r} def {:#x} name {}'.format(child, ident or 0, name))
        if child is ours:
            continue
        if ident in nophone.PHONE_DEFINITIONS or 'phone' in name:
            try:
                vis = getattr(child, 'visibility', None)
                if vis is None or vis.visibility:
                    child.visibility = VisibilityState(False)
                nophone._props[id(child)] = child
            except Exception as e:
                native._log('Photo probe: hiding the game phone failed {!r}'.format(e))


def _session(on):
    # The native side draws the phone lens as a third frame while a session runs (camera.cpp photoSession).
    global _state, _filter, _size, _landscape, _session_on
    _session_on = bool(on)
    if on:
        _state, _filter, _size, _landscape = None, 0, 2, True
        _ask_state()
    try:
        native.photo_session((2 if _selfie else 1) if on else 0)
    except Exception as e:
        native._log('Photo probe: session flag failed {!r}'.format(e))


def _event(original, self, event_type, *args, **kwargs):
    if event_type is test_events.TestEvent.ExitedPhotoMode:
        _session(False)
    elif event_type is test_events.TestEvent.PhotoTaken:
        _learn(kwargs.get('res_key'))
    return original(self, event_type, *args, **kwargs)


hooks.hook('photowatch', EventManagerService, 'process_event', _event, first_person_only=False)
hooks.hook('photowatch', _BasePhotoMode, 'create_take_photo_op', _create, first_person_only=False)
hooks.hook('photowatch', camera, 'update', _update, first_person_only=False)
hooks.hook('photowatch', TakePhoto, '_do_behavior', _do_behavior, first_person_only=True)


# ---- The phone camera app (user 2026-10-31: camera mode works entirely through the phone's screen) -----------------
# The phone's glass shows the lens' picture with buttons over it (13b_build_tray.inc, hud_buildtray.inc); each button
# arrives here as control 8800000 + n (controls.py) and goes to the game's own camera services, as SimCameraMain.as
# calls them (re/allui/as/widgets/Gameplay/SimCamera): CallGameService(name, argument) through the DLL's service_query.
# n: 0 shutter, 1 shutter with flash, 2 zoom in, 3 zoom out, 4 next filter, 5 previous filter, 6 turn the frame,
# 7 close, 8..10 photo size small..large.
FILTERS = 12   # none, gray, sepia, oversaturate, undersaturate, vignette, whiteVignette, invert, warm, cool, bright, dark
_state = None  # GetPhotographyStateData's answer (dict) once it came
_state_ticket = 0
_state_at = 0.0
_filter = 0
_size = 2      # the largest photo size, always (user 2026-11-26)
_landscape = True
_shot = None   # (ticket, asked at) of the PhotographyTakePhoto in flight
_errors = 0


def _clog(message):
    global _errors
    _errors += 1
    if _errors <= 120:
        native._log('Camera app: ' + message)


def _service(name, argument=None):
    """CallGameService(name, argument) on the game's UI thread; the ticket (0: not sent)."""
    ask = native._optional('service_query')
    if ask is None:
        _clog('service_query missing in this DLL')
        return 0
    try:
        return int(ask(name, 'null' if argument is None else json.dumps(argument)) or 0)
    except Exception as e:
        _clog('{} failed: {!r}'.format(name, e))
        return 0


def _ask_state():
    global _state_ticket, _state_at
    _state_ticket = _service('GetPhotographyStateData')
    _state_at = time.monotonic()


def _stage():
    """The Flash stage the camera rectangle is in (LayoutAPI.StageWidth / StageHeight); [photo] stage_w / stage_h."""
    try:
        return float(native.setting('photo', 'stage_w', 1920)), float(native.setting('photo', 'stage_h', 1080))
    except Exception:
        return 1920.0, 1080.0


def _rect():
    """The photo's rectangle on the stage, as SimCameraMain.DrawCameraUI works it out."""
    sw, sh = _stage()
    ratio = 0.75
    if _state:
        ax, ay = float(_state.get('aspect_ratio_x') or 0), float(_state.get('aspect_ratio_y') or 0)
        if ax and ay:
            ratio = ay / ax
    long_side = 0.75
    short = long_side * ratio
    if _landscape:
        w = long_side * sw
        h = short / long_side * w
        if h > sh * 0.8:
            h = short * sh
            w = long_side / short * h
    else:
        w = short * sw
        h = long_side / short * w
        if h > sh * 0.8:
            h = long_side * sh
            w = short / long_side * h
    w, h = round(w), round(h)
    return {'x': sw * 0.5 - w * 0.5, 'y': sh * 0.5 - h * 0.5, 'width': w, 'height': h}


def control(n):
    """A button of the phone camera app."""
    global _filter, _size, _landscape, _shot
    if not native.setting_flag('photo', 'camera_app', True):
        return
    if _state is None and not _state_ticket:
        _ask_state()
    if n in (0, 1):
        if _shot is not None and time.monotonic() - _shot[1] < 3.0:
            return
        arg = _rect()
        arg.update(size=_size, flash=(n == 1), take_second_capture=False)
        if _state and int(_state.get('camera_mode', 0) or 0) == 7:  # decorator: a second, extra large capture
            arg.update(take_second_capture=True, second_size=3)
        native.photo_capture(700)  # every frame is the lens' picture while the game captures
        _shot = (_service('PhotographyTakePhoto', arg), time.monotonic())
        _clog('shutter: size {} flash {} rect {} (ticket {})'.format(_size, n == 1, arg, _shot[0]))
    elif n == 2:
        _service('CameraZoomIn', 0)
    elif n == 3:
        _service('CameraZoomOut', 0)
    elif n in (4, 5):
        _filter = (_filter + (1 if n == 4 else -1)) % FILTERS
        _service('PhotographySetFilter', {'filter': _filter})
        _clog('filter {}'.format(_filter))
    elif n == 6:
        _landscape = not _landscape
        _clog('frame {}'.format('landscape' if _landscape else 'portrait'))
    elif n == 7:
        _service('PhotographyLeaveMode')
    elif 8 <= n <= 10:
        _size = n - 8
    elif 20 <= n < 20 + FILTERS:  # the filter strip: exactly this filter
        _filter = n - 20
        _service('PhotographySetFilter', {'filter': _filter})
        _clog('filter {}'.format(_filter))
    elif n == 40:
        open_last()
    elif n in (41, 42):  # the phone turned upright / on its side (13b_build_tray.inc)
        _landscape = n == 42


def tick():
    """Reads the answers of the camera services (controls.py, every frame)."""
    global _state, _state_ticket, _shot
    if _session_on:
        try:
            _hide_phones()
        except Exception as e:
            _clog('hiding the game phone failed: {!r}'.format(e))
    if _owned:
        try:
            _supply()
        except Exception as e:
            _clog('supplying photo pictures failed: {!r}'.format(e))
    result = native._optional('service_result')
    if result is None:
        return
    if _state_ticket:
        text = result(_state_ticket)
        if text is not None:
            _state_ticket = 0
            try:
                _state = json.loads(text) if text else None
            except Exception:
                _state = None
            _clog('state {}'.format(text[:600]))
        elif time.monotonic() - _state_at > 5:
            _state_ticket = 0
    if _shot is not None:
        ticket, at = _shot
        text = result(ticket) if ticket else ''
        if text is not None:
            _shot = None
            _clog('PhotographyTakePhoto answered {}'.format(text[:300]))
            try:
                answer = json.loads(text) if text else {}
            except Exception:
                answer = {}
            if isinstance(answer, dict) and answer.get('tookPhoto'):
                # As the game's widget does after a photo it took (SimCameraMain.OnCameraClicked).
                ui = native._optional('ui_message')
                if ui is not None:
                    ui('TriggerScreenshotRequest', json.dumps({'bSuppressSound': True, 'bForceNoUI': True}))
        elif time.monotonic() - at > 5:
            _shot = None
            _clog('PhotographyTakePhoto: no answer in 5 s')


# ---- The Photos app's pictures (phone_photos.inc) ------------------------------------------------------------------
# A photo object keeps only its picture's instance (canvas_component.painting_state.texture_id). The picture itself is
# a resource the capture made (create_photo_from_photo_data gets its full key: type, instance, group). The type and
# group are learnt from every photo taken (the PhotoTaken event's res_key) and kept in sims4vr_photo_key.txt beside
# the log, so photos of earlier sessions load too; before any is known the usual picture types are tried.
_PICTURE_TYPES = (0x2F7D0004, 0x00B2D882, 0x3453CF95, 0x2F7D0002)  # PNG, DST / DDS, RLE2, JPEG
_learnt = []     # [(type, group)] newest first
_owned = {}      # texture id -> 'wait' | 'ok' | 'fail': small pictures this module hands the HUD (hud_icon)
_big_for = 0     # the texture id of the large picture asked for last
_loaded_logs = 0
_last_instance = 0   # the instance of the last photo taken (PhotoTaken)


def _key_file():
    import os
    try:
        return os.path.join(os.path.dirname(native._log_path()), 'sims4vr_photo_key.txt')
    except Exception:
        return None


def _read_learnt():
    if _learnt:
        return
    path = _key_file()
    try:
        with open(path) as f:
            for line in f:
                t, g = line.split()
                _learnt.append((int(t, 16), int(g, 16)))
    except Exception:
        pass


def _learn(res_key):
    if res_key is None:
        return
    try:
        global _last_instance
        _last_instance = int(res_key.instance)
        pair = (int(res_key.type), int(res_key.group))
        native._log('Photo: picture key type {:08x} group {:08x} instance {:016x}'.format(pair[0], pair[1], int(res_key.instance)))
        _read_learnt()
        if pair in _learnt:
            _learnt.remove(pair)
        _learnt.insert(0, pair)
        del _learnt[4:]
        path = _key_file()
        if path:
            with open(path, 'w') as f:
                for t, g in _learnt:
                    f.write('{:08x} {:08x}\n'.format(t, g))
    except Exception as e:
        native._log('Photo: learning the picture key failed: {!r}'.format(e))


def picture_bytes(texture_id):
    """The image bytes of a photo's picture, or None."""
    global _loaded_logs
    import sims4.resources
    _read_learnt()
    tried = []
    for t, g in list(_learnt) + [(t, 0) for t in _PICTURE_TYPES]:
        if (t, g) in tried:
            continue
        tried.append((t, g))
        try:
            data = bytes(sims4.resources.load(sims4.resources.Key(t, texture_id, g)))
        except Exception:
            continue
        if data:
            if _loaded_logs < 12:
                _loaded_logs += 1
                native._log('Photo: picture {:016x} read as type {:08x} group {:08x}, {} bytes, starts {!r}'.format(
                    texture_id, t, g, len(data), data[:4]))
            return data
    if _loaded_logs < 12:
        _loaded_logs += 1
        native._log('Photo: picture {:016x} not found (tried {})'.format(
            texture_id, ', '.join('{:08x}/{:08x}'.format(t, g) for t, g in tried)))
    return None


def want_icon(texture_id):
    """The Photos grid shows photo `texture_id`: True while it is (or will be) a picture of ours, False once it can't be read."""
    state = _owned.get(texture_id)
    if state is None:
        _owned[texture_id] = 'wait'
        return True
    return state != 'fail'


def view(index):
    """Control 8810000 + n: the Photos app opened the photo of inventory item n large."""
    global _big_for
    try:
        from sims4vr import phonesim
        import services
        if not 0 <= index < len(phonesim._inventory):
            return
        obj = services.object_manager().get(phonesim._inventory[index])
        if obj is None:
            sim = services.active_sim_info().get_sim_instance()
            obj = sim.inventory_component.get_item_with_id(phonesim._inventory[index]) if sim is not None else None
        texture = obj.canvas_component.painting_state.texture_id
        _big_for = texture
        data = picture_bytes(texture)
        if data is None:
            return
        fn = native._optional('photo_view')
        if fn is not None:
            fn(int(texture) & 0xffffffffffffffff, data)
    except Exception as e:
        native._log('Photo: opening a photo failed: {!r}'.format(e))


def _supply():
    """The HUD's small pictures of our photos, one a tick."""
    missing = native.hud_missing_icons()
    if not missing:
        return
    for k in range(len(missing) // 8):
        import struct
        instance = struct.unpack_from('<Q', missing, 8 * k)[0]
        if instance not in _owned:
            continue
        data = picture_bytes(instance)
        _owned[instance] = 'ok' if data else 'fail'
        native.hud_icon(instance, data or b'')
        return


def open_last():
    """The camera app's last-photo button: the last photo taken, large, in the Photos app's viewer."""
    try:
        if not _last_instance:
            return
        data = picture_bytes(_last_instance)
        fn = native._optional('photo_view')
        if data is not None and fn is not None:
            fn(_last_instance & 0xffffffffffffffff, data)
    except Exception as e:
        native._log('Photo: opening the last photo failed: {!r}'.format(e))
