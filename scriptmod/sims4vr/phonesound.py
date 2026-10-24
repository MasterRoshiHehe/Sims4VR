"""The phone's sounds (user 2026-10-23 23:55Z): a ringtone for calls, one tone for texts and
notifications, and the phone's mode (sound, vibrate, mute), chosen on the phone's Sounds screen
(hud.cpp) or the debug menu ([phone] ringtone, text_tone, sound_mode).

The tones are the game's own music stings and notification bumpers (their PropX audio
resources, as the game's tuning names them: re/tuning/xml ... p="InGame\\Audio\\Stings\\...").
They play the way the game plays tuned audio (audio.primitive.PlaySound on your Sim, the
TunablePlayAudio instance as the sound id), first person only, in sound mode:
- ringtone: a popup the game rings the phone for (PhoneRingType.RING: a call);
- text tone: a popup that buzzes or alarms (BUZZ, ALARM: texts), and each new notification.
Index 0 is "None" (only the controllers buzz). The names must match settings.cpp's rows.
Control 7100000 + kind * 100 + n: preview tone n (kind 0 ringtones, 1 text tones).
"""
import time

from sims4vr import hooks, native

RINGTONES = (None, 0xec3c578eda473ff1, 0xc16f7f8c8e553337, 0x54381264540dd53c, 0xd02188390ced38fb,
             0xa77d02b83bb9aa0d, 0x6020aef623140084, 0x6253d240b8fb6c23, 0x426a2439e0b2666c,
             0xb43dc21d9bacf1ff, 0x5ec499dca3e09c07)
# None, Off to Work (sting_work_go), Promotion (sting_career_progess), Skill Up (sting_skill_progress),
# Collection (sting_collection_complete), Karaoke Win, First Kiss, Birthday (sting_birthday_blowcandles),
# Reward (sting_challenge_reward), Eureka (sting_scientist_eureka), Liftoff (sting_rocket_complete)
TEXT_TONES = (None, 0x930fe9ffd1801772, 0x3cd5473d134af89c, 0x4ff5444df03530b2, 0xfda52168fd102d9c,
              0x76dbf3276cba61be, 0x85688f084e31aad5, 0x8af8b916cf64c646, 0xe40a07dc0168907b)
# None, Positive (bumper_tns_positive), Neutral, Uh-oh (bumper_tns_negative), Bills Paid, Club (bumper_club_join),
# Accepted (bumper_enrollment_accepted), Moodlet (sting_buff_gain), Advice (sting_advice_received)

_playing = None
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


def _play(sound_id):
    global _playing
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
    sound = PlaySound(sim, sound_id)
    sound.start()
    _playing = sound


def _ring(kind):
    """kind 0: a call, 1: a text or notification. Sound mode, first person, not twice in a moment."""
    global _last
    try:
        if not hooks.in_first_person() or _index('sound_mode', 3, 0) != 0:
            return
        now = time.monotonic()
        if now - _last < 1.5:
            return
        _last = now
        tones = RINGTONES if kind == 0 else TEXT_TONES
        _play(tones[_index('ringtone' if kind == 0 else 'text_tone', len(tones))])
    except Exception as e:
        _log_error('a tone', e)


def on_popup(dialog):
    """objmenu.py: a popup shows; its phone ring type picks the tone."""
    try:
        ring = int(dialog.get_phone_ring_type())
    except Exception:
        ring = 0
    if ring == 2:
        _ring(0)
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
            _play(tones[n])
        except Exception as e:
            _log_error('a preview', e)
