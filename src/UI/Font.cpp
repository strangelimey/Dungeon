#include "UI/Font.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"

#include <cmath>

#include <stb_truetype.h>

namespace dungeon::ui {

namespace {
constexpr int kFirstChar = 32;
constexpr int kCharCount = 96;
constexpr int kMaxAtlasSize = 4096;
} // namespace

Font::Font(gfx::GraphicsDevice& device, const std::string& path, float pixelHeight)
	: m_device(device) {
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
	Bake(pixelHeight);
}

void Font::SetHeight(float pixelHeight) {
	pixelHeight = std::round(pixelHeight);
	if (pixelHeight == m_pixelHeight || pixelHeight <= 0) return;
	// In-flight frames may still sample the old atlas; resizes are rare
	// enough that a full drain is fine (same as the quality hot-swap).
	m_device.WaitIdle();
	Bake(pixelHeight);
}

void Font::Bake(float pixelHeight) {
	m_pixelHeight = pixelHeight;

	// Size the atlas to the glyph height (~96 glyphs of avg width ~0.6h pack
	// well within an 8h x 8h square); if a bake still comes back partial
	// (negative row count), double and retry.
	int atlasSize = 256;
	while (atlasSize < static_cast<int>(pixelHeight * 8.0f) &&
		   atlasSize < kMaxAtlasSize)
		atlasSize *= 2;

	std::vector<u8> alpha;
	std::vector<stbtt_bakedchar> baked(kCharCount);
	int rows = 0;
	for (;;) {
		alpha.assign(static_cast<size_t>(atlasSize) * atlasSize, 0);
		rows = stbtt_BakeFontBitmap(m_ttf.data(), 0, pixelHeight, alpha.data(),
									atlasSize, atlasSize, kFirstChar, kCharCount,
									baked.data());
		if (rows > 0 || atlasSize >= kMaxAtlasSize) break;
		atlasSize *= 2;
	}
	DN_ASSERT(rows > 0, "font baking failed");

	stbtt_fontinfo info;
	stbtt_InitFont(&info, m_ttf.data(), stbtt_GetFontOffsetForIndex(m_ttf.data(), 0));
	int ascent = 0, descent = 0, lineGap = 0;
	stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
	m_ascent =
		static_cast<float>(ascent) * stbtt_ScaleForPixelHeight(&info, pixelHeight);

	// White RGBA atlas with glyph coverage in alpha.
	assets::ImageData image;
	image.width = image.height = atlasSize;
	image.pixels.resize(static_cast<size_t>(atlasSize) * atlasSize * 4);
	for (size_t i = 0; i < alpha.size(); ++i) {
		image.pixels[i * 4 + 0] = 255;
		image.pixels[i * 4 + 1] = 255;
		image.pixels[i * 4 + 2] = 255;
		image.pixels[i * 4 + 3] = alpha[i];
	}
	m_atlas = std::make_unique<gfx::Texture>(m_device, image);

	m_glyphs.assign(kCharCount, {});
	const float inv = 1.0f / static_cast<float>(atlasSize);
	for (int i = 0; i < kCharCount; ++i) {
		const stbtt_bakedchar& b = baked[i];
		Glyph& g = m_glyphs[static_cast<size_t>(i)];
		g.uv = {b.x0 * inv, b.y0 * inv, (b.x1 - b.x0) * inv, (b.y1 - b.y0) * inv};
		g.offset = {b.xoff, b.yoff};
		g.size = {static_cast<float>(b.x1 - b.x0), static_cast<float>(b.y1 - b.y0)};
		g.advance = b.xadvance;
	}
}

float Font::MeasureWidth(std::string_view text) const {
	float width = 0;
	for (const char c : text) {
		const int index = static_cast<unsigned char>(c) - kFirstChar;
		if (index >= 0 && index < kCharCount) width += m_glyphs[index].advance;
	}
	return width;
}

void Font::Draw(gfx::SpriteBatch& batch, std::string_view text, float x, float y,
				const Vec4& color) const {
	// y is the top of the text box; pen baseline sits one ascent below.
	float penX = x;
	const float baseline = y + m_ascent;
	for (const char c : text) {
		const int index = static_cast<unsigned char>(c) - kFirstChar;
		if (index < 0 || index >= kCharCount) continue;
		const Glyph& g = m_glyphs[index];
		if (g.size.x > 0 && g.size.y > 0) {
			const gfx::Rect dst{penX + g.offset.x, baseline + g.offset.y, g.size.x,
								g.size.y};
			batch.DrawSprite(dst, g.uv, *m_atlas, color);
		}
		penX += g.advance;
	}
}

} // namespace dungeon::ui
