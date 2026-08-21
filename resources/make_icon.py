#!/usr/bin/env python3
"""Generates the LuxTrace application icon.

The icon is a converging ray bundle through a biconvex lens: three or five amber
rays enter from the left, refract at both glass surfaces and meet at a bright
focus on the right. That is literally what the app does, and the silhouette -- a
lens plus a converging wedge -- still reads at 16 pixels.

This script is the source of truth rather than a checked-in binary, so the icon
can be re-cut at any size and the geometry stays a set of numbers instead of a
blob. It emits, from the same constants:

    luxtrace.svg          the vector master, for editing or print
    luxtrace_<n>.png      raster sizes for the Qt resource
    luxtrace.ico          the multi-size Windows icon

Run:  python resources/make_icon.py
"""

import math
import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- geometry --
# Everything is in a normalised 0..1 square with y pointing down, which is what
# both PIL and SVG use, so no coordinate flip is needed anywhere below.

BADGE_INSET  = 0.035
BADGE_RADIUS = 0.225

LENS_AXIS_X  = 0.40    # x of the lens edges, where the two surfaces meet
LENS_HALF_H  = 0.34    # half height of the lens
LENS_BULGE   = 0.10    # how far each surface bulges from the edge plane
FOCUS        = (0.875, 0.50)
RAY_START_X  = 0.105

# Radius of a circular arc that spans 2*LENS_HALF_H and bulges LENS_BULGE.
LENS_R  = (LENS_HALF_H ** 2 + LENS_BULGE ** 2) / (2.0 * LENS_BULGE)
FRONT_CX = LENS_AXIS_X - LENS_BULGE + LENS_R    # centre of the left surface
BACK_CX  = LENS_AXIS_X + LENS_BULGE - LENS_R    # centre of the right surface

# How much the glass bends a ray toward the axis, as a fraction of its height.
# Not Snell's law -- it is a drawing -- but it has to lean the right way.
BEND = 0.90

RAYS_DETAIL = [0.235, 0.3675, 0.50, 0.6325, 0.765]
RAYS_SIMPLE = [0.300, 0.50, 0.700]

# ------------------------------------------------------------------ colour --

BG_TOP     = (0x16, 0x19, 0x23)
BG_BOTTOM  = (0x05, 0x06, 0x0A)
RIM        = (0x39, 0x41, 0x55)
GLASS_FILL = (0x3E, 0x8F, 0xD0)
GLASS_EDGE = (0xB4, 0xE6, 0xFF)
RAY_COLD   = (0xFF, 0xA6, 0x33)   # before the lens
RAY_HOT    = (0xFF, 0xEE, 0xC4)   # converging on the focus
GLOW       = (0xFF, 0xD9, 0x8A)


def front_x(y):
    """x of the left (front) lens surface at height y."""
    dy = y - 0.5
    return FRONT_CX - math.sqrt(max(0.0, LENS_R ** 2 - dy * dy))


def back_x(y):
    """x of the right (back) lens surface at height y."""
    dy = y - 0.5
    return BACK_CX + math.sqrt(max(0.0, LENS_R ** 2 - dy * dy))


def ray_path(y0):
    """The four points of one ray: in, front surface, back surface, focus."""
    y1 = 0.5 + (y0 - 0.5) * BEND
    return [(RAY_START_X, y0), (front_x(y0), y0), (back_x(y1), y1), FOCUS]


def taper(a, b, width_a, width_b):
    """A quadrilateral from `a` to `b`, `width_a` wide at one end and `width_b`
    at the other. PIL cannot stroke a line with a varying width, so a converging
    ray has to be a polygon."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dx, dy)
    if length < 1e-9:
        return [a, a, b, b]
    nx, ny = -dy / length, dx / length          # unit normal
    ha, hb = width_a * 0.5, width_b * 0.5
    return [(a[0] + nx * ha, a[1] + ny * ha), (b[0] + nx * hb, b[1] + ny * hb),
            (b[0] - nx * hb, b[1] - ny * hb), (a[0] - nx * ha, a[1] - ny * ha)]


def lens_polygon(steps=160):
    """The lens outline, sampled from both arcs."""
    top, bottom = 0.5 - LENS_HALF_H, 0.5 + LENS_HALF_H
    pts = []
    for i in range(steps + 1):                       # down the front surface
        y = top + (bottom - top) * i / steps
        pts.append((front_x(y), y))
    for i in range(steps + 1):                       # back up the rear surface
        y = bottom + (top - bottom) * i / steps
        pts.append((back_x(y), y))
    return pts


# ------------------------------------------------------------------ raster --

def draw_master(size, rays, detailed):
    """Renders the icon at `size` pixels. Supersampled by the caller."""
    s = float(size)

    def P(pt):
        return (pt[0] * s, pt[1] * s)

    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))

    # Background: a vertical gradient, painted row by row and then masked to the
    # rounded square so the corners come out clean.
    grad = Image.new("RGBA", (size, size))
    gd = ImageDraw.Draw(grad)
    for y in range(size):
        t = y / max(1, size - 1)
        gd.line([(0, y), (size, y)],
                fill=tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)) + (255,))

    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [BADGE_INSET * s, BADGE_INSET * s, (1 - BADGE_INSET) * s, (1 - BADGE_INSET) * s],
        radius=BADGE_RADIUS * s, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img, "RGBA")

    # The focus glow, drawn under everything else so the rays land on top of it.
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    r = 0.085 * s
    gdraw.ellipse([P(FOCUS)[0] - r, P(FOCUS)[1] - r, P(FOCUS)[0] + r, P(FOCUS)[1] + r],
                  fill=GLOW + (215,))
    glow = glow.filter(ImageFilter.GaussianBlur(0.055 * s))
    img.alpha_composite(glow)

    # The lens: translucent glass with a bright rim, so it reads as a solid at
    # any size rather than as an outline that disappears when downsampled.
    poly = [P(p) for p in lens_polygon()]
    d.polygon(poly, fill=GLASS_FILL + (120,))
    d.line(poly + [poly[0]], fill=GLASS_EDGE + (235,), width=max(1, int(0.016 * s)))

    # Rays. The segment inside the glass is drawn at reduced alpha so the lens
    # still reads as glass in front of it, and the segment after it tapers toward
    # the focus -- a line of constant width converging on a point looks like a
    # fan, and the taper is what makes it look like light coming together.
    width = (0.028 if detailed else 0.050) * s
    for y0 in rays:
        pts = [P(p) for p in ray_path(y0)]
        d.line(pts[0:2], fill=RAY_COLD + (255,), width=max(1, int(round(width))))
        d.line(pts[1:3], fill=RAY_COLD + (150,), width=max(1, int(round(width))))
        d.polygon(taper(pts[2], pts[3], width, width * 0.22), fill=RAY_HOT + (255,))
        # Round off the two kinks at the glass surfaces.
        for pt, alpha in ((pts[1], 255), (pts[2], 200)):
            rr = width * 0.5
            d.ellipse([pt[0] - rr, pt[1] - rr, pt[0] + rr, pt[1] + rr],
                      fill=RAY_COLD + (alpha,))

    # A hot core at the focus, on top of the rays.
    core = 0.030 * s if detailed else 0.042 * s
    d.ellipse([P(FOCUS)[0] - core, P(FOCUS)[1] - core,
               P(FOCUS)[0] + core, P(FOCUS)[1] + core], fill=(255, 255, 250, 255))

    # Inner rim last, so nothing overlaps the badge edge.
    d.rounded_rectangle(
        [BADGE_INSET * s, BADGE_INSET * s, (1 - BADGE_INSET) * s, (1 - BADGE_INSET) * s],
        radius=BADGE_RADIUS * s, outline=RIM + (255,), width=max(1, int(0.011 * s)))

    return img


def render(size, detailed):
    """One icon size, supersampled 8x and resampled down for antialiasing."""
    ss = min(2048, size * 8)
    master = draw_master(ss, RAYS_DETAIL if detailed else RAYS_SIMPLE, detailed)
    return master.resize((size, size), Image.LANCZOS)


# --------------------------------------------------------------------- SVG --

def svg():
    """The vector master, from the same constants as the raster."""
    def f(v):
        return f"{v:.4f}"

    top, bottom = 0.5 - LENS_HALF_H, 0.5 + LENS_HALF_H
    lens = (f"M {f(LENS_AXIS_X)} {f(top)} "
            f"A {f(LENS_R)} {f(LENS_R)} 0 0 0 {f(LENS_AXIS_X)} {f(bottom)} "
            f"A {f(LENS_R)} {f(LENS_R)} 0 0 0 {f(LENS_AXIS_X)} {f(top)} Z")

    def hex_of(c):
        return "#%02X%02X%02X" % c

    rays = []
    for y0 in RAYS_DETAIL:
        p = ray_path(y0)
        rays.append(
            f'    <polyline points="{f(p[0][0])},{f(p[0][1])} {f(p[1][0])},{f(p[1][1])} '
            f'{f(p[2][0])},{f(p[2][1])}" stroke="{hex_of(RAY_COLD)}" stroke-width="0.028" '
            f'fill="none" stroke-linecap="round" stroke-linejoin="round"/>')
        rays.append(
            f'    <line x1="{f(p[2][0])}" y1="{f(p[2][1])}" x2="{f(p[3][0])}" y2="{f(p[3][1])}" '
            f'stroke="{hex_of(RAY_HOT)}" stroke-width="0.028" stroke-linecap="round"/>')

    inset, side = BADGE_INSET, 1 - 2 * BADGE_INSET
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!-- LuxTrace application icon. Generated by resources/make_icon.py -- edit the
     constants there rather than this file, so the SVG, the PNGs and the ICO
     cannot drift apart. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1 1" width="512" height="512">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{hex_of(BG_TOP)}"/>
      <stop offset="1" stop-color="{hex_of(BG_BOTTOM)}"/>
    </linearGradient>
    <radialGradient id="glow">
      <stop offset="0" stop-color="{hex_of(GLOW)}" stop-opacity="0.85"/>
      <stop offset="1" stop-color="{hex_of(GLOW)}" stop-opacity="0"/>
    </radialGradient>
    <clipPath id="badge">
      <rect x="{f(inset)}" y="{f(inset)}" width="{f(side)}" height="{f(side)}"
            rx="{f(BADGE_RADIUS)}" ry="{f(BADGE_RADIUS)}"/>
    </clipPath>
  </defs>

  <g clip-path="url(#badge)">
    <rect x="0" y="0" width="1" height="1" fill="url(#bg)"/>
    <circle cx="{f(FOCUS[0])}" cy="{f(FOCUS[1])}" r="0.20" fill="url(#glow)"/>
    <path d="{lens}" fill="{hex_of(GLASS_FILL)}" fill-opacity="0.47"
          stroke="{hex_of(GLASS_EDGE)}" stroke-width="0.016" stroke-opacity="0.92"/>
{chr(10).join(rays)}
    <circle cx="{f(FOCUS[0])}" cy="{f(FOCUS[1])}" r="0.030" fill="#FFFFFA"/>
  </g>
  <rect x="{f(inset)}" y="{f(inset)}" width="{f(side)}" height="{f(side)}"
        rx="{f(BADGE_RADIUS)}" ry="{f(BADGE_RADIUS)}"
        fill="none" stroke="{hex_of(RIM)}" stroke-width="0.011"/>
</svg>
"""


# -------------------------------------------------------------------- main --

def main():
    # Small sizes get the simplified three-ray version: five rays at 16 pixels
    # is a smear, and simplifying below a threshold is what every real icon set
    # does rather than downsampling one master all the way.
    sizes = [(16, False), (24, False), (32, False),
             (48, True), (64, True), (128, True), (256, True)]

    images = []
    for size, detailed in sizes:
        img = render(size, detailed)
        images.append(img)
        if size in (32, 64, 128, 256):
            path = os.path.join(HERE, f"luxtrace_{size}.png")
            img.save(path)
            print("wrote", os.path.relpath(path, os.path.dirname(HERE)))

    ico = os.path.join(HERE, "luxtrace.ico")
    images[-1].save(ico, format="ICO",
                    sizes=[(s, s) for s, _ in sizes])
    print("wrote", os.path.relpath(ico, os.path.dirname(HERE)))

    svg_path = os.path.join(HERE, "luxtrace.svg")
    with open(svg_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(svg())
    print("wrote", os.path.relpath(svg_path, os.path.dirname(HERE)))


if __name__ == "__main__":
    main()
