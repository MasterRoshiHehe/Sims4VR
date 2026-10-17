"""Before the game saves, your hands put down what they hold.

Held objects hang off the Sim's hand joint (hands.py), which the game never
expects in a save; they go onto the nearest surface or the floor first, the
way the game puts down what a Sim carries. Wraps
PersistenceService._fill_and_send_save_game_protobufs_gen, the step every
save (manual, travel, exit) goes through.
"""
from services import persistence_service

from sims4vr import native

_original = getattr(persistence_service.PersistenceService, '_fill_and_send_save_game_protobufs_gen', None)


def _fill_and_send_save_game_protobufs_gen(self, *args, **kwargs):
    try:
        from sims4vr import hands
        hands.release_all('the game is saving')
        from sims4vr import phone
        phone.before_save()
    except Exception as e:
        native._log('saving: putting things down failed: {!r}'.format(e))
    result = yield from _original(self, *args, **kwargs)
    try:
        from sims4vr import phone
        phone.after_save()
    except Exception as e:
        native._log('saving: phone back in the hand failed: {!r}'.format(e))
    return result


if _original is not None:
    persistence_service.PersistenceService._fill_and_send_save_game_protobufs_gen = _fill_and_send_save_game_protobufs_gen
else:
    native._log('saving: save step not found; held objects are not put down before saving')
