"""First person: plan your outfit in the room, no trip to Create a Sim (user 2026-10-29 19:36Z / 19:43Z,
research/CAS-VR.md 4b, research/CAS-LIVE-PLAN.md P6; built 2026-10-31, docs/OUTFIT-LIVE-PROGRESS.md). God view:
nothing changes.

The game's Plan Outfit / Change Appearance on a mirror, dresser or wardrobe ends in cas_commands.py sending
"sims.exit2caswithhouseholdid" (or "sims.exit2cas"), which unloads the lot for CAS. For the Sim you are in, that
command (sims4.commands.client_cheat) opens our outfit panel instead (drawn by the DLL: hud.cpp's kOutfit region,
hud_outfit.inc; placed by vr_parts/14c_outfit.inc):
- a mirror: the panel stands on its glass: the CAS category buttons with the game's own icons and names (the
  CASMenuItem tuning), the catalog grid with the game's CAS thumbnails, the worn item's colours, Undo / Redo /
  Cancel / Done;
- a dresser or wardrobe: nothing shows until you grab its front (grip and trigger, as a fridge handle) and pull it
  toward you; a drawer slides out at that height, fitted to the object, holding one kind of clothes
  (DRAWER_LAYOUTS, or what the container is tagged to store). With [outfit] always on, dressers and wardrobes open
  that way whenever you pull one, no Plan Outfit needed.
- anything else: the panel stands in front of you where it opened (pose fixed when shown).
A pick wears the part on the outfit you have on, at once and permanently, the way the game wears jewelry and
tattoos in live mode (AppearanceModifier.SetCASPart with update_genetics through the appearance tracker; the game's
own conflict rules take off what it replaces). Every change is checked; a part not worn afterwards (or a top still
over a full body outfit) is done again the direct way (set_caspart into a copy, then apply_siminfo_override over
every body type that changed). A top or bottom that comes without its other half gets the half you wore before.
Undo / Redo step through copies of the outfits taken before each change; Cancel puts back the copy from when the
panel opened; Done, walking away, pushing the drawer back or leaving first person keep it.

The catalog is the game's live CAS catalog (cas.get_catalog_casparts_by_bodytype, what its CASCatalogPicker
lists: owned packs, rewards, locked parts greyed). Colours: the game's live service GetCatalogColorFamilies, asked
through the DLL (clientdlg query) [guess: its answer's shape outside CAS]; if it answers nothing usable, every
colour is listed as its own item instead ([outfit] all_variants forces that).

Tagged containers: in first person a dresser's or wardrobe's own pie has a "Store" menu (kinds and occasions); its
drawers then hold only those. Kept per save in our own file (outfit_tags.json beside the log), keyed by zone and
object id, never in the save.

Probe (debug menu, developer row "Outfit probe"): catalog sizes and times, the colour service's raw answer,
CAS thumbnails asked for, a wear + undo on your top (timed), and whether the last change was still worn after the
last load (checked by itself on every lot load, from outfit_probe.json).

Switches ([outfit] in sims4vr.ini, debug menu Options > Game settings > Live outfit planning): on (the whole feature:
off = Plan Outfit goes to CAS, no drawers, no Store options), drawers, always, drawer_pull, sounds, tags; developer:
all_variants, apply_route, log.
"""
import json
import math
import os
import time

from sims4vr import hooks, native

MIRROR, DRAWER, FRONT = 1, 2, 3           # outfit_place kinds: on a mirror's glass, a drawer, in front of you
RACK = 4   # a coat / clothes / hat / shoe rack: grabbed and pulled like a drawer, the panel on its front (DLL: MIRROR)
WARDROBE = 5  # a wardrobe's doors (a dresser front taller than WARDROBE_HEIGHT): grabbed and pulled open, the panel
              # on its front (DLL: MIRROR). The game has no door animation for wardrobes (Sims spin in front of them).
WARDROBE_HEIGHT = 1.3  # m: a dresser's front taller than this is a wardrobe, opened by its doors (user 2026-10-31,
                       # 809: a drawer sliding out of a wardrobe's doors looked wrong)
CONTROL = 8600000                         # controls.py sends 8600000..8699999 here (control)
C_TAB, C_GROUP, C_PART, C_COLOUR = 0, 100, 1000, 70000
C_UNDO, C_REDO, C_REMOVE, C_DONE, C_CANCEL = 80000, 80001, 80002, 80003, 80004
C_PROBE = 90000
PIE_CATEGORY = 0x5653435453544F52         # our pie sub menu "Store" (pie.custom_categories)
GUID = 0x5653344F55544649                 # our appearance modifiers' id (no persistent data under it)

WALK_AWAY = 2.0      # m from the object's front (flat) that closes the panel (kept)
REACH = 1.3          # m from your head to a dresser's front for its drawers to answer a pull
FRONT_NEAR = 0.2     # m: a hand this far out from a front (along its normal) can take hold of it
FRONT_BEHIND = 0.12  # m: ... or this far behind it (the hand stops at the visible front; palm centre, slack)
DRAWER_PULL = 0.18   # m: default [outfit] drawer_pull
SCAN_EVERY = 30.0    # s between looks for dressers / wardrobes / mirrors on the lot
GESTURE_TICK = 0.05  # s: hands polled this often while one of them is near a front
UNDO_STEPS = 30
COLOUR_WAIT = 4.0    # s for the colour service's answer

# The CAS menu items (cas.cas_tuning CASMenuItem, 1.128 tuning re/cas/tuning/CASMenuItem.xml): name (string hash),
# icon, selected icon, the body types they list (sims.outfits.outfit_enums.BodyType), and whether a worn part can
# be taken off (the vanilla cell's X; tops, bottoms, full body, shoes and hair can only be swapped).
CATEGORIES = {
    'hair': (0x6B11F8D9, 0xa37496e69727489f, 0x30d04e6930ba763a, (2,), False),
    'hats': (0x932CC1D3, 0x54389e1f2836f8d6, 0x5f351227a07095ea, (1,), True),
    'glasses': (0x6CC1672C, 0x96c81dfeac638781, 0xb870bc0660ba21d4, (11,), True),
    'earrings': (0xFF4DA82D, 0x3de5798a3af3701e, 0xdb42926556f5b815, (10,), True),
    'tops': (0x19DB81DB, 0x46d6121fb1a77c10, 0x3adb0bc62bd6ea57, (6,), False),
    'bottoms': (0xADD19AF2, 0x7c493b701de28da8, 0x1eedeb0e10aefc9f, (7,), False),
    'fullbody': (0x57ACEDFB, 0xeb413aa4ee45362e, 0xb3804f124675f665, (5,), False),
    'shoes': (0xE2EA280E, 0x99aff0d3c97a1298, 0x4ac468b228b8bc8f, (8,), False),
    'necklaces': (0x5462E06E, 0x334d6eca343ffc32, 0x15b339c50849c369, (12,), True),
    'bracelets': (0xDDAAD362, 0xbe775f90f42eae36, 0xfd40485baf66cd7d, (14, 15), True),
    'rings': (0xE6BBD9BC, 0xe64fa31ad7349570, 0xbc31864228317ef7, (22, 23, 24, 25, 26, 27), True),
    'gloves': (0x4A2E2B31, 0xfc777083dd08da15, 0xdfc0382eb94843e8, (13,), True),
    'socks': (0x69B7B549, 0x944a3d13cf6a535e, 0x9cf1a423b5be4f55, (36,), True),
    'tights': (0xE59B9C7D, 0x15d3f20bdde906d0, 0x1503ef01d7b02717, (42,), True),
}
# The groups of the vanilla CAS context menu arcs (a group with several categories gets tabs over its grid);
# "Accessories" is CASMenuItem_BodyAccessories' name and icon.
GROUPS = {
    'hair': ('hair',), 'hats': ('hats',), 'tops': ('tops',), 'bottoms': ('bottoms',), 'fullbody': ('fullbody',),
    'shoes': ('shoes',),
    'accessories': ('glasses', 'earrings', 'necklaces', 'bracelets', 'rings', 'gloves', 'socks', 'tights'),
}
GROUP_LOOK = {'accessories': (0x8D495509, 0xd960de5e7b5f328e, 0x51e278d1d2f71e05)}
MIRROR_GROUPS = ('hair', 'hats', 'tops', 'bottoms', 'fullbody', 'shoes', 'accessories')
# A dresser's drawers, top to bottom, by how many it has (its front's height); a wardrobe's shelves the same.
DRAWER_KINDS = ('accessories', 'hats', 'tops', 'fullbody', 'bottoms', 'shoes')
DRAWER_LAYOUTS = {
    1: (DRAWER_KINDS,),
    2: (('accessories', 'hats', 'tops', 'fullbody'), ('bottoms', 'shoes')),
    3: (('accessories', 'hats'), ('tops', 'fullbody'), ('bottoms', 'shoes')),
    4: (('accessories', 'hats'), ('tops',), ('fullbody', 'bottoms'), ('shoes',)),
    5: (('accessories', 'hats'), ('tops',), ('fullbody',), ('bottoms',), ('shoes',)),
}
# Occasions a container can be tagged with: the CAS part tags OutfitCategory_* (tag ids from the part index
# re/client/cas/cas_parts.tsv.gz).
OCCASIONS = (('everyday', 'Everyday', 77), ('formal', 'Formal', 78), ('athletic', 'Athletic', 80),
             ('sleep', 'Sleep', 81), ('party', 'Party', 83), ('swimwear', 'Swimwear', 1229),
             ('hot', 'Hot weather', 2053), ('cold', 'Cold weather', 2054))
KIND_NAMES = (('accessories', 'Accessories'), ('hats', 'Hats'), ('tops', 'Tops'), ('fullbody', 'Full body'),
              ('bottoms', 'Bottoms'), ('shoes', 'Shoes'))
# Plannable outfit categories (sims.outfits.outfit_enums REGULAR_OUTFIT_CATEGORIES): Everyday, Formal, Athletic,
# Sleep, Party, Swimwear, Hot / Cold weather. Others (bathing, career, situation...) go to the game's CAS.
PLANNABLE = (0, 1, 2, 3, 4, 9, 10, 11)
UPPER, LOWER, FULL = 6, 7, 5
# A drawer's sounds, where the game has them (EP pet cage drawers; nothing plays without that pack): open, close.
DRAWER_SOUNDS = (0x0C82AD1773D1BEC8, 0x6CD00FE9B4B4A0C4)
# A wardrobe's doors: Lovestruck's closet door (ep16_closet_woohoo_door_open / _close, re/audio configs.tsv): open, close.
DOOR_SOUNDS = (0x9F5CC3180B8A5DBE, 0x319F407B2BE65DDA)

_logs = 0
ALWAYS = 10 ** 9   # log limit for the Plan Outfit decisions (rare, and the test needs every one)
_owner = type('_Owner', (), {})()
_session = None
_lot = []            # [(object id, kind)] of the dressers, wardrobes and mirrors on the lot (_scan)
_scanned = 0.0
_scan_zone = None
_kinds = {}          # object definition id: its kind (MIRROR, DRAWER, RACK or 0; a tall DRAWER opens as WARDROBE)
_faces = {}          # object id: (time, face) from _face
_front_noted = set()  # definition ids whose front was logged
_hold_notes = [0, 0.0]  # hand-near-a-front-but-no-hold log lines written, when the last one was
_grab = [None, None]  # per hand: what it holds of a front (dict) while grip and trigger stay closed
_closed = [False, False]  # per hand: grip and trigger closed at the last tick (_note_unknown)
_unknown_noted = set()  # definition ids a closing hand was logged by, "not a dresser ..." (once each)
_gesture_alarm = None
_waiting = None      # (object id, Sim id): Plan Outfit on a dresser waits for a pulled drawer
_why = ''            # why the last CAS command was left to the game (log)
_catalogs = {}       # (Sim id, category, occasions, all variants): [(part, locked tooltip id)]
_colour_fail = 0
_colour_logged = 0
_tags = None         # {save guid: {"zone:object": [tags]}} (outfit_tags.json)
_checked_zone = None
_probe_result = ''
_probe_steps = []    # [(time, what)] the probe's later steps
_times = []          # ms of each wear, for the probe's summary


def _log(message, most=400):
    """A log line while fewer than `most` have been written (every one with [outfit] log on, developer)."""
    global _logs
    _logs += 1
    if _logs <= most or native.setting_flag('outfit', 'log', False):
        native._log('outfit: ' + message)


def _dll(name):
    return native._optional(name)


def _on():
    """Live outfit planning on (debug menu row "Live outfit planning"): off, everything is the game's own."""
    return native.setting_flag('outfit', 'on', True)


def _setting_number(key, default):
    try:
        return float(native.setting('outfit', key, default))
    except Exception:
        return default


# ---------------------------------------------------------------------------------------------------------------
# The outfit itself: what is worn, copies, wearing and taking off.

def _parts_of(sim_info, outfit):
    """{body type: [part ids]} of that outfit (category, index); {} if the Sim has none (never generated here)."""
    try:
        base = sim_info._base
        if not base.has_outfit(outfit[0], outfit[1]):
            return {}
        data = base.get_outfit(outfit[0], outfit[1])
    except Exception:
        return {}
    worn = {}
    try:
        for part, body_type in zip(data.part_ids, data.body_types):
            worn.setdefault(int(body_type), []).append(int(part))
    except Exception as e:
        _log('outfit parts not read: {!r}'.format(e), 40)
    return worn


def _worn(sim_info):
    return _parts_of(sim_info, sim_info.get_current_outfit())


def _snapshot(sim_info):
    genetic = None
    try:
        genetic = sim_info._base.genetic_data
        if hasattr(genetic, 'SerializeToString'):
            genetic = genetic.SerializeToString()
        genetic = bytes(genetic) if genetic is not None else None
    except Exception:
        genetic = None
    return bytes(sim_info._base.outfits), genetic


def _restore(sim_info, snap):
    outfits, genetic = snap
    sim_info._base.outfits = outfits
    if genetic is not None:
        try:
            current = sim_info._base.genetic_data
            if isinstance(current, (bytes, bytearray)) or current is None:
                sim_info._base.genetic_data = genetic
            else:
                copy = type(current)()
                copy.ParseFromString(genetic)
                sim_info._base.genetic_data = copy
        except Exception as e:
            _log('genetic data not put back: {!r}'.format(e), 40)
    try:
        sim_info.resend_outfits()
        sim_info.resend_genetic_data()
    except Exception:
        sim_info.resend_physical_attributes()


def _body_type(part):
    from cas import cas
    return int(cas.get_caspart_bodytype(part))


def _flags():
    from cas.cas import OutfitOverrideOptionFlags
    return OutfitOverrideOptionFlags.SKIP_BODY_MODIFICATIONS | OutfitOverrideOptionFlags.APPLY_IN_CURRENT_MODIFIED_SIM_INFO


def _by_tracker(sim_info, part=None, body_type=None):
    """The game's own live route (jewelry, tattoos): a permanent appearance modifier on the current outfit."""
    from buffs.appearance_modifier.appearance_modifier import AppearanceModifier, AppearanceModifierPriority
    from objects.components.jewelry_component import AppearanceModifierTuple
    from tunable_multiplier import TunableMultiplier  # as jewelry_component.py imports it
    if part is not None:
        modifier = AppearanceModifier.SetCASPart(
            cas_part=part, should_toggle=False, replace_with_random=False, update_genetics=True,
            _is_combinable_with_same_type=False, remove_conflicting=True, outfit_type_compatibility=None,
            appearance_modifier_tag=None, expect_invalid_parts=True, hsv_color_shift=None, object_id=0,
            part_layer_index=-1, rgba_color_shift=0, should_refresh_thumbnail=False)
    else:
        modifier = AppearanceModifier.RemoveCASPartByBodyType(
            body_type=body_type, update_genetics=True, outfit_type_compatibility=None, appearance_modifier_tag=None,
            _is_combinable_with_same_type=False, should_refresh_thumbnail=False, remove_custom_textures=False)
    element = AppearanceModifierTuple(modifier, TunableMultiplier.ONE)
    sim_info.appearance_tracker.add_appearance_modifiers(((element,),), GUID, AppearanceModifierPriority.INVALID,
                                                         False, _flags(), source='sims4vr outfit')


def _by_override(sim_info, part=None, body_type=None):
    """The direct way: set_caspart (or remove_caspart_by_bodytype) into a copy of the Sim, then the copy's current
    outfit over the Sim's for every body type that differs (so a full body taken off by the conflict rules goes
    too). False if the native call refused."""
    import random
    from cas.cas import set_caspart, remove_caspart_by_bodytype, apply_siminfo_override, OutfitOverrideOptionFlags
    from sims.outfits.outfit_enums import BodyTypeFlag
    from sims.sim_info_base_wrapper import SimInfoBaseWrapper
    outfit = sim_info.get_current_outfit()
    before = _worn(sim_info)
    modified = SimInfoBaseWrapper(gender=sim_info.gender, age=sim_info.age, species=sim_info.species,
                                  first_name=sim_info.first_name, last_name=sim_info.last_name,
                                  breed_name=sim_info.breed_name, full_name_key=sim_info.full_name_key,
                                  breed_name_key=sim_info.breed_name_key)
    with sim_info.set_temporary_outfit_flags_on_all_outfits(0):
        if part is not None:
            done = set_caspart(sim_info._base, modified._base, part, False, False, True, random.randint(0, 0xffffffff),
                               remove_conflicting=True, h_shift=0, s_shift=0, v_shift=0, object_id=0,
                               part_layer_index=-1, rgba_color_shift=0)
        else:
            done = remove_caspart_by_bodytype(sim_info._base, modified._base, body_type, True, False)
    if not done:
        return False
    after = _parts_of(modified, outfit)
    changed = set()
    if after:
        changed = {bt for bt in set(before) | set(after) if sorted(before.get(bt, ())) != sorted(after.get(bt, ()))}
    else:
        _log('override: the copy has no outfit {}; only the part\'s body type (and its halves) taken'.format(outfit), 40)
    kind = _body_type(part) if part is not None else int(body_type)
    changed.add(kind)
    if kind in (UPPER, LOWER, FULL):
        changed |= {UPPER, LOWER, FULL} if after else set()
    flags = BodyTypeFlag.make_body_type_flag(*changed)
    options = OutfitOverrideOptionFlags.OVERRIDE_HAIR_MATCH_FLAGS | OutfitOverrideOptionFlags.SKIP_BODY_MODIFICATIONS
    try:
        modified.set_current_outfit(outfit)  # as appearance_tracker.apply_appearance_modifiers does before its override
    except Exception as e:
        _log('override: the copy\'s outfit not set: {!r}'.format(e), 20)
    with modified.set_temporary_outfit_flags(outfit[0], outfit[1], flags):
        apply_siminfo_override(sim_info._base, modified._base, sim_info._base, options)
    sim_info.resend_physical_attributes()
    return True


def _good(worn, part, kind):
    if part not in worn.get(kind, ()):
        return False
    if kind in (UPPER, LOWER) and worn.get(FULL):
        return False
    if kind == FULL and (worn.get(UPPER) or worn.get(LOWER)):
        return False
    return True


def _route():
    """[outfit] apply_route (developer): 0 the game's route, checked, then the direct way; 1 the game's only; 2 direct."""
    try:
        return max(0, min(2, int(_setting_number('apply_route', 0))))
    except Exception:
        return 0


def wear(sim_info, part, fill=True, note=True):
    """Wears `part` on the outfit on now (permanent). (worn, route, ms). note: the persistence check's record."""
    started = time.perf_counter()
    kind = _body_type(part)
    before = _worn(sim_info)
    route = _route()
    used = None
    for name, fn in (('appearance', _by_tracker), ('override', _by_override)):
        if (route == 1 and name != 'appearance') or (route == 2 and name != 'override'):
            continue
        try:
            fn(sim_info, part=part)
        except Exception as e:
            _log('{} route for part {} failed: {!r}'.format(name, part, e), 80)
            continue
        if _good(_worn(sim_info), part, kind):
            used = name
            break
        _log('{} route: part {} (body type {}) not worn as asked afterwards: {}'.format(
            name, part, kind, _short(_worn(sim_info))), 80)
    if used and fill and kind in (UPPER, LOWER):
        _fill_half(sim_info, UPPER + LOWER - kind, before)
    ms = (time.perf_counter() - started) * 1000
    _times.append(ms)
    _log('wore part {} (body type {}) by the {} route in {:.0f} ms'.format(part, kind, used or 'NO', ms), 200)
    if used and note:
        _remember(sim_info, part, kind)
    return used is not None, used, ms


def take_off(sim_info, kind):
    started = time.perf_counter()
    route = _route()
    used = None
    for name, fn in (('appearance', _by_tracker), ('override', _by_override)):
        if (route == 1 and name != 'appearance') or (route == 2 and name != 'override'):
            continue
        try:
            fn(sim_info, body_type=kind)
        except Exception as e:
            _log('{} route taking off body type {} failed: {!r}'.format(name, kind, e), 80)
            continue
        if not _worn(sim_info).get(kind):
            used = name
            break
    ms = (time.perf_counter() - started) * 1000
    _log('took off body type {} by the {} route in {:.0f} ms'.format(kind, used or 'NO', ms), 200)
    return used is not None


def _fill_half(sim_info, kind, before):
    """A top or bottom worn where a full body outfit was: the other half from what was worn before, else from the
    Sim's other outfits, else the first one in the catalog."""
    if _worn(sim_info).get(kind):
        return
    choices = list(before.get(kind, ()))
    try:
        for outfit in sim_info.get_all_outfit_entries():
            if outfit[0] in PLANNABLE:
                choices.extend(_parts_of(sim_info, outfit).get(kind, ()))
    except Exception:
        pass
    if not choices:
        key = 'tops' if kind == UPPER else 'bottoms'
        choices = [p for p, locked in _catalog(sim_info, key, (), False) if not locked][:1]
    for part in choices:
        ok, _, _ = wear(sim_info, part, fill=False)
        if ok:
            _log('the other half: part {} (body type {})'.format(part, kind), 80)
            return


def _short(worn):
    return ', '.join('{}:{}'.format(k, len(v)) for k, v in sorted(worn.items()))


# ---------------------------------------------------------------------------------------------------------------
# The catalog and colours.

def _catalog(sim_info, key, occasions, all_variants):
    cache_key = (sim_info.sim_id, key, tuple(occasions), bool(all_variants))
    if cache_key in _catalogs:
        return _catalogs[cache_key]
    from cas import cas
    rewards = []
    try:
        household = sim_info.household
        for reward in household._reward_inventory.reward_parts or ():
            if not reward.sim_id or reward.sim_id == sim_info.id:
                rewards.append(reward.part_id)
    except Exception:
        rewards = []
    body_types = list(CATEGORIES[key][3])
    found = []
    started = time.perf_counter()
    for tags in ([[t] for t in occasions] if occasions else [[]]):
        try:
            parts = cas.get_catalog_casparts_by_bodytype(sim_info=sim_info._base, include_tags=tags, exclude_tags=[],
                                                         bodytypes=body_types, show_all_variants=bool(all_variants),
                                                         rewards_parts=rewards)
        except Exception as e:
            _log('catalog of {} failed: {!r}'.format(key, e), 60)
            parts = {}
        for part, info in parts.items():
            locked = 0
            try:
                locked = int(info[1] or 0)
            except Exception:
                locked = 0
            found.append((int(part), locked, _tags_of(info)))
    own, other = _preference_tags(sim_info)
    seen = set()
    unique = []
    left_out = 0
    for part, locked, tags in found:
        if part in seen:
            continue
        seen.add(part)
        if other in tags and own not in tags:  # the other clothing preference's only (CAS leaves these out too)
            left_out += 1
            continue
        unique.append((part, locked))
    _log('catalog {} ({}{}): {} parts, {} locked, {} of the other clothing preference left out, {:.0f} ms'.format(
        key, 'occasions {} '.format(list(occasions)) if occasions else '', 'every colour' if all_variants else 'bases',
        len(unique), sum(1 for _, l in unique if l), left_out, (time.perf_counter() - started) * 1000), 120)
    _catalogs[cache_key] = unique
    return unique


def _tags_of(info):
    """The part's tags as ints, from the catalog's (tags, locked tooltip id) (cas_catalog_picker.picker_rows_gen)."""
    try:
        return {int(t) for t in (info[0] or ())}
    except Exception:
        return set()


GENDER_TAGS = {'MALE': 1529, 'FEMALE': 1530}  # GenderAppropriate_Male / _Female (tag tuning), if the tuning is not read


def _preference_tags(sim_info):
    """(own, other): the GenderAppropriate tag of the Sim's clothing preference and the other one. CAS shows clothes
    by clothing preference (client tagtuning TagGenderMapping; the game's own copy is
    GlobalGenderPreferenceTuning.GENDERED_CLOTHING_PREFERENCE_TAGS, read by custom_schedule_outfit_tuning)."""
    try:
        from sims.sim_info_types import Gender
        own = sim_info.clothing_preference_gender
        other = Gender.FEMALE if own == Gender.MALE else Gender.MALE
    except Exception as e:
        _log('clothing preference not read ({!r}): nothing left out by it'.format(e), 20)
        return -1, -2
    try:
        from sims.global_gender_preference_tuning import GlobalGenderPreferenceTuning
        tags = GlobalGenderPreferenceTuning.GENDERED_CLOTHING_PREFERENCE_TAGS
        return int(tags[own]), int(tags[other])
    except Exception:
        return GENDER_TAGS.get(getattr(own, 'name', ''), -1), GENDER_TAGS.get(getattr(other, 'name', ''), -2)


def _all_variants():
    return native.setting_flag('outfit', 'all_variants', False) or _colour_fail >= 2


def _ask_colours(s, part):
    """The colour families of `part` from the game's live service (answer read in tick)."""
    s.colour_base = part
    s.colours = []
    s.query = 0
    if not part or _all_variants():
        return
    ask = _dll('service_query')
    if ask is None:
        return
    payload = json.dumps({'base': str(part), 'simId': str(s.sim_id), 'targetSimId': str(s.sim_id)})
    try:
        s.query = int(ask('GetCatalogColorFamilies', payload) or 0)
        s.query_at = time.monotonic()
    except Exception as e:
        _log('colour query not sent: {!r}'.format(e), 40)
        s.query = 0


def _argb(value):
    try:
        if isinstance(value, bool):
            return 0
        if isinstance(value, (int, float)):
            return int(value) & 0xffffffff
        if isinstance(value, str):
            text = value.strip().lstrip('#')
            return int(text, 16 if not text.isdigit() else 10) & 0xffffffff
        if isinstance(value, (list, tuple)) and value:
            return _argb(value[0])
        if isinstance(value, dict):
            r, g, b = (int(value.get(k, 0)) for k in ('r', 'g', 'b'))
            if max(r, g, b) <= 1:
                r, g, b = r * 255, g * 255, b * 255
            return 0xff000000 | (r & 255) << 16 | (g & 255) << 8 | (b & 255)
    except Exception:
        return 0
    return 0


_ID_KEYS = ('data_id', 'dataid', 'variation_id', 'variationid', 'part_id', 'partid', 'cas_part', 'caspart', 'res_id',
            'resid', 'id', 'colordataid', 'color_data_id')


def _parse_colours(text):
    """[(variant part id, argb, exact)] from the service's answer as JSON, whatever its shape: every object with an
    id and a colour key [guess]. exact: the id came as text (or small enough for a JSON number to hold it)."""
    data = json.loads(text)
    found = []

    def walk(v, depth=0):
        if depth > 6:
            return
        if isinstance(v, dict):
            ident = colour = None
            for k, x in v.items():
                low = k.lower()
                if ident is None and low in _ID_KEYS and not isinstance(x, (dict, list)):
                    ident = x
                if colour is None and ('color' in low or 'colour' in low) and 'id' not in low:
                    colour = x
            if ident is not None:
                try:
                    pid = int(ident)
                except (TypeError, ValueError):
                    pid = 0
                if pid > 0:  # (every EA part id is under 2^32, some under 0xffff; _poll_colours keeps catalog parts)
                    found.append((pid, _argb(colour), isinstance(ident, str) or pid < 2 ** 53))
            for x in v.values():
                walk(x, depth + 1)
        elif isinstance(v, list):
            for x in v:
                walk(x, depth + 1)

    walk(data)
    seen = set()
    out = []
    for pid, colour, exact in found:
        if pid not in seen:
            seen.add(pid)
            out.append((pid, colour or 0xff808080, exact))
    return out


def _poll_colours(s):
    global _colour_fail, _colour_logged
    if not s.query:
        return
    result = _dll('service_result')
    text = result(s.query) if result is not None else None
    if text is None:
        if time.monotonic() - s.query_at > COLOUR_WAIT:
            _log('colour families of {}: no answer in {:.0f} s'.format(s.colour_base, COLOUR_WAIT), 40)
            s.query = 0
            _colour_failed(s)
        return
    s.query = 0
    if _colour_logged < 4:
        _colour_logged += 1
        _log('colour families of {}: answer {}'.format(s.colour_base, text[:1500]), 200)
    try:
        colours = _parse_colours(text) if text and text not in ('null', 'undefined') else []
    except Exception as e:
        _log('colour answer not read: {!r}'.format(e), 40)
        colours = []
    # Only parts of the catalog's every-colour list of this category: drops other ids in the answer, and ids
    # written as JSON numbers that lost digits past 2^53 (64-bit custom content ids).
    known = set(p for p, _ in _catalog(s.sim_info, _key(s), (), True))
    colours = [c for c in colours if c[0] in known]
    if len(colours) < 1:
        _colour_failed(s)
        return
    _colour_fail = 0
    s.colours = [(p, argb) for p, argb, _ in colours[:30]]
    _send()


def _colour_failed(s):
    global _colour_fail
    _colour_fail += 1
    if _colour_fail == 2:
        _log('colour service gave nothing usable twice: every colour listed as its own item from now on', 40)
        _load_tab(s)
        _send()


# ---------------------------------------------------------------------------------------------------------------
# The panel session.

class _Session(object):
    def __init__(self, sim_info, obj, kind):
        self.sim_info = sim_info
        self.sim_id = sim_info.sim_id
        self.obj = obj
        self.obj_id = getattr(obj, 'id', 0) if obj is not None else 0
        self.kind = kind
        self.groups = []      # the category buttons (mirror / front): group keys
        self.group = 0
        self.tabs = []        # the categories over the grid
        self.tab = 0
        self.occasions = ()   # tag ids the container is tagged with (drawers)
        self.parts = []
        self.serial = 0
        self.start = _snapshot(sim_info)
        self.undo = []
        self.redo = []
        self.colours = []
        self.colour_base = 0
        self.query = 0
        self.query_at = 0.0
        self.message = ''
        self.drawer = -1
        self.face = None      # (centre, normal, up, width, height, depth) of the object's front
        self.place = None     # what was sent to outfit_place
        self.outfit = sim_info.get_current_outfit()
        self.opened = time.monotonic()
        self.changes = 0
        self.record = _read_record()  # the persistence check's last change, put back on Cancel


def showing():
    return _session is not None


def _key(s):
    return s.tabs[s.tab] if s.tabs and 0 <= s.tab < len(s.tabs) else 'tops'


def _load_tab(s):
    key = _key(s)
    s.parts = _catalog(s.sim_info, key, s.occasions, _all_variants())
    s.serial += 1
    worn = [p for bt in CATEGORIES[key][3] for p in _worn(s.sim_info).get(bt, ())]
    _ask_colours(s, worn[0] if worn else 0)


def _open(sim, obj, kind, tabs=None, drawer=-1, how='', pulled=False):
    global _session
    sim_info = sim.sim_info
    if _dll('outfit_content') is None or _dll('outfit_place') is None:
        _log('the DLL has no outfit panel (older DLL): {} left to the game'.format(how), 20)
        return False
    if sim_info.get_current_outfit()[0] not in PLANNABLE:
        _log('outfit {} is not one you plan here: {} left to the game'.format(sim_info.get_current_outfit(), how), ALWAYS)
        if pulled:  # no CAS trip follows a pull: say why nothing opens
            _note('The outfit you are wearing can\'t be planned here. Live outfit planning changes Everyday, Formal, '
                  'Athletic, Sleep, Party, Swimwear and Hot or Cold weather outfits.', 'Plan Outfit')
        return False
    if _session is not None and _session.sim_id == sim_info.sim_id and _session.obj_id == getattr(obj, 'id', 0):
        if kind in (RACK, WARDROBE) and _session.kind == kind:
            return True  # the other hand on the same doors / rack (809: both hands pulled the doors at once)
        s = _session  # another drawer of the same dresser: the same session (undo goes on)
        s.kind = kind
    else:
        close(True, 'another panel opened')
        s = _Session(sim_info, obj, kind)
    s.drawer = drawer
    if kind == DRAWER or (kind in (RACK, WARDROBE) and len(tabs or ()) < 2):
        s.groups, s.group = [], 0
        s.tabs = [c for g in tabs for c in GROUPS[g]] if tabs else list(GROUPS['tops'])
        s.tab = 0
        s.occasions = _occasions_of(obj)
    elif kind in (RACK, WARDROBE):
        # What it holds as the mirror's categories down the left (no hair: a rack or wardrobe holds clothes).
        s.groups = [g for g in MIRROR_GROUPS if g in tabs]
        s.group = s.groups.index('tops') if 'tops' in s.groups else 0
        s.tabs = list(GROUPS[s.groups[s.group]])
        s.tab = 0
        s.occasions = _occasions_of(obj)
    else:
        s.groups = list(MIRROR_GROUPS)
        s.group = s.groups.index('tops')
        s.tabs = list(GROUPS[s.groups[s.group]])
        s.tab = 0
        s.occasions = ()
    s.message = ''
    _session = s
    try:
        _load_tab(s)
        _place(s)
        _send()
    except Exception as e:
        _session = None
        _clear_panel()
        _log('panel not opened ({!r}): {} left to the game'.format(e, how), 20)
        return False
    if kind == DRAWER:
        _sound(obj, 0)
    elif kind == WARDROBE:
        _sound(obj, 0, DOOR_SOUNDS)
    _log('panel opened ({}) on {}: {}{}'.format({MIRROR: 'mirror', DRAWER: 'drawer', FRONT: 'in front of you',
                                                 RACK: 'rack', WARDROBE: 'wardrobe doors'}[kind],
                                                obj, how, ' drawer {}'.format(drawer) if drawer >= 0 else ''), 200)
    return True


def close(keep=True, why=''):
    global _session, _waiting
    s = _session
    _waiting = None
    if s is None:
        return
    _session = None
    if not keep:
        try:
            _restore(s.sim_info, s.start)
            _write_record(s.record)
        except Exception as e:
            _log('cancel: the outfit not put back: {!r}'.format(e), 40)
    _clear_panel()
    if s.kind == DRAWER and s.obj is not None:
        _sound(s.obj, 1)
    elif s.kind == WARDROBE and s.obj is not None:
        _sound(s.obj, 1, DOOR_SOUNDS)
    _log('panel closed ({}; {}, {} changes)'.format(why, 'kept' if keep else 'put back as it was', s.changes), 200)


def _clear_panel():
    place = _dll('outfit_place')
    content = _dll('outfit_content')
    if place is not None:
        place(0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0)
    if content is not None:
        content(b'')


def _note(text, title):
    """A notification from us (in first person it shows on the notification card)."""
    try:
        from sims4.localization import LocalizationHelperTuning
        from ui.ui_dialog_notification import UiDialogNotification
        dialog = UiDialogNotification.TunableFactory().default(
            _fp_sim(), text=lambda *_, **__: LocalizationHelperTuning.get_raw_text(text),
            title=lambda *_, **__: LocalizationHelperTuning.get_raw_text(title))
        dialog.show_dialog()
    except Exception as e:
        _log('notification not shown: {!r}'.format(e), 20)


def _send():
    s = _session
    content = _dll('outfit_content')
    if content is None:
        return
    if s is None:
        content(b'')
        return
    key = _key(s)
    name, _icon, _sel, body_types, removable = CATEGORIES[key]
    worn = _worn(s.sim_info)
    worn_here = set(p for bt in body_types for p in worn.get(bt, ()))
    flags = (1 if s.undo else 0) | (2 if s.redo else 0) | (4 if removable and worn_here else 0) | \
            (8 if _all_variants() else 0) | (16 if s.kind == DRAWER else 0)
    hint = next((i for i, (p, _) in enumerate(s.parts) if p in worn_here), -1)
    gender = _gender_letter(s.sim_info)
    records = ['outfit\x1f{}\x1f{}\x1f{}\x1f{{"hash": {}}}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}'.format(
        MIRROR if s.kind in (RACK, WARDROBE) else s.kind, gender, flags, name, s.tab, s.serial, hint, s.group if s.groups else -1, _raw(s.message))]
    for g in s.groups:
        gname, gicon, gsel = _group_look(g)
        records.append('group\x1f{}\x1f{{"hash": {}}}\x1f{}\x1f{}'.format(g, gname, gicon, gsel))
    for t in s.tabs:
        tname, ticon, tsel = CATEGORIES[t][:3]
        records.append('tab\x1f{}\x1f{{"hash": {}}}\x1f{}\x1f{}'.format(t, tname, ticon, tsel))
    for part, locked in s.parts:
        records.append('part\x1f{}\x1f{}\x1f{}'.format(
            part, (1 if part in worn_here else 0) | (2 if locked else 0), '{{"hash": {}}}'.format(locked) if locked else ''))
    for part, argb in s.colours:
        records.append('colour\x1f{}\x1f{}\x1f{}'.format(part, argb, 1 if part in worn_here else 0))
    content('\x1e'.join(records).encode('utf-8'))


def _gender_letter(sim_info):
    """'f' or 'm', as the CAS thumbnails' names end (CASCatalogProduct.LoadThumbnail)."""
    try:
        from sims.sim_info_types import Gender
        return 'f' if sim_info.gender == Gender.FEMALE else 'm'
    except Exception:
        return 'f' if 'FEMALE' in str(getattr(sim_info, 'gender', '')) else 'm'


def _group_look(g):
    if g in GROUP_LOOK:
        return GROUP_LOOK[g]
    return CATEGORIES[GROUPS[g][0]][:3]


def _raw(text):
    if not text:
        return ''
    return json.dumps({'raw': text})


def control(code):
    """controls.py: 8600000 + code from the panel (hud.cpp drawOutfit) or the debug menu."""
    try:
        if code == C_PROBE:
            probe()
            return
        s = _session
        if s is None:
            return
        si = s.sim_info
        if code == C_CANCEL:
            close(False, 'Cancel')
        elif code == C_DONE:
            close(True, 'Done')
        elif code == C_UNDO and s.undo:
            s.redo.append(_snapshot(si))
            _restore(si, s.undo.pop())
            _after_change(s)
        elif code == C_REDO and s.redo:
            s.undo.append(_snapshot(si))
            _restore(si, s.redo.pop())
            _after_change(s)
        elif code == C_REMOVE:
            key = _key(s)
            if CATEGORIES[key][4]:
                snap = _snapshot(si)
                done = False
                for bt in CATEGORIES[key][3]:
                    if _worn(si).get(bt):
                        done = take_off(si, bt) or done
                if done:
                    _push_undo(s, snap)
                _after_change(s)
        elif C_GROUP <= code < C_GROUP + 100 and s.groups:
            g = code - C_GROUP
            if 0 <= g < len(s.groups) and g != s.group:
                s.group, s.tabs, s.tab = g, list(GROUPS[s.groups[g]]), 0
                _load_tab(s)
                _send()
        elif C_TAB <= code < C_TAB + 100:
            t = code - C_TAB
            if 0 <= t < len(s.tabs) and t != s.tab:
                s.tab = t
                _load_tab(s)
                _send()
        elif C_PART <= code < C_COLOUR:
            i = code - C_PART
            if 0 <= i < len(s.parts):
                part, locked = s.parts[i]
                if locked:
                    return
                if not _all_variants():
                    _ask_colours(s, part)  # (the old part's colours go before the panel is sent again)
                _pick(s, part)
        elif C_COLOUR <= code < C_COLOUR + 1000:
            i = code - C_COLOUR
            if 0 <= i < len(s.colours) and not _pick(s, s.colours[i][0]):
                _colour_failed(s)  # a colour the service named that can't be worn
    except Exception as e:
        _log('control {} failed: {!r}'.format(code, e), 60)


def _pick(s, part):
    """Wears `part` (an undo step); False when it could not be worn."""
    si = s.sim_info
    if part in _worn(si).get(_body_type(part), ()):
        return True
    snap = _snapshot(si)
    ok, route, ms = wear(si, part)
    if ok:
        _push_undo(s, snap)
        s.message = ''
    else:
        s.message = "That can't be worn with this outfit"
    _after_change(s)
    return ok


def _push_undo(s, snap):
    s.undo.append(snap)
    del s.undo[:-UNDO_STEPS]
    s.redo = []
    s.changes += 1


def _after_change(s):
    _send()


# ---------------------------------------------------------------------------------------------------------------
# Where the panel goes.

def _face(obj):
    """(centre, normal, up, width, height, depth) of the object's front (objmenu's model box), cached 5 s."""
    now = time.monotonic()
    cached = _faces.get(obj.id)
    if cached is not None and now - cached[0] < 5.0:
        return cached[1]
    from sims4vr import objmenu
    centre, normal, up, width, height, _video = objmenu._front(obj)
    depth = 0.45
    box = None
    try:
        box, _screen = objmenu._model_box(obj)
        if box is not None:
            depth = (box[5] - box[2]) * (getattr(obj, 'scale', 1.0) or 1.0)
    except Exception:
        pass
    visible = _visible_front(obj)
    if visible is not None:
        centre, width, height, depth, front_z = visible
        key = getattr(getattr(obj, 'definition', None), 'id', None)
        if key not in _front_noted:
            _front_noted.add(key)
            _log('front of {}: its visible shape, {:.2f} x {:.2f} m, {:.2f} m deep{}'.format(
                obj, width, height, depth, '; the model box reaches {:.2f} m further out (floor shadow)'.format(
                    box[5] - front_z) if box is not None else ''), 300)
    n = math.hypot(normal[0], normal[2]) or 1.0
    face = (centre, (normal[0] / n, 0.0, normal[2] / n), (0.0, 1.0, 0.0), width, height, depth)
    _faces[obj.id] = (now, face)
    return face


def _visible_front(obj):
    """(centre, width, height, depth, front z in model space) of what your hands meet of the object: its model's
    visible shape (collide.py's pieces, the shape that stops your hands), not the model box (objmenu), which reaches
    out to the floor shadow (user 2026-10-31: pulling a drawer did nothing, the hand never got near that front).
    None if the shape is unknown (the box is used then)."""
    try:
        from sims4vr import collide
        prisms = collide._model_shape(obj)
    except Exception:
        return None
    if not prisms:
        return None
    pts = []
    bottom = top = None
    for corners, y0, y1, _leaves in prisms:
        bottom = y0 if bottom is None else min(bottom, y0)
        top = y1 if top is None else max(top, y1)
        if y1 > 0.25:  # feet, plinths and rugs below that are not the front
            pts.extend(corners)
    if not pts or top - bottom < 0.2:
        return None
    min_x, max_x = min(x for x, _z in pts), max(x for x, _z in pts)
    min_z, max_z = min(z for _x, z in pts), max(z for _x, z in pts)
    scale = getattr(obj, 'scale', 1.0) or 1.0
    try:
        from sims4vr import objmenu
        tr = obj.transform
        p, o = tr.translation, tr.orientation
        r = objmenu._rotate((o.x, o.y, o.z, o.w), ((min_x + max_x) / 2 * scale, (bottom + top) / 2 * scale,
                                                   max_z * scale))
    except Exception:
        return None
    return ((p.x + r[0], p.y + r[1], p.z + r[2]), (max_x - min_x) * scale, (top - bottom) * scale,
            (max_z - min_z) * scale, max_z)


def _drawers(face, wardrobe):
    """The drawers (shelves) of a front, top to bottom: [(bottom y, top y)]."""
    centre, _n, _u, _w, height, _d = face
    base = centre[1] - height / 2
    low, high = base + 0.08, base + height - 0.04
    if wardrobe:
        low, high = max(low, base + 0.3), min(high, base + 1.75)
    count = max(1, min(5, int(round((high - low) / (0.32 if wardrobe else 0.26)))))
    step = (high - low) / count
    return [(high - (i + 1) * step, high - i * step) for i in range(count)]


def _place(s):
    place = _dll('outfit_place')
    if place is None:
        return
    if s.kind in (MIRROR, DRAWER, RACK, WARDROBE) and s.obj is not None:
        try:
            s.face = _face(s.obj)
        except Exception as e:
            _log('the front of {} not read ({!r}): the panel in front of you'.format(s.obj, e), 20)
            s.face = None
    if s.kind in (MIRROR, RACK, WARDROBE) and s.face is not None:
        c, n, u, w, h, d = s.face
        args = (MIRROR, c[0], c[1], c[2], n[0], n[1], n[2], u[0], u[1], u[2], w, h, 0.0)
    elif s.kind == DRAWER and s.face is not None:
        c, n, u, w, h, d = s.face
        bands = _drawers(s.face, h > 1.3)
        band = bands[max(0, min(len(bands) - 1, s.drawer))]
        y = band[0] + 0.03
        args = (DRAWER, c[0], y, c[2], n[0], n[1], n[2], u[0], u[1], u[2], max(0.2, w - 0.08), band[1] - band[0],
                max(0.2, min(d * 0.9, 0.6)))
    else:
        snap = native.hands()
        if snap is None:
            return
        head, forward = snap[0], snap[1]
        f = math.hypot(forward[0], forward[2]) or 1.0
        fx, fz = forward[0] / f, forward[2] / f
        at = (head[0] + fx * 0.65, head[1] - 0.25, head[2] + fz * 0.65)
        args = (FRONT, at[0], at[1], at[2], -fx, 0.0, -fz, 0.0, 1.0, 0.0, 0.55, 0.8, 0.0)
        s.face = (at, (-fx, 0.0, -fz), (0.0, 1.0, 0.0), 0.55, 0.8, 0.0)
    s.place = args
    place(*args)


# ---------------------------------------------------------------------------------------------------------------
# Objects: mirrors, dressers, wardrobes.

_MIRROR_WORDS = ('mirror',)
# Racks (user 2026-10-31: "a wall mounted coat rack thing ... that and other items in the future"): what hangs or
# stands rather than sits in drawers. By tuning name (str(obj); the game has no model names at run time, its
# definition.name is None, as lightswitch.py found): the hanging-clothes dressers (object_dresser_Hang_Clothes,
# object_dresser_Hang_Lrg: Dream Home Decorator's wall racks and others), the shoe racks of the shoes-off rule
# (Object_shoeRemovalSign_Rack; not the sign, Object_shoeRemovalSign), and a mod's racks named so.
_RACK_WORDS = ('dresser_hang', 'shoeremovalsign_rack', 'coatrack', 'coat_rack', 'coathanger', 'coat_hanger',
               'coathook', 'coat_hook', 'clothesrack', 'clothes_rack', 'clothingrack', 'clothing_rack', 'garmentrack',
               'garment_rack', 'clotheshanger', 'clothes_hanger', 'hatrack', 'hat_rack', 'hatstand', 'hat_stand',
               'shoerack', 'shoe_rack', 'shoeshelf', 'shoe_shelf', 'coatstand', 'coat_stand')
# EA's racks under decoration or shelf tunings, by definition (OBJD) id: re/tuning/objects.tsv (1.121). A new one:
# grab it with grip and trigger (the log names its tuning and definition id, "not a dresser ...") and add it here.
_RACK_RANGES = (
    (0xA512, 0xA513), (0xA515, 0xA515),  # sculptWallGENCoatRack_01 (wall coat rack)
    (0xA5C7, 0xA5C9),  # sculptFloorGENCoatRack_01 (floor coat rack)
    (0xB647, 0xB64C), (0xB653, 0xB658), (0xB65E, 0xB663), (0xB7DF, 0xB7E4),  # sculptWallGEN{FC,FF,MC,MF}Hanger_01
    (0x786AB, 0x786B4),  # sculptWall_SP71CK007coatrack
    (0x78694, 0x78698), (0x786A5, 0x786A9),  # sculptWall_SP71CK007hatrack
    (0x4B2A9, 0x4B2AE),  # sculptFloor_EP12THRIFTclothes
    (0x2F5ED, 0x2F5F5),  # shelfFloor2x1_EP05WOODcoatHanger
    (0x5388F, 0x5389E),  # shelfFloor2x1_EP14CFclothes
    (0x6C4D0, 0x6C4DB),  # shelfFloor2x1_SP55KIDclothingRack
    (0x3729E, 0x372A5),  # dresserTall2x1_SP15GENclothingRack (a dresser: one rail, no drawers)
    (0x32C48, 0x32C51),  # dresserTall1x1_EP06NBshoeRackTileable3Tile (a dresser: shoe shelves)
)
_RACK_DEFINITIONS = frozenset(i for first, last in _RACK_RANGES for i in range(first, last + 1))
_DRESSER_WORDS = ('dresser', 'wardrobe', 'closet', 'armoire', 'drawers', 'chestofdrawer', 'chest_of_drawer')


def _kind_of(obj, interaction=None):
    """MIRROR, DRAWER (dressers, wardrobes), RACK (coat, clothes, hat, shoe racks) or 0."""
    if obj is None:
        return 0
    if interaction is not None and 'mirror' in type(interaction).__name__.lower():
        return MIRROR
    key = getattr(getattr(obj, 'definition', None), 'id', None)
    if key in _kinds:
        return _kinds[key]
    kind = 0
    try:
        text = (type(obj).__name__ + ' ' + str(obj)).lower()
        if key in _RACK_DEFINITIONS:
            kind = RACK
        elif any(w in text for w in _MIRROR_WORDS):
            kind = MIRROR
        elif any(w in text for w in _RACK_WORDS):
            kind = RACK
        elif any(w in text for w in _DRESSER_WORDS):
            kind = DRAWER
        if not kind:
            # The game's own Plan Outfit on it (a mod's dresser named otherwise): a mirror's or a dresser's.
            for affordance in obj.super_affordances():
                name = getattr(affordance, '__name__', '').lower()
                if 'planoutfit' in name or 'plan_outfit' in name:
                    kind = MIRROR if 'mirror' in name else DRAWER
                    break
    except Exception:
        kind = 0
    if key is not None:
        _kinds[key] = kind
    return kind


def _find_interaction(sim, interaction_id):
    if sim is None or not interaction_id:
        return None
    try:
        found = sim.queue.find_interaction_by_id(interaction_id)
        if found is not None:
            return found
    except Exception:
        pass
    try:
        for si in sim.si_state:
            if si.id == interaction_id:
                return si
    except Exception:
        pass
    return None


def _nearest(sim, kinds, within=2.5):
    best, best_d = None, within
    try:
        p = sim.position
    except Exception:
        return None
    for obj_id, kind in _lot:
        if kind not in kinds:
            continue
        obj = _object(obj_id)
        if obj is None:
            continue
        d = math.hypot(obj.position.x - p.x, obj.position.z - p.z)
        if d < best_d:
            best, best_d = obj, d
    return best


def _object(obj_id):
    import services
    manager = services.object_manager()
    return manager.get(obj_id) if manager is not None else None


def _fp_sim():
    from sims4vr import body
    fp = body.following()
    if not fp:
        return None
    return _object(fp)


def _scan(force=False):
    """The dressers, wardrobes and mirrors on the lot (their kind cached per definition)."""
    global _lot, _scanned, _scan_zone
    import services
    now = time.monotonic()
    zone = services.current_zone_id()
    if not force and zone == _scan_zone and now - _scanned < SCAN_EVERY:
        return
    _scanned, _scan_zone = now, zone
    found = []
    started = time.perf_counter()
    try:
        for obj in list(services.object_manager().values()):
            if getattr(obj, 'is_sim', False):
                continue
            kind = _kind_of(obj)
            if kind:
                found.append((obj.id, kind))
    except Exception as e:
        _log('scan failed: {!r}'.format(e), 20)
    if len(found) != len(_lot):
        _log('{} dressers / wardrobes / racks / mirrors on the lot ({:.0f} ms)'.format(
            len(found), (time.perf_counter() - started) * 1000), 60)
    _lot = found


def _box_distance(obj, p):
    """m from the point p to the object's model box (objmenu's MODL bounds, turned and scaled as placed), or None."""
    from sims4vr import objmenu
    box, _screen = objmenu._model_box(obj)
    if box is None:
        return None
    tr = obj.transform
    t, o = tr.translation, tr.orientation
    scale = getattr(obj, 'scale', 1.0) or 1.0
    q = objmenu._rotate((-o.x, -o.y, -o.z, o.w), (p[0] - t.x, p[1] - t.y, p[2] - t.z))
    q = [v / scale for v in q]
    d = [max(box[i] - q[i], 0.0, q[i] - box[i + 3]) for i in range(3)]
    return math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * scale


def _note_unknown(snap):
    """A hand closing (grip and trigger) on something that is not a dresser, wardrobe, rack or mirror: what it is
    (tuning, definition id) in the log, once per definition, so an item you grab and mark can be added as a rack
    (user 2026-10-31: "that and other items in the future")."""
    import services
    for side in (0, 1):
        hand = snap[3 + side]
        closed = bool(hand[0] and hand[3] >= 0.5 and hand[4] >= 0.5 and not hand[5])
        was, _closed[side] = _closed[side], closed
        if not closed or was or _grab[side] is not None or len(_unknown_noted) >= 40:
            continue
        p = hand[1]
        near = []
        for obj in list(services.object_manager().values()):
            try:
                if not getattr(obj, 'is_sim', False):
                    flat = math.hypot(obj.position.x - p[0], obj.position.z - p[2])
                    if flat < 1.2:
                        near.append((flat, obj))
            except Exception:
                continue
        best, best_d = None, 0.25
        for _flat, obj in sorted(near, key=lambda fo: fo[0])[:8]:  # a model read each at most: no hitch
            try:
                d = _box_distance(obj, p)
            except Exception:
                continue
            if d is not None and d < best_d:
                best, best_d = obj, d
        if best is None or _kind_of(best):
            continue
        key = getattr(getattr(best, 'definition', None), 'id', None)
        if key in _unknown_noted:
            continue
        _unknown_noted.add(key)
        _log('hand {} closed on {} (definition {:#x}), {:.2f} m from its box: not a dresser, wardrobe, rack or '
             'mirror (a mark now and it can be added)'.format('LR'[side], best, key or 0, best_d), ALWAYS)


# ---------------------------------------------------------------------------------------------------------------
# The game's Plan Outfit: its trip to CAS.

_PLAN_WORDS = ('planoutfit', 'plan_outfit', 'changeappearance', 'change_appearance')
_CAS_COMMANDS = ('sims.modify_in_cas_with_householdid', 'sims.modify_in_cas')
_passing = False     # a CAS command the DoCommand hook left to the game is running (client_cheat lets it through)


def _name_of(interaction):
    return type(interaction).__name__ if interaction is not None else 'no interaction'


def _plans(interaction):
    """A Plan Outfit / Change Appearance interaction (the testing cheat's Modify in CAS keeps the game's CAS)."""
    name = _name_of(interaction).lower()
    return 'cheat' not in name and any(w in name for w in _PLAN_WORDS)


def _do_command(original, self, *args, **kwargs):
    """DoCommand._do_behavior: Plan Outfit's command (sims.modify_in_cas_with_householdId) not run, our panel instead.
    Here the interaction and its object are known for sure."""
    global _passing
    command = (getattr(self, 'command', None) or '').strip()
    if command.lower() in _CAS_COMMANDS:
        interaction = getattr(self, 'interaction', None)
        try:
            if not hooks.in_first_person():
                _log('"{}" of {}: left to the game (not in first person)'.format(command, _name_of(interaction)), ALWAYS)
            elif not _plans(interaction):
                _log('"{}" of {}: left to the game (not a Plan Outfit)'.format(command, _name_of(interaction)), ALWAYS)
            elif _take_over(interaction, '{} ({})'.format(_name_of(interaction), command)):
                _log('"{}" of {}: taken over'.format(command, _name_of(interaction)), ALWAYS)
                return True
            else:
                _log('"{}" of {}: left to the game ({})'.format(command, _name_of(interaction),
                                                                _why or 'the panel did not open, see above'), ALWAYS)
        except Exception as e:
            _log('"{}" of {} not taken over ({!r}): the game\'s CAS'.format(command, _name_of(interaction), e), ALWAYS)
        _passing = True
        try:
            return original(self, *args, **kwargs)
        finally:
            _passing = False
    return original(self, *args, **kwargs)


def _client_cheat(original, s, context=None, *args, **kwargs):
    """The client's CAS trip ("sims.exit2caswithhouseholdid A B" / "sims.exit2cas A B C"): a Plan Outfit the
    DoCommand hook did not see (another route to the command) is taken over here."""
    if not _passing and isinstance(s, str) and (s.startswith('sims.exit2caswithhouseholdid ') or
                                                s.startswith('sims.exit2cas ')):
        try:
            if not hooks.in_first_person():
                _log('game command "{}": left to the game (not in first person)'.format(s), ALWAYS)
            elif _from_game(s):
                _log('game command "{}": taken over'.format(s), ALWAYS)
                return None
            else:
                _log('game command "{}": left to the game ({})'.format(s, _why or 'the panel did not open, see above'),
                     ALWAYS)
        except Exception as e:
            _log('game command "{}" not taken over ({!r}): the game\'s CAS'.format(s, e), ALWAYS)
    return original(s, context, *args, **kwargs)


def _from_game(command):
    """client_cheat: the interaction that sent it, from the callers (the DoCommand element, or the command's
    _interaction_id); only a Plan Outfit / Change Appearance one is taken over."""
    global _why
    _why = ''
    words = command.split()
    plain = (words[0] == 'sims.exit2caswithhouseholdid' and len(words) == 3) or \
            (words[0] == 'sims.exit2cas' and len(words) == 4)
    if not plain:
        _why = 'not a plain Plan Outfit / Change Appearance'
        return False
    sim = _fp_sim()
    if sim is None or str(sim.sim_id) != words[1]:
        _why = 'not the Sim you are in ({})'.format(getattr(sim, 'sim_id', None))
        return False
    interaction = None
    interaction_id = None
    frame = hooks.caller_frame(1)
    for _ in range(25):
        if frame is None:
            break
        here = frame.f_locals
        element = here.get('self')
        if interaction is None and element is not None and type(element).__name__ == 'DoCommand':
            interaction = getattr(element, 'interaction', None)
        if interaction_id is None and here.get('_interaction_id') is not None:
            interaction_id = here.get('_interaction_id')
        frame = frame.f_back
    if interaction is None and interaction_id is not None:
        interaction = _find_interaction(sim, int(interaction_id))
    if interaction is None:
        _why = 'no interaction found that sent it'
        return False
    if not _plans(interaction):
        _why = '{} is not a Plan Outfit'.format(_name_of(interaction))
        return False
    return _take_over(interaction, '{} ({})'.format(_name_of(interaction), command))


def _take_over(interaction, how):
    """Our panel (or, on a dresser, the wait for a pulled drawer) for this Plan Outfit; False: the game's CAS."""
    global _waiting, _why
    _why = ''
    if not _on():
        _why = 'Live outfit planning is off'
        return False
    sim = _fp_sim()
    actor = getattr(interaction, 'sim', None)
    if sim is None or actor is None or actor.sim_id != sim.sim_id:
        _why = 'not the Sim you are in'
        return False
    obj = getattr(interaction, 'target', None)
    if obj is sim or getattr(obj, 'is_sim', False):
        obj = None
    _scan()
    kind = _kind_of(obj, interaction)
    if obj is None or not kind:
        near = _nearest(sim, (MIRROR, DRAWER, RACK))
        if near is not None:
            obj, kind = near, _kind_of(near)
    if kind == DRAWER and native.setting_flag('outfit', 'drawers', True):
        if sim.sim_info.get_current_outfit()[0] not in PLANNABLE or _dll('outfit_place') is None:
            _why = 'outfit {} is not planned here'.format(sim.sim_info.get_current_outfit())
            return False
        _waiting = (obj.id, sim.sim_id)
        _start_gestures()
        native.haptic(1, 0.2, 0.05)
        if _wardrobe(obj):
            _note('Grab its doors with grip and trigger and pull them toward you. Your outfit shows on its front.',
                  'Plan Outfit')
            _log('Plan Outfit on {}: waits for you to open its doors ({})'.format(obj, how), ALWAYS)
        else:
            _note('Grab a drawer front with grip and trigger and pull it toward you. The drawer that slides out holds '
                  'that part of your outfit.', 'Plan Outfit')
            _log('Plan Outfit on {}: waits for you to pull a drawer ({})'.format(obj, how), ALWAYS)
        return True
    if kind == DRAWER:
        kind = MIRROR  # drawers off: the panel on the dresser's front
    return _open(sim, obj, kind or FRONT, tabs=_layout(obj, 1)[0] if kind == RACK else None, how=how)


# ---------------------------------------------------------------------------------------------------------------
# Hands: pulling a drawer out, pushing it back.

def _start_gestures():
    global _gesture_alarm
    if _gesture_alarm is not None:
        return
    import alarms
    import clock
    _gesture_alarm = alarms.add_alarm_real_time(_owner, clock.interval_in_real_seconds(GESTURE_TICK), _gesture_tick,
                                                repeating=True)


def _stop_gestures():
    global _gesture_alarm
    _grab[0] = _grab[1] = None
    if _gesture_alarm is not None:
        try:
            import alarms
            alarms.cancel_alarm(_gesture_alarm)
        except Exception:
            pass
        _gesture_alarm = None


def _candidates(head):
    """Dressers / wardrobes your hands can reach now: [(object, face)]."""
    out = []
    always = native.setting_flag('outfit', 'always', True)
    for obj_id, kind in _lot:
        if kind not in (DRAWER, RACK):
            continue
        if kind == DRAWER and not always and (_waiting is None or _waiting[0] != obj_id) and \
                (_session is None or _session.obj_id != obj_id):
            continue
        obj = _object(obj_id)
        if obj is None:
            continue
        try:
            if math.hypot(obj.position.x - head[0], obj.position.z - head[2]) > REACH + 1.0:
                continue
            face = _face(obj)
        except Exception:
            continue
        c = face[0]
        if math.hypot(c[0] - head[0], c[2] - head[2]) < REACH:
            out.append((obj, face))
    return out


def _pull():
    return max(0.05, _setting_number('drawer_pull', DRAWER_PULL))


def _gesture_tick(_handle):
    try:
        snap = native.hands()
        sim = _fp_sim()
        if snap is None or sim is None or not hooks.in_first_person() or not _on():
            _stop_gestures()
            return
        head = snap[0]
        near = _candidates(head)
        s = _session
        drawer_open = s is not None and s.kind == DRAWER
        if not near and not drawer_open:
            _stop_gestures()
            return
        for side in (0, 1):
            hand = snap[3 + side]
            g = _grab[side]
            if not hand[0] or hand[3] < 0.35 or hand[4] < 0.35 or hand[5]:
                if g is None and hand[0] and hand[3] >= 0.5 and not hand[5]:
                    _no_hold(side, hand[1], near, 'gripped without the trigger (both take hold)', open_now=s is not None)
                if g is not None and not g['push']:
                    _log('hand {} let go of the drawer front after pulling {:.2f} of {:.2f} m{}'.format(
                        'LR'[side], g['most'], _pull(), ' (it held an object)' if hand[5] else ''), 300)
                _grab[side] = None
                continue
            p = hand[1]
            if g is None:
                if hand[3] < 0.5 or hand[4] < 0.5:
                    continue
                _grab[side] = _take_hold(side, p, head, near, s)
                continue
            n = g['n']  # (x, z) of the front's normal
            rel = (p[0] - head[0]) * n[0] + (p[2] - head[2]) * n[1]
            if g['push']:
                if g['rel'] - rel >= _pull() * 0.6:
                    native.haptic(side, 0.3, 0.05)
                    _grab[side] = None
                    close(True, 'drawer pushed back')
                continue
            g['most'] = max(g['most'], rel - g['rel'])
            if rel - g['rel'] >= _pull():
                native.haptic(side, 0.35, 0.05)
                _grab[side] = None
                obj = _object(g['obj'])
                opens = g.get('opens', DRAWER)
                if obj is not None and opens in (RACK, WARDROBE):
                    _open(sim, obj, opens, tabs=_layout(obj, 1)[0], how='{} pulled by hand {}'.format(
                        'rack' if opens == RACK else 'wardrobe doors', 'LR'[side]), pulled=True)
                elif obj is not None:
                    tabs = _layout(obj, g['bands'])[g['band']]
                    _open(sim, obj, DRAWER, tabs=tabs, drawer=g['band'], how='drawer pulled by hand {}'.format('LR'[side]),
                          pulled=True)
    except Exception as e:
        _stop_gestures()
        _log('drawer hands failed: {!r}'.format(e), 40)


def _take_hold(side, p, head, near, s):
    if s is not None and s.kind == DRAWER and s.place is not None:
        # The open drawer's front edge: grab it and push it back to close.
        _k, cx, cy, cz, nx, _ny, nz, _ux, _uy, _uz, w, _h, depth = s.place
        along = (p[0] - cx) * nx + (p[2] - cz) * nz
        lat = abs((p[0] - cx) * -nz + (p[2] - cz) * nx)
        if depth - 0.15 < along < depth + 0.12 and lat < w / 2 + 0.05 and abs(p[1] - cy) < 0.15:
            native.haptic(side, 0.15, 0.04)
            return {'push': True, 'n': (nx, nz), 'rel': (p[0] - head[0]) * nx + (p[2] - head[2]) * nz}
    for obj, face in near:
        c, n, _u, w, h, _d = face
        along = (p[0] - c[0]) * n[0] + (p[2] - c[2]) * n[2]
        lat = abs((p[0] - c[0]) * -n[2] + (p[2] - c[2]) * n[0])
        if not (-FRONT_BEHIND < along < FRONT_NEAR) or lat > w / 2 + 0.08:
            continue
        opens, bands = _bands(obj, face)
        band = next((i for i, (lo, hi) in enumerate(bands) if lo - 0.05 <= p[1] <= hi + 0.05), -1)
        if band < 0:
            continue
        native.haptic(side, 0.15, 0.04)
        rel = (p[0] - head[0]) * n[0] + (p[2] - head[2]) * n[2]
        what = {RACK: 'the rack', WARDROBE: 'the doors of'}.get(opens) or 'drawer {} of {} of'.format(band + 1, len(bands))
        _log('hand {} holds {} {}: pull {:.2f} m toward you to open {}'.format(
            'LR'[side], what, obj, _pull(), 'them' if opens == WARDROBE else 'it'), 300)
        return {'push': False, 'obj': obj.id, 'band': band, 'bands': len(bands), 'n': (n[0], n[2]),
                'rel': rel, 'most': 0.0, 'opens': opens}
    _no_hold(side, p, near, open_now=s is not None)
    return None


def _bands(obj, face):
    """(how it opens, [(bottom y, top y)] where a hand takes hold, top to bottom): a rack as one piece (RACK), a
    wardrobe's doors as one (WARDROBE: a dresser front taller than WARDROBE_HEIGHT, from 30 cm up), else a dresser's
    drawers (DRAWER)."""
    c, _n, _u, _w, h, _d = face
    base = c[1] - h / 2
    if _kind_of(obj) == RACK:
        return RACK, [(base, base + h)]
    if h > WARDROBE_HEIGHT:
        return WARDROBE, [(base + 0.3, base + h)]
    return DRAWER, _drawers(face, False)


def _wardrobe(obj):
    """A dresser that opens by its doors (front taller than WARDROBE_HEIGHT)."""
    try:
        return _kind_of(obj) == DRAWER and _face(obj)[4] > WARDROBE_HEIGHT
    except Exception:
        return False


def _no_hold(side, p, near, how='closed', open_now=False):
    """Log (a few lines, 2 s apart) why a closed hand at a dresser took hold of nothing. Not while a panel is open
    (809: a hand gripping at the open panel used up all 30 lines)."""
    now = time.monotonic()
    if _hold_notes[0] >= 30 or now - _hold_notes[1] < 2.0 or not near or open_now:
        return
    obj, face = min(near, key=lambda of: math.hypot(of[1][0][0] - p[0], of[1][0][2] - p[2]))
    c, n, _u, w, h, _d = face
    along = (p[0] - c[0]) * n[0] + (p[2] - c[2]) * n[2]
    lat = abs((p[0] - c[0]) * -n[2] + (p[2] - c[2]) * n[0])
    opens, bands = _bands(obj, face)
    _hold_notes[0] += 1
    _hold_notes[1] = now
    _log('hand {} {} at {} but holds nothing: {:.2f} m out from its front (takes hold {:.2f} to {:.2f}), {:.2f} m '
         'from its middle (up to {:.2f}), {:.2f} m high ({} {:.2f} to {:.2f})'.format(
             'LR'[side], how, obj, along, -FRONT_BEHIND, FRONT_NEAR, lat, w / 2 + 0.08, p[1] - (c[1] - h / 2),
             {RACK: 'rack', WARDROBE: 'doors'}.get(opens, 'drawers'), bands[-1][0] - (c[1] - h / 2),
             bands[0][1] - (c[1] - h / 2)), 1000)


def _layout(obj, count):
    """What each drawer holds, top to bottom: DRAWER_LAYOUTS, or the container's tagged kinds spread over them."""
    tags = _tags_for(obj)
    kinds = [k for k in DRAWER_KINDS if k in tags]
    count = max(1, min(5, count))
    if not kinds:
        return list(DRAWER_LAYOUTS[count])
    used = min(count, len(kinds))
    per = int(math.ceil(len(kinds) / float(used)))
    layout = [tuple(kinds[i * per:(i + 1) * per]) for i in range(used)]
    layout = [l for l in layout if l] or [tuple(kinds)]
    while len(layout) < count:  # more drawers than kinds stored: the lower ones hold the last kind too
        layout.append(layout[-1])
    return layout


def _sound(obj, which, sounds=DRAWER_SOUNDS):
    """The open (0) or close (1) sound at the object, 3D at its place: a drawer's, or a wardrobe's doors (DOOR_SOUNDS)."""
    if not native.setting_flag('outfit', 'sounds', True):
        return
    try:
        from audio.primitive import PlaySound
        PlaySound(obj, sounds[which]).start()
    except Exception as e:
        _log('drawer / door sound not played: {!r}'.format(e), 10)


# ---------------------------------------------------------------------------------------------------------------
# Tagged containers ("Store" on a dresser's or wardrobe's own pie).

def _tags_path():
    return os.path.join(os.path.dirname(native._log_path()), 'outfit_tags.json')


def _save_key():
    import services
    try:
        guid = services.get_persistence_service().get_save_slot_proto_guid()
    except Exception:
        guid = None
    return str(guid or 0)


def _all_tags():
    global _tags
    if _tags is None:
        try:
            with open(_tags_path(), 'r', encoding='utf-8') as f:
                _tags = json.load(f)
        except Exception:
            _tags = {}
    return _tags


def _tags_for(obj):
    import services
    if obj is None:
        return set()
    key = '{}:{}'.format(services.current_zone_id(), obj.id)
    return set(_all_tags().get(_save_key(), {}).get(key, ()))


def _set_tags(obj, tags):
    import services
    every = _all_tags()
    save = every.setdefault(_save_key(), {})
    key = '{}:{}'.format(services.current_zone_id(), obj.id)
    if tags:
        save[key] = sorted(tags)
    else:
        save.pop(key, None)
    try:
        with open(_tags_path(), 'w', encoding='utf-8') as f:
            json.dump(every, f, indent=1)
    except Exception as e:
        _log('tags not saved: {!r}'.format(e), 20)


def _occasions_of(obj):
    tags = _tags_for(obj)
    return tuple(t for key, _name, t in OCCASIONS if key in tags)


def pie_extras(target):
    """pie.py (our own pie in first person): this object's Store options, [(label json, what)], in PIE_CATEGORY."""
    try:
        if not _on() or not native.setting_flag('outfit', 'tags', True) or _kind_of(target) not in (DRAWER, RACK):
            return []
        from sims4vr import objmenu
        from sims4.localization import LocalizationHelperTuning
        tags = _tags_for(target)
        out = []
        for key, label in KIND_NAMES + tuple((k, n) for k, n, _ in OCCASIONS):
            text = label + (' (stored here)' if key in tags else '')
            out.append((objmenu._loc(LocalizationHelperTuning.get_raw_text(text)), ('tag', target.id, key)))
        if tags:
            out.append((objmenu._loc(LocalizationHelperTuning.get_raw_text('Everything (clear)')), ('clear', target.id, '')))
        return out
    except Exception as e:
        _log('Store options not made: {!r}'.format(e), 20)
        return []


def pie_pick(what):
    try:
        action, obj_id, key = what
        obj = _object(obj_id)
        if obj is None:
            return
        tags = _tags_for(obj)
        if action == 'clear':
            tags = set()
        elif key in tags:
            tags.discard(key)
        else:
            tags.add(key)
        _set_tags(obj, tags)
        _log('{} stores: {}'.format(obj, ', '.join(sorted(tags)) or 'everything'), 60)
        names = [n for k, n in KIND_NAMES if k in tags] + [n for k, n, _ in OCCASIONS if k in tags]
        then = '' if native.setting_flag('outfit', 'always', True) else 'Pick Plan Outfit on it, then '
        if _kind_of(obj) == RACK:
            how, shows = 'Grab it with grip and trigger and pull it toward you', 'it shows only those'
        elif _wardrobe(obj):
            how = then + ('grab' if then else 'Grab') + ' its doors with grip and trigger and pull them toward you'
            shows = 'it shows only those'
        else:
            how = 'Grab a drawer front with grip and trigger and pull it toward you' if native.setting_flag(
                'outfit', 'always', True) else 'Pick Plan Outfit on it, then grab a drawer front with grip and trigger ' \
                                               'and pull it'
            shows = 'its drawers show only those'
        if names:
            _note('It now holds: {}. {}: {}.'.format(', '.join(names), how, shows), 'Store')
        else:
            _note('It holds everything again. {} to see it all.'.format(how), 'Store')
        s = _session
        if s is not None and s.obj_id == obj_id and s.kind in (DRAWER, RACK, WARDROBE):
            close(True, 'its Store tags changed')
    except Exception as e:
        _log('Store pick failed: {!r}'.format(e), 20)


# ---------------------------------------------------------------------------------------------------------------
# Every controls tick (0.1 s while a lot plays).

def tick():
    global _waiting
    try:
        if not _on():
            if _session is not None:
                close(True, 'Live outfit planning switched off')
            _waiting = None
            if _gesture_alarm is not None:
                _stop_gestures()
            _probe_tick()
            return
        if _session is None and _waiting is None and not native.setting_flag('outfit', 'always', True):
            _probe_tick()
            return
        if not hooks.in_first_person():
            if _session is not None:
                close(True, 'left first person')
            _waiting = None
            return
        _check_persistence()
        _probe_tick()
        snap = None
        s = _session
        if s is not None:
            _poll_colours(s)
            if s.face is not None and s.kind != FRONT:
                snap = native.hands()
                if snap is not None:
                    c, head = s.face[0], snap[0]
                    if math.hypot(c[0] - head[0], c[2] - head[2]) > WALK_AWAY:
                        close(True, 'walked away')
            elif s.kind == FRONT and s.face is not None:
                snap = native.hands()
                if snap is not None and math.hypot(s.face[0][0] - snap[0][0], s.face[0][2] - snap[0][2]) > WALK_AWAY:
                    close(True, 'walked away')
        if native.setting_flag('outfit', 'always', True) or _waiting:
            _scan()
            snap = snap or native.hands()
            if snap is not None and _gesture_alarm is None and (_candidates(snap[0]) or (_session and _session.kind == DRAWER)):
                _start_gestures()
            if snap is not None and _session is None:
                _note_unknown(snap)
        if _waiting is not None and _object(_waiting[0]) is None:
            _waiting = None
    except Exception as e:
        _log('tick failed: {!r}'.format(e), 20)


# ---------------------------------------------------------------------------------------------------------------
# Probe (developer row) and the persistence check.

def _probe_path():
    return os.path.join(os.path.dirname(native._log_path()), 'outfit_probe.json')


def _remember(sim_info, part, kind):
    try:
        import services
        record = {'sim': sim_info.sim_id, 'outfit': list(sim_info.get_current_outfit()), 'part': part, 'body_type': kind,
                  'zone': services.current_zone_id(), 'save': _save_key(), 'at': time.strftime('%Y-%m-%d %H:%M:%S')}
        with open(_probe_path(), 'w', encoding='utf-8') as f:
            json.dump(record, f)
    except Exception as e:
        _log('last change not noted: {!r}'.format(e), 10)


def _read_record():
    try:
        with open(_probe_path(), 'r', encoding='utf-8') as f:
            return f.read()
    except Exception:
        return None


def _write_record(text):
    try:
        if text is None:
            if os.path.exists(_probe_path()):
                os.remove(_probe_path())
        else:
            with open(_probe_path(), 'w', encoding='utf-8') as f:
                f.write(text)
    except Exception as e:
        _log('last change note not put back: {!r}'.format(e), 10)


def _check_persistence():
    """Once per lot load: is the last change (outfit_probe.json) still worn? It only can be if the game was saved
    after it."""
    global _checked_zone, _probe_result
    import services
    zone = services.current_zone_id()
    if zone == _checked_zone:
        return
    _checked_zone = zone
    try:
        with open(_probe_path(), 'r', encoding='utf-8') as f:
            record = json.load(f)
    except Exception:
        return
    sim_info = services.sim_info_manager().get(record.get('sim'))
    if sim_info is None:
        return
    parts = _parts_of(sim_info, tuple(record['outfit']))
    still = record['part'] in parts.get(record['body_type'], ())
    _probe_result = 'last change ({} on outfit {}, made {}) {} after this load'.format(
        record['part'], record['outfit'], record['at'], 'STILL WORN' if still else 'NOT worn')
    _log('probe: ' + _probe_result + (' (only kept if the game was saved after it)' if not still else ''), 60)


def probe():
    """The debug menu's Outfit probe: everything that needs the game to answer, in the log."""
    sim = _fp_sim()
    if sim is None:
        _log('probe: not in first person', 20)
        return
    si = sim.sim_info
    own, other = _preference_tags(si)
    _log('probe: {} outfit {}, worn {}, clothing preference {} (tag {}, other {}); {}'.format(
        si, si.get_current_outfit(), _short(_worn(si)), getattr(getattr(si, 'clothing_preference_gender', None), 'name',
                                                                '?'), own, other,
        _probe_result or 'no earlier change noted'), 200)
    _catalogs.clear()
    for key in ('tops', 'bottoms', 'shoes', 'hats', 'necklaces'):
        _catalog(si, key, (), False)
        _catalog(si, key, (), True)
    tops = [p for p, locked in _catalog(si, 'tops', (), False) if not locked]
    worn = _worn(si)
    thumb = _dll('thumb')
    gender = _gender_letter(si)
    for part in tops[:3]:
        path = 'cas/c_{}_l_{}'.format(part, gender)
        ident = _fnv1a64('thumbs/' + path)
        asked = thumb(ident, path) if thumb is not None else None
        try:
            from cas import cas
            marks = 'own preference tag {}, other {}'.format(bool(cas.caspart_has_tag(part, [own])),
                                                             bool(cas.caspart_has_tag(part, [other])))
        except Exception as e:
            marks = 'tags not read: {!r}'.format(e)
        _log('probe: thumbnail {} as {:016x}: {}; {}'.format(path, ident, 'asked' if asked else 'NOT asked', marks), 200)
    ask = _dll('service_query')
    if ask is not None and worn.get(UPPER):
        payload = json.dumps({'base': str(worn[UPPER][0]), 'simId': str(si.sim_id), 'targetSimId': str(si.sim_id)})
        ticket = ask('GetCatalogColorFamilies', payload)
        _probe_steps.append((time.monotonic() + 2.0, ('colours', ticket)))
    other = next((p for p in tops if p not in worn.get(UPPER, ())), None)
    if other is not None:
        snap = _snapshot(si)
        ok, route, ms = wear(si, other, note=False)  # put back in 3 s: not the persistence check's change
        _log('probe: wear top {}: {} by {} in {:.0f} ms; worn now {}'.format(other, 'OK' if ok else 'FAILED', route, ms,
                                                                            _short(_worn(si))), 200)
        _probe_steps.append((time.monotonic() + 3.0, ('undo', si.sim_id, snap)))
    if _times:
        _log('probe: wear times so far: {} changes, mean {:.0f} ms, worst {:.0f} ms'.format(
            len(_times), sum(_times) / len(_times), max(_times)), 200)


def _probe_tick():
    if not _probe_steps:
        return
    now = time.monotonic()
    for step in list(_probe_steps):
        if step[0] > now:
            continue
        _probe_steps.remove(step)
        what = step[1]
        try:
            if what[0] == 'colours':
                result = _dll('service_result')
                text = result(what[1]) if result is not None and what[1] else None
                _log('probe: colour service answer: {}'.format(text[:1500] if text else 'NONE'), 200)
                if text:
                    try:
                        _log('probe: colours read from it: {}'.format(_parse_colours(text)[:10]), 200)
                    except Exception as e:
                        _log('probe: answer not JSON: {!r}'.format(e), 200)
            elif what[0] == 'undo':
                import services
                si = services.sim_info_manager().get(what[1])
                if si is not None:
                    started = time.perf_counter()
                    _restore(si, what[2])
                    _log('probe: put back in {:.0f} ms; worn now {}'.format((time.perf_counter() - started) * 1000,
                                                                           _short(_worn(si))), 200)
        except Exception as e:
            _log('probe step {} failed: {!r}'.format(what[0], e), 60)


def _fnv1a64(text):
    h = 0xcbf29ce484222325
    for b in text.encode('utf-8'):
        h ^= b
        h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h


def _install():
    import sims4.commands
    # Not first-person-only hooks: both check first person themselves, only for the game's CAS commands, so a
    # Plan Outfit left to the game is always in the log with its reason.
    hooks.hook('outfit', sims4.commands, 'client_cheat', _client_cheat)
    try:
        from interactions.utils.tunable import DoCommand
        hooks.hook('outfit', DoCommand, '_do_behavior', _do_command)
    except Exception as e:
        _log('Plan Outfit hook on DoCommand not made: {!r}'.format(e), ALWAYS)
    try:
        from sims4vr import pie
        pie.custom_categories[PIE_CATEGORY] = 'Store'
    except Exception as e:
        _log('Store pie category not made: {!r}'.format(e), 10)


try:
    _install()
except Exception as _e:
    native._log('outfit: not installed: {!r}'.format(_e))
