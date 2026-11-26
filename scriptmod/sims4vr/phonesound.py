"""The phone's sounds (user 2026-10-23 23:55Z): a ringtone for calls, one tone for texts and
notifications, and the phone's mode (sound, vibrate, mute), chosen on the phone's Sounds screen
(hud.cpp) or the debug menu ([phone] ringtone, text_tone, sound_mode).

The ringtones are the game's radio stations (a song of the station, cut off when the call's popup goes or
after RING_SECONDS); the text tones are the game's own music stings and notification bumpers (their PropX audio
resources, as the game's tuning names them: re/tuning/xml ... p="InGame\\Audio\\Stings\\...").
They play the way the game plays tuned audio (audio.primitive.PlaySound on your Sim, the
TunablePlayAudio instance as the sound id) on the joint of the hand holding the phone (or the hip it
hangs at), so the game places them there in 3D; first person only, in sound mode:
- ringtone: a popup the game rings the phone for (PhoneRingType.RING: a call);
- text tone: a popup that buzzes or alarms (BUZZ, ALARM: texts), and each new notification.
Index 0 is "None" (only the controllers buzz). The names must match settings.cpp's rows.
Control 7100000 + kind * 100 + n: preview tone n (kind 0 ringtones, 1 text tones).
"""
import time

from sims4vr import hooks, native

# Ringtones and notification sounds are our own files only (user 2026-10-29: the game's flat jingles are gone; more
# files will come). One fixed sound, never a random pick; a call repeats it every RING_REPEAT seconds while its popup
# is up. The names must match settings.cpp's rows. The ini keys are ringtone_file / text_tone_file, so a choice saved
# when the list held the game's jingles is ignored (the default applies).
LOOPS = ()
# sounds/ringtones' files in name order ignoring case: bustin, Old Phone 1, Old Phone 2, outin
RINGTONES = (None, 'wav:0', 'wav:1', 'wav:2', 'wav:3')
# sounds/notifications' files in name order ignoring case: Click, Promo, Slap, Success, Upgrade
TEXT_TONES = (None, 'note:0', 'note:1', 'note:2', 'note:3', 'note:4')
RING_DEFAULT, TEXT_DEFAULT = 2, 4  # Old Phone 1, Success

RING_SECONDS = 20.0   # a ringing call stops after this long at most
RING_REPEAT = 3.5     # a ringtone is a short sound: it plays again this often while the call rings
PREVIEW_SECONDS = 12.0

_playing = None
_playing_until = 0.0  # when the ringing stops (0: a short tone, played once)
_ring_id = 0          # the ringtone to repeat
_ring_next = 0.0      # when it plays again
_ring_dialog = None   # the popup it rings for: the ringing stops when it goes
_last = 0.0
_errors = 0


def _log_error(where, e):
    global _errors
    _errors += 1
    if _errors <= 8:
        native._log('phonesound: {} failed: {!r}'.format(where, e))


def _index(key, count, default=1):
    try:
        n = int(float(native.setting('phone', key, str(default))))
    except (TypeError, ValueError):
        n = default
    return n if 0 <= n < count else default


def _phone_joint():
    """Where the phone is (user 2026-10-24 00:31Z: its sounds from the phone, heard in 3D): the joint of the hand
    holding it (phone.py: the DLL gives a right-hand mount only while the right hand holds it), else the right
    hip it hangs at."""
    from sims4vr import phone
    name = 'b__R_Thigh__'
    try:
        if phone.is_out():
            name = 'b__R_Hand__' if native.phone_mount() is not None else 'b__L_Hand__'
    except Exception:
        pass
    return phone._joint_hash(name)  # the joint hash as the game's (FNV-32 of the lower-case name)


# Where a sound that belongs to the phone is placed in 3D (user 2026-10-28): automatic, the first place in
# PLACEMENTS that applies (each a (name, applies, joint hash) function pair), joints of the Sim's body the sound is
# attached to. A new place (wrist ...) = one entry here, put before the ones it should win over.
def _in_hand():
    try:
        from sims4vr import phone
        return phone.is_out()
    except Exception:
        return False


def _joint_in_hand():
    """The phone is out: the hand holding it."""
    return _phone_joint()


def _joint_draw_spot():
    """The phone is put away: where it is pulled from, the right hip."""
    from sims4vr import phone
    return phone._joint_hash('b__R_Thigh__')


PLACEMENTS = (('hand', _in_hand, _joint_in_hand), ('draw spot', lambda: True, _joint_draw_spot))


def _placement_joint():
    for _name, applies, joint in PLACEMENTS:
        if applies():
            return joint()
    return _joint_draw_spot()


class _Wav:
    """A ringtone of ours (sounds/ringtones, played by the DLL): stop() as a game sound has."""

    def stop(self):
        native.ring_stop()


# 
WAV_AT = (0.15, -0.35, -0.25)


def _play(sound_id, seconds=0.0, dialog=None, joint=None, repeat=0.0):
    global _playing, _playing_until, _ring_dialog, _ring_id, _ring_next
    if not sound_id:
        return
    if isinstance(sound_id, str) and sound_id.startswith('note:'):
        # One of our own notification sounds: a short one-shot played by the DLL at the phone.
        if not native.ring_wav(int(sound_id[5:]), WAV_AT, True):
            native._log('phonesound: notification sound {} is not in sounds/notifications'.format(sound_id))
        return
    if isinstance(sound_id, str):
        # One of our own ringtones: the DLL plays the whole file (the files are cut to RING_SECONDS), stopped with the call.
        try:
            if _playing is not None:
                _playing.stop()
        except Exception:
            pass
        if not native.ring_wav(int(sound_id[4:]), WAV_AT):
            native._log('phonesound: ringtone {} is not in sounds/ringtones'.format(sound_id))
            return
        _playing = _Wav()
        _ring_id = 0
        _playing_until = repeat or 0.0
        _ring_dialog = dialog
        return
    import services
    from audio.primitive import PlaySound
    sim_info = services.active_sim_info()
    sim = sim_info.get_sim_instance() if sim_info is not None else None
    if sim is None:
        return
    try:
        if _playing is not None:
            _playing.stop()
    except Exception:
        pass
    sound = PlaySound(sim, sound_id, joint_name_hash=joint if joint is not None else _phone_joint())
    sound.start()
    _playing = sound
    now = time.monotonic()
    if repeat:  # a ringing call: the same sound again every RING_REPEAT s until its time or popup is over
        _ring_id, _ring_next = sound_id, now + RING_REPEAT
        _playing_until = repeat
    else:
        _ring_id = 0
        _playing_until = now + seconds if seconds else 0.0
    _ring_dialog = dialog


def stop():
    """The ringtone stops now (phonecall.py: answered, declined, or the phone's screen turned off)."""
    global _playing, _playing_until, _ring_dialog, _ring_id
    try:
        if _playing is not None:
            _playing.stop()
    except Exception:
        pass
    _playing, _playing_until, _ring_dialog, _ring_id = None, 0.0, None, 0


def tick():
    """controls.py, every tick: a ringtone song stops when its popup is gone or its time is up."""
    global _playing, _playing_until, _ring_dialog, _ring_id
    if _playing is None or not _playing_until:
        return
    try:
        now = time.monotonic()
        done = now > _playing_until
        if not done and _ring_dialog is not None:
            import services
            dialogs = getattr(services.ui_dialog_service(), '_active_dialogs', None)
            done = dialogs is not None and getattr(_ring_dialog, 'dialog_id', None) not in dialogs
        if not done and _ring_id and now >= _ring_next:
            _play(_ring_id, dialog=_ring_dialog, repeat=_playing_until)
        if done:
            _playing.stop()
            _playing, _playing_until, _ring_dialog, _ring_id = None, 0.0, None, 0
    except Exception as e:
        _playing, _playing_until, _ring_dialog, _ring_id = None, 0.0, None, 0
        _log_error('stopping the ringtone', e)


def _ring(kind, dialog=None):
    """kind 0: a call, 1: a text or notification. Sound mode, first person, not twice in a moment."""
    global _last
    try:
        if not hooks.in_first_person() or _index('sound_mode', 3, 0) != 0:
            return
        now = time.monotonic()
        if now - _last < 1.5:
            return
        _last = now
        if kind == 0:
            tone = RINGTONES[_index('ringtone_file', len(RINGTONES), RING_DEFAULT)]
            if tone in LOOPS:  # rings by itself until stopped
                _play(tone, RING_SECONDS, dialog)
            else:
                _play(tone, dialog=dialog, repeat=time.monotonic() + RING_SECONDS)
        else:
            _play(TEXT_TONES[_index('text_tone_file', len(TEXT_TONES), TEXT_DEFAULT)])
    except Exception as e:
        _log_error('a tone', e)


# The game's own phone beeps (re/audio/configs.tsv; the id is the FNV-1 64 of the name): phone_on_ui, the beep
# of a Sim's phone switching on in its animations, and phone_tapscreenicon_touch, a Sim tapping an app.
# The game has no vibrate sound (no config named for it), so vibrate mode stays silent (the controller buzzes).
CLICKS = (0x2B04ADACD6479E66, 0x8BAB942224BE342A)
_click = None


def click(kind):
    """kind 0: the screen woke, 1: a press on it (not a swipe). Sound mode only, first person (user 2026-10-24 05:16Z)."""
    global _click
    try:
        if not hooks.in_first_person() or _index('sound_mode', 3, 0) != 0 or not 0 <= kind < len(CLICKS):
            return
        import services
        from audio.primitive import PlaySound
        sim_info = services.active_sim_info()
        sim = sim_info.get_sim_instance() if sim_info is not None else None
        if sim is None:
            return
        try:
            if _click is not None:
                _click.stop()
        except Exception:
            pass
        _click = PlaySound(sim, CLICKS[kind], joint_name_hash=_phone_joint())
        _click.start()
    except Exception as e:
        _log_error('a click', e)


def on_popup(dialog):
    """objmenu.py: a popup shows; its phone ring type picks the tone."""
    try:
        ring = int(dialog.get_phone_ring_type())
    except Exception:
        ring = 0
    if ring == 2:
        _ring(0, dialog)
    elif ring in (1, 3):
        _ring(1)


UI_TNS_INCOMING = 0xEED2F6CC4FDC818E  # ui_tns_incoming: what the game's UI plays when a notification appears
_note_logs = 0


def on_notification(phone_only=False, sting=0):
    """notify.py: a new notification was kept; one sound each. One that shows live has the game's own (the UI's
    sound; notify.py held back its sting). One kept off the game's UI (phone only) plays from the phone's place
    (PLACEMENTS) when notes_sound_phone is on: the game's own sound for it (its sting, else the UI's notification
    sound) with notes_sound_vanilla, else the selected text tone."""
    global _note_logs
    if not phone_only:
        return
    global _last
    try:
        if (not native.setting_flag('phone', 'notes_sound_phone', True) or not hooks.in_first_person()
                or _index('sound_mode', 3, 0) != 0):
            return
        now = time.monotonic()
        if now - _last < 1.5:
            return
        _last = now
        if native.setting_flag('phone', 'notes_sound_vanilla', False):
            sound_id = sting or UI_TNS_INCOMING
        else:
            sound_id = TEXT_TONES[_index('text_tone_file', len(TEXT_TONES), TEXT_DEFAULT)]
        _note_logs += 1
        if _note_logs <= 12:
            native._log('phonesound: phone-only notification sound {:x}'.format(sound_id or 0))
        _play(sound_id, joint=_placement_joint())
    except Exception as e:
        _log_error('a notification sound', e)


def preview(code):
    kind, n = divmod(code, 100)
    tones = RINGTONES if kind == 0 else TEXT_TONES
    if 0 <= n < len(tones):
        try:
            _play(tones[n])  # once: one fixed sound
        except Exception as e:
            _log_error('a preview', e)
