#!/usr/bin/env python3
"""Renders src/app.ico and docs/icon.png: two concentric progress rings — the outer one the session, the
inner one the week — on a dark slate tile, the same idea as the tray icon. Requires Pillow.

    python3 tools/make_icon.py
"""
import math

from PIL import Image, ImageDraw

SS = 8  # supersampling for clean edges at every size


def ring(d, cx, cy, r, width, track, color, fraction):
    box = [cx - r, cy - r, cx + r, cy + r]
    d.ellipse(box, outline=track, width=int(width))
    start = -90
    end = start + 360 * fraction
    d.arc(box, start=start, end=end, fill=color, width=int(width))
    # Round caps: the arc's ends sit on the stroke's centre line.
    for a in (start, end):
        x = cx + math.cos(math.radians(a)) * (r - width / 2)
        y = cy + math.sin(math.radians(a)) * (r - width / 2)
        d.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=color)


def render(size):
    px = size * SS
    img = Image.new("RGBA", (px, px), (0, 0, 0, 0))

    # Tile: a vertical slate gradient in a rounded square that fills the canvas (small sizes need every pixel).
    inset = px * (0.02 if size <= 24 else 0.04)
    top, bottom = (44, 50, 72), (20, 23, 36)
    grad = Image.new("RGBA", (px, px))
    gd = ImageDraw.Draw(grad)
    for y in range(px):
        t = y / (px - 1)
        gd.line([(0, y), (px, y)], fill=tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).rounded_rectangle([inset, inset, px - inset, px - inset], radius=px * 0.22, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    c = px / 2
    # Thicker strokes as the icon shrinks, so the two rings stay apart and legible at 16 px.
    w = px * (0.13 if size <= 24 else 0.105)
    gap = px * (0.035 if size <= 24 else 0.045)
    outer = px * 0.40
    inner = outer - w - gap
    # Opaque: ImageDraw writes RGBA straight into the tile rather than blending, so a translucent track would
    # punch a hole in it.
    track = (66, 72, 96, 255)
    ring(d, c, c, outer, w, track, (52, 211, 120, 255), 0.72)   # session: green, on pace
    ring(d, c, c, inner, w, track, (255, 170, 40, 255), 0.45)   # week: amber
    if size >= 32:
        dot = px * 0.055
        d.ellipse([c - dot, c - dot, c + dot, c + dot], fill=(255, 255, 255, 230))
    return img.resize((size, size), Image.LANCZOS)


if __name__ == "__main__":
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = [render(n) for n in sizes]
    frames[-1].save("src/app.ico", format="ICO", sizes=[(n, n) for n in sizes], append_images=frames[:-1])
    render(512).resize((256, 256), Image.LANCZOS).save("docs/icon.png")
    # A contact sheet of the small sizes, for checking legibility.
    sheet = Image.new("RGBA", (sum(sizes[:6]) + 70, 72), (32, 32, 32, 255))
    x = 10
    for f in frames[:6]:
        sheet.paste(f, (x, 10), f)
        x += f.width + 10
    sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save("/tmp/claude-0/-home-claudeuser-pacer/c323340d-2591-4f96-8b35-e2bea9c774a1/scratchpad/sheet.png")
