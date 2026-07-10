#include "UI/Skin.h"

#include <algorithm>

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

} // namespace

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
