"""The game's calendar as an app on the first-person phone (user 2026-10-18
22:04Z), drawn by the DLL (hud.cpp drawCalendar) from what is gathered here
the way the game's Calendar widget gathers it (CalendarDataFeed.as): the
weeks of a year (4 a season's length; 4 without Seasons) from the first day
of this week; per day its season, weather forecast (weather_service, the
game's 7 days), moon phase (lunar_cycle_service), the calendar entries
(calendar_service: holidays, events, festivals, weddings, vacations...), the
household's birthdays (days until ready to age, aging on) and work shifts
(each career's work scheduler).

To the DLL (native.phone_calendar), records split by 0x1e, fields by 0x1f:
  "cal"  \x1f <title json> \x1f <today: index of today> \x1f <days> \x1f <empty-day json> \x1f <all-day json>
  "day"  \x1f <index> \x1f <season 0 summer..3 spring, -1> \x1f <weather icon> \x1f <moon 0..7, -1>
         \x1f <main kind> \x1f <main picture> \x1f <small kinds, comma separated> \x1f <forecast name json>
  "item" \x1f <index> \x1f <kind> \x1f <picture> \x1f <name json> \x1f <time text>
Kinds: CalendarEntry types (1 event, 2/3 holiday, 5 vacation, 6 festival,
7 wedding, 8 work event, 9 stayover, 10 funeral, 11 getaway), 20 birthday,
21 a work shift. Control 81 = the calendar was opened (send it).
"""
import math
from sims4vr import native

_TITLE = 2199317903  # KeysGP.CALENDAR_TITLE ("The {0.String} Calendar")
_EMPTY = 1572021010  # KeysGP.CALENDAR_SCHEDULE_EMPTY_DAY
_ALL_DAY = 1242913989  # KeysGP.CALENDAR_SCHEDULE_ENTRY_TIME_ALL_DAY
_MULTI_DAY = (5, 9, 11)  # CalendarEntry.MultiDay: vacation, stayover, getaway
_BIRTHDAY, _WORK = 20, 21
_errors = 0


def _log_error(where, e):
    global _errors
    _errors += 1
    if _errors <= 10:
        native._log('phonecal: {} failed: {!r}'.format(where, e))


def _loc(value):
    from sims4vr import objmenu
    return objmenu._loc(value) if value is not None else ''


def _key(string_id, *tokens):
    from sims4.localization import _create_localized_string
    return _loc(_create_localized_string(string_id, *tokens))


def _raw(text):
    from sims4.localization import LocalizationHelperTuning
    return _loc(LocalizationHelperTuning.get_raw_text(text))


def _clock(t):
    hour, minute = int(t.hour()), int(t.minute())
    return '{}:{:02d} {}'.format((hour + 11) % 12 + 1, minute, 'AM' if hour < 12 else 'PM')


def _time_text(start, end):
    """CalendarUtils.GetEntryTimeText: start, start - end, or all day."""
    from date_and_time import DateAndTime
    if not start:
        return ''
    s = DateAndTime(start)
    if not end:
        return _clock(s)
    e = DateAndTime(end)
    if int(e.absolute_days()) == int(s.absolute_days()) + 1 and e.hour() + 24 - s.hour() >= 23:
        return None  # all day
    return '{} - {}'.format(_clock(s), _clock(e))


def _days_shown():
    """Refresh: weeks in the year (SeasonTypes.GetWeeksInYear) or 4, as days."""
    import services
    weeks = 4
    try:
        seasons = services.season_service()
        if seasons is not None:
            weeks = max(1, int(math.ceil(seasons.season_length.in_days() * 4 / 7)))
    except Exception as e:
        _log_error('the season length', e)
    return weeks * 7


def _seasons(days, start):
    """PopulateSeasons: each day's season from today on."""
    import services
    from date_and_time import create_time_span
    out = {}
    seasons = services.season_service()
    if seasons is None:
        return out
    now = services.time_service().sim_now
    one = create_time_span(days=1)
    t = now
    gen = seasons.get_seasons_gen()
    season, content = next(gen)
    for i in range(start, days):
        for _ in range(8):
            if t in content:
                break
            season, content = next(gen)
        out[i] = int(season)
        t = t + one
    return out


def send():
    if native._optional('phone_calendar') is None:
        return
    try:
        records = _build()
    except Exception as e:
        _log_error('building the calendar', e)
        return
    native.phone_calendar('\x1e'.join(records).encode('utf-8'))


def _build():
    import services
    from sims4vr import hudinfo
    now = services.time_service().sim_now
    today_abs = int(now.absolute_days())
    today = int(now.day()) % 7  # the first column is the week's first day
    count = _days_shown()
    household = services.active_household()
    family = getattr(household, 'name', '') or ''
    days = {}  # index -> dict

    def day(i):
        return days.setdefault(i, {'season': -1, 'weather': 0, 'moon': -1, 'entries': [], 'forecast': ''})

    items = []
    for i, season in _seasons(count, today).items():
        day(i)['season'] = season
    weather = services.weather_service()
    if weather is not None:
        try:
            weather.populate_forecasts(7)
            for k, forecast in enumerate(list(weather._forecasts)[:7]):
                if forecast is not None and today + k < count:
                    d = day(today + k)
                    d['weather'] = hudinfo._icon(forecast.calendar_icon)
                    d['forecast'] = _loc(forecast.forecast_name)
        except Exception as e:
            _log_error('the weather forecast', e)
    lunar = getattr(services, 'lunar_cycle_service', None)
    lunar = lunar() if lunar is not None else None
    if lunar is not None:
        try:
            lunar.populate_forecasts(count - today)
            for abs_day, phase in dict(lunar._forecasts).items():
                i = today + int(abs_day) - today_abs
                if today <= i < count:
                    day(i)['moon'] = int(phase)
        except Exception as e:
            _log_error('the moon forecast', e)
    # The calendar's entries (CalendarGetEntries: what calendar_service marked).
    calendar = services.calendar_service() if hasattr(services, 'calendar_service') else None
    if calendar is not None:
        from drama_scheduler.drama_node import DramaNodeUiDisplayType
        for ref in list(calendar._event_data_map.values()):
            node = ref()
            if node is None or node.ui_display_type == DramaNodeUiDisplayType.ALERTS_ONLY:
                continue
            try:
                entry = node.create_calendar_entry()
            except Exception as e:
                _log_error('an entry', e)
                continue
            from date_and_time import DateAndTime
            kind = int(entry.entry_type)
            start = int(DateAndTime(entry.start_time).absolute_days()) - today_abs if entry.start_time else 0
            end = int(DateAndTime(entry.end_time).absolute_days()) - today_abs if entry.end_time else start
            name = _loc(entry.icon_info.name) if entry.icon_info.HasField('name') else ''
            picture = hudinfo._icon(entry.icon_info.icon) if entry.icon_info.HasField('icon') else 0
            span = range(start, end + 1) if kind in _MULTI_DAY else (start,)
            text = _time_text(entry.start_time, entry.end_time)
            for k in span:
                i = today + k
                if k < 0 or i >= count:
                    continue
                day(i)['entries'].append((kind, picture))
                items.append((i, kind, picture, name, _key(_ALL_DAY) if text is None else _raw(text) if text else ''))
    # Birthdays (PopulateBirthdays) and work shifts (PopulateWorkSchedules).
    aging = services.get_aging_service() if hasattr(services, 'get_aging_service') else None
    from sims.sim_info_types import Age, Species
    for sim_info in (list(household) if household is not None else ()):
        name = '{} {}'.format(sim_info.first_name, sim_info.last_name).strip()
        try:
            if (sim_info.age != Age.ELDER and sim_info.species == Species.HUMAN
                    and (aging is None or aging.is_aging_enabled_for_sim_info(sim_info))):
                k = int(now.absolute_days() + sim_info.days_until_ready_to_age()) - today_abs
                if 0 <= k and today + k < count:
                    bust = hudinfo._sim_bust(sim_info.sim_id)
                    day(today + k)['entries'].append((_BIRTHDAY, bust))
                    items.append((today + k, _BIRTHDAY, bust, _raw(name), ''))
        except Exception as e:
            _log_error('a birthday', e)
        careers = getattr(sim_info, 'careers', None) or {}
        for career in list(careers.values()):
            try:
                scheduler = career._get_work_scheduler()
                if scheduler is None:
                    continue
                try:
                    title = _loc(career.current_level_tuning.get_title(sim_info))
                except Exception:
                    title = _raw(name)
                bust = hudinfo._sim_bust(sim_info.sim_id)
                for start, end in scheduler.get_schedule_entries():
                    weekday = int(start.day()) % 7
                    length = end - start
                    if length.in_ticks() < 0:  # wraps the week's end
                        from date_and_time import create_time_span
                        length = length + create_time_span(days=7)
                    finish = start + length
                    text = '{} - {}'.format(_clock(start), _clock(finish))
                    for i in range(weekday, count, 7):
                        if i >= today:
                            day(i)['entries'].append((_WORK, bust))
                            items.append((i, _WORK, bust, title, _raw(text)))
            except Exception as e:
                _log_error('a work schedule', e)
    records = ['cal\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
        _key(_TITLE, family), today, count, _key(_EMPTY), _key(_ALL_DAY))]
    for i in sorted(days):
        d = days[i]
        main, picture, small = _main(d['entries'])
        records.append('day\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
            i, d['season'], d['weather'], d['moon'], main, picture, ','.join(str(k) for k in small), d['forecast']))
    for i, kind, picture, name, text in items:
        records.append('item\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(i, kind, picture, name, text))
    native._log('phonecal: {} days, {} entries'.format(count, len(items)))
    return records


def _main(entries):
    """CalendarDayView.SetupMainEntry and its small icons (3 at most, the last
    a "more" when there are more)."""
    kinds = [k for k, _ in entries]
    main, picture = 0, 0
    for k in (7, 10, 11, 3, 2, 1, 8, 6):
        if k in kinds:
            main, picture = k, entries[kinds.index(k)][1]
            break
    if not main and _BIRTHDAY in kinds:
        main, picture = _BIRTHDAY, entries[kinds.index(_BIRTHDAY)][1]
    small = []
    for k in (1, 6, 9, 5, 11, _WORK, _BIRTHDAY):
        if k != main and k in kinds and not (k == 1 and main == 1):
            small.append(k)
    if len(small) > 3:
        small = small[:2] + [0]  # 0: more
    return main, picture, small
