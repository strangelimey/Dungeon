// ============================================================================
// UI/Font.h — TTF font with an on-demand glyph atlas.
//
// stb_truetype rasterizes glyphs INTO a growing alpha atlas the first time
// each codepoint is drawn or measured (stored as white RGBA so glyphs tint via
// vertex color); Latin-1 (32..255) is pre-warmed at bake time so Western text
// costs nothing extra at runtime. Any Unicode codepoint the loaded font has a
// glyph for is supported — Cyrillic, Greek, CJK, etc. — without a fixed bake
// range; the cache simply grows. (CJK still needs a font that CONTAINS those
// glyphs: the Windows fallbacks — Consolas/Segoe UI/Arial — cover Latin +
// Cyrillic + Greek but NOT CJK, so a CJK language must ship/point at a CJK
// font.)
//
// Caching happens during Draw()/MeasureWidth(), but the GPU texture is only
// (re)built by Commit(), which drains the GPU first — so Commit() must run
// between frames, never while recording. Call it once per frame per font
// before any drawing (GameUI::UpdateFonts does this for the UI fonts). A glyph
// first seen during a frame's draw pass therefore appears one frame later; UI
// text is on screen for many frames, so this is invisible in practice.
//
// SetHeight() re-bakes every cached glyph at the new size when the window
// height changes, so text scales with the normalized UI (see Widget.h). If the
// requested font file is missing it falls back to standard Windows fonts
// (Consolas -> Segoe UI -> Arial).
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Graphics/SpriteBatch.h"
#include "Graphics/Texture.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo; // resident font info, kept opaque to avoid leaking stb

namespace dungeon::ui {
class Font {
public:
	// Tries `path` first, then common Windows fonts as fallback.
	Font(gfx::GraphicsDevice& device, const std::string& path, float pixelHeight);
	~Font(); // out-of-line for the unique_ptr<stbtt_fontinfo> member

	// Re-bakes every cached glyph at a new size. No-op unless the rounded
	// height changes; otherwise rebuilds the atlas (Commit drains the GPU
	// first), so call between frames, not while recording.
	void SetHeight(float pixelHeight);

	// Uploads any glyphs cached since the last call to the GPU atlas. Cheap
	// no-op when nothing new was seen. Drains the GPU when it does upload, so
	// call once per frame before drawing — never mid-record.
	void Commit();

	float Height() const { return m_pixelHeight; }
	float LineAdvance() const { return m_pixelHeight * 1.25f; }
	float MeasureWidth(std::string_view text) const;

	void Draw(gfx::SpriteBatch& batch, std::string_view text, float x, float y,
			  const Vec4& color) const;

private:
	struct Glyph {
		gfx::Rect uv;      // normalized atlas coordinates
		Vec2 offset;       // placement relative to the pen position
		Vec2 size;         // pixel size of the quad (0 = no bitmap, e.g. space)
		float advance = 0;
	};

	void Rebake(float pixelHeight);     // (re)compute metrics + re-raster all glyphs
	void ResetAtlas(int size) const;    // clear the CPU atlas + shelf packer
	void Grow() const;                  // double the atlas, re-raster every glyph
	// Rasterizes `cp` into the atlas on first use; returns its cached glyph, or
	// nullptr if it was deferred (atlas full this frame — packs after Grow).
	const Glyph* EnsureGlyph(u32 cp) const;

	gfx::GraphicsDevice& m_device;
	std::vector<u8> m_ttf;                       // kept resident so we can re-bake
	std::unique_ptr<stbtt_fontinfo> m_info;      // resident font info
	std::unique_ptr<gfx::Texture> m_atlas;
	float m_scale = 0;       // stb pixel scale for the current height
	float m_pixelHeight = 0;
	float m_ascent = 0;

	// The glyph cache + CPU-side atlas are an implementation detail of the
	// const query path (Draw/MeasureWidth), hence mutable.
	mutable std::vector<u8> m_alpha;             // CPU atlas: per-texel coverage
	mutable int m_atlasSize = 0;
	mutable int m_penX = 0, m_penY = 0, m_rowH = 0; // shelf packer cursor
	mutable std::unordered_map<u32, Glyph> m_glyphs;
	mutable bool m_dirty = false;                // CPU atlas changed since Commit
	mutable bool m_growNeeded = false;           // a glyph overflowed; grow at Commit
};

} // namespace dungeon::ui
