"""The phone's call screens (user 2026-10-28): an incoming call shows on the phone with a green pick-up and a red
hang-up button, a call in progress with the portrait of the Sim you talk to and a hang-up button (hud.cpp drawPhone,
the DLL gets "call" 0x1f <mode 1 incoming / 2 in call / 0 none> 0x1f <name> 0x1f <portrait>; native.phone_call).

First person only. The game rings the phone by sending the client MSG_UI_PHONE_RING (UiDialogService.dialog_show with
PhoneRingType.RING: the client plays its own ring and shows its phone alert). That message is not sent for you: the
dialog is kept active as the game does and answered from here (dialog_respond, as the client does for its buttons),
so only our ringtone (phonesound.py) plays. Controls: 88 answer, 89 hang up (a decline while it rings), 80 the phone's
screen was turned off while it rings (the ringtone stops; the DLL goes on vibrating).
Switch: [phone] call_screen (default on). A DLL without the screens, or a call dialog with no responses, keeps the
game's own handling.
"""
import time

from sims4vr import hooks, native

RING_SECONDS = 30.0  # an unanswered call is declined (a missed call) after this long
ANSWER_GRACE = 6.0   # the call interaction needs a moment to start after the answer
END_GRACE = 3.0      # no phone interaction left for this long: the call is over

_ring = None   # [dialog, caller sim id or None, started (monotonic)] while it rings
_talk = None   # [sim id or None, started, last seen running (monotonic)] while in a call
_text = None   # [sim id or None, last seen running (monotonic)] while you text (the placeholder chat screen)
_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 60:
        native._log('phonecall: ' + message)


def _log_error(where, e):
    global _errors
    _errors += 1
    if _errors <= 8:
        native._log('phonecall: {} failed: {!r}'.format(where, e))


def _on():
    return native.setting_flag('phone', 'call_screen', True) and native._optional('phone_call') is not None


def _sim_info(sim_id):
    try:
        import services
        return services.sim_info_manager().get(sim_id) if sim_id else None
    except Exception:
        return None


def _send(mode, sim_id=None):
    """The call screen to the DLL: mode 0 clears it, 3 is the text chat (also gets your own portrait)."""
    fn = native._optional('phone_call')
    if fn is None:
        return
    name, portrait, mine = '', 0, 0
    info = _sim_info(sim_id)
    if mode and info is not None:
        name = '{} {}'.format(info.first_name, info.last_name).strip()
        from sims4vr import hudinfo
        portrait = hudinfo._sim_bust(info.id) or hudinfo._thumb('sims/f_{}_l_0'.format(info.id))
    if mode == 3:
        me = _you()
        if me is not None:
            from sims4vr import hudinfo
            mine = hudinfo._sim_bust(me.sim_info.id) or hudinfo._thumb('sims/f_{}_l_0'.format(me.sim_info.id))
    fn('call\x1f{}\x1f{}\x1f{}\x1f{}'.format(mode, name, portrait, mine).encode('utf-8'))


def _responses(dialog):
    try:
        return [r.dialog_response_id for r in dialog._get_responses_gen()]
    except Exception:
        return []


def _pick(dialog, answer):
    """The response id for picking up / declining (OK and OK_ALT / CANCEL; else the first / the last)."""
    from ui.ui_dialog import ButtonType
    ids = _responses(dialog)
    if not ids:
        return None
    wanted = (ButtonType.DIALOG_RESPONSE_OK, ButtonType.DIALOG_RESPONSE_OK_ALT) if answer else (ButtonType.DIALOG_RESPONSE_CANCEL,)
    for i in ids:
        if i in wanted:
            return i
    if answer:
        return ids[0]
    return ids[-1] if len(ids) > 1 else None


def _service():
    import services
    return services.ui_dialog_service()


def _wrap():
    from ui.ui_dialog import PhoneRingType
    from ui.ui_dialog_service import UiDialogService

    def dialog_show(original, self, dialog, *args, **kwargs):
        global _ring
        try:
            ring = args[0] if args else kwargs.get('phone_ring_type')
            if ring == PhoneRingType.RING:
                why = []
                if not _on():
                    why.append('switch off or DLL without the screens')
                if not dialog.has_responses():
                    why.append('no responses')
                if self.auto_respond:
                    why.append('auto respond')
                if self.is_phone_silenced:
                    why.append('phone silenced')
                if not hooks.in_first_person():
                    why.append('not first person')
                _log('a ring: {} caller {} -> {}'.format(type(dialog).__name__, kwargs.get('caller_id'),
                                                          ', '.join(why) or 'taken by the phone'))
            if ring == PhoneRingType.RING and _on() and dialog.has_responses() and not self.auto_respond and \
                    not self.is_phone_silenced and hooks.in_first_person():
                caller = kwargs.get('caller_id')
                self._active_dialogs[dialog.dialog_id] = dialog
                _ring = [dialog, caller, time.monotonic()]
                _send(1, caller)
                _log('incoming call from {} ({}): the game\'s ring is not sent'.format(caller, type(dialog).__name__))
                try:
                    from sims4vr import phonesound
                    phonesound.on_popup(dialog)
                except Exception as e:
                    _log_error('the ringtone', e)
                return None
        except Exception as e:
            _log_error('taking the ring', e)
        return original(self, dialog, *args, **kwargs)

    hooks.hook('phonecall', UiDialogService, 'dialog_show', dialog_show, first_person_only=True)


def _you():
    try:
        from sims4vr import body
        import services
        if not body.enabled:
            return None
        info = services.active_sim_info()
        return info.get_sim_instance() if info is not None else None
    except Exception:
        return None


def _end_ring():
    global _ring
    _ring = None
    try:
        from sims4vr import phonesound
        phonesound.stop()
    except Exception:
        pass


def mute_ring():
    """The phone's screen was turned off while it rings: the rest of the ring is vibration (the DLL buzzes)."""
    try:
        if _ring is not None:
            from sims4vr import phonesound
            phonesound.stop()
            _log('screen off: the ringtone stopped, vibrating')
    except Exception as e:
        _log_error('muting the ring', e)


def answer():
    global _ring, _talk
    try:
        if _ring is None:
            return
        dialog, caller = _ring[0], _ring[1]
        response = _pick(dialog, True)
        _end_ring()
        if response is not None:
            _service().dialog_respond(dialog.dialog_id, response)
        _talk = [caller, time.monotonic(), time.monotonic()]
        _send(2, caller)
        native.set_call(True)
        _log('call answered ({})'.format(response))
    except Exception as e:
        _log_error('answering', e)


def _decline(why):
    global _ring
    try:
        if _ring is None:
            return
        dialog = _ring[0]
        response = _pick(dialog, False)
        _end_ring()
        service = _service()
        if response is not None:
            service.dialog_respond(dialog.dialog_id, response)
        else:
            service.dialog_cancel(dialog.dialog_id)
        _send(0)
        _log('call declined ({}, {})'.format(why, response))
    except Exception as e:
        _log_error('declining', e)
        _end_ring()
        _send(0)


def _phone_interactions(sim):
    found = []
    try:
        for si in list(sim.si_state) + list(sim.queue):
            name = (type(si).__name__ + ' ' + str(getattr(getattr(si, 'affordance', None), '__name__', ''))).lower()
            if 'phone' in name and 'phonebook' not in name:
                found.append(si)
    except Exception as e:
        _log_error('listing the phone interactions', e)
    return found


def _is_text(si):
    """The game's Text interaction (phone_Text*, phone_WooHootyText, not the pickers or the autonomous ones)."""
    name = (str(getattr(getattr(si, 'affordance', None), '__name__', '')) + ' ' + type(si).__name__).lower()
    return ('phone_text' in name or 'woohootytext' in name) and 'picker' not in name and 'autonomous' not in name


def _text_target(si):
    target = getattr(si, 'target', None)
    return target.sim_info.id if getattr(target, 'is_sim', False) else None


def _text_tick(sim, now):
    """Texting a Sim: the placeholder chat screen (mode 3) while the Text interaction runs. True while it shows."""
    global _text
    mine = [si for si in _phone_interactions(sim) if _is_text(si)]
    if mine:
        if _text is None:
            _text = [_text_target(mine[0]), now]
            _send(3, _text[0])
            native.set_call(True)
            _log('texting {}: the chat screen'.format(_text[0]))
        else:
            _text[1] = now
        return True
    if _text is not None and now - _text[1] > END_GRACE:
        _text = None
        _send(0)
        native.set_call(False)
        _log('the text is over')
        return False
    return _text is not None


def hang_up():
    """The red button: declines a ring, else ends the call (its phone interactions are cancelled)."""
    global _talk
    try:
        if _ring is not None:
            _decline('hang up')
            return
        sim = _you()
        if sim is not None:
            from interactions.interaction_finisher import FinishingType
            for si in _phone_interactions(sim):
                try:
                    si.cancel(FinishingType.USER_CANCEL, cancel_reason_msg='VR phone: hang up')
                except Exception as e:
                    _log_error('cancelling a call', e)
        try:
            from sims4vr import nophone
            nophone._call = None
        except Exception:
            pass
        _talk = None
        _send(0)
        native.set_call(False)
        _log('hung up')
    except Exception as e:
        _log_error('hanging up', e)


def active():
    """A call runs (nophone.call_check keeps the phone in the hand)."""
    return _talk is not None or _ring is not None or _text is not None


def tick():
    """controls.py, every tick: a ring that was answered elsewhere or went unanswered; the call's end."""
    global _talk
    try:
        now = time.monotonic()
        if _ring is not None:
            dialog = _ring[0]
            gone = _service().get_dialog(dialog.dialog_id) is not dialog
            if gone or now - _ring[2] > RING_SECONDS:
                if gone:
                    _end_ring()
                    _send(0)
                    _log('the ring ended')
                else:
                    _decline('not answered')
            return
        if _talk is None:
            sim = _you()
            if sim is not None and native.setting_flag('phone', 'text_screen', True) and native._optional('phone_call') is not None:
                if _text_tick(sim, now):
                    return
            # A call you started yourself (nophone.py): the in-call screen too.
            try:
                from sims4vr import nophone
                call = nophone._call
                if call is not None and (_is_text(call) or 'text' in (str(getattr(getattr(call, 'affordance', None), '__name__', '')) + type(call).__name__).lower()):
                    return
                if call is not None and not getattr(call, 'is_finishing', True) and _on():
                    target = getattr(call, 'target', None)
                    _talk = [target.sim_info.id if getattr(target, 'is_sim', False) else None, now, now]
                    _send(2, _talk[0])
                    _log('a call you started: the in-call screen')
            except Exception as e:
                _log_error('looking for your call', e)
            return
        sim = _you()
        if sim is None:
            return
        if _phone_interactions(sim):
            _talk[2] = now
        elif now - _talk[1] > ANSWER_GRACE and now - _talk[2] > END_GRACE:
            _talk = None
            _send(0)
            native.set_call(False)
            _log('the call ended')
    except Exception as e:
        _log_error('tick', e)


try:
    _wrap()
except Exception as _e:
    native._log('phonecall: wrapping the call ring failed: {!r}'.format(_e))
