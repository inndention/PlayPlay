#!/usr/bin/env python3
"""spot_card.py — the FURNITURE plane of a PLAY PLAY auto-generated spot (Pillow, no browser).

Two-plane rule (KB newsspots/02-CARD-SYSTEM §2.0): ffmpeg moves the photograph, something else
draws only the furniture on a transparent background, ffmpeg composites the two. This script is the
"something else": it renders a 1920x1080 RGBA PNG with the kicker chip, headline, bullet lines and
the station strap, and (when there is no photograph) a static gradient BED that ffmpeg drifts with
zoompan exactly like a photo.

    python -I spot_card.py --spec spec.json --out furniture.png [--bed bed.png]

spec.json:
  { "width": 1920, "height": 1080,
    "kicker": "WEATHER", "headline": "Cooler Air Moves In Tonight",
    "lines": ["Tonight: Low 54", "Friday: Sunny, 76", "Rain returns Sunday"],
    "brand": "STAR NEWS", "brandName": "GetStarNews", "series": "GetStarNews Weather",
    "footer": "Thursday, October 22 - 6:05 PM",
    "accent": "#d80018", "ink": "#0b1220",
    "photo": true,                 # a photograph sits under the furniture -> draw a readability scrim
    "fonts": {"bold": "C:/Windows/Fonts/segoeuib.ttf", "regular": "C:/Windows/Fonts/segoeui.ttf"} }

Stdout: one JSON object {ok, furniture, bed, headlineLines}. Exit 0 / 2.

Scenes mode (Headline News, one card per story):

    python -I spot_card.py --scenes scenes.json

scenes.json: the strap fields above (brand, brandName, series, accent, ink, kicker, site, siteName, date) plus
  "scenes": [ {"kind": "intro", "rundown": ["headline", ...], "photo": "", "furniture": "out.png", "bedBase": "x"},
              {"kind": "story", "headline"|"title": "...", "summary": "...", "place": "Roanoke", "category": "Community",
               "counter": "1 / 5", "photo": "story.jpg", "furniture": "...", "bedBase": "..."},
              {"kind": "outro", "photo": "", "furniture": "...", "bedBase": "..."} ]
For every scene it writes the furniture PNG and the still BED ffmpeg moves: <bedBase>.jpg from the photo
(landscape fills the frame; square / portrait sits inset on a blurred copy of itself; intro / outro get a
dimmed, blurred backdrop) or <bedBase>.png, the gradient bed, when there is no usable photo.
Stdout: {ok, scenes: [{kind, furniture, bed, photo, layout, photoSize}]}.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_CANDIDATES_BOLD = ["C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/calibrib.ttf", "DejaVuSans-Bold.ttf"]
FONT_CANDIDATES_REG = ["C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/calibri.ttf", "DejaVuSans.ttf"]


def log(m: str) -> None:
    sys.stderr.write(str(m) + "\n")


def hex_rgb(s: str, default=(216, 0, 24)) -> tuple[int, int, int]:
    s = (s or "").strip().lstrip("#")
    if len(s) == 3:
        s = "".join(c * 2 for c in s)
    try:
        return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]
    except Exception:  # noqa: BLE001
        return default


def load_font(paths: list[str], size: int) -> ImageFont.FreeTypeFont:
    for p in paths:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()  # type: ignore[return-value]


def clean(s: str) -> str:
    """Screen hygiene: smart punctuation -> plain, drop control chars, collapse spaces."""
    repl = {"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-", "\u2014": " - ",
            "\u2026": "...", "\u00b7": "-", "\u2022": "-", "\u00a0": " ", "\t": " "}
    for k, v in repl.items():
        s = s.replace(k, v)
    s = "".join(c for c in s if ord(c) >= 32)
    return " ".join(s.split())


def text_w(draw: ImageDraw.ImageDraw, s: str, font) -> int:
    b = draw.textbbox((0, 0), s, font=font)
    return b[2] - b[0]


def wrap(draw: ImageDraw.ImageDraw, s: str, font, max_w: int, max_lines: int) -> list[str]:
    words = clean(s).split()
    lines: list[str] = []
    cur = ""
    for w in words:
        trial = (cur + " " + w).strip()
        if not cur or text_w(draw, trial, font) <= max_w:
            cur = trial
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        last = lines[-1]
        while last and text_w(draw, last + "...", font) > max_w:
            last = last.rsplit(" ", 1)[0] if " " in last else last[:-1]
        lines[-1] = last + "..."
    return lines


def ellipsize(draw: ImageDraw.ImageDraw, s: str, font, max_w: int) -> str:
    s = clean(s)
    if text_w(draw, s, font) <= max_w:
        return s
    while s and text_w(draw, s + "...", font) > max_w:
        s = s.rsplit(" ", 1)[0] if " " in s else s[:-1]
    return s + "..."


def shadow_text(img: Image.Image, xy, s: str, font, fill, blur=6, off=(0, 3), alpha=170) -> None:
    """Soft drop shadow then the text, so type reads over any photograph."""
    sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).text((xy[0] + off[0], xy[1] + off[1]), s, font=font, fill=(0, 0, 0, alpha))
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    img.alpha_composite(sh)
    ImageDraw.Draw(img).text(xy, s, font=font, fill=fill)


# ----------------------------------------------------------------------------- bed (no photo)
def render_bed(path: str, w: int, h: int, accent, ink) -> None:
    """Static station bed: vertical ink gradient + accent glow top-right + faint scanlines.
    ffmpeg zoompans this exactly like a photograph, so a text-only spot still breathes."""
    import numpy as np
    y = np.linspace(0.0, 1.0, h, dtype="float32")[:, None]
    top = np.array([min(255, ink[0] + 22), min(255, ink[1] + 26), min(255, ink[2] + 34)], dtype="float32")
    bot = np.array([max(0, ink[0] - 6), max(0, ink[1] - 8), max(0, ink[2] - 10)], dtype="float32")
    base = top * (1 - y) + bot * y                                  # (h,3)
    img = np.repeat(base[:, None, :], w, axis=1)                    # (h,w,3)
    # accent glow (radial, top right)
    yy, xx = np.mgrid[0:h, 0:w].astype("float32")
    cx, cy, r = w * 0.86, h * 0.12, w * 0.55
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / r
    glow = np.clip(1.0 - d, 0.0, 1.0) ** 2.2 * 0.42
    img = img * (1 - glow[..., None]) + np.array(accent, dtype="float32") * glow[..., None]
    # a second cool glow bottom-left keeps the frame from going flat black
    cx2, cy2, r2 = w * 0.08, h * 0.95, w * 0.5
    d2 = np.sqrt((xx - cx2) ** 2 + (yy - cy2) ** 2) / r2
    glow2 = np.clip(1.0 - d2, 0.0, 1.0) ** 2.5 * 0.22
    img = img * (1 - glow2[..., None]) + np.array([40, 90, 140], dtype="float32") * glow2[..., None]
    # scanline texture
    lines = (np.arange(h) % 6 == 0).astype("float32")[:, None, None] * 10.0
    img = np.clip(img + lines, 0, 255).astype("uint8")
    Image.fromarray(img, "RGB").save(path, "PNG", compress_level=3)


# ----------------------------------------------------------------------------- furniture
def render_furniture(spec: dict, out: str) -> dict:
    W = int(spec.get("width") or 1920)
    H = int(spec.get("height") or 1080)
    accent = hex_rgb(spec.get("accent"), (216, 0, 24))
    ink = hex_rgb(spec.get("ink"), (11, 18, 32))
    photo = bool(spec.get("photo"))
    fonts = spec.get("fonts") or {}
    bold_paths = ([fonts["bold"]] if fonts.get("bold") else []) + FONT_CANDIDATES_BOLD
    reg_paths = ([fonts["regular"]] if fonts.get("regular") else []) + FONT_CANDIDATES_REG
    s = H / 1080.0                                   # scale everything from a 1080 design
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    safe_x, safe_y = int(W * 0.058), int(H * 0.06)    # title-safe-ish margins
    text_col_w = int(W * 0.56) if photo else int(W * 0.80)

    # readability scrim: left-to-right when a photo is underneath, a soft top/bottom vignette otherwise
    if photo:
        scrim = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        sd = ImageDraw.Draw(scrim)
        span = int(W * 0.70)
        for x in range(span):
            a = int(225 * (1.0 - (x / span)) ** 1.35)
            sd.line([(x, 0), (x, H)], fill=(ink[0], ink[1], ink[2], a))
        img.alpha_composite(scrim)
    else:
        vig = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        vd = ImageDraw.Draw(vig)
        for yy in range(int(H * 0.25)):
            a = int(90 * (1.0 - yy / (H * 0.25)))
            vd.line([(0, H - 1 - yy), (W, H - 1 - yy)], fill=(0, 0, 0, a))
        img.alpha_composite(vig)
        draw = ImageDraw.Draw(img)

    # kicker chip
    kicker = clean(spec.get("kicker") or "").upper()
    y = int(140 * s)
    if kicker:
        f_k = load_font(bold_paths, int(34 * s))
        kw = text_w(draw, kicker, f_k)
        pad_x, pad_y = int(22 * s), int(10 * s)
        chip_h = int(34 * s) + pad_y * 2 + int(6 * s)
        draw.rectangle([safe_x, y, safe_x + kw + pad_x * 2, y + chip_h], fill=accent + (255,))
        draw.text((safe_x + pad_x, y + pad_y - int(2 * s)), kicker, font=f_k, fill=(255, 255, 255, 255))
        y += chip_h + int(26 * s)

    # headline: bold, up to 3 lines, auto-shrinks once
    headline = clean(spec.get("headline") or "")
    head_lines: list[str] = []
    if headline:
        size = int(76 * s)
        f_h = load_font(bold_paths, size)
        head_lines = wrap(draw, headline, f_h, text_col_w, 3)
        if len(head_lines) == 3 and len(headline) > 70:
            size = int(64 * s)
            f_h = load_font(bold_paths, size)
            head_lines = wrap(draw, headline, f_h, text_col_w, 3)
        for ln in head_lines:
            shadow_text(img, (safe_x, y), ln, f_h, (255, 255, 255, 255), blur=int(7 * s), off=(0, int(3 * s)))
            y += int(size * 1.12)
        draw = ImageDraw.Draw(img)
        y += int(22 * s)

    # bullet lines
    lines = [clean(str(x)) for x in (spec.get("lines") or []) if str(x).strip()][:4]
    if lines:
        f_l = load_font(reg_paths, int(40 * s))
        bullet = int(14 * s)
        for ln in lines:
            cy = y + int(40 * s) * 0.55
            draw.rectangle([safe_x, cy - bullet / 2, safe_x + bullet, cy + bullet / 2], fill=accent + (255,))
            txt = ellipsize(draw, ln, f_l, text_col_w - bullet - int(20 * s))
            shadow_text(img, (safe_x + bullet + int(20 * s), y), txt, f_l, (230, 236, 244, 255), blur=int(5 * s), off=(0, int(2 * s)), alpha=140)
            draw = ImageDraw.Draw(img)
            y += int(40 * s * 1.45)

    # station strap along the bottom
    draw_strap(img, spec, W, H, s, accent, ink, bold_paths, reg_paths, clean(spec.get("footer") or ""))

    Path(out).parent.mkdir(parents=True, exist_ok=True)
    img.save(out, "PNG", compress_level=3)
    return {"headlineLines": head_lines, "lines": len(lines)}


def draw_strap(img: Image.Image, spec: dict, W: int, H: int, s: float, accent, ink, bold_paths, reg_paths, footer: str) -> None:
    """Station strap: ink band with an accent rule, call letters, brand - series, and a right-aligned footer."""
    safe_x = int(W * 0.058)
    strap_h = int(84 * s)
    sy = H - strap_h - int(26 * s)
    strap = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(strap).rectangle([0, sy, W, sy + strap_h], fill=ink + (228,))
    img.alpha_composite(strap)
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, sy, W, sy + int(5 * s)], fill=accent + (255,))
    brand = clean(spec.get("brand") or "").upper()
    brand_name = clean(spec.get("brandName") or "")
    series = clean(spec.get("series") or "")
    f_b = load_font(bold_paths, int(34 * s))
    f_r = load_font(reg_paths, int(26 * s))
    tx = safe_x
    ty = sy + (strap_h - int(34 * s)) // 2 - int(2 * s)
    if brand:
        draw.text((tx, ty), brand, font=f_b, fill=(255, 255, 255, 255))
        tx += text_w(draw, brand, f_b) + int(22 * s)
    sub = " - ".join([p for p in (brand_name if brand_name.upper() != brand else "", series) if p])
    fw = text_w(draw, footer, f_r) if footer else 0
    if sub:
        room = max(int(W * 0.2), W - safe_x - fw - int(40 * s) - tx)
        draw.text((tx, ty + int(7 * s)), ellipsize(draw, sub, f_r, min(int(W * 0.45), room)), font=f_r, fill=(170, 184, 200, 255))
    if footer:
        draw.text((W - safe_x - fw, ty + int(7 * s)), footer, font=f_r, fill=(200, 210, 222, 255))


# ----------------------------------------------------------------------------- scenes (Headline News)
BED_W, BED_H = 2880, 1620          # 1.5x the frame: ffmpeg's zoompan then only ever scales the photo down


def _cover(img: Image.Image, w: int, h: int, ybias: float = 0.4) -> Image.Image:
    k = max(w / img.width, h / img.height)
    nw, nh = max(w, int(round(img.width * k))), max(h, int(round(img.height * k)))
    r = img.resize((nw, nh), Image.LANCZOS)
    x0, y0 = (nw - w) // 2, int((nh - h) * ybias)
    return r.crop((x0, y0, x0 + w, y0 + h))


def _backdrop(img: Image.Image, dim: float) -> Image.Image:
    from PIL import ImageEnhance
    small = _cover(img, BED_W // 8, BED_H // 8, 0.5).filter(ImageFilter.GaussianBlur(5))
    return ImageEnhance.Brightness(small.resize((BED_W, BED_H), Image.BICUBIC)).enhance(dim)


def render_photo_bed(photo: str, base: str, kind: str, story_layout: str = "box") -> tuple[str, str, list[int]]:
    """The still photo plane for one scene (JPEG, BED_W x BED_H). Returns (path, layout, [w, h]).
    layout "box": a story whose picture is framed whole in the furniture plane; the bed is its blurred copy."""
    from PIL import ImageOps
    im = ImageOps.exif_transpose(Image.open(photo)).convert("RGB")
    w, h = im.size
    if w < 80 or h < 80:
        raise ValueError(f"photo too small ({w}x{h})")
    if kind in ("intro", "outro"):
        bed, layout = _backdrop(im, 0.42), "backdrop"
    elif story_layout == "box":
        bed, layout = _backdrop(im, 0.55), "box"
    elif w / h >= 1.5:                                           # landscape: fill the frame, cropped a little above centre
        bed, layout = _cover(im, BED_W, BED_H), "cover"
    else:                                                        # square / portrait: inset on the right over a blurred copy
        bed, layout = _backdrop(im, 0.5).convert("RGBA"), "inset"
        fh = int(BED_H * 0.92)
        fw = int(round(w * fh / h))
        if fw > int(BED_W * 0.5):
            fw = int(BED_W * 0.5)
            fh = int(round(h * fw / w))
        fg = im.resize((fw, fh), Image.LANCZOS)
        x, y = BED_W - fw - int(BED_W * 0.10), (BED_H - fh) // 2
        sh = Image.new("RGBA", (BED_W, BED_H), (0, 0, 0, 0))
        ImageDraw.Draw(sh).rectangle([x - 6, y + 14, x + fw + 6, y + fh + 26], fill=(0, 0, 0, 170))
        bed.alpha_composite(sh.filter(ImageFilter.GaussianBlur(28)))
        bed = bed.convert("RGB")
        bed.paste(fg, (x, y))
    out = base + ".jpg"
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    bed.save(out, "JPEG", quality=90)
    return out, layout, [w, h]


def _setup(spec: dict):
    W = int(spec.get("width") or 1920)
    H = int(spec.get("height") or 1080)
    fonts = spec.get("fonts") or {}
    bold = ([fonts["bold"]] if fonts.get("bold") else []) + FONT_CANDIDATES_BOLD
    reg = ([fonts["regular"]] if fonts.get("regular") else []) + FONT_CANDIDATES_REG
    return W, H, H / 1080.0, hex_rgb(spec.get("accent"), (216, 0, 24)), hex_rgb(spec.get("ink"), (11, 18, 32)), bold, reg


def _scrim(img: Image.Image, W: int, H: int, ink, span: float, peak: int) -> None:
    scrim = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sd = ImageDraw.Draw(scrim)
    n = int(W * span)
    for x in range(n):
        sd.line([(x, 0), (x, H)], fill=(ink[0], ink[1], ink[2], int(peak * (1.0 - (x / n)) ** 1.35)))
    img.alpha_composite(scrim)


def _wash(img: Image.Image, W: int, H: int, ink, alpha: int) -> None:
    """Full-frame ink wash plus a bottom vignette, so big type reads over a busy backdrop."""
    wash = Image.new("RGBA", (W, H), ink + (alpha,))
    vd = ImageDraw.Draw(wash)
    for yy in range(int(H * 0.3)):
        vd.line([(0, H - 1 - yy), (W, H - 1 - yy)], fill=(0, 0, 0, min(255, alpha + int(80 * (1.0 - yy / (H * 0.3))))))
    img.alpha_composite(wash)


def _chip(img: Image.Image, x: int, y: int, text: str, font, bg, fg, s: float, outline=None) -> tuple[int, int]:
    """A filled label; returns (right edge, height)."""
    d = ImageDraw.Draw(img)
    tw = text_w(d, text, font)
    px, py = int(20 * s), int(8 * s)
    hgt = _metrics_h(font, s) + py * 2
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).rectangle([x, y, x + tw + px * 2, y + hgt], fill=bg, outline=outline, width=max(1, int(2 * s)) if outline else 0)
    img.alpha_composite(layer)
    ImageDraw.Draw(img).text((x + px, y + py), text, font=font, fill=fg)
    return x + tw + px * 2, hgt


def _spaced(img: Image.Image, xy, text: str, font, fill, spacing: int) -> int:
    """Letter-spaced caps (place names, eyebrows); returns the width drawn."""
    d = ImageDraw.Draw(img)
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=font, fill=fill)
        x += (font.getlength(ch) if hasattr(font, "getlength") else text_w(d, ch, font)) + spacing   # advance width: a space has no ink box
    return int(x - xy[0])


def _metrics_h(font, s: float) -> int:
    asc, desc = font.getmetrics() if hasattr(font, "getmetrics") else (int(30 * s), int(8 * s))
    return asc + desc


def _footer(spec: dict) -> str:
    return "  |  ".join(p for p in (clean(spec.get("date") or ""), clean(spec.get("site") or "")) if p)


def _story_box(img: Image.Image, photo_path: str, W: int, H: int, s: float, safe_x: int, accent, strap_top: int):
    """The post's own picture, whole and never cropped (editorial art carries its own text), framed on the right
    with a shadow and an accent rule. Returns the box's left edge, or None when the picture cannot be read."""
    from PIL import ImageOps
    try:
        im = ImageOps.exif_transpose(Image.open(photo_path)).convert("RGB")
    except Exception as e:  # noqa: BLE001
        log(f"box picture unreadable: {e}")
        return None
    ar = im.width / im.height
    if ar >= 1.0:
        bw = int(W * 0.47)
        bh = int(bw / ar)
        if bh > int(H * 0.60):
            bh = int(H * 0.60)
            bw = int(bh * ar)
    else:
        bh = int(H * 0.66)
        bw = int(bh * ar)
    top, bottom = int(196 * s), strap_top - int(40 * s)
    if bh > bottom - top:
        bh = bottom - top
        bw = int(bh * ar)
    x = W - safe_x - bw
    y = max(top, min(bottom - bh, int(H * 0.535) - bh // 2))
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rectangle([x - int(4 * s), y + int(10 * s), x + bw + int(4 * s), y + bh + int(24 * s)], fill=(0, 0, 0, 190))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(max(1, int(22 * s)))))
    img.paste(im.resize((bw, bh), Image.LANCZOS), (x, y))
    d = ImageDraw.Draw(img)
    d.rectangle([x - 1, y - 1, x + bw, y + bh], outline=(255, 255, 255, 90), width=max(1, int(2 * s)))
    d.rectangle([x, y + bh, x + bw, y + bh + int(7 * s)], fill=accent + (255,))
    return x


UP = (92, 214, 130)
DOWN = (255, 107, 107)
FLAT = (255, 214, 102)


def _tone_color(tone: str):
    """Market colours: green up, red down, gold for flat / unknown."""
    return UP if tone == "up" else DOWN if tone == "down" else FLAT


def _draw_spark(img: Image.Image, box, values, color, s: float, label: str = "") -> None:
    """A line chart of `values` in box=(x, y, w, h): soft fill under the line, the last point dotted, the range labelled at the ends."""
    vals = [float(v) for v in (values or []) if isinstance(v, (int, float))]
    x, y, w, h = [int(v) for v in box]
    if len(vals) < 2 or w < 10 or h < 10:
        return
    lo, hi = min(vals), max(vals)
    span = (hi - lo) or 1.0
    pad = int(h * 0.12)
    pts = [(x + int(i * w / (len(vals) - 1)), y + h - pad - int((v - lo) / span * (h - 2 * pad))) for i, v in enumerate(vals)]
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.polygon(pts + [(pts[-1][0], y + h), (pts[0][0], y + h)], fill=color + (46,))
    d.line(pts, fill=color + (255,), width=max(2, int(4 * s)), joint="curve")
    r = max(4, int(7 * s))
    d.ellipse([pts[-1][0] - r, pts[-1][1] - r, pts[-1][0] + r, pts[-1][1] + r], fill=(255, 255, 255, 255), outline=color + (255,), width=max(2, int(3 * s)))
    img.alpha_composite(layer)
    if label:
        f = load_font(FONT_CANDIDATES_REG, int(24 * s))
        ImageDraw.Draw(img).text((x, y + h + int(8 * s)), label, font=f, fill=(206, 216, 228, 220))


def _draw_rows(img: Image.Image, rows, x0: int, y0: int, w: int, s: float, bold, reg) -> int:
    """A quote table: label + ticker, a sparkline, the price and a coloured change chip per row. Returns the y below the table."""
    rows = [r for r in (rows or []) if isinstance(r, dict)][:6]
    if not rows:
        return y0
    row_h = int(118 * s)
    panel = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(panel).rounded_rectangle([x0 - int(22 * s), y0 - int(12 * s), x0 + w + int(22 * s), y0 + row_h * len(rows) + int(12 * s)], radius=int(18 * s), fill=(6, 12, 24, 128))
    img.alpha_composite(panel)
    f_l, f_s, f_v, f_c = load_font(bold, int(46 * s)), load_font(reg, int(28 * s)), load_font(bold, int(50 * s)), load_font(bold, int(36 * s))
    for i, r in enumerate(rows):
        top = y0 + i * row_h
        color = _tone_color(r.get("tone") or "")
        d = ImageDraw.Draw(img)
        if i:
            d.line([(x0, top), (x0 + w, top)], fill=(255, 255, 255, 38), width=max(1, int(2 * s)))
        d.text((x0 + int(10 * s), top + int(14 * s)), clean(r.get("label") or ""), font=f_l, fill=(255, 255, 255, 255))
        if r.get("sub"):
            d.text((x0 + int(10 * s), top + int(14 * s) + int(52 * s)), clean(r.get("sub")), font=f_s, fill=(190, 202, 218, 235))
        _draw_spark(img, (x0 + int(w * 0.34), top + int(18 * s), int(w * 0.20), row_h - int(36 * s)), r.get("spark"), color, s)
        val = clean(r.get("value") or "")
        d = ImageDraw.Draw(img)
        d.text((x0 + int(w * 0.77) - text_w(d, val, f_v), top + int(26 * s)), val, font=f_v, fill=(255, 255, 255, 255))
        chg = clean(r.get("change") or "")
        if chg:
            cw = text_w(d, chg, f_c) + int(34 * s)
            _chip(img, x0 + w - cw, top + int(30 * s), chg, f_c, color + (255,), (8, 14, 26, 255), s)
    return y0 + row_h * len(rows)



def render_story(spec: dict, sc: dict, out: str, photo: bool, layout: str, photo_path: str = "") -> None:
    W, H, s, accent, ink, bold, reg = _setup(spec)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    safe_x = int(W * 0.058)
    strap_top = H - int(84 * s) - int(26 * s)
    box_x = None
    if layout == "box" and photo_path:
        _scrim(img, W, H, ink, 0.62, 205)                        # the text column over the moving blurred backdrop
        box_x = _story_box(img, photo_path, W, H, s, safe_x, accent, strap_top)
    if box_x is not None:
        col_w = max(int(W * 0.30), box_x - int(56 * s) - safe_x)
    elif photo:
        col_w = int(W * 0.56)
        _scrim(img, W, H, ink, 0.74, 236)
    else:
        col_w = int(W * 0.80)
        _wash(img, W, H, ink, 40)
    boxed = box_x is not None
    y = int(104 * s)
    right, chip_h = _chip(img, safe_x, y, clean(spec.get("kicker") or "HEADLINE NEWS").upper(), load_font(bold, int(32 * s)), accent + (255,), (255, 255, 255, 255), s)
    cat = clean(sc.get("category") or "").upper()
    if cat:
        f_c = load_font(bold, int(26 * s))
        d = ImageDraw.Draw(img)
        cat = ellipsize(d, cat, f_c, max(int(120 * s), col_w - (right - safe_x) - int(40 * s)))
        cy = y + (chip_h - (_metrics_h(f_c, s) + int(16 * s))) // 2
        _chip(img, right + int(14 * s), cy, cat, f_c, (255, 255, 255, 40), (255, 255, 255, 240), s, outline=(255, 255, 255, 120))
    counter = clean(sc.get("counter") or "")
    if counter:
        f_n = load_font(bold, int(30 * s))
        d = ImageDraw.Draw(img)
        cw = text_w(d, counter, f_n) + int(40 * s)
        _chip(img, W - safe_x - cw, y, counter, f_n, (0, 0, 0, 150), (255, 255, 255, 255), s)
    y += chip_h + int(40 * s)
    place = clean(sc.get("place") or "").upper()
    if place:
        f_p = load_font(bold, int(30 * s))
        dot = int(12 * s)
        ImageDraw.Draw(img).ellipse([safe_x, y + int(12 * s), safe_x + dot, y + int(12 * s) + dot], fill=accent + (255,))
        _spaced(img, (safe_x + dot + int(14 * s), y), place, f_p, (255, 255, 255, 235), int(4 * s))
        y += int(30 * s * 1.6)
    headline = clean(sc.get("headline") or sc.get("title") or "")
    if headline:
        d = ImageDraw.Draw(img)
        big, small, max_l = (60, 52, 4) if boxed else (74, 62, 3)       # the boxed layout has a narrower column
        size = int(big * s)
        f_h = load_font(bold, size)
        lines = wrap(d, headline, f_h, col_w, max_l)
        if len(lines) == max_l and (len(headline) > 26 * max_l or lines[-1].endswith("...")):
            size = int(small * s)
            f_h = load_font(bold, size)
            lines = wrap(d, headline, f_h, col_w, max_l)
        for ln in lines:
            shadow_text(img, (safe_x, y), ln, f_h, (255, 255, 255, 255), blur=int(7 * s), off=(0, int(3 * s)))
            y += int(size * 1.1)
        y += int(20 * s)
    if sc.get("rows"):                                              # a quote table (markets): label, sparkline, price, change
        y = _draw_rows(img, sc.get("rows"), safe_x, y + int(10 * s), W - 2 * safe_x, s, bold, reg) + int(20 * s)
    detail = clean(sc.get("detail") or "")                          # the line a viewer must not miss: showtimes, the score, the price
    if detail:
        d = ImageDraw.Draw(img)
        big = bool(sc.get("spark"))                                  # a price next to its chart is the headline number
        f_d = load_font(bold, int((88 if big else 46) * s))
        dc = _tone_color(sc.get("tone") or "") + (255,)
        for ln in wrap(d, detail, f_d, col_w, 2):
            shadow_text(img, (safe_x, y), ln, f_d, dc, blur=int(6 * s), off=(0, int(3 * s)))
            y += int((88 if big else 46) * s * 1.18)
        y += int(14 * s)
    if sc.get("spark"):
        pw, ph = int(W * 0.40), int(H * 0.40)
        px, py = W - safe_x - pw, int(250 * s)
        pl = Image.new("RGBA", img.size, (0, 0, 0, 0))
        ImageDraw.Draw(pl).rounded_rectangle([px - int(28 * s), py - int(24 * s), px + pw + int(28 * s), py + ph + int(64 * s)], radius=int(20 * s), fill=(6, 12, 24, 128))
        img.alpha_composite(pl)
        _draw_spark(img, (px, py, pw, ph), sc.get("spark"), _tone_color(sc.get("tone") or ""), s, clean(sc.get("sparkLabel") or ""))
    summary = clean(sc.get("summary") or "")
    if summary:
        d = ImageDraw.Draw(img)
        fs = int((32 if boxed else 36) * s)
        f_s = load_font(reg, fs)
        step = int(fs * 1.42)
        max_lines = max(1, min(4 if boxed else 3, (strap_top - int(30 * s) - y) // max(1, step)))
        for ln in wrap(d, summary, f_s, col_w, max_lines):
            shadow_text(img, (safe_x, y), ln, f_s, (226, 233, 243, 255), blur=int(5 * s), off=(0, int(2 * s)), alpha=150)
            y += step
    draw_strap(img, spec, W, H, s, accent, ink, bold, reg, _footer(spec))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    img.save(out, "PNG", compress_level=3)


def render_intro(spec: dict, sc: dict, out: str, photo: bool, layout: str) -> None:
    W, H, s, accent, ink, bold, reg = _setup(spec)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    safe_x = int(W * 0.058)
    _wash(img, W, H, ink, 120 if photo else 30)
    y = int(196 * s)
    _, ch = _chip(img, safe_x, y, clean(sc.get("eyebrow") or "TOP STORIES").upper(), load_font(bold, int(28 * s)), accent + (255,), (255, 255, 255, 255), s)
    y += ch + int(18 * s)
    title = clean(spec.get("kicker") or "HEADLINE NEWS").upper()
    f_t = load_font(bold, int(128 * s))
    d = ImageDraw.Draw(img)
    title = ellipsize(d, title, f_t, int(W * 0.86))
    shadow_text(img, (safe_x - int(4 * s), y), title, f_t, (255, 255, 255, 255), blur=int(9 * s), off=(0, int(4 * s)))
    y += int(128 * s * 1.12)
    ImageDraw.Draw(img).rectangle([safe_x, y, safe_x + int(180 * s), y + int(9 * s)], fill=accent + (255,))
    y += int(30 * s)
    site_name = clean(spec.get("siteName") or spec.get("brandName") or "")
    sub = clean(sc.get("sub") or "") or (("from " + site_name) if site_name else "")           # a scene's own sub line replaces "from <site>"
    if sub:
        f_sub = load_font(reg, int(44 * s))
        shadow_text(img, (safe_x, y), ellipsize(ImageDraw.Draw(img), sub, f_sub, int(W * 0.86)), f_sub, (214, 222, 234, 255), blur=int(5 * s), off=(0, int(2 * s)), alpha=150)
        y += int(44 * s * 1.3)
    date = clean(spec.get("date") or "")
    if date:
        _spaced(img, (safe_x, y + int(4 * s)), date.upper(), load_font(bold, int(30 * s)), (236, 241, 248, 255), int(3 * s))
        y += int(30 * s * 1.7)
    y += int(24 * s)
    rundown = [clean(str(t)) for t in (sc.get("rundown") or []) if str(t).strip()][:5]
    f_r = load_font(reg, int(32 * s))
    f_n = load_font(bold, int(26 * s))
    box = int(42 * s)
    strap_top = H - int(84 * s) - int(26 * s)
    for i, t in enumerate(rundown):
        if y + box > strap_top - int(16 * s):
            break
        d = ImageDraw.Draw(img)
        d.rectangle([safe_x, y, safe_x + box, y + box], fill=accent + (255,))
        num = str(i + 1)
        d.text((safe_x + (box - text_w(d, num, f_n)) // 2, y + int(5 * s)), num, font=f_n, fill=(255, 255, 255, 255))
        shadow_text(img, (safe_x + box + int(20 * s), y + int(1 * s)), ellipsize(d, t, f_r, int(W * 0.80)), f_r, (236, 241, 248, 255), blur=int(4 * s), off=(0, int(2 * s)), alpha=140)
        y += int(58 * s)
    draw_strap(img, spec, W, H, s, accent, ink, bold, reg, clean(spec.get("site") or ""))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    img.save(out, "PNG", compress_level=3)


def render_outro(spec: dict, sc: dict, out: str, photo: bool, layout: str) -> None:
    W, H, s, accent, ink, bold, reg = _setup(spec)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    safe_x = int(W * 0.058)
    _wash(img, W, H, ink, 150 if photo else 30)
    site = clean(spec.get("site") or "")
    name = clean(spec.get("brandName") or spec.get("siteName") or "")
    y = int(318 * s)
    eyebrow = clean(sc.get("eyebrow") or "").upper() or ("FOR MORE ON THESE STORIES" if site else "THAT'S THE LATEST")
    _spaced(img, (safe_x, y), eyebrow, load_font(bold, int(38 * s)), accent + (255,), int(5 * s))
    y += int(38 * s * 1.8)
    big = clean(sc.get("big") or "") or site or name or "HEADLINE NEWS"
    f_b = load_font(bold, int(124 * s))
    d = ImageDraw.Draw(img)
    big = ellipsize(d, big, f_b, int(W * 0.86))
    shadow_text(img, (safe_x - int(4 * s), y), big, f_b, (255, 255, 255, 255), blur=int(9 * s), off=(0, int(4 * s)))
    y += int(124 * s * 1.18)
    ImageDraw.Draw(img).rectangle([safe_x, y, safe_x + text_w(ImageDraw.Draw(img), big, f_b), y + int(10 * s)], fill=accent + (255,))
    y += int(44 * s)
    sub = clean(sc.get("sub") or "") or (name if name and name.lower() != big.lower() else "")     # a scene's own sub line (phone, address, source) replaces the site name
    if sub:
        f_sub = load_font(reg, int(42 * s))
        for ln in wrap(ImageDraw.Draw(img), sub, f_sub, int(W * 0.86), 2):
            shadow_text(img, (safe_x, y), ln, f_sub, (206, 216, 228, 255), blur=int(5 * s), off=(0, int(2 * s)), alpha=150)
            y += int(42 * s * 1.3)
    draw_strap(img, spec, W, H, s, accent, ink, bold, reg, clean(spec.get("date") or ""))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    img.save(out, "PNG", compress_level=3)


def render_scenes(spec: dict) -> dict:
    W, H, _s, accent, ink, _b, _r = _setup(spec)
    story_layout = str(spec.get("storyLayout") or "box").lower()      # box: the picture framed whole; full: the picture fills the frame
    done = []
    for i, sc in enumerate(spec.get("scenes") or []):
        kind = sc.get("kind") or "story"
        if sc.get("anim"):                                           # an animated scene (weather): a looping frame-sequence bed + its furniture
            sys.path.insert(0, str(Path(__file__).resolve().parent))  # python -I leaves the script's own folder off sys.path
            import weather_cards                                     # lazily: it imports this module back
            r = weather_cards.render_scene(spec, sc, sc["furniture"], sc["bedBase"])
            done.append({"kind": kind, "furniture": str(Path(sc["furniture"]).resolve()), "bed": str(Path(r["bed"]).resolve()), "photo": False, "layout": "animated", "photoSize": None,
                         "bedSeq": str(Path(r["bed"]).resolve().parent / Path(r["bedSeq"]).name), "frames": r["frames"], "fps": r["fps"]})
            continue
        photo = sc.get("photo") or ""
        bed, layout, size = "", "gradient", None
        if photo and Path(photo).is_file():
            try:
                bed, layout, size = render_photo_bed(photo, sc["bedBase"], kind, story_layout)
            except Exception as e:  # noqa: BLE001
                log(f"scene {i}: photo unusable ({e}); gradient bed")
                bed, layout = "", "gradient"
        if not bed:
            bed = sc["bedBase"] + ".png"
            render_bed(bed, W, H, accent, ink)
        if kind in ("intro", "outro"):
            (render_intro if kind == "intro" else render_outro)(spec, sc, sc["furniture"], layout != "gradient", layout)
        else:
            render_story(spec, sc, sc["furniture"], layout != "gradient", layout, photo if layout == "box" else "")
        done.append({"kind": kind, "furniture": str(Path(sc["furniture"]).resolve()), "bed": str(Path(bed).resolve()),
                     "photo": layout != "gradient", "layout": layout, "photoSize": size})
    return {"ok": True, "scenes": done}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spec", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--bed", default="")
    ap.add_argument("--scenes", default="", help="Headline News: paint every scene of scenes.json (see the module doc)")
    a = ap.parse_args()
    if a.scenes:
        try:
            spec = json.loads(Path(a.scenes).read_text(encoding="utf-8-sig"))
            print(json.dumps(render_scenes(spec)))
            return 0
        except Exception as e:  # noqa: BLE001
            log(repr(e))
            print(json.dumps({"ok": False, "error": str(e)}))
            return 2
    if not a.spec or not a.out:
        print(json.dumps({"ok": False, "error": "--spec and --out are required (or --scenes)"}))
        return 2
    try:
        spec = json.loads(Path(a.spec).read_text(encoding="utf-8-sig"))
    except Exception as e:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": f"bad spec: {e}"}))
        return 2
    try:
        info = render_furniture(spec, a.out)
        bed = ""
        if a.bed:
            render_bed(a.bed, int(spec.get("width") or 1920), int(spec.get("height") or 1080),
                       hex_rgb(spec.get("accent"), (216, 0, 24)), hex_rgb(spec.get("ink"), (11, 18, 32)))
            bed = str(Path(a.bed).resolve())
        print(json.dumps({"ok": True, "furniture": str(Path(a.out).resolve()), "bed": bed, **info}))
        return 0
    except Exception as e:  # noqa: BLE001
        log(repr(e))
        print(json.dumps({"ok": False, "error": str(e)}))
        return 2


if __name__ == "__main__":
    sys.exit(main())
