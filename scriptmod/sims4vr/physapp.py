"""The phone's Body Physics app (user 2026-10-28 13:50Z): pick a Sim by looking at or touching her, freeze her
motor functions and hold her in a pose while you tune her physics from the phone.

The physics themselves stay in the separate Integrated Body Physics mod. This module only drives its Python
side when that mod is loaded (integratedbodyphysics.pose.freeze / release / hold, the same calls as its pie
menu's Freeze all motor functions) and the DLL reads/writes its settings through ibp_get/ibp_set and
ibp_menu (hud.cpp, the Phone Options page renderer). Nothing of this is inside the physics mod.

Picking (the app's "Pick a Sim" arms it; it disarms after PICK_TIMEOUT s or when she is picked):
- touch, grab or poke her with a hand (bodygrab.tick hands every contact to pick_touch; no reaction then),
- or look at her: the Sim nearest the middle of your look (within DWELL_ANGLE degrees, up to DWELL_RANGE m)
  for DWELL_SECONDS. The app's "Pick the Sim I look at" button does the same at once.
Only women of an adult age are eligible (what the physics mod works on); others are refused with a log line.

Poses: the physics mod's own (Neutral, Dance, Stretch) and, when WickedWhims is loaded (a module named
wickedwhims in sys.modules), the pose interactions found in the game's interaction manager by name
([bodyphysics] ww_pose_words in sims4vr.ini, default "pose"; a class whose module or name has "wickedwhims"
and one of the words). Names are matched only, nothing of WickedWhims is read or copied. Log lines:
"physapp: ..." (sims4vr_python.log), including how many WW poses were found.

To the DLL (appended to phonesim.send, fields split by 0x1f):
  "bp"  \x1f <physics mod loaded 0/1> \x1f <armed 0/1> \x1f <picked Sim's name or ""> \x1f <frozen 0/1>
        \x1f <WickedWhims 0/1> \x1f <status text>
  "bpp" \x1f <index> \x1f <name> \x1f <held now 0/1> \x1f <0 physics mod, 1 WickedWhims>
  "bpo" \x1f <index> \x1f <label> \x1f <wearing now 0/1>     (her outfits, the pie menu's list)
Controls (8400000 + n): 0 arm / disarm, 1 let her go, 2 pick the Sim I look at, 10 previous outfit, 11 next outfit,
12 open WickedWhims' pose menu, 13 freeze her again after WickedWhims, 100 + i pose i, 200 + i outfit i.

WickedWhims (user 2026-10-28 17:50Z): its pose entries are one "WickedWhims" button that opens its own pose menu.
Cause of the endless reopening: the pose was HELD like the physics mod's own poses, and the physics mod pushes a held
interaction again every 2 s when it is not running (a menu interaction ends at once). Now the WickedWhims menu is pushed
once, and the physics mod's hold on her is dropped while WickedWhims runs her (its queue gate refuses everything else
too); "Freeze her again" (control 13) holds her in the neutral pose again.
"""
import math
import sys
import time

from sims4vr import native

PICK_TIMEOUT = 60.0  # s armed without a pick
DWELL_SECONDS = 1.2
DWELL_ANGLE = 8.0  # degrees off the middle of your look
DWELL_RANGE = 15.0  # m
LOOK_ANGLE = 15.0  # degrees: "Pick the Sim I look at"
MAX_WW_POSES = 60

_armed = 0.0  # time.monotonic() it was armed, 0 = not armed
_dwell = None  # (sim id, since)
_status = ''
_poses = []  # (name, ids tuple, source 0 game pose / 1 WickedWhims / 2 looping clip)
_poses_for = None  # the frozen Sim id the list was built for
_ww_free = 0  # the Sim id whose freeze hold was dropped for WickedWhims' menu
_ww_logged = False
_thawed = {}  # sim id -> [kind 'act'/'follow', since, idle since or None, the held affordance]
_resume_at = 0.0
_guard_at = 0.0
_guard_busy = None  # when a foreign action on the frozen Sim was first seen
_guard_last = 0.0
_skip_token = None
_frozen_id = 0  # the Sim the guard saw frozen last
_skipped = set()  # Sim ids with autonomy skipped by us
THAW_MIN = 3.0  # s a gesture's action gets before she may be frozen again
IDLE_FOR = 1.5  # s she must be idle for
_logs = 0
_errors = 0


def _number(key, default):
    try:
        return float(native.setting('bodyphysics', key, str(default)) or default)
    except ValueError:
        return default


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 200:
        native._log('physapp: ' + message)
        fn = native._optional('dll_log')  # also in sims4vr.log, which is the file that gets sent
        if fn is not None:
            try:
                fn('physapp: ' + message)
            except Exception:
                pass


def _error(where, e):
    global _errors
    _errors += 1
    if _errors <= 8:
        native._log('physapp: {} failed: {!r}'.format(where, e))


def _ibp():
    """The Integrated Body Physics script package, or None when it is not loaded."""
    try:
        import integratedbodyphysics
        from integratedbodyphysics import pose
        return pose if integratedbodyphysics is not None else None
    except Exception:
        return None


def _wickedwhims():
    return any(name.lower().startswith('wickedwhims') for name in list(sys.modules))


_grace = [None]  # (sim id, time) set when the app switches her pose


def _frozen_sim():
    """The Sim the physics mod holds frozen (hold + current Sim), or None."""
    pose = _ibp()
    if pose is None:
        return None
    import services
    sim_id = getattr(pose, '_current', 0)
    if not sim_id:
        # a pose switch resets her and the physics mod lets go of her for a moment: she stays picked meanwhile
        g = _grace[0]
        if g is None or time.monotonic() - g[1] > 3.0:
            return None
        sim_id = g[0]
    else:
        _grace[0] = (sim_id, time.monotonic()) if _grace[0] is not None and _grace[0][0] == sim_id else _grace[0]
    info = services.sim_info_manager().get(sim_id)
    return info.get_sim_instance() if info is not None else None


def _ww_candidates():
    """WickedWhims interactions whose name has a pose word, best guess for its pose menu first."""
    words = [w.strip().lower() for w in (native.setting('bodyphysics', 'ww_pose_words', 'pose') or 'pose').split(',') if w.strip()]
    import services
    import sims4.resources
    out = []
    try:
        manager = services.get_instance_manager(sims4.resources.Types.INTERACTION)
        for key, cls in manager.types.items():
            name = getattr(cls, '__name__', '') or ''
            module = (getattr(cls, '__module__', '') or '').lower()
            low = name.lower()
            if ('wickedwhims' in low or 'wickedwhims' in module) and any(w in low for w in words):
                score = sum(w in low for w in ('menu', 'picker', 'select', 'open', 'player', 'poses'))
                if low.endswith('affsuper_pose'):
                    score += 20  # the one that opens WickedWhims' pose menu (logs 2026-10-28: the others were refused or are mixers)
                if any(bad in low for bad in ('stop', 'takepicture', 'mixer')):
                    score -= 20
                out.append((-score, name, key.instance))
    except Exception as e:
        _error('listing the WickedWhims poses', e)
    out.sort()
    return [(name, instance) for _, name, instance in out]


def _ww_menu_target():
    global _ww_logged
    found = _ww_candidates()
    if not _ww_logged:
        _ww_logged = True
        _log('WickedWhims pose interactions ({}): {}'.format(len(found), ', '.join(n for n, _ in found[:40])))
    want = (native.setting('bodyphysics', 'ww_pose_interaction', '') or '').strip().lower()
    for name, instance in found:
        if want and name.lower() == want:
            return name, instance
    return found[0] if found and not want else (None, None)


def _open_ww_menu(sim):
    """Pushes WickedWhims' pose menu once and lets go of the physics mod's hold (see the header)."""
    global _ww_free
    pose = _ibp()
    name, instance = _ww_menu_target()
    if pose is None or name is None:
        global _status
        _status = 'WickedWhims pose menu not found (see physapp in the log)'
        return
    aff = pose.affordance((instance,))
    if aff is None:
        return
    sim_id = sim.sim_info.sim_id
    pose._held.pop(sim_id, None)
    pose._since.pop(sim_id, None)
    _ww_free = sim_id
    try:
        from sims4vr import follow
        if follow.is_follower(sim):  # follow.py refuses every action she is given, WickedWhims' pose animation too (the 713 log:
            follow.dismiss(sim, 'WickedWhims pose')  # \"refused (they follow you)\" over and over, and she kept respawning)
    except Exception as e:
        _error('dismissing the follow', e)
    pose._stop_running(sim, aff)  # her neutral pose ends first: WickedWhims' pose needs her free (a leftover exclusive stand fought it)
    result = pose._push(sim, aff, False)
    _log('{}: WickedWhims pose menu {} -> {}'.format(_name(sim), name, result))


def _build_poses(sim):
    global _poses, _poses_for
    pose = _ibp()
    poses = []
    if pose is not None:
        for name, ids in pose.POSES:
            poses.append((name, tuple(ids), 0))
        for name, clip_id in getattr(pose, 'CLIPS', ()):  # looping Jump / Jog in place (Fitness Stuff clips)
            try:
                if pose.clip_available(clip_id):
                    poses.append((name, (clip_id,), 2))
            except Exception as e:
                _error('clip check', e)
        if _wickedwhims():
            poses.append(('WickedWhims', (), 1))
    _poses = poses
    _poses_for = sim.sim_info.sim_id if sim is not None else None


def _eligible(sim):
    from integratedbodyphysics import pie
    return pie._eligible(sim)


def _name(sim):
    return '{} {}'.format(sim.sim_info.first_name, sim.sim_info.last_name).strip()


def _refresh():
    from sims4vr import phonesim
    phonesim.send(fresh=True)


def pick(sim, how):
    """Freezes `sim` (the physics mod's Freeze all motor functions) and makes her the current Sim."""
    global _armed, _status, _dwell
    pose = _ibp()
    global _ww_free
    _dwell = None
    _ww_free = 0
    if pose is None:
        _status = 'Integrated Body Physics is not loaded'
        return False
    try:
        if not _eligible(sim):
            _status = '{} is not an adult woman: the physics mod works on those'.format(sim.sim_info.first_name)
            _log('{} picked by {} refused'.format(_name(sim), how))
            return False
        pose.freeze(sim)
    except Exception as e:
        _error('freezing', e)
        _status = 'Could not freeze her'
        return False
    _armed = 0.0
    _status = ''
    _build_poses(sim)
    _log('{} picked by {} and frozen'.format(_name(sim), how))
    if native.setting_flag('physical', 'haptics', True):
        native.haptic(1, 0.5, 0.12)
    _refresh()
    return True


def pick_touch(you, npc):
    """bodygrab.tick: a hand touched / grabbed / poked `npc`. True when it was taken as the pick (no reaction)."""
    if not _armed:
        return False
    pick(npc, 'touch')
    return True


def _look_target(you, angle, far):
    snap = native.hands()
    if snap is None:
        return None
    (hx, _, hz), (fx, _, fz) = snap[0], snap[1]
    import services
    best, best_angle = None, angle
    for sim in services.sim_info_manager().instanced_sims_gen():
        if sim is you or not sim.sim_info.is_human:
            continue
        q = sim.position
        dx, dz = q.x - hx, q.z - hz
        d = math.hypot(dx, dz)
        if d > far or d < 0.2:
            continue
        a = math.degrees(math.acos(max(-1.0, min(1.0, (dx * fx + dz * fz) / (d * math.hypot(fx, fz) or 1.0)))))
        if a < best_angle:
            best, best_angle = sim, a
    return best


def _you():
    import services
    from sims4vr import body
    return services.object_manager().get(body.following())


def _outfits(sim):
    """[((category, index), label, wearing now)] of the outfits she has, in the pie menu's order."""
    from integratedbodyphysics import pie
    from sims.outfits.outfit_enums import OutfitCategory
    info = sim.sim_info
    now = info.get_current_outfit()
    out = []
    for value, name in pie.OUTFITS:
        for index in range(pie.OUTFITS_PER_CATEGORY):
            if not info.has_outfit((OutfitCategory(value), index)) and not (value == 5 and not index):
                continue  # Nude (the bathing outfit) is always listed; wearing it makes one if she has none
            label = 'Nude' if value == 5 and not index else '{} {}'.format(name, index + 1)
            out.append(((value, index), label, int(now[0]) == value and now[1] == index))
    out.sort(key=lambda o: o[0] != (5, 0))  # Nude first: the list scrolls, and she may have many outfits
    return out


def _spin(sim, outfit):
    """The classic outfit-change spin (the game's own change-outfit animation, outfit_tracker.get_change_outfit_element:
    the same one the pie's Change Outfit interaction plays), run on the Sim's timeline. False: fall back to a pop-in."""
    try:
        import elements
        import services
        from sims.outfits.outfit_enums import OutfitCategory
        info = sim.sim_info
        target = (OutfitCategory(outfit[0]), outfit[1])
        if info.get_current_outfit() == target:
            return True
        element = elements.GeneratorElement(info.get_change_outfit_element(target, do_spin=True))
        services.time_service().sim_timeline.schedule(element)
        _log('{}: outfit {} with the change spin'.format(_name(sim), target))
        return True
    except Exception as e:
        _error('the outfit spin', e)
        return False


def _groups(sim):
    """The pie menu's option lists for her: [(group, [(label, current)])] in the order the app shows them: 0 size preset,
    1 gravity, 2 spacing (friendship 3 and romance 4 are the phone's Cheats > Relationships, no records). Their controls: 300 + 30 * group + index."""
    from integratedbodyphysics import pie, store
    sim_id = sim.sim_info.sim_id
    out = []
    cup = store.cup(sim_id)
    out.append([(pie._auto_label(sim), cup is None)] + [(name, cup == i) for i, name in enumerate(pie.SIZES)])
    g = pie._own_gravity(sim)
    out.append([(name, g is not None and abs(g - v) < 0.01) for name, v in pie.GRAVITY] + [('Debug menu', g is None)])
    a = pie._own_apart(sim)
    out.append([(name, a is not None and abs(a - v) < 0.001) for name, v in pie.SPACING] + [('Debug menu', a is None)])
    return out


def _group_action(sim, group, i):
    from integratedbodyphysics import pie
    if group == 0:
        pie._set_cup(sim, None if i == 0 else i - 1)
    elif group == 1:
        pie._set_gravity(sim, None if i >= len(pie.GRAVITY) else pie.GRAVITY[i][1])
    elif group == 2:
        pie._set_apart(sim, None if i >= len(pie.SPACING) else pie.SPACING[i][1])
    elif group in (3, 4):
        levels = pie.FRIENDSHIP if group == 3 else pie.ROMANCE
        if i < len(levels):
            pie._set_rel(sim, group == 4, levels[i][1])


def _wear(sim, outfit):
    from integratedbodyphysics import pie
    if outfit == (5, 0):
        try:
            from sims.outfits.outfit_enums import OutfitCategory
            if not sim.sim_info.has_outfit((OutfitCategory.BATHING, 0)):
                sim.sim_info.generate_outfit(outfit_category_and_index=(OutfitCategory.BATHING, 0))
                _log('{}: made a bathing outfit'.format(_name(sim)))
        except Exception as e:
            _error('making the bathing outfit', e)
    if native.setting_flag('bodyphysics', 'outfit_spin', True) and _spin(sim, outfit):
        return
    pie._wear(sim, outfit[0], outfit[1])


def _step_outfit(sim, step):
    items = _outfits(sim)
    if not items:
        return
    at = next((i for i, o in enumerate(items) if o[2]), -1)
    _wear(sim, items[(at + step) % len(items)][0])


def thaw(npc, kind):
    """gestures.py / bodygrab.py, just before a gesture pushes something onto `npc` (come over, tap to turn, wave back,
    follow me): if she is frozen, the physics mod's hold on her is dropped so the action goes through (its queue gate
    refuses everything else, and its 2 s re-push would fight it). tick() freezes her again when she is idle:
    kind 'act' = after the action, 'follow' = once she no longer follows you (dismissed through the follow system:
    she stays frozen until you let her go)."""
    pose = _ibp()
    if pose is None:
        return
    try:
        sim_id = npc.sim_info.sim_id
        now = time.monotonic()
        entry = _thawed.get(sim_id)
        if entry is not None:
            entry[1] = now
            entry[2] = None
            if kind == 'follow':
                entry[0] = 'follow'
            return
        global _ww_free
        if _ww_free == sim_id and not pose.holding(npc):
            # In WickedWhims' pose state (hold already dropped): the gesture goes through, and she is frozen again after,
            # like any frozen Sim (a follower stays unfrozen while she follows; dismissed, she is held again).
            _ww_free = 0
            _thawed[sim_id] = [kind, now, None, None]
            _log('{}: gesture from the WickedWhims pose state ({})'.format(_name(npc), kind))
            return
        if not pose.holding(npc):
            return
        aff = pose._held.pop(sim_id, None)
        pose._since.pop(sim_id, None)
        _thawed[sim_id] = [kind, now, None, aff]
        _log('{}: freeze hold dropped for {}'.format(_name(npc), kind))
    except Exception as e:
        _error('thaw', e)


def _autonomy(sim, off):
    """Her own autonomy off while she is frozen (the pose hold only refuses what is queued; this stops the asking)."""
    global _skip_token
    try:
        if _skip_token is None:
            from sims4vr import follow
            _skip_token = follow._Token()  # skip_autonomy reads .source from it
        sim.skip_autonomy(_skip_token, off)
        (_skipped.add if off else _skipped.discard)(sim.sim_info.sim_id)
    except Exception as e:
        _error('autonomy', e)


def _end_follow(sim_id):
    """Unfreezing ends her Follow me too (user 2026-10-28 21:54Z)."""
    try:
        import services
        from sims4vr import follow
        info = services.sim_info_manager().get(sim_id)
        sim = info.get_sim_instance() if info is not None else None
        if sim is not None and follow.is_follower(sim):
            follow.dismiss(sim, 'she was unfrozen')
            _log('{}: no longer follows (unfrozen)'.format(_name(sim)))
    except Exception as e:
        _error('ending the follow', e)


def _guard(now):
    """Keeps the frozen Sim frozen (user 2026-10-28 19:28Z: they walked off after a while): once a second, if she is
    not held, walks, or runs / queues something that is not the pose, the hold is made again (the same call as the
    pie's Freeze all motor functions: stops her where she is and pushes the pose). Not while a gesture's action
    (thaw), WickedWhims' menu (ww_free) or the follow system has her."""
    global _guard_at, _guard_busy, _guard_last, _frozen_id
    if now < _guard_at:
        return
    _guard_at = now + 1.0
    pose = _ibp()
    sim = _frozen_sim()
    if sim is None or pose is None:
        _guard_busy = None
        if _frozen_id:  # unfrozen here or by the pie's As you were
            _end_follow(_frozen_id)
            _frozen_id = 0
        if _skipped:  # let go (here or by the pie's As you were): autonomy back
            import services
            for other_id in list(_skipped):
                info = services.sim_info_manager().get(other_id)
                other = info.get_sim_instance() if info is not None else None
                if other is not None:
                    _autonomy(other, False)
                _skipped.discard(other_id)
        return
    sim_id = sim.sim_info.sim_id
    if _ww_active(sim):
        # WickedWhims has her in its pose (its popup, or the animation after it): pushing her neutral pose over it broke the
        # animation, WickedWhims reset her, and she respawned over and over (user 2026-10-29). Hands off until she is let go.
        global _ww_free
        if _ww_free != sim_id:
            _ww_free = sim_id
            pose._held.pop(sim_id, None)
            pose._since.pop(sim_id, None)
            _log('{}: WickedWhims has her: the freeze hold is dropped'.format(_name(sim)))
        _guard_busy = None
        return
    if _frozen_id and _frozen_id != sim_id:  # another Sim was frozen instead: the first one is let go
        _end_follow(_frozen_id)
    _frozen_id = sim_id
    if sim_id in _thawed or _ww_free == sim_id:
        _guard_busy = None
        return
    if sim_id not in _skipped:
        _autonomy(sim, True)
    aff = pose._held.get(sim_id)
    why = None
    if aff is None:
        why = 'no hold'
    elif _busy(sim, aff):
        _guard_busy = _guard_busy or now
        if now - _guard_busy >= 1.0:
            why = 'doing something else'
    else:
        _guard_busy = None
    if why is None or now - _guard_last < 3.0:
        return
    _guard_last, _guard_busy = now, None
    if aff is None:
        _log('{}: guard: {}, freezing her again'.format(_name(sim), why))
        pose.hold(sim, pose.NEUTRAL, 'Neutral')
        return
    # Hold the pose she is held in (the pose you picked, not the neutral one): stop what else she does, push it again.
    pose._stop_running(sim, aff)
    result = pose._push(sim, aff, True)
    _log('{}: guard: {}, pushing {} again -> {}'.format(_name(sim), why, getattr(aff, '__name__', aff), result))


def _ww_active(sim):
    """A WickedWhims interaction runs or is queued on her (its pose menu, pose or sex animation)."""
    try:
        for si in list(sim.si_state) + list(sim.queue):
            aff = si.affordance
            if 'wickedwhims' in (getattr(aff, '__name__', '') or '').lower() or 'wickedwhims' in str(getattr(aff, '__module__', '')).lower():
                if not getattr(si, 'is_finishing', False):
                    return True
    except Exception:
        pass
    return False


def _busy(sim, aff):
    pose = _ibp()
    if pose._walked(sim):
        return True
    for si in list(sim.si_state) + list(sim.queue):
        if (aff is not None and pose._allowed(si, aff)) or getattr(si, 'is_finishing', False) or not getattr(si, 'visible', True):
            continue
        if getattr(si.affordance, '_provided_posture_type', None) is not None:
            continue
        return True
    return False


def _resume(now):
    global _resume_at
    if not _thawed or now < _resume_at:
        return
    _resume_at = now + 0.5
    import services
    pose = _ibp()
    for sim_id, entry in list(_thawed.items()):
        info = services.sim_info_manager().get(sim_id)
        sim = info.get_sim_instance() if info is not None else None
        if sim is None or pose is None or getattr(pose, '_current', 0) != sim_id:
            _thawed.pop(sim_id, None)  # gone, or you let her go
            continue
        if entry[0] == 'follow':
            from sims4vr import follow
            if follow.is_follower(sim):
                entry[2] = None
                continue
        if now - entry[1] < THAW_MIN:
            continue
        if _busy(sim, entry[3]):
            entry[2] = None
            continue
        entry[2] = entry[2] or now
        if now - entry[2] >= IDLE_FOR:
            _thawed.pop(sim_id, None)
            pose.hold(sim, pose.NEUTRAL, 'Neutral')
            _log('{}: frozen again'.format(_name(sim)))


def control(n):
    """controls.py: 8400000 + n."""
    global _armed, _status, _ww_free
    try:
        if n == 0:
            _armed = 0.0 if _armed else time.monotonic()
            _status = 'Touch her or look at her' if _armed else ''
            _log('pick mode {}'.format('armed' if _armed else 'off'))
        elif n == 1:
            pose = _ibp()
            sim = _frozen_sim()
            if pose is not None and sim is not None:
                pose.release(sim)
                _end_follow(sim.sim_info.sim_id)
                _autonomy(sim, False)
                _ww_free = 0
                _thawed.pop(sim.sim_info.sim_id, None)
                _log('{} let go'.format(_name(sim)))
        elif n == 2:
            you = _you()
            sim = _look_target(you, _number('look_angle', LOOK_ANGLE), DWELL_RANGE) if you is not None else None
            if sim is None:
                _status = 'Nobody in the middle of your look'
            else:
                pick(sim, 'look')
        elif n in (10, 11):
            sim = _frozen_sim()
            if sim is not None:
                _step_outfit(sim, 1 if n == 11 else -1)
        elif n == 12:
            sim = _frozen_sim()
            if sim is not None:
                _open_ww_menu(sim)
        elif n == 13:
            sim = _frozen_sim()
            pose = _ibp()
            if sim is not None and pose is not None:
                _ww_free = 0
                pose.hold(sim, pose.NEUTRAL, 'Neutral')
        elif n in (14, 15):
            sim = _frozen_sim()
            if sim is not None:
                from integratedbodyphysics import pie
                (pie._toggle_top if n == 14 else pie._toggle_enhanced)(sim)
        elif 300 <= n < 450:
            sim = _frozen_sim()
            if sim is not None:
                _group_action(sim, (n - 300) // 30, (n - 300) % 30)
        elif 100 <= n < 200:
            sim = _frozen_sim()
            pose = _ibp()
            if sim is not None and pose is not None and n - 100 < len(_poses):
                name, ids, source = _poses[n - 100]
                if source == 1:
                    _open_ww_menu(sim)
                elif source == 2:
                    _ww_free = 0
                    pose.hold_clip(sim, name)
                    _log('{}: pose {}'.format(_name(sim), name))
                else:
                    _ww_free = 0
                    # pose.hold resets her itself when she leaves a dance / stretch; a second reset here showed a
                    # T-pose flicker (user 2026-10-29) and dropped her from the app for a moment.
                    _grace[0] = (sim.sim_info.sim_id, time.monotonic())
                    pose.hold(sim, ids, name)
                    _log('{}: pose {}'.format(_name(sim), name))
        elif n >= 200:
            sim = _frozen_sim()
            items = _outfits(sim) if sim is not None else []
            if n - 200 < len(items):
                _wear(sim, items[n - 200][0])
    except Exception as e:
        _error('control {}'.format(n), e)
    _refresh()


def tick():
    """controls.py, every tick: the pick timeout and the look-at dwell."""
    global _armed, _dwell, _status
    now = time.monotonic()
    try:
        _resume(now)
        _guard(now)
    except Exception as e:
        _error('resume', e)
    if not _armed:
        return
    if now - _armed > _number('pick_timeout', PICK_TIMEOUT):
        _armed, _dwell, _status = 0.0, None, 'Pick mode timed out'
        _refresh()
        return
    you = _you()
    sim = _look_target(you, _number('dwell_angle', DWELL_ANGLE), DWELL_RANGE) if you is not None else None
    if sim is None:
        _dwell = None
    elif _dwell is None or _dwell[0] != sim.sim_info.sim_id:
        _dwell = (sim.sim_info.sim_id, now)
    elif now - _dwell[1] >= _number('dwell_seconds', DWELL_SECONDS):
        pick(sim, 'look')


def records(records):
    """phonesim.send: the "bp" / "bpp" records."""
    pose = _ibp()
    sim = _frozen_sim()
    if sim is not None and _poses_for != sim.sim_info.sim_id:
        _build_poses(sim)
    records.append('bp\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
        int(pose is not None), int(bool(_armed)), _name(sim) if sim is not None else '', int(sim is not None),
        int(_wickedwhims()), _status, int(sim is not None and _ww_free == sim.sim_info.sim_id)))
    if sim is None:
        return
    held = pose.held_affordance(sim) if pose is not None else None
    clip = pose.clip_name(sim) if pose is not None and hasattr(pose, 'clip_name') else None
    for i, (name, ids, source) in enumerate(_poses):
        try:
            aff = pose.affordance(ids) if ids and source != 2 else None
        except Exception:
            aff = None
        if source == 2:
            current = clip == name
        else:
            current = aff is not None and aff is held and clip is None
        records.append('bpp\x1f{}\x1f{}\x1f{}\x1f{}'.format(i, name, int(current), source))
    try:
        for g, row in enumerate(_groups(sim)):
            for i, (label, current) in enumerate(row):
                records.append('bpx\x1f{}\x1f{}\x1f{}\x1f{}'.format(g, i, label, int(current)))
        from integratedbodyphysics import pie, top_part
        top = ''
        if top_part(sim.sim_info) is not None:
            top = 'Top: Clothed' if pie._top_clothed(sim) else 'Top: Topless'
        records.append('bpt\x1f{}\x1f{}'.format(top, 'Breasts: Enhanced' if pie._enhanced(sim) else 'Breasts: Natural'))
    except Exception as e:
        _error('option lists', e)
    try:
        for i, (_, label, wearing) in enumerate(_outfits(sim)):
            records.append('bpo\x1f{}\x1f{}\x1f{}'.format(i, label, int(wearing)))
    except Exception as e:
        _error('outfits', e)
