"""One Sim in first person (user 2026-10-14 22:11Z, v0.13.115): while you are
in a Sim's body you can't switch to another Sim of your household. Refused:
clicking another Sim's portrait (and anything else that sends the game's
sims.set_active command), the game's next-Sim key (sims.set_next) and holding
the right stick down (controls.py). To play another Sim, go to the game camera
(hold the left stick down), switch there, and come back.

The game still switches on its own when it has to (your Sim left the lot, died,
left the household): then first person follows the new Sim (body.py).
God view keeps the game's switching. Switch: [body] keep_sim in sims4vr.ini.
"""
import sys

from sims4vr import native

_logs = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 40:
        native._log('onesim: ' + message)


def locked(client=None):
    """True while you are in first person and the Sim you are in is still here."""
    try:
        from sims4vr import body
        if not body.enabled or not native.setting_flag('body', 'keep_sim', True):
            return False
        if client is None:
            import services
            client = services.client_manager().get_first_client()
        info = getattr(client, 'active_sim_info', None) if client is not None else None
        return info is not None and info.is_instanced() and info.get_is_enabled_in_skewer()
    except Exception as e:
        _log('check failed: {!r}'.format(e))
        return False


def _refuse(client, what):
    _log('{}: refused, you stay in your Sim in first person'.format(what))
    try:
        client.resend_active_sim_info()  # the portraits show the Sim you are in again
    except Exception as e:
        _log('resending the active Sim failed: {!r}'.format(e))
    return False


def _wrap_client():
    from server.client import Client
    by_id = Client.__dict__.get('set_active_sim_by_id')
    next_sim = Client.__dict__.get('set_next_sim')
    if by_id is None or next_sim is None:
        native._log('onesim: Client switching methods not found')
        return

    def set_active_sim_by_id(self, sim_id, *args, **kwargs):
        # Only the sims.set_active command (portraits) calls this.
        current = self.active_sim_info
        if current is not None and current.id != sim_id and locked(self):
            return _refuse(self, 'switching to Sim {}'.format(sim_id))
        return by_id(self, sim_id, *args, **kwargs)

    def set_next_sim(self, *args, **kwargs):
        # The game also calls this itself when the active Sim can't stay active;
        # only the player's next-Sim command is refused.
        caller = sys._getframe(1)
        if caller.f_code.co_name == 'set_next_sim' and \
                caller.f_globals.get('__name__', '').endswith('sim_commands') and locked(self):
            return _refuse(self, 'next Sim')
        return next_sim(self, *args, **kwargs)

    Client.set_active_sim_by_id = set_active_sim_by_id
    Client.set_next_sim = set_next_sim


try:
    _wrap_client()
except Exception as _e:
    native._log('onesim: wrapping the client failed: {!r}'.format(_e))
