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
# The own-drawn pie's options (first person): (option id, server reference) by the index the DLL sends back.
_own_items = []
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
                _send_own(msg, frame.f_locals)
                native.pie_menu(len(msg.items))
                if msg.items:
                    _opened_on(frame.f_locals.get('pick_target'), frame.f_locals.get('sim'))
        except Exception as e:
            _log_error('create_pie_menu_message', e)
        return msg

    hooks.hook('pie', interaction_commands, 'create_pie_menu_message', create_pie_menu_message)


_bad_categories = set()  # mod categories whose name couldn't be made (logged once)


def _send_own(msg, local):
    """The pie as our own card in first person (hud.cpp drawPie; user 2026-10-22: the cut of the game's pie took
    in other boxes): the game's own PieMenuCreate, plus what the PieMenu widget gets from the game service
    GetInteractionCategoryList (the categories the items name, up to the root). The DLL builds the tree as
    InteractionMenuData.GenerateTree does (re/liveui/components/PieMenu.md). b'': none (the game's pie cut out)."""
    global _own_items
    _own_items = []
    try:
        if not native.setting_flag('ui', 'own_pie', True) or not hooks.in_first_person() or not msg.items or \
                msg.HasField('disabled_tooltip'):
            if msg.items:
                native._log('pie: own pie not sent (setting {}, first person {})'.format(
                    native.setting_flag('ui', 'own_pie', True), hooks.in_first_person()))
            native.pie_content(b'')
            return
        from sims4vr import hudinfo, objmenu
        from sims4.resources import Types
        categories = services.get_instance_manager(Types.PIE_MENU_CATEGORY)
        moods = services.get_instance_manager(Types.MOOD)
        active = services.get_active_sim()
        mood = None
        try:
            mood = active.get_mood() if active is not None else None
        except Exception:
            mood = None
        pick_type = int(local.get('pick_type', 0) or 0)
        target = int(local.get('target_id', 0) or 0)
        from server.pick_info import PickType
        is_sim = pick_type == int(PickType.PICK_SIM)
        front = (not bool(local.get('shift', 0)) and not msg.supress_social_front_page and is_sim and target != msg.sim)
        records = ['pie\x1f{}\x1f{}'.format(1 if front else 0, msg.server_reference_id)]
        sent = set()

        def category(key):
            while key and key not in sent:
                cat = categories.get(key)
                if cat is None:
                    return
                sent.add(key)
                try:
                    name = cat._display_name()
                    try:
                        del name.tokens[:]
                        name.tokens.extend(msg.category_tokens)
                    except Exception:
                        pass
                    name_json = objmenu._loc(name)
                except Exception as e:
                    # A mod's category with a string key too big for a LocalizedString (user 2026-10-23: "Value
                    # out of range: 58290212367" lost the whole pie to the cut): its key's low 32 bits, as the
                    # game's string tables are keyed.
                    string_id = int(getattr(cat._display_name, '_string_id', 0) or 0) & 0xffffffff
                    name_json = '{{"hash": {}}}'.format(string_id)
                    if key not in _bad_categories:
                        _bad_categories.add(key)
                        native._log('pie: category {} ({}) name not made ({!r}); key {:#x} used'.format(
                            key, getattr(cat, '__name__', '?'), e, string_id))
                override = ''
                try:
                    if mood is not None:
                        data = (getattr(cat, 'mood_overrides', None) or {}).get(mood)
                        if data is not None and getattr(data, 'name_override', None) is not None:
                            override = objmenu._loc(data.name_override())
                except Exception:
                    override = ''
                parent = cat._parent.guid64 if cat._parent is not None else 0
                icon = hudinfo._icon(cat._icon) if cat._icon is not None else 0
                records.append('cat\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                    key, parent, 1 if cat._collapsible else 0, int(cat._display_priority),
                    1 if cat.always_show_disabled_interactions else 0, icon, name_json, override))
                key = parent

        for i, item in enumerate(msg.items):
            _own_items.append((item.id, msg.server_reference_id))
            key = int(getattr(item, 'category_key', 0) or 0)
            if key and categories.get(key) is None:
                key = 0
            category(key)
            disabled = objmenu._loc(item.disabled_text) if item.HasField('disabled_text') and item.disabled_text.hash else ''
            mood_index = 0
            if getattr(item, 'mood', 0):
                mood_index = hudinfo._mood_index(moods.get(item.mood))
            icons = ','.join(str(hudinfo._picture(info)) for info in item.icon_infos)
            records.append('item\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                i, key, int(item.pie_menu_priority), max(0.0, float(item.score)), mood_index,
                int(getattr(item, 'mood_intensity', 0) or 0), objmenu._loc(item.loc_string), disabled, icons))
        native.pie_content('\x1e'.join(records).encode('utf-8'))
    except Exception as e:
        _own_items = []
        native.pie_content(b'')
        _log_error('own pie content', e)


def send_dialog(answers):
    """A popup's answers (objmenu.py, a Sim asking you something) as our pie at your palm: one option each (their
    texts as JSON for the DLL's string tables), no categories; a pick answers the popup (select_own)."""
    global _own_items
    try:
        _own_items = [('dialog', i) for i in range(len(answers))]
        records = ['pie\x1f0\x1f0']
        for i, text in enumerate(answers):
            records.append('item\x1f{}\x1f0\x1f{}\x1f0\x1f0\x1f0\x1f{}\x1f\x1f'.format(i, len(answers) - i, text))
        native.dialog_pie()
        native.pie_content('\x1e'.join(records).encode('utf-8'))
        native.pie_menu(len(answers))
        native._log('pie: a popup\'s {} answers sent as the pie'.format(len(answers)))
    except Exception as e:
        _own_items = []
        _log_error('popup answers as the pie', e)


def select_own(index):
    """An option picked on the own-drawn pie (the DLL, control 5000000 + index): what the PieMenu widget sends,
    interactions.select [id, server_reference_id] (PieMenuMain.OnOptionSelected)."""
    try:
        if not 0 <= index < len(_own_items):
            return
        option, reference = _own_items[index]
        if option == 'dialog':
            from sims4vr import objmenu
            objmenu.answer_popup(reference)
            native._log('pie: popup answer {} picked on the pie'.format(reference))
            return
        client = services.client_manager().get_first_client()
        if client is None:
            return
        client.select_interaction(option, reference)
        native._log('pie: own pie option {} picked (id {})'.format(index, option))
    except Exception as e:
        _log_error('own pie pick', e)


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


def open_on(obj):
    """A pie menu on an object (the phone's Inventory: what a click on it in the game's inventory panel asks,
    interactions.choices on the object), answered at your hand like any pie menu in first person."""
    try:
        sim = services.get_active_sim()
        client = services.client_manager().get_first_client()
        if sim is None or client is None or obj is None:
            return
        if _last_reference is None:
            native._log('pie: inventory pie menu needs one click on anything first (the game\'s menu has no click to answer yet)')
            return
        from server.pick_info import PickType
        position = sim.position
        choices = _debug_choices if _choices is not None else interaction_commands.generate_choices
        items = choices(obj.id, PickType.PICK_OBJECT, position.x, position.y, position.z, services.active_lot_id() or 0,
                        sim.level, 0, 0, 0, _last_reference, 0, 0, True, _connection=client.id)
        native._log('pie: pie menu on {} asked for ({} options)'.format(obj, items))
    except Exception as e:
        _log_error('object pie menu', e)


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
