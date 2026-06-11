// ============================================================================
// UI/Font.h — TTF font baked to a glyph atlas.
//
// stb_truetype rasterizes ASCII 32..126 at a fixed pixel height into one
// 512x512 alpha atlas (stored as white RGBA so glyphs tint via vertex color).
// Draw() walks a string and emits one SpriteBatch quad per glyph. If the
// requested font file is missing it falls back to standard Windows fonts
// (Consolas → Segoe UI → Arial). Glyphs outside ASCII are skipped.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Graphics/SpriteBatch.h"
#include "Graphics/Texture.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::ui {
class Font {
public:
	// Tries `path` first, then common Windows fonts as fallback.
	Font(gfx::GraphicsDevice& device, const std::string& path, float pixelHeight);

	float Height() const { return m_pixelHeight; }
	float LineAdvance() const { return m_pixelHeight * 1.25f; }
	float MeasureWidth(std::string_view text) const;

	void Draw(gfx::SpriteBatch& batch, std::string_view text, float x, float y,
			  const Vec4& color) const;

private:
	struct Glyph {
		gfx::Rect uv;      // normalized atlas coordinates
		Vec2 offset;       // placement relative to the pen position
		Vec2 size;         // pixel size of the quad
		float advance = 0;
	};

	std::unique_ptr<gfx::Texture> m_atlas;
	std::vector<Glyph> m_glyphs; // index = codepoint - 32
	float m_pixelHeight = 0;
	float m_ascent = 0;
};

} // namespace dungeon::ui
