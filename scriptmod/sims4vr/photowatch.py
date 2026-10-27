"""Probe: logs the game's photo / selfie session start and the camera updates after it.

Wraps _BasePhotoMode.create_take_photo_op (the server builds the TAKE_PHOTO op
the client turns into its photography camera). No behaviour change. Log lines
start with "Photo probe:".
"""
import time

import camera
from crafting.take_photo import TakePhoto, _BasePhotoMode
import services
from event_testing.event_manager_service import EventManagerService
from event_testing import test_events

from sims4vr import hooks, native

_after = 0  # camera.update calls still to log after a photo op


def _create(original, self, sims, interaction, *args, **kwargs):
    global _after
    try:
        native._log('Photo probe: op {} mode {} sims {} interaction {} zoom {} orient {} hide_photographer {} bone {!r} number {}'.format(
            type(self).__name__, self._get_camera_mode(), [getattr(s, 'sim_id', s) for s in sims],
            type(interaction).__name__, self.zoom_capability, self.orientation, self.hide_photographer,
            self.camera_position_bone_name, self.number_of_photos_override))
        _after = 40
        native.photo_probe(600)
    except Exception as e:
        native._log('Photo probe: op log failed {!r}'.format(e))
    return original(self, sims, interaction, *args, **kwargs)


def _update(original, *args, **kwargs):
    global _after
    if _after > 0:
        _after -= 1
        try:
            p = kwargs.get('camera_position')
            t = kwargs.get('target_position')
            native._log('Photo probe: camera.update at {:.2f} position {} target {} extra {}'.format(
                time.time(), None if p is None else (round(p.x, 3), round(p.y, 3), round(p.z, 3)),
                None if t is None else (round(t.x, 3), round(t.y, 3), round(t.z, 3)),
                sorted(k for k in kwargs if k not in ('camera_position', 'target_position'))))
        except Exception:
            pass
    return original(*args, **kwargs)


def _do_behavior(original, self, *args, **kwargs):
    # [photo] game_camera = 0 (debug menu, Phone): no photo session in first person, to test the view
    # without the game's photography camera. Default 1: the game's own session.
    try:
        if not native.setting_flag('photo', 'game_camera', True):
            native._log('Photo probe: Take Photo skipped in first person (no game photography camera) {}'.format(
                type(self.photo_mode).__name__))
            services.get_photography_service().cleanup()
            return True
    except Exception as e:
        native._log('Photo probe: skip failed {!r}'.format(e))
    result = original(self, *args, **kwargs)
    _session(True)
    return result


def _session(on):
    # The native side draws the phone lens as a third frame while a session runs (camera.cpp photoSession).
    try:
        native.photo_session(1 if on else 0)
    except Exception as e:
        native._log('Photo probe: session flag failed {!r}'.format(e))


def _event(original, self, event_type, *args, **kwargs):
    if event_type is test_events.TestEvent.ExitedPhotoMode:
        _session(False)
    return original(self, event_type, *args, **kwargs)


hooks.hook('photowatch', EventManagerService, 'process_event', _event, first_person_only=False)
hooks.hook('photowatch', _BasePhotoMode, 'create_take_photo_op', _create, first_person_only=False)
hooks.hook('photowatch', camera, 'update', _update, first_person_only=False)
hooks.hook('photowatch', TakePhoto, '_do_behavior', _do_behavior, first_person_only=True)
