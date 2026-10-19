"""Controller shortcuts that need the game (the DLL reports them):
- hold the left stick down: switch between first person and the game camera
  (god mode); the camera starts at your Sim;
- hold the right stick down: play the next Sim of your household who is here
  on the lot (not one at work or school); in the game camera only, first
  person keeps you in your Sim (onesim.py, v0.13.115).
- your mic moves the active Sim's mouth ([voice] mic in sims4vr.ini, the
  DLL does that); this tells it which Sim that is;
- close your right hand at your right hip: your Sim's phone, with the game
  menu on it (phone.py); open the hand to put it away;
- testing in the VR debug menu (B): the "Testing cheats" switch
  (testingcheats true / false), the "Cheat pie menus" switch (while on, every
  pie menu opens as the game's Shift+click cheat menu, pie.py; it turns
  testing cheats on, and testing cheats off turns it off; separate switches
  since v0.13.127, user 2026-10-15 01:06Z), and buttons "Fill needs" (the Sim
  you play gets all its needs full at once, as the game's fill-needs cheat
  does), "Motherlode", "Move objects on" (bb.moveobjects on) and "CAS full
  edit mode" (cas.fulleditmode), each run once a press as if typed in the
  console. Each button turns testing cheats on too, not cheat pie menus.
Runs while you play a lot (started and stopped by autostereo.py).
"""
import alarms
import clock
import services
import sys

from sims4vr import native

TICK_SECONDS = 0.1

_owner = type('_Owner', (), {})()
_alarm = None
_errors = 0
_voice_sim = None
_cheats_shown = None  # what the debug menu's Testing cheats switch was last told


def start():
    global _alarm, _cheats_shown
    _cheats_shown = None
    # The debug menu shows the Claude Performance page only while that script mod is loaded (user 2026-10-17 16:09Z).
    native.set_script_mods(1 << 2 if 'claudeperf' in sys.modules else 0)
    # Stick holds from before this lot (menus, loading) are stale.
    for _ in range(8):
        if not native.control_event():
            break
    if _alarm is None:
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)


def stop():
    global _alarm, _voice_sim
    alarm, _alarm = _alarm, None
    if alarm is not None:
        alarms.cancel_alarm(alarm)
    _voice_sim = None
    native.set_voice_sim(0)
    _phone('put_away', 'left the lot')


def _phone(what, *args):
    try:
        from sims4vr import phone
        getattr(phone, what)(*args)
    except Exception as e:
        native._log('controls: phone {} failed: {!r}'.format(what, e))


def _build_buy():
    try:
        from sims4vr import buildbuy
        buildbuy.poll()
    except Exception as e:
        native._log('controls: build/buy check failed: {!r}'.format(e))
    try:
        from sims4vr import phoneapp
        phoneapp.poll()  # the phone menu's tests, a slice a tick
    except Exception as e:
        native._log('controls: phone menu failed: {!r}'.format(e))
    try:
        from sims4vr import notify
        notify.poll()  # a notification card over a Sim's head follows it
    except Exception as e:
        native._log('controls: notification card failed: {!r}'.format(e))


def _report_voice_sim():
    global _voice_sim
    sim = services.get_active_sim()
    sim_id = sim.id if sim is not None else 0
    if sim_id != _voice_sim:
        _voice_sim = sim_id
        native.set_voice_sim(sim_id)


def _switch_view():
    from sims4vr import body
    native.set_mode(True)
    if body.enabled:
        body.disable()
        try:
            import camera
            sim = services.get_active_sim()
            if sim is not None:
                camera.focus_on_sim(sim=sim, follow=False)
        except Exception as e:
            native._log('controls: camera to your Sim failed: {!r}'.format(e))
        native._log('controls: game camera')
    else:
        native._log('controls: first person in {}'.format(body.enable()))


def _next_sim():
    client = services.client_manager().get_first_client()
    if client is None:
        return
    from sims4vr import onesim
    if onesim.locked(client):
        native._log('controls: next Sim refused, you stay in your Sim in first person')
        return
    here = []
    for sim_info in client.selectable_sims:
        try:
            if sim_info.is_instanced() and sim_info.get_is_enabled_in_skewer():
                here.append(sim_info)
        except Exception:
            continue
    if not here:
        return
    current = client.active_sim_info
    index = here.index(current) if current in here else -1
    target = here[(index + 1) % len(here)]
    if target is current:
        native._log('controls: no other Sim of your household is here')
        return
    client.set_active_sim_info(target)
    native._log('controls: now playing {} {}'.format(target.first_name, target.last_name))


def _stop_everything():
    """The debug menu's "Stop everything": a hard cancel of all your Sim
    does (halt.py, first person only; user 2026-10-15 18:54Z)."""
    from sims4vr import halt
    sim = services.get_active_sim()
    if sim is None or not halt.first_person(sim):
        native._log('controls: Stop everything works in first person only')
        return
    halt.everything(sim, 'VR: Stop everything (debug menu)', full=True)


def _fill_needs():
    """The played Sim's needs to full, once (the debug menu's Fill needs)."""
    sim_info = services.active_sim_info()
    tracker = getattr(sim_info, 'commodity_tracker', None) if sim_info is not None else None
    if tracker is None:
        native._log('controls: fill needs: no Sim to fill')
        return
    tracker.set_all_commodities_to_best_value(visible_only=True)
    native._log('controls: filled the needs of {} {}'.format(sim_info.first_name, sim_info.last_name))


# Sims4VR menu > Other mods > Claude Performance: actions 70-76 (that mod's own console commands;
# it shows each answer as a game notification).
_PERF_COMMANDS = ('perf.status', 'perf.freeze on', 'perf.freeze off', 'perf.log on', 'perf.log off',
                  'perf.profile 30', 'perf.profile 60')


def _console(command, client_side=False):
    """Run a cheat as if typed in the game's console (once)."""
    client = services.client_manager().get_first_client()
    if client is None:
        native._log('controls: no game client for "{}"'.format(command))
        return
    import sims4.commands
    if client_side:  # handled by the game's own client (build mode), not Python
        sims4.commands.client_cheat(command, client.id)
    else:
        sims4.commands.execute(command, client.id)
    native._log('controls: ran "{}"'.format(command))


def _testing_cheats(on):
    """The debug menu's Testing cheats switch: testingcheats true / false (off
    also ends cheat pie menus)."""
    from sims4vr import pie
    _console('testingcheats {}'.format('true' if on else 'false'))
    if not on:
        pie.debug_pies = False
    native._log('controls: testing cheats are {}'.format('on' if pie.cheats_enabled() else 'OFF'))
    _report_cheats(force=True)


def _cheat_pies(on):
    """The debug menu's Cheat pie menus switch: every pie menu is the Shift+click
    cheat menu (pie.py); that needs testing cheats, so on turns them on."""
    from sims4vr import pie
    if on and not pie.cheats_enabled():
        _console('testingcheats true')
    pie.debug_pies = on
    native._log('controls: cheat pie menus are {}'.format('on: every pie menu is the cheat menu' if on else 'off'))
    _report_cheats(force=True)


def _report_cheats(force=False):
    """Tells the debug menu's switches what the game has: 1 testing cheats, 2 cheat pie menus."""
    global _cheats_shown
    from sims4vr import pie
    cheats = pie.cheats_enabled()
    if pie.debug_pies and not cheats:
        pie.debug_pies = False
        native._log('controls: testing cheats were turned off elsewhere; pie menus are normal again')
    state = (1 if cheats else 0) | (2 if pie.debug_pies else 0)
    if force or state != _cheats_shown:
        _cheats_shown = state
        native.set_testing_cheats(state)


# The VR HUD's watch (hud.cpp): the game clock and speed, and its speed
# buttons, set as the game's own speed buttons do (GameClock.set_clock_speed,
# simulation/clock.py). Pause pressed while paused goes back to the last speed.
_clock_sent = None
_last_speed = 1


def _report_clock():
    global _clock_sent, _last_speed
    game_clock = services.game_clock_service()
    if game_clock is None:
        return
    now = game_clock.now()
    speed = int(game_clock.clock_speed)
    if speed in (1, 2, 3):
        _last_speed = speed
    state = (now.hour(), now.minute(), now.day(), speed)
    if state != _clock_sent:
        if _clock_sent is None or _clock_sent[3] != speed:
            native.set_paused(speed == 0)  # the pause symbol (also from phone.py's clock wrap)
        _clock_sent = state
        native.set_clock(*state)


def _set_speed(button):
    game_clock = services.game_clock_service()
    if game_clock is None:
        return
    if button == 0:
        speed = clock.ClockSpeedMode.PAUSED
        if game_clock.clock_speed == clock.ClockSpeedMode.PAUSED:
            speed = clock.ClockSpeedMode(_last_speed)
    else:
        speed = clock.ClockSpeedMode(button)
    game_clock.set_clock_speed(speed)
    native._log('controls: HUD speed button {}: speed {}'.format(button, speed))


@native.timed('controls tick')
def _tick(_handle):
    global _errors
    try:
        _report_voice_sim()
        _build_buy()
        for _ in range(4):
            what = native.control_event()
            if what == 1:
                _switch_view()
            elif what == 2:
                _next_sim()
            elif what == 3:
                _phone('take_out')
            elif what == 4:
                _phone('put_away')
            elif what == 6:
                _testing_cheats(True)
            elif what == 9:
                _testing_cheats(False)
            elif what in (11, 12):
                _cheat_pies(what == 11)
            elif what == 13:
                _stop_everything()
            elif 20 <= what <= 23:
                _set_speed(what - 20)
            elif 30 <= what <= 37:
                from sims4vr import hudinfo
                hudinfo.cancel(what - 30)
            elif 90 <= what <= 97:
                from sims4vr import hudinfo
                hudinfo.cancel_badge(what - 90)
            elif 40 <= what <= 43:
                from sims4vr import hudinfo
                hudinfo.dismiss_want(what - 40)
            elif 50 <= what <= 59:
                from sims4vr import objmenu
                objmenu.answer_popup(what - 50)
            elif what == 60:
                from sims4vr import objmenu
                objmenu.cancel_picker()
            elif what == 61:
                from sims4vr import pie
                pie.open_self()
            elif 70 <= what <= 76:
                _console(_PERF_COMMANDS[what - 70])  # the separate Claude Performance mod
            elif what == 77:
                from sims4vr import phoneapp
                phoneapp.toggle_silence()
            elif what in (78, 79):
                from sims4vr import notify
                notify.opened() if what == 78 else notify.clear()
            elif what == 82:
                from sims4vr import nophone
                nophone.call_check()
            elif what == 81:
                from sims4vr import phonecal
                phonecal.send()
            elif 80000 <= what < 90000:
                from sims4vr import notify
                notify.delete(what - 80000)
            elif 90000 <= what < 90100:
                from sims4vr import phoneapp
                phoneapp.set_color(what - 90000)
            elif what >= 10000000:
                from sims4vr import notify
                notify.answer((what - 10000000) // 8, (what - 10000000) % 8)
            elif what >= 100000:
                from sims4vr import phoneapp
                phoneapp.select(what - 100000)
            elif what >= 1000:
                from sims4vr import objmenu
                objmenu.answer_picker(what - 1000)
            elif what in (5, 7, 8, 10):
                _testing_cheats(True)  # the testing buttons turn testing cheats on too
                if what == 5:
                    _fill_needs()
                elif what == 7:
                    _console('motherlode')
                elif what == 8:
                    _console('bb.moveobjects on', client_side=True)
                else:  # the game's own client handles CAS commands, not Python
                    _console('cas.fulleditmode', client_side=True)
            else:
                break
        _report_cheats()
        _report_clock()
        from sims4vr import hudinfo
        hudinfo.tick()
    except Exception as e:
        _errors += 1
        if _errors <= 5:
            native._log('controls failed: {!r}'.format(e))
