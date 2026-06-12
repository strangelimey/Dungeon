// ============================================================================
// UI/Font.h — TTF font baked to a glyph atlas.
//
// stb_truetype rasterizes ASCII 32..126 at a pixel height into one alpha
// atlas (stored as white RGBA so glyphs tint via vertex color), sized to fit
// the glyphs. Draw() walks a string and emits one SpriteBatch quad per
// glyph. The UI is normalized (see Widget.h), so SetHeight() re-bakes the
// atlas when the window height changes and text scales with everything
// else. If the requested font file is missing it falls back to standard
// Windows fonts (Consolas → Segoe UI → Arial). Glyphs outside ASCII are
// skipped.
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

	// Re-bakes the atlas at a new size. No-op unless the rounded height
	// changes; otherwise drains the GPU first (in-flight frames may still
	// sample the old atlas), so call between frames, not while recording.
	void SetHeight(float pixelHeight);

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

	void Bake(float pixelHeight);

	gfx::GraphicsDevice& m_device;
	std::vector<u8> m_ttf; // kept resident so SetHeight can re-bake
	std::unique_ptr<gfx::Texture> m_atlas;
	std::vector<Glyph> m_glyphs; // index = codepoint - 32
	float m_pixelHeight = 0;
	float m_ascent = 0;
};

} // namespace dungeon::ui
