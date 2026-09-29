# tools/BuildToolIcons.py - the map editor TOOL STRIP's icon discs
# (docs/editor-updates-plan.md, P1): assets/ui/icon_tb_tool_<name>.png.
#
# Run:  python tools\BuildToolIcons.py [--montage <png>]
#
# The toolbar's discs are Wenrexa blanks with a glyph composited on; the blank
# art itself is not in the repo, so this DERIVES one: it takes the Play disc,
# measures the dark face's colour at each radius where the glyph is not, and
# refills the whole face from that profile - the bezel, rim and gradient come
# across untouched and the old glyph goes. Each tool's glyph is then drawn at
# 4x in the house glyph colour (sampled from the existing discs) over a soft
# dark drop, and scaled down onto the blank. The icons are DEFINED BY THIS
# SCRIPT (Michael's rule for authored assets): a change is an edit here and a
# re-run, and the PNGs are its output.
#
# Glyphs are authored in the discs' own 84 px space, centred on 41.5, inside
# the face radius (~27).
import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "assets", "ui")
SIZE = 84
C = 41.5            # disc centre
FACE_R = 27.5       # the dark face; the bezel ring starts at ~28
CLEAN_R = 14        # the first radius the Play glyph does not touch
SS = 4              # supersampling for the glyph layer
GLYPH = (166, 162, 102, 255)  # the house glyph colour (icon_tb_play/pause)
SHADOW = (8, 8, 6, 170)


def blank_disc():
    """The Play disc with its glyph removed: the face refilled per radius."""
    im = Image.open(os.path.join(UI, "icon_tb_play.png")).convert("RGBA")
    px = im.load()
    # The face colour at each integer radius, from pixels too dark to be glyph.
    samples = {}
    for y in range(SIZE):
        for x in range(SIZE):
            r = math.hypot(x - C, y - C)
            if r > FACE_R:
                continue
            p = px[x, y]
            if 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2] > 60:  # glyph, not face
                continue
            samples.setdefault(int(r), []).append(p)
    profile = {}
    for r, ps in samples.items():
        # Only radii clear of the Play glyph (it reaches ~12): inside that, the
        # glyph's own dark outline passes the brightness test and poisons the
        # median - the first run of this script put a black spot in the middle.
        if r >= CLEAN_R and len(ps) >= 12:
            ps = sorted(ps, key=lambda p: p[0] + p[1] + p[2])
            profile[r] = ps[len(ps) // 2]
    known = sorted(profile)
    # The middle is extrapolated inward from the slope of the first clean band.
    r0, r1 = known[0], known[min(6, len(known) - 1)]
    for r in range(0, r0):
        t = (r0 - r) / max(1, r1 - r0)
        profile[r] = tuple(
            max(0, min(255, round(profile[r0][i] + (profile[r0][i] - profile[r1][i]) * t)))
            for i in range(3)) + (255,)
    for r in range(int(FACE_R) + 1):
        if r not in profile:  # a gap between clean bands: nearest known
            profile[r] = profile[min(known, key=lambda k: abs(k - r))]
    out = im.copy()
    op = out.load()
    for y in range(SIZE):
        for x in range(SIZE):
            r = math.hypot(x - C, y - C)
            if r <= FACE_R:
                lo = profile[int(r)]
                hi = profile.get(int(r) + 1, lo)
                f = r - int(r)
                op[x, y] = tuple(round(lo[i] * (1 - f) + hi[i] * f) for i in range(3)) + (255,)
    return out


# --- glyphs (84 px space) ----------------------------------------------------
def S(pts):
    return [(x * SS, y * SS) for x, y in pts]


def line(d, a, b, w):
    d.line(S([a, b]), fill=GLYPH, width=int(w * SS))
    for p in (a, b):  # round caps
        r = w * SS / 2
        d.ellipse([p[0] * SS - r, p[1] * SS - r, p[0] * SS + r, p[1] * SS + r], fill=GLYPH)


def glyph_paint(d):
    # A brush, handle up-right, bristles down-left: handle, ferrule, and a
    # broad tuft - thin, it read as a pen.
    line(d, (56, 26), (46, 36), 4.5)
    d.polygon(S([(45, 32), (50, 37), (43, 44), (38, 39)]), fill=GLYPH)       # ferrule
    d.polygon(S([(37, 39), (44, 46), (39, 53), (26, 58), (30, 45)]), fill=GLYPH)  # tuft


def glyph_rect(d):
    # A marquee: the dashed outline of a box.
    x0, y0, x1, y1 = 27, 29, 56, 54
    dash, gap, w = 5.0, 3.0, 3.0

    def dashes(a, b):
        L = math.dist(a, b)
        t = 0.0
        while t < L:
            e = min(L, t + dash)
            p = (a[0] + (b[0] - a[0]) * t / L, a[1] + (b[1] - a[1]) * t / L)
            q = (a[0] + (b[0] - a[0]) * e / L, a[1] + (b[1] - a[1]) * e / L)
            d.line(S([p, q]), fill=GLYPH, width=int(w * SS))
            t = e + gap

    for a, b in (((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)),
                 ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))):
        dashes(a, b)


def glyph_flood(d):
    # A paint bucket tipped right, spilling a drop.
    d.polygon(S([(28, 34), (46, 30), (51, 47), (35, 53)]), fill=GLYPH)  # body
    line(d, (29, 32), (45, 27), 2.5)                                     # rim
    d.arc([30 * SS, 20 * SS, 46 * SS, 36 * SS], 200, 340, fill=GLYPH, width=int(2.5 * SS))
    d.polygon(S([(52, 42), (57, 50), (55, 55), (50, 54), (49, 49)]), fill=GLYPH)  # spill


def glyph_area(d):
    # A floor plan: a filled room, a doorway gap in its right wall, and the
    # corridor leading off - the fill takes the room and stops at the door.
    d.rectangle([26 * SS, 28 * SS, 45 * SS, 55 * SS], fill=GLYPH)       # the room
    d.rectangle([29 * SS, 31 * SS, 42 * SS, 52 * SS], fill=(0, 0, 0, 0))
    d.rectangle([31 * SS, 33 * SS, 40 * SS, 50 * SS], fill=GLYPH)       # its floor, filled
    d.rectangle([45 * SS, 38 * SS, 58 * SS, 39.5 * SS], fill=GLYPH)     # corridor walls
    d.rectangle([45 * SS, 45.5 * SS, 58 * SS, 47 * SS], fill=GLYPH)
    d.rectangle([42 * SS, 39.5 * SS, 45 * SS, 45.5 * SS], fill=(0, 0, 0, 0))  # doorway


def glyph_pick(d):
    # An eyedropper: bulb up-right, glass down to a fine tip down-left.
    d.ellipse([47 * SS, 23 * SS, 58 * SS, 34 * SS], fill=GLYPH)           # bulb
    d.polygon(S([(45, 33), (49, 37), (46, 40), (42, 36)]), fill=GLYPH)    # collar
    line(d, (44, 38), (32, 50), 4.5)                                       # glass
    line(d, (32, 50), (27, 55), 2.0)                                       # tip


def glyph_filllevel(d):
    # Every square: a 3x3 grid of filled cells.
    for j in range(3):
        for i in range(3):
            x = 28 + i * 9.5
            y = 28 + j * 9.5
            d.rectangle([x * SS, y * SS, (x + 7.5) * SS, (y + 7.5) * SS], fill=GLYPH)


GLYPHS = {
    "paint": glyph_paint,
    "rect": glyph_rect,
    "flood": glyph_flood,
    "area": glyph_area,
    "pick": glyph_pick,
    "filllevel": glyph_filllevel,
}


def render(blank, draw):
    layer = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    draw(ImageDraw.Draw(layer))
    glyph = layer.resize((SIZE, SIZE), Image.LANCZOS)
    # A soft drop under the glyph, like the house discs' pressed-in look.
    alpha = glyph.getchannel("A")
    drop = Image.new("RGBA", (SIZE, SIZE), SHADOW[:3] + (0,))
    drop.putalpha(alpha.point(lambda a: a * SHADOW[3] // 255).filter(ImageFilter.GaussianBlur(1.2)))
    out = blank.copy()
    out.alpha_composite(drop, (1, 1))
    out.alpha_composite(glyph)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--montage", help="also write a review sheet of every disc")
    args = ap.parse_args()
    blank = blank_disc()
    made = []
    for name, draw in GLYPHS.items():
        path = os.path.join(UI, f"icon_tb_tool_{name}.png")
        render(blank, draw).save(path)
        made.append(path)
        print("wrote", os.path.relpath(path, ROOT))
    if args.montage:
        sheet = Image.new("RGBA", (SIZE * (len(made) + 1), SIZE), (40, 40, 48, 255))
        sheet.alpha_composite(blank, (0, 0))
        for i, p in enumerate(made):
            sheet.alpha_composite(Image.open(p), ((i + 1) * SIZE, 0))
        sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
        sheet.save(args.montage)
        print("montage", args.montage)


if __name__ == "__main__":
    main()
