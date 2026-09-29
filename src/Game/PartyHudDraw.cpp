// ============================================================================
// Game/PartyHudDraw.cpp — see PartyHudDraw.h.
// ============================================================================
#include "Game/PartyHudDraw.h"

#include "Game/PartyHudTypes.h"

#include <algorithm>
#include <string_view>

namespace dungeon::game {

void DrawStatBar(gfx::SpriteBatch& batch, const gfx::Rect& rect, float fraction,
				 const Vec4& color, const ui::Theme& theme) {
	batch.DrawRect(rect, theme.control);
	const float t = std::clamp(fraction, 0.0f, 1.0f);
	if (t > 0.0f) batch.DrawRect({rect.x, rect.y, rect.w * t, rect.h}, color);
	ui::DrawBorder(batch, rect, theme.panelBorder);
}

void DrawIdentityBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect,
						const Character& character) {
	ui::DrawBorder(batch, rect, character.portraitColor);
	ui::DrawBorder(batch, {rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2},
				   character.portraitColor);
}

void DrawPortrait(gfx::SpriteBatch& batch, const gfx::Rect& rect,
				  const Character& character, const ui::Font& font,
				  const ui::Theme& theme) {
	if (character.portrait) {
		batch.DrawSprite(rect, {0, 0, 1, 1}, *character.portrait, {1, 1, 1, 1});
		DrawIdentityBorder(batch, rect, character);
		return;
	}
	batch.DrawRect(rect, character.portraitColor);
	DrawIdentityBorder(batch, rect, character);
	const std::string_view initial = std::string_view(character.name).substr(0, 1);
	const float initialW = font.MeasureWidth(initial);
	font.Draw(batch, initial, rect.x + (rect.w - initialW) * 0.5f,
			  rect.y + (rect.h - font.Height()) * 0.5f, theme.text);
}

void DrawRuneFace(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  const ItemIconBank* icons, bool hot, bool disabled,
				  bool background) {
	if (background) batch.DrawRect(r, hot ? Vec4{0.12f, 0.12f, 0.13f, 1.0f} : kSlotBg);
	const gfx::Texture* icon = icons ? icons->For(RuneItemId(s)) : nullptr;
	if (icon) {
		const float pad = r.w * 0.08f;
		batch.DrawSprite({r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad},
						 {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
	} else {
		// Fallback: an element-tinted fill (ElementColor is premultiplied
		// additive - rebuild it opaque for flat UI ink).
		const Vec4 e = ElementColor(s);
		batch.DrawRect({r.x + 3, r.y + 3, r.w - 6, r.h - 6},
					   {e.x * 0.6f, e.y * 0.6f, e.z * 0.6f, 1.0f});
	}
	const Vec4 e = ElementColor(s);
	if (disabled) {
		// Washed out under a dark overlay, border flattened - reads as "spent".
		batch.DrawRect(r, {0.0f, 0.0f, 0.0f, 0.62f});
		ui::DrawBorder(batch, r, {e.x * 0.25f, e.y * 0.25f, e.z * 0.25f, 1.0f});
		return;
	}
	ui::DrawBorder(batch, r, hot ? Vec4{e.x, e.y, e.z, 1.0f}
								 : Vec4{e.x * 0.6f, e.y * 0.6f, e.z * 0.6f, 1.0f});
}

} // namespace dungeon::game
