# ============================================================================
# tools/BuildFlame.py - the flame drawn over a LIT TORCH's icon in a hand.
#
#   python tools/BuildFlame.py            # writes assets/ui/flame.png
#
# A teardrop: round at the base, widest a third of the way up, drawn out to a
# point at the top. White, with the shape in ALPHA (the sprite batch blends
# straight alpha), so the draw stacks it in layers and tints each one - a deep
# orange body, a yellow heart, a near-white core - and squashes and sways them
# a little out of step, which is what makes it flicker (PartyHudDraw.cpp,
# DrawHeldFlame). The base sits on the image's bottom edge, centred, so the draw
# can stand it on the torch head. Like every UI image the PNG is committed
# source; this script is how it was made.
# ============================================================================
import math
import os
from PIL import Image

W, H = 64, 128
OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "ui", "flame.png")


def smooth(t):
	t = min(max(t, 0.0), 1.0)
	return t * t * (3.0 - 2.0 * t)


def half_width(v):
	"""The flame's half-width at height v (0 = base, 1 = tip), peak 1."""
	if v <= 0.0 or v >= 1.0:
		return 0.0
	w = math.sqrt(v) * (1.0 - v) ** 1.25
	peak = math.sqrt(0.29) * (0.71) ** 1.25  # its maximum, near v = 0.29
	return w / peak


def main():
	img = Image.new("RGBA", (W, H))
	px = img.load()
	for y in range(H):
		v = 1.0 - (y + 0.5) / H  # 0 at the bottom row, 1 at the top
		hw = half_width(v) * 0.78
		for x in range(W):
			u = abs((x + 0.5) / W * 2.0 - 1.0)  # 0 on the centre line, 1 at the sides
			edge = 0.0 if hw <= 0.0 else 1.0 - u / hw
			# A soft edge, a base that melts into the torch, a tip that thins out.
			a = smooth(edge * 2.2) * smooth(v * 3.5) * (1.0 - 0.35 * v)
			px[x, y] = (255, 255, 255, int(round(255 * min(max(a, 0.0), 1.0))))
	img.save(os.path.normpath(OUT))
	print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
	main()
