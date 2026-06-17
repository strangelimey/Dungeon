#pragma once

#include <string>

namespace dungeon::baker {

// Renders the three hit-feedback splat icons (hit_splat_small/med/hard.png,
// 128x128 RGBA with transparency) into <texturesDir>. The party bar draws one
// over a member's portrait when a monster lands a blow; severity picks the
// icon. Deterministic.
bool BakeHitSplats(const std::string& texturesDir);

} // namespace dungeon::baker
