"""Tells the DLL when the game opens a pie menu for a click into the world and
when you pick from it, so in first person the menu can show at your hand
(vr.cpp, "Pie menu at your hand").

interactions.choices (generate_choices) builds the menu with the module-level
create_pie_menu_message, looked up at call time, so wrapping that name catches
every world and Sim pie menu; the phone's menu (generate_phone_choices) uses
it too and is left out. interactions.select goes through
Client.select_interaction.

With the debug menu's Cheat pie menus switch on (debug_pies, set by controls.py),
every world and Sim pie menu opens as the game's own Shift+click cheat menu:
interactions.choices runs with its shift argument set, so the game itself
picks SHOW_DEBUG_PIE_MENU and builds the menu with the cheat interactions,
exactly as for a real Shift+click. No key is pressed. The command table holds
the game's invoke_command closure around generate_choices; that closure's
cell is pointed at our wrapper (cells are writable since Python 3.7).
"""
import gc
import inspect
import sys
import time

import services
from server.client import Client
from server_commands import interaction_commands

from sims4vr import hooks, native

_errors = 0
# The PieMenu widget's request id of the last pie menu it asked for: it shows
# a menu only for its latest request (PieMenuMain.HandlePieMenuCreate).
_last_reference = None
_self_pie = False  # open_self is building your own pie menu

# Every pie menu is the Shift+click cheat menu while this is on (controls.py).
debug_pies = False
_choices = None  # the game's own generate_choices
_choices_signature = None
_forced = 0


def _log_error(where, e):
    global _errors
    _errors += 1
    if _errors <= 5:
        native._log('pie: {} failed: {!r}'.format(where, e))


def _wrap_create():

    def create_pie_menu_message(original, *args, **kwargs):
        global _last_reference
        msg = original(*args, **kwargs)
        try:
            frame = hooks.caller_frame(1)
            if frame.f_code.co_name == 'generate_choices':
                if _self_pie:
                    # Your own pie as the game's self-click has it: the widget skips the
                    # social front page for a click on the actor itself, but it still holds
                    # the pick of your last click, which may be another Sim.
                    msg.supress_social_front_page = True
                else:
                    _last_reference = args[2] if len(args) > 2 else kwargs.get('reference_id')
                native.pie_menu(len(msg.items))
                if msg.items:
                    _opened_on(frame.f_locals.get('pick_target'), frame.f_locals.get('sim'))
        except Exception as e:
            _log_error('create_pie_menu_message', e)
        return msg

    hooks.hook('pie', interaction_commands, 'create_pie_menu_message', create_pie_menu_message)


def _opened_on(target, sim):
    """A pie menu for a click on target: the TV remote goes into your hand (remote.py)."""
    try:
        from sims4vr import remote
        remote.pie_opened(target, sim)
    except Exception as e:
        _log_error('remote for the pie menu', e)


def _wrap_select():

    def select_interaction(original, self, *args, **kwargs):
        try:
            native.pie_menu(0)
        except Exception as e:
            _log_error('select_interaction', e)
        return original(self, *args, **kwargs)

    hooks.hook('pie', Client, 'select_interaction', select_interaction)


def open_self():
    """Your own pie menu (first person; the DLL asks: stomach poke or the laser on
    your palm): what a click on your own Sim opens. Sends the PieMenu widget's own
    request (interactions.choices with a Sim pick on your Sim) under the widget's
    last request id, so it shows the answer at the mouse, as for that click."""
    global _self_pie
    try:
        from sims4vr import body
        sim = services.get_active_sim()
        if sim is None or not body.following() or body.following() != sim.id:
            native._log('pie: own pie menu refused (not in first person in the active Sim)')
            return
        if _last_reference is None:
            native._log('pie: own pie menu needs one click on anything first (the game\'s menu has no click to answer yet)')
            return
        client = services.client_manager().get_first_client()
        if client is None:
            return
        from server.pick_info import PickType
        position = sim.position
        choices = _debug_choices if _choices is not None else interaction_commands.generate_choices
        _self_pie = True
        try:
            items = choices(sim.id, PickType.PICK_SIM, position.x, position.y, position.z, services.active_lot_id() or 0,
                            sim.level, 0, 0, 0, _last_reference, 0, 0, True, _connection=client.id)
        finally:
            _self_pie = False
        native._log('pie: own pie menu asked for ({} options, request {})'.format(items, _last_reference))
    except Exception as e:
        _self_pie = False
        _log_error('own pie menu', e)


def cheats_enabled():
    service = services.get_cheat_service()
    return bool(service is not None and service.cheats_enabled)


def _debug_choices(*args, **kwargs):
    # Module globals only (no closure), so the cell search below can't find this function's own cells.
    global _forced
    if debug_pies:
        try:
            if cheats_enabled():
                bound = _choices_signature.bind(*args, **kwargs)
                if not bound.arguments.get('shift'):
                    bound.arguments['shift'] = 1
                    args, kwargs = bound.args, bound.kwargs
                    _forced += 1
                    if _forced <= 5:
                        native._log('pie: opened as the Shift+click cheat menu')
        except Exception as e:
            _log_error('debug pie', e)
    return _choices(*args, **kwargs)


def _cell_type():
    nothing = None
    return type((lambda: nothing).__closure__[0])


def _wrap_choices():
    global _choices, _choices_signature
    original = getattr(interaction_commands, 'generate_choices', None)
    if original is None:
        native._log('pie: generate_choices not found')
        return
    _choices = original
    _choices_signature = inspect.signature(original)
    cell_type = _cell_type()
    started = time.perf_counter()
    swapped = 0
    for ref in gc.get_referrers(original):
        if type(ref) is cell_type and ref.cell_contents is original:
            ref.cell_contents = _debug_choices
            swapped += 1
    native._log('pie: Shift+click cheat menu hook {} ({} cell{}, {:.0f} ms)'.format(
        'ready' if swapped else 'NOT FOUND: debug pie menus need a real Shift+click', swapped,
        '' if swapped == 1 else 's', (time.perf_counter() - started) * 1000))


_wrap_create()
_wrap_select()
_wrap_choices()
