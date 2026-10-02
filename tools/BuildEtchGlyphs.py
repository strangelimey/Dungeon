# ============================================================================
# tools/BuildEtchGlyphs.py - symbols CUT INTO stone, gold in the groove.
#
# Michael (more-ui-updates): the movement buttons become square cut stones,
# and "instead of the gold paint for the symbol, etch that into the stone
# similar to the runes. Put gold paint in the bottom of the etched groove."
# The runes he meant are the rune TABLETS, whose carve is a height field
# (tools/AssetBaker RuneBaker: glyph coverage -> recessed height -> lit). This
# is the same idea flattened into a 2D overlay.
#
# Each output is drawn OVER a cut-stone block (frame_block, BuildUiFrames.py),
# covering the whole face, and carries only what the cut adds - so it works on
# every UI material, exactly as the bevel frames do:
#   1. a groove: the glyph's coverage blurred into a V-section DEPTH, whose
#      slope is lit from the top-left like every bevel here. The wall nearer
#      the light falls into shadow, the far wall catches it; the deep middle
#      is a little occluded;
#   2. gold on the groove's FLOOR only - the deepest band - lit by the same
#      slope, so it reads as paint lying in a cut rather than painted on top.
# The _lit twin brightens the gold and lets a warm glow out of the groove: a
# current tab, a selected menu entry.
#
# Outputs, assets/ui/etch_<name>.png and etch_<name>_lit.png (128 px, the
# glyph inside the central ~60%). Glyphs are drawn per direction, never one
# turned: a rotation would carry the top-left light round with it.
#
#   python tools\BuildEtchGlyphs.py [--preview <png>]
#
# Needs Pillow + numpy. ASCII only.
# ============================================================================
import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "ui")
SIZE = 128   # output texels
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
GOLD = np.array([0.80, 0.62, 0.26])
GOLD_LIT = np.array([1.00, 0.84, 0.42])
FLOOR = (0.45, 0.80)  # depth band (of the max) where the gold lies


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


GLYPHS = {
    # The movement pad, in its own order: turn left, forward, turn right /
    # strafe left, back, strafe right. Turns are DOUBLE chevrons, as before.
    "move_turn_left": chevrons(-1, 0, 2),
    "move_forward": chevrons(0, -1, 1),
    "move_turn_right": chevrons(1, 0, 2),
    "move_strafe_left": chevrons(-1, 0, 1),
    "move_back": chevrons(0, 1, 1),
    "move_strafe_right": chevrons(1, 0, 1),
}


def over(base, rgb, a):
    """Straight-alpha 'over': layer (rgb, a) on top of base (RGBA float)."""
    under = base[..., 3]
    out_a = a + under * (1.0 - a)
    out_rgb = (rgb * a[..., None] + base[..., :3] * under[..., None] * (1.0 - a[..., None])) \
        / np.maximum(out_a[..., None], 1e-6)
    base[..., :3] = np.where(out_a[..., None] > 0, out_rgb, 0.0)
    base[..., 3] = out_a


def etch(draw, lit):
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

    out = np.zeros((SIZE, SIZE, 4), np.float32)
    zero = np.zeros((SIZE, SIZE), np.float32)

    if lit:  # a warm glow out of the groove, under everything else
        glow = np.asarray(mask_img.filter(ImageFilter.GaussianBlur(7.0))).astype(np.float32) / 255.0
        glow = np.clip(glow * 1.6, 0, 1) * 0.45
        over(out, GOLD_LIT[None, None, :], glow)

    dark = np.clip(-shade * SHADOW_A, 0, 1) * mask + OCCLUDE * depth
    over(out, zero[..., None] + 0.0, np.clip(dark, 0, 1))
    light = np.clip(shade * HIGHLIGHT_A, 0, 1) * mask
    over(out, zero[..., None] + 1.0, light)

    # Gold on the floor, lit by the same slope (paint in a cut, not on it).
    lo, hi = FLOOR
    floor = np.clip((depth - lo) / (hi - lo), 0, 1)
    floor = floor * floor * (3 - 2 * floor)
    tone = np.clip(1.0 + shade * 1.4, 0.55, 1.35)[..., None]
    gold = np.clip((GOLD_LIT if lit else GOLD)[None, None, :] * tone, 0, 1)
    over(out, gold, floor * (1.0 if lit else 0.92))

    return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def preview(path, names):
    # Each glyph on a frame_block over the first stone, both states, at 64 px.
    stones = os.path.join(OUT_DIR, "stones")
    stone = Image.open(os.path.join(stones, "granite_grey.png")).convert("RGBA")
    block = Image.open(os.path.join(OUT_DIR, "frame_block.png")).convert("RGBA")
    cell = 96
    sheet = Image.new("RGBA", (len(names) * (cell + 8) + 8, 2 * (cell + 8) + 8), (20, 20, 22, 255))
    for i, n in enumerate(names):
        for j, suffix in enumerate(("", "_lit")):
            face = stone.crop((0, 0, cell, cell))
            face = Image.alpha_composite(face, block.resize((cell, cell), Image.LANCZOS))
            g = Image.open(os.path.join(OUT_DIR, f"etch_{n}{suffix}.png")).resize((cell, cell), Image.LANCZOS)
            face = Image.alpha_composite(face, g)
            sheet.alpha_composite(face, (8 + i * (cell + 8), 8 + j * (cell + 8)))
    sheet.save(path)
    print("preview ->", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="also write a preview sheet to this png")
    args = ap.parse_args()
    for name, draw in GLYPHS.items():
        for lit in (False, True):
            out = os.path.join(OUT_DIR, f"etch_{name}{'_lit' if lit else ''}.png")
            etch(draw, lit).save(out, optimize=True)
        print(f"  etch_{name}.png (+_lit)")
    if args.preview:
        preview(args.preview, list(GLYPHS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
