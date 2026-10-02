// ============================================================================
// Game/PartyWindow.h - the whole party at once: one CARD per member, all on
// the same tab (more-ui-updates Phase 5; it replaced InventoryWindow).
//
// The sheet's "All" button used to open the party's BACKPACKS whatever tab the
// sheet was on (Michael: "it should bring up whichever tab you were viewing. if
// you were viewing stats, it brings up each characters stats"). This window
// opens on the sheet's tab and has its own row of the five cut-stone tabs;
// switching one switches all four cards.
//
// A card is a CharacterSheet in card mode (CharacterSheet::IsCard): the sheet's
// own code, at card size, so a stat or a skill row cannot read differently
// here and on the sheet. All four are built and warmed with the window - a
// click that opens it lands in a guarded frame, and nothing is added then.
//
// Like the window it replaced it FLOATS over the running game in the HUD's
// floating layer (the `inventory` panel slot - its spot, scale and opacity
// carried over): no dim, the world clickable around it, closed by its corner
// box or Esc. A status line along its foot names what the pointer is over,
// as the sheet's does.
// ============================================================================
#pragma once

#include "Game/CharacterSheet.h"
#include "UI/Controls.h"

#include <array>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace dungeon::ui {
class FloatingPanel;
}

namespace dungeon::game {

class PartyWindow : public ui::Widget {
public:
	static constexpr size_t kMaxCards = 4;
	// A card's em against the window's (the HUD's text size at the panel's
	// scale): the sheet's tab at about 0.6 of the sheet's size (0.55 made its
	// body text too small to read).
	static constexpr float kCardScale = 0.8f;
	// The chrome, in CARD em.
	static constexpr float kPadEm = 0.8f;
	static constexpr float kTabEm = 3.0f;     // one tab stone, square
	// ModeSelector splits its strip by the SHEET's gap ratio (kModeBtnGap /
	// kModeBtnW = 0.006 / 0.054), so this keeps the stones square.
	static constexpr float kTabGapEm = kTabEm * 0.006f / 0.054f;
	static constexpr float kGapEm = 0.6f;     // tab row to cards, card to card
	static constexpr float kStatusEm = 2.2f;  // the status line's band

	// The window's size for a window em of `em` pixels on any tab but
	// Inventory - what its floating panel asks for.
	static Vec2 SizeForEm(float em);
	// Its size on tab `mode` at panel scale `s` (window em `em`): the Inventory
	// tab's cards are sized by their squares, which are the SHEET'S (Phase 6),
	// and by the most rows any shown member's bag needs - so the window grows
	// on that tab and shrinks back after.
	Vec2 PanelSize(ui::UIContext& ctx, float s, float em, CharacterSheet::Mode mode) const;

	// The sheet's root text size, design pixels (UIContext::DesignHeight of its
	// context): on the Inventory tab the cards take it, times the panel's
	// scale, so their squares match the sheet's. Unset = the cards' usual size.
	std::function<float()> squareDesign;

	// `panel` is the floating panel it fills, whose scale the cards' text
	// follows. `closeIcon` is the shared corner box.
	PartyWindow(const ui::FloatingPanel* panel, std::vector<Character>* roster,
				const ResourceBarStyle* barStyle, const ItemIconBank* icons,
				const ItemWeightBank* weights, const ItemIconBank* slotIcons,
				const ItemCategoryBank* categories, HeldItem* held,
				const gfx::Texture* closeIcon, std::function<void()> onClose);

	// Opens on `mode` with every card re-pointed at its member (the bakes the
	// sheet does on open - allocation-free once warmed).
	void Open(CharacterSheet::Mode mode);
	void Close() { m_open = false; }
	bool IsOpen() const { return m_open; }
	CharacterSheet::Mode CurrentMode() const { return static_cast<CharacterSheet::Mode>(m_modeIndex); }
	// The tab stones' etched symbols and lit twins, in Mode order.
	void SetModeEtches(std::span<const gfx::Texture* const> etch,
					   std::span<const gfx::Texture* const> lit);

	// The cards, for GameUI to wire (item buttons, rejections, the defense
	// readouts) - one per roster slot up to four, null past that.
	CharacterSheet* Card(size_t i) const { return i < m_cards.size() ? m_cards[i] : nullptr; }

	// Opens so far, and tab stone `i`'s rect - for tools\AllocTest.ps1 -All,
	// which opens the window and clicks every tab inside guarded frames.
	unsigned Opens() const { return m_opens; }
	gfx::Rect StoneRect(size_t i) const { return m_strip ? m_strip->ButtonRect(i) : gfx::Rect{}; }

	// What the status line says this frame (empty = nothing).
	std::string_view StatusName() const;
	std::string_view StatusText() const;

	// The background's opacity, read live (Settings -> UI). Null = opaque.
	const float* opacity = nullptr;

private:
	void LayoutSelf(ui::UIContext& ctx) override;
	// Notes the pointer for the status line; claims nothing (the cards and the
	// stones have had their look, and the window's own background is its
	// floating panel's).
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
	// The card whose status the line shows: the one under the pointer, else
	// the first with something to say (an item riding the cursor).
	const CharacterSheet* StatusCard() const;
	// Rows of contents the Inventory tab needs: the most any shown member's
	// selected bag fills, six across, at least one.
	int InventoryRows() const;
	float CardEm() const { return Em(kCardScale); }

	const ui::FloatingPanel* m_panel;
	std::vector<Character>* m_roster;
	bool m_open = false;
	unsigned m_opens = 0;
	int m_modeIndex = 0;
	ModeSelector* m_strip = nullptr;
	ui::Widget* m_closeSlot = nullptr;
	std::array<CharacterSheet*, kMaxCards> m_cards{};
	float m_mouseX = -1.0f, m_mouseY = -1.0f; // the pointer at the last update
};

} // namespace dungeon::game
