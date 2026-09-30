// ============================================================================
// Game/HandSlot.h — one hand box of the HUD control panel (left/right, Dungeon Master style).
//
// A hand with a SET use shows it: the socket behind the item takes an accent
// tint, and a spell use also spells out its runes (Michael, 2026-09-28 - the
// runes are the first iteration; a per-spell icon may replace them), and an
// empty hand set to a verb with a picture (punch, kick) shows it. Hovering
// a set hand names the use in a tooltip; an unset hand shows none.
// ============================================================================
#pragma once

#include "Core/Loc.h"
#include "Game/PartyHudTypes.h"
#include "UI/Controls.h"

#include <functional>
#include <vector>

namespace dungeon::game {

class Spell; // Spell/Spell.h

// What a hand box shows about the hand's SET use - the one the player picked
// from its menu (GameUI::SetUseFor), never the item's own first command, which
// a left click performs on an unset hand without setting anything.
struct HandSetUse {
	bool set = false;             // a use is set for what this hand holds
	const Spell* spell = nullptr; // that use casts this spell (else a verb)
	loc::Line label;              // its display name - the hover tooltip
	// The set verb ("punch", "slash"; empty for a spell), for its picture -
	// ui/use_<verb>.png, drawn when the hand holds nothing. A view into the
	// hand's UseDefaults, valid for the frame that asked.
	std::string_view verb;
};

class HandSlot : public ui::Widget {
public:
	// `hand` is 0 = left / 1 = right (which inventory.Hand() this box shows).
	// `icons` (Game-owned, may be null) resolves the held item's icon to draw.
	// onLeft fires on a left click, onRight on a right click, onMiddle on a
	// middle click - GameUI decides what each means (place / swap / attack on
	// the left, the item's details on the right, its use menu on the middle).
	HandSlot(const gfx::Rect& rect, const std::vector<Character>* roster,
			 size_t member, int hand,
			 const ItemIconBank* icons, std::function<void()> onLeft,
			 std::function<void()> onRight, std::function<void()> onMiddle);

	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
	// The hover tooltip, in the overlay pass so nothing paints over it.
	void DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// The hand's set use, asked every frame (so a pick, a Clear, a swapped item
	// or a load is right with no notification). Unwired = never shown as set.
	// Must not allocate: the HUD draws in every guarded frame.
	std::function<HandSetUse()> setUse;
	// Hand-use pictures by verb (Game-owned, ui/use_<verb>.png), for an empty
	// hand set to that verb. Null, or no file for the verb = the tint alone.
	const ItemIconBank* useIcons = nullptr;

private:
	// A spell's recipe, drawn as rune faces inside `area`: a grid filling it
	// when the hand is empty, a strip along its bottom when an item is shown.
	void DrawSpellRunes(gfx::SpriteBatch& batch, const gfx::Rect& area,
						const Spell& spell, bool overItem) const;

	const std::vector<Character>* m_roster;
	size_t m_member;
	// Re-resolved every Update/Draw (see CharacterPanel).
	const Character* m_character = nullptr;
	int m_hand;
	const ItemIconBank* m_icons;
	std::function<void()> m_onLeft;
	std::function<void()> m_onRight;
	std::function<void()> m_onMiddle;
	bool m_hot = false;
	bool m_held = false;        // left-button press latched on this slot
	bool m_heldRight = false;   // right-button press latched on this slot
	bool m_heldMiddle = false;  // middle-button press latched on this slot
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
