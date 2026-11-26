"""Work and other rabbit holes in first person (user 2026-11-03 22:11Z decision, go 2026-11-04 15:17Z;
docs/WORK-RABBIT-HOLE.md option A).

How the game does it (re/python/simulation/rabbit_hole/, careers/career_base.py, careers/career_interactions.py):
at shift start (or from the early-warning dialog's Go to Work) the career puts the Sim in a CareerRabbitHole; the
rabbit hole service pushes the career's super interaction on the Sim itself (SCRIPT source, target = the Sim, so
scripted.py's no_errands filter lets it through). It routes the Sim to the lot's arrival spawn point, then
HideSimLiability.on_run fades the Sim out and hides it. At shift end (or leave early = cancel) the interaction ends,
HideSimLiability.release shows the Sim again and the career pays out. Other rabbit holes (school, hospital,
auditions...) use the same liability.

First person, [wrist] work_card (debug menu: Interface > Wrist HUD > Work card):
- HideSimLiability.on_add for the Sim you are in while the service has a rabbit hole for it: the eyes close to black
  (native.set_eyes_held) and the DLL shows a card (hud_work.inc, a headset layer over the closed eyes): title, shift,
  clock, Pause / 1x / 2x / 3x, the household's other Sims (tap one to play as them), Leave early (careers only, once
  you are away: the game's own leave_work_early) and a Game camera button (first person off, the vanilla stereo
  view). The route to the street runs as the game plays it (halt._picked lets it through the route guard,
  walk._take_over leaves it alone), unseen behind closed eyes.
- Another Sim picked, or the game camera: the eyes open and the card goes.
- HideSimLiability.release (the Sim is back): if you stayed, the eyes open where the Sim arrived (the lot's arrival
  spawn point, as in the vanilla game). If you left, a card over open eyes offers Swap back / Stay here (never
  automatic, user default 2026-11-04).
Not handled yet: a save loaded while your Sim is in a rabbit hole (the game picks another Sim itself), career events
and gigs that move to another lot.
"""
import time

import services

from sims4vr import hooks, native

LOG_LIMIT = 60
SEND_EVERY = 0.5   # s between card refreshes
SWITCH_SHUT = 0.35  # s the eyes need before the Sim is switched
SWITCH_OPEN = 1.6   # s after the start of a switch: eyes open
BACK_OPEN = 0.9     # s after the Sim is back: eyes open
OFFER_SECONDS = 45.0  # how long the swap back offer stays

_logs = 0
_installed = False
_state = None  # dict while a rabbit hole of the Sim you are in runs (see _begin)
_switch = None  # [sim id, started, switched] while the eyes cover a change of Sim
_last_sent = 0.0
_roster = []  # sim ids, index = the card's button


def _log(message):
    global _logs
    _logs += 1
    if _logs <= LOG_LIMIT:
        native._log('work: ' + message)


def _error(what, e):
    _log('{} failed: {!r}'.format(what, e))


def install():
    """body.enable: wrap the hide liability every rabbit hole interaction carries."""
    global _installed
    if _installed:
        return
    from interactions.rabbit_hole import HideSimLiability

    def on_add(original, self, interaction, *args, **kwargs):
        result = original(self, interaction, *args, **kwargs)
        try:
            _begin(interaction)
        except Exception as e:
            _error('starting', e)
        return result

    def on_run(original, self, *args, **kwargs):
        result = original(self, *args, **kwargs)
        try:
            _hidden(self)
        except Exception as e:
            _error('hidden', e)
        return result

    def release(original, self, *args, **kwargs):
        try:
            _returning(self)
        except Exception as e:
            _error('returning', e)
        return original(self, *args, **kwargs)

    _installed = all([hooks.hook('work', HideSimLiability, 'on_add', on_add),
                      hooks.hook('work', HideSimLiability, 'on_run', on_run),
                      hooks.hook('work', HideSimLiability, 'release', release)])
    _log('installed ({})'.format('all hooks' if _installed else 'some hook missing'))


def active():
    """walk.py: the card covers your eyes (the stick must not cancel the game's walk to the street)."""
    return _state is not None and _state['shown']


def reset():
    """controls.stop (leaving the lot)."""
    global _state, _switch
    if _state is not None or _switch is not None:
        native.set_eyes_held(False)
        native.work_card(b'')
    _state = None
    _switch = None


def _first_person(sim):
    from sims4vr import halt
    return halt.first_person(sim)


def _loc(value):
    from sims4vr import objmenu
    return objmenu._loc(value) if value is not None else ''


def _clock(t):
    hour, minute = int(t.hour()), int(t.minute())
    return '{}:{:02d} {}'.format((hour + 11) % 12 + 1, minute, 'AM' if hour < 12 else 'PM')


def _career(sim_info, uid):
    tracker = getattr(sim_info, 'career_tracker', None)
    if tracker is None or uid is None:
        return None
    return tracker.get_career_by_uid(uid)


def _begin(interaction):
    """The Sim you are in is about to enter a rabbit hole."""
    global _state
    if _state is not None:
        return
    sim = getattr(interaction, 'sim', None)
    if not _first_person(sim):
        return
    if not native.setting_flag('wrist', 'work_card', True) or native._optional('work_card') is None:
        return
    service = services.get_rabbit_hole_service()
    if service is None or not service.is_in_rabbit_hole(sim.id):
        return
    sim_info = sim.sim_info
    uid = getattr(interaction, 'career_uid', None)
    career = _career(sim_info, uid)
    title = 'Away'
    shift = ''
    if career is not None:
        try:
            title = _loc(career.current_track_tuning.get_career_name(sim_info)) or 'At work'
        except Exception:
            title = 'At work'
        try:
            start, end = career._current_work_start, career._current_work_end
            if start is not None and end is not None:
                shift = 'Shift {} - {}'.format(_clock(start), _clock(end))
        except Exception:
            pass
    interaction._sims4vr_work = True  # halt._picked: the game's walk to the street is allowed
    _state = {'sim_id': sim.id, 'uid': uid if career is not None else None, 'title': title, 'shift': shift,
              'mode': 0, 'shown': True, 'since': time.monotonic(), 'open_at': 0.0, 'offer_until': 0.0,
              'interaction': interaction}
    native.set_eyes_held(True)
    _log('{} {}: eyes shut, card shown ({})'.format(sim_info.first_name, sim_info.last_name, title))
    _send(True)


def _hidden(liability):
    """The Sim is out of the world (HideSimLiability.on_run): the rabbit hole is running."""
    st = _state
    if st is None or st['mode'] != 0:
        return
    if st['sim_id'] not in _liability_ids(liability):
        return
    st['mode'] = 1
    _log('away')
    _send(True)


def _liability_ids(liability):
    try:
        return set(s.sim_id for s in liability._sim_infos)
    except Exception:
        return set()


def _returning(liability):
    """HideSimLiability.release: the Sim is shown again at the arrival spawn point."""
    st = _state
    if st is None or st['sim_id'] not in _liability_ids(liability):
        return
    _finish('the Sim is back')


def _finish(why):
    global _state
    st = _state
    if st is None:
        return
    _log(why)
    now = time.monotonic()
    here = _in_worker(st)
    if st['shown'] and here:
        # You stayed: the card goes, the eyes open shortly (the fade-in plays first).
        st['mode'] = 2
        st['shown'] = False
        st['open_at'] = now + BACK_OPEN
        native.work_card(b'')
        return
    # You are elsewhere (another Sim or the game camera): offer to go back, never automatic.
    if st['shown']:
        st['shown'] = False
        native.set_eyes_held(False)
    st['mode'] = 2
    st['offer_until'] = now + OFFER_SECONDS
    _send(True)


def _in_worker(st):
    from sims4vr import body
    return body.enabled and body.following() == st['sim_id']


def _household_roster(st):
    """Ids of the household's other Sims who are here and can be played."""
    out = []
    try:
        info = services.sim_info_manager().get(st['sim_id'])
        for other in info.household.sim_info_gen():
            if other.id == st['sim_id']:
                continue
            try:
                if other.is_instanced() and other.get_is_enabled_in_skewer():
                    out.append(other)
            except Exception:
                continue
    except Exception as e:
        _error('the household', e)
    return out


def _send(force=False):
    global _last_sent, _roster
    st = _state
    now = time.monotonic()
    if st is None or not (st['shown'] or st['offer_until']):
        return
    if not force and now - _last_sent < SEND_EVERY:
        return
    _last_sent = now
    try:
        game_clock = services.game_clock_service()
        speed = max(0, min(3, int(game_clock.clock_speed))) if game_clock is not None else 1
        hhmm = _clock(services.time_service().sim_now)
    except Exception:
        speed, hhmm = 1, ''
    flags = 0
    records = []
    if st['mode'] == 2:
        title = '{} is back'.format(_name(st['sim_id']))
        shift = 'Play as them again?'
        flags = 2 | 8
    else:
        title, shift = st['title'], st['shift']
        if st['mode'] == 1 and st['uid'] is not None:
            flags |= 1  # leave early
        flags |= 4
    records.append('work\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(st['mode'], title, shift, hhmm, speed, flags))
    if st['mode'] != 2:
        _roster = []
        try:
            current = services.active_sim_info()
            for other in _household_roster(st):
                _roster.append(other.id)
                records.append('sim\x1f{}\x1f{}\x1f{}'.format(len(_roster) - 1, other.first_name,
                                                              1 if other is current else 0))
        except Exception as e:
            _error('the roster', e)
    native.work_card('\x1e'.join(records).encode('utf-8'))


def _name(sim_id):
    try:
        info = services.sim_info_manager().get(sim_id)
        return '{} {}'.format(info.first_name, info.last_name).strip()
    except Exception:
        return 'Your Sim'


def tick():
    """controls.py, every tick."""
    global _state, _switch
    now = time.monotonic()
    try:
        _tick_switch(now)
        st = _state
        if st is None:
            return
        if st['mode'] == 2 and st['open_at'] and now >= st['open_at']:
            native.set_eyes_held(False)
            _log('eyes open, back in the lot')
            _state = None
            return
        if st['mode'] == 2 and st['offer_until'] and now >= st['offer_until']:
            _dismiss()
            return
        if st['mode'] in (0, 1):
            service = services.get_rabbit_hole_service()
            if service is not None and not service.is_in_rabbit_hole(st['sim_id']):
                _finish('the rabbit hole ended')  # without the Sim being shown (cancelled, failed)
                return
            if now - st['since'] > 600 and st['mode'] == 0:
                _log('the Sim never left (10 minutes): giving up')
                _release()
                _state = None
                return
        _send()
    except Exception as e:
        _error('tick', e)


def _tick_switch(now):
    global _switch
    if _switch is None:
        return
    elapsed = now - _switch[1]
    if not _switch[2] and elapsed >= SWITCH_SHUT:
        _switch[2] = True
        try:
            client = services.client_manager().get_first_client()
            target = services.sim_info_manager().get(_switch[0])
            if client is not None and target is not None and target.is_instanced() and target is not client.active_sim_info:
                client.set_active_sim_info(target)
                _log('now playing {} {}'.format(target.first_name, target.last_name))
                from sims4vr import phone
                if phone._out:  # the phone into the new Sim's hand
                    phone._remove_prop()
                    phone._make_prop()
        except Exception as e:
            _error('switching Sim', e)
    if _switch[2]:
        try:  # first person follows the active Sim from camera updates alone, which the game may not send
            from sims4vr import body
            body.tick()
        except Exception as e:
            _error('following the new Sim', e)
    if elapsed >= SWITCH_OPEN:
        _switch = None
        native.set_eyes_held(False)


def _release():
    """The card goes and the eyes open (you chose to leave it)."""
    st = _state
    if st is not None and st['shown']:
        st['shown'] = False
        native.work_card(b'')
        native.set_eyes_held(False)


def _dismiss():
    global _state
    native.work_card(b'')
    if _state is not None and _state['mode'] == 2:
        _state = None
    _log('card dismissed')


def _play_as(sim_id):
    global _switch
    from sims4vr import body
    if _switch is not None:
        return
    st = _state
    if st is not None and st['shown']:
        st['shown'] = False
        native.work_card(b'')
    if body.enabled:
        _switch = [sim_id, time.monotonic(), False]
        native.set_eyes_held(True)  # shut (it stays shut if the card had them shut), opened by _tick_switch
    else:  # the game camera: no eyes to cover
        _switch = [sim_id, time.monotonic() - SWITCH_SHUT, False]


def _leave_early():
    st = _state
    if st is None or st['uid'] is None:
        return
    try:
        info = services.sim_info_manager().get(st['sim_id'])
        career = _career(info, st['uid'])
        if career is not None:
            _log('leave early')
            career.leave_work_early()
    except Exception as e:
        _error('leave early', e)


def control(code):
    """controls.py: 8700000 + code from the card."""
    from sims4vr import controls
    st = _state
    try:
        if code in (1, 2, 3):
            controls._set_speed(code)
        elif code == 4:
            controls._set_speed(0)
        elif code == 5:
            _leave_early()
        elif code == 6:  # the game camera
            if st is not None and st['mode'] != 2:
                _release()
            from sims4vr import body
            if body.enabled:
                controls._switch_view()
        elif code == 7:  # swap back to the worker
            if st is not None:
                sim_id = st['sim_id']
                _dismiss_offer()
                _play_as(sim_id)
        elif code == 8:
            _dismiss_offer()
        elif code >= 100:
            index = code - 100
            if st is not None and 0 <= index < len(_roster):
                _play_as(_roster[index])
        _send(True)
    except Exception as e:
        _error('button {}'.format(code), e)


def _dismiss_offer():
    global _state
    native.work_card(b'')
    if _state is not None and _state['mode'] == 2:
        _state = None
