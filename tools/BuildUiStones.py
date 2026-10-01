# ============================================================================
# tools/BuildUiStones.py - the curated stones the UI chrome can wear.
#
# Every UI face (panel, button, item slot) is drawn in LAYERS: a stone tile
# repeated across the face, under a stone-INDEPENDENT bevel overlay
# (tools/BuildUiFrames.py). This script makes the stone half: one seamless tile
# per entry in STONES, written to assets/ui/stones/<name>.png, which is the
# filtered list the Settings -> UI "Stone" dropdown offers. Adding a stone is
# one line here, a re-run, and a stone.<name> key in each assets/lang file
# (the dropdown's label; a missing key shows as the key itself).
#
# The source scans live in the OneDrive archive (DungeonAssets\2k\...). They
# tile at full size, so the script only RESIZES (never crops - a crop would
# break the wrap) and TONES: every stone is brought to the same mean
# luminance and its contrast eased, so whichever one is picked lands at panel
# darkness and calm enough to sit behind item icons. The runtime does no tone
# maths; it only tiles what is here.
#
#   python tools\BuildUiStones.py            # all stones
#   python tools\BuildUiStones.py --check    # also report each tile's seam
#
# Needs Pillow + numpy. ASCII only.
# ============================================================================
import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageEnhance, ImageStat

ARCHIVE = os.path.join(os.path.expandvars("%USERPROFILE%"), "OneDrive", "DungeonAssets", "2k")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "ui", "stones")

# The tile's size in texels. The UI draws it at about one texel a pixel at a
# 900px-tall window, so a panel shows a palm's width of grain, not a pattern.
SIZE = 1024

# name -> (archive path under 2k\, the mean luminance it is toned to (0..1,
# sRGB-encoded values - about 0.2 is panel darkness), contrast factor). The calm ones
# from the 2026-09-30 survey (docs/ui-panels-notes.md); busy stones (veined
# quartz, polished granite) fight item icons and stay out.
STONES = {
    "armani_marble": ("countertops/armani-marble", 0.21, 1.00),
    "granite_grey": ("rocks/granite-gray-white", 0.20, 0.80),
    "granite_flecks": ("countertops/gray-granite-flecks", 0.19, 0.75),
    "blackrock": ("rocks/blackrock", 0.18, 0.90),
    "granite_almond": ("countertops/almond-speckled-granite", 0.19, 0.70),
    "slate": ("rocks/slate-cliff-rock-bl4", 0.18, 0.70),
}


def find_albedo(folder):
    files = [f for f in os.listdir(folder) if f.lower().endswith((".png", ".jpg", ".jpeg"))]
    for key in ("albedo", "basecolor", "base_color", "diff", "color"):
        for f in files:
            low = f.lower()
            if key in low and "preview" not in low and "normal" not in low:
                return os.path.join(folder, f)
    return None


def seam_score(img):
    # How much worse the wrap edge is than an ordinary neighbouring column: 1.0
    # = indistinguishable, well above 1 = a visible seam when tiled.
    a = np.asarray(img.convert("L")).astype(np.float32)
    inner = np.abs(np.diff(a, axis=1)).mean()
    edge_x = np.abs(a[:, 0] - a[:, -1]).mean()
    edge_y = np.abs(a[0, :] - a[-1, :]).mean()
    return max(edge_x, edge_y) / max(inner, 1e-3)


def build(name, rel, target, contrast, check):
    folder = os.path.join(ARCHIVE, rel)
    src = find_albedo(folder) if os.path.isdir(folder) else None
    if not src:
        print(f"  {name}: NO ALBEDO under {folder} - skipped")
        return False
    img = Image.open(src).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)
    mean = ImageStat.Stat(img.convert("L")).mean[0] / 255.0
    img = ImageEnhance.Brightness(img).enhance(target / max(mean, 0.02))
    img = ImageEnhance.Contrast(img).enhance(contrast)
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, name + ".png")
    img.save(out, optimize=True)
    line = f"  {name}: {os.path.relpath(src, ARCHIVE)} -> {os.path.getsize(out) // 1024} KB"
    if check:
        line += f"  seam {seam_score(img):.2f}"
    print(line)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="report each tile's seam score")
    args = ap.parse_args()
    ok = sum(build(n, r, t, c, args.check) for n, (r, t, c) in STONES.items())
    print(f"{ok} / {len(STONES)} stones -> {os.path.normpath(OUT_DIR)}")
    return 0 if ok == len(STONES) else 1


if __name__ == "__main__":
    sys.exit(main())
