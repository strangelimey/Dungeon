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


# --- dialog footers ---------------------------------------------------------
# Every editor dialog's footer actions are icon discs with a hover name
# (DialogLayout's FooterIcon). Save / new / play / generate / check reuse the
# toolbar's; these are the actions the toolbar had no disc for.
def glyph_help(d):
    # A question mark: a hook, its stem, and the dot.
    w = int(4.5 * SS)
    d.arc([31 * SS, 24 * SS, 52 * SS, 44 * SS], 180, 405, fill=GLYPH, width=w)
    line(d, (45.5, 42), (41.5, 46), 4.5)
    line(d, (41.5, 46), (41.5, 49.5), 4.5)
    r = 3.0
    d.ellipse([(41.5 - r) * SS, (56 - r) * SS, (41.5 + r) * SS, (56 + r) * SS], fill=GLYPH)


def glyph_duplicate(d):
    # Two sheets, the copy offset down-right over the original.
    w = int(3.0 * SS)
    d.rounded_rectangle([27 * SS, 26 * SS, 46 * SS, 47 * SS], radius=2 * SS,
                        outline=GLYPH, width=w)
    d.rounded_rectangle([36 * SS, 35 * SS, 56 * SS, 57 * SS], radius=2 * SS,
                        fill=GLYPH)


def glyph_delete(d):
    # A bin: lid with its handle, and a tapered body scored with three ribs.
    line(d, (28, 31), (55, 31), 3.5)                       # lid
    d.rectangle([37 * SS, 25.5 * SS, 46 * SS, 28 * SS], fill=GLYPH)   # handle
    d.polygon(S([(31, 35), (52, 35), (49.5, 57), (33.5, 57)]), fill=GLYPH)
    clear = (0, 0, 0, 0)
    for x in (36.5, 41.5, 46.5):
        d.rectangle([(x - 1) * SS, 39 * SS, (x + 1) * SS, 53 * SS], fill=clear)


def glyph_anim(d):
    # A film strip: the frame between two perforated edges, a play mark in it.
    d.rectangle([29 * SS, 25 * SS, 54 * SS, 58 * SS], fill=GLYPH)
    clear = (0, 0, 0, 0)
    for y in range(28, 57, 6):
        d.rectangle([31 * SS, y * SS, 34 * SS, (y + 3) * SS], fill=clear)
        d.rectangle([49 * SS, y * SS, 52 * SS, (y + 3) * SS], fill=clear)
    d.rectangle([36.5 * SS, 29 * SS, 46.5 * SS, 54 * SS], fill=clear)   # the frame
    d.polygon(S([(39, 36), (39, 47), (45, 41.5)]), fill=GLYPH)


def glyph_enter(d):
    # Into it: a doorway, and an arrow going in.
    w = 3.5
    line(d, (40, 26), (55, 26), w)
    line(d, (55, 26), (55, 57), w)
    line(d, (55, 57), (40, 57), w)
    line(d, (26, 41.5), (42, 41.5), 4.5)
    d.polygon(S([(39, 33.5), (48.5, 41.5), (39, 49.5)]), fill=GLYPH)


# --- dialog row actions (DialogLayout's RowIcon) -----------------------------
def glyph_clear(d):
    # Clear / remove from a list: a plain cross.
    line(d, (31, 31), (52, 52), 5.0)
    line(d, (52, 31), (31, 52), 5.0)


def arrow_vertical(d, up):
    # A shaft and a broad head: move the row up (or down) the list.
    s = -1 if up else 1
    line(d, (41.5, 41.5 - s * 14), (41.5, 41.5 + s * 6), 5.0)
    tip = 41.5 + s * 16
    base = 41.5 + s * 3
    d.polygon(S([(29, base), (54, base), (41.5, tip)]), fill=GLYPH)


def glyph_up(d):
    arrow_vertical(d, True)


def glyph_down(d):
    arrow_vertical(d, False)


def glyph_open(d):
    # Open / browse / load: a folder, its lid lifted.
    d.polygon(S([(26, 30), (36, 30), (39, 33.5), (54, 33.5), (54, 40), (26, 40)]),
              fill=GLYPH)                                                   # back + tab
    d.polygon(S([(26, 43), (58, 43), (52, 57), (26, 57)]), fill=GLYPH)      # front, tilted


def glyph_route(d):
    # A patrol route: three waypoints joined by a bent path.
    pts = [(28, 53), (41.5, 33), (55, 47)]
    line(d, pts[0], pts[1], 3.0)
    line(d, pts[1], pts[2], 3.0)
    for x, y in pts:
        r = 5.0
        d.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS], fill=GLYPH)


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
    "help": glyph_help,
    "duplicate": glyph_duplicate,
    "delete": glyph_delete,
    "anim": glyph_anim,
    "enter": glyph_enter,
    "clear": glyph_clear,
    "up": glyph_up,
    "down": glyph_down,
    "open": glyph_open,
    "route": glyph_route,
}


# --- the drop-down expander (assets/ui/icon_dropdown.png) -----------------------
# The drop-down's expander box wears the discs' border, SQUARED: every pixel
# keeps its angle but measures its radius with a squircle norm, and samples the
# disc there - so the face, the light inner ring, the dark gap and the top-lit
# outer rim all come across ring for ring, lighting direction included. It is
# TWO files, closed (point down) and open (icon_dropdown_open.png, point up):
# turning one box half a rotation for "open" would also turn the rim's light
# to the bottom, so only the triangle changes (ui::DrawDropDownExpander).
SQUARE_N = 8.0      # squircle exponent: 2 = the disc, higher = squarer corners
SQUARE_SS = 4       # subsamples per axis; the disc is chunky 2x pixel art


def square_blank(blank):
    src = blank.load()
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    op = out.load()
    n = SQUARE_N
    for y in range(SIZE):
        for x in range(SIZE):
            acc = [0.0, 0.0, 0.0, 0.0]
            for j in range(SQUARE_SS):
                for i in range(SQUARE_SS):
                    dx = x + (i + 0.5) / SQUARE_SS - 0.5 - C
                    dy = y + (j + 0.5) / SQUARE_SS - 0.5 - C
                    r = (abs(dx) ** n + abs(dy) ** n) ** (1.0 / n)
                    a = math.atan2(dy, dx)
                    sx = int(round(C + r * math.cos(a)))
                    sy = int(round(C + r * math.sin(a)))
                    if 0 <= sx < SIZE and 0 <= sy < SIZE:
                        p = src[sx, sy]
                        w = p[3] / 255.0  # premultiply, so the edge blends clean
                        for k in range(3):
                            acc[k] += p[k] * w
                        acc[3] += w
            m = SQUARE_SS * SQUARE_SS
            if acc[3] > 0:
                op[x, y] = tuple(round(acc[k] / acc[3]) for k in range(3)) + (round(255 * acc[3] / m),)
    return out


def glyph_dropdown(d):
    # The expander's triangle, point down; its centroid sits near the centre so
    # the open one (its mirror) does not visibly jump when the list opens.
    d.polygon(S([(25.5, 34), (57.5, 34), (41.5, 53)]), fill=GLYPH)


def glyph_dropdown_open(d):
    d.polygon(S([(25.5, 49), (57.5, 49), (41.5, 30)]), fill=GLYPH)


# The map editor's dock collapse buttons wear the same square box
# (icon_tb_dock_left / _right, MapView's drawDockFrame): a double chevron
# pointing the way the dock will move - "<<" folds the palette away, and so on.
def double_chevron(d, left):
    s = -1 if left else 1
    # Heavy on purpose: at 4.6 wide they read as hairlines beside the
    # drop-down's solid triangle at control size.
    for x in (41.5 - 6.5, 41.5 + 6.5):   # two chevrons, centred as a pair
        tip = x + s * 5.5
        back = x - s * 5.5
        line(d, (back, 28.5), (tip, 41.5), 5.8)
        line(d, (tip, 41.5), (back, 54.5), 5.8)


def glyph_dock_left(d):
    double_chevron(d, True)


def glyph_dock_right(d):
    double_chevron(d, False)


# The shared close box (assets/ui/icon_close.png - every dialog's top-right
# corner, and the palette's filter clear) and the palette accordion's
# expand / collapse boxes, all in the same square box.
def glyph_close(d):
    line(d, (31, 31), (52, 52), 5.8)
    line(d, (52, 31), (31, 52), 5.8)


def glyph_box_plus(d):
    line(d, (28.5, 41.5), (54.5, 41.5), 5.8)
    line(d, (41.5, 28.5), (41.5, 54.5), 5.8)


def glyph_box_minus(d):
    line(d, (28.5, 41.5), (54.5, 41.5), 5.8)


# The game UI's single-step arrows in the same box: the character sheet's
# previous / next member, the map's level up / down. One file per direction,
# never one turned - a turn would move the rim's top light round with it.
def chevron(d, dx, dy):
    # A chevron pointing along (dx, dy), centred on the face.
    nx, ny = -dy, dx
    tip = (C + dx * 6.5, C + dy * 6.5)
    for s in (1, -1):
        back = (C - dx * 6.5 + nx * 12 * s, C - dy * 6.5 + ny * 12 * s)
        line(d, back, tip, 5.8)


def glyph_box_left(d):
    chevron(d, -1, 0)


def glyph_box_right(d):
    chevron(d, 1, 0)


def glyph_box_up(d):
    chevron(d, 0, -1)


def glyph_box_down(d):
    chevron(d, 0, 1)


# The player's map toggle, one box per side of it: the dungeon map's way to
# the World (a globe), and the world map's way back to the Dungeon (a barred,
# arched gate). Same corner, same size, so the pair reads as one control.
def glyph_box_world(d):
    # Rim, a meridian, the equator and two latitudes: at 14 with only the
    # equator it read as a crosshair.
    cx, cy, r = 41.5, 41.5, 16.5
    w = int(3.4 * SS)
    d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], outline=GLYPH, width=w)
    d.ellipse([(cx - r * 0.45) * SS, (cy - r) * SS, (cx + r * 0.45) * SS, (cy + r) * SS],
              outline=GLYPH, width=w)
    d.line(S([(cx - r, cy), (cx + r, cy)]), fill=GLYPH, width=w)
    for dy in (-8.5, 8.5):
        half = math.sqrt(r * r - dy * dy) - 1.0
        d.line(S([(cx - half, cy + dy), (cx + half, cy + dy)]), fill=GLYPH, width=int(2.6 * SS))


def glyph_box_dungeon(d):
    # The gate: posts and a round arch over them, three bars and a crossbar.
    x0, x1, top, base = 27.5, 55.5, 38.5, 57.0
    w = 4.0
    r = (x1 - x0) / 2
    cx = (x0 + x1) / 2
    d.arc([x0 * SS, (top - r) * SS, x1 * SS, (top + r) * SS], 180, 360,
          fill=GLYPH, width=int(w * SS))
    line(d, (x0, top), (x0, base), w)
    line(d, (x1, top), (x1, base), w)
    for x in (cx - 6.5, cx, cx + 6.5):
        top_at = top - math.sqrt(max(0.0, r * r - (x - cx) ** 2)) + 1.5
        line(d, (x, top_at), (x, base), 3.2)
    line(d, (x0, 46.5), (x1, 46.5), 3.2)


# The palette's CATEGORY BAR (MapEditor_Categories.cpp, docs/tool-refinement-
# plan.md Phase 1): one box per group of each grouping, plus the toggle's two
# faces. Square boxes like the palette's other controls, since the bar sits in
# the dock beside them. World reuses the globe - it is the same group in both
# groupings.
HOLE = (0, 0, 0, 0)


def glyph_cat_build(d):
    # A brick wall, courses offset: building the shape and the look.
    rows = ((28.0, ((26, 40), (43, 57))),
            (37.5, ((26, 32), (35, 49), (52, 57))),
            (47.0, ((26, 40), (43, 57))))
    for y, bricks in rows:
        for x0, x1 in bricks:
            d.rectangle([x0 * SS, y * SS, x1 * SS, (y + 7.5) * SS], fill=GLYPH)


def glyph_cat_populate(d):
    # A figure: what goes into a built level.
    r = 6.5
    d.ellipse([(41.5 - r) * SS, (25 - r + 6) * SS, (41.5 + r) * SS, (25 + r + 6) * SS],
              fill=GLYPH)
    d.rounded_rectangle([29 * SS, 40 * SS, 54 * SS, 60 * SS], radius=8 * SS, fill=GLYPH)
    d.rectangle([29 * SS, 55 * SS, 54 * SS, 60 * SS], fill=HOLE)  # cut flat at the waist


def glyph_cat_surfaces(d):
    # A tile: a framed square quartered, two quarters filled.
    x0, y0, x1, y1 = 27.0, 27.0, 56.0, 56.0
    d.rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], outline=GLYPH, width=int(3 * SS))
    m = (x0 + x1) / 2
    d.rectangle([x0 * SS, y0 * SS, m * SS, m * SS], fill=GLYPH)
    d.rectangle([m * SS, m * SS, x1 * SS, y1 * SS], fill=GLYPH)


def glyph_cat_structure(d):
    # A doorway: an arched frame with a door in it and its ring.
    x0, x1, top, base = 29.0, 54.0, 38.0, 57.0
    r = (x1 - x0) / 2
    cx = (x0 + x1) / 2
    d.pieslice([x0 * SS, (top - r) * SS, x1 * SS, (top + r) * SS], 180, 360, fill=GLYPH)
    d.rectangle([x0 * SS, top * SS, x1 * SS, base * SS], fill=GLYPH)
    # The door's two leaves, cut apart down the middle.
    d.rectangle([(cx - 1) * SS, (top - r + 4) * SS, (cx + 1) * SS, base * SS], fill=HOLE)
    rr = 2.2
    for x in (cx - 5, cx + 5):
        d.ellipse([(x - rr) * SS, (46 - rr) * SS, (x + rr) * SS, (46 + rr) * SS], fill=HOLE)


def glyph_cat_furnishings(d):
    # A torch in its wall sconce: what dresses a built room - the props, the
    # lights on its walls, the levers.
    d.polygon(S([(41.5, 22), (47, 31), (46.5, 36), (43.5, 39), (39.5, 39),
                 (36.5, 36), (36, 31)]), fill=GLYPH)                       # flame
    d.polygon(S([(41.5, 29), (43.5, 33), (42.5, 36), (40.5, 36), (39.5, 33)]),
              fill=HOLE)                                                   # its core
    d.polygon(S([(37.5, 41), (45.5, 41), (44, 55), (39, 55)]), fill=GLYPH)  # the torch
    line(d, (33, 47), (50, 47), 3.4)                                       # the ring
    d.rounded_rectangle([35 * SS, 55 * SS, 48 * SS, 60 * SS], radius=1.5 * SS,
                        fill=GLYPH)                                        # wall plate


def glyph_cat_creatures(d):
    # A skull: the monsters.
    cx = 41.5
    r = 13.5
    d.ellipse([(cx - r) * SS, 24 * SS, (cx + r) * SS, (24 + 2 * r) * SS], fill=GLYPH)
    d.rounded_rectangle([(cx - 8) * SS, 44 * SS, (cx + 8) * SS, 57 * SS], radius=2 * SS,
                        fill=GLYPH)
    er = 4.0
    for x in (cx - 6, cx + 6):  # eyes
        d.ellipse([(x - er) * SS, (37 - er) * SS, (x + er) * SS, (37 + er) * SS], fill=HOLE)
    d.polygon(S([(cx, 43), (cx - 2.5, 48), (cx + 2.5, 48)]), fill=HOLE)  # nose
    for x in (cx - 4, cx, cx + 4):  # teeth
        d.rectangle([(x - 0.8) * SS, 51 * SS, (x + 0.8) * SS, 57 * SS], fill=HOLE)


def glyph_cat_items(d):
    # A sword, point up-right: the things you carry.
    sword(d, (55, 27), (28, 56))


def glyph_cat_bystage(d):
    # Grouped by STAGE: three steps climbing left to right, the workflow's order.
    d.polygon(S([(26, 57), (26, 48), (35, 48), (35, 38), (45, 38), (45, 28),
                 (57, 28), (57, 57)]), fill=GLYPH)


def glyph_cat_bykind(d):
    # Grouped by KIND: four separate squares, things sorted by what they are.
    for x, y in ((27, 27), (43, 27), (27, 43), (43, 43)):
        d.rounded_rectangle([x * SS, y * SS, (x + 13) * SS, (y + 13) * SS], radius=2 * SS,
                            fill=GLYPH)


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
    square = square_blank(blank)
    for name, draw in (("icon_dropdown", glyph_dropdown),
                       ("icon_dropdown_open", glyph_dropdown_open),
                       ("icon_tb_dock_left", glyph_dock_left),
                       ("icon_tb_dock_right", glyph_dock_right),
                       ("icon_close", glyph_close),
                       ("icon_tb_box_plus", glyph_box_plus),
                       ("icon_tb_box_minus", glyph_box_minus),
                       ("icon_tb_box_left", glyph_box_left),
                       ("icon_tb_box_right", glyph_box_right),
                       ("icon_tb_box_up", glyph_box_up),
                       ("icon_tb_box_down", glyph_box_down),
                       ("icon_tb_box_world", glyph_box_world),
                       ("icon_tb_box_dungeon", glyph_box_dungeon),
                       ("icon_tb_cat_world", glyph_box_world),
                       ("icon_tb_cat_build", glyph_cat_build),
                       ("icon_tb_cat_populate", glyph_cat_populate),
                       ("icon_tb_cat_surfaces", glyph_cat_surfaces),
                       ("icon_tb_cat_structure", glyph_cat_structure),
                       ("icon_tb_cat_furnishings", glyph_cat_furnishings),
                       ("icon_tb_cat_creatures", glyph_cat_creatures),
                       ("icon_tb_cat_items", glyph_cat_items),
                       ("icon_tb_cat_bystage", glyph_cat_bystage),
                       ("icon_tb_cat_bykind", glyph_cat_bykind)):
        path = os.path.join(UI, name + ".png")
        render(square, draw).save(path)
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
