#pragma once

#include <string>

namespace dungeon::baker {

// Bakes the rune-item assets: one shared carved-stone tablet model
// (models/rune_tablet.gltf), four per-element PBR texture sets with an Elder
// Futhark glyph carved into the face (textures/rune_<elem>_2k.png + _n + _mr),
// and four small element icons for the held cursor / inventory slot
// (textures/rune_icon_<elem>.png, PNG only). `assetsDir` holds models/ and
// textures/. A full bake calls this before the mip pass; the `runes` subcommand
// runs it standalone (PNG only — the _2k set loads fine without a .dds).
bool BakeRunes(const std::string& assetsDir);

} // namespace dungeon::baker
