"""The lot's arrival banner in first person, shown while your eyes are still shut (user 2026-10-26).

The game shows lot name, household and trait icons itself as the ScreenSlam "travel billboard" when a load into
live mode ends (TransitionScreenMolecule.Hide -> ShowScreenSlam, ScreenSlamTunables.SLAM_DURATION 4 s). It is Flash
UI. Drawn by us it looked wrong (user 23:48Z), so the DLL shows a crop of the vanilla UI's middle in front of you
(hud_lotintro.inc, 12_hud.inc hudLotIntroLayer; the one element allowed to be a crop, it only shows with the eyes
shut). loadshut.py calls start() when you can move and keeps the eyes shut and time paused for the seconds it
returns (a stick push skips it). Switch: [wrist] lot_intro (debug menu HUD > Lot name and traits on arrival).
"""
from sims4vr import native

SECONDS = 4.0  # ScreenSlamTunables.SLAM_DURATION
OUT = 0.25     # the slam's 200 ms out-tween and a frame or two


def start():
    """Show the banner; the seconds the eyes stay shut for it, 0 when off or the DLL is older than this script."""
    if not native.setting_flag('wrist', 'lot_intro', True) or native._optional('lot_intro') is None:
        return 0.0
    native.lot_intro(str(SECONDS).encode('ascii'))
    native._log('lotintro: banner for {:.1f} s'.format(SECONDS + OUT))
    return SECONDS + OUT


def stop():
    native.lot_intro(b'')
