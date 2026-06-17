#include "UI/Font.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

#include <stb_truetype.h>

namespace dungeon::ui {

namespace {
// The atlas starts small (Latin-1 packs into 256-512) and doubles on demand up
// to the cap; CJK languages push it toward the ceiling, Western ones never
// leave the floor.
constexpr int kInitialAtlas = 256;
constexpr int kMaxAtlasSize = 4096;
constexpr int kGlyphPad = 1; // 1px gutter so neighbours don't bleed under bilinear

// Decodes the next UTF-8 codepoint of `text`, advancing `i` past it.
// Malformed bytes decode as '?' (one byte consumed) so bad input stays
// visible without desyncing the rest of the string.
u32 NextCodepoint(std::string_view text, size_t& i) {
	const u32 lead = static_cast<unsigned char>(text[i++]);
	if (lead < 0x80) return lead;
	u32 cp = 0;
	int continuations = 0;
	if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1F; continuations = 1; }
	else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0F; continuations = 2; }
	else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07; continuations = 3; }
	else return '?';
	while (continuations-- > 0) {
		if (i >= text.size() ||
			(static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
			return '?';
		cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 0x3F);
	}
	return cp;
}
} // namespace

Font::Font(gfx::GraphicsDevice& device, const std::string& path, float pixelHeight)
	: m_device(device), m_info(std::make_unique<stbtt_fontinfo>()) {
	auto ttf = assets::ReadBinaryFile(path);
	if (!ttf) {
		for (const char* fallback :
			 {"C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\segoeui.ttf",
			  "C:\\Windows\\Fonts\\arial.ttf"}) {
			ttf = assets::ReadBinaryFile(fallback);
			if (ttf) {
				log::Info("Font fallback: {}", fallback);
				break;
			}
		}
	}
	DN_ASSERT(ttf.has_value(), "no usable font found");
	m_ttf = std::move(*ttf);
	stbtt_InitFont(m_info.get(), m_ttf.data(),
				   stbtt_GetFontOffsetForIndex(m_ttf.data(), 0));
	Rebake(std::round(pixelHeight));
}

Font::~Font() = default;

void Font::SetHeight(float pixelHeight) {
	pixelHeight = std::round(pixelHeight);
	if (pixelHeight == m_pixelHeight || pixelHeight <= 0) return;
	Rebake(pixelHeight); // Commit() inside drains the GPU before swapping atlases
}

void Font::ResetAtlas(int size) const {
	m_atlasSize = size;
	m_alpha.assign(static_cast<size_t>(size) * size, 0);
	m_penX = m_penY = kGlyphPad;
	m_rowH = 0;
	m_glyphs.clear();
}

void Font::Rebake(float pixelHeight) {
	m_pixelHeight = pixelHeight;
	m_scale = stbtt_ScaleForPixelHeight(m_info.get(), pixelHeight);

	int ascent = 0, descent = 0, lineGap = 0;
	stbtt_GetFontVMetrics(m_info.get(), &ascent, &descent, &lineGap);
	m_ascent = static_cast<float>(ascent) * m_scale;

	// Re-raster the glyphs already in use (a height change), or pre-warm Latin-1
	// on a fresh font so Western text never pays the lazy-cache cost.
	std::vector<u32> known;
	known.reserve(m_glyphs.empty() ? 224 : m_glyphs.size());
	if (m_glyphs.empty())
		for (u32 cp = 32; cp < 256; ++cp) known.push_back(cp);
	else
		for (const auto& [cp, g] : m_glyphs) known.push_back(cp);

	// Size the fresh atlas to the glyph height (~224 Latin glyphs of avg width
	// ~0.6h pack within a 14h square); grow below if the working set needs it.
	int size = kInitialAtlas;
	while (size < static_cast<int>(pixelHeight * 14.0f) && size < kMaxAtlasSize)
		size *= 2;
	ResetAtlas(size);
	for (u32 cp : known) EnsureGlyph(cp);
	while (m_growNeeded) { m_growNeeded = false; Grow(); } // keep doubling until all fit

	Commit();
}

void Font::Grow() const {
	std::vector<u32> known;
	known.reserve(m_glyphs.size());
	for (const auto& [cp, g] : m_glyphs) known.push_back(cp);

	const int next = std::min(m_atlasSize * 2, kMaxAtlasSize);
	ResetAtlas(next);
	for (u32 cp : known) EnsureGlyph(cp);
	m_dirty = true;
}

const Font::Glyph* Font::EnsureGlyph(u32 cp) const {
	if (auto it = m_glyphs.find(cp); it != m_glyphs.end()) return &it->second;

	int advance = 0, lsb = 0;
	stbtt_GetCodepointHMetrics(m_info.get(), static_cast<int>(cp), &advance, &lsb);
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	stbtt_GetCodepointBitmapBox(m_info.get(), static_cast<int>(cp), m_scale, m_scale,
								&x0, &y0, &x1, &y1);
	const int gw = x1 - x0, gh = y1 - y0;

	Glyph g;
	g.advance = static_cast<float>(advance) * m_scale;
	g.offset = {static_cast<float>(x0), static_cast<float>(y0)};
	g.size = {static_cast<float>(gw), static_cast<float>(gh)};

	if (gw > 0 && gh > 0) {
		if (m_penX + gw + kGlyphPad > m_atlasSize) { // wrap to the next shelf
			m_penX = kGlyphPad;
			m_penY += m_rowH + kGlyphPad;
			m_rowH = 0;
		}
		if (m_penY + gh + kGlyphPad > m_atlasSize) { // shelf overflow
			if (m_atlasSize < kMaxAtlasSize) {
				m_growNeeded = true; // defer the repack to Commit (between frames)
				return nullptr;
			}
			// At the cap: cache as an (invisible) zero-size quad so layout still
			// advances and we never thrash retrying a glyph that can't fit.
			g.size = {0, 0};
			return &m_glyphs.emplace(cp, g).first->second;
		}
		// Rasterize straight into the atlas (row stride = atlas width).
		u8* dst = m_alpha.data() + static_cast<size_t>(m_penY) * m_atlasSize + m_penX;
		stbtt_MakeCodepointBitmap(m_info.get(), dst, gw, gh, m_atlasSize, m_scale,
								  m_scale, static_cast<int>(cp));
		const float inv = 1.0f / static_cast<float>(m_atlasSize);
		g.uv = {m_penX * inv, m_penY * inv, gw * inv, gh * inv};
		m_penX += gw + kGlyphPad;
		m_rowH = std::max(m_rowH, gh);
		m_dirty = true;
	}
	return &m_glyphs.emplace(cp, g).first->second;
}

void Font::Commit() {
	if (m_growNeeded) { m_growNeeded = false; Grow(); }
	if (!m_dirty) return;

	// White RGBA atlas with glyph coverage in alpha (glyphs tint via vertex
	// color). Texture is immutable, so a new glyph means a fresh upload — rare
	// after the first frames a script appears, and free for steady Latin text.
	assets::ImageData image;
	image.width = image.height = m_atlasSize;
	image.pixels.resize(static_cast<size_t>(m_atlasSize) * m_atlasSize * 4);
	for (size_t i = 0; i < m_alpha.size(); ++i) {
		image.pixels[i * 4 + 0] = 255;
		image.pixels[i * 4 + 1] = 255;
		image.pixels[i * 4 + 2] = 255;
		image.pixels[i * 4 + 3] = m_alpha[i];
	}
	// In-flight frames may still sample the old atlas; uploads are rare enough
	// that a full drain is fine (same as the old quality hot-swap).
	m_device.WaitIdle();
	m_atlas = std::make_unique<gfx::Texture>(m_device, image);
	m_dirty = false;
}

float Font::MeasureWidth(std::string_view text) const {
	float width = 0;
	for (size_t i = 0; i < text.size();) {
		if (const Glyph* g = EnsureGlyph(NextCodepoint(text, i))) width += g->advance;
	}
	return width;
}

void Font::Draw(gfx::SpriteBatch& batch, std::string_view text, float x, float y,
				const Vec4& color) const {
	if (!m_atlas) return;
	// y is the top of the text box; pen baseline sits one ascent below.
	float penX = x;
	const float baseline = y + m_ascent;
	for (size_t i = 0; i < text.size();) {
		const Glyph* g = EnsureGlyph(NextCodepoint(text, i));
		if (!g) continue; // deferred this frame (atlas growing) — appears next frame
		if (g->size.x > 0 && g->size.y > 0) {
			const gfx::Rect dst{penX + g->offset.x, baseline + g->offset.y, g->size.x,
								g->size.y};
			batch.DrawSprite(dst, g->uv, *m_atlas, color);
		}
		penX += g->advance;
	}
}

} // namespace dungeon::ui
