# ============================================================================
# tools/BuildUiFrames.py - the bevel overlays every stone UI face is drawn with.
#
# A skinned face is a stone tile (tools/BuildUiStones.py) with one of these
# drawn OVER it as a 9-slice. They carry no stone and no colour - only light:
# white with alpha where a bevel catches the light (top and left), black with
# alpha where it falls into shadow (bottom and right), a dark outline, and for
# a sunken slot a darkened centre. So any stone wears any frame, and switching
# stones never needs a re-bake of these.
#
# Authored at 2x: a 64-texel image with a 16-texel corner, drawn at about
# half scale so the bevel stays crisp. Outputs, all in assets/ui/:
#   frame_panel.png        raised, soft      - windows, docks, popups
#   frame_button.png       raised, tighter   - every button face, tab
#   frame_button_down.png  the same, sunk    - a held / active button
#   frame_slot.png         sunken, dark well - every item socket
#   sheen_panel.png        a soft polish highlight, stretched over a panel
#
#   python tools\BuildUiFrames.py [--preview <png>]
#
# Needs Pillow + numpy. ASCII only.
# ============================================================================
import argparse
import os
import sys

import numpy as np
from PIL import Image

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "ui")
N = 64       # texture size (texels)
CORNER = 16  # 9-slice corner (texels); the runtime's SkinPart.corner must match


def frame(outline, band, light, shadow, centre=0.0, sunken=False, groove=0.0):
    """One bevel overlay. outline: dark rim width (texels); band: bevel width;
    light / shadow: peak alpha of the highlight / shadow at the rim, fading to 0
    across the band; centre: black alpha over the middle (a sunken well);
    sunken swaps which sides catch the light; groove: alpha of a thin dark line
    where the bevel meets the flat."""
    rgba = np.zeros((N, N, 4), np.float32)
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32) + 0.5
    # Distance to each edge; the NEAREST edge decides which way a texel faces,
    # which splits the two mixed corners along their diagonal.
    d_top, d_left = yy, xx
    d_bottom, d_right = N - yy, N - xx
    d = np.minimum(np.minimum(d_top, d_left), np.minimum(d_bottom, d_right))
    lit = np.minimum(d_top, d_left) <= np.minimum(d_bottom, d_right)
    if sunken:
        lit = ~lit

    # The well first, so the bevel and outline draw over it.
    if centre > 0.0:
        rgba[..., 3] = np.where(d > outline, centre, 0.0)

    t = np.clip(1.0 - (d - outline) / band, 0.0, 1.0)  # 1 at the rim, 0 inside
    in_band = (d > outline) & (d <= outline + band)
    a_light = light * t ** 1.5
    a_shadow = shadow * t ** 1.2
    # White light over the well blends toward white; black shadow deepens it.
    for mask, colour, alpha in ((in_band & lit, 1.0, a_light), (in_band & ~lit, 0.0, a_shadow)):
        under = rgba[..., 3]
        out_a = alpha + under * (1.0 - alpha)
        rgb = np.where(out_a[..., None] > 0,
                       (colour * alpha[..., None] + rgba[..., :3] * under[..., None] * (1.0 - alpha[..., None]))
                       / np.maximum(out_a[..., None], 1e-6), 0.0)
        rgba[..., :3] = np.where(mask[..., None], rgb, rgba[..., :3])
        rgba[..., 3] = np.where(mask, out_a, rgba[..., 3])

    if groove > 0.0:
        g = (d > outline + band) & (d <= outline + band + 1.5)
        rgba[..., :3] = np.where(g[..., None], 0.0, rgba[..., :3])
        rgba[..., 3] = np.where(g, np.maximum(rgba[..., 3], groove), rgba[..., 3])

    rim = d <= outline
    rgba[..., :3] = np.where(rim[..., None], 0.0, rgba[..., :3])
    rgba[..., 3] = np.where(rim, 0.9, rgba[..., 3])
    return Image.fromarray((np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def sheen():
    # Polish: brightest toward the top-left, gone by the lower right. Stretched
    # over the whole panel (corner 0), so it is a gradient, not a pattern.
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32) / (N - 1)
    g = np.clip(1.0 - (xx * 0.55 + yy * 0.75), 0.0, 1.0) ** 2
    rgba = np.zeros((N, N, 4), np.float32)
    rgba[..., :3] = 1.0
    rgba[..., 3] = 0.10 * g
    return Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA")


PARTS = {
    "frame_panel": lambda: frame(outline=2, band=9, light=0.22, shadow=0.55, groove=0.22),
    "frame_button": lambda: frame(outline=2, band=7, light=0.30, shadow=0.60),
    "frame_button_down": lambda: frame(outline=2, band=6, light=0.12, shadow=0.60,
                                       centre=0.18, sunken=True),
    "frame_slot": lambda: frame(outline=2, band=6, light=0.16, shadow=0.70,
                                centre=0.72, sunken=True),
    "sheen_panel": sheen,
}


def preview(path):
    # Each part over a stone, at the 0.5 draw scale, for a look before a launch.
    stones = os.path.join(OUT_DIR, "stones")
    names = sorted(f for f in os.listdir(stones) if f.endswith(".png"))
    stone = Image.open(os.path.join(stones, names[0])).convert("RGBA")
    sheet = Image.new("RGBA", (5 * 140 + 20, 160), (20, 20, 22, 255))
    for i, part in enumerate(PARTS):
        over = Image.open(os.path.join(OUT_DIR, part + ".png")).resize((120, 120), Image.LANCZOS)
        base = stone.crop((0, 0, 120, 120))
        sheet.alpha_composite(Image.alpha_composite(base, over), (20 + i * 140, 20))
    sheet.save(path)
    print("preview ->", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="also write a preview sheet to this png")
    args = ap.parse_args()
    for name, make in PARTS.items():
        out = os.path.join(OUT_DIR, name + ".png")
        make().save(out, optimize=True)
        print(f"  {name}.png")
    if args.preview:
        preview(args.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
