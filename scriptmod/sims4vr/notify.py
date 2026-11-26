"""The game's notifications (the top-right ones, its "TNS") on the first-person
phone (user 2026-10-18 22:04Z): a bell in the phone's header with the number
unread, and their list, newest first, as the game's NotificationWall keeps
them. A notification about one Sim ("<name> had an awful Harvestfest") can
instead show as a card over that Sim's head ([phone] notes_overhead in the
debug menu: 1 = over their head, 0 = phone only).

The game sends each one to its UI from UiDialogNotification.distribute_dialog
(ui/ui_dialog_notification.py, Operation.UI_NOTIFICATION_SHOW with the
UiDialogMessage it built); that is wrapped here and the message kept: title,
text (LocalizedStrings as JSON for the DLL's string tables, objmenu._loc), its
first icon as the game's NotificationView shows it (hudinfo._picture: the
icon, or the Sim's / object's thumbnail; a Sim's bust if the icon is a Sim),
the Sims in its icons. The game keeps showing its own too (god view, the
game's UI in front).

To the DLL (native.phone_notes), records split by 0x1e, fields by 0x1f:
  "notes" \x1f <unread>
  "note"  \x1f <id> \x1f <read 0/1> \x1f <picture> \x1f <title json> \x1f <text json> \x1f <time text> \x1f <style>
          \x1f <buttons: their text json split by 0x1d, empty when none or answered>
          \x1f <group: 0 about your Sim, 1 about other Sims, 2 system and mods (bills, mod notices); the list's tabs>
<style> = the game's background (NotificationView.SetBackgroundFrame): 0 standard
(blue), 1 player (green), 2 critical (orange), 3 special moment (purple); +4
for its "_strip" frame (the notification has a secondary icon).
and native.note_over(id, x, y, z, front) for the card over a Sim's head (world
position of the head; front 1 = in front of you, your own Sim's), id 0 = none.
Controls: 78 = the list was opened (all read), 79 = clear all, 80000 + id =
delete one (the wall's DeleteButton), 10000000 + id * 8 + k = its k-th button
(user 2026-10-18 23:52Z: answers clickable, not just closing it), answered as
the game's notification button answers it (ui_dialog_service().dialog_respond
with the button's choice id) while the game still waits for the answer.
"""
import time

import time

from sims4vr import hooks, native

_MAX = 60
_SHOW_SECONDS = 9.0  # how long a card stays over the head
_notes = []  # newest last: dicts
_next_id = 1
_over = None  # (id, sim id, until, front) of the card over a head
_errors = 0
_logs = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 150:
        native._log('notify: ' + message)


def _log_error(where, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('notify: {} failed: {!r}'.format(where, e))


# Which notifications show live in front of you (user 2026-10-28): [phone] notes_live (all), notes_live_<category>
# (the phone's Notifications page and the debug menu), notes_hide_startup + notes_startup_seconds (script mods' notices
# while a lot loads). One that doesn't show live is not sent to the game's UI at all (no card, no client sound); it
# still lands on the phone (list, bell, answers) and plays its sound from the phone (phonesound.on_notification).
_zone = None      # the lot the startup window counts from
_zone_t0 = None   # when it began (None: no controls tick has seen a lot yet = still starting up)
_origins = {}     # script mod file -> its name


def _mod_origin():
    """The script mod that raised this notification: (category, name) from the nearest caller in a Mods ...ts4script
    file, or None for the game's own. Ours (sims4vr, claude*) are category 'claude'."""
    import sys
    frame = sys._getframe(1)
    for _ in range(60):
        if frame is None:
            break
        path = frame.f_code.co_filename or ''
        low = path.lower().replace('/', '\\')
        if '.ts4script' in low or low.endswith('.zip') or '.zip\\' in low:
            name = _origins.get(path)
            if name is None:
                parts = [p for p in low.split('\\') if '.ts4script' in p or p.endswith('.zip')]
                name = parts[0].rsplit('.', 1)[0] if parts else low
                _origins[path] = name
            return ('claude' if name.startswith(('claude', 'sims4vr')) else 'mod'), name
        frame = frame.f_back
    return None


def _category(dialog, origin):
    if origin is not None:
        return origin[0]
    try:
        if int(getattr(dialog, 'urgency', 0)) == 1:
            return 'urgent'
        if int(getattr(dialog, 'visual_type', 0)) == 2:
            return 'moment'
        if int(getattr(dialog, 'information_level', 1)) == 0:
            return 'player'
    except Exception as e:
        _log_error('the category', e)
    return 'sim'


def _in_startup():
    try:
        seconds = float(native.setting('phone', 'notes_startup_seconds', '120'))
    except (TypeError, ValueError):
        seconds = 120.0
    return _zone_t0 is None or time.monotonic() - _zone_t0 < seconds


def _live(dialog):
    """(shows live?, category, mod name or '') for a notification about to be shown."""
    origin = None
    try:
        origin = _mod_origin()
    except Exception as e:
        _log_error('the origin', e)
    category = _category(dialog, origin)
    live = native.setting_flag('phone', 'notes_live', True) and native.setting_flag('phone', 'notes_live_' + category, True)
    if live and origin is not None and native.setting_flag('phone', 'notes_hide_startup', True) and _in_startup():
        live = False
    return live, category, origin[1] if origin else ''


def _sim_ids(msg):
    """The Sims a notification's icons show (IconInfo.object_instance_id of a Sim)."""
    import services
    ids = []
    manager = services.sim_info_manager()
    for info in getattr(msg, 'icon_infos', ()):
        sim_id = getattr(info, 'object_instance_id', 0)
        if sim_id and manager.get(sim_id) is not None and sim_id not in ids:
            ids.append(sim_id)
    return ids


def _time_text():
    """The game's timestamp style: the Sim day and time it came."""
    try:
        import services
        now = services.time_service().sim_now
        day = ('Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday')[int(now.day()) % 7]
        hour, minute = int(now.hour()), int(now.minute())
        return '{} {}:{:02d} {}'.format(day, (hour + 11) % 12 + 1, minute, 'AM' if hour < 12 else 'PM')
    except Exception:
        return ''


def _style(dialog, msg):
    """NotificationView.SetBackgroundFrame: urgent = critical, player level =
    player, special moment = moment, else standard; "_strip" with a secondary icon."""
    style = 0
    try:
        if int(getattr(dialog, 'urgency', 0)) == 1:
            style = 2
        elif int(getattr(dialog, 'information_level', 1)) in (0, 2):
            style = 1
        elif int(getattr(dialog, 'visual_type', 0)) == 2:
            style = 3
        if msg.HasField('secondary_icon_info'):
            style += 4
    except Exception as e:
        _log_error('the style', e)
    return style


def _record(dialog, msg, held=None):
    """Keep a notification; held = (category, mod name) when it was kept off the game's UI (phone only)."""
    global _next_id
    import services
    from sims4vr import objmenu, hudinfo
    title = objmenu._loc(msg.title) if msg.HasField('title') else ''
    text = objmenu._loc(msg.text) if msg.HasField('text') else ''
    if not title and not text:
        return
    sims = _sim_ids(msg)
    owner = getattr(dialog, 'owner', None)
    owner_id = getattr(owner, 'sim_id', 0) if getattr(owner, 'is_sim', False) else 0
    if not sims and owner_id:
        sims = [owner_id]
    picture = 0
    icons = list(getattr(msg, 'icon_infos', ()))
    try:
        if icons:
            first = icons[0]
            sim_id = getattr(first, 'object_instance_id', 0)
            has_icon = first.HasField('icon') and first.icon.instance
            picture = hudinfo._sim_bust(sim_id) if sim_id in sims and not has_icon else hudinfo._picture(first)
        elif sims:
            picture = hudinfo._sim_bust(sims[0])
    except Exception as e:
        _log_error('picture', e)
    # Its buttons (UiDialogNotification ui_responses): plain answers only, as
    # the popup cards take them (a button that makes the client open part of
    # its UI, run a command or show build/buy stays on the game's own).
    buttons = []
    try:
        if dialog.dialog_id in services.ui_dialog_service()._active_dialogs:
            for c in msg.choices:
                if c.ui_request or any(c.HasField(name) for name in (
                        'ui_message_args', 'show_build_buy_args', 'exchange_args', 'command_with_args', 'tutorial_args')):
                    continue
                buttons.append((objmenu._loc(c.text) if c.HasField('text') else '{}', c.choice_id))
    except Exception as e:
        _log_error('buttons', e)
    group = 2  # the phone list's tabs: 0 about your Sim, 1 about other Sims, 2 system (bills, mods, ...)
    try:
        if _mod_origin() is None and sims:
            you = services.active_sim_info()
            group = 0 if you is not None and you.id in sims else 1
    except Exception as e:
        _log_error('the group', e)
    note = {'id': _next_id, 'group': group, 'title': title, 'text': text, 'picture': picture or 0, 'sims': sims,
            'read': False, 'time': _time_text(), 'style': _style(dialog, msg),
            'dialog': getattr(dialog, 'dialog_id', 0), 'buttons': buttons[:8]}
    _next_id += 1
    _notes.append(note)
    del _notes[:-_MAX]
    _log('#{} {} Sim(s) {}, picture {:x}{}'.format(note['id'], len(sims), sims, note['picture'],
                                                  ', phone only ({} {})'.format(*held) if held else ''))
    if len(sims) == 1 and not held:
        _maybe_over(note, sims[0])
    send()


def _maybe_over(note, sim_id):
    """A notification about one Sim: a card over their head (first person, the setting on)."""
    global _over
    if not native.setting_flag('phone', 'notes_overhead', True) or native._optional('note_over') is None:
        return
    from sims4vr import objmenu
    you = objmenu._first_person_sim()
    if you is None:
        return
    import time
    if sim_id == you.id:
        front = True
    else:
        sim = objmenu._sim_instance(sim_id)
        if sim is None:
            return
        try:
            p, q = sim.position, you.position
            if (p - q).magnitude() > 20 or sim.level != you.level:
                return
        except Exception:
            return
        front = False
    _over = (note['id'], sim_id, time.monotonic() + _SHOW_SECONDS, front)
    note['read'] = True  # seen over their head
    _place()


def _place():
    global _over
    if _over is None:
        return
    import time
    from sims4vr import objmenu
    note_id, sim_id, until, front = _over
    sim = objmenu._sim_instance(sim_id)
    if time.monotonic() > until or (sim is None and not front):
        _over = None
        native.note_over(0, 0, 0, 0, 0)
        return
    x, y, z = objmenu._head(sim) if sim is not None else (0, 0, 0)
    native.note_over(note_id, x, y, z, 1 if front else 0)


def poll():
    """Controls tick: the card over a head follows it."""
    global _zone, _zone_t0
    if _over is not None:
        try:
            _place()
        except Exception as e:
            _log_error('placing the card', e)
    try:  # the startup window of a lot (script mods' notices while it loads)
        import services
        zone = services.current_zone_id()
        if zone != _zone:
            _zone, _zone_t0 = zone, time.monotonic()
    except Exception:
        pass


def send():
    if native._optional('phone_notes') is None:
        return
    unread = sum(1 for n in _notes if not n['read'])
    records = ['notes\x1f{}'.format(unread)]
    for n in reversed(_notes):
        buttons = '\x1d'.join(text for text, _ in n['buttons']) if _waiting(n) else ''
        records.append('note\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
            n['id'], 1 if n['read'] else 0, n['picture'], n['title'], n['text'], n['time'], n.get('style', 0), buttons,
            n.get('group', 2)))
    native.phone_notes('\x1e'.join(records).encode('utf-8'))


def _waiting(note):
    """The game still waits for this notification's answer."""
    if not note.get('buttons'):
        return False
    try:
        import services
        return note['dialog'] in services.ui_dialog_service()._active_dialogs
    except Exception:
        return False


def answer(note_id, k):
    """Its k-th button: the game's answer for it; the buttons go once answered."""
    for n in _notes:
        if n['id'] != note_id:
            continue
        if not _waiting(n) or not 0 <= k < len(n['buttons']):
            _log('#{} button {}: the game no longer waits for an answer'.format(note_id, k))
            break
        import services
        choice = n['buttons'][k][1]
        try:
            services.ui_dialog_service().dialog_respond(n['dialog'], choice)
            _log('#{} answered with button {} (choice {})'.format(note_id, k, choice))
        except Exception as e:
            _log_error('answering', e)
        n['buttons'] = []
        n['read'] = True
        break
    send()


def opened():
    """The phone's notification list was opened: all read."""
    if any(not n['read'] for n in _notes):
        for n in _notes:
            n['read'] = True
        send()


def delete(note_id):
    _notes[:] = [n for n in _notes if n['id'] != note_id]
    send()


def clear():
    del _notes[:]
    send()


def _wrap():
    from ui.ui_dialog_notification import UiDialogNotification

    def show_dialog(original, self, *args, **kwargs):
        # Decided here, before the game plays the notification's own sting (head-centred): one kept off the game's UI
        # (not live) gets that sting from the phone instead (phonesound.on_notification).
        sting = None
        try:
            self._s4vr_held = None
            self._s4vr_sting = 0
            if hooks.in_first_person():
                live, category, name = _live(self)
                if not live:
                    self._s4vr_held = (category, name)
                # One sound per notification (user 2026-10-28): the game's tuned sting is held back, always (a live one
                # keeps the UI's own sound, a phone-only one plays one from the phone).
                sting = getattr(self, 'audio_sting', None)
                if sting is not None:
                    try:
                        self._s4vr_sting = int(sting(None).sound_id)
                    except Exception as e:
                        _log_error('the sting', e)
                    self.audio_sting = None
        except Exception as e:
            _log_error('deciding live or phone only', e)
        try:
            return original(self, *args, **kwargs)
        finally:
            if sting is not None:
                self.audio_sting = sting

    def distribute_dialog(original, self, dialog_type, dialog_msg, **kwargs):
        held = getattr(self, '_s4vr_held', None)
        result = None if held else original(self, dialog_type, dialog_msg, **kwargs)
        try:
            _record(self, dialog_msg, held)
        except Exception as e:
            _log_error('keeping a notification', e)
        try:
            from sims4vr import phonesound
            phonesound.on_notification(held is not None, getattr(self, '_s4vr_sting', 0))
        except Exception as e:
            _log_error('the notification tone', e)
        return result
    hooks.hook('notify_show', UiDialogNotification, 'show_dialog', show_dialog)
    hooks.hook('notify', UiDialogNotification, 'distribute_dialog', distribute_dialog)


try:
    _wrap()
except Exception as e:
    native._log('notify: wrapping the notifications failed: {!r}'.format(e))
