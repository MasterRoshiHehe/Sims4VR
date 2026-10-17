"""Sound log (first person): who starts each sound through the game's PlaySound
and on what. A sound on no object plays flat, in the middle of your head; one
on your own Sim plays from your head too. Some of a partner's sounds came from
the middle of the head (user 2026-10-17 04:39Z/04:42Z); this shows which ones
and who played them. Sounds started by animations themselves don't pass here.
Sound starts sent as their own message (not through PlaySound, e.g. by other
mods) are logged from the message itself: who made it and on what object.
Sounds in the animation clips themselves (clip Sound events, played by the
game's audio engine) are logged as the animation's events pass: the sound, the
clip and the Sim it plays on (user 11:54Z: voices of Sims in WickedWhims scenes
without you play mid-head).
"""
import sys

from sims4vr import native

MAX_LOGS = 150
_seen = set()  # (sound, target kind, caller) already logged
_logs = 0
_installed = False


def install():
    """Wrap the game's PlaySound.start (on top of simsforever.py's wrap, if any)."""
    global _installed
    if _installed:
        return
    from audio.primitive import PlaySound
    original = PlaySound.start

    def start(self):
        result = original(self)
        try:
            note(self)
        except Exception as e:
            native._log('sound log failed: {!r}'.format(e))
        return result

    PlaySound.start = start
    _installed = True
    try:
        _install_ops()
    except Exception as e:
        native._log('sound log: message wrap failed: {!r}'.format(e))
    try:
        _install_clips()
    except Exception as e:
        native._log('sound log: clip event wrap failed: {!r}'.format(e))


def _install_clips():
    """Clip Sound events as an animation's events are handled."""
    from native.animation import arb
    original = arb.NativeArb.handle_events

    def handle_events(self, events=None, event_context=None):
        if events is None:
            events = self._events()
        try:
            for event_type, _, data in events:
                if _types_left > 0:
                    _count_type(event_type)
                if event_type == arb.ClipEventType.Sound:
                    _note_clip(data)
        except Exception as e:
            native._log('sound log failed: {!r}'.format(e))
        return original(self, events, event_context)

    arb.NativeArb.handle_events = handle_events


_clip_keys_logged = False
_clip_logs = 0
_types_left = 3000  # animation events counted by type, then logged once
_types = {}


def _count_type(event_type):
    """Which clip event types the game's animations pass to scripts at all."""
    global _types_left
    _types[event_type] = _types.get(event_type, 0) + 1
    _types_left -= 1
    if _types_left == 0:
        native._log('sound log: animation event types in the first 3000: {}'.format(
            ', '.join('{} x{}'.format(getattr(k, 'name', k), v) for k, v in sorted(_types.items(), key=lambda kv: -kv[1]))))


def _note_clip(data):
    """A clip Sound event: its data names the sound, the clip and the actors."""
    global _clip_logs, _clip_keys_logged
    if _clip_logs >= MAX_LOGS:
        return
    from sims4vr import body
    if not body.enabled or not isinstance(data, dict):
        return
    if not _clip_keys_logged:
        _clip_keys_logged = True
        native._log('sound (clip event) data: {}'.format(
            ', '.join('{}={!r}'.format(k, v)[:80] for k, v in sorted(data.items(), key=lambda kv: str(kv[0])))))
    import services
    actors = []
    target = None
    for key, value in sorted(data.items(), key=lambda kv: str(kv[0])):
        if str(key).endswith('actor_id') and value:
            obj = services.object_manager().get(value)
            actors.append('{} {}'.format(key, _target_kind(obj) if obj is not None else value))
            target = target or obj
    name = data.get('sound_name') or data.get('sound_id') or '?'
    clip = data.get('clip_name', '?')
    kind = ', '.join(actors) or 'no actor'
    key = (name, kind, 'clip')
    if key in _seen:
        return
    _seen.add(key)
    _clip_logs += 1
    native._log('sound (clip event): {} in {} on {}{}'.format(name, clip, kind, _distance(target)))


def _install_ops():
    """Sound starts sent as their own message: GenericProtocolBufferOp remembers
    who made one, Op.serialize_op logs it when it goes to the client."""
    import distributor.ops
    from protocolbuffers import DistributorOps_pb2
    from audio.primitive import PlaySound
    sound_start = DistributorOps_pb2.Operation.SOUND_START
    generic = distributor.ops.GenericProtocolBufferOp
    original_init = generic.__init__
    original_serialize = distributor.ops.Op.serialize_op

    def init(self, *args, **kwargs):
        original_init(self, *args, **kwargs)
        if getattr(self, 'type_constant', None) == sound_start:
            try:
                made_in = sys._getframe(1).f_code.co_filename.replace('\\', '/')
                self._s4vr_by = None if made_in.endswith('audio/primitive.py') else _caller()  # PlaySound's: logged there
            except Exception:
                pass

    def serialize_op(self, msg, op, op_type):
        original_serialize(self, msg, op, op_type)
        if op_type == sound_start and not isinstance(self, PlaySound) and getattr(self, '_s4vr_by', '') is not None:
            try:
                _note_message(self, op)
            except Exception as e:
                native._log('sound log failed: {!r}'.format(e))

    generic.__init__ = init
    distributor.ops.Op.serialize_op = serialize_op


def _caller():
    """file:function of the first frame outside the game's audio code and ours."""
    frame = sys._getframe(1)
    parts = []
    while frame is not None and len(parts) < 5:
        path = frame.f_code.co_filename.replace('\\', '/')
        if not path.endswith(('audio/primitive.py', 'sims4vr/simsforever.py', 'sims4vr/soundlog.py')):
            parts.append('{}:{}'.format('/'.join(path.split('/')[-2:]), frame.f_code.co_name))
        frame = frame.f_back
    return ' < '.join(parts) or '?'


def _target(sound):
    target = getattr(sound, 'target', None)
    if target is None:
        return 'nothing (plays flat, mid-head)', None
    return _target_kind(target), target


def _target_kind(target):
    import services
    if target is services.get_active_sim():
        return 'your Sim (plays at your head)'
    if getattr(target, 'is_sim', False):
        return 'Sim {}'.format(getattr(target, 'full_name', target.id))
    return 'object {}'.format(target)


def _note_message(owner, start):
    """A SoundStart message not sent by PlaySound."""
    global _logs
    if _logs >= MAX_LOGS:
        return
    from sims4vr import body
    if not body.enabled:
        return
    import services
    target = services.object_manager().get(start.object_id) if start.object_id else None
    if start.object_id == 0:
        kind = 'nothing (plays flat, mid-head)'
    elif target is None:
        kind = 'object id {} not found'.format(start.object_id)
    else:
        kind = _target_kind(target)
    name = start.sound_name or start.sound_id
    caller = getattr(owner, '_s4vr_by', None) or '{}.{}'.format(type(owner).__module__, type(owner).__name__)
    key = (name, kind, caller)
    if key in _seen:
        return
    _seen.add(key)
    _logs += 1
    native._log('sound (own message): {} on {}{} (vox {}, joint {}, active Sim only {}) by {}'.format(
        name, kind, _distance(target), start.is_vox, start.joint_name_hash or None, start.play_on_active_sim_only, caller))


def _distance(target):
    try:
        import services
        you = services.get_active_sim()
        if target is not None and you is not None and target is not you:
            return ', {:.1f} m from you'.format((target.position - you.position).magnitude())
    except Exception:
        pass
    return ''


def note(sound):
    """A sound the game's PlaySound starts."""
    global _logs
    if _logs >= MAX_LOGS:
        return
    from sims4vr import body
    if not body.enabled:
        return
    kind, target = _target(sound)
    name = getattr(sound, 'sound_name', None) or getattr(sound, 'sound_id', None)
    caller = _caller()
    key = (name, kind, caller)
    if key in _seen:
        return
    _seen.add(key)
    _logs += 1
    native._log('sound: {} on {}{} (vox {}, joint {}, active Sim only {}) by {}'.format(
        name, kind, _distance(target), getattr(sound, 'is_vox', None), getattr(sound, 'joint_name_hash', None),
        getattr(sound, 'play_on_active_sim_only', None), caller))
