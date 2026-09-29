# tools/BuildToolIcons.py - the map editor's script-drawn icon discs
# (docs/editor-updates-plan.md): the TOOL STRIP's (P1,
# assets/ui/icon_tb_tool_<name>.png), the New world disc (P4,
# icon_tb_newworld.png) and the toolbar's right-hand group (Level, Balance,
# Generate, Check, Undo/Redo, Save, To source, Play/Pause).
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
    """The blank disc: icon_tb_blank.png, derived once from the Play disc.

    The derivation reads a Wenrexa disc, and since this script now draws the
    Play disc too, deriving on every run would read its own output. So the
    first run keeps the blank beside the icons and every later run starts from
    that; delete it to derive afresh (from a genuine Wenrexa Play disc)."""
    path = os.path.join(UI, "icon_tb_blank.png")
    if os.path.exists(path):
        return Image.open(path).convert("RGBA")
    blank = derive_blank(os.path.join(UI, "icon_tb_play.png"))
    blank.save(path)
    print("wrote", os.path.relpath(path, ROOT))
    return blank


def derive_blank(src):
    """The Play disc with its glyph removed: the face refilled per radius."""
    im = Image.open(src).convert("RGBA")
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


def glyph_newworld(d):
    # A globe - rim, equator, a meridian - with a plus at its shoulder: a new
    # world, beside the Worlds disc's plain globe.
    cx, cy, r = 38.5, 44.5, 13.0
    w = int(2.6 * SS)
    d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], outline=GLYPH, width=w)
    d.ellipse([(cx - r * 0.45) * SS, (cy - r) * SS, (cx + r * 0.45) * SS, (cy + r) * SS],
              outline=GLYPH, width=w)
    d.line(S([(cx - r, cy), (cx + r, cy)]), fill=GLYPH, width=w)
    px, py = 54.0, 30.0
    d.rectangle([(px - 7) * SS, (py - 2) * SS, (px + 7) * SS, (py + 2) * SS], fill=GLYPH)
    d.rectangle([(px - 2) * SS, (py - 7) * SS, (px + 2) * SS, (py + 7) * SS], fill=GLYPH)


# --- the toolbar's right-hand group ------------------------------------------
# Level settings, Balance, Generate, Check, Undo/Redo, Save, To source and
# Play/Pause. They were Wenrexa discs with stock glyphs (Check a square card
# that matched nothing); drawing them here puts the whole toolbar in one hand.
def glyph_level(d):
    # The level's mood knobs: three slider tracks, their knobs set apart.
    for y, k in ((31.5, 49), (41.5, 35), (51.5, 44)):
        d.rectangle([27 * SS, (y - 1.2) * SS, 56 * SS, (y + 1.2) * SS], fill=GLYPH)
        d.rounded_rectangle([(k - 3) * SS, (y - 4.5) * SS, (k + 3) * SS, (y + 4.5) * SS],
                            radius=1.5 * SS, fill=GLYPH)


def sword(d, tip, pommel):
    # A straight sword from tip to pommel: blade, crossguard, grip, pommel.
    tx, ty = tip
    px, py = pommel
    L = math.dist(tip, pommel)
    ux, uy = (px - tx) / L, (py - ty) / L     # tip -> pommel
    nx, ny = -uy, ux                          # across the blade

    def at(t, s=0.0):
        return (tx + ux * t + nx * s, ty + uy * t + ny * s)

    # Heavy on purpose: at 1.9 half-width the pair read as scratches at 84 px.
    guard = L * 0.64
    d.polygon(S([at(0), at(5, 2.8), at(guard, 2.8), at(guard, -2.8), at(5, -2.8)]), fill=GLYPH)
    line(d, at(guard, 7.5), at(guard, -7.5), 3.8)   # crossguard
    line(d, at(guard), at(L - 2.5), 3.4)            # grip
    r = 3.3
    cx, cy = at(L - 1)
    d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=GLYPH)


def glyph_balance(d):
    # Combat tuning: two swords crossed, points up.
    sword(d, (28, 27), (55, 56))
    sword(d, (55, 27), (28, 56))


def glyph_generate(d):
    # A die showing five: the level generator's roll.
    d.rounded_rectangle([27 * SS, 27 * SS, 56 * SS, 56 * SS], radius=5 * SS,
                        outline=GLYPH, width=int(3 * SS))
    for x, y in ((34.5, 34.5), (48.5, 34.5), (41.5, 41.5), (34.5, 48.5), (48.5, 48.5)):
        r = 2.8
        d.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS], fill=GLYPH)


def glyph_check(d):
    # The checker: a tick.
    line(d, (29, 42), (38, 51.5), 5.5)
    line(d, (38, 51.5), (55, 31), 5.5)


def turn_arrow(d, mirror):
    # A half-turn arrow over the top, head down at the end it turns toward:
    # undo turns back to the left, redo on to the right.
    cx, cy, r, w = 41.5, 45.0, 12.0, 4.0
    box = [(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS]
    if mirror:
        d.arc(box, 160, 360, fill=GLYPH, width=int(w * SS))
        hx = cx + r
    else:
        d.arc(box, 180, 380, fill=GLYPH, width=int(w * SS))
        hx = cx - r
    d.polygon(S([(hx - 6.5, cy - 1), (hx + 6.5, cy - 1), (hx, cy + 8)]), fill=GLYPH)


def glyph_undo(d):
    turn_arrow(d, False)


def glyph_redo(d):
    turn_arrow(d, True)


def glyph_save(d):
    # A floppy disk: chamfered body, the shutter and the label cut out of it.
    d.polygon(S([(27, 27), (50, 27), (56, 33), (56, 56), (27, 56)]), fill=GLYPH)
    clear = (0, 0, 0, 0)
    d.rectangle([33 * SS, 30 * SS, 47 * SS, 38 * SS], fill=clear)   # shutter window
    d.rectangle([43 * SS, 31.5 * SS, 45.5 * SS, 36.5 * SS], fill=GLYPH)  # its slide
    d.rectangle([31 * SS, 43 * SS, 52 * SS, 53 * SS], fill=clear)   # label
    d.rectangle([34 * SS, 46 * SS, 49 * SS, 47.5 * SS], fill=GLYPH)  # label lines
    d.rectangle([34 * SS, 49.5 * SS, 45 * SS, 51 * SS], fill=GLYPH)


def glyph_source(d):
    # To source: an arrow down into a tray - the project copied home.
    line(d, (41.5, 25), (41.5, 42), 4.0)
    d.polygon(S([(33, 38), (50, 38), (41.5, 48)]), fill=GLYPH)
    line(d, (27, 44), (27, 56), 3.5)
    line(d, (27, 56), (56, 56), 3.5)
    line(d, (56, 56), (56, 44), 3.5)


def glyph_play(d):
    # Resume: a play triangle, nudged right so it sits optically centred.
    d.polygon(S([(35, 28.5), (35, 54.5), (56, 41.5)]), fill=GLYPH)


def glyph_pause(d):
    d.rounded_rectangle([31 * SS, 29 * SS, 38.5 * SS, 54 * SS], radius=1.5 * SS, fill=GLYPH)
    d.rounded_rectangle([44.5 * SS, 29 * SS, 52 * SS, 54 * SS], radius=1.5 * SS, fill=GLYPH)


# Output name -> glyph; each writes assets/ui/icon_tb_<name>.png.
GLYPHS = {
    "tool_paint": glyph_paint,
    "tool_rect": glyph_rect,
    "tool_flood": glyph_flood,
    "tool_area": glyph_area,
    "tool_pick": glyph_pick,
    "tool_filllevel": glyph_filllevel,
    "newworld": glyph_newworld,
    "level": glyph_level,
    "balance": glyph_balance,
    "generate": glyph_generate,
    "check": glyph_check,
    "undo": glyph_undo,
    "redo": glyph_redo,
    "save": glyph_save,
    "source": glyph_source,
    "play": glyph_play,
    "pause": glyph_pause,
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
        path = os.path.join(UI, f"icon_tb_{name}.png")
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
