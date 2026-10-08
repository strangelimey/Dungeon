// ============================================================================
// UI/Font.h — TTF font with an on-demand glyph atlas.
//
// stb_truetype rasterizes glyphs INTO a growing alpha atlas the first time
// each codepoint is drawn or measured (stored as white RGBA so glyphs tint via
// vertex color); Latin-1 (32..255) is pre-warmed at bake time, and so is every
// code point the active language's .lang file uses (FontLibrary::Prewarm, at
// each language load - code-review C229), so the game's own text costs nothing
// at runtime. A glyph met after that (a name typed in another script) is a LATE
// bake: it is counted (LateGlyphs) and, inside a guarded frame, reported by the
// allocation guard rather than excused. Any Unicode codepoint the loaded font has a
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
// A Font is ONE size for life: a new size is a new Font, which FontLibrary
// vends per (face, pixel height) - text tracks the window by asking the library
// for another (see Widget.h). If the requested font file is missing it falls
// back to standard Windows fonts (Consolas -> Segoe UI -> Arial).
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Graphics/SpriteBatch.h"
#include "Graphics/Texture.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo; // resident font info, kept opaque to avoid leaking stb

namespace dungeon::ui {

// The bytes of one typeface. Kept resident because a glyph first met later is
// rasterized from them, and SHARED because a face is immutable once loaded and the same file
// backs every size it is drawn at: FontLibrary hands one blob to many Fonts
// instead of each keeping its own copy (see FontLibrary.h).
using FaceData = std::shared_ptr<const std::vector<u8>>;

// Reads a typeface, falling back to standard Windows fonts (Consolas -> Segoe
// UI -> Arial) when `path` is empty or missing. Never returns null — a machine
// with no usable font at all is a hard failure.
FaceData LoadFace(const std::string& path);

class Font {
public:
	// Draws from an already-loaded face. `face` must be non-null; several Fonts
	// (one per pixel size) normally share one blob. Bakes Latin-1 plus `warm`
	// (the language's code points) and uploads, so make it between frames.
	Font(gfx::GraphicsDevice& device, FaceData face, float pixelHeight,
		 std::span<const u32> warm = {});
	~Font(); // out-of-line for the unique_ptr<stbtt_fontinfo> member

	// Bakes every code point of `codepoints` not yet in the atlas and uploads
	// them - a language load (FontLibrary::Prewarm). Drains the GPU when there
	// is anything new, so call between frames, never mid-record.
	void Prewarm(std::span<const u32> codepoints);

	// Uploads any glyphs cached since the last call to the GPU atlas. Cheap
	// no-op when nothing new was seen. Drains the GPU when it does upload, so
	// call once per frame before drawing — never mid-record.
	void Commit();

	// Glyphs baked AFTER construction and the last Prewarm - met lazily in a
	// draw or a measure. Each is a whole-atlas upload with a GPU drain on the
	// next Commit; `fonts` prints the library's total.
	u64 LateGlyphs() const { return m_lateGlyphs; }

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

	void ResetAtlas(int size) const;    // clear the CPU atlas + shelf packer
	void Grow() const;                  // double the atlas, re-raster every glyph
	// Bakes every code point of `codepoints`, growing the atlas as often as it
	// takes - each growth repacks what is cached and the pass runs again for
	// what the full atlas turned away. Uploads nothing (Commit does).
	void BakeAll(std::span<const u32> codepoints) const;
	// Rasterizes `cp` into the atlas on first use; returns its cached glyph, or
	// nullptr if it was deferred (atlas full this frame — packs after Grow).
	const Glyph* EnsureGlyph(u32 cp) const;

	gfx::GraphicsDevice& m_device;
	FaceData m_face;                             // kept resident so we can re-bake
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
	// Set while a deliberate bake (construction, Prewarm) runs, so the glyphs
	// it makes are not counted as late.
	mutable bool m_baking = false;
	mutable u64 m_lateGlyphs = 0;
};

} // namespace dungeon::ui
