"""Packs the game's own UI art (re/liveui/art, colour re-cut) that our HUD
draws into one atlas the DLL embeds: art/hud_art.bin and src/hud_art.h (each
piece's rect in it). hud_art.bin: u32 width, u32 height, then straight-alpha
BGRA pixels run-length coded: u32 n; n & 0x80000000 = (n & 0x7fffffff)
transparent pixels, else n literal pixels follow.

Pieces are upscaled (Lanczos, in premultiplied space so edges keep no dark
fringe) by SCALE: the game's art is made for ~1x screen pixels, the wrist is
seen up close.

Usage: python3 tools/pack_hud_art.py   (from the repo root)
"""
import os, struct
import numpy as np
from PIL import Image

SCALE = 3
ART = 're'
# name in hud_art.h, file under re/liveui/art
PIECES = [
    ('kTray', 'liveui/art/TimeControls/201__TimeControlTray__TimeControlTrayContainer@default.png'),
    ('kTimePill', 'liveui/art/TimeControls/195__HudSeasonsWeatherBG_93@controller_out.png'),
    ('kHoverDisc', 'liveui/art/TimeControls/217.png'),
    ('kPauseUp', 'liveui/art/TimeControls/263__TimeControlsPauseButton@disabled__TimeControlsPauseButton@up.png'),
    ('kPauseOver', 'liveui/art/TimeControls/266.png'),
    ('kPauseOn', 'liveui/art/TimeControls/269__TimeControlsPauseButton@controller_selected_over__TimeControlsPauseButton@selected_up.png'),
    ('kOneUp', 'liveui/art/TimeControls/233__TimeControlsSpeedOneButton@disabled__TimeControlsSpeedOneButton@up.png'),
    ('kOneOver', 'liveui/art/TimeControls/236.png'),
    ('kOneOn', 'liveui/art/TimeControls/239__TimeControlsSpeedOneButton@controller_selected_over__TimeControlsSpeedOneButton@selected_up.png'),
    ('kTwoUp', 'liveui/art/TimeControls/253__TimeControlsSpeedTwoButton@disabled__TimeControlsSpeedTwoButton@up.png'),
    ('kTwoOver', 'liveui/art/TimeControls/256.png'),
    ('kTwoOn', 'liveui/art/TimeControls/259__TimeControlsSpeedTwoButton@controller_selected_over__TimeControlsSpeedTwoButton@selected_up.png'),
    ('kThreeUp', 'liveui/art/TimeControls/243__TimeControlsSpeedThreeButton@disabled__TimeControlsSpeedThreeButton@up.png'),
    ('kThreeOver', 'liveui/art/TimeControls/246.png'),
    ('kThreeOn', 'liveui/art/TimeControls/249__TimeControlsSpeedThreeButton@controller_selected_over__TimeControlsSpeedThreeButton@selected_up.png'),
    # Moodlets: the game's BuffBGClip tile (Fine: near white) tinted per mood.
    ('kMoodTile', 'liveui/art/SimHub/180__BuffBGClip_19@Fine.png'),
    # The queue's cancel X.
    ('kCancel', 'liveui/art/InteractionQueue/96.png'),
    # Action queue (InteractionQueue SWF, NormalInteractionButton's mcProgress
    # and MixerInteractionButton): the progress ring's dark back, one of its
    # 100 green spokes, its rim; the side action's small cancel X.
    ('kProgressShadow', 'liveui/art/InteractionQueue/45.png'),
    ('kProgressSpoke', 'liveui/art/InteractionQueue/47.png'),
    ('kProgressRim', 'liveui/art/InteractionQueue/50.png'),
    ('kMixerCancel', 'liveui/art/InteractionQueue/41.png'),
    # Wants: the whim cloud.
    ('kWantCloud', 'liveui/art/SimHub/117.png'),
    ('kFearBubble', 'liveui/art/SimHub/88.png'),  # the spiky bubble of a fear (confrontation want)
    # The build/buy button's icon (GameTray), up and over.
    ('kBuildIcon', 'buildui/art/GameTray/93__BuildBuyButton@controller_disabledOver__BuildBuyButton@disabled.png'),
    ('kBuildIconOver', 'buildui/art/GameTray/96__BuildBuyButton@controller_down__BuildBuyButton@flair_over.png'),
    # Popups (Dialog): the two-Sim chat (SimSpeechHeader / SimSpeechFooter):
    # the other Sim's portrait ring and blue speech bubble, your portrait ring
    # and the light answer bubble; the dialog's button (out / over) and the
    # plain dialog's panel (DialogHeaderBackground_9@default).
    ('kPortraitRing', 'liveui/art/Dialog/64.png'),
    ('kSpeechBubble', 'liveui/art/Dialog/67.png'),
    ('kYourRing', 'liveui/art/Dialog/107.png'),
    ('kAnswerBubble', 'liveui/art/Dialog/109.png'),
    ('kDialogButton', 'liveui/art/Dialog/241__DialogButtonIconInstance@down__DialogButtonIconInstance@out.png'),
    ('kDialogButtonOver', 'liveui/art/Dialog/247__DialogButtonIconInstance@over__DialogButtonInstance@over.png'),
    ('kDialogPanel', 'liveui/art/Dialog/200__DialogHeaderBackground_9@default.png'),
    # The phone (SimInfoTray: PhonePanel and its screens, the game's own phone
    # menu drawn on our phone, hud.cpp drawPhone): the dark band of the app
    # list's carousel, the pill behind the tool buttons, the battery, the
    # silence button (up / over, silenced up / over), an app's hover frame and
    # notification glow, the carousel arrow, the back button, a list row
    # (over / down), the page dots (other / over / this page), the list's
    # slider; the game menu's three dots (GameTray EscapeMenuBtn) for Options.
    ('kPhoneTint', 'liveui/art/SimInfoTray/56.png'),
    ('kPhonePill', 'liveui/art/SimInfoTray/81.png'),
    ('kPhoneBattery', 'liveui/art/SimInfoTray/78.png'),
    ('kPhoneSilence', 'liveui/art/SimInfoTray/63__PhoneSilenceButton@controller_disabledOver__PhoneSilenceButton@disabled.png'),
    ('kPhoneSilenceOver', 'liveui/art/SimInfoTray/66__PhoneSilenceButton@over.png'),
    ('kPhoneSilenced', 'liveui/art/SimInfoTray/69__PhoneSilenceButton@selected_disabled__PhoneSilenceButton@selected_up.png'),
    ('kPhoneSilencedOver', 'liveui/art/SimInfoTray/72__PhoneSilenceButton@controller_selected_over__PhoneSilenceButton@selected_over.png'),
    ('kPhoneAppOver', 'liveui/art/SimInfoTray/59__PhoneCategoryCellView@over__PhoneCategoryCellView@selected_up.png'),
    ('kPhoneBadgeGlow', 'liveui/art/SimInfoTray/7__MotivesVampireSunlightOverlay@intro__PhoneCategoryCellView@up.png'),
    ('kPhoneArrow', 'liveui/art/SimInfoTray/20__PhoneArrowButton@out__PhoneArrowButton@release.png'),
    ('kPhoneArrowOver', 'liveui/art/SimInfoTray/23__PhoneArrowButton@controller_over__PhoneArrowButton@over.png'),
    ('kPhoneBack', 'liveui/art/SimInfoTray/33__PhoneBackButton@out__PhoneBackButton@release.png'),
    ('kPhoneBackOver', 'liveui/art/SimInfoTray/36__PhoneBackButton@over__PhoneBackButton@selected_disabled.png'),
    ('kPhoneRowOver', 'liveui/art/SimInfoTray/11__PhoneItemCellView@over.png'),
    ('kPhoneRowDown', 'liveui/art/SimInfoTray/14__PhoneItemCellView@down.png'),
    ('kPhonePage', 'liveui/art/SimInfoTray/85__PhoneMenu_Button_OtherPage_124@out__PhoneMenu_Button_OtherPage_124@release.png'),
    ('kPhonePageOver', 'liveui/art/SimInfoTray/88__PhoneMenu_Button_OtherPage_124@over__PhonePageButton@over.png'),
    ('kPhonePageOn', 'liveui/art/SimInfoTray/100__PhonePageButton@selected_disable__PhonePageButton@selected_up.png'),
    ('kPhoneSlider', 'liveui/art/SimInfoTray/43__PhoneMenuScrollList.png'),
    ('kPhoneColorBtn', 'liveui/art/SimInfoTray/113__PhoneBackgroundColorSelectorButton@controller_disabledOver__PhoneBackgroundColorSelectorButton@controller_selected_up.png'),
    ('kPhoneColorBtnOver', 'liveui/art/SimInfoTray/116__PhoneBackgroundColorSelectorButton@controller_selected_over__PhoneBackgroundColorSelectorButton@over.png'),
    ('kPhoneColorBtnDown', 'liveui/art/SimInfoTray/119__PhoneBackgroundColorSelectorButton@controller_selected_down__PhoneBackgroundColorSelectorButton@down.png'),
    ('kPhoneSelectorArrow', 'liveui/art/SimInfoTray/47__PhoneBackgroundColorSelector__PhoneCaseSelector.png'),
    ('kNoteDelete', 'liveui/art/NotificationWall/8__DeleteButton@disabled__DeleteButton@up.png'),
    ('kNoteDeleteOver', 'liveui/art/NotificationWall/10__DeleteButton@over.png'),
    # The notification backgrounds (Notifications SWF: QueuedEffectBackground_4's
    # frames standard/player/critical/moment, each with its "_strip" frame).
    ('kNoteStandard', 'liveui/art/Notifications/3__QueuedEffectBackground_4@standard.png'),
    ('kNotePlayer', 'liveui/art/Notifications/7__QueuedEffectBackground_4@player.png'),
    ('kNoteCritical', 'liveui/art/Notifications/11__QueuedEffectBackground_4@critical.png'),
    ('kNoteStandardStrip', 'liveui/art/Notifications/5__QueuedEffectBackground_4@standard_strip.png'),
    ('kNotePlayerStrip', 'liveui/art/Notifications/9__QueuedEffectBackground_4@player_strip.png'),
    ('kNoteCriticalStrip', 'liveui/art/Notifications/13__QueuedEffectBackground_4@critical_strip.png'),
    ('kNoteMomentStrip', 'liveui/art/Notifications/17__QueuedEffectBackground_4@moment_strip.png'),
    # The calendar app (Calendar SWF: CalendarDayView, SchedulePane; the HUD's
    # calendar button), drawn small on the phone: 1x pieces (third field).
    ('kCalendarButton', 'liveui/art/SimInfoTray/404__CalendarButton@over.png'),
    ('kCalSpring', 'liveui/art/Calendar/207__SeasonalBackgrounds_20@none.png', 1),
    ('kCalSummer', 'liveui/art/Calendar/209__SeasonalBackgrounds_20@summer.png', 1),
    ('kCalFall', 'liveui/art/Calendar/211__SeasonalBackgrounds_20@fall.png', 1),
    ('kCalWinter', 'liveui/art/Calendar/213__SeasonalBackgrounds_20@winter.png', 1),
    ('kCalDayPast', 'liveui/art/Calendar/270__CalendarDayView@past_up.png', 1),
    ('kCalDaySelected', 'liveui/art/Calendar/267__CalendarDayView@over.png', 1),
    ('kCalBgWedding', 'liveui/art/Calendar/26.png', 1),
    ('kCalBgHoliday', 'liveui/art/Calendar/111.png', 1),
    ('kCalBgGetaway', 'liveui/art/Calendar/128.png', 1),
    ('kCalBgFuneral', 'liveui/art/Calendar/142.png', 1),
    ('kCalBgFestival', 'liveui/art/Calendar/155.png', 1),
    ('kCalBgEvent', 'liveui/art/Calendar/170.png', 1),
    ('kCalBgWork', 'liveui/art/Calendar/184__ScheduleWorkEventEntry.png', 1),
    ('kCalBgBirthday', 'liveui/art/Calendar/254.png', 1),
    ('kCalIconWedding', 'liveui/art/Calendar/28__DayViewActivityIcon_30@wedding.png', 1),
    ('kCalIconVacation', 'liveui/art/Calendar/63__DayViewActivityIcon_30@vacation.png', 1),
    ('kCalIconStayover', 'liveui/art/Calendar/76.png', 1),
    ('kCalIconHoliday', 'liveui/art/Calendar/113__CalendarHolidayTooltipEntry__DayViewActivityIcon_30@holiday.png', 1),
    ('kCalIconGetaway', 'liveui/art/Calendar/126__DayViewActivityIcon_30@getaway.png', 1),
    ('kCalIconFuneral', 'liveui/art/Calendar/144__DayViewActivityIcon_30@funeral.png', 1),
    ('kCalIconFestival', 'liveui/art/Calendar/157__CalendarFestivalTooltipEntry__DayViewActivityIcon_30@festival.png', 1),
    ('kCalIconEvent', 'liveui/art/Calendar/172__DayViewActivityIcon_30@event.png', 1),
    ('kCalIconWork', 'liveui/art/Calendar/188__DayViewActivityIcon_30@work.png', 1),
    ('kCalIconBirthday', 'liveui/art/Calendar/200__DayViewActivityIcon_30@birthday.png', 1),
    ('kCalEntryBg', 'liveui/art/Calendar/1.png', 1),
    ('kCalMoon0', 'liveui/art/Calendar/226__CalendarLunarPhaseIcon_29@new_moon.png', 1),
    ('kCalMoon1', 'liveui/art/Calendar/228__CalendarLunarPhaseIcon_29@waxing_crescent.png', 1),
    ('kCalMoon2', 'liveui/art/Calendar/230__CalendarLunarPhaseIcon_29@first_quarter.png', 1),
    ('kCalMoon3', 'liveui/art/Calendar/232__CalendarLunarPhaseIcon_29@waxing_gibbous.png', 1),
    ('kCalMoon4', 'liveui/art/Calendar/234__CalendarLunarPhaseIcon_29@full_moon.png', 1),
    ('kCalMoon5', 'liveui/art/Calendar/236__CalendarLunarPhaseIcon_29@waning_gibbous.png', 1),
    ('kCalMoon6', 'liveui/art/Calendar/238__CalendarLunarPhaseIcon_29@third_quarter.png', 1),
    ('kCalMoon7', 'liveui/art/Calendar/240__CalendarLunarPhaseIcon_29@waning_crescent.png', 1),
    ('kGameMenuDots', 'buildui/art/GameTray/235__EscapeMenuBtn@disabled__EscapeMenuBtn@selected_disabled.png'),
    ('kGameMenuDotsOver', 'buildui/art/GameTray/238.png'),
]
EXTRA = 'tools/hud_art_extra.txt'
# Arrays of one piece per mood (MOODS order): the frames of a mood sprite,
# '<dir>/*__<sprite>@<Mood>.png'; Default is the sprite's Fine frame (its
# "Default" label shows the Fine picture). The queue button's disc
# (InteractionButtonEmotionBg_6) and the side action's
# (MixerInteractionButtonEmotionBg_21): InteractionButton.as shows the
# Sim's mood frame when the interaction lists that mood, else Default.
MOOD_ARRAYS = [
    ('kQueueDisc', 'liveui/art/InteractionQueue', 'InteractionButtonEmotionBg_6'),
    ('kMixerDisc', 'liveui/art/InteractionQueue', 'MixerInteractionButtonEmotionBg_21'),
]
# The moods, in the order the script sends them (hudinfo.py MOODS); their
# colour = the game's BuffBGClip_19@<mood> tile over the Fine tile.
MOODS = ['Default', 'Angry', 'Asleep', 'Bored', 'Confident', 'Dazed', 'Embarrassed', 'Energized', 'Fine', 'Flirty',
         'Focused', 'Happy', 'Inspired', 'Playful', 'Sad', 'Scared', 'Tense', 'Uncomfortable']

def mood_tints():
    import glob
    def mean(path):
        a = np.asarray(Image.open(path).convert('RGBA')).astype(np.float32)
        m = a[:, :, 3] > 200
        return a[m][:, :3].mean(0)
    base = mean(os.path.join(ART, 'liveui/art/SimHub/180__BuffBGClip_19@Fine.png'))
    out = []
    for mood in MOODS:
        f = glob.glob(os.path.join(ART, 'liveui/art/SimHub/*__BuffBGClip_19@%s.png' % mood))
        if not f and mood == 'Scared':  # its tile has no label in the SWF; the next BuffBGClip frame (maroon)
            f = [os.path.join(ART, 'liveui/art/SimHub/204.png')]
        c = mean(f[0]) / np.maximum(base, 1) if f else np.ones(3)
        r, g, b = (np.clip(c, 0, 1) * 255 + 0.5).astype(int)
        out.append(0xff000000 | (b << 16) | (g << 8) | r)  # ImGui IM_COL32 order (ABGR)
    return out  # more "name path" lines (needs, moodlets...)

def load(path, scale=SCALE):
    a = np.asarray(Image.open(path).convert('RGBA')).astype(np.float32) / 255
    a[:, :, :3] *= a[:, :, 3:4]  # premultiply
    h, w = a.shape[:2]
    chans = [Image.fromarray(a[:, :, c], 'F').resize((w * scale, h * scale), Image.LANCZOS) for c in range(4)]
    out = np.stack([np.asarray(c) for c in chans], 2).clip(0, 1)
    out[:, :, :3] = np.minimum(out[:, :, :3], out[:, :, 3:4])
    return out

def main():
    pieces = list(PIECES)
    if os.path.exists(EXTRA):
        for line in open(EXTRA):
            line = line.split('#')[0].strip()
            if line:
                n, p = line.split(None, 1)
                pieces.append((n, p))
    import glob
    arrays = []  # (array name, [piece name per mood])
    for name, folder, sprite in MOOD_ARRAYS:
        names = []
        for mood in MOODS:
            frame = 'Fine' if mood == 'Default' else mood
            f = glob.glob(os.path.join(ART, folder, '*__%s@%s.png' % (sprite, frame)))
            if len(f) != 1:
                raise SystemExit('%s: %d pictures for %s' % (name, len(f), frame))
            piece = '%s_%s' % (name, frame)
            if piece not in [t[0] for t in pieces]:
                pieces.append((piece, os.path.relpath(f[0], ART)))
            names.append(piece)
        arrays.append((name, names))
    imgs = [(t[0], load(os.path.join(ART, t[1]), *t[2:])) for t in pieces]
    W = 1024
    x = y = 0; row = 0; rects = []
    for n, im in sorted(imgs, key=lambda t: -t[1].shape[0]):
        h, w = im.shape[:2]
        if x + w + 2 > W: x = 0; y += row + 2; row = 0
        rects.append((n, x, y, w, h, im)); x += w + 2; row = max(row, h)
    H = y + row
    H = (H + 15) // 16 * 16  # (D3D11 textures need no power of two)
    atlas = np.zeros((H, W, 4), np.float32)
    for n, x, y, w, h, im in rects:
        atlas[y:y + h, x:x + w] = im
    # Straight alpha in the file (Dear ImGui blends straight alpha).
    a = atlas[:, :, 3:4]
    atlas[:, :, :3] = np.where(a > 1e-4, atlas[:, :, :3] / np.maximum(a, 1e-4), 0)
    bgra = (atlas[:, :, [2, 1, 0, 3]].clip(0, 1) * 255 + 0.5).astype(np.uint8)
    os.makedirs('art', exist_ok=True)
    with open('art/hud_art.bin', 'wb') as f:
        f.write(struct.pack('<II', W, H))
        px = bgra.reshape(-1, 4)
        empty = px[:, 3] == 0
        i, n = 0, len(px)
        while i < n:
            j = i
            if empty[i]:
                while j < n and empty[j]: j += 1
                f.write(struct.pack('<I', 0x80000000 | (j - i)))
            else:
                while j < n and not empty[j]: j += 1
                f.write(struct.pack('<I', j - i)); f.write(px[i:j].tobytes())
            i = j
    with open('src/hud_art.h', 'w') as f:
        f.write('#pragma once\n// Generated by tools/pack_hud_art.py from the game\'s own UI art (re/liveui/art).\n')
        f.write('// Pieces of art/hud_art.bin (%dx%d, straight-alpha BGRA, art upscaled %dx): x, y, w, h in pixels.\n' % (W, H, SCALE))
        f.write('namespace s4vr::hud::art {\nconstexpr int kAtlasW = %d, kAtlasH = %d, kScale = %d;\n' % (W, H, SCALE))
        f.write('struct Piece {\n    int x, y, w, h;\n};\n')
        in_arrays = {n for _, names in arrays for n in names}
        at = {n: (x, y, w, h) for n, x, y, w, h, im in rects}
        for n, x, y, w, h, im in sorted(rects, key=lambda r: [p[0] for p in pieces].index(r[0])):
            if n not in in_arrays:
                f.write('constexpr Piece %s{%d, %d, %d, %d};\n' % (n, x, y, w, h))
        for name, names in arrays:
            f.write('// One per mood, in hudinfo.py MOODS order.\n')
            f.write('constexpr Piece %s[%d] = {%s};\n' % (name, len(names), ', '.join('{%d, %d, %d, %d}' % at[n] for n in names)))
        f.write('// Mood colours (IM_COL32), in hudinfo.py MOODS order: %s.\n' % ', '.join(MOODS))
        f.write('constexpr unsigned kMoodTint[%d] = {%s};\n' % (len(MOODS), ', '.join('0x%08xu' % t for t in mood_tints())))
        f.write('}  // namespace s4vr::hud::art\n')
    print('atlas %dx%d, %d pieces' % (W, H, len(rects)))

if __name__ == '__main__':
    main()
