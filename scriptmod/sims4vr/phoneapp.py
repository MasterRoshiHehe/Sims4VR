"""The game's own phone menu on your Sim's phone in first person (user
2026-10-18 20:35Z: "take the game's existing phone menu and convert it to run
on our custom phone").

Taking the phone out (phone.take_out) builds the menu exactly as the game's
phone button does (server_commands/interaction_commands.py
generate_phone_choices, the command interactions.phone_choices): the phone
tests (PhoneTuning.DISABLE_PHONE_TESTS), a ChoiceMenu of the played Sim's
phone affordances (Sim tuning _phone_affordances; this is also where other
mods put their apps), set on the client, and the same PieMenuCreate message
(create_pie_menu_message with do_phone_tests). Only it is not sent to the
game's UI: the DLL draws the phone itself (hud.cpp drawPhone, the layout of
the game's PhonePanel), from what this module hands it (native.phone_content):

  "phone" \x1f <silenced 0/1> \x1f <disabled tooltip json or "">
  "cat"   \x1f <node> \x1f <parent node, -1 the home screen> \x1f <icon instance>
          \x1f <name json> \x1f <priority>
  "item"  \x1f <node> \x1f <parent node> \x1f <name json> \x1f <disabled text json or "">
          \x1f <notification 0/1> \x1f <priority>

Records split by 0x1e. The tree is the game's InteractionMenuData.GenerateTree:
each item under its category (PieMenuCategory.category_key), each category
under its _parent; a collapsible category holding one entry is replaced by it
(with the category's priority). The DLL sorts as the game does (it has the
texts). A press on item `node` comes back as control 100000 + node and is
selected as the game's interactions.select does (Client.select_interaction).
Control 77 toggles the phone's silence (ui.toggle_silence_phone).

  "colors" \x1f <chosen index or -1> \x1f <swatch icon instances, comma separated>

The wallpaper colours (user 2026-10-18 21:23Z): the game's
PhoneBackgroundColorSelector lists PhoneTuning.PHONE_BACKGROUND_COLOR_VARIATION_TUNING
(bg_image + swatch icon). That list is exported to the client only (empty in
Python), so COLORS below is copied from the game's own SimData of
sims.phone_tuning (re/tuning/simdata.jsonl, 6C2298980B4F1732; no required
packs). The game keeps the choice per Sim in UserSetting.ini
("<sim id>#phone.background_image#string") from its UI, which Python can't
write to (the game rewrites the file), so a choice made on the VR phone is
kept in sims4vr_phone.ini beside the script and wins over the game's.
Control 90000 + i picks colour i.
"""
from sims4vr import native

_IMAGE = 0x00B2D882  # bg_image / icon resource type
COLORS = (  # (bg_image, icon) in the game's order
    (0x47FDC4D22B931D50, 0x7BAA83A4661EB5C2), (0x01259F89F7880963, 0x8DD8B09FAA173046),
    (0xD06BA43F16818C3F, 0x79DA57057115FD5C), (0x7BEC9425066B6BBE, 0x7988071B4728A65D),
    (0xFD63E1C6ABAD4CC6, 0x8125E1D9B5B0F417), (0x842FAAC00D218647, 0xE6A037C68D08DE3C),
    (0x1AE37A2357514D74, 0x2DAFF68752C09AB9), (0x817A572C7C3FC340, 0x6AFCE19BEEFD2969),
    (0x71FAC82F699293D2, 0xE64932B75A811A15), (0xDD9E3C23346F29A5, 0xE1B184872843D31C),
    (0xA39A300D2A12C673, 0xE57764E89BFB75FA), (0x7D77E1D4691764B1, 0xC098956EF05D93F2),
    (0x109A32E3A01C2834, 0xC00EC6929C9D0BCD), (0xF3F2C30B4324D4F7, 0xA942FC3BC04892DC),
    (0x987A6AE6908D07FD, 0x67CD14F81DA07374), (0x86A25618E3A12209, 0x31956412FAF8C718),
    (0x9F335305DA64AAC5, 0x2595C946D4B10818), (0x23EAA48930EB0D09, 0xE2482E798D217FEA),
    (0x16DB72255DE528B3, 0x8AFD4D1B511611B4),
)
_chosen = None  # {sim id: bg_image} picked on the VR phone (sims4vr_phone.ini)
_choice_ids = {}  # node -> aop id of the choice menu set on the client
_revision = None  # that menu's revision (the game's server_reference_id)
_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 120:
        native._log('phoneapp: ' + message)


def _log_error(where, e):
    global _errors
    _errors += 1
    if _errors <= 8:
        native._log('phoneapp: {} failed: {!r}'.format(where, e))


def _client():
    import services
    try:
        return services.client_manager().get_first_client()
    except Exception:
        return None


def _silenced():
    import services
    try:
        return bool(services.current_zone().ui_dialog_service.is_phone_silenced)
    except Exception:
        return False


def _choices_path():
    """sims4vr_phone.ini in the folder holding the script (the mod's own folder)."""
    import os
    path = os.path.abspath(__file__)
    while True:
        parent = os.path.dirname(path)
        if parent == path:
            return None
        if path.lower().endswith('.ts4script'):
            return os.path.join(parent, 'sims4vr_phone.ini')
        path = parent


def _load_choices():
    global _chosen
    if _chosen is not None:
        return _chosen
    _chosen = {}
    try:
        path = _choices_path()
        if path:
            import os
            if os.path.isfile(path):
                with open(path, 'r', encoding='utf-8') as f:
                    for line in f:
                        name, sep, value = line.partition('=')
                        if sep and name.strip().endswith('#background'):
                            _chosen[int(name.strip().split('#')[0])] = int(value.strip())
    except Exception as e:
        _log_error('reading sims4vr_phone.ini', e)
    return _chosen


def chosen_background(sim_id):
    """The wallpaper picked on the VR phone for this Sim, or None (then the game's choice)."""
    return _load_choices().get(int(sim_id))


def _color_index(sim):
    from sims4vr import buildbuy
    chosen = chosen_background(sim.id)
    if chosen is None:
        try:
            chosen = buildbuy._chosen_background(sim.id)
        except Exception:
            chosen = None
    if chosen is None:
        return 0  # the game shows its first colour when nothing was chosen
    for i, (image, _) in enumerate(COLORS):
        if image == chosen:
            return i
    return -1


def set_color(index):
    """Wallpaper colour `index` picked on the phone (PhoneBackgroundColorSelector)."""
    client = _client()
    sim = client.active_sim if client is not None else None
    if sim is None or not 0 <= index < len(COLORS):
        return
    choices = _load_choices()
    choices[int(sim.id)] = COLORS[index][0]
    try:
        path = _choices_path()
        if path:
            with open(path, 'w', encoding='utf-8') as f:
                f.write('; Phone wallpaper picked on the Sims4VR phone, per Sim (sim id#background = picture instance)\n')
                for sim_id, image in sorted(choices.items()):
                    f.write('{}#background = {}\n'.format(sim_id, image))
    except Exception as e:
        _log_error('saving sims4vr_phone.ini', e)
    _log('wallpaper colour {} ({:016x}) for {}'.format(index, COLORS[index][0], sim))
    if _last is not None and _last[0] == sim.id:
        _send_records(sim, _last[1])  # only the colour and the wallpaper change: no new menu
    else:
        send()


_job = None  # the phone menu being built a slice a tick: [client, sim, context, choice menu, aops, next]
_BUDGET = 0.012  # seconds of aop tests a tick (the whole menu at once stalled the game 140-280 ms)
_last = None  # (sim id, records) of the menu sent last: shown at once while the fresh one is tested


def _start_menu(client, sim):
    """generate_phone_choices, its tests spread over ticks (poll). A finished
    message (phone disabled, queue full) comes back at once; else None."""
    global _job
    from event_testing.resolver import SingleSimResolver
    from interactions.choices import ChoiceMenu
    from server_commands import interaction_commands as ic
    from sims.phone_tuning import PhoneTuning
    _job = None
    resolver = SingleSimResolver(sim.sim_info)
    for phone_test in PhoneTuning.DISABLE_PHONE_TESTS:
        if resolver(phone_test.test):
            return ic.create_pie_menu_message(sim, None, 0, None, failure_tooltip=phone_test.tooltip(sim))
    if not (sim.queue is None or sim.queue.can_queue_visible_interaction()):
        return ic.create_pie_menu_message(sim, None, 0, ic.PieMenuActions.INTERACTION_QUEUE_FULL_TOOLTIP,
                                          do_phone_tests=True)
    context = client.create_interaction_context(sim, shift_held=False)
    _job = [client, sim, context, ChoiceMenu(sim), list(sim.potential_phone_interactions(context)), 0]
    return None


def poll():
    """Controls tick: the phone menu's next slice of tests; sent when done."""
    global _job
    if _job is None:
        return
    import time
    client, sim, context, menu, aops, at = _job
    if client.active_sim is not sim:
        _job = None
        return
    started = time.perf_counter()
    try:
        while at < len(aops) and time.perf_counter() - started < _BUDGET:
            menu.add_potential_aops(None, context, aops[at:at + 1], None)
            at += 1
    except Exception as e:
        _job = None
        _log_error('testing the phone menu', e)
        return
    _job[5] = at
    if at < len(aops):
        return
    _job = None
    from server_commands import interaction_commands as ic
    try:
        t0 = time.perf_counter()
        client.set_choices(menu)
        msg = ic.create_pie_menu_message(sim, menu, 0, ic.PieMenuActions.SHOW_PIE_MENU, do_phone_tests=True)
        t1 = time.perf_counter()
    except Exception as e:
        _log_error('building the phone menu', e)
        return
    _send_message(sim, msg)
    _log('menu message {:.0f} ms, sent to the DLL {:.0f} ms ({} aops tested over several ticks)'.format(
        (t1 - t0) * 1000, (time.perf_counter() - t1) * 1000, len(aops)))


def _category(key):
    import services
    import sims4.resources
    try:
        return services.get_instance_manager(sims4.resources.Types.PIE_MENU_CATEGORY).get(key)
    except Exception:
        return None


def _tree(sim, msg):
    """The game's GenerateTree: nodes {key: [kind, parent key, children, data]}."""
    from sims4vr import objmenu
    root = {'kind': 'root', 'children': [], 'priority': 0}
    cats = {0: root}

    def fetch(key):
        if key in cats:
            return cats[key]
        cat = _category(key)
        node = {'kind': 'cat', 'children': [], 'key': key, 'priority': 1, 'icon': 0, 'name': '',
                'collapsible': True}
        cats[key] = node
        if cat is not None:
            node['priority'] = int(getattr(cat, '_display_priority', 1) or 0)
            node['collapsible'] = bool(getattr(cat, '_collapsible', True))
            icon = getattr(cat, '_icon', None)
            if icon is not None:
                node['icon'] = int(icon.instance)
                try:
                    from sims4vr import hudinfo
                    hudinfo._icon_types[int(icon.instance)] = int(icon.type)
                except Exception:
                    pass
            try:
                name = cat._display_name(sim)
                if name is not None:
                    node['name'] = objmenu._loc(name)
            except Exception as e:
                _log_error('category {} name'.format(key), e)
            parent = getattr(cat, '_parent', None)
            parent_key = int(parent.guid64) if parent is not None else 0
        else:
            parent_key = 0
        fetch(parent_key)['children'].append(node)
        return node

    for item in msg.items:
        node = {'kind': 'item', 'children': [], 'id': item.id, 'priority': int(item.pie_menu_priority),
                'name': objmenu._loc(item.loc_string),
                'disabled': objmenu._loc(item.disabled_text) if item.HasField('disabled_text') else '',
                'notif': bool(item.display_notification)}
        fetch(int(item.category_key) if item.category_key else 0)['children'].append(node)

    def collapse(node):
        for k, child in enumerate(node['children']):
            while child['kind'] == 'cat' and len(child['children']) == 1 and child['collapsible']:
                only = child['children'][0]
                only['priority'] = child['priority']
                child = only
            node['children'][k] = child
            collapse(child)
    collapse(root)
    return root


def send():
    """The phone's menu for the played Sim, to the DLL (phone taken out): the
    tests run a slice a tick (poll) and the menu goes when they are done."""
    if native._optional('phone_content') is None:
        return
    client = _client()
    sim = client.active_sim if client is not None else None
    if sim is None:
        _choice_ids.clear()
        native.phone_content(b'phone\x1f0\x1f')
        return
    if _last is not None and _last[0] == sim.id:
        _send_records(sim, _last[1])  # the last menu now, the fresh one when its tests are done
    try:
        msg = _start_menu(client, sim)
    except Exception as e:
        _log_error('building the phone menu', e)
        return
    if msg is not None:
        _send_message(sim, msg)


def _colors_record(sim):
    from sims4vr import hudinfo
    for _, icon in COLORS:
        hudinfo._icon_types[icon] = _IMAGE
    return 'colors\x1f{}\x1f{}'.format(_color_index(sim), ','.join(str(icon) for _, icon in COLORS))


def _send_records(sim, records):
    """The menu to the DLL, its colour line made again (the wallpaper colour may have changed)."""
    out = []
    for record in records:
        if record.startswith('phone\x1f'):
            parts = record.split('\x1f')
            parts[1] = '1' if _silenced() else '0'
            record = '\x1f'.join(parts)
        elif record.startswith('colors\x1f'):
            try:
                record = _colors_record(sim)
            except Exception as e:
                _log_error('wallpaper colours', e)
        out.append(record)
    native.phone_content('\x1e'.join(out).encode('utf-8'))
    try:
        from sims4vr import buildbuy
        buildbuy.want_wallpaper()
    except Exception as e:
        _log_error('phone background', e)


def _send_message(sim, msg):
    global _revision, _last
    _choice_ids.clear()
    _revision = None
    from sims4vr import objmenu
    disabled = objmenu._loc(msg.disabled_tooltip) if msg.HasField('disabled_tooltip') else ''
    records = ['phone\x1f{}\x1f{}'.format(1 if _silenced() else 0, disabled)]
    records.append('colors\x1f')  # made in _send_records
    if not disabled:
        _revision = msg.server_reference_id
        root = _tree(sim, msg)
        counter = [0]

        def emit(node, parent):
            index = counter[0]
            counter[0] += 1
            if node['kind'] == 'cat':
                records.append('cat\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                    index, parent, node['icon'], node['name'], node['priority']))
            else:
                _choice_ids[index] = node['id']
                records.append('item\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                    index, parent, node['name'], node['disabled'], 1 if node['notif'] else 0, node['priority']))
            for child in node['children']:
                emit(child, index)
        for child in root['children']:
            emit(child, -1)
        _log('{} entries for {} ({} apps, menu {})'.format(
            len(msg.items), sim, sum(1 for c in root['children'] if c['kind'] == 'cat'), _revision))
    else:
        _log('the phone is disabled for {} now'.format(sim))
    _last = (sim.id, records)
    _send_records(sim, records)


def clear():
    global _job
    _job = None
    _choice_ids.clear()
    if native._optional('phone_content') is not None:
        native.phone_content(b'')


def select(node):
    """Item `node` pressed on the phone: the game's interactions.select."""
    choice = _choice_ids.get(node)
    client = _client()
    if choice is None or client is None or _revision is None:
        _log('press on {} with no menu for it'.format(node))
        return
    try:
        result = client.select_interaction(choice, _revision)
        _log('picked {} (choice {}): {}'.format(node, choice, result))
    except Exception as e:
        _log_error('picking {}'.format(node), e)
    # The game's panel hides once something is picked; the phone shows its
    # home screen again with a fresh menu (the old one is used up).
    send()


def toggle_silence():
    import services
    try:
        services.current_zone().ui_dialog_service.toggle_is_phone_silenced()
    except Exception as e:
        _log_error('silence', e)
    client = _client()
    sim = client.active_sim if client is not None else None
    if sim is not None and _last is not None and _last[0] == sim.id:
        _send_records(sim, _last[1])  # only the button changes
    else:
        send()
