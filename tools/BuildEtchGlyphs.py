# ============================================================================
# tools/BuildEtchGlyphs.py - symbols CUT INTO stone, gold in the groove.
#
# Michael (more-ui-updates): the movement buttons become square cut stones,
# and "instead of the gold paint for the symbol, etch that into the stone
# similar to the runes. Put gold paint in the bottom of the etched groove."
# The runes he meant are the rune TABLETS, whose carve is a height field
# (tools/AssetBaker RuneBaker: glyph coverage -> recessed height -> lit). This
# is the same idea flattened into a 2D overlay, drawn OVER a cut-stone block
# (frame_block, BuildUiFrames.py) and covering the whole face:
#   1. a groove: the glyph's coverage blurred into a V-section DEPTH, whose
#      slope is lit from the top-left like every bevel here. The wall nearer
#      the light falls into shadow, the far wall catches it; the deep middle
#      is a little occluded;
#   2. gold on the groove's FLOOR only - the deepest band - shaded by the same
#      slope (darker on the shadowed wall, brighter on the lit one), so it
#      reads as paint lying in a cut rather than painted on top.
# The _lit twin brightens the gold and lets a warm glow out of the groove: a
# current tab, a selected menu entry.
#
# THE GOLD IS NOT IN HERE (code-review C204). It used to be baked in, the
# dark-stone gold, so the etched symbols skipped the per-material ink solve
# (ui::ResolveInks) that every carved word goes through, and sat at 1-2:1 on
# the lighter materials. Each output is now THREE 128 px panels side by side
# (384 x 128), all drawn over the same square by ui::DrawCutStone:
#   [0] the GROOVE - grey and alpha only (black shadow, white highlight, the
#       occluded middle, and the shadowed floor's darkening): light, no
#       colour, so it suits every material, exactly as the bevel frames do;
#   [1] the GOLD FLOOR - a white mask the game tints with the material's
#       solved gold (CarvedGold; CarvedLit for the _lit twin, whose mask also
#       carries the glow);
#   [2] its SHEEN - a white mask over the floor's lit slope, tinted with that
#       ink brightened by TONE_MAX (ui::kEtchSheen - THE TWO MUST MATCH).
# The panels are SOLVED from the one-image look, not authored separately: for
# any ink, groove-then-floor-then-sheen over any stone equals the glow, groove
# and ink-x-tone floor composited the old way, wherever no channel of ink x
# tone clips (where one does, the sheen is that clipped colour and the slope
# between is a straight blend - `--check` reports by how much).
#
# Outputs, assets/ui/etch_<name>.png and etch_<name>_lit.png (the glyph inside
# each panel's central ~60%). Glyphs are drawn per direction, never one
# turned: a rotation would carry the top-left light round with it.
#
#   python tools\BuildEtchGlyphs.py [--preview <png>] [--ink r,g,b] [--stone <name>]
#   python tools\BuildEtchGlyphs.py --check    # solve exact + files current
#
# Needs Pillow + numpy. ASCII only.
# ============================================================================
import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "ui")
SIZE = 128   # one panel's side, in output texels
PANELS = 3   # groove, gold floor, sheen - ui::DrawCutStone draws them in that order
SS = 4       # supersampling for the glyph mask
W = SIZE * SS

# The groove. STROKE is the cut's width (in output texels). Its depth is a V:
# the DISTANCE in from the cut's edge (the mask eroded a texel at a time and
# summed), lightly smoothed - a blur alone gives a flat-topped profile whose
# floor is nearly the whole stroke, and then the gold hides the walls.
STROKE = 15.0
SMOOTH = 1.1
DEPTH = 3.0          # slope gain: how steep the walls light
LIGHT = np.array([-0.55, -0.70, 1.0])  # from the top-left, toward the viewer
SHADOW_A = 1.25      # black alpha per unit of shade falling away from the light
HIGHLIGHT_A = 0.85   # white alpha per unit facing it
OCCLUDE = 0.30       # black over the deepest part of the cut
FLOOR = (0.45, 0.80)  # depth band (of the max) where the gold lies
# The gold's shading across the floor, as a multiple of the ink: the shadowed
# wall's gold darkens to TONE_MIN, the lit wall's brightens to TONE_MAX.
# TONE_MAX is ui::kEtchSheen (src/UI/Skin.h): the sheen panel is tinted with
# the ink times it, so the two must change together.
TONE_MIN = 0.55
TONE_MAX = 1.35
TONE_GAIN = 1.4      # tone per unit of shade
FLOOR_A = 0.92       # the floor's opacity (1 on the _lit twin)
GLOW_A = 0.45        # the _lit twin's glow, at its brightest


def S(v):
    return v * SS


def line(d, pts, w):
    """A round-capped polyline in output texels on the supersampled canvas."""
    p = [(S(x), S(y)) for x, y in pts]
    d.line(p, fill=255, width=int(S(w)), joint="curve")
    r = S(w) / 2
    for x, y in (p[0], p[-1]):
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)


def chevron(d, cx, cy, dx, dy, size, w):
    """A chevron pointing along (dx, dy), its tip at the centre plus half size."""
    px, py = -dy, dx  # perpendicular
    tip = (cx + dx * size * 0.5, cy + dy * size * 0.5)
    a = (cx - dx * size * 0.5 + px * size, cy - dy * size * 0.5 + py * size)
    b = (cx - dx * size * 0.5 - px * size, cy - dy * size * 0.5 - py * size)
    line(d, [a, tip, b], w)


def chevrons(dx, dy, count):
    def draw(d):
        c = SIZE / 2
        # A pair is smaller and spaced so its parallel arms clear by a stroke
        # and a gap (they sit step * 0.89 apart): run together they read as one
        # thick blob, not two cuts.
        size = 28.0 if count == 1 else 21.0
        step = 27.0
        off = -(count - 1) * step / 2
        for i in range(count):
            o = off + i * step
            chevron(d, c + dx * o, c + dy * o, dx, dy, size, STROKE)
    return draw


def outline(*paths):
    """Closed and open strokes, each a list of points (closed = repeat the
    first point last)."""
    def draw(d):
        for p in paths:
            line(d, p, STROKE)
    return draw


def star(cx, cy, r_out, r_in, points=5):
    import math
    pts = []
    for k in range(points * 2 + 1):
        a = -math.pi / 2 + k * math.pi / points
        r = r_out if k % 2 == 0 else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


GLYPHS = {
    # The movement pad, in its own order: turn left, forward, turn right /
    # strafe left, back, strafe right. Turns are DOUBLE chevrons, as before.
    "move_turn_left": chevrons(-1, 0, 2),
    "move_forward": chevrons(0, -1, 1),
    "move_turn_right": chevrons(1, 0, 2),
    "move_strafe_left": chevrons(-1, 0, 1),
    "move_back": chevrons(0, 1, 1),
    "move_strafe_right": chevrons(1, 0, 1),
    # The character sheet's tabs (more-ui-updates Phase 3), in its Mode order.
    # They are drawn small (the tab stones are ~30 px tall), so each is a BOLD
    # outline filling most of the square - the old hand-drawn glyphs' shapes,
    # a backpack in place of the grid.
    "tab_inventory": outline(
        [(38, 54), (90, 54), (95, 100), (33, 100), (38, 54)],  # the bag
        [(50, 54), (50, 38), (78, 38), (78, 54)],               # its handle
        [(48, 72), (80, 72)]),                                  # the flap's edge
    "tab_stats": outline(
        [(38, 100), (38, 78)], [(64, 100), (64, 58)], [(90, 100), (90, 34)]),
    "tab_skills": outline(star(64, 68, 40, 17)),
    "tab_spells": outline([(64, 26), (94, 64), (64, 102), (34, 64), (64, 26)]),
    "tab_effects": outline(
        [(38, 30), (90, 30), (40, 98), (88, 98), (38, 30)]),     # an hourglass
}


def over(base, rgb, a):
    """Straight-alpha 'over': layer (rgb, a) on top of base (RGBA float)."""
    under = base[..., 3]
    out_a = a + under * (1.0 - a)
    out_rgb = (rgb * a[..., None] + base[..., :3] * under[..., None] * (1.0 - a[..., None])) \
        / np.maximum(out_a[..., None], 1e-6)
    base[..., :3] = np.where(out_a[..., None] > 0, out_rgb, 0.0)
    base[..., 3] = out_a


def layers(draw, lit):
    """The cut as the one-image look had it, with the ink left OUT: the glow's
    alpha, the groove (grey, alpha), the floor's alpha and its tone."""
    mask_img = Image.new("L", (W, W), 0)
    draw(ImageDraw.Draw(mask_img))
    mask_img = mask_img.resize((SIZE, SIZE), Image.LANCZOS)
    mask = np.asarray(mask_img).astype(np.float32) / 255.0

    # Depth: the distance in from the cut's edge - the mask eroded one texel at
    # a time and the layers summed - smoothed, normalised so the deepest point
    # of the thickest stroke is 1.
    acc = np.zeros((SIZE, SIZE), np.float32)
    layer = mask_img
    for _ in range(int(STROKE / 2) + 2):
        acc += np.asarray(layer).astype(np.float32) / 255.0
        layer = layer.filter(ImageFilter.MinFilter(3))
    # Scaled by the stroke's HALF-WIDTH, not by the deepest texel: where two
    # strokes meet the distance runs deeper, and scaling by that put the whole
    # gold floor in the joint and left every straight run bare.
    depth = np.clip(acc / (STROKE * 0.5), 0.0, 1.0)
    depth_img = Image.fromarray((depth * 255).astype(np.uint8))
    depth = np.asarray(depth_img.filter(ImageFilter.GaussianBlur(SMOOTH))).astype(np.float32) / 255.0
    depth *= mask

    # The cut's surface is z = -depth: its normal is (dD/dx, dD/dy, 1) / |..|,
    # so a wall whose depth grows away from the light faces away from it.
    gy, gx = np.gradient(depth * DEPTH * 8.0)
    n = np.stack([gx, gy, np.ones_like(gx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    L = LIGHT / np.linalg.norm(LIGHT)
    shade = n @ L - L[2]  # 0 on the flat stone

    zero = np.zeros((SIZE, SIZE), np.float32)
    glow = zero
    if lit:  # a warm glow out of the groove, under everything else
        blur = np.asarray(mask_img.filter(ImageFilter.GaussianBlur(7.0))).astype(np.float32) / 255.0
        glow = np.clip(blur * 1.6, 0, 1) * GLOW_A

    groove = np.zeros((SIZE, SIZE, 4), np.float32)
    dark = np.clip(-shade * SHADOW_A, 0, 1) * mask + OCCLUDE * depth
    over(groove, zero[..., None] + 0.0, np.clip(dark, 0, 1))
    light = np.clip(shade * HIGHLIGHT_A, 0, 1) * mask
    over(groove, zero[..., None] + 1.0, light)

    # The floor, lit by the same slope (paint in a cut, not on it).
    lo, hi = FLOOR
    floor = np.clip((depth - lo) / (hi - lo), 0, 1)
    floor = floor * floor * (3 - 2 * floor) * (1.0 if lit else FLOOR_A)
    tone = np.clip(1.0 + shade * TONE_GAIN, TONE_MIN, TONE_MAX)
    return {"glow": glow, "grey": groove[..., 0], "groove": groove[..., 3],
            "floor": floor, "tone": tone}


def solve(ly):
    """The three panels' (grey, alpha) / alpha / alpha that, drawn groove then
    floor (x ink) then sheen (x ink x TONE_MAX) over any stone, reproduce
    glow (x ink) under the groove under a floor of ink x tone.

    Writing f for the floor's alpha, g / v for the groove's alpha / grey, w for
    the glow's and t = (tone - 1) / (TONE_MAX - 1) for how far up its lit slope
    a texel is, the old composite over stone s is
        ink * (f (1-t) min(tone,1) + (1-f)(1-g) w) + sheen * f t
          + (1-f) g v + s (1-f)(1-g)(1-w)
    and the three panels give
        sheen * C + ink * (1-C) B + (1-C)(1-B) A u + s (1-C)(1-B)(1-A).
    Matching term by term: C, then B, then A, then u. Every one lands in 0..1
    (the floor's black share - its shadowed wall - is the gap between the two
    weights the groove keeps), so no texel is ever clamped."""
    f, g, v, w, tone = ly["floor"], ly["groove"], ly["grey"], ly["glow"], ly["tone"]
    t = np.clip((tone - 1.0) / (TONE_MAX - 1.0), 0.0, 1.0)
    lo = np.minimum(tone, 1.0)
    C = f * t
    ink = f * (1.0 - t) * lo + (1.0 - f) * (1.0 - g) * w
    B = np.where(C < 1.0 - 1e-6, ink / np.maximum(1.0 - C, 1e-6), 0.0)
    D = (1.0 - C) * (1.0 - B)                  # what the floor and sheen leave
    through = (1.0 - f) * (1.0 - g) * (1.0 - w)  # the stone's share
    A = np.where(D > 1e-6, 1.0 - through / np.maximum(D, 1e-6), 0.0)
    u = np.where(D - through > 1e-6, (1.0 - f) * g * v / np.maximum(D - through, 1e-6), 0.0)
    return [np.clip(x, 0.0, 1.0) for x in (u, A, B, C)]


def bake(draw, lit):
    """One etch: the three panels as an RGBA image, 384 x 128. The masks are
    white EVERYWHERE, their alpha alone the coverage, so filtering never pulls
    a dark fringe into the gold."""
    u, A, B, C = solve(layers(draw, lit))
    out = np.ones((SIZE, SIZE * PANELS, 4), np.float32)
    out[:, :SIZE, :3] = u[..., None]
    out[:, :SIZE, 3] = A
    out[:, SIZE:2 * SIZE, 3] = B
    out[:, 2 * SIZE:, 3] = C
    out[:, :SIZE, :3] *= (A > 0)[..., None]  # a transparent texel's grey is moot: 0
    return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def draw_panels(img, ink, stone):
    """What ui::DrawCutStone draws: the three panels over `stone` (an HxWx3
    float array the size of one panel), the floor tinted with `ink`, the sheen
    with ink x TONE_MAX clipped - as the render target clamps it."""
    p = np.asarray(img).astype(np.float32) / 255.0
    ink = np.asarray(ink, np.float32)
    sheen = np.clip(ink * TONE_MAX, 0, 1)
    out = stone.copy()
    for k, tint in ((0, np.ones(3, np.float32)), (1, ink), (2, sheen)):
        layer = p[:, k * SIZE:(k + 1) * SIZE]
        a = layer[..., 3:4]
        out = layer[..., :3] * tint * a + out * (1 - a)
    return out


def draw_reference(ly, ink, stone):
    """The one-image look, composited directly: glow, groove, then the floor
    as ink x tone, each channel clipped."""
    ink = np.asarray(ink, np.float32)
    w = ly["glow"][..., None]
    out = ink * w + stone * (1 - w)
    g = ly["groove"][..., None]
    out = ly["grey"][..., None] * g + out * (1 - g)
    f = ly["floor"][..., None]
    gold = np.clip(ink * ly["tone"][..., None], 0, 1)
    return gold * f + out * (1 - f)


def check():
    """The panels against the one-image look, and the files against a fresh
    bake. Exits non-zero on a solve that drifts or a file that is stale."""
    # Inks to test the solve with: one no channel of which clips at TONE_MAX
    # (there it must be exact, to the byte), and the kinds the solve picks -
    # the dark-stone gold, a pale gold, a dark bronze - where a channel clips.
    exact_inks = [(0.60, 0.46, 0.20), (0.30, 0.25, 0.20)]
    clipping_inks = [(0.80, 0.62, 0.26), (1.00, 0.86, 0.46), (1.00, 0.95, 0.80), (0.12, 0.08, 0.03)]
    stones = [np.full((SIZE, SIZE, 3), v, np.float32) for v in (0.0, 0.19, 0.45, 1.0)]
    worst_exact = worst_clip = 0.0
    p999 = []
    stale = []
    for name, draw in GLYPHS.items():
        for lit in (False, True):
            ly = layers(draw, lit)
            img = bake(draw, lit)
            path = os.path.join(OUT_DIR, f"etch_{name}{'_lit' if lit else ''}.png")
            disk = np.asarray(Image.open(path).convert("RGBA")) if os.path.exists(path) else None
            if disk is None or disk.shape != np.asarray(img).shape or (disk != np.asarray(img)).any():
                stale.append(os.path.basename(path))
            for stone in stones:
                for ink in exact_inks + clipping_inks:
                    d = np.abs(draw_panels(img, ink, stone) - draw_reference(ly, ink, stone)).max(-1) * 255
                    if ink in exact_inks:
                        worst_exact = max(worst_exact, float(d.max()))
                    else:
                        worst_clip = max(worst_clip, float(d.max()))
                        p999.append(float(np.percentile(d, 99.9)))
    print(f"  solve, ink unclipped: worst {worst_exact:.2f}/255 (8-bit rounding only)")
    print(f"  solve, a channel clips: worst {worst_clip:.2f}/255, 99.9th percentile {max(p999):.2f}/255"
          " (the lit slope's straight blend to the clipped sheen)")
    ok = worst_exact <= 3.0 and worst_clip <= 24.0
    if stale:
        print(f"  STALE: {len(stale)} file(s) differ from a fresh bake - re-run the script:")
        for s in stale:
            print(f"    {s}")
    else:
        print(f"  files: all {len(GLYPHS) * 2} etches match a fresh bake")
    verdict = "PASS" if ok and not stale else "FAIL"
    print(f"etchcheck RESULT={verdict}")
    return 0 if verdict == "PASS" else 1


def preview(path, names, ink, stone_name):
    # Each glyph on a frame_block over a stone, both states, at 96 px - the
    # floor tinted with `ink` (the lit row with it too: the game uses CarvedLit).
    stone = Image.open(os.path.join(OUT_DIR, "stones", f"{stone_name}.png")).convert("RGBA")
    block = Image.open(os.path.join(OUT_DIR, "frame_block.png")).convert("RGBA")
    cell = 96
    sheet = Image.new("RGBA", (len(names) * (cell + 8) + 8, 2 * (cell + 8) + 8), (20, 20, 22, 255))
    for i, n in enumerate(names):
        for j, suffix in enumerate(("", "_lit")):
            face = stone.crop((0, 0, cell, cell))
            face = Image.alpha_composite(face, block.resize((cell, cell), Image.LANCZOS))
            etch = Image.open(os.path.join(OUT_DIR, f"etch_{n}{suffix}.png")).convert("RGBA")
            etch = etch.resize((cell * PANELS, cell), Image.LANCZOS)
            base = np.asarray(face).astype(np.float32)[..., :3] / 255.0
            # draw_panels works at panel size: scale its SIZE view of the image.
            p = np.asarray(etch).astype(np.float32) / 255.0
            out = base
            sheen = np.clip(np.asarray(ink, np.float32) * TONE_MAX, 0, 1)
            for k, tint in ((0, np.ones(3, np.float32)), (1, np.asarray(ink, np.float32)), (2, sheen)):
                layer = p[:, k * cell:(k + 1) * cell]
                a = layer[..., 3:4]
                out = layer[..., :3] * tint * a + out * (1 - a)
            tile = Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")
            sheet.paste(tile, (8 + i * (cell + 8), 8 + j * (cell + 8)))
    sheet.save(path)
    print("preview ->", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="also write a preview sheet to this png")
    ap.add_argument("--ink", default="0.80,0.62,0.26",
                    help="the preview's gold, r,g,b (default: the dark-stone gold)")
    ap.add_argument("--stone", default="granite_grey", help="the preview's material")
    ap.add_argument("--check", action="store_true",
                    help="check the solve and that the files are current; write nothing")
    args = ap.parse_args()
    if args.check:
        return check()
    for name, draw in GLYPHS.items():
        for lit in (False, True):
            out = os.path.join(OUT_DIR, f"etch_{name}{'_lit' if lit else ''}.png")
            bake(draw, lit).save(out, optimize=True)
        print(f"  etch_{name}.png (+_lit)")
    if args.preview:
        ink = [float(c) for c in args.ink.split(",")]
        preview(args.preview, list(GLYPHS), ink, args.stone)
    return 0


if __name__ == "__main__":
    sys.exit(main())
