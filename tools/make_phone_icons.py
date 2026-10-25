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


def person(cx, cy, s):
    """A Sim's head and shoulders (mask)."""
    return mask_of(lambda d: (d.ellipse((cx - 34 * s, cy - 90 * s, cx + 34 * s, cy - 22 * s), fill=255),
                              d.rounded_rectangle((cx - 62 * s, cy - 12 * s, cx + 62 * s, cy + 70 * s), radius=int(40 * s), fill=255)))


def my_sim():
    img, inner = tile((170, 245, 200), (40, 150, 110), (20, 95, 65, 255))
    tinted(img, person(400, 410, 0.9), (20, 100, 70))
    plumbob(img, 240, 245, 360)
    finish(img, inner, 'my_sim')


def aspirations():
    img, inner = tile((255, 220, 250), (175, 85, 200), (110, 40, 130, 255))
    tinted(img, mask_of(lambda d: d.polygon([(400, 330), (420, 380), (470, 385), (430, 415), (445, 465), (400, 438),
                                                 (355, 465), (370, 415), (330, 385), (380, 380)], fill=255)), (110, 40, 135))
    cloud = mask_of(lambda d: (d.ellipse((90, 170, 250, 330), fill=255), d.ellipse((170, 110, 350, 290), fill=255),
                               d.ellipse((270, 170, 420, 320), fill=255), d.rounded_rectangle((120, 230, 400, 340), radius=55, fill=255)))
    shadow(img, cloud)
    fill(img, cloud, (255, 255, 255), (215, 200, 235), 90, outline=(95, 45, 120, 255), width=7)
    for (x, y, r) in ((150, 380, 26), (105, 425, 15)):
        b = mask_of(lambda d, x=x, y=y, r=r: d.ellipse((x - r, y - r, x + r, y + r), fill=255))
        fill(img, b, (255, 255, 255), (215, 200, 235), 90, outline=(95, 45, 120, 255), width=5, shade=False)
    star = mask_of(lambda d: d.polygon([(255, 160), (275, 215), (332, 218), (288, 252), (303, 305), (255, 275),
                                        (207, 305), (222, 252), (178, 218), (235, 215)], fill=255))
    fill(img, star, (255, 235, 110), (240, 150, 30), 90, outline=(150, 80, 20, 255), width=5)
    gloss(img, cloud, (100, 100, 360, 240), 110)
    finish(img, inner, 'aspirations')


def career():
    img, inner = tile((255, 230, 170), (215, 140, 50), (130, 75, 20, 255))
    tinted(img, mask_of(lambda d: (d.rectangle((345, 380, 375, 470), fill=255), d.rectangle((390, 340, 420, 470), fill=255),
                                   d.rectangle((435, 300, 465, 470), fill=255))), (130, 70, 20))
    handle = mask_of(lambda d: d.rounded_rectangle((190, 110, 320, 190), radius=26, outline=255, width=24))
    fill(img, handle, (110, 80, 60), (60, 40, 30), 90, outline=(40, 25, 15, 255), width=4, shade=False)
    case = mask_of(lambda d: d.rounded_rectangle((90, 170, 420, 390), radius=34, fill=255))
    shadow(img, case)
    fill(img, case, (175, 115, 70), (110, 65, 35), 90, outline=(60, 35, 15, 255), width=7)
    band = ImageChops.multiply(case, mask_of(lambda d: d.rectangle((0, 255, N, 285), fill=255)))
    tinted(img, band, (80, 45, 20), 160)
    clasp = mask_of(lambda d: d.rounded_rectangle((225, 245, 285, 300), radius=10, fill=255))
    fill(img, clasp, (255, 230, 140), (200, 150, 40), 90, outline=(110, 75, 15, 255), width=4, shade=False)
    gloss(img, case, (100, 150, 380, 270), 110)
    finish(img, inner, 'career')


def skills():
    img, inner = tile((175, 230, 255), (40, 120, 200), (20, 65, 130, 255))
    tinted(img, mask_of(lambda d: d.ellipse((350, 340, 470, 460), outline=255, width=20)), (20, 60, 130))
    for k, (x, top) in enumerate(((110, 300), (205, 230), (300, 150))):
        bar = mask_of(lambda d, x=x, top=top: d.rounded_rectangle((x, top, x + 80, 410), radius=14, fill=255))
        shadow(img, bar, 8, 10, 70)
        fill(img, bar, (150, 240, 90), (40, 160, 40), 0, outline=(20, 80, 30, 255), width=6)
    arrow = mask_of(lambda d: (d.line((95, 260, 200, 180, 290, 120, 360, 80), fill=255, width=24, joint='curve'),
                               d.polygon([(330, 50), (410, 55), (385, 130)], fill=255)))
    fill(img, arrow, (255, 240, 140), (240, 170, 40), 90, outline=(130, 80, 20, 255), width=5, shade=False)
    gloss(img, inner, (60, 40, 330, 200), 90)
    finish(img, inner, 'skills')


def heart_mask(cx, cy, r):
    return mask_of(lambda d: (d.ellipse((cx - r, cy - r * 0.9, cx + r * 0.05, cy + r * 0.15), fill=255),
                              d.ellipse((cx - r * 0.05, cy - r * 0.9, cx + r, cy + r * 0.15), fill=255),
                              d.polygon([(cx - r * 0.97, cy - r * 0.2), (cx + r * 0.97, cy - r * 0.2), (cx, cy + r)], fill=255)))


def relationships():
    """Contacts (user 2026-10-24: a generic phone contacts icon, as on Android and iPhone): an address book card
    with a person's head and shoulders in a circle and the index tabs on its right edge."""
    img, inner = tile((120, 190, 245), (30, 100, 200), (15, 60, 140, 255))
    card = mask_of(lambda d: d.rounded_rectangle((105, 80, 385, 432), radius=34, fill=255))
    shadow(img, card)
    fill(img, card, (255, 255, 255), (214, 222, 232), 90, outline=(40, 70, 120, 255), width=6)
    for i, colour in enumerate(((235, 90, 80), (250, 180, 50), (90, 190, 90), (70, 150, 230))):
        t = mask_of(lambda d, i=i: d.rounded_rectangle((372, 118 + i * 76, 420, 172 + i * 76), radius=14, fill=255))
        fill(img, t, tuple(min(255, c + 40) for c in colour), colour, 90, outline=(40, 70, 120, 255), width=5, shade=False)
    ring = mask_of(lambda d: d.ellipse((150, 140, 340, 330), fill=255))
    fill(img, ring, (205, 214, 226), (160, 172, 190), 90, shade=False)
    who = ImageChops.multiply(person(245, 250, 0.95), ring)
    fill(img, who, (110, 122, 140), (70, 82, 100), 90, shade=False)
    for k in range(3):
        line = mask_of(lambda d, k=k: d.rounded_rectangle((160, 352 + k * 24, 330 - k * 40, 364 + k * 24), radius=6, fill=255))
        tinted(img, line, (150, 162, 180), 230)
    gloss(img, card, (60, 30, 360, 200), 110)
    finish(img, inner, 'relationships')


def simology():
    """Simology (the Sim's traits page): a Sim's head and shoulders with three trait bubbles round it."""
    img, inner = tile((175, 225, 255), (60, 120, 210), (25, 70, 150, 255))
    for cx, cy, r, c in ((120, 150, 52, (250, 200, 60)), (395, 135, 46, (120, 210, 90)), (410, 300, 40, (240, 110, 160))):
        b = mask_of(lambda d, cx=cx, cy=cy, r=r: d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255))
        shadow(img, b, 6, 8, 70)
        fill(img, b, tuple(min(255, v + 50) for v in c), c, 90, outline=(30, 60, 120, 255), width=5)
        gloss(img, b, (cx - r, cy - r, cx + r, cy), 120)
    who = person(250, 330, 1.55)
    shadow(img, who)
    fill(img, who, (255, 255, 255), (200, 212, 230), 90, outline=(30, 60, 120, 255), width=7)
    gloss(img, who, (150, 150, 350, 300), 110)
    finish(img, inner, 'simology')


def souls_journey():
    """Soul's Journey: a soul sitting cross-legged in a glow, on a dusk-blue tile."""
    img, inner = tile((110, 140, 235), (45, 40, 140), (30, 25, 100, 255))
    glow = mask_of(lambda d: d.ellipse((96, 96, 416, 416), fill=255)).filter(ImageFilter.GaussianBlur(30))
    w = Image.new('RGBA', (N, N), (255, 255, 255, 0))
    w.putalpha(ImageChops.multiply(glow.point(lambda v: v * 0.4), inner))
    img.alpha_composite(w)
    ring = mask_of(lambda d: d.ellipse((120, 120, 392, 392), outline=255, width=14))
    tinted(img, ring, (255, 255, 255), 200)
    soul = mask_of(lambda d: (d.ellipse((222, 150, 290, 218), fill=255),
                              d.polygon([(256, 222), (318, 318), (194, 318)], fill=255),
                              d.rounded_rectangle((176, 236, 336, 262), radius=13, fill=255),
                              d.ellipse((160, 300, 352, 352), fill=255)))
    shadow(img, soul, 6, 8, 60)
    tinted(img, soul.filter(ImageFilter.MaxFilter(13)), (40, 35, 120), 255)
    tinted(img, soul, (255, 255, 255), 255)
    finish(img, inner, 'souls_journey')


def inventory():
    img, inner = tile((215, 240, 200), (110, 160, 80), (55, 95, 35, 255))
    tinted(img, mask_of(lambda d: d.rounded_rectangle((350, 350, 465, 465), radius=16, fill=255)), (55, 95, 35))
    flap = mask_of(lambda d: d.rounded_rectangle((180, 90, 330, 170), radius=40, outline=255, width=22))
    fill(img, flap, (170, 110, 60), (110, 65, 30), 90, outline=(60, 35, 15, 255), width=4, shade=False)
    bag = mask_of(lambda d: d.rounded_rectangle((110, 140, 400, 430), radius=80, fill=255))
    shadow(img, bag)
    fill(img, bag, (230, 160, 90), (160, 90, 40), 90, outline=(80, 45, 15, 255), width=7)
    top = ImageChops.multiply(bag, mask_of(lambda d: d.rounded_rectangle((110, 140, 400, 260), radius=60, fill=255)))
    fill(img, top, (200, 125, 60), (150, 85, 35), 90, shade=False)
    pocket = mask_of(lambda d: d.rounded_rectangle((180, 290, 330, 390), radius=24, fill=255))
    fill(img, pocket, (210, 140, 75), (150, 85, 35), 90, outline=(80, 45, 15, 255), width=5, shade=False)
    buckle = mask_of(lambda d: d.rounded_rectangle((235, 235, 275, 285), radius=8, fill=255))
    fill(img, buckle, (255, 230, 140), (200, 150, 40), 90, outline=(110, 75, 15, 255), width=4, shade=False)
    gloss(img, bag, (120, 150, 350, 280), 110)
    finish(img, inner, 'inventory')


def needs():
    img, inner = tile((210, 255, 200), (70, 180, 80), (30, 110, 40, 255))
    tinted(img, heart_mask(405, 400, 58), (30, 105, 40))
    for k in range(3):
        y = 130 + k * 95
        back = mask_of(lambda d, y=y: d.rounded_rectangle((90, y, 420, y + 56), radius=28, fill=255))
        shadow(img, back, 6, 8, 70)
        fill(img, back, (60, 70, 80), (30, 35, 45), 90, outline=(20, 50, 25, 255), width=5, shade=False)
        w = (380, 300, 210)[k]
        c0, c1 = ((150, 240, 90), (40, 160, 40)) if k < 2 else ((255, 230, 100), (230, 150, 30))
        bar = mask_of(lambda d, y=y, w=w: d.rounded_rectangle((96, y + 6, 96 + w, y + 50), radius=22, fill=255))
        fill(img, bar, c0, c1, 90, shade=False)
        gloss(img, bar, (96, y, 96 + w, y + 28), 150)
    finish(img, inner, 'needs')


def social_groups():
    img, inner = tile((200, 230, 255), (90, 130, 230), (40, 60, 150, 255))
    tinted(img, mask_of(lambda d: d.polygon([(330, 360), (470, 360), (470, 470), (330, 470)], fill=0)), (0, 0, 0))
    for (cx, cy, s, c0, c1) in ((150, 300, 0.9, (255, 200, 120), (225, 130, 50)),
                                (362, 300, 0.9, (200, 160, 255), (130, 80, 210)),
                                (256, 330, 1.15, (130, 230, 255), (40, 140, 220))):
        m = person(cx, cy, s)
        shadow(img, m)
        fill(img, m, c0, c1, 90, outline=(30, 40, 90, 255), width=6)
    gloss(img, inner, (60, 40, 330, 200), 80)
    finish(img, inner, 'social_groups')


def owned_businesses():
    img, inner = tile((255, 240, 190), (230, 175, 60), (140, 95, 20, 255))
    coin = mask_of(lambda d: d.ellipse((345, 345, 465, 465), outline=255, width=20))
    tinted(img, coin, (140, 90, 15))
    shop = mask_of(lambda d: d.rectangle((110, 200, 400, 420), fill=255))
    shadow(img, shop)
    fill(img, shop, (255, 255, 255), (215, 220, 230), 90, outline=(90, 60, 25, 255), width=7)
    door = mask_of(lambda d: d.rectangle((215, 300, 295, 420), fill=255))
    fill(img, door, (120, 190, 255), (50, 110, 200), 90, outline=(40, 50, 80, 255), width=5, shade=False)
    win = mask_of(lambda d: (d.rectangle((130, 300, 195, 365), fill=255), d.rectangle((315, 300, 380, 365), fill=255)))
    fill(img, win, (170, 225, 255), (70, 140, 220), 90, outline=(40, 50, 80, 255), width=4, shade=False)
    stripes = Image.new('L', (N, N), 0)
    sd = ImageDraw.Draw(stripes)
    for k in range(6):
        x0 = 90 + k * 55
        sd.polygon([(x0, 150), (x0 + 55, 150), (x0 + 55, 250), (x0, 250)], fill=255 if k % 2 == 0 else 0)
    awning = mask_of(lambda d: (d.polygon([(90, 250), (420, 250), (400, 150), (110, 150)], fill=255),
                                *[d.ellipse((90 + k * 55, 222, 145 + k * 55, 278), fill=255) for k in range(6)]))
    shadow(img, awning)
    fill(img, awning, (255, 255, 255), (230, 230, 235), 90, outline=(110, 30, 30, 255), width=6, shade=False)
    fill(img, ImageChops.multiply(awning, stripes), (255, 90, 80), (200, 35, 35), 90, shade=False)
    gloss(img, awning, (100, 120, 400, 220), 120)
    finish(img, inner, 'owned_businesses')


def household():
    img, inner = tile((255, 225, 200), (215, 120, 80), (130, 60, 35, 255))
    tinted(img, heart_mask(410, 405, 52), (130, 55, 30))
    roof = mask_of(lambda d: d.polygon([(70, 250), (256, 85), (442, 250)], fill=255))
    body = mask_of(lambda d: d.rectangle((115, 225, 397, 430), fill=255))
    shadow(img, ImageChops.lighter(roof, body))
    fill(img, body, (255, 250, 240), (220, 205, 190), 90, outline=(90, 45, 25, 255), width=7)
    fill(img, roof, (235, 95, 70), (175, 45, 30), 90, outline=(90, 25, 15, 255), width=7)
    for (cx, cy, s) in ((205, 410, 0.75), (305, 410, 0.75), (256, 425, 0.5)):
        m = person(cx, cy, s)
        fill(img, ImageChops.multiply(m, body), (110, 190, 255), (40, 110, 210), 90, shade=False)
    gloss(img, roof, (110, 90, 330, 230), 120)
    finish(img, inner, 'household')


def weather():
    img, inner = tile((175, 225, 255), (60, 140, 235), (30, 75, 160, 255))
    tinted(img, mask_of(lambda d: [d.line((380 + k * 32, 360, 365 + k * 32, 420), fill=255, width=14) for k in range(3)]),
           (25, 70, 160))
    sun = mask_of(lambda d: d.ellipse((150, 90, 330, 270), fill=255))
    rays = Image.new('L', (N, N), 0)
    rd = ImageDraw.Draw(rays)
    for k in range(10):
        a = 2 * math.pi * k / 10
        rd.polygon([(240 + 105 * math.cos(a - 0.12), 180 + 105 * math.sin(a - 0.12)),
                    (240 + 150 * math.cos(a), 180 + 150 * math.sin(a)),
                    (240 + 105 * math.cos(a + 0.12), 180 + 105 * math.sin(a + 0.12))], fill=255)
    fill(img, rays, (255, 225, 90), (245, 160, 30), 90, shade=False)
    shadow(img, sun)
    fill(img, sun, (255, 240, 120), (245, 160, 30), 90, outline=(170, 95, 15, 255), width=6)
    cloud = mask_of(lambda d: (d.ellipse((90, 250, 230, 390), fill=255), d.ellipse((170, 190, 340, 360), fill=255),
                               d.ellipse((270, 250, 410, 390), fill=255), d.rounded_rectangle((110, 300, 390, 395), radius=45, fill=255)))
    shadow(img, cloud)
    fill(img, cloud, (255, 255, 255), (200, 215, 235), 90, outline=(40, 70, 130, 255), width=7)
    gloss(img, cloud, (110, 200, 360, 320), 120)
    gloss(img, sun, (160, 95, 280, 180), 150)
    finish(img, inner, 'weather')


def wants():
    img, inner = tile((255, 240, 200), (235, 170, 90), (140, 90, 30, 255))
    tinted(img, mask_of(lambda d: d.ellipse((350, 340, 470, 460), outline=255, width=18)), (140, 85, 25))
    cover = mask_of(lambda d: d.rounded_rectangle((110, 80, 380, 430), radius=26, fill=255))
    shadow(img, cover)
    fill(img, cover, (90, 160, 230), (35, 90, 170), 90, outline=(25, 45, 90, 255), width=7)
    page = mask_of(lambda d: d.rounded_rectangle((150, 100, 365, 410), radius=14, fill=255))
    fill(img, page, (255, 253, 240), (235, 228, 205), 90, shade=False)
    lines = Image.new('L', (N, N), 0)
    ld = ImageDraw.Draw(lines)
    for k in range(6):
        y = 150 + k * 42
        ld.line((175, y, 345, y), fill=255, width=6)
    tinted(img, lines, (120, 160, 210), 200)
    tinted(img, mask_of(lambda d: d.line((175, 150, 290, 150), fill=255, width=12)), (40, 60, 120), 230)
    rings = Image.new('L', (N, N), 0)
    rd = ImageDraw.Draw(rings)
    for k in range(6):
        y = 125 + k * 52
        rd.rounded_rectangle((128, y, 172, y + 16), radius=8, fill=255)
    fill(img, rings, (245, 245, 250), (150, 155, 170), 0, outline=(60, 60, 70, 255), width=4, shade=False)
    pen = mask_of(lambda d: d.line((260, 400, 420, 150), fill=255, width=40))
    shadow(img, pen)
    fill(img, pen, (255, 120, 110), (200, 40, 50), 0, outline=(100, 20, 25, 255), width=5)
    tip = mask_of(lambda d: d.polygon([(245, 395), (285, 420), (238, 440)], fill=255))
    fill(img, tip, (250, 230, 190), (200, 160, 110), 0, outline=(90, 60, 30, 255), width=4, shade=False)
    gloss(img, cover, (100, 70, 330, 220), 110)
    finish(img, inner, 'wants')


def god_view():
    """Settings > God View: an isometric house seen from above, a camera eye over it."""
    img, inner = tile((200, 235, 200), (70, 150, 120), (30, 90, 70, 255))
    ground = mask_of(lambda d: d.polygon([(256, 300), (470, 400), (256, 500), (42, 400)], fill=255))
    tinted(img, ground, (30, 90, 70), 150)
    # the house in isometric view: left wall, right wall, roof
    left = mask_of(lambda d: d.polygon([(130, 300), (256, 360), (256, 470), (130, 410)], fill=255))
    right = mask_of(lambda d: d.polygon([(256, 360), (382, 300), (382, 410), (256, 470)], fill=255))
    roof = mask_of(lambda d: d.polygon([(110, 300), (256, 370), (402, 300), (300, 200), (212, 200)], fill=255))
    house = ImageChops.lighter(ImageChops.lighter(left, right), roof)
    shadow(img, house)
    fill(img, left, (250, 245, 230), (200, 190, 170), 90, outline=(90, 70, 50, 255), width=5, shade=False)
    fill(img, right, (215, 205, 185), (160, 150, 130), 90, outline=(90, 70, 50, 255), width=5, shade=False)
    fill(img, roof, (235, 110, 80), (170, 60, 40), 90, outline=(100, 35, 20, 255), width=6)
    gloss(img, roof, (180, 190, 330, 290), 120)
    # the eye above
    eye = mask_of(lambda d: d.ellipse((176, 60, 336, 150), fill=255))
    shadow(img, eye, alpha=60)
    fill(img, eye, (255, 255, 255), (200, 215, 230), 90, outline=(40, 60, 90, 255), width=6, shade=False)
    iris = mask_of(lambda d: d.ellipse((226, 75, 286, 135), fill=255))
    fill(img, iris, (90, 170, 240), (30, 80, 160), 90, shade=False)
    pupil = mask_of(lambda d: d.ellipse((243, 92, 269, 118), fill=255))
    tinted(img, pupil, (10, 20, 40), 255)
    finish(img, inner, 'god_view')


def cheats():
    """Home > Cheats: a magic wand over sparkles on a purple tile (the debug menu's testing cheats)."""
    img, inner = tile((215, 190, 255), (110, 60, 190), (60, 25, 120, 255))
    wand = mask_of(lambda d: d.line((150, 400, 330, 220), fill=255, width=46))
    tip = mask_of(lambda d: d.line((300, 250, 340, 210), fill=255, width=46))
    shadow(img, wand)
    fill(img, wand, (70, 70, 85), (20, 20, 30), 90, outline=(10, 10, 15, 255), width=5)
    fill(img, tip, (255, 255, 255), (200, 205, 220), 90, shade=False)
    def star(cx, cy, r):
        pts = []
        for k in range(8):
            a = -math.pi / 2 + k * math.pi / 4
            rr = r if k % 2 == 0 else r * 0.32
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        return mask_of(lambda d: d.polygon(pts, fill=255))
    for (cx, cy, r) in ((365, 170, 95), (240, 140, 45), (410, 300, 40), (320, 90, 28)):
        m = star(cx, cy, r)
        shadow(img, m, alpha=60)
        fill(img, m, (255, 250, 190), (250, 190, 40), 90, outline=(160, 100, 0, 255), width=4)
    finish(img, inner, 'cheats')


def figure(cx, top, s):
    """A standing Sim seen from the front, arms at its sides (mask): the Sims 1 family icon's people."""
    def draw(d):
        d.rounded_rectangle((cx - 26 * s, top, cx + 26 * s, top + 62 * s), radius=int(22 * s), fill=255)  # head
        d.rectangle((cx - 10 * s, top + 56 * s, cx + 10 * s, top + 72 * s), fill=255)  # neck
        d.rounded_rectangle((cx - 50 * s, top + 68 * s, cx + 50 * s, top + 190 * s), radius=int(20 * s), fill=255)  # torso
        d.rounded_rectangle((cx - 72 * s, top + 74 * s, cx - 46 * s, top + 200 * s), radius=int(13 * s), fill=255)  # arms
        d.rounded_rectangle((cx + 46 * s, top + 74 * s, cx + 72 * s, top + 200 * s), radius=int(13 * s), fill=255)
        d.rounded_rectangle((cx - 44 * s, top + 180 * s, cx - 4 * s, top + 330 * s), radius=int(12 * s), fill=255)  # legs
        d.rounded_rectangle((cx + 4 * s, top + 180 * s, cx + 44 * s, top + 330 * s), radius=int(12 * s), fill=255)
    return mask_of(draw)


def sims_forever():
    """Home > Sims Forever: the Sims 1 family icon (user 2026-10-24 02:24Z), an adult and a child hand in hand
    in pale cyan on deep blue, redrawn smooth on our tile."""
    img, inner = tile((70, 140, 230), (10, 40, 130), (5, 20, 80, 255))
    glow = Image.new('L', (N, N), 0)
    ImageDraw.Draw(glow).ellipse((90, 60, 420, 420), fill=120)
    glow = ImageChops.multiply(glow.filter(ImageFilter.GaussianBlur(70)), inner)
    g = Image.new('RGBA', (N, N), (120, 190, 255, 0))
    g.putalpha(glow)
    img.alpha_composite(g)
    adult = figure(205, 70, 1.12)
    child = figure(334, 228, 0.66)  # its hand at the adult's
    people = ImageChops.lighter(adult, child)
    shadow(img, people, dx=14, dy=10, alpha=140)
    fill(img, people, (215, 250, 248), (120, 200, 205), 0, outline=(10, 40, 90, 255), width=5)
    gloss(img, people, (120, 40, 300, 220), 110)
    finish(img, inner, 'sims_forever')


# The Needs app's own need icons (user 2026-10-24 01:58Z: HD, not the game's small ones): one glossy object each,
# no tile, in the game's need colours.
def plain(name, img):
    img.resize((128, 128), Image.LANCZOS).save(os.path.join(OUT, 'need_' + name + '.png'))


def blank():
    return Image.new('RGBA', (N, N), (0, 0, 0, 0))


def need_hunger():  # a drumstick
    img = blank()
    meat = mask_of(lambda d: d.ellipse((90, 70, 360, 330), fill=255))
    bone = mask_of(lambda d: (d.line((300, 280, 420, 400), fill=255, width=46),
                              d.ellipse((380, 380, 450, 450), fill=255), d.ellipse((410, 340, 480, 410), fill=255)))
    shadow(img, bone)
    fill(img, bone, (255, 252, 240), (210, 200, 180), 90, outline=(120, 100, 80, 255), width=7)
    shadow(img, meat)
    fill(img, meat, (240, 160, 80), (170, 80, 30), 90, outline=(100, 45, 15, 255), width=8)
    gloss(img, meat, (120, 90, 290, 200), 150)
    plain('hunger', img)


def need_hygiene():  # bubbles
    img = blank()
    for (cx, cy, r) in ((210, 280, 150), (390, 160, 85), (395, 380, 60)):
        m = mask_of(lambda d, cx=cx, cy=cy, r=r: d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255))
        shadow(img, m, alpha=60)
        fill(img, m, (215, 245, 255), (90, 170, 230), 120, outline=(40, 100, 160, 255), width=7)
        gloss(img, m, (cx - r * 0.7, cy - r * 0.85, cx + r * 0.2, cy - r * 0.05), 220)
    plain('hygiene', img)


def need_bladder():  # a drop
    img = blank()
    m = mask_of(lambda d: (d.ellipse((120, 200, 400, 480), fill=255), d.polygon([(260, 30), (128, 300), (392, 300)], fill=255)))
    shadow(img, m)
    fill(img, m, (255, 245, 150), (225, 175, 20), 90, outline=(130, 95, 0, 255), width=8)
    gloss(img, m, (160, 220, 300, 380), 190)
    plain('bladder', img)


def need_energy():  # a lightning bolt
    img = blank()
    m = mask_of(lambda d: d.polygon([(300, 30), (110, 290), (240, 290), (190, 490), (410, 200), (275, 200), (350, 30)], fill=255))
    shadow(img, m)
    fill(img, m, (255, 250, 170), (240, 170, 20), 90, outline=(140, 90, 0, 255), width=8)
    gloss(img, m, (150, 40, 360, 260), 170)
    plain('energy', img)


def need_fun():  # a star
    img = blank()
    pts = []
    for k in range(10):
        a = -math.pi / 2 + k * math.pi / 5
        r = 230 if k % 2 == 0 else 100
        pts.append((256 + r * math.cos(a), 270 + r * math.sin(a)))
    m = mask_of(lambda d: d.polygon(pts, fill=255))
    shadow(img, m)
    fill(img, m, (255, 200, 240), (210, 70, 170), 90, outline=(120, 25, 95, 255), width=8)
    gloss(img, m, (130, 60, 340, 250), 170)
    plain('fun', img)


def need_social():  # two speech bubbles
    img = blank()
    back = mask_of(lambda d: (d.rounded_rectangle((210, 60, 480, 260), radius=90, fill=255),
                              d.polygon([(400, 240), (450, 320), (350, 250)], fill=255)))
    shadow(img, back, alpha=60)
    fill(img, back, (190, 230, 255), (80, 150, 220), 90, outline=(30, 80, 140, 255), width=7)
    front = mask_of(lambda d: (d.rounded_rectangle((40, 170, 340, 400), radius=100, fill=255),
                               d.polygon([(90, 380), (60, 470), (170, 390)], fill=255)))
    shadow(img, front)
    fill(img, front, (180, 255, 170), (60, 180, 60), 90, outline=(25, 100, 25, 255), width=8)
    gloss(img, front, (80, 180, 290, 290), 170)
    plain('social', img)


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for f in (settings, calendar, build, game_options, phone_options, vr_options, my_sim, aspirations, career, skills,
              relationships, inventory, needs, social_groups, owned_businesses, household, weather, wants,
              need_hunger, need_hygiene, need_bladder, need_energy, need_fun, need_social, god_view, sims_forever, simology, souls_journey, cheats):
        f()
    print('phone icons in', OUT)
