"""Build/buy mode, for the first-person tablet.

Tells the DLL whether the lot is in build/buy mode (native.set_build_buy):
in first person your Sim's phone gesture then brings out a tablet with the
whole build/buy UI on it instead of the phone (vr.cpp). Wraps
Zone.on_build_buy_enter / on_build_buy_exit, and controls.py also checks
zone.is_in_build_buy every tick in case the game calls those another way.

On entering build/buy it also tells the DLL which picture the played Sim's
own phone uses as its background (native.set_tablet_wallpaper), for the
tablet's background: the game keeps each Sim's choice in UserSetting.ini
("<sim id>#phone.background_image#string = <image instance>"), from the
list in PhoneTuning.PHONE_BACKGROUND_COLOR_VARIATION_TUNING. That list is
exported to the client only, so the picture is found by its instance alone
(native.resource_bytes type 0 = any picture).
"""
import os

import services

from sims4vr import native

_reported = None
_wallpaper_wanted = None  # (type, instance) still to read and send
_wallpaper_sent = None


def _report(on, why):
    global _reported
    on = bool(on)
    if on != _reported:
        _reported = on
        native._log('buildbuy: {} build/buy mode ({})'.format('in' if on else 'out of', why))
        native.set_build_buy(on)
        if on:
            try:
                _send_wallpaper()
            except Exception as e:
                native._log('buildbuy: phone background failed: {!r}'.format(e))


def _user_settings_path():
    """UserSetting.ini in the game's Documents folder (this script sits in its Mods folder)."""
    path = os.path.abspath(__file__)
    while True:
        parent = os.path.dirname(path)
        if parent == path:
            return None
        if os.path.basename(path).lower() == 'mods':
            return os.path.join(parent, 'UserSetting.ini')
        path = parent


def _chosen_background(sim_id):
    path = _user_settings_path()
    if not path or not os.path.isfile(path):
        return None
    key = '{}#phone.background_image#string'.format(sim_id)
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            name, sep, value = line.partition('=')
            if sep and name.strip() == key:
                try:
                    return int(value.strip())
                except ValueError:
                    return None
    return None


def _send_wallpaper():
    sim = services.get_active_sim()
    chosen = _chosen_background(sim.id) if sim is not None else None
    choices = []
    try:
        from sims.phone_tuning import PhoneTuning
        for entry in PhoneTuning.PHONE_BACKGROUND_COLOR_VARIATION_TUNING or ():
            image = getattr(entry, 'bg_image', None)
            if image is not None:
                choices.append((int(image.type), int(image.instance)))
    except Exception:
        pass
    # The list is exported to the game's client only (usually empty here), so a
    # Sim's choice is looked up by its instance as any picture (type 0).
    pick = next((c for c in choices if c[1] == chosen), None)
    if pick is None and chosen is not None:
        pick = (0, chosen)
    if pick is None and choices:
        pick = choices[0]
    if pick is None:
        native._log('buildbuy: no phone background chosen for this Sim and no default known: the tablet\'s own')
        return
    native._log('buildbuy: phone background {:08x}:{:016x} ({}; the Sim chose {})'.format(
        pick[0], pick[1], 'chosen' if chosen == pick[1] else 'default',
        chosen if chosen is not None else 'nothing'))
    global _wallpaper_wanted
    if pick != _wallpaper_sent:
        _wallpaper_wanted = pick


def poll():
    zone = services.current_zone()
    _report(zone is not None and getattr(zone, 'is_in_build_buy', False), 'zone')
    if _wallpaper_wanted is not None:
        try:
            _read_wallpaper()
        except Exception as e:
            _stop_wallpaper('failed: {!r}'.format(e))


def _stop_wallpaper(why):
    global _wallpaper_wanted
    native._log('buildbuy: phone background {}'.format(why))
    _wallpaper_wanted = None


def _read_wallpaper():
    """Reads the picture once the DLL has found it in the packages, unpacks it, hands it over."""
    global _wallpaper_wanted, _wallpaper_sent
    type_, instance = _wallpaper_wanted
    found = native.resource_bytes(type_, instance)
    if found is None:
        _stop_wallpaper('not read: the DLL is older than the script')
        return
    status, compression, size, data = found
    if status == 1:
        return  # still looking
    if status != 0:
        _stop_wallpaper('not found in the game or mods (status {})'.format(status))
        return
    from sims4vr import objmenu
    data = objmenu._unpack(compression, size, data)
    if not data:
        _stop_wallpaper('could not be unpacked (compression {:#x})'.format(compression))
        return
    native.set_tablet_wallpaper(type_, data)
    _wallpaper_sent = _wallpaper_wanted
    _wallpaper_wanted = None
    native._log('buildbuy: phone background sent ({} bytes, starts {!r})'.format(len(data), bytes(data[:4])))


def _wrap():
    from zone import Zone

    def wrap(name, on):
        original = getattr(Zone, name)

        def wrapped(self, *args, **kwargs):
            try:
                return original(self, *args, **kwargs)
            finally:
                try:
                    _report(on, name)
                except Exception as e:
                    native._log('buildbuy: report failed: {!r}'.format(e))
        wrapped.__name__ = name
        setattr(Zone, name, wrapped)

    wrap('on_build_buy_enter', True)
    wrap('on_build_buy_exit', False)


try:
    _wrap()
except Exception as e:
    native._log('buildbuy: wrapping the zone failed: {!r}'.format(e))
