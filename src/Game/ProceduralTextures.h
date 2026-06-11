#pragma once

#include "Assets/Image.h"

namespace dungeon::game {

// Stone textures generated in code so the game runs with zero asset files.
assets::ImageData MakeBrickWallTexture(u32 size = 256);
assets::ImageData MakeFloorSlabTexture(u32 size = 256);
assets::ImageData MakeCeilingTexture(u32 size = 256);

} // namespace dungeon::game
