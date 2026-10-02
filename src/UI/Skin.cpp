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

void DrawCutStone(gfx::SpriteBatch& batch, const gfx::Rect& dst, const Skin& skin,
				  const gfx::Texture* etch, float depth, bool hot, const Vec4& tint) {
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
	batch.DrawSprite({dst.x + (dst.w - side) * 0.5f + sink,
					  dst.y + (dst.h - side) * 0.5f + sink, side, side},
					 {0, 0, 1, 1}, *etch, {tint.x, tint.y, tint.z, tint.w});
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

} // namespace dungeon::ui
