# ============================================================================
# tools/BuildRuneIcons.py - the rune icons for the runes that have no hand-made
# one (assets/ui/rune_icon_<id>.png).
#
# The first seven icons were committed as images. This draws further ones IN
# THEIR STYLE by starting from one of them: rune_icon_sight.png is split into
# its tile and its glyph (each pixel a blend of exactly those two colours - the
# same observation BuildRuneGlow.py rests on), the glyph is painted out, and
# the new rune's strokes are drawn in its place: the same tile, the same gold,
# round-capped strokes of the same width, anti-aliased by supersampling.
#
# The strokes are the SAME unit-cell coordinates as tools/AssetBaker/
# RuneBaker.cpp's kRunes (y up), so the icon and the carved tablet agree. Keep
# the two tables in step.
#
# CHECK: --check redraws Dagaz over the painted-out Sight tile and reports how
# far it lands from the committed rune_icon_sight.png - the evidence that the
# stroke width and mapping really are the original's. Measured: mean 2.5 of
# 255, the residue all in the outermost pixel of each stroke (the original's
# edge anti-aliasing is a little harder than this supersampling).
#
# Run: python tools/BuildRuneIcons.py [--check]   (Pillow + numpy). Then run
# tools/BuildRuneGlow.py, which derives the glyph + glow images from these.
# ============================================================================
import os
import sys

import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(REPO, "assets", "ui")
TEMPLATE = "sight"
STROKE_HALF = 5.6   # px at 128: half the committed icons' stroke width
SUPER = 4           # supersampling per axis

# id -> strokes (x0, y0, x1, y1), unit cell, y UP. Mirrors RuneBaker.cpp.
RUNES = {
	# Ingwaz - the seed: a closed diamond, one thing becoming many.
	"multiple": [(0.50, 0.86, 0.76, 0.50), (0.76, 0.50, 0.50, 0.14),
				 (0.50, 0.14, 0.24, 0.50), (0.24, 0.50, 0.50, 0.86)],
	# Hagalaz - hail: two staves and the falling bar between them.
	"explode": [(0.34, 0.14, 0.34, 0.86), (0.66, 0.14, 0.66, 0.86),
				(0.34, 0.64, 0.66, 0.40)],
}
CHECK = {"sight": [(0.34, 0.14, 0.34, 0.86), (0.66, 0.14, 0.66, 0.86),
				   (0.34, 0.14, 0.66, 0.86), (0.34, 0.86, 0.66, 0.14)]}


def split(im):
	"""(tile rgb, glyph rgb, glyph coverage 0..1) of an icon - BuildRuneGlow's split."""
	rgb, a = im[..., :3], im[..., 3]
	opaque = a > 250
	cols, counts = np.unique(rgb[opaque].reshape(-1, 3), axis=0, return_counts=True)
	order = np.argsort(-counts)
	tile = cols[order[0]]
	glyph = next(cols[i] for i in order[1:] if np.linalg.norm(cols[i] - tile) > 60.0)
	d = glyph - tile
	t = np.clip(((rgb - tile) @ d) / float(d @ d), 0.0, 1.0)
	return tile, glyph, t


def coverage(strokes, size):
	"""Round-capped stroke coverage, supersampled."""
	n = size * SUPER
	ys, xs = np.mgrid[0:n, 0:n].astype(np.float64)
	px = (xs + 0.5) / SUPER
	py = (ys + 0.5) / SUPER
	best = np.full((n, n), 1e9)
	for x0, y0, x1, y1 in strokes:
		ax, ay = x0 * size, (1.0 - y0) * size
		bx, by = x1 * size, (1.0 - y1) * size
		vx, vy = bx - ax, by - ay
		wx, wy = px - ax, py - ay
		t = np.clip((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0, 1.0)
		best = np.minimum(best, np.hypot(wx - t * vx, wy - t * vy))
	inside = (best <= STROKE_HALF).astype(np.float64)
	return inside.reshape(size, SUPER, size, SUPER).mean(axis=(1, 3))


def draw(clean, tile, glyph, strokes):
	"""The new glyph over `clean` (the template with its own glyph painted out).
	Blending toward the glyph colour by coverage keeps whatever the tile does
	elsewhere (its rim, its corners) exactly as the template has it."""
	cover = coverage(strokes, clean.shape[0])[..., None]
	out = clean.copy()
	out[..., :3] = clean[..., :3] + cover * (glyph - clean[..., :3])
	return out


def main():
	src = np.asarray(Image.open(os.path.join(UI, f"rune_icon_{TEMPLATE}.png")).convert("RGBA")).astype(np.float64)
	tile, glyph, t = split(src)
	# Paint the template's glyph out: each pixel is tile + t * (glyph - tile).
	src = src.copy()
	clean = src.copy()
	clean[..., :3] = src[..., :3] - t[..., None] * (glyph - tile)
	if "--check" in sys.argv:
		got = draw(clean, tile, glyph, CHECK[TEMPLATE])
		diff = np.abs(got[..., :3] - src[..., :3]).max(axis=-1) * (src[..., 3] / 255.0)
		print(f"check vs rune_icon_{TEMPLATE}.png: mean {diff.mean():.2f}, "
			  f"p99 {np.percentile(diff, 99):.1f}, max {diff.max():.0f} (0..255)")
		return
	for rune, strokes in RUNES.items():
		out = draw(clean, tile, glyph, strokes)
		path = os.path.join(UI, f"rune_icon_{rune}.png")
		Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), "RGBA").save(path)
		print(f"wrote {path}")


if __name__ == "__main__":
	main()
