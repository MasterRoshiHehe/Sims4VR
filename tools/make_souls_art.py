"""Soul's Journey ring fill (re/liveui/art/SimInfoAspirationPanel/110.png, the RankedStatUnfinishedBusinessMeter's
coloured dashes) cut into its 8 dashes, so the phone can light them one rank at a time: art/souls/fill_<n>.png in the
ring's order (clockwise from the bottom left), and their places in the 202x170 ring printed for hud.cpp.

Usage: python3 tools/make_souls_art.py   (from the repo root)
"""
import math, os
import numpy as np
from PIL import Image

SRC = 're/liveui/art/SimInfoAspirationPanel/110.png'
OUT = 'art/souls'
CENTRE = (102, 95)  # the ring's middle in its picture

a = np.asarray(Image.open(SRC).convert('RGBA'))
mask = a[:, :, 3] > 20
H, W = mask.shape
label = np.zeros((H, W), int)
n = 0
for y in range(H):
    for x in range(W):
        if mask[y, x] and not label[y, x]:
            n += 1
            stack = [(y, x)]
            label[y, x] = n
            while stack:
                cy, cx = stack.pop()
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        yy, xx = cy + dy, cx + dx
                        if 0 <= yy < H and 0 <= xx < W and mask[yy, xx] and not label[yy, xx]:
                            label[yy, xx] = n
                            stack.append((yy, xx))
parts = []
for i in range(1, n + 1):
    ys, xs = np.nonzero(label == i)
    angle = math.degrees(math.atan2(ys.mean() - CENTRE[1], xs.mean() - CENTRE[0]))
    parts.append(((angle - 120) % 360, i, xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))
parts.sort()
os.makedirs(OUT, exist_ok=True)
for k, (_, i, x0, y0, x1, y1) in enumerate(parts):
    piece = a[y0:y1, x0:x1].copy()
    piece[label[y0:y1, x0:x1] != i] = 0
    Image.fromarray(piece).save(os.path.join(OUT, 'fill_%d.png' % k))
    print('fill_%d: {%d, %d, %d, %d}' % (k, x0, y0, x1 - x0, y1 - y0))
