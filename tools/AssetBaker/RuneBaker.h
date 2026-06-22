#pragma once

#include <string>

namespace dungeon::baker {

// Bakes the rune-item assets: one shared carved-stone tablet model
// (models/rune_tablet.gltf), four per-element PBR texture sets with an Elder
// Futhark glyph carved into the face (textures/rune_<elem>_2k.png + _n + _mr),
// and four small element icons for the held cursor / inventory slot
// (ui/rune_icon_<elem>.png, PNG only — UI icons live under ui/, not textures/).
// `assetsDir` holds models/, textures/ and ui/. A full bake calls this before
// the mip pass (which only processes textures/); the `runes` subcommand
// runs it standalone (PNG only — the _2k set loads fine without a .dds).
bool BakeRunes(const std::string& assetsDir);

} // namespace dungeon::baker
