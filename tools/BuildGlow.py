# ============================================================================
# tools/BuildGlow.py - the soft radial glow behind a SET hand box.
#
#   python tools/BuildGlow.py            # writes assets/ui/glow_radial.png
#
# White, with the falloff in ALPHA (the sprite batch blends straight alpha), so
# the draw tints it to any colour: the hand box draws it in the theme accent.
# Full at the centre, easing to nothing at the edge of the inscribed circle
# (smoothstep, so there is no visible rim). Like every UI image the PNG is
# committed source; this script is how it was made, so a change to the falloff
# is a constant edit and a re-run.
# ============================================================================
import os
from PIL import Image

SIZE = 128
OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "ui", "glow_radial.png")


def falloff(r):
    """1 at the centre, 0 at the rim (r = 1), smooth at both ends."""
    t = min(max(1.0 - r, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def main():
    img = Image.new("RGBA", (SIZE, SIZE))
    c = (SIZE - 1) / 2.0
    px = img.load()
    for y in range(SIZE):
        for x in range(SIZE):
            r = (((x - c) ** 2 + (y - c) ** 2) ** 0.5) / c
            px[x, y] = (255, 255, 255, int(round(255 * falloff(r))))
    img.save(os.path.normpath(OUT))
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
