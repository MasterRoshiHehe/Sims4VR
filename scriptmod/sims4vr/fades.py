"""Eyes closed while the game fades your Sim out (first person). WickedWhims
fades the Sims out between positions and back in at the next one, and the view,
in your Sim's head, jumped across the room in between (user 2026-10-18 22:10Z:
"it can be very disorienting to have the camera just instantly teleport
around"). Every fade of an object goes to the client as an opacity message
(objects/client_object_mixin.py fade_out/fade_in -> FadeOpacity, the opacity
field -> SetOpacity), recorded through the distributor; one for the Sim you
are in closes the eyelids (native.set_eyes_covered) when it heads below half
and opens them when it comes back. Any other fade of your Sim does the same.

WickedWhims doesn't fade between two animations of a scene: it ends one and
starts the next, and the view jumped with the new pose (user 2026-10-18
23:16Z: start, object change and end worked, animation switches not). The
eyes also close when the WickedWhims animation your Sim is in or heading for
changes to another one, and open once the new one plays (scene_tick).

A scene's animation plays inside its transition: it never shows in the Sim's
running actions, so "once the new one plays" never came and the eyes stayed
shut the full 4 s on every switch (user 2026-10-19 01:34Z: too slow, dark too
long). The new animation reaching the client is its StartArb message on your
Sim, recorded through the same distributor; the eyes open shortly after that
(SWITCH_SETTLE), or after SWITCH_MAX at the latest.

Pushing the stick out of a scene (walk.py's hard cancel): WickedWhims stops it
with a reset that moves your Sim at once and its fade-out lasts no time, so
the eyes never shut before the view jumped (user 01:34Z). stick_ready closes
them first and holds the cancel back until they are shut; they open again
STICK_OPEN after it.

An animation that plays again (user 2026-10-19 02:15Z: the eyes closed when
an animation repeated) starts a new scene animation like a switch does, so
the eyes closed for it too. The first StartArb of each scene animation on your
Sim is remembered by the names in it (clips, state machine); when the new one
has the same names as the one before, the eyes open at once.
"""
import re
import sys

from sims4vr import native

MAX_LOGS = 40
SCENE_ANIMATION = 'WickedWhims_Sex_Animation'  # affordance names of a scene's animations
SWITCH_GAP = 2.0  # s: a new animation this soon after the last one is a switch
SWITCH_SETTLE = 0.2  # s after the new animation went to the client before the eyes open
SWITCH_MAX = 1.5  # s: open anyway
STICK_SHUT = 0.15  # s: the eyes shut (the DLL's 0.12 s close) before the stick's cancel ends a scene
STICK_OPEN = 0.6  # s after that cancel: open (WickedWhims' reset has moved your Sim by then)
STICK_GIVE_UP = 1.0  # s: the stick let go before the cancel: open
_logs = 0
_installed = False
_covered = False  # what the DLL was told
_faded = False  # your Sim faded out
_switch = None  # (interaction id, since, sent to the client at or None) while an animation switch covers the eyes
_last_animation = 0  # id of the scene animation last seen on your Sim
_last_seen = 0.0
_last_arb = 0.0  # when an animation (StartArb) last went to the client on your Sim
_stick = None  # [asked at, cancelled at or None] while the stick's way out of a scene covers the eyes
_clips_now = None  # names in the first animation message of the scene animation on your Sim
_clips_before = None  # the same for the animation before it
_clips_wanted = 0.0  # when a new scene animation began (0: none); its first animation message is the one to remember
_NAMES = re.compile(rb'[A-Za-z0-9_.:-]{6,}')


def install():
    """Wrap the distributor's add_op, which every recorded message goes through."""
    global _installed
    if _installed:
        return
    import distributor.ops
    from distributor.system import Distributor
    fade_ops = (distributor.ops.FadeOpacity, distributor.ops.SetOpacity)
    arb_op = distributor.ops.StartArb
    original = Distributor.add_op

    def add_op(self, obj, op):
        result = original(self, obj, op)
        if isinstance(op, fade_ops):
            try:
                _seen(obj, op)
            except Exception as e:
                native._log('fades failed: {!r}'.format(e))
        elif (_switch is not None or _clips_wanted) and isinstance(op, arb_op):
            try:
                _arb_seen(obj, op)
            except Exception as e:
                native._log('fades: animation check failed: {!r}'.format(e))
        return result

    Distributor.add_op = add_op
    _installed = True


def _seen(obj, op):
    global _covered
    from sims4vr import body
    if not body.enabled or obj is None or getattr(obj, 'id', 0) != body.following():
        return
    global _faded
    opacity = 1.0 if op.opacity is None else op.opacity
    _faded = opacity < 0.5
    _log(obj, op, opacity, _faded)
    _tell()


def _arb_seen(obj, op):
    """An animation went to the client on your Sim during a switch, or the
    first one of a new scene animation."""
    global _last_arb, _clips_now, _clips_wanted, _switch
    from sims4vr import body
    if obj is None or getattr(obj, 'id', 0) != body.following():
        return
    import time
    _last_arb = time.monotonic()
    if not _clips_wanted:
        return
    wanted, _clips_wanted = _clips_wanted, 0.0
    if _last_arb - wanted > SWITCH_MAX:
        return  # too late to be that animation's own
    data = getattr(op, '_arb_bytes', None)
    _clips_now = frozenset(_NAMES.findall(data)) if data else None
    if _switch is not None and _clips_now and _clips_now == _clips_before:
        _switch = None
        _log_switch('eyes open: the same animation plays again ({} names)'.format(len(_clips_now)))
        _tell()
    elif _switch is not None:
        _log_switch('another animation: {} names, {} the same as the one before'.format(
            len(_clips_now or ()), len((_clips_now or frozenset()) & (_clips_before or frozenset()))))


def _tell():
    global _covered
    covered = _faded or _switch is not None or _stick is not None
    if covered != _covered:
        _covered = covered
        native.set_eyes_covered(covered)


def reset():
    """Out of first person: eyes open."""
    global _faded, _switch, _last_animation, _stick, _clips_now, _clips_before, _clips_wanted
    _faded = False
    _switch = None
    _stick = None
    _last_animation = 0
    _clips_now = _clips_before = None
    _clips_wanted = 0.0
    _tell()


def _animation(si):
    affordance = getattr(si, 'affordance', None)
    return si is not None and SCENE_ANIMATION in (getattr(affordance, '__name__', '') or '') and \
        not getattr(si, 'is_finishing', False)


def in_scene(sim):
    """A WickedWhims animation your Sim plays or heads for."""
    controller = sim.transition_controller
    if controller is not None and _animation(getattr(controller, 'interaction', None)):
        return True
    return any(_animation(si) for si in sim.si_state)


def stick_ready(sim):
    """walk.py, before the stick's hard cancel: True to cancel now. In a
    WickedWhims scene the eyes close first and the cancel waits STICK_SHUT."""
    global _stick
    import time
    now = time.monotonic()
    if _stick is not None and _stick[1] is None and now - _stick[0] >= STICK_SHUT:
        _stick[1] = now
        _log_switch('eyes shut: the stick ends the scene')
        return True
    if _stick is not None:
        return _stick[1] is not None
    if not in_scene(sim):
        return True
    _stick = [now, None]
    _log_switch('eyes closing: the stick leaves the scene')
    _tell()
    return False


def scene_tick(sim):
    """walk.py, every tick in first person: a WickedWhims animation switch."""
    global _switch, _last_animation, _last_seen, _stick, _clips_now, _clips_before, _clips_wanted
    try:
        import time
        now = time.monotonic()
        if _stick is not None:
            asked, cancelled = _stick
            if cancelled is not None and now - cancelled >= STICK_OPEN:
                _stick = None
                _log_switch('eyes open: {:.1f} s after the stick ended the scene'.format(now - cancelled))
            elif cancelled is None and now - asked >= STICK_GIVE_UP:
                _stick = None
                _log_switch('eyes open: the stick let go before the scene ended')
        controller = sim.transition_controller
        heading = getattr(controller, 'interaction', None) if controller is not None else None
        heading = heading if _animation(heading) else None
        playing = None
        if heading is None:
            for si in sim.si_state:
                if _animation(si):
                    playing = si
                    break
        current = heading or playing
        if current is not None:
            if current.id != _last_animation:
                _clips_before, _clips_now, _clips_wanted = _clips_now, None, now
            if _last_animation and current.id != _last_animation and now - _last_seen < SWITCH_GAP and \
                    (_switch is None or _switch[0] != current.id):
                _switch = (current.id, now, None)
                _log_switch('eyes closing: the scene goes on to {}'.format(current))
            _last_animation = current.id
            _last_seen = now
        if _switch is not None:
            sid, since, sent = _switch
            if sent is None and (_last_arb > since or (playing is not None and playing.id == sid)):
                _switch = (sid, since, now)
            elif sent is not None and now - sent >= SWITCH_SETTLE:
                _switch = None
                _log_switch('eyes open: the new animation went out {:.2f} s after the switch began'.format(
                    sent - since))
            if _switch is not None and now - since > SWITCH_MAX:
                _switch = None
                _log_switch('eyes open: no new animation went out in {:.1f} s'.format(SWITCH_MAX))
        _tell()
    except Exception as e:
        _log_switch('scene check failed: {!r}'.format(e))


def _log_switch(message):
    global _logs
    if _logs < MAX_LOGS:
        _logs += 1
        native._log('fades: ' + message)


def _log(obj, op, opacity, covered):
    global _logs
    if _logs >= MAX_LOGS:
        return
    _logs += 1
    parts = []
    frame = sys._getframe(3)
    while frame is not None and len(parts) < 5:
        path = frame.f_code.co_filename.replace('\\', '/')
        if not path.endswith(('sims4vr/fades.py', 'distributor/ops.py', 'distributor/system.py')):
            parts.append('{}:{}'.format('/'.join(path.split('/')[-2:]), frame.f_code.co_name))
        frame = frame.f_back
    native._log('fades: your Sim {} to {:.2f} over {} ({}): eyes {} by {}'.format(
        type(op).__name__, opacity, getattr(op, 'duration', None), 'immediate' if getattr(op, 'immediate', False) else
        'eased', 'closing' if covered else 'open', ' < '.join(parts) or '?'))
