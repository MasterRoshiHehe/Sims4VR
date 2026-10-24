"""Icons for the build tablet's tool tray (src/hud_buildtray.inc), drawn in the style of the vanilla build UI's
tool icons (re/buildui/art/buildbuyinspector, CameraControl): a white glyph with a soft top-to-bottom grey
gradient, a thin dark rim and a soft shadow below it. The tray tints them green while a tool is on.

Writes art/tray_icons/<name>.png (96 x 96). Then: python3 tools/pack_hud_art.py.
Usage: python3 tools/make_tray_icons.py   (from the repo root)
"""
import math, os
from PIL import Image, ImageDraw, ImageFilter, ImageChops

S = 96          # output size
K = 4           # supersampling
N = S * K
OUT = 'art/tray_icons'


def P(x, y):
    """0..1 icon coordinates to supersampled pixels."""
    return (x * N, y * N)


def W(w):
    return max(1, int(w * N))


def finish(mask):
    """The vanilla look from a glyph mask (L, N x N)."""
    mask = mask.resize((S, S), Image.LANCZOS)
    grad = Image.new('L', (1, S))
    for y in range(S):
        grad.putpixel((0, y), int(255 - 60 * y / (S - 1)))
    grad = grad.resize((S, S))
    fill = Image.merge('RGBA', (grad, grad, ImageChops.add(grad, Image.new('L', (S, S), 8)), mask))
    rim = mask.filter(ImageFilter.MaxFilter(3))
    shadow = rim.filter(ImageFilter.GaussianBlur(2.2))
    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    sh = Image.new('RGBA', (S, S), (10, 20, 35, 0))
    sh.putalpha(shadow.point(lambda a: int(a * 0.55)))
    out.alpha_composite(sh, (0, 2))
    edge = Image.new('RGBA', (S, S), (70, 80, 95, 0))
    edge.putalpha(rim.point(lambda a: int(a * 0.8)))
    out.alpha_composite(edge)
    out.alpha_composite(fill)
    return out


def canvas():
    m = Image.new('L', (N, N), 0)
    return m, ImageDraw.Draw(m)


def arrow(d, x0, y0, x1, y1, w=0.06, head=0.14):
    ang = math.atan2(y1 - y0, x1 - x0)
    bx, by = x1 - head * 0.8 * math.cos(ang), y1 - head * 0.8 * math.sin(ang)
    d.line([P(x0, y0), P(bx, by)], fill=255, width=W(w))
    l = (x1 - head * math.cos(ang - 0.5), y1 - head * math.sin(ang - 0.5))
    r = (x1 - head * math.cos(ang + 0.5), y1 - head * math.sin(ang + 0.5))
    d.polygon([P(x1, y1), P(*l), P(*r)], fill=255)


def arc_arrow(d, cx, cy, r, a0, a1, w=0.08, flip=False):
    box = [P(cx - r, cy - r), P(cx + r, cy + r)]
    d.arc(box, a0, a1, fill=255, width=W(w))
    a = math.radians(a1 if not flip else a0)
    tx, ty = cx + r * math.cos(a), cy + r * math.sin(a)
    tang = a + (math.pi / 2 if not flip else -math.pi / 2)
    h = 0.17
    tip = (tx + h * 0.6 * math.cos(tang), ty + h * 0.6 * math.sin(tang))
    l = (tx + h * 0.55 * math.cos(a), ty + h * 0.55 * math.sin(a))
    rr = (tx - h * 0.55 * math.cos(a), ty - h * 0.55 * math.sin(a))
    d.polygon([P(*tip), P(*l), P(*rr)], fill=255)


def box(d, x0, y0, x1, y1, w=None, fill=False):
    if fill:
        d.rectangle([P(x0, y0), P(x1, y1)], fill=255)
    else:
        d.rectangle([P(x0, y0), P(x1, y1)], outline=255, width=W(w or 0.06))


def cube(d, cx, cy, s, w=0.05):
    """A box in the build UI's isometric way."""
    t = [(cx, cy - s), (cx + s, cy - s / 2), (cx, cy), (cx - s, cy - s / 2)]
    b = [(x, y + s) for x, y in t]
    for a, c in zip(t, t[1:] + t[:1]):
        d.line([P(*a), P(*c)], fill=255, width=W(w))
    for i in (1, 2, 3):
        d.line([P(*t[i]), P(*b[i])], fill=255, width=W(w))
    d.line([P(*b[1]), P(*b[2]), P(*b[3])], fill=255, width=W(w), joint='curve')
    d.polygon([P(*t[0]), P(*t[1]), P(*t[2]), P(*t[3])], fill=255)


def roof(d, peak=0.22, w=0.07, curved=False, side=None):
    if curved:
        pts = [(0.12 + 0.76 * i / 40, 0.70 - 0.48 * math.sin(math.pi * i / 40) ** 0.7) for i in range(41)]
        d.line([P(*p) for p in pts], fill=255, width=W(w), joint='curve')
    else:
        d.line([P(0.10, 0.62), P(0.5, peak), P(0.90, 0.62)], fill=255, width=W(w), joint='curve')
    box(d, 0.22, 0.58, 0.78, 0.84, 0.06)
    if side == 'left':
        d.polygon([P(0.10, 0.62), P(0.5, peak), P(0.5, peak + 0.12), P(0.18, 0.66)], fill=255)


ICONS = {}


def icon(name):
    def reg(fn):
        ICONS[name] = fn
        return fn
    return reg


@icon('free_place')
def _(d):
    for x1, y1 in ((0.5, 0.08), (0.5, 0.92), (0.08, 0.5), (0.92, 0.5)):
        arrow(d, 0.5, 0.5, x1, y1, 0.07, 0.18)
    d.ellipse([P(0.40, 0.40), P(0.60, 0.60)], fill=255)


@icon('copies')
def _(d):
    cube(d, 0.38, 0.40, 0.22)
    cube(d, 0.62, 0.58, 0.22)


@icon('snap')
def _(d):
    box(d, 0.14, 0.14, 0.86, 0.86, 0.06)
    d.line([P(0.5, 0.14), P(0.5, 0.86)], fill=255, width=W(0.04))
    d.line([P(0.14, 0.5), P(0.86, 0.5)], fill=255, width=W(0.04))
    box(d, 0.5, 0.5, 0.86, 0.86, fill=True)


@icon('grid')
def _(d):
    for k in range(4):
        v = 0.14 + k * 0.24
        d.line([P(v, 0.14), P(v, 0.86)], fill=255, width=W(0.05))
        d.line([P(0.14, v), P(0.86, v)], fill=255, width=W(0.05))


@icon('turn_left')
def _(d):
    arc_arrow(d, 0.5, 0.52, 0.32, 200, 520 - 40, 0.09, flip=True)


@icon('turn_right')
def _(d):
    arc_arrow(d, 0.5, 0.52, 0.32, 20 + 40, 340, 0.09)


@icon('raise')
def _(d):
    cube(d, 0.5, 0.62, 0.22)
    arrow(d, 0.5, 0.40, 0.5, 0.04, 0.07, 0.18)


@icon('lower')
def _(d):
    cube(d, 0.5, 0.30, 0.22)
    arrow(d, 0.5, 0.64, 0.5, 0.97, 0.07, 0.18)


@icon('bigger')
def _(d):
    box(d, 0.32, 0.32, 0.68, 0.68, fill=True)
    for sx, sy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        arrow(d, 0.5 + sx * 0.24, 0.5 + sy * 0.24, 0.5 + sx * 0.44, 0.5 + sy * 0.44, 0.06, 0.15)


@icon('smaller')
def _(d):
    box(d, 0.38, 0.38, 0.62, 0.62, fill=True)
    for sx, sy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        arrow(d, 0.5 + sx * 0.46, 0.5 + sy * 0.46, 0.5 + sx * 0.22, 0.5 + sy * 0.22, 0.06, 0.15)


@icon('swatch')
def _(d):
    # The design tool's fan of colour cards, as the vanilla swatch button.
    for k, a in enumerate((-35, -10, 15)):
        card = Image.new('L', (N, N), 0)
        cd = ImageDraw.Draw(card)
        cd.rounded_rectangle([P(0.40, 0.10), P(0.60, 0.80)], radius=W(0.04), fill=255)
        cd.rectangle([P(0.43, 0.14 + 0.0), P(0.57, 0.30)], fill=0)
        card = card.rotate(-a, center=P(0.5, 0.85), resample=Image.BICUBIC)
        d.bitmap((0, 0), card, fill=255)


@icon('slot')
def _(d):
    d.rectangle([P(0.08, 0.62), P(0.92, 0.72)], fill=255)
    d.line([P(0.18, 0.72), P(0.18, 0.92)], fill=255, width=W(0.06))
    d.line([P(0.82, 0.72), P(0.82, 0.92)], fill=255, width=W(0.06))
    for x in (0.25, 0.5, 0.75):
        d.ellipse([P(x - 0.07, 0.44), P(x + 0.07, 0.58)], outline=255, width=W(0.04))
    d.ellipse([P(0.43, 0.44), P(0.57, 0.58)], fill=255)
    arrow(d, 0.30, 0.22, 0.72, 0.22, 0.06, 0.15)


@icon('quarter')
def _(d):
    box(d, 0.14, 0.14, 0.86, 0.86, 0.06)
    d.line([P(0.14, 0.14), P(0.86, 0.86)], fill=255, width=W(0.05))
    d.line([P(0.86, 0.14), P(0.14, 0.86)], fill=255, width=W(0.05))
    d.polygon([P(0.14, 0.14), P(0.86, 0.14), P(0.5, 0.5)], fill=255)


@icon('fill_room')
def _(d):
    d.polygon([P(0.20, 0.40), P(0.55, 0.15), P(0.85, 0.50), P(0.50, 0.80)], outline=255, width=W(0.06))
    d.polygon([P(0.24, 0.44), P(0.55, 0.22), P(0.62, 0.32), P(0.30, 0.55)], fill=255)
    d.ellipse([P(0.10, 0.62), P(0.26, 0.86)], fill=255)


@icon('one_wall')
def _(d):
    d.polygon([P(0.08, 0.30), P(0.50, 0.14), P(0.50, 0.70), P(0.08, 0.86)], outline=255, width=W(0.05))
    d.polygon([P(0.50, 0.14), P(0.92, 0.30), P(0.92, 0.86), P(0.50, 0.70)], fill=255)


@icon('roof_eaves')
def _(d):
    roof(d, side='left')


@icon('roof_height')
def _(d):
    roof(d, peak=0.34)
    arrow(d, 0.5, 0.26, 0.5, 0.0, 0.06, 0.14)


@icon('roof_curve')
def _(d):
    roof(d, curved=True)


@icon('eraser')
def _(d):
    e = Image.new('L', (N, N), 0)
    ed = ImageDraw.Draw(e)
    ed.rounded_rectangle([P(0.18, 0.36), P(0.82, 0.64)], radius=W(0.05), outline=255, width=W(0.06))
    ed.rectangle([P(0.18, 0.36), P(0.46, 0.64)], fill=255)
    e = e.rotate(35, center=P(0.5, 0.5), resample=Image.BICUBIC)
    d.bitmap((0, 0), e, fill=255)
    d.line([P(0.30, 0.88), P(0.90, 0.88)], fill=255, width=W(0.05))


@icon('move_objects')
def _(d):
    cube(d, 0.40, 0.42, 0.24)
    c2 = Image.new('L', (N, N), 0)
    cube(ImageDraw.Draw(c2), 0.62, 0.52, 0.24)
    d.bitmap((0, 0), c2.point(lambda a: a // 2), fill=255)


@icon('hidden')
def _(d):
    d.ellipse([P(0.08, 0.30), P(0.92, 0.70)], outline=255, width=W(0.06))
    d.ellipse([P(0.36, 0.36), P(0.64, 0.64)], fill=255)


@icon('live_edit')
def _(d):
    d.polygon([P(0.5, 0.08), P(0.80, 0.52), P(0.20, 0.52)], fill=255)
    d.polygon([P(0.5, 0.30), P(0.88, 0.80), P(0.12, 0.80)], fill=255)
    d.rectangle([P(0.44, 0.80), P(0.56, 0.94)], fill=255)


@icon('exit_first_person')
def _(d):
    d.ellipse([P(0.14, 0.10), P(0.40, 0.36)], fill=255)
    d.pieslice([P(0.06, 0.40), P(0.48, 0.90)], 180, 360, fill=255)
    d.rectangle([P(0.06, 0.64), P(0.48, 0.90)], fill=255)
    arrow(d, 0.52, 0.55, 0.95, 0.55, 0.07, 0.18)


@icon('cat_objects')
def _(d):
    # A chair, the buy catalog's usual sign.
    d.rectangle([P(0.24, 0.10), P(0.36, 0.56)], fill=255)
    d.rectangle([P(0.24, 0.48), P(0.78, 0.58)], fill=255)
    d.rectangle([P(0.24, 0.58), P(0.33, 0.92)], fill=255)
    d.rectangle([P(0.69, 0.58), P(0.78, 0.92)], fill=255)


@icon('cat_walls')
def _(d):
    for row in range(4):
        y = 0.14 + row * 0.18
        off = 0.0 if row % 2 else 0.18
        x = 0.08 - off
        while x < 0.92:
            box(d, max(x, 0.08), y, min(x + 0.34, 0.92), y + 0.15, fill=True)
            x += 0.38
    d.rectangle([P(0.08, 0.86), P(0.92, 0.92)], fill=255)


@icon('cat_roofs')
def _(d):
    roof(d)


@icon('cat_terrain')
def _(d):
    d.polygon([P(0.04, 0.84), P(0.36, 0.30), P(0.54, 0.56), P(0.68, 0.40), P(0.96, 0.84)], fill=255)
    d.ellipse([P(0.66, 0.08), P(0.86, 0.28)], fill=255)


@icon('cat_cheats')
def _(d):
    # A wrench.
    d.line([P(0.24, 0.80), P(0.62, 0.40)], fill=255, width=W(0.13))
    d.ellipse([P(0.52, 0.10), P(0.92, 0.50)], fill=255)
    d.polygon([P(0.66, 0.06), P(0.82, 0.06), P(0.76, 0.30), P(0.70, 0.30)], fill=0)
    d.ellipse([P(0.16, 0.72), P(0.32, 0.88)], fill=255)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, fn in ICONS.items():
        m, d = canvas()
        fn(d)
        finish(m).save(os.path.join(OUT, name + '.png'))
    print('%d tray icons in %s' % (len(ICONS), OUT))


if __name__ == '__main__':
    main()
