"""First person: the game's phone never shows in your Sim's hand.

Your Sim has its own phone (phone.py). The game's phone calls (quit job, order
service...) and phone idles would put the game's cell phone prop in the same
hand. For the Sim you are in, in first person:
- AnimationContext._get_prop: a prop the animations make that is a phone is
  hidden (checked at every use, so it stays hidden); the call itself plays
  as before. Our own phone is made by phone.py, not here.
- InteractionQueue.append: the idles where a Sim takes a phone out to browse,
  text or play (autonomous browse, fame/celebrity/playground/retail phone
  mixers...) are refused unless you picked them.
Switch: [phone] no_game_phone in sims4vr.ini (default on).
"""
from sims4vr import hooks, native

PHONE_DEFINITIONS = (0x285C, 0x23819)
IDLE_WORDS = ('browse', 'playonphone', 'fanatic', 'sitphone', 'usephone', 'tend_phone', 'pose_phone',
              'checkphone', 'lookingphone', 'phonepanic', 'phonechat', 'phone_idle', 'idle_phone')

_logs = 0
_hidden = set()


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 80:
        native._log('nophone: ' + message)


def _on():
    return native.setting_flag('phone', 'no_game_phone', True)


def _first_person(sim):
    try:
        from sims4vr import body
        return sim is not None and body.enabled and getattr(sim, 'id', None) == body.following()
    except Exception:
        return False


def _is_phone(prop_name, definition_id):
    return definition_id in PHONE_DEFINITIONS or 'phone' in str(prop_name).lower()


def _wrap_get_prop():
    import animation

    def _get_prop(original, self, asm, prop_name, definition_id, interaction=None):
        prop = original(self, asm, prop_name, definition_id, interaction)
        try:
            sim = getattr(interaction, 'sim', None) if interaction is not None else None
            if prop is not None and _first_person(sim) and _on() and \
                    _is_phone(prop_name, definition_id or getattr(getattr(prop, 'definition', None), 'id', 0)):
                from sims4vr import phone
                if prop is not getattr(phone, '_phone', None):
                    from objects import VisibilityState
                    prop.visibility = VisibilityState(False)
                    if prop_name not in _hidden:
                        _hidden.add(prop_name)
                        _log('the game\'s phone prop "{}" hidden for {}'.format(prop_name, interaction))
        except Exception as e:
            _log('phone prop check failed: {!r}'.format(e))
        return prop

    hooks.hook('nophone', animation.AnimationContext, '_get_prop', _get_prop, first_person_only=True)


def _phone_idle(interaction):
    name = getattr(interaction, 'affordance', None)
    name = getattr(name, '__name__', None) or type(interaction).__name__
    name = name.lower()
    return 'phone' in name and any(word in name for word in IDLE_WORDS)


def _wrap_queue_append():
    from event_testing.results import TestResult
    from interactions.context import InteractionSource
    from interactions.interaction_finisher import FinishingType
    from interactions.interaction_queue import InteractionQueue
    yours = (InteractionSource.PIE_MENU, InteractionSource.SCRIPT_WITH_USER_INTENT)

    def append(original, self, interaction, *args, **kwargs):
        try:
            if _first_person(getattr(interaction, 'sim', None)) and _on() and \
                    getattr(getattr(interaction, 'context', None), 'source', None) not in yours and \
                    _phone_idle(interaction):
                _log('{}: your Sim does not take out a phone on its own'.format(interaction))
                try:
                    interaction.cancel(FinishingType.INTERACTION_QUEUE, cancel_reason_msg='VR first person: no phone idle')
                except Exception:
                    pass
                return TestResult(False, 'VR first person: no phone idle')
        except Exception as e:
            _log('idle check failed: {!r}'.format(e))
        return original(self, interaction, *args, **kwargs)

    hooks.hook('nophone', InteractionQueue, 'append', append, first_person_only=True)


try:
    _wrap_get_prop()
except Exception as _e:
    native._log('nophone: wrapping the animation props failed: {!r}'.format(_e))
try:
    _wrap_queue_append()
except Exception as _e:
    native._log('nophone: wrapping the queue failed: {!r}'.format(_e))
