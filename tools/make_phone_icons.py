"""Our phone apps' icons (Settings, Calendar, Build Mode, Game Options, Phone
Options, VR Options) drawn in the style of the game's own phone app icons
(InGame/UI/Icons/PhoneMenu/icon_phone_category_*: a rounded tile in a soft
two-colour gradient with a gloss, one glossy object, a tinted second object in
a corner). 128 x 128 PNGs into art/phone_icons/, packed by pack_hud_art.py.

Usage: python3 tools/make_phone_icons.py   (from the repo root)
"""
import math, os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageChops

N = 512  # drawn at 4x, then reduced
OUT = 'art/phone_icons'


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def gradient(size, c0, c1, angle=135):
    """A linear gradient c0 -> c1 (RGBA image)."""
    w, h = size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    a = math.radians(angle)
    t = (x * math.cos(a) + y * math.sin(a))
    t = (t - t.min()) / max(1e-6, t.max() - t.min())
    c0 = np.array(c0 + (255,) if len(c0) == 3 else c0, np.float32)
    c1 = np.array(c1 + (255,) if len(c1) == 3 else c1, np.float32)
    img = c0 + (c1 - c0) * t[..., None]
    return Image.fromarray(img.clip(0, 255).astype(np.uint8), 'RGBA')


def mask_of(draw_fn, size=(N, N)):
    m = Image.new('L', size, 0)
    draw_fn(ImageDraw.Draw(m))
    return m


def fill(canvas, mask, c0, c1, angle=90, outline=None, width=0, shade=True):
    """mask filled with a gradient, an outline of `width` px around it, a soft inner shade."""
    if outline and width:
        grown = mask.filter(ImageFilter.MaxFilter(width * 2 + 1))
        canvas.paste(Image.new('RGBA', canvas.size, outline), (0, 0), grown)
    g = gradient(canvas.size, c0, c1, angle)
    canvas.paste(g, (0, 0), mask)
    if shade:  # darker toward the lower right edge, light along the upper left (a rounded look)
        inner = mask.filter(ImageFilter.GaussianBlur(10))
        edge = ImageChops.subtract(mask, inner)
        dark = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
        dark.putalpha(edge.point(lambda v: v * 0.45))
        canvas.alpha_composite(dark)


def gloss(canvas, mask, box, strength=150):
    """A white highlight (upper part of `box`) inside mask."""
    hl = Image.new('L', canvas.size, 0)
    ImageDraw.Draw(hl).ellipse(box, fill=strength)
    hl = hl.filter(ImageFilter.GaussianBlur(18))
    hl = ImageChops.multiply(hl, mask)
    white = Image.new('RGBA', canvas.size, (255, 255, 255, 0))
    white.putalpha(hl)
    canvas.alpha_composite(white)


def tile(c0, c1, rim):
    """The rounded tile: gradient, a lighter wash upper left, a thin darker rim."""
    img = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    m = mask_of(lambda d: d.rounded_rectangle((10, 10, N - 10, N - 10), radius=110, fill=255))
    img.paste(Image.new('RGBA', (N, N), rim), (0, 0), m)
    inner = mask_of(lambda d: d.rounded_rectangle((20, 20, N - 20, N - 20), radius=100, fill=255))
    img.paste(gradient((N, N), c0, c1, 60), (0, 0), inner)
    wash = Image.new('L', (N, N), 0)
    ImageDraw.Draw(wash).ellipse((-140, -160, 330, 300), fill=150)
    wash = ImageChops.multiply(wash.filter(ImageFilter.GaussianBlur(60)), inner)
    w = Image.new('RGBA', (N, N), (255, 255, 255, 0))
    w.putalpha(wash)
    img.alpha_composite(w)
    return img, inner


def shadow(canvas, mask, dx=10, dy=14, alpha=90):
    s = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    sh = mask.filter(ImageFilter.GaussianBlur(12)).point(lambda v: v * alpha / 255)
    s.putalpha(ImageChops.offset(sh, dx, dy))
    canvas.alpha_composite(s)


def gear_mask(cx, cy, r_out, r_in, teeth, hole, size=(N, N)):
    m = Image.new('L', size, 0)
    d = ImageDraw.Draw(m)
    pts = []
    for k in range(teeth * 4):
        a = 2 * math.pi * k / (teeth * 4) + math.pi / (teeth * 4)
        r = r_out if (k % 4) in (1, 2) else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=255)
    d.ellipse((cx - r_in * 0.98, cy - r_in * 0.98, cx + r_in * 0.98, cy + r_in * 0.98), fill=255)
    if hole:
        d.ellipse((cx - hole, cy - hole, cx + hole, cy + hole), fill=0)
    return m


def tinted(canvas, mask, colour, alpha=170):
    """The second object: flat, in a deeper shade of the tile."""
    t = Image.new('RGBA', canvas.size, colour + (0,))
    t.putalpha(mask.point(lambda v: v * alpha / 255))
    canvas.alpha_composite(t)


def finish(img, inner, name):
    img.putalpha(ImageChops.multiply(img.getchannel('A'),
                                     mask_of(lambda d: d.rounded_rectangle((10, 10, N - 10, N - 10), radius=110, fill=255))))
    img.resize((128, 128), Image.LANCZOS).save(os.path.join(OUT, name + '.png'))


def settings():
    img, inner = tile((150, 205, 215), (60, 110, 140), (40, 80, 105, 255))
    small = gear_mask(390, 380, 92, 70, 8, 30)
    tinted(img, small, (30, 70, 95))
    g = gear_mask(230, 235, 170, 132, 10, 52)
    shadow(img, g)
    fill(img, g, (235, 242, 248), (120, 140, 160), 70, outline=(55, 70, 90, 255), width=7)
    ring = mask_of(lambda d: d.ellipse((230 - 80, 235 - 80, 230 + 80, 235 + 80), outline=255, width=16))
    tinted(img, ring, (95, 115, 140), 200)
    gloss(img, g, (90, 70, 330, 230))
    finish(img, inner, 'settings')


def calendar():
    img, inner = tile((255, 200, 150), (225, 95, 70), (150, 55, 40, 255))
    tinted(img, mask_of(lambda d: d.ellipse((330, 330, 470, 470), outline=255, width=22)), (150, 50, 35))
    tinted(img, mask_of(lambda d: d.line((400, 400, 400, 350), fill=255, width=16)), (150, 50, 35))
    tinted(img, mask_of(lambda d: d.line((400, 400, 435, 415), fill=255, width=16)), (150, 50, 35))
    page = mask_of(lambda d: d.rounded_rectangle((95, 110, 380, 400), radius=34, fill=255))
    shadow(img, page)
    fill(img, page, (255, 255, 255), (220, 228, 238), 90, outline=(110, 45, 35, 255), width=7)
    band = ImageChops.multiply(page, mask_of(lambda d: d.rectangle((0, 0, N, 185), fill=255)))
    fill(img, band, (245, 90, 75), (190, 40, 40), 90, shade=False)
    for x in (160, 315):
        ring = mask_of(lambda d, x=x: d.rounded_rectangle((x - 14, 78, x + 14, 150), radius=12, fill=255))
        fill(img, ring, (240, 240, 245), (150, 155, 170), 0, outline=(90, 40, 35, 255), width=5)
    grid = Image.new('L', (N, N), 0)
    gd = ImageDraw.Draw(grid)
    for r in range(3):
        for c in range(4):
            x0, y0 = 120 + c * 64, 205 + r * 62
            gd.rounded_rectangle((x0, y0, x0 + 44, y0 + 42), radius=8, fill=255)
    tinted(img, grid, (120, 145, 180), 150)
    today = mask_of(lambda d: d.rounded_rectangle((120 + 2 * 64, 205 + 62, 120 + 2 * 64 + 44, 205 + 62 + 42), radius=8, fill=255))
    fill(img, today, (120, 220, 90), (40, 150, 40), 90, shade=False)
    gloss(img, page, (60, 60, 360, 220), 120)
    finish(img, inner, 'calendar')


def build():
    img, inner = tile((180, 225, 255), (70, 120, 210), (40, 70, 140, 255))
    # the second object: a house outline in the corner
    house = mask_of(lambda d: d.polygon([(320, 470), (320, 380), (395, 320), (470, 380), (470, 470)], fill=255))
    tinted(img, house, (35, 70, 150))
    # a hammer: wooden handle, steel head
    handle = mask_of(lambda d: d.line((150, 410, 330, 170), fill=255, width=46))
    handle = handle.filter(ImageFilter.MaxFilter(3))
    shadow(img, handle)
    fill(img, handle, (230, 170, 100), (150, 90, 40), 0, outline=(90, 50, 25, 255), width=6)
    head = Image.new('L', (N, N), 0)
    hd = ImageDraw.Draw(head)
    cx, cy, a = 335, 160, math.radians(-53)
    def rot(px, py):
        return (cx + px * math.cos(a) - py * math.sin(a), cy + px * math.sin(a) + py * math.cos(a))
    hd.polygon([rot(-40, -120), rot(40, -120), rot(40, 70), rot(-40, 70)], fill=255)
    hd.polygon([rot(-40, 70), rot(40, 70), rot(25, 120), rot(-25, 120)], fill=255)
    shadow(img, head)
    fill(img, head, (245, 248, 252), (110, 125, 145), 30, outline=(45, 55, 75, 255), width=7)
    gloss(img, head, (230, 60, 400, 190), 140)
    gloss(img, handle, (140, 200, 300, 330), 90)
    finish(img, inner, 'build')


def plumbob(img, cx, cy, h):
    w = h * 0.5
    top = mask_of(lambda d: d.polygon([(cx, cy - h / 2), (cx + w / 2, cy), (cx - w / 2, cy)], fill=255))
    bottom = mask_of(lambda d: d.polygon([(cx, cy + h / 2), (cx + w / 2, cy), (cx - w / 2, cy)], fill=255))
    both = ImageChops.lighter(top, bottom)
    shadow(img, both)
    fill(img, both, (150, 240, 90), (40, 160, 40), 90, outline=(25, 90, 25, 255), width=6, shade=False)
    left = mask_of(lambda d: d.polygon([(cx, cy - h / 2), (cx, cy + h / 2), (cx - w / 2, cy)], fill=255))
    tinted(img, ImageChops.multiply(left, top), (220, 255, 170), 120)
    tinted(img, ImageChops.multiply(left, bottom), (30, 120, 30), 90)
    gloss(img, top, (cx - w / 2, cy - h / 2, cx + w / 4, cy), 120)


def game_options():
    img, inner = tile((190, 245, 190), (60, 160, 110), (30, 100, 70, 255))
    small = gear_mask(395, 385, 85, 64, 8, 26)
    tinted(img, small, (25, 95, 60))
    # three sliders behind the plumbob
    for k, (y, x) in enumerate(((130, 300), (235, 170), (340, 260))):
        track = mask_of(lambda d, y=y: d.rounded_rectangle((70, y - 12, 330, y + 12), radius=12, fill=255))
        fill(img, track, (245, 250, 255), (170, 190, 200), 90, outline=(40, 90, 70, 255), width=5, shade=False)
        knob = mask_of(lambda d, x=x, y=y: d.ellipse((x - 30, y - 30, x + 30, y + 30), fill=255))
        shadow(img, knob, 5, 7, 70)
        fill(img, knob, (255, 255, 255), (150, 170, 190), 80, outline=(40, 70, 90, 255), width=5)
    plumbob(img, 380, 215, 250)
    finish(img, inner, 'game_options')


def phone_options():
    img, inner = tile((175, 215, 255), (70, 95, 200), (40, 55, 140, 255))
    small = gear_mask(395, 385, 88, 66, 8, 28)
    tinted(img, small, (35, 50, 140))
    body = mask_of(lambda d: d.rounded_rectangle((150, 70, 330, 430), radius=36, fill=255))
    shadow(img, body)
    fill(img, body, (90, 100, 120), (30, 35, 45), 90, outline=(20, 25, 35, 255), width=7)
    screen = mask_of(lambda d: d.rounded_rectangle((168, 110, 312, 375), radius=14, fill=255))
    fill(img, screen, (90, 190, 255), (30, 90, 200), 90, shade=False)
    apps = Image.new('L', (N, N), 0)
    ad = ImageDraw.Draw(apps)
    for r in range(3):
        for c in range(3):
            x0, y0 = 182 + c * 44, 130 + r * 52
            ad.rounded_rectangle((x0, y0, x0 + 30, y0 + 30), radius=8, fill=255)
    tinted(img, apps, (255, 255, 255), 190)
    tinted(img, mask_of(lambda d: d.rounded_rectangle((215, 392, 265, 404), radius=6, fill=255)), (170, 180, 200), 230)
    gloss(img, ImageChops.multiply(screen, mask_of(lambda d: d.polygon([(160, 100), (320, 100), (160, 300)], fill=255))),
          (150, 90, 330, 320), 90)
    g = gear_mask(330, 330, 78, 60, 8, 24)
    shadow(img, g)
    fill(img, g, (245, 245, 250), (140, 150, 170), 70, outline=(45, 55, 80, 255), width=6)
    finish(img, inner, 'phone_options')


def vr_options():
    img, inner = tile((225, 195, 255), (130, 80, 205), (75, 40, 140, 255))
    small = gear_mask(400, 395, 80, 60, 8, 25)
    tinted(img, small, (75, 35, 140))
    strap = mask_of(lambda d: d.rounded_rectangle((50, 215, 462, 265), radius=24, fill=255))
    fill(img, strap, (110, 100, 130), (50, 45, 65), 90, outline=(30, 25, 45, 255), width=5, shade=False)
    body = mask_of(lambda d: d.rounded_rectangle((95, 150, 417, 330), radius=70, fill=255))
    shadow(img, body)
    fill(img, body, (250, 250, 255), (170, 170, 195), 90, outline=(45, 40, 70, 255), width=7)
    face = mask_of(lambda d: d.rounded_rectangle((120, 175, 392, 305), radius=52, fill=255))
    fill(img, face, (60, 60, 80), (20, 20, 30), 90, shade=False)
    for x in (190, 322):
        lens = mask_of(lambda d, x=x: d.ellipse((x - 52, 240 - 46, x + 52, 240 + 46), fill=255))
        fill(img, lens, (120, 220, 255), (40, 90, 200), 120, outline=(15, 20, 40, 255), width=4, shade=False)
        gloss(img, lens, (x - 50, 196, x + 20, 245), 170)
    nose = mask_of(lambda d: d.polygon([(226, 305), (286, 305), (272, 272), (240, 272)], fill=255))
    tinted(img, nose, (170, 170, 195), 255)
    gloss(img, body, (110, 120, 400, 220), 110)
    finish(img, inner, 'vr_options')


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for f in (settings, calendar, build, game_options, phone_options, vr_options):
        f()
    print('phone icons in', OUT)
