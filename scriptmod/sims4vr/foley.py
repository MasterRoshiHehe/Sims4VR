"""Touch sounds (user 2026-10-26): your hands touching other Sims make the game's own foley, by how
hard and where: a slow touch is a soft cloth rustle or skin touch, faster a pat, fast a slap; sliding
along them a rub; grabbing (grip + trigger) and taking their hand their own sounds.

The DLL (armik.cpp keepOutOfBodies / foleyTrack) sees your hand meet a body part and how fast it went
into it and along it (native.foley_event). Here the speed picks light / pat / slap ([foley] pat_speed,
slap_speed, metres a second), the part and their outfit pick clothes or bare skin, and the debug menu's
choice for that (Options > Game settings > Other Sims > Touch sounds) picks the sound.

The sounds are the game's own foley (re/audio/configs.tsv: sound config instance = FNV-1 64 of the
lower-case name = the PlaySound sound id), base game unless noted. They play the way the game plays
tuned audio (audio.primitive.PlaySound, immediate) on the touched Sim at the joint of the part you
touched (user 2026-10-26 20:54Z: 3D where the touch is, not in your head; on your own Sim they were
heard mid-head), so the game places them there in 3D; first person only. PlaySound has no loudness: the game's
Effects slider (and Extended Sound Control) set it.
Control 7650000 + kind * 100 + n (settings.cpp, a choice picked): plays sound n of that kind.
The user's own sound files (user 2026-10-26 19:52Z: touch, pat, slap, punch, grab) play in the DLL
(foleyaudio.cpp) before any event reaches here; only what has none of them comes to foley.py.
"""
import math
import time

import alarms
import clock
import services

from sims4vr import native

TICK_SECONDS = 0.02  # a touch is heard within a frame or two of the DLL seeing it
LIVE_SECONDS = 4.0   # a sound is stopped (detached from your Sim) after this; the longest foley here is ~3 s
MAX_PLAYING = 8

# Sound ids (FNV-1 64 of the config name).
CLOTH_RUSTLE = 0x187B670A3202ED0B   # bunkbed_handgrab_cloth (18 short cloth grabs)
CLOTH_BRUSH = 0xDCA599D4D60F71D6    # mirror_smoothclothes_brush
ARM_SQUEEZE = 0x34458BDA394EA8B1    # intimate_cuddle_squeeze_armrub
SOFT_TOUCH = 0x4F68B4D1E6D1A220     # romantic_gosteady_giddypicture_succ_touching
KNEE_TOUCH = 0x1927B736E6E077AB     # simtoanimal_social_hand_touch_knee (EP14)
CUDDLE_TOUCH = 0x04CA235636437142   # cuddleinbed_hand_touch (EP16)
HAND_PAT = 0xA4A38BA9DFBD456A       # infant_handpat_sleep
SHOULDER_PAT = 0x14BECB4A1C5E0025   # vampirefight_vampiretovampire_spar_shoulder_pat
BACK_PAT = 0x50047E93FC429F66       # friendly_hug_succ
ARM_PUNCH = 0x5128F291CA08360D      # friendly_cheerup_puncharm_fail
THIGH_PAT = 0x57A58BDBB549AC20      # baby_coo_thighpat_succ
KNEE_PAT = 0x3233568646C39412       # hand_pat_knee (EP12)
LEG_PAT = 0x7AD7D0F266E8B3AF        # marbles_putdownmarbles_patleg (EP15)
LEG_SLAP = 0x6C3E5657BEF7C5B0       # react_cheer_slapleg_short
KNEE_SLAP = 0x554762616F2EB0B4      # soc_entertain_react_kneeSlap_hands
HAND_SLAP = 0x0553E8DF41C899C8      # mischief_hand_slap
QUICK_SLAP = 0x65300EF126106564     # bush_woohoo_getin_slap
SPIN_SLAP = 0xE1BF006AC5845A53      # mischief_fisticuffs_spinslap
SLAP_FIGHT = 0x01DF098EA42F2CB3     # mischief_fisticuffs_slapfight
WEREWOLF_SLAP = 0x0AEFBD4C3FEEB1D3  # werewolffights_werewolftowerewolf_loop_02_x_slap (GP12)
ARM_RUB = 0x2699206E7F0C260C        # intimate_cuddle_look_armrub
SNUGGLE_RUB = 0xCBFEEDC17C6307E9    # romantic_seated_intimate_snuggle_rub
BACK_RUB = 0xA6FE7F01C68CEC1E       # romantic_blanket_snuggle_rub_back (EP16)
BODY_GRAB = 0xB4C743A52ED54554      # vampirefight_vampiretovampire_start_body_grab
HUG_SQUEEZE = 0xFC0DE07D0625836D    # friendly_hug_squeeze_succ
HIGH_FIVE = 0xA774ECB676D25E08      # friendly_highfive_succ
HAND_CLASP = 0xC0FAE36BDDB522B9     # romantic_exchangevows_confident_handclasp
SOFT_CLASP = 0xE58E15144322C111     # friendly_begforgiveness_handclasp
HANDSHAKE = 0x2401BBB068717D3E      # club_welcomenewmember_handshake
# Your hand hitting objects (user 2026-10-27).
WALL_KNOCKS = 0x9C28EFD8E8D62FD0    # bumpersfx_sp18_hh_wall_knocks (SP18)
FINGER_TAP = 0xFC4C93FFDB3A0486     # politics_askbribe_finger_tap (EP03)
HEAD_BONK = 0x01710D5DDF0817D5      # bunkbed_bonkhead
CLAY_THUMP = 0xA0F7F1DA905425FA     # schoolprojects_clay_thump_carefully (GP05)
TABLE_SLAM = 0x5D4A8BF41F95D9E7     # rectable_simbles_wingame_slamtable_hands (EP13)
PIECE_SLAM = 0x4350D2B281526CEB     # rectable_simbles_wingame_slamtable_piece (EP13)
KEYBOARD_BANG = 0x795246F2FC4DF9F3  # computerstand_react_bangonkeyboard
FIST_POUND = 0x3F5D6F0B29C5EAC8     # fist_pound_hand
FIST_BANG = 0x5A02F1F9D576AD4E      # celldoor_makeruckus_bangfist
LOCKER_PUNCH = 0x2E0349C51BC8DF42   # schoollocker_hitlocker_punch (EP12)
LOCKER_KICK = 0xA9824FBA8E64C14C    # schoollocker_hitlocker_kick (EP12)
# Plain hand-on-surface foley (user 2026-10-27 14:15Z: the wall knocks sounded creepy, reverb).
HAND_ON_TILE = 0x45B23DA343FF0DC5   # shower_hand_place (2, 0.4-0.6 s)
HAND_ON_FRAME = 0x745A18ECE93ABB0E  # ep16_closet_woohoo_hand_on_doorframe (EP16)
HAND_PLANT = 0xD82C4AF1E6AC3C54     # fairyrelatedwoohoo_getout_handplant (EP19)
QUIET_TAP = 0x6D47339A1F32E968      # generic_awkward_confession_tap (3, 0.2 s)
PUT_DOWN = 0x19F9143A658A9846       # obj_put (76 short put-downs on surfaces)
HAND_HIT = 0x1996E14288E9DA67       # friendly_debate_hand_hit_succ
LADDER_HAND = 0x327AD5E28EE5302F    # oceanladder_hand (metal ladder, 6)
LADDER_GRAB = 0x4C227DD86D5249B9    # ladder_handgrab (14 short)
BLANKET_HAND = 0xC17385CA064F2B66   # romantic_blanket_hand_plant (EP16, 6 short)

# Each kind: its [foley] key and its sounds (index 0 none); order and names as settings.cpp's rows.
KINDS = (
    ('light_cloth', (None, CLOTH_RUSTLE, CLOTH_BRUSH, ARM_SQUEEZE)),
    ('light_skin', (None, SOFT_TOUCH, KNEE_TOUCH, CUDDLE_TOUCH, HAND_PAT)),
    ('pat_cloth', (None, SHOULDER_PAT, BACK_PAT, ARM_PUNCH, CLOTH_RUSTLE)),
    ('pat_skin', (None, HAND_PAT, THIGH_PAT, KNEE_PAT, LEG_PAT)),
    ('slap_cloth', (None, LEG_SLAP, KNEE_SLAP, ARM_PUNCH)),
    ('slap_skin', (None, HAND_SLAP, QUICK_SLAP, SPIN_SLAP, SLAP_FIGHT, WEREWOLF_SLAP)),
    ('rub_cloth', (None, CLOTH_BRUSH, ARM_SQUEEZE, CLOTH_RUSTLE)),
    ('rub_skin', (None, ARM_RUB, SNUGGLE_RUB, BACK_RUB)),
    ('grab_cloth', (None, CLOTH_RUSTLE, BODY_GRAB, HUG_SQUEEZE)),
    ('grab_skin', (None, ARM_SQUEEZE, SOFT_TOUCH, HAND_PAT)),
    ('hands', (None, HIGH_FIVE, HAND_CLASP, SOFT_CLASP)),
    ('hold_hands', (None, HAND_CLASP, SOFT_CLASP, HANDSHAKE)),
    ('object_tap', (None, HAND_ON_TILE, HAND_ON_FRAME, HAND_PLANT, QUIET_TAP, PUT_DOWN, FINGER_TAP, CLAY_THUMP,
                    WALL_KNOCKS)),
    ('object_slap', (None, TABLE_SLAM, HAND_HIT, PIECE_SLAM, KEYBOARD_BANG, HAND_ON_FRAME)),
    ('object_punch', (None, TABLE_SLAM, FIST_POUND, HEAD_BONK, FIST_BANG, HAND_HIT)),
    ('object_metal', (None, LOCKER_PUNCH, FIST_BANG, LOCKER_KICK)),
    ('object_soft', (None, BLANKET_HAND, CLOTH_RUSTLE, BACK_PAT, CLAY_THUMP, BODY_GRAB)),
    ('object_metal_tap', (None, LADDER_HAND, LADDER_GRAB, HAND_ON_TILE)),
)
OBJECT_HITS = ('object_tap', 'object_slap', 'object_punch')
MATERIAL_NAMES = ('hard', 'metal', 'soft', 'glass', 'wall')
KIND_INDEX = {key: i for i, (key, _) in enumerate(KINDS)}

# The DLL's parts (armik.h FoleyEvent).
TORSO, HEAD, UPPER_ARM_L, UPPER_ARM_R, FOREARM_L, FOREARM_R, THIGH_L, THIGH_R, CALF_L, CALF_R, HAND, HIPS = range(12)
PART_NAMES = ('torso', 'head', 'upper arm', 'upper arm', 'forearm', 'forearm', 'thigh', 'thigh', 'calf', 'calf', 'hand',
              'hips')

# Outfits (sims/outfits/outfit_enums.py): OutfitCategory and the CAS BodyType a part covers.
EVERYDAY, FORMAL, ATHLETIC, SLEEP, PARTY, BATHING, CAREER, SITUATION, SPECIAL, SWIMWEAR, HOTWEATHER, COLDWEATHER = range(12)
FULL_BODY, UPPER_BODY, LOWER_BODY = 5, 6, 7

_owner = type('_Owner', (), {})()
_alarm = None
_playing = []  # (PlaySound, started)
_settings = {}
_settings_at = -10.0
_sims = {}  # actor -> (sim id, when found)
_outfits = {}  # sim id -> ((category, index), (upper covered, lower covered), when read)
_logs = 0
_errors = 0


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 60:
        native._log('foley: ' + message)


def _error(where, e):
    global _errors
    _errors += 1
    if _errors <= 8:
        native._log('foley: {} failed: {!r}'.format(where, e))


def _setting(key, default):
    try:
        return float(native.setting('foley', key, str(default)))
    except (TypeError, ValueError):
        return default


def _refresh_settings(now):
    """[foley] values, read again every 2 s (the debug menu changes them live)."""
    global _settings_at
    if now - _settings_at < 2.0:
        return
    _settings_at = now
    _settings['on'] = native.setting_flag('foley', 'on', True)
    _settings['pat_speed'] = _setting('pat_speed', 0.5)
    _settings['slap_speed'] = _setting('slap_speed', 2.2)
    _settings['rub_speed'] = _setting('rub_speed', 0.3)
    for key, sounds in KINDS:
        n = int(_setting(key, 1))
        _settings[key] = n if 0 <= n < len(sounds) else 0


def _joint(name):
    h = 0x811c9dc5
    for c in name.lower().encode('ascii'):
        h = ((h * 0x1000193) & 0xffffffff) ^ c
    return h


def _hand_joint(hand):
    return _joint('b__R_Hand__' if hand else 'b__L_Hand__')


# The touched Sim's joint for each part (the DLL's part numbers); their hand: its side isn't sent.
PART_JOINTS = tuple(_joint(n) for n in (
    'b__Spine2__', 'b__Head__', 'b__L_UpperArm__', 'b__R_UpperArm__', 'b__L_Forearm__', 'b__R_Forearm__',
    'b__L_Thigh__', 'b__R_Thigh__', 'b__L_Calf__', 'b__R_Calf__', 'b__R_Hand__', 'b__Pelvis__'))


def _you():
    from sims4vr import body
    return services.object_manager().get(body.following())


def _play(target, joint, sound_id):
    """The game's PlaySound on that Sim at that joint, sent at once (not after its animation)."""
    from audio.primitive import PlaySound
    now = time.monotonic()
    while len(_playing) >= MAX_PLAYING:
        _stop(_playing.pop(0)[0])
    sound = PlaySound(target, sound_id, joint_name_hash=joint, immediate=True)
    sound.start()
    _playing.append((sound, now))


def _stop(sound):
    try:
        sound.stop()
    except Exception:
        pass


def _npc(you, actor, now):
    """The Sim near you whose client object is `actor` (looked up once a few seconds)."""
    found = _sims.get(actor)
    if found is not None and now - found[1] < 5.0:
        return services.object_manager().get(found[0])
    from sims4vr import bodygrab
    sim = bodygrab._sim_for_actor(you, actor)
    _sims[actor] = (sim.id if sim is not None else 0, now)
    return sim


def _covered(sim, now):
    """(upper body covered, lower body covered) by the outfit the Sim wears now (CAS body types; bathing:
    none, swimwear: the legs bare). Unknown: both covered."""
    if sim is None:
        return True, True
    info = sim.sim_info
    try:
        current = tuple(info.get_current_outfit())
    except Exception:
        return True, True
    known = _outfits.get(sim.id)
    if known is not None and known[0] == current and now - known[2] < 10.0:
        return known[1]
    upper = lower = True
    try:
        category = int(current[0])
        types = set(int(t) for t in info.get_outfit(*current).body_types)
        upper = bool(types & {FULL_BODY, UPPER_BODY}) and category != BATHING
        lower = bool(types & {FULL_BODY, LOWER_BODY}) and category not in (BATHING, SWIMWEAR)
        if category == SWIMWEAR:
            upper = False  # a swimsuit top covers little of it
    except Exception as e:
        _error('outfit', e)
    _outfits[sim.id] = (current, (upper, lower), now)
    return upper, lower


def _cloth(sim, part, now):
    """Whether that part of the Sim is under clothes."""
    if part in (HEAD, HAND, FOREARM_L, FOREARM_R):
        if part in (FOREARM_L, FOREARM_R) and sim is not None:
            try:
                return int(sim.sim_info.get_current_outfit()[0]) == COLDWEATHER  # long sleeves
            except Exception:
                return False
        return False
    upper, lower = _covered(sim, now)
    if part in (TORSO, UPPER_ARM_L, UPPER_ARM_R):
        return upper
    if part in (CALF_L, CALF_R) and sim is not None:
        try:
            if int(sim.sim_info.get_current_outfit()[0]) == HOTWEATHER:
                return False  # shorts and skirts
        except Exception:
            pass
    return lower


def _kind_for(event, sim, now):
    """The [foley] key for a touch event, or None (too slow to hear, or rubs off)."""
    kind, _hand, part, _actor, into, along = event[:6]
    if kind == 4:
        return 'hold_hands'
    surface = 'cloth' if _cloth(sim, part, now) else 'skin'
    if kind == 3:
        return 'grab_' + surface
    if kind == 2:
        rub = _settings['rub_speed']
        return 'rub_' + surface if rub > 0 and along >= rub else None
    if part == HAND and into >= _settings['pat_speed']:
        return 'hands'
    if into >= _settings['slap_speed']:
        return 'slap_' + surface
    if into >= _settings['pat_speed']:
        return 'pat_' + surface
    return 'light_' + surface


@native.timed('foley tick')
def _tick(_handle):
    try:
        now = time.monotonic()
        while _playing and now - _playing[0][1] > LIVE_SECONDS:
            _stop(_playing.pop(0)[0])
        _refresh_settings(now)
        from sims4vr import knockout
        knockout.tick()
        played = 0
        while True:
            event = native.foley_event()
            if event is None:
                break
            if event[0] == 5:  # a slap or punch (knockout.py; its sound came as kind 1)
                you = _you()
                if you is not None:
                    from sims4vr import knockout
                    knockout.hit(_npc(you, event[3], now), event[1], event[2], event[4], event[5] > 0)
                continue
            if not _settings['on'] or played >= 3:
                continue  # drained, not heard
            you = _you()
            if you is None:
                continue
            if event[0] == 6:
                if _object_hit(you, event):
                    played += 1
                continue
            npc = _npc(you, event[3], now)
            key = _kind_for(event, npc, now)
            if key is None:
                continue
            n = _settings[key]
            sound_id = KINDS[KIND_INDEX[key]][1][n]
            if not sound_id:
                continue
            part = event[2]
            if npc is not None and 0 <= part < len(PART_JOINTS):
                _play(npc, PART_JOINTS[part], sound_id)
            else:
                _play(you, _hand_joint(event[1]), sound_id)
            played += 1
            _log('{} hand: {} on {}\'s {} ({:.2f} m/s in, {:.2f} along) -> sound {} of {}'.format(
                'right' if event[1] else 'left', key, npc.full_name if npc is not None else 'a Sim',
                PART_NAMES[event[2]] if 0 <= event[2] < len(PART_NAMES) else event[2], event[4], event[5], n, key))
    except Exception as e:
        _error('tick', e)


def _object_at(x, y, z):
    """The object of collide.py's scene under (x, y, z), or None."""
    from sims4vr import collide
    best, best_d = None, 0.25
    for pts, y0, y1, _, _, _, obj_id in collide.objects:
        if not y0 - 0.1 <= y <= y1 + 0.15:
            continue
        inside = False
        n = len(pts)
        for i in range(n):
            (xi, zi), (xj, zj) = pts[i], pts[i - 1]
            if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
                inside = not inside
        if inside:
            return services.object_manager().get(obj_id)
        cx = sum(p[0] for p in pts) / n
        cz = sum(p[1] for p in pts) / n
        d = min(math.hypot(px - x, pz - z) for px, pz in pts + [(cx, cz)])
        if d < best_d:
            best, best_d = obj_id, d
    return services.object_manager().get(best) if best is not None else None


def _object_hit(you, event):
    """Your hand hit an object or wall (kind 6) with none of your own sounds for it: the game's."""
    _, hand, part, _, into, _, at = event
    material, hit = divmod(part, 4)
    if not 0 <= hit < len(OBJECT_HITS):
        return False
    if material == 1:
        key = 'object_metal_tap' if hit == 0 else 'object_metal'
    else:
        key = 'object_soft' if material == 2 else OBJECT_HITS[hit]
    n = _settings[key]
    sound_id = KINDS[KIND_INDEX[key]][1][n]
    if not sound_id:
        return False
    obj = _object_at(*at) if material != 4 else None
    if obj is not None:
        _play(obj, None, sound_id)
    else:
        _play(you, _hand_joint(hand), sound_id)
    _log('{} hand: {} on {} ({}, {:.2f} m/s) -> sound {} of {}'.format(
        'right' if hand else 'left', OBJECT_HITS[hit], obj if obj is not None else 'a wall' if material == 4 else 'something',
        MATERIAL_NAMES[material] if 0 <= material < len(MATERIAL_NAMES) else material, into, n, key))
    return True


def preview(code):
    """A choice picked on the debug menu (settings.cpp): its sound at your right hand."""
    kind, n = divmod(code, 100)
    try:
        if not 0 <= kind < len(KINDS) or not 0 < n < len(KINDS[kind][1]):
            return
        you = _you()
        if you is None:
            sim_info = services.active_sim_info()
            you = sim_info.get_sim_instance() if sim_info is not None else None
        if you is None:
            return
        _play(you, _hand_joint(1), KINDS[kind][1][n])
        _log('preview: sound {} of {}'.format(n, KINDS[kind][0]))
    except Exception as e:
        _error('a preview', e)


def start():
    global _alarm, _settings_at
    _settings_at = -10.0
    if _alarm is None:
        _alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(TICK_SECONDS), _tick, repeating=True)
        _log('on')


def stop():
    global _alarm
    if _alarm is not None:
        alarms.cancel_alarm(_alarm)
        _alarm = None
    while _playing:
        _stop(_playing.pop(0)[0])
    _sims.clear()
    _outfits.clear()
