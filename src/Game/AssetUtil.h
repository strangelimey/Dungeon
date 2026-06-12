// ============================================================================
// Game/AssetUtil.h — asset-loading helpers shared across the game layer.
//
// Required assets fail hard (DN_ASSERT) with the loader's reason — a missing
// model or texture means the assets/ directory wasn't baked or copied next
// to the exe. Sounds degrade gracefully (the game runs silent), and textures
// prefer the baked .dds mip chains, falling back to PNG + runtime mips so a
// fresh checkout works before `AssetBaker mips` has run.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Assets/Wav.h"
#include "Graphics/Texture.h"

#include <memory>
#include <string>

namespace dungeon::game {

// Loads models/<name> or aborts with the loader's error.
assets::ModelData LoadModelOrDie(const std::string& name);

// Loads sounds/<name>; a missing file warns and returns silence.
assets::SoundData LoadSound(const std::string& name);

// Loads a texture by stem (no extension), preferring the baked .dds mip
// chain (no runtime filtering); falls back to the PNG + runtime mips.
// Returns null if neither file exists.
std::unique_ptr<gfx::Texture> TryLoadTextureFile(gfx::GraphicsDevice& device,
												 const std::string& stemPath);

// As TryLoadTextureFile, but the texture is required — missing aborts.
std::unique_ptr<gfx::Texture> LoadTextureFile(gfx::GraphicsDevice& device,
											  const std::string& stemPath);

} // namespace dungeon::game
