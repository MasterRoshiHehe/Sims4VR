"""Loads sims4vr.dll as the Python extension module _sims4vr and wraps it.

The game's embedded Python has no ctypes, so the DLL doubles as an extension
module (PyInit__sims4vr) and is loaded with importlib.
"""
import atexit
import os
import time
import traceback

DLL_NAME = 'sims4vr.dll'

_mod = None
_error = None


def _find_dll():
    # Inside a .ts4script zip, __file__ is ".../Mods/sims4vr/sims4vr.ts4script/sims4vr/native.pyc",
    # so walk up until a real folder holds the DLL. Also covers loose .py files.
    path = os.path.dirname(os.path.abspath(__file__))
    for _ in range(5):
        candidate = os.path.join(path, DLL_NAME)
        if os.path.isfile(candidate):
            return candidate
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    return None


def _log_path():
    return os.path.join(os.path.dirname(_find_dll() or __file__), 'sims4vr_python.log')


_log_file = None


def _log(message):
    # One handle kept open: opening the file for every line went through the
    # mod manager's virtual file system each time, a small stall in the frame.
    global _log_file
    try:
        if _log_file is None:
            _log_file = open(_log_path(), 'a')
        _log_file.write('[{}.{:03d}] [py] {}\n'.format(time.strftime('%H:%M:%S'), int(time.time() * 1000) % 1000, message))
        _log_file.flush()
    except Exception:
        _log_file = None


try:
    from sims4vr import _build  # written by tools/build_scriptmod.py; absent in loose-source runs
    _log('sims4vr script mod v{} (commit {})'.format(_build.VERSION, _build.COMMIT))
except Exception:
    _log('sims4vr script mod (no build info)')

_slow_logs = 0
_times = {}  # name -> [calls, total seconds, worst seconds, calls over the limit, the limit]
_times_since = time.perf_counter()
TIMES_EVERY = 30.0  # seconds between the timing summaries in the log


def _report_times(now):
    global _times_since
    parts = ['{} {}x, {:.1f} ms on average, {:.0f} ms at most, {} over {:.0f} ms'.format(
        name, calls, total * 1000 / calls, worst * 1000, slow, limit * 1000)
        for name, (calls, total, worst, slow, limit) in sorted(_times.items()) if calls]
    if parts:
        _log('time in the last {:.0f} s: {}'.format(now - _times_since, '; '.join(parts)))
    _times.clear()
    _times_since = now


def timed(name, limit=0.008):
    """Decorator for work on the game's thread (the frame waits for it): every
    30 s the log gets how often each ran, its average and its worst; calls far
    over `limit` seconds are also logged one by one, the first few dozen."""
    def wrap(fn):
        def run(*args, **kwargs):
            global _slow_logs
            started = time.perf_counter()
            try:
                return fn(*args, **kwargs)
            finally:
                now = time.perf_counter()
                took = now - started
                entry = _times.get(name)
                if entry is None:
                    entry = _times[name] = [0, 0.0, 0.0, 0, limit]
                entry[0] += 1
                entry[1] += took
                if took > entry[2]:
                    entry[2] = took
                if took > limit:
                    entry[3] += 1
                    if took > limit * 3 and _slow_logs < 40:
                        _slow_logs += 1
                        _log('slow: {} took {:.0f} ms'.format(name, took * 1000))
                if now - _times_since > TIMES_EVERY:
                    _report_times(now)
        return run
    return wrap


_fatal_file = None


def _keep_previous(path):
    """A relaunch must not wipe the last session's fatal error."""
    try:
        with open(path) as f:
            text = f.read(4096)
        if len(text.splitlines()) > 1:
            os.replace(path, path[:-len('.log')] + '_prev.log')
    except OSError:
        pass


def _watch_fatal_errors():
    """Python's own fatal errors, and the Python side of a crash, go to
    sims4vr_fatal.log: the game shows them nowhere and just closes."""
    global _fatal_file
    if _fatal_file is not None:
        return
    try:
        import faulthandler
        path = os.path.join(os.path.dirname(_log_path()), 'sims4vr_fatal.log')
        _keep_previous(path)
        _fatal_file = open(path, 'w')
        _fatal_file.write('session started; nothing fatal recorded yet\n')
        _fatal_file.flush()
        faulthandler.enable(file=_fatal_file, all_threads=True)
        # "Fatal Python error" messages are written to stderr.
        os.dup2(_fatal_file.fileno(), 2)
    except Exception as e:
        _log('fatal error log unavailable: {!r}'.format(e))


def load():
    """Load the DLL once and call init. Never raises: errors go to the log."""
    global _mod, _error
    if _mod is not None:
        return True
    try:
        path = _find_dll()
        if path is None:
            _error = DLL_NAME + ' not found next to the .ts4script'
            _log(_error)
            return False
        import importlib.machinery
        import importlib.util
        loader = importlib.machinery.ExtensionFileLoader('_sims4vr', path)
        spec = importlib.util.spec_from_file_location('_sims4vr', path, loader=loader)
        mod = importlib.util.module_from_spec(spec)
        loader.exec_module(mod)
        _mod = mod
        atexit.register(mod.shutdown)
        result = mod.init()
        _log('loaded {} (version {}), init returned {}'.format(path, mod.version(), result))
        _watch_fatal_errors()
        if result != 0:
            _error = 'init returned {} (see sims4vr.log)'.format(result)
        return result == 0
    except Exception:
        _error = traceback.format_exc()
        _log(_error)
        return False


def start():
    if _mod is None and not load():
        return _error
    result = _mod.start()
    return None if result == 0 else 'start returned {} (see sims4vr.log)'.format(result)


def loaded():
    return _mod is not None


def recenter():
    if _mod is not None:
        _mod.recenter()


def set_mode(stereo):
    if _mod is not None:
        _mod.set_mode(1 if stereo else 0)


def set_body(object_id):
    """View from this Sim's eyes (body mode); 0 goes back to the game camera."""
    if _mod is not None:
        _mod.set_body(object_id)


def set_body_root(position, orientation):
    if _mod is not None:
        _mod.set_body_root(position.x, position.y, position.z,
                           orientation.x, orientation.y, orientation.z, orientation.w)


def set_floor(y):
    if _mod is not None:
        _mod.set_floor(y)


def on_gameplay(playing):
    if _mod is not None:
        return _mod.on_gameplay(1 if playing else 0)
    return 0


def set_scale(scale):
    if _mod is not None:
        _mod.set_scale(scale)


def status():
    if _mod is None:
        return 'DLL not loaded: {}'.format(_error)
    return _mod.status()


def find_camera(position, target):
    """Search game memory for the camera at these coordinates; returns a summary line."""
    if _mod is None:
        return 'DLL not loaded: {}'.format(_error)
    return _mod.find_camera(*position, *target)


def walk_input():
    """Left stick in body mode as a world direction: (x, z, strength 0..1), or None."""
    if _mod is not None:
        return _mod.walk_input()
    return None


# Developer probe helpers (see probe.py)
def dev_probe():
    return _mod is not None and _mod.dev_probe() == 1


def game_base():
    return _mod.game_base()


def mem_read(address, size):
    return _mod.mem_read(address, size)


def mem_write(address, data):
    return _mod.mem_write(address, data)


def rtti_name(address):
    return _mod.rtti_name(address)


def object_ptr(object_id):
    return _mod.object_ptr(object_id)


def joint(object_id, name):
    return _mod.joint(object_id, name)


def set_player_state(free_allowed, blocked_mask, floor, flight=None):
    """floor: (x, z, height, rise_x, rise_z[, lowest, highest]) around the player;
    flight: the stairs you are on or at, (bottom centre x, z, up x, z, run,
    half width), whose sides hold you."""
    if _mod is not None:
        free = 1 if free_allowed else 0
        try:
            if flight is not None and len(floor) == 7:
                _mod.set_player_state(free, blocked_mask, *floor, *flight)
            else:
                _mod.set_player_state(free, blocked_mask, *floor)
        except TypeError:  # a DLL older than the script takes fewer values
            try:
                _mod.set_player_state(free, blocked_mask, *floor)
            except TypeError:
                _mod.set_player_state(free, blocked_mask, *floor[:5])


def door(object_id, open_):
    """Swing a door open for the player (count them in), or let it close."""
    if _mod is not None:
        _mod.door(object_id, 1 if open_ else 0)


def set_held(ids):
    """Objects (up to 8) that may be in the Sim's hands; the DLL checks which
    sit in your palms, and those fingers keep the game's grip."""
    fn = getattr(_mod, 'set_held', None)  # absent in a DLL older than the script
    if fn is not None:
        fn(*ids[:8])


def pie_menu(items):
    """The game built a pie menu with this many options for a click into the
    world (0: you picked from it)."""
    if _mod is not None:
        fn = getattr(_mod, 'pie_menu', None)  # absent in a DLL older than the script
        if fn is not None:
            fn(int(items))


def set_sleeping(sleeping):
    """Close the eyelids over the view (the Sim you are in sleeps or naps)."""
    fn = getattr(_mod, 'set_sleeping', None)  # absent in a DLL older than the script
    if fn is not None:
        fn(bool(sleeping))


def set_eyes_covered(covered):
    """Close the eyelids quickly while the game fades your Sim out (fades.py)."""
    fn = _optional('set_eyes_covered')  # absent in a DLL older than the script
    if fn is not None:
        fn(bool(covered))


def set_body_view(on):
    """An animation in [body] body_view_anims plays: the view lies with the body."""
    fn = _optional('set_body_view')  # absent in a DLL older than the script
    if fn is not None:
        fn(bool(on))


def set_game_menu(open_):
    """Whether a game menu or dialog (a UI modal) is open, for the phone."""
    fn = _optional('set_game_menu')  # absent in a DLL older than the script
    if fn is not None:
        fn(bool(open_))


def set_popup(open_):
    """Whether the game shows a popup (a modal dialog), for the phone's buzz."""
    fn = _optional('set_popup')  # absent in a DLL older than the script
    if fn is not None:
        fn(bool(open_))


def set_clock(hour, minute, day, speed):
    """The game clock for the VR HUD (day 0 = Sunday; speed = ClockSpeedMode)."""
    fn = _optional('set_clock')  # absent in a DLL older than the script
    if fn is not None:
        fn(int(hour), int(minute), int(day), int(speed))


def hud_items(kind, packed):
    """The HUD's needs 0 / moodlets 1 / queue 2 / wants 3 (hudinfo.py): packed 20-byte items."""
    fn = _optional('hud_items')
    if fn is not None:
        fn(int(kind), bytes(packed))


def hud_icon(instance, data):
    """An icon the HUD asked for: its image bytes (empty: it could not be read)."""
    fn = _optional('hud_icon')
    if fn is not None:
        fn(int(instance) & 0xffffffffffffffff, bytes(data))


def hud_texts(kind, packed):
    """The tooltips of the HUD's needs 0 / moodlets 1 (hudinfo.py): per item
    title, text, reason (LocalizedString JSON) and minutes left, split by 0x1f,
    items by 0x1e."""
    fn = _optional('hud_texts')
    if fn is not None:
        fn(int(kind), bytes(packed))


def money_changed(amount):
    """The active household's money went up or down (hudinfo.py): the HUD's flyaway."""
    fn = _optional('money_changed')
    if fn is not None:
        fn(int(amount))


def trail_set(packed):
    """The guiding trail's route (trail.py): packed floats goal x, y, z, fx, fz, then x, y, z per
    point along the floor; b'': no trail."""
    fn = _optional('trail_set')
    if fn is not None:
        fn(bytes(packed))


def popup_content(packed):
    """The popup's texts and buttons from the game's message (objmenu.py); b'': gone."""
    fn = _optional('popup_content')
    if fn is not None:
        fn(bytes(packed))


def picker_content(packed):
    """The picker on its object from the game's message (objmenu.py); b'': gone."""
    fn = _optional('picker_content')
    if fn is not None:
        fn(bytes(packed))


def phone_notes(packed):
    """The game's notifications for the phone's bell and list (notify.py)."""
    fn = _optional('phone_notes')
    if fn is not None:
        fn(bytes(packed))


def phone_calendar(packed):
    """The game's calendar for the phone's Calendar app (phonecal.py)."""
    fn = _optional('phone_calendar')
    if fn is not None:
        fn(bytes(packed))


def note_over(note_id, x, y, z, front):
    """Notification `note_id` as a card over a Sim's head at x, y, z (world), or in front of you; 0: none."""
    fn = _optional('note_over')
    if fn is not None:
        fn(int(note_id), float(x), float(y), float(z), int(front))


def phone_content(packed):
    """The game's phone menu for the phone in your hand (phoneapp.py); b'': gone."""
    fn = _optional('phone_content')
    if fn is not None:
        fn(bytes(packed))


def hud_missing_icons():
    """Packed u64 instances of the icons the HUD still needs (b'' none)."""
    fn = _optional('hud_missing_icons')
    return fn() if fn is not None else b''


def set_paused(paused):
    """Whether the game clock is paused, for the pause symbol in first person."""
    fn = _optional('set_paused')  # absent in a DLL older than the script
    if fn is not None:
        fn(bool(paused))


def set_build_buy(on):
    """Whether the lot is in build/buy mode (the first-person tablet, buildbuy.py)."""
    fn = _optional('set_build_buy')  # absent in a DLL older than the script
    if fn is not None:
        fn(bool(on))


def resource_bytes(type_, instance):
    """Any game resource as stored: (status, compression, size, bytes); status
    0 found, 1 still looking, 2 none, 3 read failed. None with an older DLL."""
    fn = _optional('resource_bytes')
    if fn is None:
        return None
    return fn(int(type_) & 0xffffffff, int(instance) & 0xffffffffffffffff)


def set_tablet_wallpaper(type_, data):
    """The played Sim's phone background picture (unpacked image bytes), for the tablet."""
    fn = _optional('set_tablet_wallpaper')  # absent in a DLL older than the script
    if fn is not None:
        fn(int(type_) & 0xffffffff, bytes(data))


def model_bytes(instance):
    """An object model from the game's packages: (status, compression, size,
    bytes); status 0 found, 1 not indexed yet, 2 none, 3 read failed. None
    with an older DLL."""
    fn = _optional('model_bytes')
    if fn is None:
        return None
    return fn(int(instance) & 0xFFFFFFFFFFFFFFFF)


def set_object_menu(kind, centre=(0, 0, 0), normal=(0, 0, 1), up=(0, 1, 0), width=0.0, height=0.0):
    """Show the open picker on an object's front face (kind 1 a door or front,
    2 a screen; world units), or take it off (kind 0)."""
    fn = _optional('set_object_menu')  # absent in a DLL older than the script
    if fn is not None:
        fn(int(kind), *(float(v) for v in tuple(centre) + tuple(normal) + tuple(up)), float(width), float(height))


def sims_forever(on, centre=(0, 0, 0), normal=(0, 0, 1), up=(0, 1, 0), width=0.0, height=0.0, exe='', args=''):
    """Sims 1 on a computer's screen (world units; simsforever.py), started from
    exe with args the first time; on False takes it off."""
    fn = _optional('sims_forever')  # absent in a DLL older than the script
    if fn is not None:
        fn(1 if on else 0, *(float(v) for v in tuple(centre) + tuple(normal) + tuple(up)), float(width), float(height),
           str(exe), str(args))


def claude_screen(on, centre=(0, 0, 0), normal=(0, 0, 1), up=(0, 1, 0), width=0.0, height=0.0, exe=''):
    """The Claude app on a computer's screen (world units; claude.py); False takes it off."""
    fn = _optional('claude_screen')
    if fn is not None:
        fn(1 if on else 0, *(float(v) for v in tuple(centre) + tuple(normal) + tuple(up)), float(width), float(height),
           str(exe))


def sims_forever_surface(nx=0, ny=0, depths=()):
    """The screen's shape for Sims 1's picture (screenmesh.py): an nx x ny grid
    of depths behind its front (world units, rows from the bottom left); 0 x 0
    = flat."""
    fn = _optional('sims_forever_surface')
    if fn is not None:
        import struct
        depths = list(depths)[:int(nx) * int(ny)]
        fn(int(nx), int(ny), struct.pack('<{}f'.format(len(depths)), *depths))


def phone_mount():
    """Our phone on the Sim's right hand joint: ((x, y, z), (qx, qy, qz, qw)) in
    that joint's frame, or None (old DLL, or the rig isn't read yet)."""
    fn = _optional('phone_mount')
    if fn is None:
        return None
    try:
        return fn()
    except Exception:
        return None


def grip_learn(on):
    """The game's remote animation runs on your Sim (True): the DLL learns how
    it holds the remote (where it sits in the hand, how the fingers close)."""
    fn = _optional('grip_learn')
    if fn is not None:
        fn(bool(on))


def grip_hold(hand, on):
    """That hand (0 left) holds our remote: its fingers take the learned grip."""
    fn = _optional('grip_hold')
    if fn is not None:
        fn(int(hand), bool(on))


def outdoors(through, walls):
    """First person: how much of the outdoors reaches you inside through doors
    and arches, 0..1 (negative: not known, the DLL uses the wall distance
    alone), and how much comes through plain walls, 0..1 (outdoors.py)."""
    fn = _optional('outdoors')  # absent in a DLL older than the script
    if fn is not None:
        fn(float(through), float(walls))


def grip(hand):
    """Where the remote goes on that hand's joint, learned from the game:
    ((x, y, z), (qx, qy, qz, qw)) in the joint's frame, or None until learned
    (or with an older DLL)."""
    fn = _optional('grip')
    if fn is None:
        return None
    try:
        return fn(int(hand))
    except Exception:
        return None


def set_phone_prop(width, height, depth):
    """The game phone model's size, so the DLL draws our phone around it."""
    fn = _optional('set_phone_prop')
    if fn is not None:
        fn(float(width), float(height), float(depth))


def set_voice_sim(object_id):
    """The Sim whose mouth your mic moves (0: none; the mic is released)."""
    fn = _optional('set_voice_sim')  # absent in a DLL older than the script
    if fn is not None:
        fn(int(object_id))


def walk_stop(object_id, route_id):
    """End our walk route on the client, if it is still the one running."""
    if _mod is not None:
        _mod.walk_stop(object_id, route_id)


def client_route(object_id):
    """The route the client has active on that object, as of the last frame
    (0: none, or a DLL older than the script)."""
    fn = _optional('client_route')
    return fn(int(object_id)) if fn is not None else 0


def walk_pose():
    """(x, y, z, yaw, moving, free, vx, vz) of the player's body on the client, or None."""
    if _mod is not None:
        return _mod.walk_pose()
    return None


def dll_log(text):
    """A line in sims4vr.log, which has times (to line up with the DLL's lines)."""
    fn = _optional('dll_log')
    if fn is not None:
        try:
            fn(str(text))
        except Exception:
            pass


def _optional(name):
    """A DLL function that an older DLL may not have (None then)."""
    return getattr(_mod, name, None) if _mod is not None else None


def click_hand():
    """The hand of the last click into the world in first person (0 left,
    1 right; laser or fingertip), or -1 if none in the last 5 s."""
    fn = _optional('click_hand')
    try:
        return int(fn()) if fn is not None else -1
    except Exception:
        return -1


def pie_shown():
    """True while the pie menu shows at your hand; None with a DLL that can't tell."""
    fn = _optional('pie_shown')
    try:
        return bool(fn()) if fn is not None else None
    except Exception:
        return None


def running():
    """True while the player runs (a tap of the left stick in first person)."""
    fn = _optional('running')  # absent in a DLL older than the script
    try:
        return bool(fn()) if fn is not None else False
    except Exception:
        return False


def setting(section, key, default=None):
    """A value from sims4vr.ini as text, or default if it is not set."""
    fn = _optional('setting')
    if fn is None:
        return default
    try:
        value = fn(section, key).strip()
    except Exception:
        return default
    return value if value else default


def setting_flag(section, key, default=True):
    value = setting(section, key)
    if value is None:
        return default
    return value not in ('0', 'false', 'off', 'no')


def set_grabbables(objects):
    """Objects you can pick up near you: (id, x, y, z, reach) each, up to 12."""
    fn = _optional('set_grabbables')
    if fn is not None:
        flat = []
        for entry in objects[:12]:
            flat.extend((int(entry[0]), float(entry[1]), float(entry[2]), float(entry[3]), float(entry[4])))
        fn(*flat)


def hand_event():
    """(kind, hand, object_id): 1 a hand grabbed it, 2 let go of it; None if none."""
    fn = _optional('hand_event')
    return fn() if fn is not None else None


def set_touch_restricted(ids):
    """Sims (up to 16) whose body may only be held at the hand, arms,
    shoulders, back and hair."""
    fn = _optional('set_touch_restricted')
    if fn is not None:
        fn(*[int(i) for i in list(ids)[:16]])


def touch_event():
    """(kind, hand, region, actor, value) of the oldest touch on another Sim's
    body: kind 1 took hold, 2 let go (value seconds), 3 pulled hard (value
    metres); actor = its client object (compare object_ptr). None if none."""
    fn = _optional('touch_event')
    return fn() if fn is not None else None


def grab_set(hand, object_id):
    """What that hand (0 left) holds now; 0 when a grab did not work out."""
    fn = _optional('grab_set')
    if fn is not None:
        fn(int(hand), int(object_id))


def hand_joint(hand):
    """The first-person Sim's hand joint in the world, read by the game
    thread: (sim id, (x, y, z), (qx, qy, qz, qw)), or None."""
    fn = _optional('hand_joint')
    return fn(hand) if fn is not None else None


def hands():
    """Head and hands in the world in first person, or None:
    ((head xyz), (forward xyz), (up xyz), left, right) with each hand
    (valid, (palm xyz), (q xyzw), grip, trigger, held object id)."""
    fn = _optional('hands')
    return fn() if fn is not None else None


def haptic(hand, amplitude, seconds):
    fn = _optional('haptic')
    if fn is not None:
        fn(int(hand), float(amplitude), float(seconds))


def set_testing_cheats(state):
    """The debug menu's switches show this: 1 testing cheats on, 2 every pie
    menu the Shift+click cheat menu (v0.13.127; a bool before)."""
    fn = _optional('set_testing_cheats')  # absent in a DLL older than the script
    if fn is not None:
        fn(int(state))


def set_script_mods(bits):
    """Other script mods loaded, for the debug menu's pages: 1 << 2 Claude Performance (v0.13.235)."""
    fn = _optional('set_script_mods')  # absent in a DLL older than the script
    if fn is not None:
        fn(int(bits))


def control_event():
    """Controller shortcut: 1 switch first person / game camera, 2 next Sim, 0 none."""
    fn = _optional('control_event')
    return fn() if fn is not None else 0
