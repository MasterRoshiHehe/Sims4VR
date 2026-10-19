"""First person: a game picker menu shows on the object it is for.

Cook... on a fridge opens the recipe picker; in first person the DLL shows it
on the fridge's door, like a smart fridge. A channel or movie picker on a TV
shows on the TV's screen. Every other picker shows as a card, like a popup (below).

When the game shows a picker dialog (UiDialogService.dialog_show) for the
Sim you are in, the object is found from the interaction that opened it (up
the call stack: the game's picker interactions, other mods' too), its front
face from the object's model box, and the DLL (vr.cpp, "Menus on objects")
is told where that face is in the world. The DLL finds the menu in the UI and
draws that part on the face; you point at it or poke it like the pie menu.
Answering or cancelling the dialog takes it off. A picker on a front (the
fridge) is cancelled when you walk away from the object (Escape, as the game's
Cancel; the server cancel only if that fails); one on a TV stays. While it is on the object the
game keeps running: the picker's pause (a UI modal pause, GameClock.push_speed)
gets a validity check that is false meanwhile, as phone.py does for the phone.
God view: nothing changes.
Switch: [ui] object_menus in sims4vr.ini.

Popups (a modal dialog waiting for an answer, e.g. "become a Fun-seeking
Sim?") go into the world too, in first person: as a card beside the Sim it is
about, like a speech bubble, when that Sim is within 5 m on your floor and in
your room; else as a card in front of you. The game still pauses for them as
it always does. Switch: [ui] popups_3d.
"""
import math
import struct
import sys
import time

from sims4vr import native

DOOR, SCREEN, VIDEO = 1, 2, 3  # VIDEO: the TV's own video screen, found in its model
POPUP_FRONT, POPUP_SIM = 4, 5     # a popup in front of you / beside the Sim it is about

# MATD shader hashes (FNV-1 32) of "VideoSurface", the shader of the mesh a
# TV plays its pictures on.
_VIDEO_SHADERS = (0x5595C1CB, 0x9841DC4B)

_logs = 0
_shown = None  # the dialog id shown on an object
_boxes = {}    # definition id: (model box, video screen box) (min xyz, max xyz each, or None)
_dumped = set()
_owner = type('_Owner', (), {})()
_alarm = None  # walk-away watch on a front picker
_watch = None  # (dialog id, face centre x, z, distance that closes it)
_escaped = 0.0  # when Escape went to the picker after walking away

WALK_AWAY = 2.0     # m from the front face (flat) that closes a front picker
WATCH_TICK = 0.25   # s
ESCAPE_WAIT = 1.5   # s for the game's picker to answer Escape
NEAR_SIM = 5.0      # m: a popup about a Sim this near (same floor and room) shows beside it
POPUP_TICK = 0.25   # s: the Sim's head followed this often while its popup shows
HEAD_JOINT = 'b__Head__'

_picker_request = None  # the picker's set_object_menu arguments, put back after a popup on top of it
_popup = None           # (dialog id, kind, Sim id or 0) of the popup shown in the world
_popup_alarm = None
_last_msg = None        # the UiDialogMessage the last dialog_show sent the client
_popup_choices = []     # the choice ids of the popup card's buttons, in order
_picker_rows = []       # the option ids of the picker card's rows, in the order sent (the DLL answers with an index)


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 80:
        native._log('objmenu: ' + message)


def _first_person_sim():
    try:
        from sims4vr import body
        if body.enabled and body.following():
            import services
            return services.object_manager().get(body.following())
    except Exception:
        pass
    return None


def _is_picker(dialog):
    return any('Picker' in cls.__name__ for cls in type(dialog).__mro__)


def _as_object(o):
    """A placed game object (not a Sim); a part counts as its whole object."""
    if o is None or getattr(o, 'is_sim', False):
        return None
    if getattr(o, 'is_part', False):
        o = getattr(o, 'part_owner', None)
        if o is None:
            return None
    if hasattr(o, 'definition') and hasattr(o, 'transform') and getattr(o, 'id', 0):
        return o
    return None


def _target_of(dialog, sim):
    """The object the picker is for: the target of the first-person Sim's
    interaction up the call stack, else an object the dialog names."""
    found = None
    try:
        frame = sys._getframe(2)
        for _ in range(40):
            if frame is None:
                break
            for value in tuple(frame.f_locals.values()):
                if hasattr(value, 'affordance') and getattr(value, 'sim', None) is sim:
                    target = _as_object(getattr(value, 'target', None))
                    if target is not None:
                        return target
            frame = frame.f_back
    except Exception as e:
        _log('call stack search failed: {!r}'.format(e))
    for name in ('target_object', '_target_object', 'target'):
        found = _as_object(getattr(dialog, name, None))
        if found is not None:
            return found
    return None


def _refpack(data, size):
    """EA's RefPack (QFS) unpacked."""
    out = bytearray()
    if len(data) < 2:
        return bytes(out)
    flags = data[0]
    at = 2 + (4 if flags & 0x80 else 3)
    if flags & 0x01:
        at += 4 if flags & 0x80 else 3
    n = len(data)
    while at < n:
        b0 = data[at]
        if b0 < 0x80:
            b1 = data[at + 1]
            plain, copy, offset, at = b0 & 3, ((b0 >> 2) & 7) + 3, ((b0 & 0x60) << 3) + b1 + 1, at + 2
        elif b0 < 0xC0:
            b1, b2 = data[at + 1], data[at + 2]
            plain, copy, offset, at = b1 >> 6, (b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1, at + 3
        elif b0 < 0xE0:
            b1, b2, b3 = data[at + 1], data[at + 2], data[at + 3]
            plain, copy = b0 & 3, ((b0 & 0x0C) << 6) + b3 + 5
            offset, at = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1, at + 4
        elif b0 < 0xFC:
            plain, copy, at = ((b0 & 0x1F) << 2) + 4, 0, at + 1
            offset = 0
        else:
            plain, copy, at, offset = b0 & 3, 0, at + 1, 0
        out += data[at:at + plain]
        at += plain
        for _ in range(copy):
            out.append(out[-offset])
        if b0 >= 0xFC:
            break
    return bytes(out)


def _unpack(compression, size, data):
    if compression == 0:
        return data
    if compression == 0x5A42:
        import zlib
        return zlib.decompress(data)
    if compression == 0xFFFF:
        return _refpack(data, size)
    raise ValueError('compression {:#x}'.format(compression))


def _rcol_chunks(data):
    """(public chunk count, [(tag, position, size)]) of an RCOL resource."""
    _version, public, _unused, external, internal = struct.unpack_from('<5I', data, 0)
    if not 0 < internal < 4096 or external > 4096:
        raise ValueError('{} chunks, {} external'.format(internal, external))
    at = 20 + (internal + external) * 16  # past the chunk and external TGIs
    chunks = []
    for i in range(internal):
        pos, size = struct.unpack_from('<2I', data, at + i * 8)
        if pos + size > len(data):
            raise ValueError('chunk {} past the end'.format(i))
        chunks.append((data[pos:pos + 4], pos, size))
    return public, chunks


def _chunk(value, public, chunks, tags):
    """The chunk a chunk reference names, if its tag is one of tags."""
    if not value or value >> 28 > 1:
        return None
    index = (value & 0x0FFFFFFF) - 1
    # Public (0x0...) counts from the first chunk, private (0x1...) from past
    # the public ones; the other first as a fallback.
    for i in ((index + public, index) if value >> 28 == 1 else (index, index + public)):
        if 0 <= i < len(chunks) and chunks[i][0] in tags:
            return chunks[i]
    return None


def _video_screen(data, shaders):
    """Box (min xyz, max xyz) of the meshes drawn with the VideoSurface shader
    in the first detail level that has them, or None. shaders collects every
    shader hash seen (for the log)."""
    public, chunks = _rcol_chunks(data)
    video = set()
    for tag, pos, size in chunks:
        if tag == b'MATD' and size >= 16:
            shader = struct.unpack_from('<I', data, pos + 12)[0]
            shaders.add(shader)
            if shader in _VIDEO_SHADERS:
                video.add(pos)

    def is_video(material):
        found = _chunk(material, public, chunks, (b'MATD', b'MTST'))
        if found is None:
            return False
        if found[0] == b'MATD':
            return found[1] in video
        # A material set: any of its materials.
        for at in range(found[1] + 8, found[1] + found[2] - 3, 4):
            ref = _chunk(struct.unpack_from('<I', data, at)[0], public, chunks, (b'MATD',))
            if ref is not None and ref[1] in video:
                return True
        return False

    if not video:
        return None
    for tag, pos, size in chunks:
        if tag != b'MLOD':
            continue
        count = struct.unpack_from('<I', data, pos + 8)[0]
        at, end = pos + 12, pos + size
        box = None
        for _ in range(min(count, 256)):
            if at + 76 > end:
                break
            length = struct.unpack_from('<I', data, at)[0]
            entry = at + 4
            if is_video(struct.unpack_from('<I', data, entry + 4)[0]):
                b = struct.unpack_from('<6f', data, entry + 48)
                box = b if box is None else tuple(min(box[i], b[i]) for i in range(3)) + \
                    tuple(max(box[3 + i], b[3 + i]) for i in range(3))
            at = entry + length
        if box is not None:
            return box
    return None


def _dump(instance, data):
    """A TV model whose screen wasn't found, saved next to the log to look at."""
    if instance in _dumped or len(_dumped) >= 8:
        return
    _dumped.add(instance)
    try:
        import os
        folder = os.path.join(os.path.dirname(native._log_path()), 'models')
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, '{:016x}.rcol'.format(instance)), 'wb') as f:
            f.write(data)
    except Exception as e:
        _log('model dump failed: {!r}'.format(e))


def _model_box(obj, tv=False):
    """(model box, video screen box) from the object's model, each (min xyz,
    max xyz) or None. The DLL reads the model from the game's packages (the
    game's Python can't)."""
    key = getattr(obj.definition, 'id', None)
    if key in _boxes:
        return _boxes[key]
    box = screen = None
    try:
        model = obj.definition.get_model(0)
        instance = getattr(model, 'instance', None)
        found = native.model_bytes(instance) if instance is not None else None
        if found is None:
            _log('no model reader (old DLL or no model key) for {}'.format(obj))
        elif found[0] == 1:
            _log('the game\'s models are still being indexed; {} gets a guessed size this time'.format(obj))
            return None, None  # not cached: next time
        elif found[0] != 0:
            _log('model {:#018x} of {}: {}'.format(instance, obj, 'not in the game\'s packages (custom content?)'
                                                    if found[0] == 2 else 'read failed'))
        else:
            data = _unpack(found[1], found[2], found[3])
            at = data.find(b'MODL')
            while at >= 0 and box is None:
                for skip in (12, 8):
                    if at + skip + 24 <= len(data):
                        b = struct.unpack_from('<6f', data, at + skip)
                        size = [b[3 + i] - b[i] for i in range(3)]
                        if all(0.01 < v < 20.0 for v in size):
                            box = b
                            break
                at = data.find(b'MODL', at + 4)
            if tv:
                shaders = set()
                try:
                    screen = _video_screen(data, shaders)
                except Exception as e:
                    _log('reading the screen of {} failed: {!r}'.format(obj, e))
                if screen is not None:
                    w, h, d = (screen[3 + i] - screen[i] for i in range(3))
                    if not (w > 0.05 and h > 0.05 and d < 0.25 * min(w, h)):
                        _log('screen box {} of {} is not a flat front; not used'.format(
                            tuple(round(v, 3) for v in screen), obj))
                        screen = None
                if screen is None:
                    _log('no video screen in model {:#018x} (shaders {}); saved to models/'.format(
                        instance, ' '.join('{:08x}'.format(v) for v in sorted(shaders))))
                    _dump(instance, data)
            _log('model {:#018x} of {}: {} bytes, box {}, screen {}'.format(
                instance, obj, len(data), tuple(round(v, 3) for v in box) if box else 'NOT FOUND',
                tuple(round(v, 3) for v in screen) if screen else '-'))
    except Exception as e:
        _log('no model box for {}: {!r}'.format(obj, e))
    _boxes[key] = (box, screen)
    return box, screen


def _rotate(q, v):
    x, y, z, w = q
    tx = 2 * (y * v[2] - z * v[1])
    ty = 2 * (z * v[0] - x * v[2])
    tz = 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))


def _front(obj, tv=False):
    """Centre of the object's front face (a TV's video screen if its model has
    one), its outward normal, its up axis, width and height (world units),
    and whether it is the video screen."""
    if tv:
        # The exact area the game plays its video on, from the screen mesh's own
        # vertices (screenmesh.py): its real tilt and size (user 2026-10-14
        # 22:54Z: laptops' lids, every computer and TV).
        try:
            from sims4vr import screenmesh
            exact = screenmesh.screen(obj)
        except Exception as e:
            exact = None
            _log('screen shape of {} failed: {!r}'.format(obj, e))
        if exact is not None:
            return exact + (True,)
    t = obj.transform
    p, o = t.translation, t.orientation
    q = (o.x, o.y, o.z, o.w)
    scale = getattr(obj, 'scale', 1.0) or 1.0
    box, screen = _model_box(obj, tv)
    if screen is not None:
        box = screen
    if box is None:  # a guess: a TV 0.8 x 0.55, else a fridge-sized front
        box = (-0.4, 0.0, -0.1, 0.4, 0.55, 0.1) if tv else (-0.45, 0.0, -0.3, 0.45, 1.8, 0.3)
    local = ((box[0] + box[3]) / 2 * scale, (box[1] + box[4]) / 2 * scale, box[5] * scale)
    r = _rotate(q, local)
    centre = (p.x + r[0], p.y + r[1], p.z + r[2])
    return centre, _rotate(q, (0.0, 0.0, 1.0)), _rotate(q, (0.0, 1.0, 0.0)), \
        (box[3] - box[0]) * scale, (box[4] - box[1]) * scale, screen is not None


def _is_computer(obj):
    try:
        from sims4vr import simsforever
        return simsforever._is_computer(obj)
    except Exception:
        return False


def _is_fridge(obj):
    try:
        return 'fridge' in str(obj).lower() or 'fridge' in type(obj).__name__.lower()
    except Exception:
        return False


def _is_tv(obj):
    try:
        from sims4vr import remote
        return remote._is_tv(obj)
    except Exception:
        return False


def _show(dialog):
    global _shown
    if not _is_picker(dialog):
        return
    sim = _first_person_sim()
    if sim is None:
        return
    owner = getattr(dialog, 'owner', None)  # a Sim or a SimInfo
    if owner is not None and getattr(owner, 'sim_id', None) != getattr(sim, 'sim_id', None):
        return
    obj = _target_of(dialog, sim)
    if obj is None:
        _log('{}: no object found for it; a card (popup)'.format(type(dialog).__name__))
        return
    tv = _is_tv(obj) or _is_computer(obj)  # a computer's pickers go on its screen too
    if not tv and not _is_fridge(obj):
        # Only TVs, computers and fridges show pickers on themselves (user
        # 2026-10-15 03:34Z); every other picker is a card like a popup.
        _log('{} on {}: a card (popup)'.format(type(dialog).__name__, obj))
        return
    centre, normal, up, width, height, video = _front(obj, tv)
    kind = (VIDEO if video else SCREEN) if tv else DOOR
    global _picker_request
    _picker_request = (kind, centre, normal, up, width, height)
    native.set_object_menu(kind, centre, normal, up, width, height)
    _shown = getattr(dialog, 'dialog_id', None)
    _refresh_clock()
    if kind == DOOR and _shown is not None:
        _watch_walk_away(_shown, centre, sim)
    _log('{} on {} ({}): face at {}, facing {}, {:.2f} x {:.2f}'.format(
        type(dialog).__name__, obj, {DOOR: 'front', SCREEN: 'screen (guessed)', VIDEO: 'video screen'}[kind],
        tuple(round(v, 2) for v in centre), tuple(round(v, 2) for v in normal), width, height))


def _check_gone(service):
    global _shown
    if _shown is None:
        return
    try:
        if service.get_dialog(_shown) is not None:
            return
    except Exception:
        pass
    _shown = None
    _stop_watch()
    global _picker_request, _picker_rows
    _picker_request = None
    _picker_rows = []
    try:
        native.picker_content(b'')
    except Exception:
        pass
    if _popup is not None:
        _send_popup()  # a popup still waits: it keeps the card
    else:
        native.set_object_menu(0)
    _refresh_clock()
    _log('picker answered or closed')


def _you(sim):
    """Where you stand: the player's body on the client (the simulation's Sim lags while you walk)."""
    pose = native.walk_pose()
    if pose is not None and all(math.isfinite(v) for v in pose[:3]):
        return pose[0], pose[2]
    p = sim.position
    return p.x, p.z


def _watch_walk_away(dialog_id, centre, sim):
    global _alarm, _watch
    _stop_watch()
    x, z = _you(sim)
    start = math.hypot(x - centre[0], z - centre[2])
    _watch = (dialog_id, centre[0], centre[2], max(WALK_AWAY, start + 1.0))
    import alarms
    import clock
    _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(WATCH_TICK), _tick, repeating=True)


def _stop_watch():
    global _alarm, _watch, _escaped
    _watch = None
    _escaped = 0.0
    if _alarm is not None:
        try:
            import alarms
            alarms.cancel_alarm(_alarm)
        except Exception:
            pass
        _alarm = None


def _tick(_handle):
    global _escaped
    watch = _watch
    if watch is None or watch[0] != _shown:
        _stop_watch()
        return
    try:
        if _escaped:
            # Escape went to the game's picker; it answers it as Cancel, and
            # _check_gone clears this. If it hasn't after a while, cancel it here.
            if time.monotonic() - _escaped < ESCAPE_WAIT:
                return
            _stop_watch()
            import services
            service = services.ui_dialog_service()
            if service.get_dialog(watch[0]) is None:
                return
            _log('Escape did not close the picker: cancelling it')
            from ui.ui_dialog import ButtonType
            service.dialog_respond(watch[0], ButtonType.DIALOG_RESPONSE_CANCEL)
            if service.get_dialog(watch[0]) is not None:
                service.dialog_cancel(watch[0])
            return
        sim = _first_person_sim()
        if sim is None:
            return
        x, z = _you(sim)
        away = math.hypot(x - watch[1], z - watch[2])
        if away <= watch[3]:
            return
        # Closed the way the game's Cancel button closes it (Escape, sent by
        # the DLL), so the game's own menu goes too, wrist included.
        _log('walked away ({:.1f} m): Escape cancels the picker'.format(away))
        _escaped = time.monotonic()
        native.set_object_menu(-1)
    except Exception as e:
        _stop_watch()
        _log('walk-away close failed: {!r}'.format(e))


def _refresh_clock():
    try:
        import services
        services.game_clock_service()._update_speed(immediate=True)
    except Exception as e:
        _log('clock refresh failed: {!r}'.format(e))


# --- popups ---------------------------------------------------------------------

def _sim_instance(sim_id):
    if not sim_id:
        return None
    try:
        import services
        info = services.sim_info_manager().get(sim_id)
        return info.get_sim_instance() if info is not None else None
    except Exception:
        return None


def _room(position, level):
    try:
        import build_buy
        import services
        return build_buy.get_room_id(services.current_zone_id(), position, level) or 0
    except Exception:
        return 0


def _popup_sim(dialog, you):
    """The Sim the popup is about, if it is near you: within NEAR_SIM on your
    floor and in your room (a wall between you would hide the card), and not you."""
    owner = getattr(dialog, 'owner', None)
    ids = (getattr(dialog, '_target_sim_id', None), getattr(owner, 'sim_id', None) or getattr(owner, 'id', None))
    x, z = _you(you)
    for sim_id in ids:
        other = _sim_instance(sim_id)
        if other is None or other is you or getattr(other, 'sim_id', None) == getattr(you, 'sim_id', None):
            continue
        try:
            p = other.position
            far = math.hypot(p.x - x, p.z - z)
            if far > NEAR_SIM or other.level != you.level:
                _log('popup about {}: {:.1f} m away, floor {} (yours {}): in front of you'.format(
                    other, far, other.level, you.level))
                continue
            here = _room(you.position, you.level)
            if _room(p, other.level) != here:
                _log('popup about {}: {:.1f} m away in another room: in front of you'.format(other, far))
                continue
            return other
        except Exception as e:
            _log('popup Sim check failed: {!r}'.format(e))
    return None


def _head(sim):
    try:
        found = native.joint(sim.id, HEAD_JOINT)
        if found is not None:
            return tuple(found[0])
    except Exception:
        pass
    p = sim.position
    return (p.x, p.y + 1.75, p.z)


def _send_popup():
    if _popup is None:
        return
    kind, sim_id = _popup[1], _popup[2]
    if kind == POPUP_SIM:
        sim = _sim_instance(sim_id)
        if sim is not None:
            native.set_object_menu(POPUP_SIM, _head(sim))
            return
    native.set_object_menu(POPUP_FRONT)


def _is_popup(service, dialog):
    """A modal dialog the client shows centred and paused (as phone.py counts
    them), or a picker that isn't shown on its object."""
    if _is_picker(dialog) and _shown is not None and getattr(dialog, 'dialog_id', None) == _shown:
        return False
    try:
        from protocolbuffers import Consts_pb2
        from ui.ui_dialog import PhoneRingType
        if dialog.DIALOG_MSG_TYPE == Consts_pb2.MSG_UI_NOTIFICATION_SHOW:
            return False
        if dialog.get_phone_ring_type() != PhoneRingType.NO_RING:
            return False
    except Exception as e:
        _log('popup type check failed: {!r}'.format(e))
        return False
    return service.get_dialog(getattr(dialog, 'dialog_id', None)) is dialog


def _show_popup(service, dialog):
    global _popup
    if not native.setting_flag('ui', 'popups_3d', True) or not _is_popup(service, dialog):
        return
    you = _first_person_sim()
    if you is None:
        return
    other = _popup_sim(dialog, you)
    _popup = (dialog.dialog_id, POPUP_SIM if other is not None else POPUP_FRONT,
              other.id if other is not None else 0)
    _send_popup_content(dialog)
    _send_popup()
    _start_popup_watch()
    _log('{}: {}'.format(type(dialog).__name__, 'beside {}'.format(other) if other is not None else 'in front of you'))


def _loc(value):
    """A LocalizedString message as JSON for the DLL's string tables
    (strings.cpp): its hash and tokens, every field that is set."""
    import json

    def plain(message):
        out = {}
        for field, v in message.ListFields():
            name = field.name
            if field.type == field.TYPE_MESSAGE:
                if field.label == field.LABEL_REPEATED:
                    out[name] = [plain(x) for x in v]
                else:
                    out[name] = plain(v)
            elif field.label == field.LABEL_REPEATED:
                out[name] = list(v)
            elif field.type == field.TYPE_ENUM and name == 'type':
                out[name] = field.enum_type.values_by_number[v].name if v in field.enum_type.values_by_number else v
            elif isinstance(v, bytes):
                out[name] = v.decode('utf-8', 'replace')
            else:
                out[name] = v
        return out
    return json.dumps(plain(value), ensure_ascii=False)


# Drama nodes whose yes travels your household (npc_invite_situation_drama_node,
# player_planned_drama_node travel_seed, player_planned_stayover_drama_node
# travel_to_destination).
_TRAVEL_NODES = {'NPCInviteSituationDramaNode', 'PlayerPlannedDramaNode',
                 'PlayerPlannedStayoverDramaNode'}


def _travel_popup(dialog, zone_id):
    """Why this popup's yes would travel (an invitation to another lot), or
    None: the game's NPC invitation dialog (NPCHostedSituationDialog, also its
    "bring others" answers), a popup showing another lot (zone_id), or one
    answered by a drama node that travels on yes (SituationDramaNode with a lot)."""
    import services
    from situations.npc_hosted_situations import NPCHostedSituationDialog
    if isinstance(dialog, NPCHostedSituationDialog):
        return 'invitation'
    here = services.current_zone_id()
    if zone_id and zone_id != here:
        return 'shows another lot'
    for listener in list(getattr(dialog, '_listeners', None) or ()):
        owners = [getattr(listener, '__self__', None)]
        for cell in getattr(listener, '__closure__', None) or ():
            try:
                owners.append(cell.cell_contents)
            except ValueError:
                pass
        for owner in owners:
            if owner is None:
                continue
            names = {c.__name__ for c in type(owner).__mro__}
            if names & _TRAVEL_NODES:
                return type(owner).__name__
            lot = getattr(owner, 'lot', None) if 'SituationDramaNode' in names else None
            if lot is not None:
                try:
                    from world import get_lot_id_from_instance_id
                    zone = services.get_persistence_service().resolve_lot_id_into_zone_id(
                        get_lot_id_from_instance_id(lot), ignore_neighborhood_id=True)
                except Exception:
                    zone = None
                if zone != here:
                    return type(owner).__name__
    return None


def _others_decide(dialog, zone_id=None):
    """First person, [ui] others_decide (debug menu): a decision popup about
    Sims that don't include yours is answered by the game at once with one of
    its answers picked at random, never shown (user 2026-10-17 11:53Z). The
    answer to give, or None to show it as usual. A popup about no Sim (saving,
    lot events) or with a button that opens part of the game's UI shows. An
    invitation to travel is always answered no (Cancel), never at random
    (user 2026-10-19 00:47Z: a yes travelled the household with no warning);
    one with no Cancel shows."""
    if not native.setting_flag('ui', 'others_decide', True) or _is_picker(dialog):
        return None
    you = _first_person_sim()
    if you is None:
        return None
    from protocolbuffers import Consts_pb2
    if getattr(dialog, 'DIALOG_MSG_TYPE', None) == Consts_pb2.MSG_UI_NOTIFICATION_SHOW:
        return None
    owner = getattr(dialog, 'owner', None)
    ids = {getattr(owner, 'sim_id', None), getattr(dialog, '_target_sim_id', None)} - {None, 0}
    if not ids or getattr(you, 'sim_id', None) in ids:
        return None
    from ui.ui_dialog import ButtonType, UiDialogResponse
    answers = []
    for response in dialog._get_responses_gen():
        rid = getattr(response, 'dialog_response_id', None)
        if rid is None or rid == ButtonType.DIALOG_RESPONSE_NO_RESPONSE:
            continue
        if getattr(response, 'ui_request', 0) not in (0, UiDialogResponse.UiDialogUiRequest.NO_REQUEST):
            return None
        if rid not in answers:
            answers.append(rid)
    if len(answers) < 2:
        return None  # nothing to decide (an OK)
    travel = _travel_popup(dialog, zone_id)
    if travel is not None:
        if ButtonType.DIALOG_RESPONSE_CANCEL not in answers:
            _log('travel popup about other Sims ({}) shown: no Cancel ({}, {})'.format(
                ', '.join(str(i) for i in ids), travel, type(dialog).__name__))
            return None
        _log('travel popup about other Sims ({}) answered no ({}, {})'.format(
            ', '.join(str(i) for i in ids), travel, type(dialog).__name__))
        return ButtonType.DIALOG_RESPONSE_CANCEL
    import random
    answer = random.choice(answers)
    _log('popup about other Sims ({}) answered at random: {} of {} answers ({})'.format(
        ', '.join(str(i) for i in ids), answer, len(answers), type(dialog).__name__))
    return answer


def _send_popup_content(dialog):
    """The popup's texts and buttons, for the card the DLL draws in the
    game's own dialog art (instead of a cut of the flat UI). Only plain
    answer dialogs: text inputs or buttons that make the client open
    something (CAS, build/buy, a store) keep the cut of the game's own dialog."""
    global _popup_choices
    _popup_choices = []
    msg = _last_msg
    try:
        if msg is None or getattr(msg, 'dialog_id', None) != dialog.dialog_id:
            raise ValueError('no message')
        # DEFAULT is 1 in the game's dialog message (UiDialogMessage.as
        # TYPE_DEFAULT = 1; unset reads as 1 too): plain answer dialogs are 1.
        # Checking for 0 sent every popup to the cut (user 2026-10-17 11:53Z).
        if msg.dialog_type not in (0, 1) or len(msg.text_input):
            raise ValueError('dialog type {} with {} text inputs'.format(msg.dialog_type, len(msg.text_input)))
        for c in msg.choices:
            for name in ('ui_message_args', 'show_build_buy_args', 'exchange_args', 'command_with_args', 'tutorial_args'):
                if c.HasField(name):
                    raise ValueError('a button with {}'.format(name))
            if c.ui_request:
                raise ValueError('a button with UI request {}'.format(c.ui_request))
        chat = bool(msg.target_id) and bool(msg.owner_id)
        # The two Sims' pictures as the game's SimSpeechHeader / SimSpeechFooter
        # load them (no more cuts of the game's UI, user 2026-10-17 13:52Z /
        # 14:17Z: the menu or nothing showed): the header's mask icon if it has
        # one, else the other Sim's bust; your bust.
        left = right = 0
        if chat:
            from sims4vr import hudinfo
            try:
                if msg.HasField('mask_header_icon') and msg.mask_header_icon.icon.instance:
                    left = hudinfo._picture(msg.mask_header_icon)
                else:
                    left = hudinfo._sim_bust(msg.target_id)
                right = hudinfo._sim_bust(msg.owner_id)
            except Exception as e:
                _log('popup pictures failed: {!r}'.format(e))
        records = ['{}\x1f{}\x1f{}\x1f{}'.format(1 if chat else 0, msg.dialog_id, left, right)]
        if msg.HasField('title'):
            records.append('title\x1f' + _loc(msg.title))
        if msg.HasField('text'):
            records.append('text\x1f' + _loc(msg.text))
        for c in msg.choices:
            records.append('button\x1f' + (_loc(c.text) if c.HasField('text') else '{}') + '\x1f' + str(c.choice_id))
            _popup_choices.append(c.choice_id)
        native.popup_content('\x1e'.join(records).encode('utf-8'))
        _log('popup card from the game\'s message: {} buttons{}'.format(len(_popup_choices), ', two Sims' if chat else ''))
    except Exception as e:
        try:
            native.popup_content(b'')
        except Exception:
            pass
        _log('popup card as a cut of the game\'s dialog ({})'.format(e))


def _send_picker_content(dialog):
    """A picker on a fridge as our own card (hud.cpp drawPicker; user
    2026-10-16 09:57Z: the cut of the game's flat picker looked badly cropped),
    from the game's own message, laid out by the rules of the game's recipe
    picker (GridPicker, re/allui/as/widgets/Pickers): the rows no other row
    links to make the list; a row's linked rows are its servings (itself first
    when it shows as a subrow, then those with its buff); the Use Ingredients
    box shows with an ingredients column and display_ingredient_check and is
    ticked, Use Prepped Ingredients the same with SP46. Single-pick recipe
    pickers only: any other keeps the cut of the game's picker."""
    global _picker_rows
    _picker_rows = []
    msg = _last_msg
    try:
        if msg is None or getattr(msg, 'dialog_id', None) != dialog.dialog_id:
            raise ValueError('no message')
        if not msg.HasField('picker_data'):
            raise ValueError('no picker data')
        picker = msg.picker_data
        if not picker.HasField('recipe_picker_data'):
            raise ValueError('picker type {}, not recipes'.format(picker.type))
        if (picker.min_selectable < 1 or picker.max_selectable > 1 or picker.max_selectable == 0 or
                picker.force_done_button):
            raise ValueError('picks several ({} to {})'.format(picker.min_selectable, picker.max_selectable))
        if getattr(msg, 'validation_command', ''):
            raise ValueError('a validation command')
        from sims4vr import hudinfo
        data = picker.recipe_picker_data
        columns = [c.column_data_name for c in data.column_list]
        ingredients = 'ingredients' in columns
        prepped = False
        if ingredients and data.display_prepped_ingredient_check:
            try:
                from sims4.common import Pack, is_available_pack
                prepped = bool(is_available_pack(Pack.SP46))
            except Exception:
                prepped = False
        flags = ((1 if ingredients and data.display_ingredient_check else 0) | (2 if prepped else 0) |
                 (4 if ingredients else 0) | (8 if 'price' in columns else 0))
        sort = ''
        for k in data.column_sort_list:
            if 0 <= k < len(columns) and data.column_list[k].sortable:
                sort = columns[k]
                break
        records = ['picker\x1f{}\x1f{}\x1f{}'.format(msg.dialog_id, flags, sort)]
        if picker.HasField('title'):
            records.append('title\x1f' + _loc(picker.title))
        elif msg.HasField('title'):
            records.append('title\x1f' + _loc(msg.title))
        for group in picker.filter_data:
            for f in group.filter_data:
                icon = hudinfo._icon(f.icon_info.icon) if f.HasField('icon_info') else 0
                records.append('filter\x1f{}\x1f{}\x1f{}'.format(
                    f.tag_type, icon, _loc(f.description) if f.HasField('description') else ''))
        rows = list(data.row_data)
        where = {}
        for i, row in enumerate(rows):
            where.setdefault(row.base_data.option_id, i)
        linked = set()
        for row in rows:
            for option in row.linked_option_ids:
                if option in where:
                    linked.add(where[option])
        for i, row in enumerate(rows):
            b = row.base_data
            servings = []
            if i not in linked and len(row.linked_option_ids):
                if row.visible_as_subrow:
                    servings.append(i)
                for option in row.linked_option_ids:
                    j = where.get(option)
                    if j is not None and rows[j].buff_id == row.buff_id:
                        servings.append(j)
            key = b.icon if b.HasField('icon') and b.icon.instance else (b.icon_info.icon if b.HasField('icon_info') else None)
            enabled = ((1 if b.is_enable else 0) | (2 if b.is_enable_fresh else 0) | (4 if b.is_enable_prepped else 0) |
                       (8 if b.is_enable_both_fresh_prepped else 0))
            records.append('\x1f'.join([
                'row', '0' if i in linked else '1', _loc(b.name) if b.HasField('name') else '', str(hudinfo._icon(key)),
                str(enabled), str(row.price), str(row.price_with_ingredients), str(row.price_with_only_prepped_ingredients),
                str(row.price_with_both_fresh_prepped_ingredients), str(row.discounted_price if row.is_discounted else 0),
                str(row.skill_level), _loc(row.serving_display_name) if row.HasField('serving_display_name') else '',
                ','.join(str(k) for k in servings), ','.join(str(t) for t in b.tag_list)]))
            _picker_rows.append(b.option_id)
        native.picker_content('\x1e'.join(records).encode('utf-8'))
        _log('picker card from the game\'s message: {} rows ({} in the list), {} filters, boxes {}, sorted by {}'.format(
            len(rows), len(rows) - len(linked), sum(len(g.filter_data) for g in picker.filter_data), flags, sort or '-'))
    except Exception as e:
        _picker_rows = []
        try:
            native.picker_content(b'')
        except Exception:
            pass
        _log('picker as a cut of the game\'s picker ({})'.format(e))


def answer_picker(code):
    """A row on the picker card (the DLL, control event 1000 + code; code =
    row * 4 + 1 use ingredients + 2 use prepped ingredients): the game's
    answer as its picker sends it, ui.dialog.pick_result then ui.dialog.respond OK."""
    try:
        index = code >> 2
        if _shown is None or not 0 <= index < len(_picker_rows):
            return
        import services
        from ui.ui_dialog import ButtonType
        service = services.ui_dialog_service()
        option = _picker_rows[index]
        picked = service.dialog_pick_result(_shown, picked_results=[option], ingredient_check=bool(code & 1),
                                            prepped_ingredient_check=bool(code & 2))
        if not picked:
            _log('the game refused the picker row {} (option {})'.format(index, option))
            return
        dialog_id = _shown
        service.dialog_respond(dialog_id, ButtonType.DIALOG_RESPONSE_OK)
        _log('picker answered with row {} (option {}, ingredients {}, prepped {})'.format(
            index, option, bool(code & 1), bool(code & 2)))
    except Exception as e:
        _log('answering the picker failed: {!r}'.format(e))


def cancel_picker():
    """The picker card's close button (control event 60): the game's Cancel,
    pick_result with nothing picked, then respond CANCEL."""
    try:
        if _shown is None:
            return
        import services
        from ui.ui_dialog import ButtonType
        service = services.ui_dialog_service()
        dialog_id = _shown
        service.dialog_pick_result(dialog_id, picked_results=[])
        service.dialog_respond(dialog_id, ButtonType.DIALOG_RESPONSE_CANCEL)
        _log('picker closed (cancel)')
    except Exception as e:
        _log('closing the picker failed: {!r}'.format(e))


def answer_popup(index):
    """A button on the popup card (the DLL, control event 50 + n): the game's
    answer for it, as a click on the dialog's button sends."""
    try:
        if _popup is None or not 0 <= index < len(_popup_choices):
            return
        import services
        dialog_id, choice = _popup[0], _popup_choices[index]
        services.ui_dialog_service().dialog_respond(dialog_id, choice)
        _log('popup answered with button {} (choice {})'.format(index, choice))
    except Exception as e:
        _log('answering the popup failed: {!r}'.format(e))


_map_view = None  # the map view's dialog id while it is up in first person


def _map_shown(dialog):
    """The map view (UiMapViewPicker, the travel screen) is a whole screen of
    the game's UI, not a popup: in first person it shows on the flat screen,
    as outside first person, until it closes (user 19:57Z). True if it is one."""
    global _map_view
    if not any(cls.__name__ == 'UiMapViewPicker' for cls in type(dialog).__mro__):
        return False
    if _first_person_sim() is not None:
        _map_view = getattr(dialog, 'dialog_id', None)
        native.set_mode(False)
        _log('map view: on the flat screen until it closes')
    return True


def _map_gone(service):
    global _map_view
    if _map_view is None:
        return
    try:
        if service.get_dialog(_map_view) is not None:
            return
    except Exception:
        pass
    _map_view = None
    if _first_person_sim() is not None:
        native.set_mode(True)
    _log('map view closed: back in first person')


def _popup_gone(service):
    global _popup
    if _popup is None:
        return
    try:
        if service.get_dialog(_popup[0]) is not None:
            return
    except Exception:
        pass
    _popup = None
    try:
        native.popup_content(b'')
    except Exception:
        pass
    _stop_popup_watch()
    if _picker_request is not None and _shown is not None:
        native.set_object_menu(*_picker_request)
    else:
        native.set_object_menu(0)
    _log('popup answered or closed')


def _start_popup_watch():
    global _popup_alarm
    _stop_popup_watch()
    if _popup is None or _popup[1] != POPUP_SIM:
        return
    import alarms
    import clock
    _popup_alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(POPUP_TICK), _popup_tick,
                                              repeating=True)


def _stop_popup_watch():
    global _popup_alarm
    if _popup_alarm is not None:
        try:
            import alarms
            alarms.cancel_alarm(_popup_alarm)
        except Exception:
            pass
        _popup_alarm = None


def _popup_tick(_handle):
    """The Sim's head, followed (the game may run on, or the Sim turn)."""
    try:
        if _popup is None or _first_person_sim() is None:
            _stop_popup_watch()
            if _popup is not None:
                native.set_object_menu(0)
            return
        _send_popup()
    except Exception as e:
        _stop_popup_watch()
        _log('popup follow failed: {!r}'.format(e))


def _on_object():
    """A picker is on its object, in first person: the game keeps running."""
    return _shown is not None and _first_person_sim() is not None


def _wrap_clock():
    from clock import ClockSpeedMode, GameClock, GameSpeedChangeSource
    original = GameClock.push_speed  # phone.py's wrapper, if it loaded first

    def push_speed(self, speed, source=GameSpeedChangeSource.GAMEPLAY, validity_check=None, reason='',
                   immediate=False):
        if speed == ClockSpeedMode.PAUSED and source == GameSpeedChangeSource.UI_MODAL:
            check = validity_check

            def not_while_on_object():
                return not _on_object() and (check is None or check())
            validity_check = not_while_on_object
            if _on_object():
                _log('kept the game running: picker pause "{}"'.format(reason))
        return original(self, speed, source=source, validity_check=validity_check, reason=reason,
                        immediate=immediate)

    GameClock.push_speed = push_speed


def _wrap():
    from ui.ui_dialog_service import UiDialogService
    original_show = UiDialogService.dialog_show

    def dialog_show(self, dialog, *args, **kwargs):
        try:
            if _map_shown(dialog):
                return original_show(self, dialog, *args, **kwargs)
        except Exception as e:
            _log('map view check failed: {!r}'.format(e))
        try:
            answer = _others_decide(dialog, kwargs.get('zone_id'))
        except Exception as e:
            answer = None
            _log('other Sims\' decision check failed: {!r}'.format(e))
        if answer is not None:
            # The game's own auto-respond: the dialog is registered and answered
            # on the next alarm (do_auto_respond), nothing reaches the screen.
            if len(args) >= 2:
                args = (args[0], answer) + tuple(args[2:])
            else:
                kwargs['auto_response'] = answer
            was = self._auto_respond
            self._auto_respond = True
            try:
                return original_show(self, dialog, *args, **kwargs)
            finally:
                self._auto_respond = was
        # First, so the DLL knows before the popup report (phone.py) that this
        # one shows on an object and needs no buzz.
        try:
            _show(dialog)
        except Exception as e:
            _log('showing on the object failed: {!r}'.format(e))
        # The message the game builds for the client (its texts, buttons and
        # Sims), kept for the popup card drawn from it.
        global _last_msg
        _last_msg = None
        try:
            build = dialog.build_msg

            def build_msg(*a, **k):
                global _last_msg
                msg = build(*a, **k)
                _last_msg = msg
                return msg
            dialog.build_msg = build_msg
        except Exception:
            build = None
        try:
            result = original_show(self, dialog, *args, **kwargs)
        finally:
            if build is not None:
                try:
                    del dialog.build_msg
                except Exception:
                    pass
        try:
            if _shown is not None and _shown == getattr(dialog, 'dialog_id', None) and _picker_request is not None \
                    and _picker_request[0] == DOOR:
                _send_picker_content(dialog)
        except Exception as e:
            _log('the picker card failed: {!r}'.format(e))
        try:
            _show_popup(self, dialog)
        except Exception as e:
            _log('showing the popup in the world failed: {!r}'.format(e))
        return result
    dialog_show.__name__ = 'dialog_show'
    UiDialogService.dialog_show = dialog_show

    def wrap_close(name):
        original = getattr(UiDialogService, name)

        def wrapped(self, *args, **kwargs):
            try:
                return original(self, *args, **kwargs)
            finally:
                try:
                    _check_gone(self)
                except Exception:
                    pass
                try:
                    _popup_gone(self)
                except Exception:
                    pass
                try:
                    _map_gone(self)
                except Exception:
                    pass
        wrapped.__name__ = name
        setattr(UiDialogService, name, wrapped)

    for name in ('dialog_respond', 'dialog_cancel', '_dialog_cancel_internal', 'disable_on_teardown'):
        if hasattr(UiDialogService, name):
            wrap_close(name)


try:
    _wrap()
except Exception as _e:
    native._log('objmenu: wrapping the game dialogs failed: {!r}'.format(_e))

try:
    _wrap_clock()
except Exception as _e:
    native._log('objmenu: wrapping the game clock failed: {!r}'.format(_e))
