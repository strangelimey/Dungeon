// ============================================================================
// Game/ControlBar.h — the right-hand HUD panel: movement pad, hands, magic.
//
// The Dungeon Master control panel as a tree (docs/ui-hierarchy.md):
//
//   ControlBar          the framed panel; ContentRect is its padded interior
//     MovementPad       3x2 grid of turn/step buttons
//       Button x6
//     HandsArea         2x2 grid, one cell per party member
//       HandPair        that member's two hands
//         HandSlot x2
//     MagicArea         the "Magic" heading and the spellbook box
//       Label
//       SpellbookPanel
//
// Every bound is a fraction of its own parent, so the whole panel moves or
// resizes by setting ControlBar::bounds — none of the areas know where the
// panel sits, and GameUI no longer chains innerX / moveTop / handsTop /
// magicTop arithmetic to place them.
//
// The one size that still depends on content is the hand grid: a party of one
// or two fills a single row, so HandsArea is shorter and MagicArea starts
// higher. ControlBar takes the row count and works both out.
// ============================================================================
#pragma once

#include "Game/HandSlot.h"
#include "Game/Party.h"
#include "Game/PartyHudTypes.h"
#include "Game/SpellbookPanel.h"
#include "UI/Controls.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

// What the bar needs to build its parts. Grouped because it threads three
// levels down and a positional argument list that long is unreadable.
struct ControlBarDeps {
	const std::vector<Character>* roster = nullptr;
	const ItemIconBank* icons = nullptr;
	const gfx::Texture* chevron = nullptr;  // step/strafe face
	const gfx::Texture* chevron2 = nullptr; // turn face (double chevron)
	std::function<void(MoveAction)> onMove;
	std::function<void(size_t member, size_t hand)> onHandLeft;
	std::function<void(size_t member, size_t hand)> onHandRight;
	std::string magicLabel; // localized "Magic" heading
};

// 3x2 grid of movement buttons: turn-left / forward / turn-right over
// strafe-left / back / strafe-right. One chevron asset serves every direction
// (Button::iconTurns rotates it in quarter turns).
class MovementPad : public ui::Widget {
public:
	MovementPad(const gfx::Rect& rect, const ControlBarDeps& deps);
};

// One member's two hand boxes, side by side.
class HandPair : public ui::Widget {
public:
	HandPair(const gfx::Rect& rect, size_t member, const ControlBarDeps& deps);
};

// The hand grid: one HandPair per member, two per row.
class HandsArea : public ui::Widget {
public:
	HandsArea(const gfx::Rect& rect, const ControlBarDeps& deps);
};

// The magic box: the heading, then the spellbook filling the rest.
class MagicArea : public ui::Widget {
public:
	MagicArea(const gfx::Rect& rect, const ControlBarDeps& deps);

	SpellbookPanel* Spellbook() { return m_spellbook; }

private:
	// The book's framed background (the spellbook draws its contents over it).
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	SpellbookPanel* m_spellbook = nullptr;
	gfx::Rect m_bookBounds{}; // fractions of this area, for the frame
};

class ControlBar : public ui::Widget {
public:
	ControlBar(const gfx::Rect& rect, const ControlBarDeps& deps);

	SpellbookPanel* Spellbook() { return m_magic->Spellbook(); }

	// The padded interior every area resolves against.
	gfx::Rect ContentRect() const override;

private:
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	MagicArea* m_magic = nullptr;
};

} // namespace dungeon::game
