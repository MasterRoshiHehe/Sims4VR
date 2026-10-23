"""The phone's My Sim app (user 2026-10-23 21:35Z: the HUD's lower right panels on
the phone), drawn by the DLL (hud.cpp drawMySim) from what is gathered here the
way the game's panels gather it:
- skills: SimInfoSkillPanelMain (its data: the Sim's Skill statistics,
  statistics/skill.py): visible (not hidden) skills that are learned (level 1+)
  or rising, grouped by skill_category (KeysGP.SIM_INFO_SKILL_PANEL_CATEGORY_*,
  highest category first as CompareCategories), each its icon, name, level,
  the progress through the level (_get_level_bounds), mastered (max level) and
  rising (change rate > 0, the cell's mcHighlight).
The needs come from hudinfo.py (they are on the wrist already).

To the DLL (native.phone_sim), records split by 0x1e, fields by 0x1f:
  "sim"   \x1f <age (sims.sim_info_types.Age)>
  "cat"   \x1f <category> \x1f <title json>
  "skill" \x1f <category> \x1f <icon> \x1f <name json> \x1f <level> \x1f <max level> \x1f <progress 0..1>
          \x1f <mastered 0/1> \x1f <rising 0/1>
Control 85 = My Sim is open (send; the DLL asks again every 2 s while it is).
"""
from sims4vr import native

_CATEGORY_TITLES = (675525327, 1883184146)  # KeysGP.SIM_INFO_SKILL_PANEL_CATEGORY_DEFAULT, _CHILD
_sent = None
_errors = 0


def _log_error(where, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('phonesim: {} failed: {!r}'.format(where, e))


def _loc(value):
    from sims4vr import objmenu
    return objmenu._loc(value) if value is not None else ''


def _key(string_id):
    from sims4.localization import _create_localized_string
    return _loc(_create_localized_string(string_id))


def _skills(sim_info):
    from sims4vr import hudinfo
    out = []
    for stat in sim_info.all_skills():
        try:
            if not getattr(stat, 'is_skill', False) or getattr(stat, 'hidden', False):
                continue
            level = int(stat.get_user_value())
            rate = stat.get_change_rate() if hasattr(stat, 'get_change_rate') else 0
            rising = 1 if rate and rate > 0 else 0
            if level < 1 and not rising:
                continue  # IsValidForDisplay: learned, or being learned
            top = int(getattr(stat, 'max_level', level) or level)
            low, high = type(stat)._get_level_bounds(level)
            value = stat.get_value()
            mastered = 1 if level >= top else 0
            progress = 1.0 if mastered or high <= low else max(0.0, min(1.0, (value - low) / float(high - low)))
            category = int(getattr(stat, 'skill_category', 0) or 0)
            out.append((category, hudinfo._icon(getattr(stat, 'icon', None)), _loc(getattr(stat, 'stat_name', None)), level,
                        top, progress, mastered, rising))
        except Exception as e:
            _log_error('a skill', e)
    out.sort(key=lambda s: -s[0])  # CompareCategories: the higher category first (the game's sort is not by name)
    return out


def send():
    global _sent
    if native._optional('phone_sim') is None:
        return
    import services
    sim_info = services.active_sim_info()
    records = []
    try:
        if sim_info is not None:
            records.append('sim\x1f{}'.format(int(sim_info.age)))
            skills = _skills(sim_info)
            for category in sorted({s[0] for s in skills}, reverse=True):
                title = _CATEGORY_TITLES[category] if 0 <= category < len(_CATEGORY_TITLES) else _CATEGORY_TITLES[0]
                records.append('cat\x1f{}\x1f{}'.format(category, _key(title)))
            for s in skills:
                records.append('skill\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{:.4f}\x1f{}\x1f{}'.format(*s))
    except Exception as e:
        _log_error('My Sim', e)
        return
    data = '\x1e'.join(records).encode('utf-8')
    if data != _sent:
        _sent = data
        native.phone_sim(data)
