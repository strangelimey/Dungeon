// ============================================================================
// Game/ControlBar.h - the right-hand HUD column: movement, hands, magic.
//
// THREE PANELS, NOT ONE (play-test #6-#8, Michael 2026-09-28). The Dungeon
// Master control panel was one framed box holding all three parts; it is now a
// frameless column of three framed docks, one per part:
//
//   ControlBar          the column; lays the docks out, draws nothing itself
//     HudDock "move"    header (title + minimize) over the MovementPad
//       MovementPad     3x2 grid of turn/step buttons
//     HudDock "hands"   no header; the hand grid alone
//       HandsArea       2x2 grid, one cell per party member
//         HandPair      that member's two hands
//     HudDock "magic"   header (title + minimize) over the spellbook
//       SpellbookPanel
//
// His rules for the three, each of which is a line of LayoutSelf:
//   - Movement and Magic can each be MINIMIZED to their header strip, and start
//     expanded. The flags are the player's (settings.ini hud_move_collapsed /
//     hud_magic_collapsed), so ControlBarDeps carries pointers to them.
//   - Minimizing a panel DOES NOT MOVE THE OTHERS. Every dock is placed as if
//     all were expanded, and a minimized one only draws shorter. That is also
//     the first half of "later, each panel resizable and movable with the
//     mouse": no dock's position is derived from another's CURRENT size.
//   - Magic is not shown at all until some member KNOWS A SYMBOL (it appears the
//     moment one is learned). Derived every layout from the roster, never
//     latched, so a load or a roster change is right with no notification.
//
// Every bound is a fraction of its own parent, so the whole column moves or
// resizes by setting ControlBar::bounds. The one size that depends on content
// is the hand grid: a party of one or two fills a single row.
// ============================================================================
#pragma once

#include "Game/HandSlot.h"
#include "Game/Party.h"
#include "Game/PartyHudTypes.h"
#include "Game/SpellbookPanel.h"
#include "UI/Controls.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

// What the column needs to build its parts. Grouped because it threads three
// levels down and a positional argument list that long is unreadable.
struct ControlBarDeps {
	const std::vector<Character>* roster = nullptr;
	const ItemIconBank* icons = nullptr;
	const gfx::Texture* chevron = nullptr;  // step/strafe face
	const gfx::Texture* chevron2 = nullptr; // turn face (double chevron)
	std::function<void(MoveAction)> onMove;
	std::function<void(size_t member, size_t hand)> onHandLeft;
	std::function<void(size_t member, size_t hand)> onHandRight;
	// What a hand is SET to (HandSlot::setUse), asked every frame. Must not
	// allocate.
	std::function<HandSetUse(size_t member, size_t hand)> handSetUse;
	// Hand-use pictures by verb (ui/use_<verb>.png), for an empty set hand.
	const ItemIconBank* useIcons = nullptr;
	// The offense/defense stance slider under a member's hands: the widget
	// mutates nothing itself, it reports where it was dragged to.
	std::function<void(size_t member, float share)> onGuardChange;
	// The live Balance::exertMax - the share full over-exertion means. Asked
	// every frame, since the Balance dialog edits it live.
	std::function<float()> exertMax;
	std::string moveLabel;  // localized "Movement" heading
	std::string magicLabel; // localized "Magic" heading
	// The two minimize flags (GameSettings), and who to tell when a click flips
	// one (GameUI saves the settings). Null = that dock cannot be minimized.
	bool* moveCollapsed = nullptr;
	bool* magicCollapsed = nullptr;
	std::function<void()> onCollapseChanged;
};

// 3x2 grid of movement buttons: turn-left / forward / turn-right over
// strafe-left / back / strafe-right. One chevron asset serves every direction
// (Button::iconTurns rotates it in quarter turns).
class MovementPad : public ui::Widget {
public:
	MovementPad(const gfx::Rect& rect, const ControlBarDeps& deps);

	// Square cells sized from the WIDTH, and the height that follows. Asked by
	// ControlBar before it lays the docks out, for the same reason HandPair is.
	static float CellSide(float widthPx, float emPx);
	static float NeededHeight(float widthPx, float emPx);

private:
	void LayoutSelf(ui::UIContext& ctx) override;
};

// One member's two hand boxes side by side, with the stance slider spanning
// the full width beneath BOTH of them - one decision for the character, not
// one per hand.
class HandPair : public ui::Widget {
public:
	HandPair(const gfx::Rect& rect, size_t member, const ControlBarDeps& deps);

	// The height a pair of this width needs, and the pieces it is made of.
	// ControlBar asks BEFORE laying out, because the hand grid's height is a
	// consequence of the column's width and nothing else can know that.
	static float NeededHeight(float widthPx, float emPx);
	static float SquareSide(float widthPx, float emPx);
	static float BandHeight(float emPx);

private:
	// The boxes are SQUARE, and squareness cannot be authored: `bounds` are
	// fractions of the parent in each axis independently, so a w/h pair only
	// comes out square when the parent's own pixel aspect happens to agree.
	// The side is therefore COMPUTED here, once the pixel rect is known, which
	// is what LayoutSelf is for (docs/ui-hierarchy.md: bounds may be derived
	// when a child is aspect-locked).
	void LayoutSelf(ui::UIContext& ctx) override;

	ui::Widget* m_slots[2]{nullptr, nullptr};
	ui::Widget* m_guard = nullptr;
};

// The hand grid: one HandPair per member, two per row.
class HandsArea : public ui::Widget {
public:
	HandsArea(const gfx::Rect& rect, const ControlBarDeps& deps);

	// The height `rows` of pairs need at this width, gaps between them
	// included. ControlBar asks before laying the docks out.
	static float NeededHeight(float widthPx, float emPx, size_t rows);

private:
	void LayoutSelf(ui::UIContext& ctx) override;
};

// One framed panel of the column: an optional HEADER (title, and a minimize
// button when it has a flag to flip) over one content widget. The column sets
// its bounds; the dock lays out its own header and content inside its padding.
// Minimized, the content is hidden and the dock is only as tall as its header
// - the column decides that height too, so the dock just follows the flag.
class HudDock : public ui::Widget {
public:
	// `title` empty = no header. `collapsed` null = cannot be minimized.
	HudDock(std::string title, bool* collapsed, std::function<void()> onCollapseChanged);

	// The content widget, added by the owner after construction so its bounds
	// resolve against this dock.
	template <typename T, typename... Args> T* SetContent(Args&&... args) {
		T* w = Add<T>(std::forward<Args>(args)...);
		m_content = w;
		return w;
	}

	bool Collapsed() const { return m_collapsed && *m_collapsed; }
	bool HasHeader() const { return m_title != nullptr; }
	// The padding, header height and header-to-content gap, in pixels - asked by
	// the column before it places anything. Padding follows the WIDTH (it is
	// what the old single panel used, so the hand boxes keep their size).
	static float Pad(float widthPx);
	static float HeaderHeight(float emPx);
	static float HeaderGap(float emPx);

	gfx::Rect ContentRect() const override; // the padded interior

private:
	void LayoutSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	ui::Label* m_title = nullptr;
	ui::Button* m_toggle = nullptr;
	ui::Widget* m_content = nullptr;
	bool* m_collapsed = nullptr;
};

class ControlBar : public ui::Widget {
public:
	ControlBar(const gfx::Rect& rect, const ControlBarDeps& deps);

	SpellbookPanel* Spellbook() { return m_spellbook; }

private:
	// Places the three docks, in PIXELS, then converts to fractions. The hand
	// grid's height is DERIVED from the width (square boxes); Magic takes what
	// is left below it. Positions are always the EXPANDED ones (see the header).
	void LayoutSelf(ui::UIContext& ctx) override;

	const std::vector<Character>* m_roster = nullptr;
	HudDock* m_moveDock = nullptr;
	HudDock* m_handsDock = nullptr;
	HudDock* m_magicDock = nullptr;
	SpellbookPanel* m_spellbook = nullptr;
	size_t m_rows = 1;
};

} // namespace dungeon::game
