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
  "cdg"   \x1f <age icon> \x1f <age name>                  (the game's relationship tooltip, RelationshipTooltip:
  "cdj"   \x1f <icon> \x1f <career> \x1f <company> \x1f <hours> \x1f <work days, 7 x 0/1 from Sunday>   known careers
  "cdc"   \x1f <icon> \x1f <name> \x1f <description>        compatibility (CompatibilityTuning icons map)
  "cdx"   \x1f <icon> \x1f <name> \x1f <description>        attraction (AttractionTuning track's bit), your Sim's)
  "cda"   \x1f <phone menu node> \x1f <name json> \x1f <disabled text json>  (the phone's Social app entries, user
          2026-10-24 01:17Z: control 7400000 + node runs it and its Sim picker is answered with this contact;
          the Social app itself is left as it is)
Inventory (the game's Sim inventory panel: the visible storage, one entry a stack):
  "inv"   \x1f <index> \x1f <picture> \x1f <name> \x1f <count> \x1f <value>
Control 7500000 + n = the pie menu of "inv" n (at your hand, as a click on it in the inventory panel).
Social Groups (the game's club panel, clubs/club.py: the Sim's clubs from the club service):
  "club"  \x1f <index> \x1f <icon> \x1f <name> \x1f <description> \x1f <leader name> \x1f <gathering now 0/1>
  "cmem"  \x1f <club index> \x1f <face picture> \x1f <first name> \x1f <leader 0/1>
  "crule" \x1f <club index> \x1f <encouraged 0/1> \x1f <text>
Owned Businesses (business_service: the household's business managers, by zone or by owner for small businesses):
  "biz"   \x1f <type (BusinessType)> \x1f <name> \x1f <open 0/1> \x1f <funds> \x1f <stars> \x1f <today's revenue>
          \x1f <employees>
Weather (the home screen's Weather app: what the game's clock hover shows, weather_service / season_service):
  "wx"    \x1f <season (SeasonType) or -1> \x1f <temperature (weather_enums.Temperature) or ""> \x1f <raining 0/1>
          \x1f <snowing 0/1> \x1f <seasons pack 0/1>
  "fc"    \x1f <day from today> \x1f <icon> \x1f <name> \x1f <description>   (the forecast, today first)
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
_inventory = []  # the "inv" objects sent last, by index (object ids)
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
        found.append((max(abs(f), abs(r)) + (500 if target.household is sim_info.household else 0), target, f, r))
    # Household members with no relationship yet are contacts too (the Household filter shows them all).
    have = set(t.id for _, t, _, _ in found)
    for member in (sim_info.household.sim_info_gen() if sim_info.household is not None else ()):
        if member is not sim_info and member.id not in have:
            found.append((500, member, 0.0, 0.0))
    found.sort(key=lambda x: -x[0])
    _contacts[:] = [t.id for _, t, _, _ in found[:80]]
    tags_of = _contact_tagger(sim_info)
    for n, (_, target, f, r) in enumerate(found[:80]):
        def bit_of(track):
            try:
                t = tracker.get_relationship_track(target.id, track)
                bit = t.get_bit_for_client() if t is not None else None
                return _text_for(getattr(bit, 'display_name', None), sim_info, target) if bit is not None else ''
            except Exception:
                return ''
        has_romance = romance is not None and tracker.get_relationship_track(target.id, romance) is not None

        def family_bit():
            # The other bits the relationship panel shows (family: Husband, Daughter...), highest priority first:
            # household members often have no friendship bit to show (user 2026-10-24 02:41Z).
            try:
                track_bits = set()
                for track in tracker.relationship_tracks_gen(target.id):
                    try:
                        track_bits.add(track.get_bit_for_client())
                    except Exception:
                        pass
                shown = []
                for bit in tracker.get_all_bits(target.id):
                    if bit in track_bits or not getattr(bit, 'visible', False) or getattr(bit, 'invisible_filterable', False):
                        continue
                    shown.append(bit)
                shown.sort(key=lambda b: -(getattr(b, 'priority', 0) or 0))
                for bit in shown:
                    text = _text_for(getattr(bit, 'display_name', None), sim_info, target)
                    if text:
                        return text
            except Exception:
                pass
            return ''
        records.append('ct\x1f{}\x1f{}\x1f{}\x1f{:.0f}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
            n, '{} {}'.format(target.first_name, target.last_name).strip(), hudinfo._thumb('sims/f_{}_m_0'.format(target.id)),
            f, '{:.0f}'.format(r) if has_romance else '', bit_of(friendship), bit_of(romance) if has_romance else '',
            int(target.is_instanced()), family_bit(), tags_of(target, f, r)))


def _contact_tagger(sim_info):
    """The Contacts filters a Sim is in (user 2026-10-24 17:54Z), as letters: H household, F family, f friend,
    R romance, E rival, W coworker, S schoolmate, L on this lot, C same club, P pet."""
    import services
    from sims.sim_info_types import Age
    tracker = sim_info.relationship_tracker
    family = set()
    try:
        family = set(sim_info.genealogy.get_family_sim_ids_gen())
    except Exception as e:
        _log_error('the family', e)
    coworker_bit = None
    try:
        from careers.coworker import CoworkerMixin
        coworker_bit = CoworkerMixin.COWORKER_RELATIONSHIP_BIT
    except Exception:
        pass
    young = sim_info.age in (Age.CHILD, Age.TEEN)
    school = set()
    try:
        if young:
            school = set(c.guid64 for c in sim_info.career_tracker)
    except Exception:
        pass
    clubs = set()
    try:
        service = services.get_club_service()
        if service is not None:
            clubs = set(service.get_clubs_for_sim_info(sim_info))
    except Exception:
        pass
    lot = services.active_lot()

    def tags(target, f, r):
        out = ''
        if target.household is sim_info.household:
            out += 'H'
        if target.id in family:
            out += 'F'
        if f >= 30:
            out += 'f'
        if r >= 10 or (tracker.has_romantic_relationship(target.id) if hasattr(tracker, 'has_romantic_relationship') else False):
            out += 'R'
        if f <= -30:
            out += 'E'
        try:
            if coworker_bit is not None and tracker.has_bit(target.id, coworker_bit):
                out += 'S' if young else 'W'
            elif young and target.age in (Age.CHILD, Age.TEEN) and school & set(c.guid64 for c in target.career_tracker):
                out += 'S'
        except Exception:
            pass
        try:
            sim = target.get_sim_instance()
            if sim is not None and lot is not None and lot.is_position_on_lot(sim.position):
                out += 'L'
        except Exception:
            pass
        try:
            if clubs and clubs & set(services.get_club_service().get_clubs_for_sim_info(target)):
                out += 'C'
        except Exception:
            pass
        if getattr(target, 'is_pet', False):
            out += 'P'
        return out
    return tags


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
                                                      _text_for(getattr(trait, 'display_name', None), target, target)))
    _tooltip(sim_info, target, knowledge, records)
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
    try:
        from sims4vr import phoneapp
        for node, name, disabled in phoneapp.social:
            records.append('cda\x1f{}\x1f{}\x1f{}'.format(node, name, disabled))
    except Exception as e:
        _log_error('social entries', e)
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


def _clock2(t):
    hour, minute = int(t.hour()), int(t.minute())
    return '{}:{:02d} {}'.format((hour + 11) % 12 + 1, minute, 'AM' if hour < 12 else 'PM')


def _tooltip(sim_info, target, knowledge, records):
    """What the game's relationship tooltip shows (RelationshipTooltip.UpdateDataAndReposition): age, known careers
    with company, next work hours and work days, compatibility, attraction."""
    from sims4vr import hudinfo
    try:
        trait = target.get_age_transition_data(target.age).age_trait
        records.append('cdg\x1f{}\x1f{}'.format(hudinfo._icon(getattr(trait, 'icon', None)),
                                                 _text_for(getattr(trait, 'display_name', None), target, target)))
    except Exception as e:
        _log_error('the age', e)
    if knowledge is not None:
        try:
            known = list(knowledge.get_known_careers())
        except Exception:
            known = []
        for career in known:
            try:
                track = career.current_track_tuning
                name = _text(getattr(track, 'career_name', None), target)
                company = ''
                try:
                    company = _loc(career.get_company_name())
                except Exception:
                    pass
                hours, days = '', ['0'] * 7
                try:
                    scheduler = career._get_work_scheduler()
                    entries = list(scheduler.get_schedule_entries()) if scheduler is not None else []
                    for start, end in entries:
                        days[int(start.day()) % 7] = '1'
                    if entries:
                        _, start, end = career.get_next_work_time()
                        if start is not None and end is not None:
                            hours = '{} - {}'.format(_clock2(start), _clock2(end))
                except Exception:
                    pass
                records.append('cdj\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(hudinfo._icon(getattr(track, 'icon', None)), name,
                                                                       company, hours, ''.join(days)))
            except Exception as e:
                _log_error('a tooltip career', e)
    try:
        import services
        from relationships.compatibility_tuning import CompatibilityTuning
        level = services.relationship_service().get_compatibility_level(sim_info.sim_id, target.sim_id)
        data = CompatibilityTuning.COMPATIBILITY_LEVEL_ICONS_MAP.get(level) if level is not None else None
        if data is not None and data.icon is not None:
            records.append('cdc\x1f{}\x1f{}\x1f{}'.format(hudinfo._icon(data.icon), _loc(data.level_name), _loc(data.descriptive_text)))
    except Exception as e:
        _log_error('compatibility', e)
    try:
        from relationships.attraction_tuning import AttractionTuning
        track_type = AttractionTuning.ATTRACTION_RELATIONSHIP_TRACK
        track = sim_info.relationship_tracker.get_relationship_track(target.sim_id, track_type) if track_type is not None else None
        bit = track.get_active_bit() if track is not None else None
        icon = getattr(bit, 'small_icon', None) or getattr(bit, 'icon', None) if bit is not None else None
        if bit is not None and icon is not None:
            records.append('cdx\x1f{}\x1f{}\x1f{}'.format(hudinfo._icon(icon), _text_for(bit.display_name, sim_info, target),
                                                           _text_for(getattr(bit, 'bit_description', None), sim_info, target)))
    except Exception as e:
        _log_error('attraction', e)


_auto_pick = None  # [sim id, until (monotonic)]: the next Sim picker of yours picks this Sim


def contact_action(node):
    """A Social entry on a contact's page: the phone's own entry (phoneapp.select), its Sim picker answered with the contact."""
    global _auto_pick
    import time
    from sims4vr import phoneapp
    if _contact is None:
        return
    _auto_pick = [_contact, time.monotonic() + 20.0]
    native._log('phonesim: contact action {} for sim {}'.format(node, _contact))
    phoneapp.select(node)


def auto_pick_option(dialog):
    """objmenu's dialog_show: the option of the contact's row in this Sim picker, if a Contacts action waits for one."""
    global _auto_pick
    import time
    if _auto_pick is None:
        return None
    if time.monotonic() > _auto_pick[1]:
        _auto_pick = None
        return None
    rows = getattr(dialog, 'picker_rows', None)
    if not rows:
        return None
    target, _auto_pick = _auto_pick[0], None
    for row in rows:
        if getattr(row, 'sim_id', None) == target:
            return row.option_id
    native._log('phonesim: the picker has no row for sim {}, shown as it is'.format(target))
    return None


def _inventory_list(sim_info, records):
    from sims4vr import hudinfo
    from distributor.shared_messages import create_icon_info_msg
    sim = sim_info.get_sim_instance()
    _inventory.clear()
    inventory = getattr(sim, 'inventory_component', None) if sim is not None else None
    if inventory is None:
        return
    try:
        from sims4.localization import LocalizationHelperTuning
    except Exception:
        LocalizationHelperTuning = None
    for obj in list(inventory.visible_storage)[:120]:
        try:
            picture = hudinfo._picture(create_icon_info_msg(obj.get_icon_info_data()))
        except Exception:
            picture = 0
        name = ''
        try:
            name = _loc(LocalizationHelperTuning.get_object_name(obj)) if LocalizationHelperTuning else ''
        except Exception:
            pass
        favorite = 0
        try:
            tracker = sim_info.favorites_tracker
            favorite = int(bool(tracker is not None and tracker.is_favorite_stack(obj)))
        except Exception:
            pass
        records.append('inv\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
            len(_inventory), picture, name, obj.stack_count(), int(getattr(obj, 'current_value', 0) or 0), favorite,
            int(bool(getattr(obj, 'non_deletable_by_user', False)))))
        _inventory.append(obj.id)


_SIMOLOGY_TYPES = (0, 1, 5, 6, 10, 12, 25, 28, 29, 30)  # SimTraitsPanel's order: personality, gameplay, ghost, aspiration,
# phase, infection, tradition, pack friend, fear, high school (traits.trait_type.TraitType)
_SIMOLOGY_STYLE = {0: 0, 10: 2, 25: 3, 29: 4}  # the cell's highlight: personality, phase, tradition, fear; others reward


def _simology(sim_info, records):
    """Simology (SimInfoSummaryPanel): "sy" \x1f <group T/L/D/O/F> \x1f <cell style> \x1f <icon> \x1f <name>: the traits
    in SimTraitsPanel's order, then likes, dislikes, turn-ons, turn-offs."""
    from sims4vr import hudinfo
    try:
        from traits.attraction_preference import AttractionPreference
    except Exception:
        AttractionPreference = None
    traits = [t for t in sim_info.trait_tracker if getattr(t, 'display_name', None) is not None]
    order = {t: i for i, t in enumerate(_SIMOLOGY_TYPES)}

    def kind(t):
        return int(getattr(t, 'trait_type', -1))

    rows = []
    for t in sorted((t for t in traits if kind(t) in order), key=lambda t: order[kind(t)]):
        rows.append(('T', _SIMOLOGY_STYLE.get(kind(t), 1), t))
    for t in traits:
        if kind(t) not in (22, 23):
            continue
        romantic = AttractionPreference is not None and isinstance(t, type) and issubclass(t, AttractionPreference)
        rows.append((('O' if kind(t) == 22 else 'F') if romantic else ('L' if kind(t) == 22 else 'D'), 0, t))
    for group, style, t in rows:
        name = _text_for(t.display_name, sim_info, sim_info)
        if name:
            records.append('sy\x1f{}\x1f{}\x1f{}\x1f{}'.format(group, style, hudinfo._icon(getattr(t, 'icon', None)), name))


def _souls_journey(sim_info, records):
    """Soul's Journey (the aspiration panel's UnfinishedBusinessPanel, Life & Death): "sj" \x1f <ghost 0/1> \x1f <rank>
    \x1f <progress in the rank 0..1> \x1f <first name> \x1f <complete 0/1>, then "sjg" \x1f <goal text> \x1f <done> \x1f <value>
    \x1f <goal> \x1f <show progress> for the bucket list (the aspiration tracker's additional objectives)."""
    try:
        from aspirations.unfinished_business_aspiration_tuning import UnfinishedBusiness
    except Exception:
        return
    stat_type = UnfinishedBusiness.UNFINISHED_BUSINESS_STAT
    aspiration = UnfinishedBusiness.global_unfinished_business_aspiration
    if stat_type is None or aspiration is None:
        return
    stat = None
    try:
        stat = sim_info.get_statistic(stat_type, add=False)
    except Exception:
        try:
            stat = sim_info.commodity_tracker.get_statistic(stat_type)
        except Exception:
            stat = None
    rank, progress = 0, 0.0
    if stat is not None:
        try:
            rank = int(stat.rank_level)
            low, high = stat.points_to_rank(rank), stat.points_to_rank(rank + 1)
            progress = max(0.0, min(1.0, (stat.get_value() - low) / float(high - low))) if high > low else 0.0
        except Exception as e:
            _log_error('the soul rank', e)
    ghost = bool(getattr(sim_info, 'is_ghost', False))
    try:
        last = stat_type.max_rank
        last = int(last() if callable(last) else last)
    except Exception:
        last = 9
    complete = rank >= (last if ghost else last - 1)
    records.append('sj\x1f{}\x1f{}\x1f{:.3f}\x1f{}\x1f{}'.format(int(ghost), rank, progress, sim_info.first_name, int(complete)))
    _souls_tips(sim_info, stat_type, ghost, records)
    tracker = getattr(sim_info, 'aspiration_tracker', None)
    if tracker is None:
        return
    try:
        objectives = list(tracker.get_additional_objectives(aspiration))
    except Exception:
        objectives = []
    for objective in objectives:
        try:
            done = bool(tracker.objective_completed(objective))
            try:
                goal = float(objective.goal_value())
            except Exception:
                goal = 1.0
            value = tracker.get_last_updated_value_for_objective(objective) if hasattr(
                tracker, 'get_last_updated_value_for_objective') else None
            value = goal if done else float(value or 0)
            records.append('sjg\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                _text_for(getattr(objective, 'display_text', None), sim_info, int(goal)), int(done), int(value), int(goal),
                int(bool(getattr(objective, 'show_progress', True))),
                _text_for(getattr(objective, 'tooltip', None), sim_info, int(goal))))
        except Exception as e:
            _log_error('a bucket list goal', e)


# The panel's tooltips (UnfinishedBusinessPanel / RankedStatUnfinishedBusinessMeter, KeysGP).
_SJ_GHOST_LOCK = 3679656623
_SJ_HELP, _SJ_HELP_EP05, _SJ_HELP_EP13, _SJ_GHOST_HELP = 3028011275, 1610422057, 1627199644, 701526614


def _souls_tips(sim_info, stat_type, ghost, records):
    """"sjt" \x1f <which> \x1f <text>: 0-7 the pips' (each rank's first level's event tooltip, as RankedStatisticData.GetRank),
    8 the ninth pip's ghost lock, 9-11 the help button's parts (living or ghost, then Seasons' and Growing Together's)."""
    from sims4.localization import _create_localized_string
    try:
        levels = len(stat_type.get_level_list())
        rank, seen = 0, set()
        for level in range(levels):
            data = stat_type.event_data.get(level)
            if data is not None and data.rank_up:
                rank += 1
            if rank in seen or rank > 7:
                continue
            seen.add(rank)
            tip = getattr(data, 'tooltip', None) if data is not None else None
            text = _text_for(tip, sim_info, sim_info)
            if text:
                records.append('sjt\x1f{}\x1f{}'.format(rank, text))
    except Exception as e:
        _log_error('the soul ranks\' tooltips', e)
    parts = [(8, _SJ_GHOST_LOCK), (9, _SJ_GHOST_HELP if ghost else _SJ_HELP)]
    try:
        from sims4.common import Pack, is_available_pack
        if is_available_pack(Pack.EP05):
            parts.append((10, _SJ_HELP_EP05))
        if is_available_pack(Pack.EP13):
            parts.append((11, _SJ_HELP_EP13))
    except Exception as e:
        _log_error('the packs', e)
    for which, key in parts:
        try:
            records.append('sjt\x1f{}\x1f{}'.format(which, _loc(_create_localized_string(key, sim_info))))
        except Exception as e:
            _log_error('a Soul\'s Journey tooltip', e)


def _clubs(sim_info, records):
    import services
    from sims4vr import hudinfo
    service = services.get_club_service()
    if service is None:
        return
    gathering = getattr(service, 'clubs_to_gatherings_map', {}) or {}
    for n, club in enumerate(sorted(service.get_clubs_for_sim_info(sim_info), key=lambda c: c.club_id)[:10]):
        leader = club.leader
        records.append('club\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
            n, hudinfo._icon(club.icon), _loc(club.name), _loc(club.description) if club.description is not None else '',
            '{} {}'.format(leader.first_name, leader.last_name) if leader is not None else '', int(club in gathering)))
        for member in list(club.members)[:12]:
            records.append('cmem\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                n, hudinfo._thumb('sims/f_{}_m_0'.format(member.id)), member.first_name, int(member is leader)))
        for rule in list(club.rules)[:12]:
            try:
                encouraged = rule.is_encouraged() if callable(rule.is_encouraged) else rule.is_encouraged
                records.append('crule\x1f{}\x1f{}\x1f{}'.format(n, int(bool(encouraged)), _loc(rule.action.name)))
            except Exception:
                continue


_BUSINESS_TYPES = {1: 'Retail store', 2: 'Restaurant', 3: 'Vet clinic', 4: 'Rental', 5: 'Small business'}


def _businesses(sim_info, records):
    import services
    service = services.business_service()
    household = sim_info.household
    if service is None or household is None:
        return
    managers = service.get_business_managers_for_household(household.id) or {}
    seen = set()
    for key, manager in managers.items():
        if id(manager) in seen:
            continue
        seen.add(id(manager))
        name = ''
        zone_id = getattr(manager, 'business_zone_id', None)
        try:
            if zone_id:
                proto = services.get_persistence_service().get_zone_proto_buff(zone_id)
                name = proto.name if proto is not None else ''
        except Exception:
            pass
        if not name:
            owner = services.sim_info_manager().get(key)
            name = "{}'s business".format(owner.first_name) if owner is not None else ''
        kind = int(getattr(manager, 'business_type', 0) or 0)
        def num(attr, call=False):
            try:
                v = getattr(manager, attr)
                v = v() if call else v
                return v
            except Exception:
                return 0
        funds = num('funds')
        money = getattr(funds, 'money', 0) if funds else 0
        records.append('biz\x1f{}\x1f{}\x1f{}\x1f{}\x1f{:.1f}\x1f{}\x1f{}'.format(
            kind, name or _BUSINESS_TYPES.get(kind, 'Business'), int(bool(num('is_open'))), int(money or 0),
            float(num('get_star_rating', True) or 0), int(num('daily_revenue') or 0), int(num('employee_count') or 0)))


def _weather(sim_info, records):
    """The Weather app = the game's weather and season tooltip (WeatherAndSeasonTooltip.as): the season and its
    day, the current weather and temperature (WeatherService.WEATHER_TYPE_UI_INFO by group, as GetWeather /
    GetTemperature), today's forecast, the lunar phase (and its effect text), the street's eco footprint."""
    import services
    from sims4vr import hudinfo
    season = day = length = -1
    try:
        seasons = services.season_service()
        if seasons is not None and seasons.season is not None:
            season = int(seasons.season)
            content = seasons.season_content
            now = services.time_service().sim_now
            day = int((now - content.start_time).in_days()) + 1
            length = int(round(content.length.in_days()))
    except Exception as e:
        _log_error('the season', e)
    weather = ['', '', '', '', '', '']  # icon, name: weather, temperature, forecast
    enabled = 0
    service = services.weather_service()
    if service is not None:
        enabled = 1
        try:
            from weather.weather_enums import WeatherTypeGroup
            info = type(service).WEATHER_TYPE_UI_INFO
            for weather_type in list(service._current_weather_types or ()):
                entry = info.get(weather_type)
                if entry is None:
                    continue
                slot = 0 if entry.weather_group_type == WeatherTypeGroup.WEATHER else 2 if entry.weather_group_type == WeatherTypeGroup.TEMPERATURE else -1
                if slot < 0 or weather[slot]:
                    continue
                shown = entry.display_information
                weather[slot] = hudinfo._icon(shown.icon_large or shown.icon)
                weather[slot + 1] = _loc(shown.name)
        except Exception as e:
            _log_error('the weather now', e)
        try:
            forecasts = list(service._forecasts or ())
            if forecasts and forecasts[0] is not None:
                f = forecasts[0]
                weather[4] = hudinfo._icon(getattr(f, 'calendar_icon_large', None) or getattr(f, 'calendar_icon', None))
                weather[5] = _loc(f.forecast_name)
        except Exception as e:
            _log_error('the forecast', e)
    phase, effect = -1, ''
    try:
        lunar = services.lunar_cycle_service()
        if lunar is not None and lunar.current_phase is not None:
            phase = int(lunar.current_phase)
            tracker = getattr(sim_info, 'lunar_effect_tracker', None)
            tip = tracker.find_active_effect_tooltip() if tracker is not None else None
            effect = _loc(tip) if tip is not None else ''
    except Exception as e:
        _log_error('the lunar phase', e)
    eco = [-1, 0.5, 0, '', '']  # state, progress in it, delta, name, description
    try:
        street_service = services.street_service()
        if street_service is not None:
            provider = street_service.get_provider(services.current_street())
            if provider is not None and getattr(provider, '_is_eco_footprint_compatible', False):
                from eco_footprint.eco_footprint_tuning import EcoFootprintTunables
                from eco_footprint.eco_footprint_enums import EcoFootprintStateType
                stat = provider.get_street_footprint()
                value = stat.get_value()
                state = provider.current_eco_footprint_state
                data = EcoFootprintTunables.ECO_FOOTPRINT_STATE_DATA
                if state == EcoFootprintStateType.GREEN:
                    lo, hi = stat.min_value, data.green_threshold
                elif state == EcoFootprintStateType.NEUTRAL:
                    lo, hi = data.green_threshold, data.industrial_threshold
                else:
                    lo, hi = data.industrial_threshold, stat.max_value
                ui = EcoFootprintTunables.ECO_FOOTPRINT_UI_TUNABLES.get(state)
                eco = [int(state), (value - lo) / (hi - lo) if hi != lo else 0.5, stat.convergence_value - value,
                       _loc(ui.name_text) if ui is not None else '', _loc(ui.HUD_tooltip_description_text) if ui is not None else '']
    except Exception as e:
        _log_error('the eco footprint', e)
    records.append('wx2\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{:.3f}\x1f{:.3f}\x1f{}\x1f{}\x1f{}'.format(
        season, day, length, *weather, phase, effect, eco[0], eco[1], eco[2], eco[3], eco[4], enabled))


def inventory_pie(index):
    import services
    from sims4vr import pie
    if 0 <= index < len(_inventory):
        pie.open_on(services.object_manager().get(_inventory[index]) or services.inventory_manager().get(_inventory[index]))


_marked = []  # inventory rows picked on the phone's multiselect (control 7800000 + row), then sold or favorited


def inventory_mark(index):
    if 0 <= index < len(_inventory) and _inventory[index] not in _marked:
        _marked.append(_inventory[index])


def inventory_marked(what):
    """The phone's multiselect buttons (SimInfoInventoryPanelMain): 0 sells the picked stacks (as
    inventory.sim_inventory_sell_multiple does: value x count x your sell modifier, then destroyed), 1 / 2 adds
    or removes them as favorites (inventory.sim_inventory_favorite_multiple)."""
    import services
    picked, _marked[:] = list(_marked), []
    sim_info = services.active_sim_info()
    sim = sim_info.get_sim_instance() if sim_info is not None else None
    inventory = getattr(sim, 'inventory_component', None) if sim is not None else None
    if inventory is None:
        return
    manager = services.inventory_manager()
    items = [manager.get(i) for i in picked]
    items = [i for i in items if i is not None and i in inventory]
    if what == 0:
        from protocolbuffers import Consts_pb2
        value, sold, tags = 0, [], set()
        for item in items:
            if item.non_deletable_by_user:
                continue
            value += item.current_value * item.stack_count() * sim_info.get_item_sell_price_modifier(item)
            sold.append(item)
            tags.update(item.get_tags())
        if sold:
            services.active_household().funds.add(round(value), Consts_pb2.TELEMETRY_OBJECT_SELL, sim)
            services.get_reset_and_delete_service().trigger_batch_destroy(sold)
        native._log('phonesim: sold {} stacks for {}'.format(len(sold), round(value)))
    else:
        tracker = sim_info.favorites_tracker
        if tracker is None:
            return
        for item in items:
            if what == 1:
                tracker.set_favorite_stack(item)
            else:
                tracker.unset_favorite_stack(item)
            inventory.push_inventory_item_stack_update_msg(item)
    send(fresh=True)


def select_contact(index):
    """Contacts: the page of contact n (its details are sent with the list)."""
    global _contact
    _contact = _contacts[index] if 0 <= index < len(_contacts) else None
    send(fresh=True, contacts=True)


def contact_pie(index):
    """Contacts: the portrait on a contact's page pressed (user 2026-10-24 17:59Z): their pie menu, as a click
    on them asks, so a first person Sim can interact from afar (reach.py brings them over for a social)."""
    import services
    from sims4vr import pie
    target = services.sim_info_manager().get(_contacts[index]) if 0 <= index < len(_contacts) else None
    sim = target.get_sim_instance() if target is not None else None
    if sim is None:
        native._log('phonesim: contact {} is not here, no pie menu'.format(index))
        return
    pie.open_on(sim, pick_sim=True)


def contact_play_as(index):
    """Contacts: Play as on a household member's page (as the Household app's)."""
    global _switch
    import time
    import services
    if _switch is not None or not 0 <= index < len(_contacts):
        return
    target = services.sim_info_manager().get(_contacts[index])
    active = services.active_sim_info()
    if target is None or active is None or target.household is not active.household:
        return
    _switch = [target.id, time.monotonic(), False]
    native.set_eyes_held(True)


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
            funds = ''
            try:
                funds = int(sim_info.household.funds.money)
            except Exception:
                pass
            records.append('sim\x1f{}\x1f{} {}\x1f{}'.format(int(sim_info.age), sim_info.first_name, sim_info.last_name, funds))
            skills = _skills(sim_info)
            for category in sorted({s[0] for s in skills}, reverse=True):
                title = _CATEGORY_TITLES[category] if 0 <= category < len(_CATEGORY_TITLES) else _CATEGORY_TITLES[0]
                records.append('cat\x1f{}\x1f{}'.format(category, _key(title)))
            for s in skills:
                records.append('skill\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{:.4f}\x1f{}\x1f{}'.format(*s))
            for part in (_careers, _mood, _members, _inventory_list, _simology, _souls_journey, _clubs, _businesses, _weather) + ((_contact_list, _contact_page) if contacts else ()):
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
