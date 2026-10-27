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

RINGTONES = (None, 0x221dea395dfcdfae, 0xc4f531d9d7d156c6, 0x5d51327b9182bc04, 0xab2adaf0c366b4d1, 0x22170b395df6f921,
             0x90e2d18104edb944, 0x435fa71eee9c0b62, 0x3658c55340bfcdd4, 0xb42cfdf7df7c7d80, 0x84bf16d610f2d690,
             0xfda9ab45a2178107, 0x4c53dab8e0d1d31e, 0x4e3fccd0de44076d, 0xd7e185ce565e5692, 0x270a91f84325e121,
             0xc22da5ecb7e5e8c9, 0x3fe7d4f35fdc3b3b, 0xbf8257b951fd20ed, 0x22e03780d6e24c24, 0x768a40c9ee3f0993,
             0x9fa688c3916f0b9d, 0x14b1fe87fb5820d4, 0xe2370f53df849a36, 0xc4d1e9ba398864a6, 0x7cb78093b4567513,
             0xf1a70374a8ff49ca, 0xd0500b31764b388a)
# The game's radio stations (user 2026-10-27 14:28Z: songs, not the jingles), as the stereo plays them: each
# station's music config holds its songs and the game picks one (re/audio/configs.tsv, mu_stereo_*). Names:
# None, Pop, Alternative, Electronica, Hip Hop, R&B, Spop, Sim Retro, Classical, Blues, Romance, World, New Age,
# Singer-Songwriter, Easy Listening, NuDisco, Latin Pop, Island Radio, Summer Strut, Americana, Soul, Metal, Focus,
# Afrobeats, Batuu Radio, Strange Tunes, Winter Holiday, Spooky
TEXT_TONES = (None, 0x930fe9ffd1801772, 0x3cd5473d134af89c, 0x4ff5444df03530b2, 0xfda52168fd102d9c,
              0x76dbf3276cba61be, 0x85688f084e31aad5, 0x8af8b916cf64c646, 0xe40a07dc0168907b)
# None, Positive (bumper_tns_positive), Neutral, Uh-oh (bumper_tns_negative), Bills Paid, Club (bumper_club_join),
# Accepted (bumper_enrollment_accepted), Moodlet (sting_buff_gain), Advice (sting_advice_received)

RING_SECONDS = 20.0   # a ringtone song plays this long at most
PREVIEW_SECONDS = 12.0

_playing = None
_playing_until = 0.0  # when the playing ringtone song stops (0: a short tone, left to end)
_ring_dialog = None   # the popup it rings for: the song stops when it goes
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
        n = 1
    return n if 0 <= n < count else 0


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


def _play(sound_id, seconds=0.0, dialog=None):
    global _playing, _playing_until, _ring_dialog
    if not sound_id:
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
    sound = PlaySound(sim, sound_id, joint_name_hash=_phone_joint())
    sound.start()
    _playing = sound
    _playing_until = time.monotonic() + seconds if seconds else 0.0
    _ring_dialog = dialog


def tick():
    """controls.py, every tick: a ringtone song stops when its popup is gone or its time is up."""
    global _playing, _playing_until, _ring_dialog
    if _playing is None or not _playing_until:
        return
    try:
        done = time.monotonic() > _playing_until
        if not done and _ring_dialog is not None:
            import services
            dialogs = getattr(services.ui_dialog_service(), '_active_dialogs', None)
            done = dialogs is not None and getattr(_ring_dialog, 'dialog_id', None) not in dialogs
        if done:
            _playing.stop()
            _playing, _playing_until, _ring_dialog = None, 0.0, None
    except Exception as e:
        _playing, _playing_until, _ring_dialog = None, 0.0, None
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
            _play(RINGTONES[_index('ringtone', len(RINGTONES))], RING_SECONDS, dialog)
        else:
            _play(TEXT_TONES[_index('text_tone', len(TEXT_TONES))])
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


def on_notification():
    """notify.py: a new notification was kept."""
    _ring(1)


def preview(code):
    kind, n = divmod(code, 100)
    tones = RINGTONES if kind == 0 else TEXT_TONES
    if 0 <= n < len(tones):
        try:
            _play(tones[n], PREVIEW_SECONDS if kind == 0 else 0.0)
        except Exception as e:
            _log_error('a preview', e)
