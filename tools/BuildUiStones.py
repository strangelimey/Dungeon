# ============================================================================
# tools/BuildUiStones.py - the curated stones the UI chrome can wear.
#
# Every UI face (panel, button, item slot) is drawn in LAYERS: a stone tile
# repeated across the face, under a stone-INDEPENDENT bevel overlay
# (tools/BuildUiFrames.py). This script makes the stone half: one seamless tile
# per entry in STONES, written to assets/ui/stones/<name>.png, which is the
# curated list the Settings -> Stone tab offers. Adding a stone is one line
# here, a re-run, and a stone.<name> key in each assets/lang file (the tab's
# label; a missing key shows as the key itself).
#
# Beside the tiles it writes what the tab needs to SHOW them without loading
# a 1024px tile per entry:
#   thumbs/<name>.png  a small crop of the tile at the grain the UI draws it
#                      at (a downscale of the whole tile would show the grain
#                      several times finer than any panel ever does);
#   stones.cat         one [name] block per stone: its toned `luminance` (the
#                      tab's light / dark filter reads it) and its `source`.
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
from PIL import Image, ImageEnhance, ImageFilter, ImageStat

ARCHIVE = os.path.join(os.path.expandvars("%USERPROFILE%"), "OneDrive", "DungeonAssets", "2k")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "ui", "stones")

# The tile's size in texels. The UI draws it at about one texel a pixel at a
# 900px-tall window, so a panel shows a palm's width of grain, not a pattern.
SIZE = 1024

# The thumbnail: a THUMB_CROP-texel square of the tile (about what one HUD dock
# shows of it), saved at THUMB texels.
THUMB_CROP = 256
THUMB = 160

# At or above this toned luminance a stone files under "light" in the tab.
LIGHT_FROM = 0.25

# name -> (archive path under 2k\, the mean luminance it is toned to (0..1,
# sRGB-encoded values - about 0.2 is panel darkness), contrast factor, FAMILY -
# the Material tab's kind filter and stones.cat's `family`). The calm ones
# from the 2026-09-30 survey (docs/ui-panels-notes.md); busy stones (veined
# quartz, polished granite) fight item icons and stay out.
#
# The LIGHT stones (~0.30) are the more-ui-updates survey (2026-10-01): 29 light
# candidates toned to 0.30 on one contact sheet. Out: the granites (busy at any
# tone), sandstonecliff and flaking-limestone (seam 1.5+), and the near-
# featureless marbles and stucco, which tone to a flat grey. The calm, low-
# variation ones that stayed get MORE contrast than the dark set, not less - at
# 0.8 their grain disappears and the face reads as paint.
#
# The other FAMILIES (wood, forest, snow, rock) came with the idea that the
# place picks the material (Michael, same day): 30 candidates on one sheet.
# Out: oak-wood-bare and bare-wood1 (they do not tile - seam 7), the warm
# planks and plywood (seam 2+), and forest-floor / mossy-mud (too busy to put
# an icon on). Snow is toned LIGHTER than anything else (~0.42) and with much
# more contrast: at the stones' 0.3 it reads as dull grey card. Lava and mossy
# rock are loud on purpose - they are here to be judged, not because they are
# known to work.
STONES = {
    "armani_marble": ("countertops/armani-marble", 0.21, 1.00, "stone"),
    "granite_grey": ("rocks/granite-gray-white", 0.20, 0.80, "stone"),
    "granite_flecks": ("countertops/gray-granite-flecks", 0.19, 0.75, "stone"),
    "blackrock": ("rocks/blackrock", 0.18, 0.90, "stone"),
    "granite_almond": ("countertops/almond-speckled-granite", 0.19, 0.70, "stone"),
    "slate": ("rocks/slate-cliff-rock-bl4", 0.18, 0.70, "stone"),
    "limestone_flat": ("rocks/limestone_flat_textured", 0.30, 1.20, "stone"),
    "limestone_cliffs": ("rocks/limestone-cliffs", 0.30, 1.00, "stone"),
    "limestone_marked": ("rocks/limestonemarked2", 0.30, 1.00, "stone"),
    "limestone_pale": ("rocks/limestone3_bl4", 0.30, 0.90, "stone"),
    "rock_smooth": ("rocks/rock_smooth", 0.30, 1.20, "stone"),
    "marble_pillar": ("rocks/marble_pillar", 0.30, 1.00, "stone"),
    "marble_white": ("rocks/marble_white", 0.30, 1.40, "stone"),
    "speckled_stone": ("countertops/speckled-countertop1", 0.30, 1.30, "stone"),
    "planks_weathered": ("wood/wood_planks_old3", 0.28, 1.00, "wood"),
    "wood_cherry": ("wood/cherry-wood-veneer2", 0.24, 1.00, "wood"),
    "wood_dark": ("wood/antique-veneer1 bl", 0.20, 1.00, "wood"),
    "leaf_fall": ("ground/leaf-fall1-bl4", 0.24, 0.80, "forest"),
    "moss": ("ground/mixedmoss-bl4", 0.24, 0.80, "forest"),
    "pine_needles": ("ground/pineneedles-ground", 0.26, 0.80, "forest"),
    "snow_packed": ("ground/snow-packed12", 0.42, 1.60, "snow"),
    "snow_crusted": ("ground/Crusted_snow2", 0.42, 1.60, "snow"),
    "ice_field": ("ground/ice-field", 0.40, 1.40, "snow"),
    "snow_rock": ("ground/rock-snow-ice1-2k", 0.40, 1.20, "snow"),
    "desert_rock": ("ground/desert-rocks1", 0.28, 0.90, "rock"),
    "rubble": ("ground/rubble", 0.26, 0.90, "rock"),
    "mossy_rock": ("rocks/wet-mossy-rocks", 0.22, 0.80, "rock"),
    "lava_rock": ("ground/lava-and-rock", 0.20, 0.80, "rock"),
}


def find_albedo(folder):
    # .tif too: some archive sets ship only a TIFF albedo, which Pillow reads.
    exts = (".png", ".jpg", ".jpeg", ".tif", ".tiff")
    files = [f for f in os.listdir(folder) if f.lower().endswith(exts)]
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
    os.makedirs(os.path.join(OUT_DIR, "thumbs"), exist_ok=True)
    out = os.path.join(OUT_DIR, name + ".png")
    img.save(out, optimize=True)
    thumb = img.crop((0, 0, THUMB_CROP, THUMB_CROP)).resize((THUMB, THUMB), Image.LANCZOS)
    thumb.save(os.path.join(OUT_DIR, "thumbs", name + ".png"), optimize=True)
    line = f"  {name}: {os.path.relpath(src, ARCHIVE)} -> {os.path.getsize(out) // 1024} KB"
    if check:
        line += f"  seam {seam_score(img):.2f}"
    print(line)
    return True


def measure(name):
    # What the GAME needs to keep text legible on the baked tile (the contrast
    # pass, more-ui-updates): its mean colour - what a calming wash is made of -
    # and its DETAIL, the texture's contrast at the scale of a glyph (a band-pass:
    # a 1 px blur less a 12 px one, the spread of what is left). Busy leaves
    # score 0.05, smooth marble 0.01. Measured from the tile ON DISK, so the
    # index can be rewritten without the archive (--index-only).
    img = Image.open(os.path.join(OUT_DIR, name + ".png")).convert("RGB")
    mean = np.asarray(img, dtype=np.float32).reshape(-1, 3).mean(0) / 255.0
    lum = img.convert("L")
    fine = np.asarray(lum.filter(ImageFilter.GaussianBlur(1.0)), dtype=np.float32)
    coarse = np.asarray(lum.filter(ImageFilter.GaussianBlur(12.0)), dtype=np.float32)
    return mean, float((fine - coarse).std() / 255.0)


def write_index():
    # Block format (Game/Serialize.h), CRLF like every other .cat. Written from
    # the TABLE, so the luminance is the target the stone was toned to - the
    # number the filter means - not a re-measurement of the contrast-eased tile.
    lines = ["; Written by tools/BuildUiStones.py - do not edit; re-run the script.",
             "; luminance = the mean the stone was toned to (light from %.2f)." % LIGHT_FROM,
             "; family    = stone / wood / forest / snow / rock (the tab's kind filter).",
             "; mean      = the baked tile's mean colour (sRGB 0..1).",
             "; detail    = its contrast at glyph scale (busy = high; GameUI calms it).", ""]
    for name, (rel, target, _, family) in sorted(STONES.items()):
        mean, detail = measure(name)
        lines += [f"[{name}]", f"luminance = {target:.2f}", f"family = {family}",
                  f"source = {rel}", f"mean = {mean[0]:.3f} {mean[1]:.3f} {mean[2]:.3f}",
                  f"detail = {detail:.3f}", ""]
    with open(os.path.join(OUT_DIR, "stones.cat"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="report each tile's seam score")
    ap.add_argument("--index-only", action="store_true",
                    help="rewrite stones.cat from the tiles already baked (no archive needed)")
    args = ap.parse_args()
    if args.index_only:
        write_index()
        print(f"stones.cat rewritten for {len(STONES)} stones")
        return 0
    ok = sum(build(n, r, t, c, args.check) for n, (r, t, c, _) in STONES.items())
    write_index()
    print(f"{ok} / {len(STONES)} stones -> {os.path.normpath(OUT_DIR)}")
    return 0 if ok == len(STONES) else 1


if __name__ == "__main__":
    sys.exit(main())
