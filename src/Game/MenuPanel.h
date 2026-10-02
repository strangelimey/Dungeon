// ============================================================================
// Game/MenuPanel.h - the stone card the pause and title menus stand on.
//
// more-ui-updates Phase 4 (Michael: "the pause menu needs a lot of work. We'll
// need the same cut, bevelled stone"). The entries were bare text over the
// dimmed scene; now they are cut-stone blocks with their words carved in
// (ui::MenuList, skinned), on a stone PANEL, with the pause menu's title on the
// panel rather than floating above it.
//
// The card is sized in REM from what it holds - a title band, the entries, the
// longest label - so it is the same shape at any window size and in any
// language. It is computed in LayoutSelf, which runs before the children are
// laid out, and the list's bounds are written there too: the widget covers the
// whole window and draws its card inside, so the card and the list land on the
// SAME frame they are measured (a widget that resized its own bounds would show
// one wrong frame each time the menu opened).
// ============================================================================
#pragma once

#include "UI/Controls.h"

#include <string>

namespace dungeon::game {

class MenuPanel : public ui::Widget {
public:
	// `entries` is how many the list will hold (it is filled by the caller,
	// through List()). `title` empty = no title band. `top` is the card's top
	// as a window fraction; < 0 centres it.
	MenuPanel(std::string title, size_t entries, float top);

	ui::MenuList* List() const { return m_list; }

	// The card, not the whole window, is what the panel paints.
	gfx::Rect InkRect() const override { return Card(); }

	// The card's measures, in rem.
	static constexpr float kEntryRem = 1.9f; // one stone's height
	static constexpr float kGapRem = 0.4f;   // between stones
	static constexpr float kMinWidthRem = 11.0f;
	static constexpr float kPadRem = 1.0f;
	static constexpr float kTitleRem = 2.8f; // the title band
	static constexpr float kTitleFontRem = 1.9f;

private:
	void LayoutSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
	gfx::Rect Card() const;

	std::string m_title;
	size_t m_entries;
	float m_top;
	ui::MenuList* m_list = nullptr;
	float m_labelW = 0.0f; // the longest entry, measured at layout
	const ui::Font* m_titleFont = nullptr;
};

// The same card for a menu PAGE with content of its own - the save, load and
// world pages (Michael: "the 'world selection' menu ... needs the same
// treatment. Oh, and the load and save screens, too"). The card is the
// widget's bounds (window fractions); its title is carved in a band at the
// top, and its CHILDREN - the page's rows, normally one ui::Stack - resolve
// against what is left inside the padding, so nothing on the page is placed
// by hand against the card.
class PageCard : public ui::Widget {
public:
	PageCard(const gfx::Rect& rect, std::string title);
	gfx::Rect ContentRect() const override;

private:
	void LayoutSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	std::string m_title;
	const ui::Font* m_titleFont = nullptr;
};

// The title carved across the top of a card, centred in its band. Shared by
// both cards so they cannot drift apart.
void DrawCardTitle(ui::UIContext& ctx, gfx::SpriteBatch& batch, const ui::Font& font,
				   const std::string& title, const gfx::Rect& card, float pad, float band);

} // namespace dungeon::game
