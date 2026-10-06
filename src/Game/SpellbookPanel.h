// ============================================================================
// Game/SpellbookPanel.h — the HUD Magic-area spellbook (member selector + rune sequence + Cast/Clear).
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "Game/Spells.h"
#include "UI/Controls.h"

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dungeon::game {

// One party slot's button in the spellbook's selector row: a face in that
// member's identity color, pressed while their book is open, washed out while
// they are absent, down, or know no symbols. It owns its own hover, so the
// panel tracks none.
class MemberButton : public ui::Widget {
public:
	MemberButton(size_t member, const std::vector<Character>* roster,
				 const int* selected, std::function<bool(size_t)> eligible,
				 std::function<void(size_t)> onSelect);

private:
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	size_t m_member;
	const std::vector<Character>* m_roster;
	const int* m_selected; // the panel's live selection
	std::function<bool(size_t)> m_eligible;
	std::function<void(size_t)> m_onSelect;
	bool m_hot = false;
};

// The selector row: one MemberButton per party slot, in even columns. This is
// the sketch's "character selection" (docs/ui-hierarchy.md). The symbol grid
// and the sequence/Cast/Clear below it stay the panel's own — see that doc for
// why they were left whole.
class MemberRow : public ui::Widget {
public:
	MemberRow(const gfx::Rect& rect, const std::vector<Character>* roster,
			  const int* selected, std::function<bool(size_t)> eligible,
			  std::function<void(size_t)> onSelect);
};

// The HUD Magic-area SPELLBOOK. A row of member-coloured SELECTOR buttons tops
// the box - one per party slot, disabled while that member is down or has NO
// memorized symbols, and no button at all for a slot a short party leaves
// empty; the selected one draws pressed (Michael, 2026-07-10; no name line -
// the pressed button says whose book). Selecting a member fills the box with
// THEIR KNOWN SYMBOLS as rune buttons, one row per tier (the schools always,
// then the forms and the modifiers they know), the sequence "spelled out" so
// far with, beside it, the spell it resolves to once this member has learned
// it, and Cast / Clear. This is where the player BUILDS a spell: click symbols
// to append - one that cannot follow the sequence (spent, or out of the
// school - form - modifier order, Spells.h SymbolMayFollow) draws dark and
// stops responding - click a sequence slot to remove that symbol AND
// everything spelled after it, and Cast fires onCast (the world gates
// vocabulary and mana), which empties the slate unless it asks to keep the
// spell. With no member selected the selector row tops the dim placeholder
// line. One persistent widget - no HUD rebuild on select; it re-resolves its
// member by roster index every frame (RosterMember), deselects one who went
// down, and drops sequence symbols the member no longer knows, so a roster
// reset cannot dangle it.
class SpellbookPanel : public ui::Widget {
public:
	SpellbookPanel(const gfx::Rect& rect, const std::vector<Character>* roster,
				   const ItemIconBank* icons);

	// Shows this member's book (fresh sequence) — the selector row's click.
	void SelectMember(size_t member);
	// The same, from outside the row (the dev `book` command): refuses, false,
	// for a member whose selector button is disabled.
	bool Open(size_t member) {
		if (!MemberEligible(member)) return false;
		SelectMember(member);
		return true;
	}
	void Close();
	bool IsOpen() const { return m_member >= 0; }

	// The harness's hands (the dev `book spell|cast|status`): lay runes on the
	// slate as the grid's clicks would (false, and nothing laid, if the member
	// does not know one or there are too many), press Cast exactly as the button
	// does, and read back what is built.
	bool SetSequence(std::span<const SpellSymbol> seq);
	void PressCast();
	size_t SequenceLength() const { return m_seqLen; }

	// Cast pressed: (member, the built sequence) — wired to the world's cast
	// façade. Fired only with a non-empty sequence. Returns true to KEEP the
	// built spell (the caster lacked the mana - Michael: report it, but leave
	// the spell to cast again); otherwise the slate empties.
	std::function<bool(size_t, std::span<const SpellSymbol>)> onCast;
	// The spell registry, for the live "= <spell>" match label (GameUI's
	// spellDefs source). Null-safe: no registry, no label.
	std::function<std::span<const std::unique_ptr<Spell>>()> spells;
	std::function<void()> onClick; // UI click feedback
	// Cast / Clear wear these glyphs on their faces, their words moving to the
	// tooltip; a null glyph leaves that button its word.
	void SetActionIcons(const gfx::Texture* cast, const gfx::Texture* clear);

	void LayoutSelf(ui::UIContext& ctx) override;
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
	// The hovered rune's name (DrawRuneTip), over everything.
	void DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	// The most symbols a built sequence holds: the recipe grammar's longest
	// (school, form, modifier - Spells.h).
	static constexpr size_t kMaxSequence = kMaxRecipe;

	// One cell of the rune-button grid, ONE ROW PER TIER: the four SCHOOL runes
	// always hold the top row (schools-table order, drawn as empty frames until
	// memorized); the forms the member knows take the next row and the
	// modifiers the one after, each row appearing once it has a rune in it.
	struct RuneSlot {
		SpellSymbol symbol;
		bool known;
		u8 row = 0, col = 0;
	};
	// A member's grid, INLINE. The panel rebuilds it in every Update and Draw
	// while a book is open - every settled frame the steady-state allocation
	// guard watches - so it is a fixed array rather than a returned vector.
	// Each symbol appears at most once, so kSymbolCount cells always suffice.
	struct RuneSlotList {
		std::array<RuneSlot, kSymbolCount> slot{};
		size_t count = 0;
		size_t rows = 0;
		std::span<const RuneSlot> View() const { return {slot.data(), count}; }
	};
	RuneSlotList RuneSlots(const Character& c) const;
	// The sequence spelled so far (inline for the same reason: a rune click
	// lands in a guarded frame).
	std::span<const SpellSymbol> Sequence() const { return {m_sequence.data(), m_seqLen}; }
	// Whether party slot i's selector button responds: the member exists and
	// is standing (absent / unconscious / dead all disable).
	bool MemberEligible(size_t i) const;
	// Layout inside the live box, shared by Update (hit-test) and Draw. A
	// COLUMN is the full-size cell a column holds (the member buttons stand on
	// them); a rune's cell is its slot's row and column, shrunk when the grid's
	// rows would otherwise run into the spell name's line, and centred in its
	// column. The sequence row indexes m_sequence.
	gfx::Rect ColumnRect(const gfx::Rect& px, size_t col) const;
	gfx::Rect SymbolRect(const gfx::Rect& px, const RuneSlot& slot, size_t rows) const;
	gfx::Rect SequenceRect(const gfx::Rect& px, size_t i) const;
	gfx::Rect CastRect(const gfx::Rect& px) const;
	gfx::Rect ClearRect(const gfx::Rect& px) const;
	// The height every VERTICAL measure is a fraction of: the panel's own, but
	// never more than kRefAspect of its width. A Magic dock stretched to fill a
	// column (Movement minimized) keeps its rows and buttons the size they
	// were tuned at; only the gap between the grid and the sequence grows
	// (Michael: Cast / Clear "shouldn't scale the same").
	static float RefH(const gfx::Rect& px);
	// The spell the sequence spells out, if any.
	const Spell* Match() const;
	// (Runes draw through PartyHudDraw's DrawRuneGlow, shared with the hand
	// boxes and every other rune control.)

	const std::vector<Character>* m_roster;
	const ItemIconBank* m_icons;
	int m_member = -1;  // roster slot whose book is open (-1 = none selected)
	std::array<SpellSymbol, kMaxSequence> m_sequence{};
	size_t m_seqLen = 0; // how much of m_sequence is spelled
	int m_hotSymbol = -1, m_hotSeq = -1;
	// The rune under the pointer for its TOOLTIP - any known rune, grid or
	// sequence, including a spent or out-of-turn one (which is not hot, but
	// still shows a glyph to name). m_tipSymbol < 0 = none.
	int m_tipSymbol = -1;
	gfx::Rect m_tipRect{};
	// Cast and Clear are ordinary ui::Buttons (ui-updates: they were two hand-
	// drawn icon discs) - the settings tabs' stone face and the shared push
	// animation, laid out each frame over CastRect / ClearRect.
	ui::Button* m_castButton = nullptr;
	ui::Button* m_clearButton = nullptr;
	ui::Widget* m_memberRow = nullptr; // placed each layout (RefH)
	std::string m_placeholder, m_castLabel, m_clearLabel; // localized once
};

} // namespace dungeon::game
