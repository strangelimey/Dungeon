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

struct ItemIconBank; // PartyHudTypes.h

// One rune face: the rune-item icon when loaded, else an element-tinted
// fallback square; element-coloured border. The spellbook's grid and sequence
// and a hand box armed with a spell all draw runes through this, so a rune
// reads the same everywhere. `disabled` washes it out under a dark overlay.
// `background` = false skips the face's own black fill, so whatever is under
// it shows through (a set hand's accent tint).
void DrawRuneFace(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  const ItemIconBank* icons, bool hot, bool disabled = false,
				  bool background = true);

} // namespace dungeon::game
