// ============================================================================
// Game/SpellbookPanel.h — the HUD Magic-area spellbook (member selector + rune sequence + Cast/Clear).
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "Game/Spells.h"
#include "UI/Controls.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dungeon::game {

class SpellbookPanel : public ui::Widget {
public:
	SpellbookPanel(const gfx::Rect& rect, const std::vector<Character>* roster,
				   const ItemIconBank* icons);

	// Shows this member's book (fresh sequence) — the selector row's click.
	void SelectMember(size_t member);
	void Close();
	bool IsOpen() const { return m_member >= 0; }

	// Cast pressed: (member, the built sequence) — wired to the world's cast
	// façade. Fired only with a non-empty sequence.
	std::function<void(size_t, const std::vector<SpellSymbol>&)> onCast;
	// The spell registry, for the live "= <spell>" match label (GameUI's
	// spellDefs source). Null-safe: no registry, no label.
	std::function<std::span<const std::unique_ptr<Spell>>()> spells;
	std::function<void()> onClick; // UI click feedback
	// Optional icon faces for Cast/Clear (assets/ui/icon_cast / icon_clear —
	// complete round buttons with alpha, from the Wenrexa UI pack). Null =
	// the localized text buttons (the fallback keeps the lang keys alive).
	const gfx::Texture* castIcon = nullptr;
	const gfx::Texture* clearIcon = nullptr;

	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	// One cell of the rune-button grid: the four SCHOOL runes always hold the
	// TOP ROW (schools-table order, drawn as empty frames until memorized);
	// other runes appear in the rows below as the member learns them.
	struct RuneSlot {
		SpellSymbol symbol;
		bool known;
	};
	std::vector<RuneSlot> RuneSlots(const Character& c) const;
	// Whether party slot i's selector button responds: the member exists and
	// is standing (absent / unconscious / dead all disable).
	bool MemberEligible(size_t i) const;
	// Layout inside the live box, shared by Update (hit-test) and Draw. The
	// selector row indexes party slots 0-3; the symbol grid indexes RuneSlots
	// (4 per row); the sequence row indexes m_sequence.
	gfx::Rect MemberButtonRect(const gfx::Rect& px, size_t i) const;
	gfx::Rect SymbolRect(const gfx::Rect& px, size_t i) const;
	gfx::Rect SequenceRect(const gfx::Rect& px, size_t i) const;
	gfx::Rect CastRect(const gfx::Rect& px) const;
	gfx::Rect ClearRect(const gfx::Rect& px) const;
	// The spell the sequence spells out, if any.
	const Spell* Match() const;
	// Draws one rune face: the rune-item icon when loaded, else an
	// element-tinted fallback square; element-coloured border. Disabled (the
	// symbol is already in the sequence) washes it out under a dark overlay.
	void DrawRune(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  bool hot, bool disabled = false) const;

	const std::vector<Character>* m_roster;
	const ItemIconBank* m_icons;
	int m_member = -1;  // roster slot whose book is open (-1 = none selected)
	std::vector<SpellSymbol> m_sequence;
	int m_hotSymbol = -1, m_hotSeq = -1, m_hotMember = -1;
	bool m_hotCast = false, m_hotClear = false;
	std::string m_placeholder, m_castLabel, m_clearLabel; // localized once
};

// The COMBINED party inventory: a centered panel with one backpack column per
// member, for swapping items between characters at a glance. Opened by the
// sheet's "All" button or by right-clicking the world while carrying a tablet;
// non-modal (the world keeps running) but claims the mouse like the map overlay.
// Click a slot to drop the held tablet in (swapping any occupant onto the
// cursor) or, empty-handed, to pick the slot's item up; click off the panel —
// or Esc (handled by Game) — closes it. Overlay-drawn so it floats above the
// HUD; the held-cursor icon (drawn last) stays on top.
} // namespace dungeon::game
