#!/usr/bin/env python3
# PShare - Draws the icon, the home-screen backgrounds and the in-app art.
# Copyright (C) 2026 PShare contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render PShare's artwork from code: flat, geometric, deliberately unrealistic.

The motif is a capture card (a rounded frame with a little sun-and-hills
picture in it) launching a paper plane along a dashed flight path. Everything
is drawn with Pillow at a multiple of the final size and scaled down, which
is the anti-aliasing.

Outputs
  sce_sys/icon0.png                    512 x 512 home-screen tile
  sce_sys/background-source.png        3840 x 2160, shown while PShare is selected
  sce_sys/launch-background-source.png 3840 x 2160, shown while it starts (the splash)
  sce_sys/pic0.dds, pic1.dds           the two above as BC7 DDS (needs `pip install etcpak`)
  assets/art/backdrop.jpg              1920 x 1080 behind the app's screens
  assets/art/splash.jpg                1920 x 1080 the app's own intro
  assets/art/logo.png                  512 x 512 the mark on transparency
  assets/art/empty.png                 640 x 440 empty-state illustration

usage: tools/make-art.py [--no-dds] [--only icon|backgrounds|app]
"""

import argparse
import math
import random
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "third_party/fonts"

# The palette the app's theme uses too (src/app/style.cpp).
NIGHT = (21, 18, 58)
NIGHT_2 = (33, 28, 85)
INK = (14, 11, 40)
VIOLET = (107, 76, 230)
PLUM = (150, 60, 190)
CORAL = (255, 92, 138)
AQUA = (62, 230, 224)
SUN = (255, 210, 94)
CREAM = (255, 244, 230)
WHITE = (255, 255, 255)


def rgba(color, alpha=255):
    return (color[0], color[1], color[2], alpha)


# ---- primitives ---------------------------------------------------------


def gradient(size, stops, angle=90.0):
    """A linear gradient; angle in degrees, 90 is top to bottom."""
    w, h = size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    a = math.radians(angle)
    dx, dy = math.cos(a), math.sin(a)
    t = (x * dx + y * dy)
    t = (t - t.min()) / max(1e-6, (t.max() - t.min()))
    out = np.zeros((h, w, 4), np.float32)
    positions = [p for p, _ in stops]
    for channel in range(4):
        values = [(c + (255,))[channel] if len(c) == 3 else c[channel] for _, c in stops]
        out[..., channel] = np.interp(t, positions, values)
    return Image.fromarray(out.clip(0, 255).astype(np.uint8), "RGBA")


def radial(size, center, radius, color, alpha=255, power=1.6):
    """A soft round glow."""
    w, h = size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((x - center[0]) ** 2 + (y - center[1]) ** 2) / radius
    a = np.clip(1.0 - d, 0.0, 1.0) ** power * alpha
    out = np.zeros((h, w, 4), np.uint8)
    out[..., 0], out[..., 1], out[..., 2] = color
    out[..., 3] = a.astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def layer(size):
    return Image.new("RGBA", size, (0, 0, 0, 0))


def paste(base, top):
    base.alpha_composite(top)
    return base


def rotate_points(points, angle, cx, cy):
    a = math.radians(angle)
    c, s = math.cos(a), math.sin(a)
    return [((x - cx) * c - (y - cy) * s + cx, (x - cx) * s + (y - cy) * c + cy) for x, y in points]


def rounded_poly(draw, box, radius, fill, angle=0.0, outline=None, width=0):
    """A rounded rectangle turned by angle degrees about its centre."""
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    r = min(radius, (x1 - x0) / 2, (y1 - y0) / 2)
    points = []
    for corner, (ox, oy) in enumerate(((x1 - r, y0 + r), (x1 - r, y1 - r), (x0 + r, y1 - r),
                                       (x0 + r, y0 + r))):
        start = -90 + corner * 90
        for step in range(0, 13):
            t = math.radians(start + step * 7.5)
            points.append((ox + r * math.cos(t), oy + r * math.sin(t)))
    points = rotate_points(points, angle, cx, cy)
    draw.polygon(points, fill=fill, outline=outline, width=width)
    return points


def shadow_of(image, offset, blur, alpha=110, color=INK):
    """A soft drop shadow of every opaque pixel in `image`."""
    a = image.getchannel("A")
    shade = Image.new("RGBA", image.size, rgba(color, 0))
    shade.putalpha(a.point(lambda v: v * alpha // 255))
    shade = shade.filter(ImageFilter.GaussianBlur(blur))
    moved = layer(image.size)
    moved.paste(shade, (int(offset[0]), int(offset[1])), shade)
    return moved


def sparkle(draw, cx, cy, r, color):
    """A four-pointed twinkle."""
    t = r * 0.22
    draw.polygon([(cx, cy - r), (cx + t, cy - t), (cx + r, cy), (cx + t, cy + t), (cx, cy + r),
                  (cx - t, cy + t), (cx - r, cy), (cx - t, cy - t)], fill=color)


def dashed_curve(draw, points, width, color, dash=3.0, gap=2.2):
    """A dashed Catmull-Rom path through points; dash and gap are in widths."""
    samples = []
    for i in range(len(points) - 1):
        p0 = points[max(0, i - 1)]
        p1, p2 = points[i], points[i + 1]
        p3 = points[min(len(points) - 1, i + 2)]
        for s in range(40):
            t = s / 40
            t2, t3 = t * t, t * t * t
            x = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2
                       + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            y = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2
                       + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            samples.append((x, y))
    samples.append(points[-1])
    on, run, period = True, 0.0, (dash + gap) * width
    segment = [samples[0]]
    for a, b in zip(samples, samples[1:]):
        run += math.dist(a, b)
        segment.append(b)
        limit = dash * width if on else gap * width
        if run >= limit:
            if on and len(segment) > 1:
                draw.line(segment, fill=color, width=int(width), joint="curve")
                r = width / 2
                for p in (segment[0], segment[-1]):
                    draw.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=color)
            on, run, segment = not on, 0.0, [b]
    del period


def plane(draw, cx, cy, size, angle, body=WHITE, fold=None, shade=None):
    """A flat paper plane pointing along `angle` (degrees, 0 = right)."""
    fold = fold or (205, 214, 255)
    shade = shade or (170, 180, 240)
    s = size
    nose = (cx + s, cy)
    tail_top = (cx - s * 0.75, cy - s * 0.62)
    tail_bottom = (cx - s * 0.75, cy + s * 0.62)
    keel = (cx - s * 0.32, cy + s * 0.08)
    notch = (cx - s * 0.48, cy)
    wing_upper = [nose, tail_top, notch]
    wing_lower = [nose, notch, tail_bottom]
    keel_part = [nose, notch, keel, (cx - s * 0.2, cy + s * 0.36)]
    def turn(points):
        return rotate_points(points, angle, cx, cy)
    draw.polygon(turn(wing_lower), fill=fold)
    draw.polygon(turn(keel_part), fill=shade)
    draw.polygon(turn(wing_upper), fill=body)


def capture_card(size, angle=-8.0, frame=CREAM, sky_top=AQUA, sky_bottom=VIOLET, sun=SUN,
                 hills=(CORAL, PLUM), radius_share=0.12):
    """The capture card: a rounded frame with a sun over two hills, on its own layer."""
    w, h = size
    pad = int(min(w, h) * 0.08)
    image = layer((w + pad * 4, h + pad * 4))
    ox, oy = pad * 2, pad * 2
    draw = ImageDraw.Draw(image)
    r = min(w, h) * radius_share
    rounded_poly(draw, (ox, oy, ox + w, oy + h), r, rgba(frame))
    # The picture inside the frame.
    inner = layer(image.size)
    idraw = ImageDraw.Draw(inner)
    m = min(w, h) * 0.075
    box = (ox + m, oy + m, ox + w - m, oy + h - m * 2.4)
    sky = gradient((int(box[2] - box[0]), int(box[3] - box[1])), [(0, sky_top), (1, sky_bottom)])
    mask = layer(sky.size)
    rounded_poly(ImageDraw.Draw(mask), (0, 0, sky.size[0], sky.size[1]), r * 0.55, WHITE + (255,))
    pic = layer(sky.size)
    pic.paste(sky, (0, 0), mask)
    pdraw = ImageDraw.Draw(pic)
    pw, ph = sky.size
    pdraw.ellipse((pw * 0.62, ph * 0.16, pw * 0.86, ph * 0.16 + pw * 0.24), fill=rgba(sun))
    pdraw.polygon([(-pw * 0.1, ph), (pw * 0.32, ph * 0.42), (pw * 0.78, ph)], fill=rgba(hills[1]))
    pdraw.polygon([(pw * 0.25, ph), (pw * 0.66, ph * 0.56), (pw * 1.15, ph)], fill=rgba(hills[0]))
    # Clip the hills to the rounded picture.
    clipped = layer(pic.size)
    clipped.paste(pic, (0, 0), mask)
    inner.paste(clipped, (int(box[0]), int(box[1])), clipped)
    image.alpha_composite(inner)
    # Two little "caption" bars under the picture.
    bar_y = oy + h - m * 1.55
    draw.rounded_rectangle((ox + m, bar_y, ox + m + w * 0.38, bar_y + m * 0.55), m * 0.3,
                           fill=rgba(VIOLET, 120))
    draw.rounded_rectangle((ox + m + w * 0.42, bar_y, ox + m + w * 0.58, bar_y + m * 0.55),
                           m * 0.3, fill=rgba(CORAL, 150))
    return image.rotate(-angle, resample=Image.BICUBIC, expand=False)


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), int(size))


# ---- the mark -----------------------------------------------------------


def mark(size, background=True, rounded=True):
    """The PShare mark: a capture card launching a plane, at size x size."""
    S = size * 4
    image = layer((S, S))
    if background:
        tile = gradient((S, S), [(0, VIOLET), (0.55, PLUM), (1, CORAL)], angle=55)
        mask = layer((S, S))
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), S * 0.22 if rounded else 0,
                                               fill=WHITE + (255,))
        image.paste(tile, (0, 0), mask)
        paste(image, radial((S, S), (S * 0.25, S * 0.18), S * 0.75, WHITE, 70))
        image.putalpha(Image.fromarray(np.minimum(np.array(image.getchannel("A")),
                                                  np.array(mask.getchannel("A"))), "L"))
        d = ImageDraw.Draw(image)
        for x, y, r in ((0.16, 0.2, 0.035), (0.86, 0.78, 0.028), (0.12, 0.8, 0.02)):
            sparkle(d, S * x, S * y, S * r, rgba(CREAM, 220))
    card = capture_card((int(S * 0.56), int(S * 0.46)), angle=-9)
    card_shadow = shadow_of(card, (S * 0.012, S * 0.03), S * 0.02, 120)
    cx, cy = S * 0.42, S * 0.58
    paste(image, place(card_shadow, image.size, cx, cy))
    paste(image, place(card, image.size, cx, cy))
    d = ImageDraw.Draw(image)
    dashed_curve(d, [(S * 0.52, S * 0.4), (S * 0.62, S * 0.27), (S * 0.7, S * 0.24)],
                 S * 0.018, rgba(CREAM, 230), dash=2.2, gap=1.8)
    planes = layer((S, S))
    plane(ImageDraw.Draw(planes), S * 0.76, S * 0.22, S * 0.15, -28, body=rgba(WHITE),
          fold=rgba((255, 226, 150)), shade=rgba(SUN))
    paste(image, shadow_of(planes, (S * 0.01, S * 0.02), S * 0.012, 90))
    paste(image, planes)
    return image.resize((size, size), Image.LANCZOS)


def place(image, canvas_size, cx, cy):
    out = layer(canvas_size)
    out.paste(image, (int(cx - image.size[0] / 2), int(cy - image.size[1] / 2)), image)
    return out


# ---- scenes -------------------------------------------------------------


def night_sky(size, seed, glow=True, stars_count=140, sparkles=16, dots_on=True):
    w, h = size
    rng = random.Random(seed)
    image = gradient(size, [(0, (30, 22, 84)), (0.55, NIGHT), (1, INK)], angle=100)
    if glow:
        paste(image, radial(size, (w * 0.82, h * 0.2), w * 0.55, PLUM, 120))
        paste(image, radial(size, (w * 0.1, h * 1.0), w * 0.5, VIOLET, 110))
        paste(image, radial(size, (w * 0.95, h * 0.95), w * 0.35, CORAL, 60))
    # Big soft blobs, flat but translucent.
    blobs = layer(size)
    d = ImageDraw.Draw(blobs)
    for color, x, y, r in ((VIOLET, 0.78, 0.08, 0.22), (PLUM, 0.98, 0.62, 0.2),
                           (AQUA, 0.6, 1.05, 0.16), (CORAL, 0.05, 0.12, 0.1)):
        d.ellipse((w * x - w * r, h * y - w * r, w * x + w * r, h * y + w * r), fill=rgba(color, 46))
    paste(image, blobs.filter(ImageFilter.GaussianBlur(w * 0.004)))
    # A dot grid that fades out to the left.
    dots = layer(size)
    d = ImageDraw.Draw(dots)
    step = w / 48
    for gy in range(int(h / step) + 1 if dots_on else 0):
        for gx in range(int(w / step) + 1):
            fade = (gx * step / w) ** 1.8
            if fade < 0.08:
                continue
            r = w * 0.0011
            x, y = gx * step + step / 2, gy * step + step / 2
            d.ellipse((x - r, y - r, x + r, y + r), fill=rgba(CREAM, int(46 * fade)))
    paste(image, dots)
    # Stars and twinkles.
    stars = layer(size)
    d = ImageDraw.Draw(stars)
    for _ in range(stars_count):
        x, y = rng.random() * w, rng.random() * h
        r = w * (0.0008 + rng.random() * 0.0016)
        d.ellipse((x - r, y - r, x + r, y + r), fill=rgba(CREAM, rng.randint(60, 200)))
    for _ in range(sparkles):
        x, y = rng.random() * w, rng.random() * h * 0.9
        sparkle(d, x, y, w * (0.004 + rng.random() * 0.006), rgba(rng.choice((SUN, AQUA, CREAM)), 200))
    paste(image, stars)
    return image


def selection_background():
    """pic0: room on the left for the shell's title and buttons, the scene on the right."""
    W, H = 3840, 2160
    S = 2
    size = (W * S, H * S)
    image = night_sky(size, 7)
    w, h = size
    d = ImageDraw.Draw(image)
    # Flight paths sweeping in from the left.
    dashed_curve(d, [(w * 0.38, h * 0.92), (w * 0.52, h * 0.7), (w * 0.62, h * 0.66), (w * 0.72, h * 0.42),
                     (w * 0.9, h * 0.26)], w * 0.0042, rgba(CREAM, 150))
    dashed_curve(d, [(w * 0.55, h * 1.02), (w * 0.7, h * 0.86), (w * 0.86, h * 0.8), (w * 1.02, h * 0.62)],
                 w * 0.0032, rgba(AQUA, 140))
    # Three capture cards, the big one in front.
    cards = [
        ((0.2, 0.17), (0.62, 0.34), 10, dict(sky_top=CORAL, sky_bottom=PLUM, hills=(VIOLET, INK))),
        ((0.16, 0.13), (0.9, 0.58), -6, dict(sky_top=SUN, sky_bottom=CORAL, hills=(PLUM, VIOLET))),
        ((0.3, 0.25), (0.74, 0.56), -9, {}),
    ]
    for (cw, ch), (cx, cy), angle, colors in cards:
        card = capture_card((int(w * cw), int(w * ch)), angle=angle, **colors)
        paste(image, place(shadow_of(card, (w * 0.004, w * 0.008), w * 0.006, 130), size, w * cx, h * cy))
        paste(image, place(card, size, w * cx, h * cy))
    planes = layer(size)
    pd = ImageDraw.Draw(planes)
    plane(pd, w * 0.905, h * 0.25, w * 0.05, -32, fold=(255, 226, 150), shade=SUN)
    plane(pd, w * 0.66, h * 0.15, w * 0.022, -12, body=AQUA, fold=(40, 180, 190), shade=(30, 140, 160))
    plane(pd, w * 0.985, h * 0.6, w * 0.026, -40, body=CORAL, fold=(220, 60, 110), shade=(170, 40, 90))
    paste(image, shadow_of(planes, (w * 0.003, w * 0.006), w * 0.004, 100))
    paste(image, planes)
    # Darken the left third a touch so the shell's text reads.
    paste(image, gradient(size, [(0, rgba(INK, 150)), (0.42, rgba(INK, 0)), (1, rgba(INK, 0))], angle=0))
    return image.resize((W, H), Image.LANCZOS).convert("RGB")


def launch_background(scale=1, width=3840, height=2160, wordmark=True):
    """pic1 and the app's own splash: the mark in the middle of a burst."""
    W, H = width, height
    S = 2
    size = (W * S, H * S)
    w, h = size
    image = night_sky(size, 11, glow=False)
    paste(image, radial(size, (w / 2, h * 0.44), w * 0.42, VIOLET, 150, 1.4))
    paste(image, radial(size, (w / 2, h * 0.44), w * 0.2, CORAL, 90, 1.2))
    rings = layer(size)
    d = ImageDraw.Draw(rings)
    for i, r in enumerate((0.16, 0.24, 0.33, 0.44)):
        rr = h * r * 1.2
        d.ellipse((w / 2 - rr, h * 0.44 - rr, w / 2 + rr, h * 0.44 + rr), outline=rgba(CREAM, 40 - i * 7),
                  width=int(w * 0.0016))
    paste(image, rings)
    d = ImageDraw.Draw(image)
    # Planes circling the mark on dashed orbits.
    orbit = [(w / 2 + math.cos(t) * h * 0.4, h * 0.44 + math.sin(t) * h * 0.28)
             for t in np.linspace(math.pi * 0.95, math.pi * 1.9, 7)]
    dashed_curve(d, orbit, w * 0.0035, rgba(CREAM, 140))
    orbit2 = [(w / 2 + math.cos(t) * h * 0.58, h * 0.44 + math.sin(t) * h * 0.42)
              for t in np.linspace(math.pi * -0.05, math.pi * 0.8, 7)]
    dashed_curve(d, orbit2, w * 0.003, rgba(AQUA, 120))
    planes = layer(size)
    pd = ImageDraw.Draw(planes)
    plane(pd, orbit[-1][0], orbit[-1][1], w * 0.03, 30, fold=(255, 226, 150), shade=SUN)
    plane(pd, orbit2[-1][0], orbit2[-1][1], w * 0.022, 140, body=AQUA, fold=(40, 180, 190),
          shade=(30, 140, 160))
    paste(image, shadow_of(planes, (w * 0.002, w * 0.004), w * 0.003, 90))
    paste(image, planes)
    logo = mark(int(h * 0.34), background=True)
    paste(image, place(shadow_of(logo, (0, h * 0.02), h * 0.025, 140), size, w / 2, h * 0.42))
    paste(image, place(logo, size, w / 2, h * 0.42))
    if wordmark:
        d = ImageDraw.Draw(image)
        title_font = font("Montserrat-Medium.ttf", h * 0.09)
        text = "PShare"
        left = d.textlength("P", font=title_font)
        total = d.textlength(text, font=title_font)
        x = w / 2 - total / 2
        y = h * 0.64
        d.text((x, y), "P", font=title_font, fill=rgba(CREAM))
        d.text((x + left, y), "Share", font=title_font, fill=rgba(CORAL))
        sub = font("Inter-SemiBold.ttf", h * 0.026)
        line = "Your captures, sent the moment you take them"
        d.text((w / 2 - d.textlength(line, font=sub) / 2, h * 0.77), line, font=sub,
               fill=rgba((196, 188, 240)))
    return image.resize((W, H), Image.LANCZOS).convert("RGB")


def readme_banner(width=1600, height=480):
    """docs/banner.png: the mark and name on the left, captures flying off on the right."""
    S = 2
    w, h = width * S, height * S
    size = (w, h)
    image = night_sky(size, 23, glow=False, stars_count=220, sparkles=26)
    paste(image, radial(size, (w * 0.18, h * 0.5), w * 0.32, VIOLET, 150, 1.3))
    paste(image, radial(size, (w * 0.78, h * 0.45), w * 0.3, PLUM, 120, 1.3))
    paste(image, radial(size, (w * 0.95, h * 1.0), w * 0.25, CORAL, 80, 1.2))
    d = ImageDraw.Draw(image)

    # Right: three captures, each sending a plane off along a dashed trail.
    cards = [((w * 0.775, h * 0.6), (int(h * 0.5), int(h * 0.39)), -10, (AQUA, VIOLET), (CORAL, PLUM)),
             ((w * 0.905, h * 0.3), (int(h * 0.34), int(h * 0.27)), 8, ((255, 160, 120), PLUM),
              (SUN, CORAL)),
             ((w * 0.93, h * 0.76), (int(h * 0.28), int(h * 0.22)), -4, ((120, 230, 170), (40, 120, 140)),
              (AQUA, VIOLET))]
    trails = layer(size)
    td = ImageDraw.Draw(trails)
    planes = layer(size)
    pd = ImageDraw.Draw(planes)
    for (cx, cy), card_size, angle, sky, hills in cards:
        start = (cx + card_size[0] * 0.25, cy - card_size[1] * 0.5)
        mid = (start[0] + h * 0.07, start[1] - h * 0.12)
        end = (start[0] + h * 0.2, start[1] - h * 0.15)
        dashed_curve(td, [start, mid, end], w * 0.0022, rgba(CREAM, 170), dash=2.2, gap=1.8)
        plane(pd, end[0], end[1], h * 0.05, -32, body=rgba(WHITE), fold=rgba((255, 226, 150)),
              shade=rgba(SUN))
    for (cx, cy), card_size, angle, sky, hills in cards:
        card = capture_card(card_size, angle=angle, sky_top=sky[0], sky_bottom=sky[1], hills=hills)
        paste(image, place(shadow_of(card, (h * 0.01, h * 0.03), h * 0.025, 130), size, cx, cy))
        paste(image, place(card, size, cx, cy))
    paste(image, trails)
    paste(image, shadow_of(planes, (w * 0.002, w * 0.004), w * 0.003, 90))
    paste(image, planes)

    # Left: the mark, the name, what it does.
    logo = mark(int(h * 0.42))
    lx, ly = w * 0.1, h * 0.5
    paste(image, place(shadow_of(logo, (0, h * 0.02), h * 0.03, 140), size, lx, ly))
    paste(image, place(logo, size, lx, ly))
    d = ImageDraw.Draw(image)
    title_font = font("Montserrat-Medium.ttf", h * 0.24)
    x = lx + h * 0.3
    y = h * 0.16
    d.text((x, y), "P", font=title_font, fill=rgba(CREAM))
    d.text((x + d.textlength("P", font=title_font), y), "Share", font=title_font, fill=rgba(CORAL))
    sub = font("Inter-SemiBold.ttf", h * 0.056)
    d.text((x + h * 0.01, h * 0.47), "Your PS5 captures, sent the moment you take them", font=sub,
           fill=rgba((214, 206, 250)))
    # Where they go, as small pills.
    pill_font = font("Inter-SemiBold.ttf", h * 0.04)
    px, py = x + h * 0.01, h * 0.64
    for label, color in (("Discord", (124, 140, 255)), ("Telegram", AQUA), ("catbox", SUN),
                         ("USB & PC share", (127, 224, 168)), ("Clip player", CORAL)):
        tw = d.textlength(label, font=pill_font)
        pw, ph = tw + h * 0.1, h * 0.085
        d.rounded_rectangle((px, py, px + pw, py + ph), ph / 2, fill=rgba(NIGHT_2, 235),
                            outline=rgba(color, 220), width=int(S * 2))
        r = h * 0.014
        d.ellipse((px + h * 0.035 - r, py + ph / 2 - r, px + h * 0.035 + r, py + ph / 2 + r),
                  fill=rgba(color))
        d.text((px + h * 0.065, py + ph / 2), label, font=pill_font, fill=rgba(CREAM), anchor="lm")
        px += pw + h * 0.025
    return image.resize((width, height), Image.LANCZOS).convert("RGB")


def app_backdrop():
    """Behind the app's screens: quiet, so text on top always reads."""
    W, H = 1920, 1080
    S = 2
    size = (W * S, H * S)
    w, h = size
    image = night_sky(size, 3, stars_count=60, sparkles=0, dots_on=False)
    d = ImageDraw.Draw(image)
    dashed_curve(d, [(w * 0.62, h * 1.02), (w * 0.78, h * 0.78), (w * 0.9, h * 0.74), (w * 1.02, h * 0.5)],
                 w * 0.0026, rgba(CREAM, 60))
    dashed_curve(d, [(w * -0.02, h * 0.3), (w * 0.08, h * 0.12), (w * 0.2, h * -0.02)],
                 w * 0.0022, rgba(AQUA, 50))
    planes = layer(size)
    plane(ImageDraw.Draw(planes), w * 1.0, h * 0.5, w * 0.018, -40, body=rgba(CREAM, 90),
          fold=rgba(CREAM, 60), shade=rgba(CREAM, 40))
    paste(image, planes)
    paste(image, Image.new("RGBA", size, rgba(INK, 115)))
    return image.resize((W, H), Image.LANCZOS).convert("RGB")


def empty_illustration():
    """For "no captures yet": an empty frame and a resting plane."""
    W, H = 640, 440
    S = 4
    size = (W * S, H * S)
    w, h = size
    image = layer(size)
    paste(image, radial(size, (w / 2, h * 0.55), w * 0.42, VIOLET, 90))
    card = capture_card((int(w * 0.5), int(h * 0.56)), angle=-6, sky_top=(70, 60, 140),
                        sky_bottom=(48, 40, 110), sun=(120, 110, 190), hills=((90, 80, 170), (64, 56, 140)),
                        frame=(222, 214, 246))
    paste(image, place(shadow_of(card, (0, h * 0.02), h * 0.02, 110), size, w * 0.46, h * 0.52))
    paste(image, place(card, size, w * 0.46, h * 0.52))
    d = ImageDraw.Draw(image)
    dashed_curve(d, [(w * 0.62, h * 0.86), (w * 0.74, h * 0.78), (w * 0.8, h * 0.84)], w * 0.008,
                 rgba(CREAM, 120))
    planes = layer(size)
    plane(ImageDraw.Draw(planes), w * 0.86, h * 0.84, w * 0.07, 8, fold=(255, 226, 150), shade=SUN)
    paste(image, shadow_of(planes, (0, h * 0.012), h * 0.01, 90))
    paste(image, planes)
    for x, y, r in ((0.2, 0.2, 0.025), (0.78, 0.16, 0.02), (0.14, 0.7, 0.016)):
        sparkle(d, w * x, h * y, w * r, rgba(SUN, 200))
    # "z z" over the plane: it is asleep.
    zf = font("Montserrat-Medium.ttf", h * 0.08)
    d.text((w * 0.88, h * 0.62), "z", font=zf, fill=rgba(CREAM, 200))
    d.text((w * 0.93, h * 0.54), "z", font=font("Montserrat-Medium.ttf", h * 0.06), fill=rgba(CREAM, 150))
    return image.resize((W, H), Image.LANCZOS)


# ---- DDS ----------------------------------------------------------------


def write_bc7_dds(image, path):
    """A 3840x2160 BC7_UNORM DX10 DDS without mipmaps (what the shell reads)."""
    import etcpak  # pip install etcpak

    rgba_bytes = image.convert("RGBA").tobytes()
    width, height = image.size
    blocks = etcpak.compress_bc7(rgba_bytes, width, height)
    header = bytearray(148)
    header[0:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000  # caps, height, width, pixelformat, linearsize
    struct.pack_into("<IIIII", header, 8, flags, height, width, len(blocks), 0)
    struct.pack_into("<I", header, 28, 1)  # mip count
    struct.pack_into("<I", header, 76, 32)  # pixel format size
    struct.pack_into("<I", header, 80, 0x4)  # DDPF_FOURCC
    header[84:88] = b"DX10"
    struct.pack_into("<I", header, 108, 0x1000)  # DDSCAPS_TEXTURE
    struct.pack_into("<IIIII", header, 128, 98, 3, 0, 1, 0)  # BC7_UNORM, 2D, array 1
    path.write_bytes(bytes(header) + blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--no-dds", action="store_true", help="skip the BC7 DDS files")
    parser.add_argument("--only", choices=("icon", "backgrounds", "app", "banner"))
    args = parser.parse_args()
    sce_sys = ROOT / "sce_sys"
    art = ROOT / "assets/art"
    art.mkdir(parents=True, exist_ok=True)

    if args.only in (None, "icon"):
        # The shell rounds the tile itself: the icon fills its square.
        icon = mark(512, rounded=False)
        flat = Image.new("RGB", icon.size, NIGHT)
        flat.paste(icon, (0, 0), icon)
        flat.save(sce_sys / "icon0.png", optimize=True)
        mark(512).save(art / "logo.png", optimize=True)
        print("icon0.png, logo.png")
    if args.only in (None, "backgrounds"):
        selection = selection_background()
        selection.save(sce_sys / "background-source.png", optimize=True)
        launch = launch_background()
        launch.save(sce_sys / "launch-background-source.png", optimize=True)
        print("background-source.png, launch-background-source.png")
        if not args.no_dds:
            try:
                write_bc7_dds(selection, sce_sys / "pic0.dds")
                write_bc7_dds(launch, sce_sys / "pic1.dds")
                print("pic0.dds, pic1.dds")
            except ImportError:
                print("etcpak is missing (pip install etcpak): the DDS files were not rebuilt")
    if args.only in (None, "app"):
        app_backdrop().save(art / "backdrop.jpg", quality=90)
        launch_background(width=1920, height=1080, wordmark=False).save(art / "splash.jpg", quality=92)
        empty_illustration().save(art / "empty.png", optimize=True)
        print("backdrop.jpg, splash.jpg, empty.png")
    if args.only in (None, "banner"):
        (ROOT / "docs").mkdir(exist_ok=True)
        readme_banner().save(ROOT / "docs/banner.png", optimize=True)
        print("docs/banner.png")


if __name__ == "__main__":
    main()
