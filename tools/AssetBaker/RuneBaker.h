#pragma once

#include <string>

namespace dungeon::baker {

// Bakes the rune-item assets: one shared carved-stone tablet model
// (models/rune_tablet.gltf) and four per-element PBR texture sets with an Elder
// Futhark glyph carved into the face (textures/rune_<elem>_2k.png + _n + _mr).
// (The rune cursor/inventory ICON is no longer baked — it's a committed source
// image at assets/ui/rune_icon_<elem>.png.) A full bake calls this before the
// mip pass; the `runes` subcommand runs it standalone (PNG only — the _2k set
// loads fine without a .dds).
bool BakeRunes(const std::string& assetsDir);

} // namespace dungeon::baker
