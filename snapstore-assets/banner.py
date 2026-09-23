#!/usr/bin/env python3
"""Snap Store featured banners (720x240) for retro-midi-player-opl3-gm.

Two variants for comparison:
  banner-led.png       app-style: navy + the LED meter + the app's pixel font
  banner-synthwave.png sunset / perspective grid / neon LED skyline

Text uses the app's own 5x7 bitmap font, parsed from src/font5x7.h.
Run from anywhere: python3 snapstore-assets/banner.py  (writes next to itself).
"""
import os, re
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(OUT)
W, H = 720, 240

# ── the app's 5x7 font ─────────────────────────────────────────────────────────
FONT = {}
for m in re.finditer(r"\{'(.)', \{((?:\"[^\"]*\",?)+)\}\}", open(f"{REPO}/src/font5x7.h").read()):
    FONT[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))

def text_w(s, sc):
    return len(s) * 6 * sc - sc

def draw_text(d, x, y, s, sc, col):
    for ch in s.upper():
        g = FONT.get(ch)
        if g:
            for ry, row in enumerate(g):
                for rx, px in enumerate(row):
                    if px == "#":
                        d.rectangle([x + rx * sc, y + ry * sc, x + rx * sc + sc - 1, y + ry * sc + sc - 1], fill=col)
        x += 6 * sc

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))

# A pleasant spectrum-ish level per channel (0..1), and peak-hold caps.
LEVELS = [0.30, 0.56, 0.92, 0.68, 0.40, 0.86, 0.62, 0.24, 0.50, 0.79, 0.45, 0.20,
          0.66, 0.35, 0.74, 0.27, 0.52, 0.16]
PEAKS  = [min(1.0, l + 0.10 + 0.05 * ((i * 7) % 3)) for i, l in enumerate(LEVELS)]

# ── Variant 1: app-style LED ───────────────────────────────────────────────────
def led():
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):   # navy, slightly lighter at the top (like the app header)
        d.line([(0, y), (W, y)], fill=lerp((22, 24, 40), (10, 11, 19), y / H))

    # Title block, left.
    x0 = 34
    draw_text(d, x0, 40, "RETRO MIDI", 5, (240, 240, 255))
    draw_text(d, x0, 86, "PLAYER", 5, (240, 240, 255))
    d.rectangle([x0, 136, x0 + 48, 139], fill=(70, 120, 200))   # the seek-bar blue
    draw_text(d, x0, 152, "OPL3 FM + GENERAL MIDI", 2, (150, 170, 210))
    draw_text(d, x0, 176, "RETROWAVE OPL3 EXPRESS", 2, (95, 105, 150))
    draw_text(d, x0, 194, "PC AUDIO - EXTERNAL MIDI", 2, (95, 105, 150))

    # LED meter panel, right — same colours/segmenting as the app's LED style.
    px, py, pw, ph = 392, 28, 300, 172
    d.rectangle([px - 8, py - 8, px + pw + 8, py + ph + 24], fill=(8, 8, 14))
    n, segs = 18, 18
    slot = pw / n
    bw = int(slot) - 4
    seg_h = ph // segs
    for ch in range(n):
        x = int(px + ch * slot) + 2
        lit = round(LEVELS[ch] * segs)
        pk = round(PEAKS[ch] * segs)
        for s in range(segs):
            sy = py + ph - (s + 1) * seg_h + 1
            col = (40, 220, 70) if s < segs * 0.6 else (240, 200, 40) if s < segs * 0.85 else (240, 60, 40)
            if s + 1 == pk:
                c = (255, 255, 255)
            elif s < lit:
                c = col
            else:
                c = lerp((8, 8, 14), col, 0.10)   # unlit: ~alpha 26 over the panel
            d.rectangle([x, sy, x + bw - 1, sy + seg_h - 2], fill=c)
        lbl = str(ch + 1)
        draw_text(d, x + bw // 2 - text_w(lbl, 1) // 2, py + ph + 6, lbl, 1, (80, 80, 100))
    img.save(f"{OUT}/banner-led.png")

# ── Variant 2: synthwave neon ──────────────────────────────────────────────────
def synthwave():
    S = 4                       # supersample, then downscale for smooth lines
    w, h = W * S, H * S
    hz = int(156 * S)           # horizon
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)

    # Sky: deep violet -> hot pink at the horizon.
    for y in range(hz):
        t = y / hz
        c = lerp((18, 4, 44), (92, 12, 96), min(1, t * 1.3)) if t < 0.75 else \
            lerp((92, 12, 96), (230, 50, 120), (t - 0.75) / 0.25)
        d.line([(0, y), (w, y)], fill=c)
    # Ground.
    for y in range(hz, h):
        d.line([(0, y), (w, y)], fill=lerp((30, 6, 52), (10, 2, 22), (y - hz) / (h - hz)))

    # Sun with the classic horizontal cut-outs.
    cx, cy, r = w // 2, hz - 6 * S, 74 * S
    sun = Image.new("L", (w, h), 0)
    sd = ImageDraw.Draw(sun)
    sd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    for i in range(6):
        gy = cy + int(r * (0.05 + 0.16 * i))
        gh = int((2 + i * 1.6) * S)
        sd.rectangle([0, gy, w, gy + gh], fill=0)
    sd.rectangle([0, hz, w, h], fill=0)
    sun_col = Image.new("RGB", (w, h))
    scd = ImageDraw.Draw(sun_col)
    for y in range(cy - r, cy + r):
        scd.line([(0, y), (w, y)], fill=lerp((255, 220, 90), (255, 60, 140), (y - (cy - r)) / (2 * r)))
    glow = sun.filter(ImageFilter.GaussianBlur(18 * S))
    img = Image.composite(Image.blend(img, Image.new("RGB", (w, h), (255, 90, 150)), 0.55), img, glow)
    img = Image.composite(sun_col, img, sun)
    d = ImageDraw.Draw(img)

    # Perspective grid on the ground.
    grid = (40, 220, 255)
    for i in range(1, 12):
        t = (i / 11) ** 2.1
        y = hz + int(t * (h - hz))
        d.line([(0, y), (w, y)], fill=lerp((60, 20, 90), grid, 0.35 + 0.65 * t), width=max(1, int(S * (0.6 + t))))
    for k in range(-14, 15):
        x_far = cx + k * 26 * S
        x_near = cx + k * 150 * S
        d.line([(x_far, hz), (x_near, h)], fill=lerp((60, 20, 90), grid, 0.8), width=S)
    d.line([(0, hz), (w, hz)], fill=(255, 120, 200), width=2 * S)

    # Neon LED skyline on the horizon (the app's NEON bar style: cyan -> pink).
    n = 18
    left, right = 40 * S, w - 40 * S
    slot = (right - left) / n
    bw = int(slot * 0.62)
    bars = Image.new("RGB", (w, h))
    mask = Image.new("L", (w, h), 0)
    bd, md = ImageDraw.Draw(bars), ImageDraw.Draw(mask)
    for ch in range(n):
        x = int(left + ch * slot + (slot - bw) / 2)
        # Keep the centre low so the sun stays visible.
        centre = abs((ch + 0.5) / n - 0.5) * 2
        bh = int((14 + 78 * LEVELS[ch] * (0.12 + 0.88 * centre ** 1.4)) * S)
        top = hz - bh
        for y in range(top, hz, 3 * S):
            t = (hz - y) / (95 * S)
            bd.rectangle([x, y, x + bw, y + 2 * S], fill=lerp((40, 220, 255), (255, 80, 200), min(1, t)))
            md.rectangle([x, y, x + bw, y + 2 * S], fill=255)
        pk = top - int(6 * S)
        bd.rectangle([x, pk, x + bw, pk + int(1.5 * S)], fill=(255, 255, 255))
        md.rectangle([x, pk, x + bw, pk + int(1.5 * S)], fill=255)
    bar_glow = mask.filter(ImageFilter.GaussianBlur(6 * S))
    img = Image.composite(Image.blend(img, Image.new("RGB", (w, h), (120, 160, 255)), 0.5), img, bar_glow)
    img = Image.composite(bars, img, mask)

    # Neon title (glow + core), centred in the sky.
    title, sc = "RETRO MIDI PLAYER", 4 * S
    tl = Image.new("L", (w, h), 0)
    tx = (w - text_w(title, sc)) // 2
    draw_text(ImageDraw.Draw(tl), tx, 14 * S, title, sc, 255)
    sub, ssc = "OPL3 FM + GENERAL MIDI", 2 * S
    sl = Image.new("L", (w, h), 0)
    draw_text(ImageDraw.Draw(sl), (w - text_w(sub, ssc)) // 2, 52 * S, sub, ssc, 255)
    for layer, core, halo in ((tl, (255, 240, 255), (255, 60, 200)), (sl, (190, 250, 255), (40, 200, 255))):
        g = layer.filter(ImageFilter.GaussianBlur(5 * S))
        img = Image.composite(Image.blend(img, Image.new("RGB", (w, h), halo), 0.85), img, g)
        img = Image.composite(Image.new("RGB", (w, h), core), img, layer)

    img.resize((W, H), Image.LANCZOS).save(f"{OUT}/banner-synthwave.png")

led()
synthwave()
