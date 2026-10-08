#include "UI/Skin.h"

#include <algorithm>
#include <cmath>

namespace dungeon::ui {

// ============================================================================
// 9-slice rendering — fixed corner ring, tiled edges/center.
// ============================================================================

namespace {

// Fills dst by repeating a source tile of tileW x tileH screen pixels whose
// texture window is [u0..u1] x [v0..v1]; the last row/column clips its uv to
// the partial coverage so the pattern never squashes.
void TileRegion(gfx::SpriteBatch& batch, const gfx::Texture& texture,
				const gfx::Rect& dst, float tileW, float tileH, float u0, float v0,
				float u1, float v1, const Vec4& tint) {
	if (dst.w <= 0.0f || dst.h <= 0.0f || tileW <= 0.0f || tileH <= 0.0f) return;
	for (float y = 0.0f; y < dst.h; y += tileH) {
		const float h = std::min(tileH, dst.h - y);
		const float v = v0 + (v1 - v0) * (h / tileH);
		for (float x = 0.0f; x < dst.w; x += tileW) {
			const float w = std::min(tileW, dst.w - x);
			const float u = u0 + (u1 - u0) * (w / tileW);
			batch.DrawSprite({dst.x + x, dst.y + y, w, h}, {u0, v0, u - u0, v - v0},
							 texture, tint);
		}
	}
}

// Fills dst with the stone, tiled on a grid anchored to the SCREEN origin
// rather than to dst, so neighbouring faces continue one slab. One quad per
// grid cell dst touches (a button is one to four; a window-wide panel a few).
// The sprite sampler clamps, which is why this tiles in quads rather than
// with uv past 1.
void TileStone(gfx::SpriteBatch& batch, const gfx::Texture& stone, const gfx::Rect& dst,
			   float tile, const Vec4& tint) {
	if (dst.w <= 0.0f || dst.h <= 0.0f || tile <= 0.0f) return;
	const float right = dst.x + dst.w, bottom = dst.y + dst.h;
	for (float ty = std::floor(dst.y / tile) * tile; ty < bottom; ty += tile) {
		const float y0 = std::max(ty, dst.y), y1 = std::min(ty + tile, bottom);
		for (float tx = std::floor(dst.x / tile) * tile; tx < right; tx += tile) {
			const float x0 = std::max(tx, dst.x), x1 = std::min(tx + tile, right);
			batch.DrawSprite({x0, y0, x1 - x0, y1 - y0},
							 {(x0 - tx) / tile, (y0 - ty) / tile, (x1 - x0) / tile,
							  (y1 - y0) / tile},
							 stone, tint);
		}
	}
}

const SkinPart& PartFor(const Skin& skin, Face face) {
	switch (face) {
	case Face::Panel: return skin.panel;
	case Face::Button: return skin.button;
	case Face::ButtonDown: return skin.buttonDown.texture ? skin.buttonDown : skin.button;
	case Face::Slot: return skin.slot;
	case Face::Block: return skin.block;
	case Face::BlockDown: return skin.blockDown.texture ? skin.blockDown : skin.block;
	}
	return skin.panel;
}

// The stone's brightness under each kind of face. A button stands a little
// proud of the panel it sits on, so it catches a little more light; a slot's
// darkness comes from its overlay's well, not from here.
float StoneTone(Face face) {
	switch (face) {
	case Face::Button: return 1.12f;
	case Face::ButtonDown: return 0.95f;
	// A block stands further proud than a button plate, and when pressed sits
	// down in its joint, out of the light.
	case Face::Block: return 1.16f;
	case Face::BlockDown: return 0.90f;
	default: return 1.0f;
	}
}

} // namespace

void DrawFace(gfx::SpriteBatch& batch, const gfx::Rect& dst, const Skin& skin, Face face,
			  const Vec4& tint) {
	const SkinPart& part = PartFor(skin, face);
	if (!part.texture || dst.w <= 0.0f || dst.h <= 0.0f) return;
	const float tone = StoneTone(face);
	if (skin.stone) {
		TileStone(batch, *skin.stone, dst, skin.stoneTile,
				  {tint.x * tone, tint.y * tone, tint.z * tone, tint.w});
	} else {
		const Vec4& f = skin.stoneFallback;
		batch.DrawRect(dst, {f.x * tint.x * tone, f.y * tint.y * tone, f.z * tint.z * tone,
							 f.w * tint.w});
	}
	// A busy material's grain, calmed toward its own mean (Skin::calm). Not in a
	// slot: an item's well is dark anyway, and the grain is what reads as stone.
	if (skin.stone && skin.calm > 0.0f && face != Face::Slot) {
		const Vec4& m = skin.stoneMean;
		batch.DrawRect(dst, {m.x * tint.x * tone, m.y * tint.y * tone, m.z * tint.z * tone,
							 skin.calm * tint.w});
	}
	if (face == Face::Panel && skin.sheen.texture)
		DrawNineSlice(batch, dst, skin.sheen, {1, 1, 1, tint.w});
	// The bevel carries only light, so it takes the face's alpha, not its tint:
	// a dimmed button keeps its edges.
	DrawNineSlice(batch, dst, part, {1, 1, 1, tint.w});
}

float FaceInset(const Skin& skin, Face face) {
	const SkinPart& part = PartFor(skin, face);
	return part.texture ? part.inset * part.scale : 0.0f;
}

namespace {
// What DrawCutStone last painted (LastEtchDrawn): [0] a plain etch, [1] a lit one.
EtchDrawn g_etchDrawn[2];
} // namespace

void DrawCutStone(gfx::SpriteBatch& batch, const gfx::Rect& dst, const Skin& skin,
				  const gfx::Texture* etch, float depth, bool hot, const Vec4& tint, bool lit) {
	if (!skin.block.texture || dst.w <= 0.0f || dst.h <= 0.0f) return;
	// The face flips to the pressed block for the deeper half of the motion
	// (Button's rule), and the symbol follows the depth down by a small share
	// of the block - the sink and the rise are both seen.
	const float lift = hot && depth <= 0.0f ? 1.06f : 1.0f;
	DrawFace(batch, dst, skin, depth >= 0.5f ? Face::BlockDown : Face::Block,
			 {tint.x * lift, tint.y * lift, tint.z * lift, tint.w});
	if (!etch) return;
	// The symbol is authored square; on an oblong block (a sheet tab) it
	// stays square, centred, sized by the short side.
	const float side = std::min(dst.w, dst.h);
	const float sink = depth * std::max(1.0f, side * 0.035f);
	const gfx::Rect at{dst.x + (dst.w - side) * 0.5f + sink, dst.y + (dst.h - side) * 0.5f + sink,
					   side, side};
	// The groove (light: the tint only dims it), then the gold on its floor and
	// that floor's lit slope, in this material's inks. Each panel's border texels
	// are clear, so a sample straddling two panels picks up nothing.
	constexpr float kPanel = 1.0f / static_cast<float>(kEtchPanels);
	const Vec4 ink = EtchInk(skin, lit);
	const Vec4 sheen = EtchSheen(ink);
	const gfx::Rect uv[kEtchPanels] = {{0.0f, 0.0f, kPanel, 1.0f},
									   {kPanel, 0.0f, kPanel, 1.0f},
									   {2.0f * kPanel, 0.0f, kPanel, 1.0f}};
	const Vec4 color[kEtchPanels] = {
		tint,
		{ink.x * tint.x, ink.y * tint.y, ink.z * tint.z, tint.w},
		{sheen.x * tint.x, sheen.y * tint.y, sheen.z * tint.z, tint.w}};
	// Drawn from the very table the record copies, so the record is what was drawn.
	EtchDrawn& drawn = g_etchDrawn[lit ? 1 : 0];
	for (int i = 0; i < kEtchPanels; ++i) {
		batch.DrawSprite(at, uv[i], *etch, color[i]);
		drawn.uv[i] = uv[i];
		drawn.color[i] = color[i];
	}
	drawn.tint = tint;
	++drawn.draws;
}

const EtchDrawn& LastEtchDrawn(bool lit) { return g_etchDrawn[lit ? 1 : 0]; }

void ResetEtchDrawn() {
	g_etchDrawn[0] = {};
	g_etchDrawn[1] = {};
}

Vec4 EtchInk(const Skin& skin, bool lit) { return lit ? skin.inkLit : skin.inkGold; }

Vec4 EtchSheen(const Vec4& ink) {
	return {std::min(1.0f, ink.x * kEtchSheen), std::min(1.0f, ink.y * kEtchSheen),
			std::min(1.0f, ink.z * kEtchSheen), ink.w};
}

EtchFloor MeasureEtchFloor(std::span<const u8> rgba, u32 width, u32 height) {
	EtchFloor out;
	if (height == 0 || width != height * kEtchPanels ||
		rgba.size() < static_cast<size_t>(width) * height * 4)
		return out;
	// A texel's alpha, and its grey (the mean of r, g, b), in 0..1.
	const auto alpha = [&](u32 x, u32 y) {
		return rgba[(static_cast<size_t>(y) * width + x) * 4 + 3] / 255.0;
	};
	const auto grey = [&](u32 x, u32 y) {
		const size_t i = (static_cast<size_t>(y) * width + x) * 4;
		return (rgba[i] + rgba[i + 1] + rgba[i + 2]) / (3.0 * 255.0);
	};
	double sheen = 0.0, ink = 0.0, under = 0.0, through = 0.0;
	u64 count = 0;
	for (u32 y = 0; y < height; ++y)
		for (u32 x = 0; x < height; ++x) {
			const double a = alpha(x, y), b = alpha(x + height, y), c = alpha(x + 2 * height, y);
			if (1.0 - (1.0 - b) * (1.0 - c) < 0.5) continue; // not the floor
			// DrawCutStone's three layers, unrolled: sheen over ink over groove.
			sheen += c * grey(x + 2 * height, y);
			ink += (1.0 - c) * b * grey(x + height, y);
			under += (1.0 - c) * (1.0 - b) * a * grey(x, y);
			through += (1.0 - c) * (1.0 - b) * (1.0 - a);
			++count;
		}
	if (count == 0) return out;
	const double n = static_cast<double>(count);
	out.sheen = static_cast<float>(sheen / n);
	out.ink = static_cast<float>(ink / n);
	out.grey = static_cast<float>(under / n);
	out.through = static_cast<float>(through / n);
	out.valid = true;
	return out;
}

Vec4 EtchFloorSeen(const EtchFloor& floor, const Vec4& ink, const Vec4& stone) {
	const Vec4 sheen = EtchSheen(ink);
	const auto channel = [&](float s, float i, float st) {
		return std::clamp(s * floor.sheen + i * floor.ink + floor.grey + st * floor.through, 0.0f,
						  1.0f);
	};
	return {channel(sheen.x, ink.x, stone.x), channel(sheen.y, ink.y, stone.y),
			channel(sheen.z, ink.z, stone.z), 1.0f};
}

void DrawNineSlice(gfx::SpriteBatch& batch, const gfx::Rect& dst,
				   const SkinPart& part, const Vec4& tint) {
	if (!part.texture || dst.w <= 0.0f || dst.h <= 0.0f) return;
	const gfx::Texture& tex = *part.texture;
	const float texW = static_cast<float>(tex.Width());
	const float texH = static_cast<float>(tex.Height());

	// Corner size on screen, shrunk when the rect is too small for two corners
	// (the uv window stays the full corner — a slight squash on tiny widgets).
	const float corner = std::max(0.0f, std::min(part.corner, std::min(texW, texH) * 0.5f));
	const float cs = std::min(corner * part.scale, std::min(dst.w, dst.h) * 0.5f);

	const float fu = corner / texW; // corner as a uv fraction
	const float fv = corner / texH;
	const float innerW = dst.w - 2.0f * cs; // screen-space middle band
	const float innerH = dst.h - 2.0f * cs;
	const float tileW = std::max(1.0f, (texW - 2.0f * corner) * part.scale);
	const float tileH = std::max(1.0f, (texH - 2.0f * corner) * part.scale);
	const float x1 = dst.x + cs, x2 = dst.x + dst.w - cs;
	const float y1 = dst.y + cs, y2 = dst.y + dst.h - cs;

	// Corners (fixed).
	batch.DrawSprite({dst.x, dst.y, cs, cs}, {0, 0, fu, fv}, tex, tint);
	batch.DrawSprite({x2, dst.y, cs, cs}, {1.0f - fu, 0, fu, fv}, tex, tint);
	batch.DrawSprite({dst.x, y2, cs, cs}, {0, 1.0f - fv, fu, fv}, tex, tint);
	batch.DrawSprite({x2, y2, cs, cs}, {1.0f - fu, 1.0f - fv, fu, fv}, tex, tint);

	if (part.stretch) {
		// Authored-face mode: edges stretch along their axis and the center
		// stretches both ways, so a plaque's baked lighting gradient spans the
		// widget once instead of repeating (tiling a gradient = visible bands).
		batch.DrawSprite({x1, dst.y, innerW, cs}, {fu, 0, 1.0f - 2 * fu, fv}, tex, tint);
		batch.DrawSprite({x1, y2, innerW, cs}, {fu, 1.0f - fv, 1.0f - 2 * fu, fv}, tex,
						 tint);
		batch.DrawSprite({dst.x, y1, cs, innerH}, {0, fv, fu, 1.0f - 2 * fv}, tex, tint);
		batch.DrawSprite({x2, y1, cs, innerH}, {1.0f - fu, fv, fu, 1.0f - 2 * fv}, tex,
						 tint);
		batch.DrawSprite({x1, y1, innerW, innerH},
						 {fu, fv, 1.0f - 2 * fu, 1.0f - 2 * fv}, tex, tint);
		return;
	}

	// Edges (tiled along their axis).
	TileRegion(batch, tex, {x1, dst.y, innerW, cs}, tileW, cs, fu, 0, 1.0f - fu, fv,
			   tint);
	TileRegion(batch, tex, {x1, y2, innerW, cs}, tileW, cs, fu, 1.0f - fv, 1.0f - fu,
			   1.0f, tint);
	TileRegion(batch, tex, {dst.x, y1, cs, innerH}, cs, tileH, 0, fv, fu, 1.0f - fv,
			   tint);
	TileRegion(batch, tex, {x2, y1, cs, innerH}, cs, tileH, 1.0f - fu, fv, 1.0f,
			   1.0f - fv, tint);

	// Center (tiled both ways).
	TileRegion(batch, tex, {x1, y1, innerW, innerH}, tileW, tileH, fu, fv, 1.0f - fu,
			   1.0f - fv, tint);
}

// ============================================================================
// The carved INKS: solved against the material's mean colour (ResolveInks),
// and the getters every carved word reads them through. Moved from
// Controls.cpp by code-review C127 - the solve writes only Skin fields.
// ============================================================================

namespace {
Vec4 Mix(const Vec4& a, const Vec4& b, float t) {
	return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
			a.w + (b.w - a.w) * t};
}
// sRGB channel -> linear light, and a colour's relative luminance (WCAG 2).
float Linear(float c) {
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float RelLuminance(const Vec4& c) {
	return 0.2126f * Linear(c.x) + 0.7152f * Linear(c.y) + 0.0722f * Linear(c.z);
}
// The ends an ink is pushed toward when it does not read: a pale gold (still
// warm, so it reads as the same metal lit) and a dark bronze. A mid-grey stone
// caps any colour near 5:1, so these sit close to white and black.
constexpr Vec4 kInkPale{1.0f, 0.95f, 0.80f, 1.0f};
constexpr Vec4 kInkDeep{0.12f, 0.08f, 0.03f, 1.0f};
// What a part-transparent ink looks like over `bg` - the colour the eye judges.
Vec4 Over(const Vec4& ink, const Vec4& bg) {
	Vec4 seen = Mix(bg, ink, ink.w);
	seen.w = 1.0f;
	return seen;
}
// `ink` kept if it reads on `bg` at `target`, else moved the least distance along
// either ramp that gets there; if neither does, the ramp end that reads best.
Vec4 Legible(const Vec4& ink, const Vec4& bg, float target) {
	if (ContrastRatio(Over(ink, bg), bg) >= target) return ink;
	constexpr int kSteps = 20;
	int best = kSteps + 1;
	Vec4 pick = ink;
	for (const Vec4& end : {kInkPale, kInkDeep})
		for (int i = 1; i <= kSteps && i < best; ++i) {
			Vec4 c = Mix(ink, end, static_cast<float>(i) / kSteps);
			c.w = ink.w;
			if (ContrastRatio(Over(c, bg), bg) >= target) {
				best = i;
				pick = c;
			}
		}
	if (best <= kSteps) return pick;
	Vec4 pale = kInkPale, deep = kInkDeep;
	pale.w = deep.w = ink.w;
	return ContrastRatio(Over(pale, bg), bg) >= ContrastRatio(Over(deep, bg), bg) ? pale : deep;
}
} // namespace

float ContrastRatio(const Vec4& a, const Vec4& b) {
	const float la = RelLuminance(a), lb = RelLuminance(b);
	return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

void ResolveInks(Skin& skin) {
	const Vec4 bg{skin.stoneMean.x, skin.stoneMean.y, skin.stoneMean.z, 1.0f};
	const Skin authored; // the dark-stone inks the defaults carry
	skin.inkGold = Legible(authored.inkGold, bg, kInkContrast);
	skin.inkTitle = Legible(authored.inkTitle, bg, kInkContrast);
	skin.inkPlain = Legible(authored.inkPlain, bg, kInkContrastPlain);
	// The hover must still SHOW: lit reads at least as well as the gold, and
	// further from the stone than it, or it is the gold pushed further along.
	skin.inkLit = Legible(authored.inkLit, bg, kInkContrast);
	if (ContrastRatio(skin.inkLit, bg) < ContrastRatio(skin.inkGold, bg) + 0.5f) {
		const bool paler = RelLuminance(skin.inkGold) > RelLuminance(bg);
		skin.inkLit = Mix(skin.inkGold, paler ? Vec4{1, 1, 1, 1} : Vec4{0, 0, 0, 1}, 0.5f);
		skin.inkLit.w = 1.0f;
	}
	// Disabled: the gold sunk most of the way back into its own stone.
	skin.inkDisabled = Mix(skin.inkGold, bg, 0.55f);
	skin.inkDisabled.w = 1.0f;
}

Vec4 CarvedGold(const Skin* skin) { return skin ? skin->inkGold : Skin{}.inkGold; }
Vec4 CarvedLit(const Skin* skin) { return skin ? skin->inkLit : Skin{}.inkLit; }
Vec4 CarvedTitle(const Skin* skin) { return skin ? skin->inkTitle : Skin{}.inkTitle; }
Vec4 CarvedPlain(const Skin* skin) { return skin ? skin->inkPlain : Skin{}.inkPlain; }
Vec4 CarvedDisabled(const Skin* skin) {
	return skin ? skin->inkDisabled : Skin{}.inkDisabled;
}

} // namespace dungeon::ui
