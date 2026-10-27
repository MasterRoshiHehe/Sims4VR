"""Eyes shut and the clock paused while a load into first person finishes; eyes shut while the exit save runs.

begin() (autostereo.py, first person starts with the lot) holds the eyelids shut
(native.set_eyes_held) and pauses the game clock; walk._tick calls update() and
the first moment you can move (the game no longer has the Sim, or the stick is
pushed) opens the eyes and sets the clock back. TIMEOUT opens them anyway.
Lot intro (lotintro.py, [wrist] lot_intro): when you can move, the game's arrival banner (lot name, household, traits)
plays in front of you for lotintro.SECONDS with the eyes still shut and time paused; they open when it ends, or at
once when the stick is pushed.
Save/exit: saving.py calls save_begin()/save_end() around saves started from the
exit menus (empty slot name). Switches: [body] load_eyes_closed, exit_eyes_closed.
"""
import time

import services
from sims4vr import native

TIMEOUT = 120.0
_held = False
_since = 0.0
_speed = None  # clock speed before we paused
_saving = False
_intro_end = 0.0  # monotonic time the lot intro ends; 0 when it is not playing


def _clock():
    return services.game_clock_service()


def begin():
    """First person started with the lot: eyes shut, time paused."""
    global _held, _since, _speed
    if not native.setting_flag('body', 'load_eyes_closed', True):
        return
    _held = True
    _since = time.monotonic()
    native.set_eyes_held(True)
    try:
        game_clock = _clock()
        if game_clock is not None:
            from date_and_time import clock
            _speed = game_clock.clock_speed
            game_clock.set_clock_speed(clock.ClockSpeedMode.PAUSED)
    except Exception as e:
        native._log('loadshut: pausing failed: {!r}'.format(e))
    native._log('loadshut: eyes shut, time paused until you can move')


def _keep_paused():
    try:  # the game un-pauses when the load ends: keep it paused
        from date_and_time import clock
        game_clock = _clock()
        if game_clock is not None and game_clock.clock_speed != clock.ClockSpeedMode.PAUSED:
            game_clock.set_clock_speed(clock.ClockSpeedMode.PAUSED)
    except Exception:
        pass


def update(game, pushing):
    """Every walk tick: release once you control your movement."""
    global _intro_end
    if not _held:
        return
    why = None
    if not game:
        why = 'you can move'
    elif pushing:
        why = 'the stick is pushed'
    elif time.monotonic() - _since > TIMEOUT:
        why = 'timeout {} s'.format(int(TIMEOUT))
    if _intro_end:  # the lot's banner plays over the shut eyes
        if pushing or time.monotonic() >= _intro_end:
            release('lot intro ' + ('skipped' if pushing else 'over'))
        else:
            _keep_paused()
        return
    if why == 'you can move':
        from sims4vr import lotintro
        seconds = lotintro.start()
        if seconds:
            _intro_end = time.monotonic() + seconds
            native._log('loadshut: lot intro for {:.1f} s'.format(seconds))
            return
    if why is None:
        try:  # the game un-pauses when the load ends: keep it paused
            from date_and_time import clock
            game_clock = _clock()
            if game_clock is not None and game_clock.clock_speed != clock.ClockSpeedMode.PAUSED:
                game_clock.set_clock_speed(clock.ClockSpeedMode.PAUSED)
        except Exception:
            pass
        return
    release(why)


def release(why):
    global _held, _speed, _intro_end
    if not _held:
        return
    _held = False
    if _intro_end:
        _intro_end = 0.0
        from sims4vr import lotintro
        lotintro.stop()
    native.set_eyes_held(_saving)
    try:
        from date_and_time import clock
        game_clock = _clock()
        speed = _speed
        if speed is None or speed == clock.ClockSpeedMode.PAUSED:
            speed = clock.ClockSpeedMode.NORMAL
        if game_clock is not None:
            game_clock.set_clock_speed(speed)
    except Exception as e:
        native._log('loadshut: resuming failed: {!r}'.format(e))
    _speed = None
    native._log('loadshut: eyes open, time running ({})'.format(why))


def reset():
    """Out of first person."""
    global _held, _saving, _intro_end
    _held = False
    _saving = False
    if _intro_end:
        _intro_end = 0.0
        from sims4vr import lotintro
        lotintro.stop()
    native.set_eyes_held(False)


def save_begin(slot_name):
    """A save starts. Exit / exit to main menu saves have an empty slot name."""
    global _saving
    from sims4vr import body
    native._log('loadshut: save starts, slot name {!r}'.format(slot_name))
    if slot_name or not body.enabled or not native.setting_flag('body', 'exit_eyes_closed', True):
        return
    _saving = True
    native.set_eyes_held(True)


def save_end():
    global _saving
    if _saving:
        _saving = False
        native.set_eyes_held(_held)
