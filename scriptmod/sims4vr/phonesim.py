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
- careers (the CareerPanel; school and after-school activities are careers
  too): per career its track's icon and career_name, the level's title, the
  user level of the track's levels, work performance between the performance
  stat's minimum and the level's promote_performance_level, hourly pay, company,
  and the next shift (get_next_work_time) or "At work";
- the mood (Sim's mood: its mood_names at the current intensity, colour index
  as hudinfo's moodlets).
The needs, moodlets and wants come from hudinfo.py (they are on the wrist already).

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
  "job"   \x1f <icon> \x1f <career name json> \x1f <title json> \x1f <level> \x1f <levels> \x1f <performance 0..1>
          \x1f <pay text> \x1f <company json> \x1f <shift text>
  "mood"  \x1f <mood index (hudinfo MOODS)> \x1f <name json>
  "hh"    \x1f <index> \x1f <first name> \x1f <full name> \x1f <age> \x1f <here 0/1> \x1f <active 0/1>
          \x1f <face picture> \x1f <bust picture>   (the Household app: the household tray's members)
Contacts (the phone's main screen app, user 2026-10-24 01:08Z; the game's relationship panel and the
social hover: what your Sim knows of each Sim, sim_knowledge.py, as sim_relationship._build_relationship_update_proto):
  "ct"    \x1f <index> \x1f <name> \x1f <face picture> \x1f <friendship -100..100> \x1f <romance -100..100 or "">
          \x1f <friendship bit name json> \x1f <romance bit name json> \x1f <here 0/1>
  and for the contact picked (control 7300000 + n):
  "cd"    \x1f <index> \x1f <bust picture> \x1f <unknown personality traits>
  "cdt"   \x1f <group> \x1f <icon> \x1f <name json>        (known traits, likes/dislikes, turn-ons/offs)
  "cdk"   \x1f <label> \x1f <text json>                     (known career, major, net worth, romantic interest)
  "cds"   \x1f <icon> \x1f <name json> \x1f <description json> (sentiments, your Sim's toward them)
  "cdb"   \x1f <icon> \x1f <name json>                      (other visible relationship bits: spouse, roommate...)
Control 87 = Contacts is open (send with contacts).
Control 7200000 + n = play as "hh" n (user 2026-10-24 00:39Z: eyes close, the game switches Sim, eyes open).
Control 7000000 + n = change to "atrack" n. Control 85 = My Sim is open (send; the DLL asks again every 2 s while it is).
"""
from sims4vr import native

_CATEGORY_TITLES = (675525327, 1883184146)  # KeysGP.SIM_INFO_SKILL_PANEL_CATEGORY_DEFAULT, _CHILD
_sent = None
_errors = 0
_contacts = []  # the "ct" Sims sent last, by index (sim ids)
_contact = None  # the sim id of the contact whose page is open
_household = []  # the "hh" Sims sent last, by index (sim ids)
_switch = None  # Play as under way: [target sim id, started (monotonic), switched]
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


_DAYS = ('Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat')


def _clock(t):
    hour, minute = int(t.hour()), int(t.minute())
    return '{}{} {}'.format((hour + 11) % 12 + 1, ':{:02d}'.format(minute) if minute else '', 'AM' if hour < 12 else 'PM')


def _careers(sim_info, records):
    from sims4vr import hudinfo
    tracker = getattr(sim_info, 'career_tracker', None)
    if tracker is None:
        return
    for career in list(tracker.careers.values()):
        try:
            track = career.current_track_tuning
            level = career.current_level_tuning
            name = _text(getattr(track, 'career_name', None), sim_info)
            title = _text(getattr(level, 'title', None), sim_info)
            performance = 0.0
            try:
                stat = level.performance_stat
                low = getattr(stat, 'min_value_tuning', None)
                low = getattr(stat, 'min_value', -100) if low is None else low
                high = career._get_promote_performance_level() if hasattr(career, '_get_promote_performance_level') else 100
                performance = max(0.0, min(1.0, (career.work_performance - low) / float(high - low))) if high > low else 0.0
            except Exception:
                pass
            pay = ''
            try:
                pay = '\u00a7{}/hour'.format(int(career.get_hourly_pay()))
            except Exception:
                pass
            company = ''
            try:
                company = _loc(career.get_company_name())
            except Exception:
                pass
            shift = ''
            try:
                if career.currently_at_work:
                    shift = 'At work now'
                else:
                    _, start, end = career.get_next_work_time()
                    if start is not None:
                        shift = 'Next: {} {} - {}'.format(_DAYS[int(start.day()) % 7], _clock(start), _clock(end) if end is not None else '')
            except Exception:
                pass
            records.append('job\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{:.3f}\x1f{}\x1f{}\x1f{}'.format(
                hudinfo._icon(getattr(track, 'icon', None)), name, title, int(getattr(career, 'user_level', 0) or 0),
                len(getattr(track, 'career_levels', ()) or ()), performance, pay, company, shift))
        except Exception as e:
            _log_error('a career', e)


def _mood(sim_info, records):
    from sims4vr import hudinfo
    mood = sim_info.get_mood()
    if mood is None:
        return
    names = list(getattr(mood, 'mood_names', ()) or ())
    name = ''
    if names:
        try:
            intensity = int(sim_info.get_mood_intensity())
        except Exception:
            intensity = 1
        name = _loc(names[max(0, min(len(names) - 1, intensity - 1))])
    records.append('mood\x1f{}\x1f{}'.format(hudinfo._mood_index(mood), name))


def _members(sim_info, records):
    """The household tray's Sims (the active household, the game's skewer order: as the household's sim_info_gen)."""
    from sims4vr import hudinfo
    household = sim_info.household
    _household.clear()
    if household is None:
        return
    for n, member in enumerate(household.sim_info_gen()):
        _household.append(member.id)
        try:
            here = int(member.is_instanced() and member.get_is_enabled_in_skewer())
        except Exception:
            here = 0
        records.append('hh\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
            n, member.first_name, '{} {}'.format(member.first_name, member.last_name).strip(), int(member.age), here,
            int(member is sim_info), hudinfo._thumb('sims/f_{}_l_0'.format(member.id)), hudinfo._sim_bust(member.id)))


def _text_for(factory, actor, target):
    """A relationship bit's / trait's text with the game's tokens (actor, target), or fewer."""
    if factory is None:
        return ''
    for args in ((actor, target), (actor,), ()):
        try:
            return _loc(factory(*args))
        except Exception:
            continue
    return ''


def _contact_list(sim_info, records):
    import services
    from sims4vr import hudinfo
    from relationships.relationship_track import RelationshipTrack
    tracker = sim_info.relationship_tracker
    friendship, romance = RelationshipTrack.FRIENDSHIP_TRACK, RelationshipTrack.ROMANCE_TRACK
    manager = services.sim_info_manager()
    found = []
    for target_id in list(tracker.target_sim_gen()):
        target = manager.get(target_id)
        if target is None or target is sim_info:
            continue
        try:
            f = tracker.get_relationship_score(target_id, friendship)
            r = tracker.get_relationship_score(target_id, romance) if romance is not None else 0
        except Exception:
            continue
        found.append((max(abs(f), abs(r)) + (5 if target.household is sim_info.household else 0), target, f, r))
    found.sort(key=lambda x: -x[0])
    _contacts[:] = [t.id for _, t, _, _ in found[:60]]
    for n, (_, target, f, r) in enumerate(found[:60]):
        def bit_of(track):
            try:
                t = tracker.get_relationship_track(target.id, track)
                bit = t.get_bit_for_client() if t is not None else None
                return _text_for(getattr(bit, 'display_name', None), sim_info, target) if bit is not None else ''
            except Exception:
                return ''
        has_romance = romance is not None and tracker.get_relationship_track(target.id, romance) is not None
        records.append('ct\x1f{}\x1f{}\x1f{}\x1f{:.0f}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
            n, '{} {}'.format(target.first_name, target.last_name).strip(), hudinfo._thumb('sims/f_{}_m_0'.format(target.id)),
            f, '{:.0f}'.format(r) if has_romance else '', bit_of(friendship), bit_of(romance) if has_romance else '',
            int(target.is_instanced())))


def _contact_page(sim_info, records):
    import services
    from sims4vr import hudinfo
    if _contact not in _contacts:
        return
    target = services.sim_info_manager().get(_contact)
    if target is None:
        return
    tracker = sim_info.relationship_tracker
    knowledge = tracker.get_knowledge(target.id)
    known = set(knowledge.known_traits) if knowledge is not None else set()
    personality = list(target.trait_tracker.personality_traits)
    unknown = sum(1 for t in personality if t not in known)
    records.append('cd\x1f{}\x1f{}\x1f{}'.format(_contacts.index(_contact), hudinfo._sim_bust(target.id), unknown))
    try:
        from traits.attraction_preference import AttractionPreference
    except Exception:
        AttractionPreference = ()
    from traits.trait_type import TraitType
    ordered = [t for t in personality if t in known] + [t for t in known if t not in personality]
    for trait in ordered:
        if trait.is_personality_trait:
            group = 'Traits'
        elif AttractionPreference and isinstance(trait, type) and issubclass(trait, AttractionPreference):
            group = 'Turn-ons' if trait.trait_type == TraitType.LIKE else 'Turn-offs'
        elif trait.trait_type == TraitType.LIKE:
            group = 'Likes'
        elif trait.trait_type == TraitType.DISLIKE:
            group = 'Dislikes'
        else:
            group = 'Also'
        records.append('cdt\x1f{}\x1f{}\x1f{}'.format(group, hudinfo._icon(getattr(trait, 'icon', None)),
                                                      _text_for(getattr(trait, 'display_name', None), target)))
    if knowledge is not None:
        try:
            for career in knowledge.get_known_careers():
                track = getattr(career, 'current_track_tuning', None)
                name = _text(getattr(track, 'career_name', None), target) if track is not None else ''
                if not name:
                    name = _loc(getattr(career, 'display_name', None) and career.display_name(target)) if hasattr(career, 'display_name') else ''
                records.append('cdk\x1fCareer\x1f{}'.format(name or 'No job'))
        except Exception as e:
            _log_error('a known career', e)
        try:
            if knowledge.knows_major and knowledge.get_known_major() is not None:
                records.append('cdk\x1fMajor\x1f{}'.format(_text(knowledge.get_known_major().display_name, target)))
        except Exception:
            pass
        try:
            if knowledge.knows_net_worth:
                records.append('cdk\x1fNet worth\x1f\u00a7{:,}'.format(int(knowledge.known_net_worth)))
        except Exception:
            pass
        try:
            if knowledge.knows_romantic_preference:
                from sims.sim_info_types import Gender
                genders = sorted(knowledge.known_romantic_genders)
                text = ', '.join('Men' if g == Gender.MALE else 'Women' for g in genders) or 'Nobody'
                records.append('cdk\x1fRomantic interest\x1f{}'.format(text))
        except Exception:
            pass
    from relationships.relationship_enums import RelationshipTrackType
    try:
        for track in tracker.relationship_tracks_gen(target.id, track_type=RelationshipTrackType.SENTIMENT):
            bit = track.get_bit_for_client()
            if bit is None:
                continue
            records.append('cds\x1f{}\x1f{}\x1f{}'.format(hudinfo._icon(getattr(bit, 'icon', None)),
                           _text_for(bit.display_name, sim_info, target), _text_for(getattr(bit, 'bit_description', None), sim_info, target)))
    except Exception as e:
        _log_error('sentiments', e)
    try:
        track_bits = set()
        for track in tracker.relationship_tracks_gen(target.id):
            try:
                b = track.get_bit_for_client()
                if b is not None:
                    track_bits.add(b)
            except Exception:
                pass
        for bit in tracker.get_all_bits(target.id):
            if bit in track_bits or not getattr(bit, 'visible', False) or getattr(bit, 'invisible_filterable', False):
                continue
            name = _text_for(getattr(bit, 'display_name', None), sim_info, target)
            if name:
                records.append('cdb\x1f{}\x1f{}'.format(hudinfo._icon(getattr(bit, 'icon', None)), name))
    except Exception as e:
        _log_error('relationship bits', e)


def select_contact(index):
    """Contacts: the page of contact n (its details are sent with the list)."""
    global _contact
    _contact = _contacts[index] if 0 <= index < len(_contacts) else None
    send(fresh=True, contacts=True)


def play_as(index):
    """The Household app's Play as: the eyes shut, then the game's own switch (client.set_active_sim_info,
    as the skewer; onesim only refuses the next-Sim key and set_active_sim_by_id), then they open (tick)."""
    global _switch
    import time
    if _switch is not None or not 0 <= index < len(_household):
        return
    _switch = [_household[index], time.monotonic(), False]
    native.set_eyes_held(True)


def tick():
    """controls.py, every tick: carries Play as on (switch once the eyes are shut, open them after)."""
    global _switch
    if _switch is None:
        return
    import time
    import services
    elapsed = time.monotonic() - _switch[1]
    if not _switch[2] and elapsed >= 0.35:
        _switch[2] = True
        try:
            client = services.client_manager().get_first_client()
            target = services.sim_info_manager().get(_switch[0])
            if client is not None and target is not None and target.is_instanced() and target is not client.active_sim_info:
                client.set_active_sim_info(target)
                native._log('phonesim: now playing {} {}'.format(target.first_name, target.last_name))
                from sims4vr import phone
                if phone._out:  # the phone into the new Sim's hand
                    phone._remove_prop()
                    phone._make_prop()
        except Exception as e:
            _log_error('Play as', e)
    if elapsed >= 1.6:
        _switch = None
        native.set_eyes_held(False)
        send(fresh=True)


def send(fresh=False, contacts=False):
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
            for part in (_careers, _mood, _members) + ((_contact_list, _contact_page) if contacts else ()):
                try:
                    part(sim_info, records)
                except Exception as e:
                    _log_error(part.__name__, e)
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
    if fresh or data != _sent:
        _sent = data
        native.phone_sim(data)
