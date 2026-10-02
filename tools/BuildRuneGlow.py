# ============================================================================
# tools/BuildRuneGlow.py - the Magic window's glowing runes, from the rune icons.
#
# Each assets/ui/rune_icon_<id>.png is a translucent tinted TILE with a glyph
# drawn on it in the school's colour, and every pixel is a blend of exactly
# those two colours (the glyph's anti-aliased edge included). So the glyph can
# be lifted out EXACTLY: a pixel's coverage is how far it lies from the tile
# colour toward the glyph colour. From that coverage this writes, per rune:
#   assets/ui/rune_glyph_<id>.png - the glyph alone, white, coverage in alpha
#   assets/ui/rune_glow_<id>.png  - the glyph blurred into a soft halo, white,
#                                   normalised so its brightest point is opaque
# Both white so the game tints them (the school colour; white for the form
# runes - Michael, ui-updates). It also PRINTS each icon's glyph colour, which
# PartyHudDraw.cpp's RuneGlowColor holds as constants - re-run, copy them.
#
# Run: python tools/BuildRuneGlow.py   (needs Pillow + numpy; the script is the
# asset - the PNGs it writes are committed beside the icons they come from)
# ============================================================================
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(REPO, "assets", "ui")
RUNES = ["fire", "earth", "air", "water", "project", "protect", "sight", "multiple", "explode"]
GLOW_RADIUS = 9.0     # px of blur at the icons' 128px - about 7% of the tile
GLOW_SPREAD = 1.35    # the halo's alpha is lifted, then clipped, so it reads wide


def split(path):
	im = np.asarray(Image.open(path).convert("RGBA")).astype(np.float64)
	rgb, a = im[..., :3], im[..., 3]
	opaque = a > 250
	# The tile is the commonest opaque colour; the glyph the commonest one far
	# from it.
	cols, counts = np.unique(rgb[opaque].reshape(-1, 3), axis=0, return_counts=True)
	order = np.argsort(-counts)
	tile = cols[order[0]]
	glyph = None
	for i in order[1:]:
		if np.linalg.norm(cols[i] - tile) > 60.0:
			glyph = cols[i]
			break
	if glyph is None:
		sys.exit(f"{path}: no glyph colour found")
	d = glyph - tile
	t = ((rgb - tile) @ d) / float(d @ d)
	cover = np.clip(t, 0.0, 1.0) * (a / 255.0)
	return cover, tile, glyph


def white(alpha):
	h, w = alpha.shape
	out = np.zeros((h, w, 4), dtype=np.uint8)
	out[..., :3] = 255
	out[..., 3] = np.clip(alpha * 255.0 + 0.5, 0, 255).astype(np.uint8)
	return Image.fromarray(out, "RGBA")


def main():
	for rune in RUNES:
		src = os.path.join(UI, f"rune_icon_{rune}.png")
		cover, tile, glyph = split(src)
		white(cover).save(os.path.join(UI, f"rune_glyph_{rune}.png"))
		mask = Image.fromarray((cover * 255.0).astype(np.uint8), "L")
		blur = np.asarray(mask.filter(ImageFilter.GaussianBlur(GLOW_RADIUS))).astype(np.float64)
		blur = np.clip(blur / max(blur.max(), 1.0) * GLOW_SPREAD, 0.0, 1.0)
		white(blur).save(os.path.join(UI, f"rune_glow_{rune}.png"))
		g = glyph / 255.0
		print(f"{rune:8s} glyph {{{g[0]:.2f}f, {g[1]:.2f}f, {g[2]:.2f}f}}  tile {tuple(int(c) for c in tile)}")


if __name__ == "__main__":
	main()
