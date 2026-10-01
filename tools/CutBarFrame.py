# ============================================================================
# tools/CutBarFrame.py - cut the resource-bar FRAME out of the bought UI kit.
#
#   python tools/CutBarFrame.py [--src <png>] [--out <png>] [--width N] [--preview <png>]
#
# The Medieval RPG UI kit (itch, Vill8tion; docs/costs.md) ships its bars as
# opaque 4096x1024 PNGs: a red fluid in a dark iron tube, on solid black. The
# game wants the IRON ONLY - the fill is procedural (assets/shaders/bar.hlsl),
# drawn as a plain rect UNDER this frame. So this script:
#
#   1. keys the black field to transparent. The background is clean (brightness
#      0 or 1) but the iron is nearly black itself, so the key is a TIGHT ramp,
#      transparent at max-channel 1, opaque at 3. A looser one (tested: 2..8,
#      3..16) washes the ironwork out to grey lace.
#   2. punches the TUBE: every column's run of red pixels (fluid, foam, the lit
#      glass above it) becomes transparent, so the hole follows the capsule's
#      rounded ends rather than a rectangle. The rect the fill draws in is that
#      hole's bounding box; the opaque iron in the box's corners hides the rest.
#   3. desaturates what red is left on the frame (the glass edge's tint), since
#      the same frame carries a green and a blue fill too.
#   4. crops to the bar, downscales (premultiplied, so the keyed edges stay
#      clean), and PRINTS the numbers the code holds as constants:
#        tube insets  - the fill rect, as fractions of the frame image;
#        slice points - where the end caps stop and the plain rim begins, so the
#                       frame can be drawn 3-slice (caps at their aspect, the rim
#                       stretched) at any bar width.
#
# The SCRIPT is the asset (docs/authoring-scale.md's rule): a re-cut is a
# constant change plus a re-run. The last kit cut (skin_button / skin_slot,
# d12bc9e) lived in a scratchpad and is gone, which is why this one is here.
# Needs numpy + Pillow.
# ============================================================================
import argparse
import os
import sys

import numpy as np
from PIL import Image

KIT = os.path.join(os.environ.get("OneDrive", os.path.expanduser("~\\OneDrive")),
				   "DungeonAssets", "ui", "medieval-rpg-ui-kit", "extracted",
				   "upscayl_png_upscayl-standard-4x_4x")
DEFAULT_SRC = os.path.join(KIT, "Life Status Bars (1).png")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(REPO, "assets", "ui", "bar_frame.png")

KEY_LO, KEY_HI = 1.0, 3.0     # background key ramp, max-channel brightness
MARGIN = 8                    # source px kept around the bar's bounding box
RIM_GROW = 3                  # source px the tube hole grows into the rim's red fringe
MEDIAN_WINDOW = 41            # source columns smoothed over for the hole's edges
SNAP = 14                     # source px from the straight line that snaps onto it
RIM_TOLERANCE = 12            # source px the silhouette may wander and still be plain rim
BRIDGE = 8                    # passes closing one-column gaps in the plain run


def running_median(v, window):
	# NaN-aware running median (NaN = no tube in that column, kept as NaN).
	half = window // 2
	out = np.full_like(v, np.nan)
	for i in range(v.size):
		if np.isnan(v[i]):
			continue
		seg = v[max(i - half, 0):i + half + 1]
		out[i] = np.nanmedian(seg)
	return out


def red_mask(rgb):
	# Fluid, foam and lit glass are all strongly red-dominant; the iron is
	# neutral. (Foam is pinkish, hence the modest ratio.)
	r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
	return (r > 40) & (r > 1.5 * g) & (r > 1.5 * b)


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--src", default=DEFAULT_SRC)
	ap.add_argument("--out", default=DEFAULT_OUT)
	ap.add_argument("--width", type=int, default=1024)
	ap.add_argument("--preview", help="also write the frame over grey + checker")
	args = ap.parse_args()

	if not os.path.isfile(args.src):
		sys.exit(f"source not found: {args.src}")
	rgb = np.asarray(Image.open(args.src).convert("RGB")).astype(np.float32)
	h, w, _ = rgb.shape

	# --- 1. key the black field --------------------------------------------
	bright = rgb.max(axis=2)
	alpha = np.clip((bright - KEY_LO) / (KEY_HI - KEY_LO), 0.0, 1.0)

	# --- 2. punch the tube -------------------------------------------------
	red = red_mask(rgb)
	# The tube is the long horizontal run of red; find its band from the
	# columns that are mostly red, so a stray red speck elsewhere cannot widen it.
	col_count = red.sum(axis=0)
	tube_cols = np.where(col_count > 0.5 * col_count.max())[0]
	if tube_cols.size == 0:
		sys.exit("no red tube found - is this a Life Status Bar?")
	rows_any = np.where(red[:, tube_cols].any(axis=1))[0]
	band_top, band_bot = rows_any.min(), rows_any.max()
	# Each column's red run, then a RUNNING MEDIAN of its top and bottom: the
	# raw extents are ragged (a bubble reaches a few px lower here and there,
	# into the dark strip above the glass lip), and a ragged hole reads as
	# chewed iron once the fill behind it is a smooth glow.
	tops = np.full(w, np.nan)
	bots = np.full(w, np.nan)
	for x in range(w):
		ys = np.where(red[band_top:band_bot + 1, x])[0]
		if ys.size >= 4:
			tops[x], bots[x] = band_top + ys.min(), band_top + ys.max()
	tops, bots = running_median(tops, MEDIAN_WINDOW), running_median(bots, MEDIAN_WINDOW)
	# Along the straight run, SNAP to one top and one bottom: the fill draws as a
	# rect, so any column whose hole stops short of that rect shows the dark
	# strip as specks. Only the rounded ends (far off the line) keep their own.
	top_line, bot_line = np.nanmedian(tops), np.nanmedian(bots)
	tops = np.where(np.abs(tops - top_line) <= SNAP, top_line, tops)
	bots = np.where(np.abs(bots - bot_line) <= SNAP, bot_line, bots)
	hole = np.zeros((h, w), dtype=bool)
	for x in range(w):
		if not np.isnan(tops[x]):
			hole[int(tops[x]):int(bots[x]) + 1, x] = True
	# Grow the hole a few px into the rim's red fringe (cheap 4-way dilation).
	for _ in range(RIM_GROW):
		g = hole.copy()
		g[1:, :] |= hole[:-1, :]
		g[:-1, :] |= hole[1:, :]
		g[:, 1:] |= hole[:, :-1]
		g[:, :-1] |= hole[:, 1:]
		hole = g
	alpha[hole] = 0.0

	# --- 3. neutralise the red left on the frame ---------------------------
	lum = rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
	leftover = red & ~hole
	rgb[leftover] = lum[leftover][:, None]

	# --- 4. crop, downscale, measure ---------------------------------------
	ys, xs = np.where(alpha > 0.0)
	x0, x1 = max(xs.min() - MARGIN, 0), min(xs.max() + MARGIN + 1, w)
	y0, y1 = max(ys.min() - MARGIN, 0), min(ys.max() + MARGIN + 1, h)
	rgb, alpha, hole = rgb[y0:y1, x0:x1], alpha[y0:y1, x0:x1], hole[y0:y1, x0:x1]
	ch, cw = alpha.shape

	# Premultiply before resampling so the transparent black never bleeds
	# into the edge pixels, then divide back out.
	pm = np.dstack([rgb * alpha[..., None], alpha * 255.0]).clip(0, 255).astype(np.uint8)
	out_w = args.width
	out_h = max(1, round(ch * out_w / cw))
	small = np.asarray(Image.fromarray(pm, "RGBA").resize((out_w, out_h), Image.LANCZOS)
					   ).astype(np.float32)
	a = small[..., 3:4] / 255.0
	col = np.where(a > 0.0, small[..., :3] / np.maximum(a, 1e-6), 0.0)
	final = np.dstack([col, a * 255.0]).clip(0, 255).astype(np.uint8)
	os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
	Image.fromarray(final, "RGBA").save(args.out)

	# Tube insets: the hole's bounding box as fractions of the frame image.
	hy, hx = np.where(hole)
	tl, tr = hx.min() / cw, 1.0 - (hx.max() + 1) / cw
	tt, tb = hy.min() / ch, 1.0 - (hy.max() + 1) / ch

	# Slice points: the plain rim is where the frame's SILHOUETTE (first and
	# last solid row per column) matches the middle column's. The scrollwork
	# above and below the ends is what pushes it out, so this finds the caps.
	solid = alpha > 0.5
	first = np.where(solid.any(axis=0), solid.argmax(axis=0), -1)
	last = np.where(solid.any(axis=0), ch - 1 - solid[::-1].argmax(axis=0), -1)
	# The reference is the median over the central half, not one column: the
	# painted rim BOWS (its top edge sits ~9 px lower mid-bar than near the caps).
	mid = cw // 2
	centre = slice(cw // 4, 3 * cw // 4)
	ref_first, ref_last = np.median(first[centre]), np.median(last[centre])
	plain = (np.abs(first - ref_first) <= RIM_TOLERANCE) & \
			(np.abs(last - ref_last) <= RIM_TOLERANCE)
	# Bridge the odd column where the painted rim wobbles past the tolerance:
	# a cap is a long run of ornament, not a single stray column.
	for _ in range(BRIDGE):
		plain[1:-1] |= plain[:-2] & plain[2:]
	left = mid
	while left > 0 and plain[left - 1]:
		left -= 1
	right = mid
	while right < cw - 1 and plain[right + 1]:
		right += 1
	sl, sr = left / cw, 1.0 - (right + 1) / cw
	if os.environ.get("CUT_DEBUG"):
		for x in range(0, cw, cw // 40):
			print(f"  col {x:5d} first {first[x]:4d} last {last[x]:4d} plain {plain[x]}")

	print(f"wrote {args.out}  {out_w}x{out_h}")
	print(f"tube insets  left {tl:.4f}  right {tr:.4f}  top {tt:.4f}  bottom {tb:.4f}")
	print(f"slice caps   left {sl:.4f}  right {sr:.4f}  (plain rim between)")
	if sl <= tl or sr <= tr:
		print("WARNING: the plain rim reaches past the tube's ends - check the slice points")

	if args.preview:
		f = final.astype(np.float32)
		fa = f[..., 3:4] / 255.0
		grey = np.full_like(f[..., :3], 128.0)
		yy, xx = np.indices((out_h, out_w))
		chk = (((xx // 8 + yy // 8) % 2) * 80 + 90)[..., None].astype(np.float32)
		# Under the frame, a stand-in fill (a flat green) in the tube rect, so
		# the hole and the insets are judged together.
		def over(bg):
			bg = bg.repeat(3, axis=2) if bg.shape[2] == 1 else bg.copy()
			fx0, fx1 = round(tl * out_w), round((1 - tr) * out_w)
			fy0, fy1 = round(tt * out_h), round((1 - tb) * out_h)
			bg[fy0:fy1, fx0:fx1] = (40, 200, 70)
			return f[..., :3] * fa + bg * (1 - fa)
		# A third row: the frame 3-SLICED to twice its width (caps kept, rim
		# stretched) - the shape it takes on a long, thin bar in the game.
		lx, rx = round(sl * out_w), out_w - round(sr * out_w)
		mid_w = (rx - lx) + out_w
		mid = np.asarray(Image.fromarray(final[:, lx:rx]).resize((mid_w, out_h),
																  Image.BILINEAR))
		wide = np.concatenate([final[:, :lx], mid, final[:, rx:]], axis=1).astype(np.float32)
		wa = wide[..., 3:4] / 255.0
		wbg = np.full_like(wide[..., :3], 128.0)
		wx0, wx1 = round(tl * out_w), wide.shape[1] - round(tr * out_w)
		wbg[round(tt * out_h):out_h - round(tb * out_h), wx0:wx1] = (40, 200, 70)
		wrow = wide[..., :3] * wa + wbg * (1 - wa)
		pad = np.full((out_h, wrow.shape[1] - out_w, 3), 60.0)
		sheet = np.concatenate([np.concatenate([over(grey), pad], axis=1),
								np.concatenate([over(chk), pad], axis=1), wrow], axis=0)
		Image.fromarray(sheet.clip(0, 255).astype(np.uint8)).save(args.preview)
		print(f"preview {args.preview}")


if __name__ == "__main__":
	main()
