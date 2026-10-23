"""The phone's My Sim app (user 2026-10-23 21:35Z: the HUD's lower right panels on
the phone), drawn by the DLL (hud.cpp drawMySim) from what is gathered here the
way the game's panels gather it:
- skills: SimInfoSkillPanelMain (its data: the Sim's Skill statistics,
  statistics/skill.py): visible (not hidden) skills that are learned (level 1+)
  or rising, grouped by skill_category (KeysGP.SIM_INFO_SKILL_PANEL_CATEGORY_*,
  highest category first as CompareCategories), each its icon, name, level,
  the progress through the level (_get_level_bounds), mastered (max level) and
  rising (change rate > 0, the cell's mcHighlight).
- aspirations: the Sim's primary aspiration track (sim_info.primary_aspiration,
  aspirations/aspiration_tuning.py AspirationTrack: display_text, icon, its
  milestones in level order), each milestone done or not
  (aspiration_tracker.milestone_completed), the current one's objectives
  (display_text, done: objective_completed, progress: the tracker's last value
  of it against goal_value) and how far it is (the live tile's ring); and the
  tracks to change to (ui.aspirations.set_primary, as the game's Change
  Aspiration: available, valid for the Sim, by category ui_sort_order).
The needs come from hudinfo.py (they are on the wrist already).

To the DLL (native.phone_sim), records split by 0x1e, fields by 0x1f:
  "sim"   \x1f <age (sims.sim_info_types.Age)>
  "cat"   \x1f <category> \x1f <title json>
  "skill" \x1f <category> \x1f <icon> \x1f <name json> \x1f <level> \x1f <max level> \x1f <progress 0..1>
          \x1f <mastered 0/1> \x1f <rising 0/1>
  "asp"   \x1f <track icon> \x1f <track name json> \x1f <track description json> \x1f <current milestone index>
          \x1f <milestones> \x1f <current's progress 0..1> \x1f <track done 0/1>
  "mile"  \x1f <index> \x1f <name json> \x1f <done 0/1>
  "goal"  \x1f <text json> \x1f <done 0/1> \x1f <value> \x1f <goal> \x1f <shows progress 0/1>
  "acat"  \x1f <category index> \x1f <name json>
  "atrack" \x1f <index> \x1f <category index> \x1f <icon> \x1f <name json> \x1f <current 0/1>
Control 7000000 + n = change to "atrack" n. Control 85 = My Sim is open (send; the DLL asks again every 2 s while it is).
"""
from sims4vr import native

_CATEGORY_TITLES = (675525327, 1883184146)  # KeysGP.SIM_INFO_SKILL_PANEL_CATEGORY_DEFAULT, _CHILD
_sent = None
_errors = 0
_tracks = (None, [], [])  # (sim id and age, categories, tracks) of the Change Aspiration list sent last


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


def _text(factory, sim_info):
    """A LocalizedStringFactory's text for the Sim (the game's tokens: the Sim)."""
    if factory is None:
        return ''
    try:
        return _loc(factory(sim_info))
    except Exception:
        try:
            return _loc(factory())
        except Exception:
            return ''


def _aspiration(sim_info, records):
    from sims4vr import hudinfo
    track = getattr(sim_info, 'primary_aspiration', None)
    tracker = getattr(sim_info, 'aspiration_tracker', None)
    if track is None or tracker is None:
        return
    milestones = [a for _, a in (track.get_aspirations() or ())]
    current, done_all = len(milestones) - 1, True
    for i, milestone in enumerate(milestones):
        if not tracker.milestone_completed(milestone):
            current, done_all = i, False
            break
    goals, progress = [], 1.0
    if milestones and not done_all:
        milestone = milestones[current]
        objectives = list(tracker.get_objectives(milestone))
        done = 0
        for objective in objectives:
            complete = bool(tracker.objective_completed(objective))
            done += complete
            try:
                goal = float(objective.goal_value())
            except Exception:
                goal = 1.0
            value = tracker.get_last_updated_value_for_objective(objective) if hasattr(
                tracker, 'get_last_updated_value_for_objective') else None
            value = goal if complete else float(value or 0)
            goals.append((_text(getattr(objective, 'display_text', None), sim_info), int(complete), int(value), int(goal),
                          int(bool(getattr(objective, 'show_progress', True)))))
        need = milestone.objective_completion_count() if hasattr(milestone, 'objective_completion_count') else None
        need = need if need and need > 0 else max(1, len(objectives))
        progress = min(1.0, done / float(need))
    records.append('asp\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{:.3f}\x1f{}'.format(
        hudinfo._icon(getattr(track, 'icon', None)), _loc(getattr(track, 'display_text', None)),
        _loc(getattr(track, 'description_text', None)), current, len(milestones), progress, int(done_all)))
    for i, milestone in enumerate(milestones):
        records.append('mile\x1f{}\x1f{}\x1f{}'.format(i, _loc(getattr(milestone, 'display_name', None)),
                                                       int(bool(tracker.milestone_completed(milestone)))))
    for g in goals:
        records.append('goal\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(*g))


def _track_list(sim_info):
    """The tracks the Sim can change to, by category (made once per Sim and age)."""
    global _tracks
    import services
    import sims4.resources
    key = (sim_info.sim_id, int(sim_info.age))
    if _tracks[0] == key:
        return _tracks
    from sims4vr import hudinfo
    by_category = {}
    manager = services.get_instance_manager(sims4.resources.Types.ASPIRATION_TRACK)
    for track in manager.types.values():
        try:
            if not track.get_aspirations() or not track.is_available() or not track.is_valid_for_sim(sim_info):
                continue
            category = getattr(track, 'category', None)
            if category is None or getattr(category, 'is_sim_info_panel', False):
                continue
            by_category.setdefault(category, []).append(track)
        except Exception as e:
            _log_error('a track', e)
    categories, tracks = [], []
    for c, category in enumerate(sorted(by_category, key=lambda c: getattr(c, 'ui_sort_order', 0))):
        categories.append(_loc(getattr(category, 'display_text', None)))
        for track in by_category[category]:
            tracks.append((c, track, hudinfo._icon(getattr(track, 'icon', None)), _loc(getattr(track, 'display_text', None))))
    _tracks = (key, categories, tracks)
    return _tracks


def select_track(index):
    """Change Aspiration on the phone: the track, as the game's ui.aspirations.set_primary."""
    import services
    sim_info = services.active_sim_info()
    _, _, tracks = _tracks
    if sim_info is None or not 0 <= index < len(tracks):
        return
    track = tracks[index][1]
    try:
        sim_info.primary_aspiration = track
        native._log('phonesim: aspiration changed to {}'.format(track.__name__))
    except Exception as e:
        _log_error('changing the aspiration', e)
    send()


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
            try:
                _aspiration(sim_info, records)
            except Exception as e:
                _log_error('the aspiration', e)
            try:
                _, categories, tracks = _track_list(sim_info)
                current = getattr(sim_info, 'primary_aspiration', None)
                for c, name in enumerate(categories):
                    records.append('acat\x1f{}\x1f{}'.format(c, name))
                for n, (c, track, icon, name) in enumerate(tracks):
                    records.append('atrack\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(n, c, icon, name, int(track is current)))
            except Exception as e:
                _log_error('the aspiration list', e)
    except Exception as e:
        _log_error('My Sim', e)
        return
    data = '\x1e'.join(records).encode('utf-8')
    if data != _sent:
        _sent = data
        native.phone_sim(data)
