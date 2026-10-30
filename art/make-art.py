#!/usr/bin/env python3
"""Playser artwork: the app icon and the Steam library images.

Writes SVGs next to this script and renders PNGs with resvg, using the
app's own Inter font:   python3 art/make-art.py
"""
import math
import pathlib
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
FONT = HERE.parent / "app/data/fonts/InterVariable.ttf"

BG = "#0b0f17"      # Theme.background
BG2 = "#18223a"
ACCENT = "#f5b942"  # Theme.accent
LASER = "#ff5a3c"


def defs(p, r):
    """Gradients and filters for a disc drawn with id prefix p."""
    # iridescent sheen: thin translucent wedges in rainbow hues, as on a
    # real disc's data side
    blur = r * 0.12
    return f"""
  <clipPath id="{p}clip"><circle cx="0" cy="0" r="{r}"/></clipPath>
  <radialGradient id="{p}body" cx="0.42" cy="0.38" r="0.75">
    <stop offset="0" stop-color="#5a6378"/><stop offset="0.6" stop-color="#2a3142"/>
    <stop offset="1" stop-color="#1a1f2c"/>
  </radialGradient>
  <linearGradient id="{p}rim" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#e9eef8"/><stop offset="0.5" stop-color="#6c7690"/>
    <stop offset="1" stop-color="#cfd6e4"/>
  </linearGradient>
  <linearGradient id="{p}tri" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#ffd27a"/><stop offset="1" stop-color="{ACCENT}"/>
  </linearGradient>
  <radialGradient id="{p}spot">
    <stop offset="0" stop-color="#ffffff"/><stop offset="0.25" stop-color="#ffd27a"/>
    <stop offset="0.6" stop-color="{LASER}" stop-opacity="0.55"/>
    <stop offset="1" stop-color="{LASER}" stop-opacity="0"/>
  </radialGradient>
  <filter id="{p}glow" x="-50%" y="-50%" width="200%" height="200%">
    <feGaussianBlur stdDeviation="3"/>
  </filter>
  <filter id="{p}sheen" x="-10%" y="-10%" width="120%" height="120%">
    <feGaussianBlur stdDeviation="{blur:.1f}"/>
  </filter>
  <filter id="{p}soft" x="-50%" y="-50%" width="200%" height="200%">
    <feGaussianBlur stdDeviation="14"/>
  </filter>"""


def disc(p, cx, cy, r, laser=True):
    """A disc with a play-triangle hub and a laser reading it."""
    hues = ["#ff4d6d", "#ffb347", "#f9f871", "#4dffb8", "#4dc3ff", "#a77bff"]
    sheen = []
    for i, h in enumerate(hues * 2):
        a0 = math.radians(-150 + i * 30)
        a1 = a0 + math.radians(22)
        x0, y0 = cx + r * math.cos(a0), cy + r * math.sin(a0)
        x1, y1 = cx + r * math.cos(a1), cy + r * math.sin(a1)
        sheen.append(f'<path d="M{cx},{cy} L{x0:.1f},{y0:.1f} A{r},{r} 0 0 1 {x1:.1f},{y1:.1f} Z" '
                     f'fill="{h}"/>')
    grooves = "".join(
        f'<circle cx="{cx}" cy="{cy}" r="{r * f:.1f}" fill="none" stroke="#ffffff" '
        f'stroke-opacity="0.05" stroke-width="{r * 0.004:.2f}"/>'
        for f in (0.5, 0.58, 0.66, 0.74, 0.82, 0.9))
    hub = r * 0.30
    # play triangle, centred on its centroid, corners rounded by a stroke
    t = r * 0.24
    tri = (f"M{cx - t * 0.55:.1f},{cy - t:.1f} L{cx + t * 1.05:.1f},{cy:.1f} "
           f"L{cx - t * 0.55:.1f},{cy + t:.1f} Z")
    out = [f'<g>',
           f'<circle cx="{cx}" cy="{cy + r * 0.04}" r="{r}" fill="#000" opacity="0.45" filter="url(#{p}soft)"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="url(#{p}body)"/>',
           f'<g clip-path="url(#{p}clip)" transform="translate({cx},{cy})">'
           f'<g opacity="0.30" filter="url(#{p}sheen)" transform="translate({-cx},{-cy})">',
           *sheen, '</g></g>', grooves,
           f'<circle cx="{cx}" cy="{cy}" r="{r - r * 0.012:.1f}" fill="none" stroke="url(#{p}rim)" '
           f'stroke-width="{r * 0.025:.1f}"/>',
           # clear inner ring and hub
           f'<circle cx="{cx}" cy="{cy}" r="{hub * 1.35:.1f}" fill="#0d121c" opacity="0.55"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{hub:.1f}" fill="#0b0f17"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{hub:.1f}" fill="none" stroke="#8a93a8" '
           f'stroke-opacity="0.5" stroke-width="{r * 0.012:.1f}"/>',
           f'<path d="{tri}" fill="url(#{p}tri)" stroke="url(#{p}tri)" stroke-width="{r * 0.06:.1f}" '
           f'stroke-linejoin="round"/>']
    if laser:
        # the beam comes in from the lower left and meets the disc at a
        # bright read spot
        a = math.radians(140)
        sx, sy = cx + r * 0.66 * math.cos(a), cy + r * 0.66 * math.sin(a)
        ex, ey = cx - r * 1.25, cy + r * 1.2
        w = r * 0.035
        out += [f'<line x1="{ex:.1f}" y1="{ey:.1f}" x2="{sx:.1f}" y2="{sy:.1f}" stroke="{LASER}" '
                f'stroke-width="{w * 3:.1f}" stroke-linecap="round" opacity="0.5" filter="url(#{p}glow)"/>',
                f'<line x1="{ex:.1f}" y1="{ey:.1f}" x2="{sx:.1f}" y2="{sy:.1f}" stroke="#ffd9c9" '
                f'stroke-width="{w:.1f}" stroke-linecap="round"/>',
                f'<circle cx="{sx:.1f}" cy="{sy:.1f}" r="{r * 0.16:.1f}" fill="url(#{p}spot)"/>']
    out.append("</g>")
    return "\n".join(out)


def svg(w, h, body, extra_defs=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
            f'<defs>{extra_defs}</defs>{body}</svg>\n')


def write(name, text, w=None):
    path = HERE / f"{name}.svg"
    path.write_text(text)
    png = HERE / "out" / f"{name}.png"
    png.parent.mkdir(exist_ok=True)
    cmd = ["resvg", "--use-font-file", str(FONT), str(path), str(png)]
    if w:
        cmd[1:1] = ["-w", str(w)]
    subprocess.run(cmd, check=True)


def wordmark(x, y, size, anchor="start"):
    """ "Playser": Play in white, ser in the accent (laser + player)."""
    return (f'<text x="{x}" y="{y}" font-family="Inter Variable" font-weight="800" '
            f'font-size="{size}" letter-spacing="{-size * 0.02:.1f}" text-anchor="{anchor}" '
            f'fill="#f2f4f8">Play<tspan fill="{ACCENT}">ser</tspan></text>')


def backdrop(w, h, gx, gy, gr):
    """Dark gradient with a warm glow behind the disc."""
    return (f'<radialGradient id="bg" cx="{gx / w:.3f}" cy="{gy / h:.3f}" r="{gr:.3f}">'
            f'<stop offset="0" stop-color="{BG2}"/><stop offset="1" stop-color="{BG}"/></radialGradient>'
            f'<radialGradient id="warm" cx="0.5" cy="0.5" r="0.5">'
            f'<stop offset="0" stop-color="{ACCENT}" stop-opacity="0.18"/>'
            f'<stop offset="1" stop-color="{ACCENT}" stop-opacity="0"/></radialGradient>',
            f'<rect width="{w}" height="{h}" fill="url(#bg)"/>')


def scene(name, w, h, cx, cy, r, text=None):
    bg_defs, bg = backdrop(w, h, cx, cy, 0.8)
    glow = f'<circle cx="{cx}" cy="{cy}" r="{r * 1.6:.0f}" fill="url(#warm)"/>'
    body = bg + glow + disc("d", cx, cy, r) + (text or "")
    write(name, svg(w, h, body, bg_defs + defs("d", r)))


if __name__ == "__main__":
    # icon: the disc alone, laser clipped to the 256 box
    write("icon", svg(256, 256, disc("i", 136, 120, 104), defs("i", 104)))
    # Steam library images (names as Steam's grid folder wants them, minus
    # the shortcut's app ID): portrait capsule, wide capsule, hero, logo
    scene("steam-portrait", 600, 900, 320, 360, 210,
          wordmark(300, 760, 120, "middle")
          + f'<text x="300" y="820" font-family="Inter Variable" font-weight="500" font-size="30" '
            f'text-anchor="middle" fill="#9aa3b5">Disc player for the couch</text>')
    scene("steam-wide", 920, 430, 240, 205, 150, wordmark(440, 248, 118))
    scene("steam-hero", 3840, 1240, 2900, 640, 520)
    write("steam-logo", svg(1280, 400, wordmark(640, 280, 260, "middle")))
