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

def load(path):
    a = np.asarray(Image.open(path).convert('RGBA')).astype(np.float32) / 255
    a[:, :, :3] *= a[:, :, 3:4]  # premultiply
    h, w = a.shape[:2]
    chans = [Image.fromarray(a[:, :, c], 'F').resize((w * SCALE, h * SCALE), Image.LANCZOS) for c in range(4)]
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
            if piece not in [n for n, _ in pieces]:
                pieces.append((piece, os.path.relpath(f[0], ART)))
            names.append(piece)
        arrays.append((name, names))
    imgs = [(n, load(os.path.join(ART, p))) for n, p in pieces]
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
