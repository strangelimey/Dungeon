// ============================================================================
// Game/MemberCards.h - the MINIMAL layout's party cards (ui-panels P4).
//
// Michael: "I like the way the Grimrock UI combines the party bar with the hand
// controls. We need to add a UI setting called 'minimal' that does this." In
// the Minimal layout the party bar and the Hands dock give way to ONE CARD PER
// MEMBER - the party bar's slot (portrait, name, effects, bars) over that
// member's two hand boxes and stance slider - and the cards make one floating
// block, two across in formation order (Brand front-L, Sera front-R, Maren
// rear-L, Tilo rear-R), as the Hands dock lays its pairs out.
//
// BUILT BY REUSE, not copies: a card holds a CharacterPanel and a HandPair, the
// very widgets the Standard layout puts in the party bar and the Hands dock, so
// clicks, hand uses, tooltips and the stance all behave the same in both. The
// card draws the one stone face behind them (the CharacterPanel's own is faded
// out) and places the two pieces; the grid places the cards.
//
//   FloatingPanel "cards"
//     CardGrid          two per row, gaps in em
//       MemberCard      one member: face + the two pieces
//         CharacterPanel
//         HandPair
//
// Sizes are the content's, like every floating panel: a card's height follows
// from its width (the portrait band is a share of it, the hand boxes are
// square), and everything else is in em, so the block scales with its panel.
// ============================================================================
#pragma once

#include "UI/Widget.h"

#include <vector>

namespace dungeon::game {

struct Character;

// One member's card. Its two pieces are added by the owner (they take
// GameUI's callbacks) and handed over with SetPieces.
class MemberCard : public ui::Widget {
public:
	MemberCard(const std::vector<Character>* roster, size_t member, const float* opacity);

	void SetPieces(ui::Widget* panel, ui::Widget* hands) {
		m_panel = panel;
		m_hands = hands;
	}

	// A card of `widthPx`, its text at `emPx`: the whole card's height.
	static float Height(float widthPx, float emPx);

private:
	void LayoutSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const std::vector<Character>* m_roster;
	size_t m_member;
	const float* m_opacity;
	ui::Widget* m_panel = nullptr;
	ui::Widget* m_hands = nullptr;
};

// The cards, two per row: a party of one or two fills a single row.
class CardGrid : public ui::Widget {
public:
	CardGrid() { debugName = "CardGrid"; }

	// The block's height at `widthPx` for `members` cards.
	static float Height(float widthPx, float emPx, size_t members);
	// One card's width in a block of `widthPx`.
	static float CardWidth(float widthPx, float emPx);

private:
	void LayoutSelf(ui::UIContext& ctx) override;
};

} // namespace dungeon::game
