"""First person: no live-mode pick-up (live drag) for now.

User 2026-10-16 11:24Z: clicking the mixing bowl to open its pie menu picked
it up instead, as a click and hold does in live mode; "disable the live mode
pick up items feature for now, then we will add it in later".

How the game does it (decompiled 1.121): the client asks with the live
command live_drag.start (server_commands/live_drag_commands.py), which calls
Client.start_live_drag (server/client.py). That method itself refuses an
object in use by sending send_live_drag_cancel(id) to the client and
returning; this does the same in first person, before anything is picked up.
God view keeps live drag. Switch: [body] live_drag = 1 allows it again.
"""
from sims4vr import native

_original = None
_logs = 0


def _blocked(client):
    if native.setting_flag('body', 'live_drag', False):
        return False
    from sims4vr import halt
    import services
    return halt.first_person(services.get_active_sim())


def _start_live_drag(self, live_drag_object, start_system, *args, **kwargs):
    global _logs
    try:
        blocked = _blocked(self)
    except Exception:
        blocked = False
    if not blocked:
        return _original(self, live_drag_object, start_system, *args, **kwargs)
    self._live_drag_start_system = start_system  # as start_live_drag sets it before a refusal
    self.send_live_drag_cancel(live_drag_object.id, live_drag_end_system=start_system)
    _logs += 1
    if _logs <= 20:
        native._log('livedrag: picking up {} refused (first person)'.format(live_drag_object))


def _wrap():
    global _original
    from server.client import Client
    original = Client.start_live_drag
    if getattr(original, '_sims4vr', False):
        return
    _original = original
    _start_live_drag._sims4vr = True
    Client.start_live_drag = _start_live_drag


try:
    _wrap()
except Exception as e:
    native._log('livedrag: wrapping start_live_drag failed: {!r}'.format(e))
