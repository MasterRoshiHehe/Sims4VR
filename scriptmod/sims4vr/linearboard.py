"""First person: "Open Linear board" on a TV, a computer or any other object with a screen (thread
"Bring the Linear board into the game", GLA-46, user 2026-11-26: the project's Linear board in the game).

The pie option is claude.py's (make_affordance), as Discord's: a screen object with several screens lists them
in a sub menu. Picking it draws your Linear team's board on that screen (linearboard.cpp, through
claudescreen slot 2 and the TV screen path: our own drawing, not a browser). The laser's trigger or a finger
moves cards: press on a card and drag it to another column, or tap it for its details card and pick a column
there. Moves go to Linear at once; changes made in Linear show within [linear] poll seconds while you look at
the board (nothing is asked of Linear while it is out of view). A / X on the screen opens the screen's own pie
(Close Linear board). Walking away (2 m from a computer, 10 m from a TV) or leaving first person closes it.

Your Linear key: %APPDATA%\\sims4vr\\linear_key.txt (a personal API key from Linear: Settings > Security &
access). The DLL reads it once, keeps it encrypted for your Windows account and wipes the text file; the key is
never in an ini, a log or the mod's folders. The board says what to do while there is none.

The scrumboard (user 2026-11-26: "an actual scrumboard in game"): wall art (a painting, a poster, a bulletin
board: anything hung on a wall that Sims can look at as art) gets "Open Linear scrumboard". The same board, as a
cork board with paper notes pinned on it, over the art's front. Close a hand on a note to take it off the board,
carry it, and open the hand at the board to pin it in another column (moved in Linear); open it away from the
board and the note goes back. Taps, the laser and the details card work as on a screen.

Our own object for it (GLA-125): the "SCRUM Board" in Buy mode, a cork board on an oak stand
(Mods\\sims4vr_linearscrumboard.package); the same "Open Linear scrumboard" puts the board on its cork.

Switches: [linear] on and [linear] cork in sims4vr.ini (debug menu: Options > Interface > Pickers on objects).
"""
from sims4vr import claude, native

SLOT = 2  # claudescreen::kLinear
GUID = 0x5653434C4C494E45
CORK_GUID = 0x5653434C4C494E46
MIN_ART = 0.3  # metres: smaller wall art is no board
# Our own object (tools/cc/build_corkboard.py, GLA-125): the "SCRUM Board", a cork board on an oak stand
# (Buy > Decorations, 150). Its definition id and the board's front in the object's space (metres, +z = front):
# the frame's outer edges, the picture just in front of the frame.
STAND_ID = 0xC715BC4CD06CAD43
STAND_BOARD = (-0.46, 0.77, 0.46, 1.45)  # x0 y0 x1 y1
STAND_FRONT_Z = 0.016

_cork = False  # the board is the scrumboard on wall art (else on a screen)
_cork_affordance = None
_art = {}  # object definition id -> (is wall art, why), logged once each

_open = None  # the screen object's id while the board is on its screen
_open_index = 0
_affordance = None
_last = None  # the options last sent to the DLL


def _log(message):
    claude._log('linear: ' + message)


def shown_on(obj, index=0):
    """The board is on this screen of this object."""
    return _open is not None and _open == getattr(claude._owner(obj), 'id', None) and _open_index == index


def _options():
    team = (native.setting('linear', 'team', 'GLA') or 'GLA').strip()
    poll = int(float(native.setting('linear', 'poll', 20) or 20))
    width = int(float(native.setting('linear', 'width', 1920) or 1920))
    done_days = int(float(native.setting('linear', 'done_days', 14) or 14))
    canceled = 1 if native.setting_flag('linear', 'canceled', False) else 0
    return team, 'done_days={};canceled={};mode={}'.format(done_days, canceled, 'cork' if _cork else 'screen'), poll, width


def is_stand(obj):
    """Our SCRUM Board object (a cork board on a stand)."""
    return getattr(getattr(obj, 'definition', None), 'id', None) == STAND_ID


def _stand_front(obj):
    from sims4vr import objmenu
    t = obj.transform
    p, o = t.translation, t.orientation
    q = (o.x, o.y, o.z, o.w)
    scale = getattr(obj, 'scale', 1.0) or 1.0
    x0, y0, x1, y1 = STAND_BOARD
    r = objmenu._rotate(q, ((x0 + x1) / 2 * scale, (y0 + y1) / 2 * scale, STAND_FRONT_Z * scale))
    return ((p.x + r[0], p.y + r[1], p.z + r[2]), objmenu._rotate(q, (0.0, 0.0, 1.0)),
            objmenu._rotate(q, (0.0, 1.0, 0.0)), (x1 - x0) * scale, (y1 - y0) * scale)


def is_wall_art(obj):
    """Hung on a wall, looked at as art (the game's art_View, or a bulletin / community board), not a screen, and
    big enough for a board."""
    if obj is None or not getattr(obj, 'wall_or_fence_placement', False):
        return False
    key = getattr(getattr(obj, 'definition', None), 'id', None)
    if key in _art:
        return _art[key][0]
    names = [getattr(a, '__name__', '') for a in (getattr(obj, '_super_affordances', None) or ())]
    art = any(n.startswith('art_View') or 'BulletinBoard' in n or 'CommunityBoard' in n for n in names)
    why = 'art' if art else 'not art'
    if art and claude.any_screen(obj):
        art, why = False, 'a screen'
    if art:
        try:
            from sims4vr import objmenu
            width, height = objmenu._front(obj)[3:5]
            if width < MIN_ART or height < MIN_ART:
                art, why = False, 'too small ({:.2f} x {:.2f} m)'.format(width, height)
            else:
                why = 'art, {:.2f} x {:.2f} m'.format(width, height)
        except Exception as e:
            art, why = False, 'no front: {!r}'.format(e)
    _art[key] = (art, why)
    _log('{}: {}'.format(obj, why))
    return art


def _place(obj, index):
    """Where the board goes: a screen's picture area, or wall art's front."""
    if _cork:
        if is_stand(obj):
            return _stand_front(obj)
        from sims4vr import objmenu
        return objmenu._front(obj)[:5]
    from sims4vr import simsforever
    return simsforever._screen(obj, index)[:5]


def _push(centre, normal, up, width, height):
    global _last
    team, options, poll, picture = _last = _options()
    # window_screen's fields for slot 2: exe = the team's key, title = options, fps = seconds between refreshes,
    # max_width = the picture's width (claudescreen.h kLinear).
    native.window_screen(SLOT, True, centre, normal, up, width, height, team, options, poll, picture, 1)


def _show(obj, index=0, cork=False):
    global _open, _open_index, _cork
    _cork = cork
    centre, normal, up, width, height = _place(obj, index)
    _push(centre, normal, up, width, height)
    _open = getattr(obj, 'id', None)
    _open_index = index
    _log('{} on {} (screen {}): at {}, {:.2f} x {:.2f}, team {}, refresh every {} s'.format(
        'scrumboard' if cork else 'board', obj, index + 1, tuple(round(v, 2) for v in centre), width, height, _last[0],
        _last[2]))


def close(why):
    global _open
    if _open is None:
        return
    _open = None
    native.window_screen(SLOT, False)
    _log('closed ({})'.format(why))


def toggle(obj, index=None):
    obj = claude._owner(obj)
    if obj is None:
        return
    index = index or 0
    if shown_on(obj, index) and not _cork:
        close('you picked Close Linear board')
        return
    if claude._open is not None and claude._open == getattr(obj, 'id', None) and claude._open_index == index:
        _log('Claude is on that screen; the board not opened')
        return
    try:
        from sims4vr import discord
        if discord.shown_on(obj, index):
            _log('Discord is on that screen; the board not opened')
            return
    except Exception:
        pass
    _show(obj, index)


def tick(sim):
    """claude.tick's (every 0.5 s in first person): closes the board when you walked away from the screen or it's
    gone, and passes on the debug menu's changed options."""
    if _open is None:
        return
    import services
    obj = services.object_manager().get(_open)
    if obj is None or sim is None:
        close('the screen is gone' if obj is None else 'not in first person')
        return
    d = claude.too_far(sim, obj)
    if d is not None:
        close('you walked {:.1f} m away from the screen'.format(d))
        return
    if _options() != _last:
        try:
            _push(*_place(obj, _open_index))
        except Exception as e:
            _log('options not passed on: {!r}'.format(e))


def spec():
    return {
        'label': 'Linear board', 'name': 'vr_OpenLinearBoard', 'guid': GUID,
        'category_idle': 0x5653434C4C490001, 'category_open': 0x5653434C4C490002,
        'logo_id': 0x4C4F474F434C4403,
        'offered': lambda obj: native.setting_flag('linear', 'on', True) and claude.any_screen(obj),
        'toggle': toggle,
        'state': lambda obj: (_open is not None and not _cork and getattr(obj, 'id', None) == _open, _open_index),
    }


def toggle_cork(obj, index=None):
    obj = claude._owner(obj)
    if obj is None:
        return
    if _open is not None and _cork and _open == getattr(obj, 'id', None):
        close('you picked Close Linear scrumboard')
        return
    _show(obj, 0, cork=True)


def cork_spec():
    return {
        'label': 'Linear scrumboard', 'name': 'vr_OpenLinearScrumboard', 'guid': CORK_GUID,
        'category_idle': 0x5653434C4C490003, 'category_open': 0x5653434C4C490004,
        'logo_id': 0x4C4F474F434C4403,
        'offered': lambda obj: native.setting_flag('linear', 'on', True) and native.setting_flag('linear', 'cork', True)
        and (is_stand(obj) or is_wall_art(obj)),
        'toggle': toggle_cork,
        'state': lambda obj: (_open is not None and _cork and getattr(obj, 'id', None) == _open, 0),
    }


def start():
    global _affordance, _cork_affordance
    if not native.setting_flag('linear', 'on', True) or native._optional('window_screen') is None:
        return
    if _affordance is None:
        _affordance = claude.make_affordance(spec())
    if _cork_affordance is None:
        _cork_affordance = claude.make_affordance(cork_spec())


def stop():
    close('first person ended')
