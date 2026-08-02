// ============================================================================
// Game/PartyHudDraw.h — shared draw helpers for the party HUD widgets.
//
// Free functions (not a class): CharacterPanel, HandSlot, InventoryWindow, and
// CharacterSheet all paint slots / portraits / resource bars the same way.
// ============================================================================
#pragma once

#include "Game/Character.h"
#include "Graphics/SpriteBatch.h"
#include "UI/Controls.h"

namespace dungeon::game {

// Shared background for item-bearing slots (hands, equipment doll, backpack):
// black, so the light-haloed 3D item icons read clearly against it.
inline constexpr Vec4 kSlotBg{0.0f, 0.0f, 0.0f, 1.0f};

void DrawStatBar(gfx::SpriteBatch& batch, const gfx::Rect& rect, float fraction,
				 const Vec4& color, const ui::Theme& theme);

// Baked portrait when present; otherwise the tinted square with the character's
// initial. The border is the character's identity color (doubled so it reads at
// party-bar size), matching the HandSlot stripe.
void DrawIdentityBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect,
						const Character& character);
void DrawPortrait(gfx::SpriteBatch& batch, const gfx::Rect& rect,
				  const Character& character, const ui::Font& font,
				  const ui::Theme& theme);

} // namespace dungeon::game
