// ============================================================================
// Game/HandSlot.h — one hand box of the HUD control panel (left/right, Dungeon Master style).
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "UI/Controls.h"

#include <functional>
#include <vector>

namespace dungeon::game {

class HandSlot : public ui::Widget {
public:
	// `hand` is 0 = left / 1 = right (which inventory.Hand() this box shows).
	// `icons` (Game-owned, may be null) resolves the held item's icon to draw.
	// onLeft fires on a left click, onRight on a right click — GameUI decides
	// what each means given the held cursor (place / swap / pick up / attack /
	// context menu).
	HandSlot(const gfx::Rect& rect, const std::vector<Character>* roster,
			 size_t member, int hand,
			 const ItemIconBank* icons, std::function<void()> onLeft,
			 std::function<void()> onRight);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	const std::vector<Character>* m_roster;
	size_t m_member;
	// Re-resolved every Update/Draw (see CharacterPanel).
	const Character* m_character = nullptr;
	int m_hand;
	const ItemIconBank* m_icons;
	std::function<void()> m_onLeft;
	std::function<void()> m_onRight;
	bool m_hot = false;
	bool m_held = false;       // left-button press latched on this slot
	bool m_heldRight = false;  // right-button press latched on this slot
};

// The HUD Magic-area SPELLBOOK. A row of four member-colored SELECTOR buttons
// tops the magic box — one per party slot, disabled while that member is
// absent (short roster), down, or has NO memorized symbols; the selected one
// draws pressed (Michael, 2026-07-10; the old Magic » Spellbook menu entry is
// gone, and no name line — the pressed button says whose book).
// Selecting a member fills the box with THEIR KNOWN SYMBOLS
// as rune buttons, the sequence "spelled out" so far, the name of the spell
// that sequence resolves to (when a known recipe matches), and Cast / Clear.
// This is where the player BUILDS a spell: click symbols to append (an
// unavailable symbol draws a disabled overlay and stops responding — spent
// symbols never repeat, and the SCHOOL rule holds: the four element runes are
// mutually exclusive, one leads every spell, so the other three go dark once
// one is down and non-school symbols wait until one is), click a sequence
// slot to remove that symbol AND everything spelled after it, Cast fires
// onCast (the world gates vocabulary/mana) and clears the slate. The sequence
// row sits at the bottom, just above Cast / Clear. With no member selected
// the selector row tops the dim placeholder line. One persistent widget — no
// HUD rebuild on select; it re-resolves its member by roster index every
// frame (RosterMember), deselects one who went down, and drops sequence
// symbols the member no longer knows, so a roster reset can't dangle it.
} // namespace dungeon::game
