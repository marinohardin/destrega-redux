"""Original Destrega-style app icon, drawn procedurally (no game assets).

Motif: a glowing green magic circle (rune ring + energy swirls + a crystal
shard), with a rough red-glowing wordmark across it. Everything is generated
from geometry, a fixed random seed and a macOS system font (Herculanum), so the
output is deterministic. No game assets are used.

The rendered result is committed as assets/icon/destrega.png, which the build
uses (tools/make_app.sh); rerun this only to change the design. Needs macOS
(for the font), Pillow and NumPy.

usage: python3 tools/make_icon.py assets/icon/destrega.png [--no-text]
"""
import math
import random
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
TEXT = "--no-text" not in sys.argv
S, M = 1024, 100            # canvas, macOS Big Sur margin
B = S - 2 * M               # 824 body
R = 185                     # corner radius
C = B / 2                   # body centre
CY = B * (0.47 if TEXT else 0.5)
rng = random.Random(1998)
SS = 2                      # supersampling for line work


def glow(layer, radii=(3, 10, 28, 60), gains=(1.0, 0.9, 0.7, 0.5)):
    """Additive multi-radius bloom of an RGB layer."""
    acc = np.asarray(layer).astype(np.float32)
    for r, g in zip(radii, gains):
        acc += np.asarray(layer.filter(ImageFilter.GaussianBlur(r))).astype(np.float32) * g
    return Image.fromarray(acc.clip(0, 255).astype(np.uint8))


def screen(a, b):
    return ImageChops.screen(a, b)


def lines_layer():
    return Image.new("RGB", (B * SS, B * SS), 0)


def down(img):
    return img.resize((B, B), Image.LANCZOS)


def P(x, y):
    return (x * SS, y * SS)


# --- background: deep green-black radial gradient -------------------------
yy, xx = np.mgrid[0:B, 0:B].astype(np.float32)
d = np.sqrt((xx - C) ** 2 + (yy - CY) ** 2) / (B * 0.75)
base = np.stack([6 + 10 * (1 - d), 12 + 30 * (1 - d), 8 + 14 * (1 - d)], 2).clip(0, 255)
bg = Image.fromarray(base.astype(np.uint8))

# --- magic circle ----------------------------------------------------------
ring = lines_layer()
dr = ImageDraw.Draw(ring)
GREEN = (120, 230, 90)
PALE = (200, 255, 170)
r_out, r_in = B * 0.36, B * 0.30


def circle(r, w, col):
    dr.ellipse((*P(C - r, CY - r), *P(C + r, CY + r)), outline=col, width=w * SS)


circle(r_out, 5, GREEN)
circle(r_out - 12, 2, (70, 160, 60))
circle(r_in, 3, GREEN)
circle(B * 0.17, 2, (80, 180, 70))

# rune glyphs between the rings: small random stroke clusters
n = 28
for i in range(n):
    a = 2 * math.pi * i / n
    rr = (r_out - 12 + r_in) / 2
    gx, gy = C + rr * math.cos(a), CY + rr * math.sin(a)
    for _ in range(rng.randint(2, 3)):
        ang = a + rng.choice([0, math.pi / 2, math.pi / 4, -math.pi / 4]) + rng.uniform(-.2, .2)
        L = rng.uniform(5, 11)
        ox, oy = rng.uniform(-4, 4), rng.uniform(-4, 4)
        dr.line((*P(gx + ox - L * math.cos(ang), gy + oy - L * math.sin(ang)),
                 *P(gx + ox + L * math.cos(ang), gy + oy + L * math.sin(ang))),
                fill=PALE, width=2 * SS)

# inscribed eight-point sigil (two overlapping squares)
for k in range(2):
    pts = [P(C + r_in * 0.98 * math.cos(k * math.pi / 4 + j * math.pi / 2),
             CY + r_in * 0.98 * math.sin(k * math.pi / 4 + j * math.pi / 2))
           for j in range(4)]
    dr.polygon(pts, outline=(90, 200, 80), width=2 * SS)

# --- energy swirls: tapered arcs sweeping around the circle -----------------
swirl = lines_layer()
ds = ImageDraw.Draw(swirl)
for s in range(5):
    a0 = rng.uniform(0, 2 * math.pi)
    span = rng.uniform(1.6, 2.6)
    r0 = rng.uniform(0.38, 0.47) * B
    grow = rng.uniform(-0.06, 0.08) * B
    steps = 90
    prev = None
    for t in range(steps + 1):
        u = t / steps
        a = a0 + span * u
        r = r0 + grow * u + 8 * math.sin(u * 9 + s)
        x, y = C + r * math.cos(a), CY + r * math.sin(a) * 0.92
        if prev:
            w = max(1, int(9 * math.sin(math.pi * u)))
            col = (int(150 + 80 * u), 255, int(90 + 60 * u))
            ds.line((*P(*prev), *P(x, y)), fill=col, width=w * SS)
        prev = (x, y)
# sparks
for _ in range(140):
    a = rng.uniform(0, 2 * math.pi)
    r = rng.gauss(0.36, 0.07) * B
    x, y = C + r * math.cos(a), CY + r * math.sin(a)
    s = rng.uniform(0.8, 2.6)
    ds.ellipse((*P(x - s, y - s), *P(x + s, y + s)), fill=(210, 255, 160))

# --- crystal shard in the centre -------------------------------------------
cr = lines_layer()
dc = ImageDraw.Draw(cr)
h, w = B * 0.15, B * 0.06
top, bot = (C, CY - h), (C, CY + h * 0.9)
left, right = (C - w, CY - h * 0.15), (C + w, CY - h * 0.15)
mid = (C + w * 0.15, CY - h * 0.05)
dc.polygon([P(*top), P(*left), P(*mid)], fill=(150, 240, 150))
dc.polygon([P(*top), P(*mid), P(*right)], fill=(225, 255, 215))
dc.polygon([P(*left), P(*bot), P(*mid)], fill=(70, 170, 90))
dc.polygon([P(*mid), P(*bot), P(*right)], fill=(110, 210, 120))
for a, b_ in [(top, left), (top, right), (left, bot), (right, bot), (top, mid), (mid, bot)]:
    dc.line((*P(*a), *P(*b_)), fill=(235, 255, 230), width=2 * SS)

ring, swirl, cr = down(ring), down(swirl), down(cr)
art = screen(screen(glow(ring), glow(swirl, gains=(0.8, 0.7, 0.5, 0.35))),
             glow(cr, radii=(4, 16, 50), gains=(0.6, 0.6, 0.5)))
body = screen(bg, art.filter(ImageFilter.GaussianBlur(3)))   # soften the green art; wordmark stays sharp

# --- wordmark ----------------------------------------------------------------
if TEXT:
    font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Herculanum.ttf", 230)
    word = "DESTREGA"
    tw = font.getbbox(word)
    tmp = Image.new("L", (tw[2] + 80, tw[3] + 80), 0)
    ImageDraw.Draw(tmp).text((40, 40 - tw[1] // 2), word, font=font, fill=255)
    tmp = tmp.crop(tmp.getbbox())
    # roughen: erode edges with noise for a scratched, hand-cut look
    a = np.asarray(tmp).astype(np.float32) / 255
    noise = np.asarray(Image.fromarray((np.random.default_rng(7).random((a.shape[0] // 4 + 1, a.shape[1] // 4 + 1)) * 255)
                                       .astype(np.uint8)).resize((a.shape[1], a.shape[0]), Image.BICUBIC)).astype(np.float32) / 255
    soft = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3))).astype(np.float32) / 255
    a = ((soft - 0.25 - 0.45 * (noise - 0.5)) * 6).clip(0, 1)
    mask = Image.fromarray((a * 255).astype(np.uint8))
    # fit to width, slight italic shear
    tw_ = int(B * 0.88)
    th_ = int(mask.height * tw_ / mask.width * 1.25)
    mask = mask.resize((tw_, th_), Image.LANCZOS)
    mask = mask.transform(mask.size, Image.AFFINE, (1, 0.18, -0.09 * th_, 0, 1, 0), Image.BICUBIC)
    wm = Image.new("L", (B, B), 0)
    wy = int(B * 0.74) - th_ // 2
    wm.paste(mask, ((B - tw_) // 2, wy))
    red_glow = wm.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(14))
    shadow = wm.filter(ImageFilter.MaxFilter(13)).filter(ImageFilter.GaussianBlur(10))
    body = Image.composite(Image.new("RGB", (B, B), 0), body, shadow.point(lambda v: v * 0.85))
    body = screen(body, Image.merge("RGB", (red_glow.point(lambda v: min(255, v * 2)),
                                           red_glow.point(lambda v: v // 6),
                                           red_glow.point(lambda v: v // 8))))
    # white core with a faint red tint at the edges
    edge = ImageChops.subtract(wm, wm.filter(ImageFilter.MinFilter(5)))
    core = Image.composite(Image.new("RGB", (B, B), (255, 120, 110)), Image.new("RGB", (B, B), (255, 248, 240)), edge)
    body = Image.composite(core, body, wm)

# --- rounded-square body, rim, drop shadow -----------------------------------
mask = Image.new("L", (B, B), 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, B - 1, B - 1), R, fill=255)
rim = Image.new("RGBA", (B, B), (0, 0, 0, 0))
ImageDraw.Draw(rim).rounded_rectangle((1, 1, B - 2, B - 2), R, outline=(120, 200, 110, 70), width=3)
canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
sh = Image.new("L", (S, S), 0)
ImageDraw.Draw(sh).rounded_rectangle((M, M + 12, M + B, M + B + 12), R, fill=120)
canvas.putalpha(sh.filter(ImageFilter.GaussianBlur(18)))
bodyA = body.convert("RGBA")
bodyA.putalpha(mask)
canvas.alpha_composite(Image.alpha_composite(bodyA, rim), (M, M))
canvas.save(OUT)
