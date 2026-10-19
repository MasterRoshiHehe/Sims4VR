"""The played Sim's needs, moodlets, action queue and wants for our
first-person HUD (src/hud.cpp draws them in the game's own art), read the way
the game's own widgets get them:
- needs: TraitTracker.sort_and_send_commodity_list's list (a trait's
  ui_commodity_sort_override, else ui_sort_order), visible commodities only;
  each bar in its current CommodityState's fill / background colour, with the
  state's icon (statistics/commodity.py);
- moodlets: the visible buffs (BuffComponent), tinted by their mood;
- queue: the Sim's UIManager (sims/ui_manager.py) as the game's own messages
  describe it (_build_interaction_msg), laid out as the game's
  InteractionQueue does: running items first, side actions as badges, each
  with its picture as the game's UI loads it (an icon, or a Sim's or object's
  thumbnail from the game's thumbnail system, native.thumb) and its tooltip;
  a press on one cancels it as interactions.cancel_si does (cancel_common);
- wants: the WhimsTracker's slots (send_goals_update), each goal's display_icon.
Icons are read from the game's packages only when the DLL asks for them
(native.hud_missing_icons).
"""
import struct
import time

import services

from sims4vr import hooks, native

# hud.h Kind
NEEDS, MOODLETS, QUEUE, WANTS, QUEUE_BADGES = range(5)
# The moods in the order the DLL knows them (tools/pack_hud_art.py MOODS).
MOODS = ['default', 'angry', 'asleep', 'bored', 'confident', 'dazed', 'embarrassed', 'energized', 'fine', 'flirty',
         'focused', 'happy', 'inspired', 'playful', 'sad', 'scared', 'tense', 'uncomfortable']
_MOOD_WORDS = [('uncomfortable', 17), ('embarrass', 6), ('energ', 7), ('asleep', 2), ('sleep', 2), ('stress', 16),
               ('tense', 16), ('confident', 4), ('inspired', 12), ('playful', 13), ('focus', 10), ('flirt', 9),
               ('happy', 11), ('angry', 1), ('bored', 3), ('dazed', 5), ('scared', 15), ('sad', 14), ('fine', 8)]
EVERY = 0.5  # seconds between reads of the game's values

_last = 0
_sent = [None] * 5
_queue_ids = []  # interaction ids, in the order the DLL shows them
_badge_ids = []  # per queue button, its side action's interaction id (0 none)
_want_whims = []  # the whims of the wants shown, in order
_icon_types = {}  # icon instance -> resource type
_waiting = {}  # icon instance -> when first asked (still being looked up)
_found_log = []  # the first icons found (logged)
_errors = 0
_texts = {NEEDS: [], MOODLETS: [], WANTS: [], QUEUE: []}  # per item: (title, text, reason) LocalizedStrings or None, minutes left (-1 none)
_sent_texts = [None] * 5


def _mood_index(mood):
    if mood is None:
        return 0
    name = getattr(mood, '__name__', '') or type(mood).__name__
    name = name.lower()
    for word, index in _MOOD_WORDS:
        if word in name:
            return index
    return 0


def _int(value, default=0):
    try:
        return int(value) & 0xffffffff
    except (TypeError, ValueError):
        return default


def _icon(key):
    """A resource key's instance (remembering its type for reading it)."""
    if key is None:
        return 0
    instance = getattr(key, 'instance', 0) or 0
    if instance:
        _icon_types[instance] = getattr(key, 'type', 0) or 0
    return instance


def _pack(items):
    return b''.join(struct.pack('<QIfI', i & 0xffffffffffffffff, c & 0xffffffff, float(v), f & 0xffffffff)
                    for i, c, v, f in items[:8])


def _needs(sim_info, texts):
    import operator
    commodities, _ = sim_info.get_separate_initial_commodity_sets()
    order = []
    tracker = getattr(sim_info, 'trait_tracker', None)
    for trait in getattr(tracker, 'equipped_traits', ()) or ():
        override = getattr(trait, 'ui_commodity_sort_override', None)
        if override:
            order = [c for c in override if c in commodities]
            break
    if not order:
        order = sorted(commodities, key=operator.attrgetter('ui_sort_order'))
    items = []
    for commodity in order:
        if not commodity.visible:
            continue
        stat = sim_info.commodity_tracker.get_statistic(commodity)
        if not stat or not stat.is_visible_commodity():
            continue
        value = stat.get_value()
        low, high = stat.min_value, stat.max_value
        fraction = (value - low) / (high - low) if high > low else 0
        fill, back, icon = 0xff58c838, 0, 0
        states = getattr(stat, 'commodity_states', None)
        index = getattr(stat, '_current_state_index', None)
        if states and index is not None and 0 <= index < len(states):
            state = states[index]
            fill = getattr(state, '_fill_color', None) or fill
            back = getattr(state, '_background_color', None) or 0
            icon = _icon(getattr(state, 'icon', None))
        if not icon:
            icon = _icon(getattr(stat, 'icon', None))
        items.append((icon, _int(fill, 0xff58c838), fraction, _int(back)))
        # The bar's tooltip: the need's name and its state's description.
        state = states[index] if states and index is not None and 0 <= index < len(states) else None
        texts.append((getattr(stat, 'stat_name', None), getattr(state, '_data_description', None), None, -1))
    return items


def _moodlets(sim_info, texts):
    items = []
    for buff in sim_info.Buffs:
        if not getattr(buff, 'visible', False):
            continue
        icon = _icon(getattr(buff, 'icon', None))
        if not icon:
            continue
        items.append((icon, _mood_index(getattr(buff, 'mood_type', None)), 0, 0))
        # The moodlet's tooltip as the game's: its name, description, why it
        # is there (BuffComponent._create_buff_update_msg's reason) and the
        # time it has left.
        name = None
        try:
            name = buff.buff_name(sim_info) if callable(buff.buff_name) else buff.buff_name
        except Exception:
            name = None
        reason = getattr(buff, 'buff_reason', None) or getattr(buff, 'default_buff_reason', None)
        minutes = -1
        try:
            if getattr(buff, 'show_timeout', True):
                timeout, rate = buff.get_timeout_time()
                if timeout:
                    now = services.time_service().sim_now.absolute_ticks()
                    from date_and_time import TimeSpan
                    left = TimeSpan(max(0, timeout - now)).in_minutes()
                    minutes = int(left / rate) if rate and rate > 0 else int(left)
        except Exception:
            minutes = -1
        texts.append((name, getattr(buff, 'buff_description', None), reason, minutes))
    return items


def _send_texts(kind):
    """The tooltips of the needs, moodlets or wants (hud_texts), when they change."""
    from sims4vr import objmenu
    if native._optional('hud_texts') is None:
        return
    records = []
    for title, text, reason, minutes in _texts[kind][:16]:
        fields = []
        for value in (title, text, reason):
            try:
                fields.append(objmenu._loc(value) if value is not None and hasattr(value, 'ListFields') else '')
            except Exception:
                fields.append('')
        fields.append(str(minutes))
        records.append('\x1f'.join(fields))
    data = '\x1e'.join(records).encode('utf-8')
    if data != _sent_texts[kind]:
        _sent_texts[kind] = data
        native.hud_texts(kind, data)


# The thumbnail pictures (img://thumbs/...) the HUD shows: id -> path.
_thumbs = {}
# The game's queue tooltip of a side action queued as its own button
# (KeysGP.SUPER_MIXER_TOOLTIP, "{0.String}\n({1.String})": the interaction it
# belongs to, the side action).
SUPER_MIXER_TOOLTIP = 0xAF54BB7F


def _thumb(path):
    """A picture from the game's thumbnail system, asked for as the game's UI
    asks (ImageUtils.as builds the same paths): its HUD picture id (FNV-1a of
    the path; never an icon resource's), or 0 with a DLL that can't."""
    if native._optional('thumb') is None or not native.setting_flag('ui', 'thumbnails', True):
        return 0  # (an older DLL, or [ui] thumbnails = 0: no Sim or object pictures)
    h = 0xcbf29ce484222325
    for b in ('thumbs/' + path).encode('utf-8'):
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    _thumbs[h] = path
    try:
        native._optional('thumb')(h, path)
    except Exception as e:
        native._log('hudinfo: thumbnail {} failed: {!r}'.format(path, e))
        return 0
    return h


def _sim_is_baby(sim_id):
    try:
        from sims.sim_info_types import Age
        sim_info = services.sim_info_manager().get(sim_id)
        return sim_info is not None and sim_info.age == Age.BABY
    except Exception:
        return False


def _picture(icon_info, size='_m', baby_check=True):
    """The picture of an IconInfo message as the game's UI loads it
    (ImageUtils.CreateIconInfoImageUrl): its icon resource if it has one, else
    the thumbnail of its object (with the object's painting, effect, geometry
    and material state and colours), of its catalog definition, or of its Sim
    (a baby's own picture). 0: none."""
    if icon_info is None:
        return 0
    icon = icon_info.icon if icon_info.HasField('icon') else None
    if icon is not None and icon.instance:
        _icon_types[icon.instance] = icon.type
        return icon.instance
    obj = icon_info.icon_object if icon_info.HasField('icon_object') else None
    definition = icon_info.icon_object_def if icon_info.HasField('icon_object_def') else None

    texture = getattr(icon_info, 'texture_id', 0)
    canvas = getattr(icon_info, 'canvas_state_type', 0)
    effect = getattr(icon_info, 'texture_effect', 0)
    colours = getattr(icon_info, 'multicolor', ())

    def states(path, geo, material):
        if texture:
            if canvas == 0:  # IconInfo.CANVAS_STATE_PAINTING_CROSSSTITCH
                path += '_p{}'.format(texture)
            elif canvas == 1:  # IconInfo.CANVAS_STATE_PUZZLE
                path += '_z{}'.format(texture)
        if effect:
            path += '_e{}'.format(effect)
        if geo:
            path += '_g{}'.format(geo)
        if material:
            path += '_m{}'.format(material)
        return path

    if obj is not None and obj.manager_id == 1:  # ManagerObjectId.MGR_OBJECT
        path = states('objects/{}{}'.format(obj.object_id, size), definition.geo_state_hash if definition else 0,
                      definition.material_hash if definition else 0)
        if len(colours):
            path += '_s' + ''.join('{}:{}-{}-{}'.format(i + 1, int(c.x * 255), int(c.y * 255), int(c.z * 255))
                                   for i, c in enumerate(colours))
        return _thumb(path)
    if definition is not None and definition.definition_id:
        return _thumb(states('objects/{}{}'.format(definition.definition_id, size), definition.geo_state_hash,
                             definition.material_hash))
    if obj is not None and obj.manager_id == 3:  # ManagerObjectId.MGR_SIM_INFO
        if baby_check and _sim_is_baby(obj.object_id):
            return _thumb('babies/{}{}'.format(obj.object_id, size))
        return _thumb('sims/f_{}{}_0'.format(obj.object_id, size))
    return 0


def _sim_bust(sim_id):
    """A Sim's bust picture, as a dialog's SimSpeechHeader / SimSpeechFooter
    loads it (ImageUtils.LoadSimThumbnail, SIM_THUMBNAIL_TYPE_BUST, large,
    default pose and outfit)."""
    return _thumb('sims/b_{}_l_0_c'.format(sim_id)) if sim_id else 0


def _queue(sim, texts, badges):
    """The action queue as the game's InteractionQueue shows it, from the
    messages the game sends it (interaction_messages._build_interaction_msg),
    arranged as its InteractionContainer.as does: running buttons first (a
    side action, a mixer, sits as a small badge on the button of the
    interaction it belongs to, or on its own button showing that
    interaction's picture), then the queued ones (a queued mixer on its
    interaction's button if it has none yet); the posture in its own button
    is not one of them. Each button's disc is the Sim's mood frame when the
    interaction lists that mood, else the default one (InteractionButton.as).
    texts: two tooltips per button (its own, its badge's); badges: the
    badges, per button."""
    global _queue_ids, _badge_ids
    ui = getattr(sim, 'ui_manager', None)
    if ui is None:
        _queue_ids, _badge_ids = [], []
        return []
    from protocolbuffers import Sims_pb2
    from interactions.interaction_messages import _build_interaction_msg
    kind = Sims_pb2.Interaction
    running_states = (Sims_pb2.IQ_RUNNING, Sims_pb2.IQ_TRANSITIONING)
    buffs = getattr(sim.sim_info, 'Buffs', None)
    mood = buffs.get_mood() if buffs is not None else None
    mood_guid = getattr(mood, 'guid64', None)

    def message(info):
        msg = Sims_pb2.Interaction()
        _build_interaction_msg(info, msg)
        return msg

    def find(buttons, interaction_id):
        for button in buttons:
            if button[0].interaction_id == interaction_id:
                return button
        return None

    # Running: as the client is told (UiManager._running_interactions): the
    # super interactions, and of each visual group (a social group, ...) the
    # one it shows, the group's last (user 2026-10-15 20:45Z: the group was missing).
    running_infos = list(getattr(ui, '_super_interactions', ()))
    for infos in getattr(ui, '_group_interactions', {}).values():
        if infos:
            running_infos.append(infos[-1])
    running = []  # [msg, badge msg or None]
    for info in running_infos:
        msg = message(info)
        if msg.visual_type == kind.POSTURE or msg.canceled:
            continue
        if msg.super_id:
            owner = find(running, msg.super_id)
            if owner is not None:
                owner[1] = msg
                continue
        button = [msg, None]
        if msg.visual_type == kind.PARENT:
            for other in running:
                if other[0].super_id == msg.interaction_id:
                    button[1] = other[0]
                    running.remove(other)
                    break
        if msg.visual_type == kind.MIXER:
            button[1] = msg
        running.append(button)
    queued = []
    for info in getattr(ui, '_queued_interactions', ())[:15]:  # MAX_QUEUE_LENGTH
        msg = message(info)
        if msg.canceled or find(running, msg.interaction_id) is not None:
            continue
        if msg.visual_type == kind.MIXER and msg.super_id:
            owner = find(queued, msg.super_id)
            if owner is not None and owner[1] is None:
                owner[1] = msg
                continue
        button = [msg, None]
        if msg.visual_type == kind.MIXER:
            if not msg.HasField('super_icon_info') and msg.super_id:
                owner = find(running + queued, msg.super_id)
                if owner is not None:
                    source = owner[0].super_icon_info if owner[0].HasField('super_icon_info') else owner[0].icon_info
                    msg.super_icon_info.CopyFrom(source)
            button[1] = msg
        elif msg.visual_type == kind.PARENT:
            for other in queued:
                if other[1] is not None and other[0].visual_type == kind.MIXER and other[0].super_id == msg.interaction_id:
                    button[1] = other[1]
                    queued.remove(other)
                    break
        queued.append(button)
    buttons = (running + queued)[:8]
    ids = [msg.interaction_id for msg, _ in buttons]

    def disc(msg):
        return _mood_index(mood) if mood_guid is not None and mood_guid in msg.mood_list else 0

    def name(icon_info):
        return icon_info.name if icon_info.HasField('name') and icon_info.name.hash else None

    items = []
    _badge_ids = []
    for msg, badge in buttons:
        # A mixer on its own button shows its interaction's picture
        # (super_icon_info) and the tooltip naming both.
        own = msg.visual_type == kind.MIXER and msg.HasField('super_icon_info')
        icon_info = msg.super_icon_info if own else msg.icon_info
        tip = name(icon_info)
        if own and name(msg.icon_info) is not None and tip is not None:
            from sims4.localization import _create_localized_string
            tip = _create_localized_string(SUPER_MIXER_TOOLTIP, msg.super_icon_info.name, msg.icon_info.name)
        flags = (1 if msg.queue_ui_state in running_states else 0) | (2 if msg.cancelable else 0)
        for other_id in msg.interactions_to_be_canceled:
            if other_id in ids and buttons[ids.index(other_id)][0].cancelable:
                flags |= 1 << (8 + ids.index(other_id))
        items.append((_picture(icon_info), disc(msg), 0, flags))
        texts.append((None, tip, None, -1))
        if badge is not None:
            badges.append((_picture(badge.icon_info), disc(badge), 0,
                           1 | (2 if badge.cancelable else 0) | (4 if badge.queue_ui_state in running_states else 0)))
            texts.append((None, name(badge.icon_info), None, -1))
            _badge_ids.append(badge.interaction_id)
        else:
            badges.append((0, 0, 0, 0))
            texts.append((None, None, None, -1))
            _badge_ids.append(0)
    _queue_ids = ids
    return items


def _wants(sim_info, texts):
    """The wants panel (WhimsTracker.send_goals_update): each filled slot's
    goal icon; a confrontation want (WhimType.CONFRONTATION) is the fear the
    game shows in its spiky bubble, last."""
    global _want_whims
    tracker = getattr(sim_info, 'whim_tracker', None)
    if tracker is None or getattr(tracker, '_hidden', False):
        _want_whims = []
        return []
    from whims.whims_tracker import WhimType
    wants, fears = [], []
    for slot in getattr(tracker, '_whim_slots', ()):
        if slot.is_empty():
            continue
        whim = slot.whim
        icon = _icon(getattr(slot.goal_instance, 'display_icon', None))
        if not icon:
            continue
        fear = getattr(whim, 'type', None) == WhimType.CONFRONTATION
        # Its tooltip as the game sends it (send_goals_update): the goal's
        # name, its tooltip and the whimset's reason.
        goal = slot.goal_instance
        tip = (None, None, None, -1)
        try:
            reason = None
            whimset = getattr(slot, 'whimset', None)
            if whimset is not None and whimset.whim_reason is not None:
                reason = whimset.whim_reason(*goal.get_localization_tokens())
            tip = (goal.get_display_name(), goal.get_display_tooltip(), reason, -1)
        except Exception:
            pass
        (fears if fear else wants).append(((icon, 0, 0, 1 if fear else 0), whim, tip))
    shown = (wants[:3] + fears)[:4]
    _want_whims = [whim for _, whim, _ in shown]
    texts.extend(tip for _, _, tip in shown)
    return [item for item, _, _ in shown]


def dismiss_want(index):
    """The HUD's want `index` pressed: dismissed as the game's X does
    (whims.refresh: WhimsTracker.refresh_whim, a new want comes)."""
    if not 0 <= index < len(_want_whims):
        return
    sim_info = services.active_sim_info()
    tracker = getattr(sim_info, 'whim_tracker', None) if sim_info is not None else None
    if tracker is None:
        return
    whim = _want_whims[index]
    tracker.refresh_whim(whim)
    native._log('hudinfo: dismissed want {} ({})'.format(index, getattr(whim, '__name__', type(whim).__name__)))


_send_logs = [0] * 5


def _send(kind, items):
    data = _pack(items)
    if data != _sent[kind]:
        _sent[kind] = data
        native.hud_items(kind, data)
        if _send_logs[kind] < 4:
            _send_logs[kind] += 1
            native._log('hudinfo: {} {}'.format(('needs', 'moodlets', 'queue', 'wants', 'queue badges')[kind], len(items)))


def _icons():
    """Reads the icons the DLL still needs (a couple a tick) and hands them over."""
    from sims4vr import objmenu
    wanted = native.hud_missing_icons()
    if not wanted:
        return
    now = time.monotonic()
    for k in range(min(2, len(wanted) // 8)):
        instance = struct.unpack_from('<Q', wanted, 8 * k)[0]
        found = native.resource_bytes(_icon_types.get(instance, 0), instance)
        if found is None:
            return
        status, compression, size, data = found
        if status == 1:
            # Still being looked up: the DLL's one lookup pass over the game's
            # packages always answers (up to v0.13.147 this gave up after 20 s,
            # before the pass was done, so no icon ever showed).
            if instance not in _waiting:
                _waiting[instance] = now
            continue
        asked = _waiting.pop(instance, now)
        data = objmenu._unpack(compression, size, data) if status == 0 else b''
        if _errors < 5 and status != 0:
            native._log('hudinfo: icon {:016x} (type {:08x}) not in the game\'s packages (status {})'.format(
                instance, _icon_types.get(instance, 0), status))
        elif len(_found_log) < 6:
            _found_log.append(instance)
            native._log('hudinfo: icon {:016x} found after {:.1f} s'.format(instance, now - asked))
        native.hud_icon(instance, bytes(data or b''))


def tick():
    """controls.py, every tick: the game's values every EVERY seconds, icons as asked."""
    global _last, _errors
    if native._optional('hud_items') is None:
        return  # an older DLL
    try:
        now = time.monotonic()
        if now - _last >= EVERY:
            _last = now
            sim_info = services.active_sim_info()
            sim = sim_info.get_sim_instance() if sim_info is not None else None
            if sim_info is None:
                for kind in range(5):
                    _send(kind, [])
            else:
                _texts[NEEDS], _texts[MOODLETS] = [], []
                _send(NEEDS, _needs(sim_info, _texts[NEEDS]))
                _send(MOODLETS, _moodlets(sim_info, _texts[MOODLETS]))
                _send_texts(NEEDS)
                _send_texts(MOODLETS)
                _texts[QUEUE], badges = [], []
                _send(QUEUE, _queue(sim, _texts[QUEUE], badges) if sim is not None else [])
                _send(QUEUE_BADGES, badges)
                _send_texts(QUEUE)
                _texts[WANTS] = []
                _send(WANTS, _wants(sim_info, _texts[WANTS]))
                _send_texts(WANTS)
        _icons()
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('hudinfo failed: {!r}'.format(e))


def cancel(index):
    """The HUD's queue item `index` pressed: cancel it as interactions.cancel_si does."""
    if not 0 <= index < len(_queue_ids):
        return
    _cancel(_queue_ids[index], index)


def cancel_badge(index):
    """The side action on the HUD's queue item `index` pressed: cancelled as
    the game's MixerInteractionButton does (interactions.cancel_si with its id)."""
    if not 0 <= index < len(_badge_ids) or not _badge_ids[index]:
        return
    _cancel(_badge_ids[index], 'side action {}'.format(index))


def _cancel(interaction_id, index):
    sim_info = services.active_sim_info()
    sim = sim_info.get_sim_instance() if sim_info is not None else None
    if sim is None:
        return
    interaction = sim.find_interaction_by_id(interaction_id)
    if interaction is None:
        continuation = sim.find_continuation_by_id(interaction_id)
        if continuation is not None:
            continuation.cancel_user(cancel_reason_msg='User canceled the interaction.')
        native._log('hudinfo: cancelled queue item {} (continuation)'.format(index))
        return
    from sims4vr import halt, hint
    if halt.first_person(sim) and hint.leave(sim, interaction, 'VR: you cancelled it on your wrist'):
        native._log('hudinfo: queue item {} ({}): you left the conversation'.format(index, type(interaction).__name__))
    if halt.one(sim, interaction, 'VR: cancelled on your wrist'):
        native._log('hudinfo: hard cancel of queue item {} ({}), first person'.format(index, type(interaction).__name__))
        return  # first person: a hard cancel (user 2026-10-15 18:54Z, halt.py)
    done = interaction.cancel_user(cancel_reason_msg='Command interactions.cancel_si')
    native._log('hudinfo: cancel queue item {} ({}): {}'.format(index, type(interaction).__name__, done))


def _wrap_funds():
    """The game's money flyaway (_Funds._send_money_update_internal, the
    SetMoney op's vfx_amount) for the active household: the HUD shows it in
    first person, in front of you a little up and right (user 2026-10-15 20:46Z)."""
    from sims.funds import _Funds

    def _send_money_update_internal(original, self, household_id, vfx_amount, reason=0):
        result = original(self, household_id, vfx_amount, reason)
        try:
            if vfx_amount and household_id == services.active_household_id():
                native.money_changed(int(vfx_amount))
        except Exception as e:
            native._log('hudinfo: money flyaway failed: {!r}'.format(e))
        return result
    hooks.hook('hudinfo', _Funds, '_send_money_update_internal', _send_money_update_internal)


try:
    _wrap_funds()
except Exception as _e:
    native._log('hudinfo: wrapping the money updates failed: {!r}'.format(_e))
