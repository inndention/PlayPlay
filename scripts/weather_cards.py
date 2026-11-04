#!/usr/bin/env python3
"""weather_cards.py - animated weather scenes for PLAY PLAY spots (Pillow only, no browser).

spot_card.py hands a scene that carries an `anim` object to render_scene() here. Two planes, like the rest of the spot pipeline:

  bed        a LOOPING frame sequence (<bedBase>_000.jpg ... _059.jpg, 60 frames at 15 fps = 4 s) holding the sky, the card panels and
             everything that moves: sun rays, drifting clouds, rain, snow, twinkling stars, the moon, a burn gauge needle, lake waves,
             and each daily card's animated icon. ffmpeg loops it for as long as the narration runs, so every motion is periodic
             (rotation by a 12-fold symmetry, parallax clouds that shift by exactly one tile period, whole laps of rain) and never jumps.
  furniture  one transparent PNG with the kicker, title, strap (from spot_card) and every piece of text, so type stays crisp.

anim.type: now | cards | sunset | moon | burn | lakes   (fields: see playout/supervisor/weatherspots.h)

    python -I weather_cards.py --demo outdir       # render every scene type once (frames 0 and 20 as PNG) for a look
"""
from __future__ import annotations

import json
import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))      # python -I drops the script directory from sys.path
import spot_card as sc_mod  # noqa: E402

W, H = 1920, 1080
FRAMES, FPS = 60, 15
SAFE_X = int(W * 0.058)
TOP = 372                                                   # the content zone starts under the kicker / title block
WHITE = (255, 255, 255, 255)
SOFT = (206, 216, 228, 255)
WARM = (255, 196, 112, 255)
COOL = (150, 204, 255, 255)


# ---------------------------------------------------------------------------------------------
# Colour + background helpers
# ---------------------------------------------------------------------------------------------
def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(stops, w=W, h=H) -> Image.Image:
    """Vertical multi-stop gradient: stops = [(0..1, (r, g, b)), ...]."""
    col = Image.new("RGB", (1, h))
    px = col.load()
    for y in range(h):
        t = y / (h - 1)
        c = stops[-1][1]
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if t <= p1:
                c = mix(c0, c1, 0.0 if p1 == p0 else max(0.0, min(1.0, (t - p0) / (p1 - p0))))
                break
        px[0, y] = c
    return col.resize((w, h), Image.BILINEAR)


def sky_stops(cond: str, is_day: bool):
    if not is_day:
        return [(0, (6, 12, 34)), (1, (30, 46, 88))] if cond in ("clear", "mostly_clear", "partly") else [(0, (16, 22, 40)), (1, (50, 60, 82))]
    table = {
        "clear": [(0, (30, 104, 198)), (1, (146, 204, 242))], "mostly_clear": [(0, (40, 112, 200)), (1, (150, 204, 242))],
        "partly": [(0, (62, 122, 194)), (1, (156, 198, 232))], "cloudy": [(0, (92, 106, 124)), (1, (162, 172, 184))],
        "fog": [(0, (140, 150, 160)), (1, (200, 206, 212))], "drizzle": [(0, (70, 84, 104)), (1, (132, 146, 160))],
        "rain": [(0, (56, 68, 88)), (1, (110, 124, 140))], "heavy_rain": [(0, (40, 50, 70)), (1, (92, 104, 122))],
        "storm": [(0, (30, 34, 56)), (1, (78, 84, 106))], "snow": [(0, (118, 134, 156)), (1, (208, 218, 230))],
    }
    return table.get(cond, table["cloudy"])


def overlay(img: Image.Image, layer: Image.Image) -> None:
    img.paste(layer, (0, 0), layer)


def panel(img: Image.Image, box, fill=(6, 14, 30, 118), outline=None, radius=26, width=2) -> None:
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)
    overlay(img, layer)


# ---------------------------------------------------------------------------------------------
# Sprites and icons (drawn at 2x and reduced, so edges are smooth)
# ---------------------------------------------------------------------------------------------
def _cloud(d, cx, cy, sz, fill=(244, 247, 252, 255), shade=(200, 210, 226, 255)):
    r = sz * 0.22
    d.rounded_rectangle([cx - sz * 0.42, cy + sz * 0.03, cx + sz * 0.42, cy + sz * 0.24], radius=sz * 0.11, fill=shade)
    for x, y, rr in ((cx - sz * 0.22, cy + sz * 0.05, r), (cx + sz * 0.02, cy - sz * 0.08, r * 1.32), (cx + sz * 0.25, cy + sz * 0.06, r * 0.95)):
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=fill)
    d.rounded_rectangle([cx - sz * 0.40, cy + sz * 0.01, cx + sz * 0.40, cy + sz * 0.20], radius=sz * 0.10, fill=fill)


def _sun(d, cx, cy, r, t, glow=True):
    if glow:
        for i in range(7, 0, -1):
            rr = r * (1.0 + i * 0.16)
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(255, 214, 90, 14 + (7 - i) * 3))
    rot = t * (2 * math.pi / 12)                              # 12 rays: one ray-gap per loop is seamless
    for i in range(12):
        a = rot + i * 2 * math.pi / 12
        d.line([cx + math.cos(a) * r * 1.3, cy + math.sin(a) * r * 1.3, cx + math.cos(a) * r * 1.85, cy + math.sin(a) * r * 1.85], fill=(255, 210, 70, 255), width=max(3, int(r * 0.16)))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 205, 60, 255))
    d.ellipse([cx - r * 0.82, cy - r * 0.82, cx + r * 0.82, cy + r * 0.82], fill=(255, 226, 110, 255))


def _moon_crescent(img, cx, cy, r):
    m = Image.new("L", img.size, 0)
    md = ImageDraw.Draw(m)
    md.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    md.ellipse([cx - r * 0.55, cy - r * 1.0, cx + r * 1.45, cy + r * 1.0], fill=0)
    img.paste(Image.new("RGBA", img.size, (250, 240, 206, 255)), (0, 0), m)


def icon(kind: str, size: int, t: float, is_day: bool = True) -> Image.Image:
    """One animated weather icon at time t (0..1) as an RGBA square."""
    S = size * 2
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = S / 2, S / 2
    bob = math.sin(2 * math.pi * t) * S * 0.012
    if kind in ("clear", "mostly_clear") and not is_day:
        _moon_crescent(img, cx, cy, S * 0.30)
        for i, (x, y, rr) in enumerate(((0.74, 0.28, 0.022), (0.84, 0.50, 0.016), (0.66, 0.16, 0.014))):
            a = int(120 + 135 * (0.5 + 0.5 * math.sin(2 * math.pi * (t * (1 + i % 3) + i * 0.3))))
            d.ellipse([S * x - S * rr, S * y - S * rr, S * x + S * rr, S * y + S * rr], fill=(255, 255, 255, a))
    elif kind == "clear":
        _sun(d, cx, cy, S * 0.19, t)
    elif kind == "mostly_clear":
        _sun(d, cx - S * 0.04, cy - S * 0.04, S * 0.17, t)
        _cloud(d, cx + S * 0.22, cy + S * 0.24 + bob, S * 0.42)
    elif kind == "partly":
        _sun(d, cx - S * 0.12, cy - S * 0.12, S * 0.15, t)
        _cloud(d, cx + S * 0.06, cy + S * 0.14 + bob, S * 0.66)
    elif kind == "cloudy":
        _cloud(d, cx - S * 0.10, cy - S * 0.04 + bob, S * 0.52, fill=(214, 222, 234, 255), shade=(170, 182, 200, 255))
        _cloud(d, cx + S * 0.06, cy + S * 0.10 - bob, S * 0.70)
    elif kind == "fog":
        _cloud(d, cx, cy - S * 0.16 + bob, S * 0.58, fill=(226, 232, 240, 255), shade=(186, 196, 210, 255))
        for i in range(3):
            x = math.sin(2 * math.pi * (t + i / 3.0)) * S * 0.05
            d.rounded_rectangle([cx - S * 0.30 + x, cy + S * 0.10 + i * S * 0.09, cx + S * 0.30 + x, cy + S * 0.14 + i * S * 0.09], radius=S * 0.02, fill=(236, 242, 248, 230))
    elif kind in ("drizzle", "rain", "heavy_rain", "storm", "snow"):
        dark = kind in ("storm", "heavy_rain")
        _cloud(d, cx, cy - S * 0.12 + bob, S * 0.68, fill=(132, 142, 164, 255) if kind == "storm" else (176, 188, 208, 255) if dark else (206, 214, 228, 255),
               shade=(98, 108, 130, 255) if kind == "storm" else (140, 152, 174, 255))
        if kind == "snow":
            for i in range(5):
                ph = (t + i * 0.2) % 1.0
                x = cx - S * 0.26 + i * S * 0.13 + math.sin(2 * math.pi * (t * 2 + i * 0.37)) * S * 0.02
                y = cy + S * 0.10 + ph * S * 0.34
                rr = S * 0.025
                d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=(255, 255, 255, int(255 * (1.0 - 0.5 * ph))))
        else:
            n = {"drizzle": 3, "rain": 5, "heavy_rain": 7, "storm": 5}[kind]
            for i in range(n):
                ph = (t * 2 + i / n) % 1.0                    # two laps per loop
                x = cx - S * 0.28 + i * (S * 0.56 / max(1, n - 1)) - ph * S * 0.03
                y = cy + S * 0.08 + ph * S * 0.34
                d.line([x, y, x - S * 0.03, y + S * 0.09], fill=(110, 170, 255, int(255 * (1.0 - 0.6 * ph))), width=max(3, int(S * 0.018)))
            if kind == "storm":
                f = t % 1.0
                if 0.30 < f < 0.42 or 0.50 < f < 0.56:        # a two-flash flicker once per loop
                    bolt = [(cx + S * 0.02, cy + S * 0.02), (cx - S * 0.10, cy + S * 0.24), (cx - S * 0.01, cy + S * 0.24), (cx - S * 0.07, cy + S * 0.44), (cx + S * 0.12, cy + S * 0.18), (cx + S * 0.03, cy + S * 0.18)]
                    d.polygon(bolt, fill=(255, 226, 84, 255))
    else:
        _cloud(d, cx, cy + bob, S * 0.7)
    return img.resize((size, size), Image.LANCZOS)


def cloud_sprite(w: int, h: int, color, seed: int) -> Image.Image:
    rnd = random.Random(seed)
    S = 2
    img = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    base_y = h * S * 0.62
    d.rounded_rectangle([w * S * 0.04, base_y - h * S * 0.14, w * S * 0.96, base_y + h * S * 0.20], radius=h * S * 0.2, fill=color)
    for _ in range(5):
        r = h * S * rnd.uniform(0.22, 0.38)
        x = w * S * rnd.uniform(0.18, 0.82)
        d.ellipse([x - r, base_y - r * 1.15, x + r, base_y + r * 0.55], fill=color)
    return img.filter(ImageFilter.GaussianBlur(2 * S)).resize((w, h), Image.LANCZOS)


# ---------------------------------------------------------------------------------------------
# Sky layers: everything periodic in t
# ---------------------------------------------------------------------------------------------
class Sky:
    def __init__(self, cond: str, is_day: bool, seed: int = 7, region=(0, 0, W, H)):
        self.cond, self.is_day, self.region = cond, is_day, region
        rnd = random.Random(seed)
        gray = cond in ("cloudy", "rain", "drizzle", "heavy_rain", "storm", "fog")
        n_lanes = {"clear": 0, "mostly_clear": 1, "partly": 2, "cloudy": 3, "fog": 0, "drizzle": 3, "rain": 3, "heavy_rain": 3, "storm": 3, "snow": 2}.get(cond, 2)
        tone = (255, 255, 255, 120) if not gray else (90, 98, 116, 150) if cond in ("storm", "heavy_rain") else (206, 214, 226, 140)
        if not is_day:
            tone = (70, 82, 112, 120)
        self.lanes = []
        for i in range(n_lanes):
            period = (520, 760, 1040)[i % 3]
            sprite = cloud_sprite(int(period * 0.82), int(period * 0.30), tone, seed + i)
            self.lanes.append((period, sprite, int(self.region[1] + 40 + i * 120 + rnd.randint(0, 40))))
        count = {"drizzle": 70, "rain": 110, "heavy_rain": 190, "storm": 170}.get(cond, 0)
        self.rain = [(rnd.randint(0, W), rnd.randint(0, H), rnd.randint(34, 70), rnd.choice((2, 3))) for _ in range(count)]
        self.snow = [(rnd.randint(0, W), rnd.randint(0, H), rnd.uniform(2.5, 6.0), rnd.random()) for _ in range(140 if cond == "snow" else 0)]
        self.stars = [(rnd.randint(0, W), rnd.randint(0, int(H * 0.72)), rnd.uniform(1.2, 3.2), rnd.random(), rnd.choice((1, 2, 3))) for _ in range(90 if not is_day and cond in ("clear", "mostly_clear", "partly") else 0)]
        self.fog = [(rnd.randint(int(H * 0.25), int(H * 0.8)), rnd.randint(160, 300)) for _ in range(5 if cond == "fog" else 0)]

    def draw(self, img: Image.Image, t: float) -> None:
        ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
        d = ImageDraw.Draw(ov)
        if self.is_day and self.cond in ("clear", "mostly_clear", "partly"):                  # a pulsing sun glow high in the corner
            pulse = 1.0 + 0.05 * math.sin(2 * math.pi * t)
            for i in range(9, 0, -1):
                rr = 90 * i * pulse
                d.ellipse([1600 - rr, 150 - rr, 1600 + rr, 150 + rr], fill=(255, 236, 150, 7 + (9 - i)))
        for x, y, r, tw, m in self.stars:
            a = int(80 + 175 * (0.5 + 0.5 * math.sin(2 * math.pi * (t * m + tw))))
            d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, a))
        for cy, thick in self.fog:
            for k in range(-1, 3):
                x = (k * 760 + t * 760) % (W + 760) - 760
                d.rounded_rectangle([x, cy, x + 720, cy + thick * 0.35], radius=60, fill=(235, 240, 245, 56))
        overlay(img, ov)
        for period, sprite, y in self.lanes:                                                     # parallax cloud lanes: shift by one tile period per loop
            dx = t * period
            x = -sprite.width + dx
            while x < W:
                img.paste(sprite, (int(x), y), sprite)
                x += period
        if self.rain or self.snow:
            ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
            d = ImageDraw.Draw(ov)
            for x, y0, ln, laps in self.rain:
                y = (y0 + t * laps * (H + ln)) % (H + ln) - ln
                d.line([x, y, x - ln * 0.22, y + ln], fill=(205, 224, 255, 150), width=2)
            for x0, y0, r, ph in self.snow:
                y = (y0 + t * (H + 20)) % (H + 20) - 10
                x = x0 + 20 * math.sin(2 * math.pi * (t * 2 + ph))
                d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, 215))
            if self.cond == "storm" and (0.30 < t < 0.34 or 0.50 < t < 0.53):                       # a lightning flash in the clouds
                d.rectangle([0, 0, W, H], fill=(255, 255, 255, 52))
            overlay(img, ov)


# ---------------------------------------------------------------------------------------------
# Text helpers (the furniture plane)
# ---------------------------------------------------------------------------------------------
def fonts():
    bold = sc_mod.FONT_CANDIDATES_BOLD
    reg = sc_mod.FONT_CANDIDATES_REG
    return bold, reg


def ctext(img, cx, y, s, font, fill, shadow=True):
    d = ImageDraw.Draw(img)
    w = sc_mod.text_w(d, s, font)
    if shadow:
        sc_mod.shadow_text(img, (int(cx - w / 2), y), s, font, fill, blur=5, off=(0, 2), alpha=150)
    else:
        d.text((int(cx - w / 2), y), s, font=font, fill=fill)


def ltext(img, x, y, s, font, fill, shadow=True):
    if shadow:
        sc_mod.shadow_text(img, (x, y), s, font, fill, blur=5, off=(0, 2), alpha=150)
    else:
        ImageDraw.Draw(img).text((x, y), s, font=font, fill=fill)


def drop_shape(d, cx, cy, r, fill):
    d.polygon([(cx, cy - r * 1.5), (cx - r, cy + r * 0.2), (cx + r, cy + r * 0.2)], fill=fill)
    d.ellipse([cx - r, cy - r * 0.8, cx + r, cy + r * 1.2], fill=fill)


# ---------------------------------------------------------------------------------------------
# Scene builders: each returns (static_bed RGB, step(frame, t), paint(furniture RGBA))
# ---------------------------------------------------------------------------------------------
def _blank():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))


def build_now(spec, sc, a):
    cond, is_day = a.get("cond", "clear"), bool(a.get("isDay", True))
    bed = gradient(sky_stops(cond, is_day))
    over = _blank()
    panel(over, (SAFE_X - 30, TOP - 18, 1040, TOP + 566), fill=(4, 10, 24, 124))
    sky = Sky(cond, is_day, seed=11)

    def step(frame, t):
        sky.draw(frame, t)
        overlay(frame, over)
        ic = icon(cond, 500, t, is_day)
        frame.paste(ic, (1290, TOP - 10), ic)

    def paint(img):
        bold, reg = fonts()
        f_t, f_l, f_s, f_c = sc_mod.load_font(bold, 210), sc_mod.load_font(bold, 64), sc_mod.load_font(reg, 38), sc_mod.load_font(bold, 50)
        ltext(img, SAFE_X, TOP - 14, "%d°" % a.get("temp", 0), f_t, WHITE)
        ltext(img, SAFE_X + 8, TOP + 206, str(a.get("label", "")), f_l, WHITE)
        x = SAFE_X + 8
        for c in ("Feels like %d°" % a.get("feels", 0), "Humidity %d%%" % a.get("humidity", 0), "Wind %d mph" % a.get("wind", 0)):
            ltext(img, x, TOP + 296, c, f_s, SOFT, shadow=False)
            x += sc_mod.text_w(ImageDraw.Draw(img), c, f_s) + 40
        ltext(img, SAFE_X + 8, TOP + 362, "HIGH %d°" % a.get("hi", 0), f_c, WARM)
        ltext(img, SAFE_X + 330, TOP + 362, "LOW %d°" % a.get("lo", 0), f_c, COOL)
        drop_shape(ImageDraw.Draw(img), SAFE_X + 28, TOP + 478, 13, (120, 180, 255, 255))
        ltext(img, SAFE_X + 62, TOP + 450, "Chance of rain %d%%" % a.get("pop", 0), f_s, WHITE, shadow=False)
        if a.get("evening"):
            ltext(img, SAFE_X + 8, TOP + 502, "Tomorrow's high %d°" % a.get("tomorrowHi", 0), f_s, SOFT, shadow=False)
    return bed, step, paint


def build_cards(spec, sc, a):
    days = a.get("days", [])[:8]
    n = max(1, len(days))
    cond, is_day = a.get("cond", "clear"), bool(a.get("isDay", True))
    bed = gradient(sky_stops(cond, is_day))
    gap = 16
    cw = int((W - 2 * SAFE_X - gap * (n - 1)) / n)
    ch, y0 = 560, TOP - 6
    over = _blank()
    for i in range(n):
        x = SAFE_X + i * (cw + gap)
        panel(over, (x, y0, x + cw, y0 + ch), fill=(6, 14, 30, 130), outline=(255, 214, 102, 230) if i == 0 else (255, 255, 255, 70), width=4 if i == 0 else 2)
    sky = Sky(cond, is_day, seed=13)
    isz = min(190, cw - 30)

    def step(frame, t):
        sky.draw(frame, t)
        overlay(frame, over)
        for i, dday in enumerate(days):
            ic = icon(dday.get("cond", "cloudy"), isz, t, True)
            frame.paste(ic, (SAFE_X + i * (cw + gap) + (cw - isz) // 2, y0 + 104), ic)

    def paint(img):
        bold, reg = fonts()
        f_d, f_s, f_h, f_l, f_x = sc_mod.load_font(bold, 46), sc_mod.load_font(reg, 28), sc_mod.load_font(bold, 74), sc_mod.load_font(reg, 46), sc_mod.load_font(reg, 27)
        d = ImageDraw.Draw(img)
        for i, dday in enumerate(days):
            cx = SAFE_X + i * (cw + gap) + cw // 2
            ctext(img, cx, y0 + 18, "TODAY" if i == 0 else str(dday.get("dow", "")), f_d, WHITE)
            ctext(img, cx, y0 + 72, str(dday.get("date", "")), f_s, SOFT, shadow=False)
            ctext(img, cx, y0 + 300, str(dday.get("label", "")), f_x, SOFT, shadow=False)
            ctext(img, cx, y0 + 336, "%d°" % dday.get("hi", 0), f_h, WARM)
            ctext(img, cx, y0 + 420, "%d°" % dday.get("lo", 0), f_l, COOL)
            pop = int(dday.get("pop", 0))
            drop_shape(d, cx - 44, y0 + 514, 9, (120, 180, 255, 255 if pop >= 30 else 130))
            ctext(img, cx + 14, y0 + 496, "%d%%" % pop, f_x, WHITE if pop >= 30 else SOFT, shadow=False)
    return bed, step, paint


def _hills(rgba, base_y, color, seed, amp=90):
    rnd = random.Random(seed)
    pts = [(0, H)]
    x, y = 0, base_y
    while x <= W + 100:
        pts.append((x, y))
        x += rnd.randint(80, 200)
        y = base_y + rnd.randint(-amp, amp // 2)
    pts += [(W, H)]
    ImageDraw.Draw(rgba).polygon(pts, fill=color)


def build_sunset(spec, sc, a):
    passed = bool(a.get("passed"))
    bed = gradient([(0, (30, 26, 84)), (0.42, (150, 62, 96)), (0.72, (240, 120, 66)), (1, (255, 190, 96))] if not passed else [(0, (14, 14, 52)), (0.5, (72, 40, 90)), (1, (170, 80, 80))])
    over = _blank()
    panel(over, (SAFE_X - 30, TOP - 18, 1110, TOP + 560), fill=(8, 6, 26, 118))
    hills = _blank()
    _hills(hills, 880, (22, 14, 36, 255), 3, 70)
    _hills(hills, 940, (12, 8, 24, 255), 9, 40)
    sky = Sky("partly", True, seed=17)
    for i, (period, sprite, y) in enumerate(sky.lanes):                      # tint the clouds sunset-orange
        tinted = Image.new("RGBA", sprite.size, (255, 150, 90, 0))
        tinted.putalpha(sprite.getchannel("A").point(lambda v: int(v * 0.8)))
        sky.lanes[i] = (period, tinted, y + 200)

    def step(frame, t):
        ov = _blank()
        d = ImageDraw.Draw(ov)
        sx, sy = 1470, 820 if not passed else 930
        pulse = 1.0 + 0.04 * math.sin(2 * math.pi * t)
        for i in range(12, 0, -1):
            rr = (150 + i * 34) * pulse
            d.ellipse([sx - rr, sy - rr, sx + rr, sy + rr], fill=(255, 178, 80, 9 + (12 - i) * 2))
        d.ellipse([sx - 150, sy - 150, sx + 150, sy + 150], fill=(255, 226, 150, 255) if not passed else (255, 170, 110, 200))
        overlay(frame, ov)
        for period, sprite, y in sky.lanes:
            x = -sprite.width + t * period
            while x < W:
                frame.paste(sprite, (int(x), y), sprite)
                x += period
        overlay(frame, hills)                                                  # the hills cover the sun's lower edge
        overlay(frame, over)

    def paint(img):
        bold, reg = fonts()
        f_big, f_l, f_v = sc_mod.load_font(bold, 200), sc_mod.load_font(reg, 40), sc_mod.load_font(bold, 44)
        ltext(img, SAFE_X - 4, TOP - 14, str(a.get("sunset", "")), f_big, WHITE)
        rows = [("Golden hour begins", a.get("golden", "")), ("Sunrise", a.get("sunrise", "")), ("Daylight today", a.get("daylight", ""))]
        tm = a.get("tomorrow", "")
        if tm:
            dl = int(a.get("delta", 0))
            rows.append(("Tomorrow's sunset", "%s  (%s)" % (tm, "same time" if dl == 0 else "%d min %s" % (abs(dl), "earlier" if dl < 0 else "later"))))
        y = TOP + 238
        for k, v in rows:
            if not v or (passed and k.startswith("Golden")):
                continue
            ltext(img, SAFE_X + 4, y, k, f_l, SOFT, shadow=False)
            ltext(img, SAFE_X + 440, y - 4, str(v), f_v, WHITE)
            y += 70
    return bed, step, paint


def moon_sprite(r: int, fraction: float) -> Image.Image:
    """The moon disc with its phase: fraction 0 = new, 0.5 = full, northern-hemisphere orientation (waxing is lit on the right)."""
    S = 2
    R = r * S
    lit = Image.new("L", (2 * R, 2 * R), 0)
    px = lit.load()
    waxing = fraction < 0.5
    phi = 2 * math.pi * (fraction if waxing else 1 - fraction)
    cphi = math.cos(phi)
    for y in range(2 * R):
        ny = (y - R) / R
        if abs(ny) >= 1:
            continue
        half = math.sqrt(1 - ny * ny)
        xt = cphi * half
        x0 = int(R + (xt if waxing else -half) * R)
        x1 = int(R + (half if waxing else -xt) * R)
        for x in range(max(0, min(x0, x1)), min(2 * R, max(x0, x1))):
            px[x, y] = 255
    disc = Image.new("RGBA", (2 * R, 2 * R), (0, 0, 0, 0))
    base = Image.new("RGBA", (2 * R, 2 * R), (232, 226, 206, 255))
    bd = ImageDraw.Draw(base)
    rnd = random.Random(5)
    for _ in range(14):                                                       # maria: soft darker patches
        cx, cy, rr = rnd.randint(int(R * 0.45), int(R * 1.55)), rnd.randint(int(R * 0.4), int(R * 1.6)), rnd.randint(int(R * 0.08), int(R * 0.22))
        bd.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(188, 182, 166, 255))
    base = base.filter(ImageFilter.GaussianBlur(R * 0.03))
    circle = Image.new("L", (2 * R, 2 * R), 0)
    ImageDraw.Draw(circle).ellipse([0, 0, 2 * R - 1, 2 * R - 1], fill=255)
    dark = Image.new("RGBA", (2 * R, 2 * R), (26, 32, 52, 255))               # the unlit part of the disc stays faintly visible (earthshine)
    disc.paste(dark, (0, 0), circle)
    disc.paste(base, (0, 0), Image.composite(lit, Image.new("L", lit.size, 0), circle))
    return disc.resize((2 * r, 2 * r), Image.LANCZOS)


def build_moon(spec, sc, a):
    bed = gradient([(0, (4, 8, 26)), (1, (26, 40, 86))])
    sky = Sky("clear", False, seed=19)
    over = _blank()
    panel(over, (SAFE_X - 30, TOP - 18, 1000, TOP + 566), fill=(4, 8, 22, 118))
    frac = float(a.get("fraction", 0.5))
    ms = moon_sprite(250, frac)
    cx, cy = 1400, 650

    def step(frame, t):
        sky.draw(frame, t)
        ov = _blank()
        d = ImageDraw.Draw(ov)
        pulse = 1.0 + 0.03 * math.sin(2 * math.pi * t)
        glow = max(0.12, float(a.get("illum", 50)) / 100.0)
        for i in range(10, 0, -1):
            rr = (250 + i * 26) * pulse
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(210, 220, 255, int(2 + (10 - i) * 2.2 * glow)))
        overlay(frame, ov)
        frame.paste(ms, (cx - 250, cy - 250), ms)
        overlay(frame, over)

    def paint(img):
        bold, reg = fonts()
        f_p, f_i, f_l, f_v = sc_mod.load_font(bold, 104), sc_mod.load_font(bold, 60), sc_mod.load_font(reg, 40), sc_mod.load_font(bold, 44)
        ltext(img, SAFE_X - 2, TOP - 8, str(a.get("phase", "")), f_p, WHITE)
        ltext(img, SAFE_X + 4, TOP + 126, "%d%% illuminated" % int(a.get("illum", 0)), f_i, SOFT)
        y = TOP + 250
        for k, v in (("Next full moon", a.get("nextFull", "")), ("Next new moon", a.get("nextNew", ""))):
            ltext(img, SAFE_X + 4, y, k, f_l, SOFT, shadow=False)
            ltext(img, SAFE_X + 400, y - 4, str(v), f_v, WHITE)
            y += 74
        # the lunar cycle as a bar: new -> full -> new, a marker where tonight is
        d = ImageDraw.Draw(img)
        x0, x1, by = SAFE_X + 6, SAFE_X + 880, TOP + 470
        d.rounded_rectangle([x0, by, x1, by + 14], radius=7, fill=(255, 255, 255, 60))
        f = sc_mod.load_font(reg, 26)
        for tick, lab in ((0.0, "New"), (0.25, "First qtr"), (0.5, "Full"), (0.75, "Last qtr"), (1.0, "New")):
            x = int(x0 + tick * (x1 - x0))
            d.ellipse([x - 6, by + 1, x + 6, by + 13], fill=(255, 255, 255, 150))
            ctext(img, x, by + 30, lab, f, SOFT, shadow=False)
        mx = int(x0 + frac * (x1 - x0))
        d.ellipse([mx - 15, by - 8, mx + 15, by + 22], fill=(255, 214, 102, 255), outline=(255, 255, 255, 255), width=3)
    return bed, step, paint


RATING_COLOR = {"Low": (84, 196, 110), "Moderate": (230, 204, 70), "High": (240, 150, 52), "Very High": (232, 84, 52), "Extreme": (210, 40, 56)}


def build_burn(spec, sc, a):
    level = float(a.get("level", 0.3))
    top = mix((34, 74, 86), (96, 24, 24), min(1.0, level * 1.2))
    bed = gradient([(0, top), (0.7, mix((120, 92, 74), (190, 82, 40), level)), (1, mix((60, 50, 44), (90, 40, 24), level))])
    _hills(bed.convert("RGBA"), 900, (24, 18, 18, 255), 4, 70)
    hills = _blank()
    _hills(hills, 905, (24, 18, 18, 255), 4, 70)
    over = _blank()
    panel(over, (SAFE_X - 30, TOP - 18, 1040, TOP + 566), fill=(8, 8, 18, 124))
    gcx, gcy, gr = 1440, 790, 300
    arcs = [(60, 180, 90), (150, 200, 60), (240, 200, 50), (240, 130, 40), (220, 50, 40)]
    d = ImageDraw.Draw(over)
    for i, col in enumerate(arcs):
        d.arc([gcx - gr, gcy - gr, gcx + gr, gcy + gr], 180 + i * 36 + 1.5, 180 + (i + 1) * 36 - 1.5, fill=col + (255,), width=62)
    rnd = random.Random(23)
    embers = [(rnd.randint(1050, 1850), rnd.randint(0, H), rnd.uniform(3, 7), rnd.random()) for _ in range(int(10 + level * 70))]

    def step(frame, t):
        overlay(frame, hills)
        overlay(frame, over)
        ov = _blank()
        d = ImageDraw.Draw(ov)
        for x, y0, r, ph in embers:                                          # embers rise one lap per loop
            y = H - ((H - y0 + t * (H + 20)) % (H + 20)) + 10
            x2 = x + 16 * math.sin(2 * math.pi * (t * 2 + ph))
            d.ellipse([x2 - r, y - r, x2 + r, y + r], fill=(255, 150 + int(70 * ph), 60, int(40 + 150 * (1 - y / H))))
        ang = math.radians(180 + level * 180 + 2.5 * math.sin(2 * math.pi * t))
        nx, ny = gcx + math.cos(ang) * (gr - 10), gcy + math.sin(ang) * (gr - 10)
        px, py = math.cos(ang + math.pi / 2) * 14, math.sin(ang + math.pi / 2) * 14
        d.polygon([(nx, ny), (gcx + px, gcy + py), (gcx - px, gcy - py)], fill=(255, 255, 255, 255))
        d.ellipse([gcx - 26, gcy - 26, gcx + 26, gcy + 26], fill=(255, 255, 255, 255))
        d.ellipse([gcx - 12, gcy - 12, gcx + 12, gcy + 12], fill=(40, 40, 48, 255))
        overlay(frame, ov)

    def paint(img):
        bold, reg = fonts()
        f_r, f_b, f_a, f_g, f_law = sc_mod.load_font(bold, 140), sc_mod.load_font(reg, 40), sc_mod.load_font(reg, 38), sc_mod.load_font(bold, 26), sc_mod.load_font(reg, 30)
        rating = str(a.get("rating", ""))
        col = RATING_COLOR.get(rating, (240, 240, 240)) + (255,)
        ltext(img, SAFE_X - 4, TOP - 12, rating.upper(), f_r, col)
        y = TOP + 160
        for r in (a.get("reasons") or [])[:3]:
            ltext(img, SAFE_X + 4, y, "•  " + str(r)[:1].upper() + str(r)[1:], f_b, WHITE, shadow=False)
            y += 56
        d = ImageDraw.Draw(img)
        for ln in sc_mod.wrap(d, str(a.get("advice", "")), f_a, 900, 3):
            ltext(img, SAFE_X + 4, y + 14, ln, f_a, SOFT, shadow=False)
            y += 50
        law = str(a.get("law", ""))
        if law:
            panel(img, (SAFE_X - 10, 836, 1010, 936), fill=(255, 214, 102, 36), outline=(255, 214, 102, 170))
            for i, ln in enumerate(sc_mod.wrap(d, law, f_law, 960, 3)):
                ltext(img, SAFE_X + 6, 846 + i * 32, ln, f_law, WHITE, shadow=False)
        for i, lab in enumerate(("LOW", "MOD", "HIGH", "V.HIGH", "EXTREME")):         # gauge labels
            ang = math.radians(180 + i * 36 + 18)
            ctext(img, int(gcx + math.cos(ang) * (gr + 62)), int(gcy + math.sin(ang) * (gr + 62)) - 14, lab, f_g, SOFT, shadow=False)
    return bed, step, paint


def build_lakes(spec, sc, a):
    is_day = bool(a.get("isDay", True))
    bed = gradient([(0, (28, 70, 120)), (0.5, (30, 100, 150)), (1, (14, 52, 96))] if is_day else [(0, (6, 14, 40)), (1, (16, 44, 86))])
    layers = [(0.62, 26, 2, (140, 205, 245, 70)), (0.72, 34, 3, (100, 176, 232, 90)), (0.82, 44, 2, (60, 140, 208, 120)), (0.92, 52, 3, (34, 104, 176, 160))]

    def step(frame, t):
        ov = _blank()
        d = ImageDraw.Draw(ov)
        for yf, amp, cyc, col in layers:
            base = int(H * yf)
            pts = [(0, H)]
            for x in range(0, W + 40, 40):
                pts.append((x, base + amp * math.sin(2 * math.pi * (x / 620.0 + t * cyc))))
            pts.append((W, H))
            d.polygon(pts, fill=col)
        overlay(frame, ov)

    def paint(img):
        bold, reg = fonts()
        end = sc_mod._draw_rows(img, sc.get("rows") or [], SAFE_X, TOP - 6, W - 2 * SAFE_X, 1.0, bold, reg)
        ltext(img, SAFE_X, end + 24, "Smith Mountain and Leesville: Appalachian Power.  Philpott: USGS.", sc_mod.load_font(reg, 28), SOFT, shadow=False)
    return bed, step, paint


BUILDERS = {"now": build_now, "cards": build_cards, "sunset": build_sunset, "moon": build_moon, "burn": build_burn, "lakes": build_lakes}


# ---------------------------------------------------------------------------------------------
# Entry point used by spot_card.render_scenes
# ---------------------------------------------------------------------------------------------
def render_scene(spec: dict, sc: dict, furniture_out: str, bed_base: str, frames: int = FRAMES, only=None) -> dict:
    a = sc.get("anim") or {}
    builder = BUILDERS.get(a.get("type"))
    if builder is None:
        raise ValueError("unknown weather scene type %r" % a.get("type"))
    static, step, paint = builder(spec, sc, a)
    Path(bed_base).parent.mkdir(parents=True, exist_ok=True)
    for k in (range(frames) if only is None else only):
        frame = static.copy()
        step(frame, k / float(frames))
        frame.save("%s_%03d.jpg" % (bed_base, k), "JPEG", quality=88)
    first = "%s_%03d.jpg" % (bed_base, 0 if only is None else only[0])
    Image.open(first).save(bed_base + ".jpg", "JPEG", quality=88)
    # furniture: the shared chrome (kicker, category chip, counter, title, strap) with the scene's text on top
    chrome = {"title": sc.get("title"), "category": sc.get("category"), "place": sc.get("place"), "counter": sc.get("counter")}
    sc_mod.render_story(spec, chrome, furniture_out, False, "gradient")
    img = Image.open(furniture_out).convert("RGBA")
    paint(img)
    img.save(furniture_out, "PNG", compress_level=3)
    return {"bed": bed_base + ".jpg", "bedSeq": bed_base + "_%03d.jpg", "frames": frames, "fps": FPS, "furniture": furniture_out}


# ---------------------------------------------------------------------------------------------
# Demo: python -I weather_cards.py --demo outdir
# ---------------------------------------------------------------------------------------------
def _demo(outdir: str) -> None:
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    days = [{"dow": d, "date": "Oct %d" % (23 + i), "cond": c, "label": l, "hi": hi, "lo": lo, "pop": p, "gust": 12}
            for i, (d, c, l, hi, lo, p) in enumerate((("FRI", "clear", "Sunny", 66, 37, 0), ("SAT", "cloudy", "Cloudy", 62, 40, 0), ("SUN", "partly", "Partly cloudy", 65, 41, 10), ("MON", "rain", "Rain", 71, 42, 70),
                                                      ("TUE", "storm", "Stormy", 71, 47, 80), ("WED", "snow", "Snow", 63, 46, 40), ("THU", "fog", "Foggy", 68, 43, 5)))]
    spec = {"width": W, "height": H, "kicker": "WEATHER", "brand": "STAR NEWS", "brandName": "GetStarNews", "series": "Weather", "site": "getstarnews.com", "date": "Friday, October 23", "accent": "#d80018", "ink": "#0b1220"}
    scenes = [
        {"title": "Martinsville Weather", "category": "RIGHT NOW", "place": "FRIDAY, OCTOBER 23", "counter": "1 / 6", "anim": {"type": "now", "cond": "partly", "isDay": True, "temp": 66, "feels": 60, "hi": 66, "lo": 37, "pop": 10, "humidity": 45, "wind": 8, "label": "Partly cloudy"}},
        {"title": "7-Day Forecast", "category": "FORECAST", "place": "FRIDAY, OCTOBER 23", "counter": "2 / 6", "anim": {"type": "cards", "cond": "partly", "isDay": True, "days": days}},
        {"title": "Sunset", "category": "ALMANAC", "place": "FRIDAY, OCTOBER 23", "counter": "3 / 6", "anim": {"type": "sunset", "sunset": "6:32 PM", "sunrise": "7:21 AM", "golden": "5:32 PM", "tomorrow": "6:31 PM", "delta": -1, "daylight": "11 h 11 m", "passed": False}},
        {"title": "Moon Phase", "category": "ALMANAC", "place": "FRIDAY, OCTOBER 23", "counter": "4 / 6", "anim": {"type": "moon", "phase": "Waxing Crescent", "illum": 23, "fraction": 0.18, "waxing": True, "nextFull": "Fri, Nov 6", "nextNew": "Thu, Nov 12"}},
        {"title": "Burn Safety", "category": "FIRE WEATHER TODAY", "place": "FRIDAY, OCTOBER 23", "counter": "5 / 6", "anim": {"type": "burn", "rating": "High", "level": 0.75, "when": "today", "reasons": ["gusts to 20 mph", "humidity as low as 31%", "no rain in the last 3 days"],
                                                                                                                       "advice": "Fire could spread quickly. Burning is not recommended today.", "law": ""}},
        {"title": "Lake Levels", "category": "ALMANAC", "place": "FRIDAY, OCTOBER 23", "counter": "6 / 6", "isDay": True, "anim": {"type": "lakes", "isDay": True},
         "rows": [{"label": "Smith Mountain Lake", "sub": "4.36 ft below full pond (795)", "value": "790.64 ft", "change": "-0.06 ft 24h", "tone": "down"},
                  {"label": "Leesville Lake", "sub": "9.25 ft below full pond (613)", "value": "603.75 ft", "change": "+0.02 ft 24h", "tone": "up"},
                  {"label": "Philpott Lake", "sub": "reading from USGS", "value": "970.71 ft", "change": "+0.10 ft 24h", "tone": "up"}]},
    ]
    for i, sc in enumerate(scenes):
        r = render_scene(spec, sc, str(out / ("f%d.png" % i)), str(out / ("b%d" % i)), frames=FRAMES, only=[0, 20])
        for k in (0, 20):
            bed = Image.open("%s_%03d.jpg" % (out / ("b%d" % i), k)).convert("RGBA")
            bed.alpha_composite(Image.open(r["furniture"]).convert("RGBA"))
            bed.convert("RGB").resize((1280, 720)).save(out / ("view%d_%d.png" % (i, k)))
    print("demo written to", out)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--demo":
        _demo(sys.argv[2])
    else:
        print(__doc__)
