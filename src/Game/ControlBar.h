// ============================================================================
// Game/ControlBar.h - the HUD's movement, hands and magic docks.
//
// THREE PANELS, NOT ONE (play-test #6-#8, Michael 2026-09-28). The Dungeon
// Master control panel was one framed box holding all three parts; it became
// a column of three framed docks, and now (ui-panels P3a) three FLOATING
// panels the player moves and resizes on their own (UI/FloatingPanel.h):
//
//   FloatingPanel "move"   placement + grips
//     HudDock              header (title + minimize) over the MovementPad
//       MovementPad        3x2 grid of turn/step buttons
//   FloatingPanel "hands"
//     HudDock              no header; the hand grid alone
//       HandsArea          2x2 grid, one cell per party member
//         HandPair         that member's two hands
//   FloatingPanel "magic"
//     HudDock              header (title + minimize) over the spellbook
//       SpellbookPanel
//
// His rules for the three:
//   - Movement and Magic each have a MINIMIZE button in their header, and start
//     shown. Minimized, a dock goes away entirely and a button in the HUD's
//     closed-panels tray brings it back (ui-updates Phase 8, Game/HudTray.h; it
//     used to shrink to its header strip). The flag is the panel's own
//     HudPanelLook::hidden; the Hands dock minimizes too, by its Ctrl button.
//   - Minimizing a dock CLOSES UP THE COLUMN (Michael, ui-updates: with
//     Movement closed, "the hands panel moves up and the magic panel expands
//     vertically to fill the rest"): a dock's DEFAULT spot stacks under the
//     SHOWN docks above it, and Magic's default height is what they leave. Only
//     docks still on their default spots move - one the player placed stays.
//   - Magic is not shown at all until some member KNOWS A SYMBOL (it appears the
//     moment one is learned). Derived every layout from the roster, never
//     latched, so a load or a roster change is right with no notification.
//
// A dock's SIZE is its content's: square cells and boxes sized from the
// width, so the height follows (the size functions below). The one size that
// depends on the party is the hand grid: a party of one or two fills one row.
// ============================================================================
#pragma once

#include "Game/HandSlot.h"
#include "Game/Party.h"
#include "Game/PartyHudTypes.h"
#include "Game/SpellbookPanel.h"
#include "UI/Controls.h"
#include "UI/FloatingPanel.h"

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
	// The docks' minimize button: the square "-" box (null = text) and its
	// tooltip.
	const gfx::Texture* boxMinus = nullptr;
	std::string minimizeTip;
	std::function<void(MoveAction)> onMove;
	std::function<void(size_t member, size_t hand)> onHandLeft;
	std::function<void(size_t member, size_t hand)> onHandRight;  // details
	std::function<void(size_t member, size_t hand)> onHandMiddle; // use menu
	// What a hand is SET to (HandSlot::setUse), asked every frame. Must not
	// allocate.
	std::function<HandSetUse(size_t member, size_t hand)> handSetUse;
	// Hand-use pictures by verb (ui/use_<verb>.png), for an empty set hand.
	const ItemIconBank* useIcons = nullptr;
	// The soft radial glow a SET hand box draws behind its contents (null = the
	// flat tint alone).
	const gfx::Texture* glow = nullptr;
	// The offense/defense stance slider under a member's hands: the widget
	// mutates nothing itself, it reports where it was dragged to.
	std::function<void(size_t member, float share)> onGuardChange;
	// The live Balance::exertMax - the share full over-exertion means. Asked
	// every frame, since the Balance dialog edits it live.
	std::function<float()> exertMax;
	std::string moveLabel;  // localized "Movement" heading
	std::string magicLabel; // localized "Magic" heading
	// Who to tell when a dock's header button minimizes it (GameUI saves the
	// settings; the flag is the dock's HudPanelLook::hidden).
	std::function<void()> onHideChanged;
	// Each dock's placement, scale, background opacity and minimized flag
	// (GameSettings, Settings -> UI and the panel grips), read live every
	// layout and draw.
	HudPanelLook* moveLook = nullptr;
	HudPanelLook* handsLook = nullptr;
	HudPanelLook* magicLook = nullptr;
	// Where the docks' DEFAULT column sits: its width and right margin as
	// window fractions (at scale 1), and its top and bottom in pixels (asked
	// every layout - the top follows the party bar's default height).
	float columnW = 0.156f;
	float columnMargin = 0.01f;
	std::function<float(ui::UIContext&)> columnTop;
	std::function<float(ui::UIContext&)> columnBottom;
	// Settings -> UI "Lock HUD layout", and who to tell when a drag ends.
	const bool* locked = nullptr;
	std::function<void()> onPlacementChanged;
	// THE MINIMAL LAYOUT (Game/MemberCards.h): no Hands dock - the hands ride
	// the party cards - and the Magic dock's default spot and its height at
	// scale 1 (pixels) come from the owner, since the column under Movement now
	// holds the cards. Null = the Standard column's rules.
	bool withHands = true;
	std::function<Vec2(ui::UIContext&)> magicDefaultPos;
	std::function<float(ui::UIContext&)> magicHeight1;
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
// one per hand. The three are framed TOGETHER by one border in the member's
// identity colour (ui-updates: it replaced a colour stripe inside each box), so
// whose hands these are reads as a property of the group.
class HandPair : public ui::Widget {
public:
	HandPair(const gfx::Rect& rect, size_t member, const ControlBarDeps& deps);

	// The height a pair of this width needs, and the pieces it is made of.
	// ControlBar asks BEFORE laying out, because the hand grid's height is a
	// consequence of the column's width and nothing else can know that.
	static float NeededHeight(float widthPx, float emPx);
	static float SquareSide(float widthPx, float emPx);
	static float BandHeight(float emPx);
	// The padding on EVERY side that holds the member border and its glow, so
	// the frame stays inside the pair's own area (the bar frames' rule).
	static float FramePad(float emPx);

private:
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
	// The boxes are SQUARE, and squareness cannot be authored: `bounds` are
	// fractions of the parent in each axis independently, so a w/h pair only
	// comes out square when the parent's own pixel aspect happens to agree.
	// The side is therefore COMPUTED here, once the pixel rect is known, which
	// is what LayoutSelf is for (docs/ui-hierarchy.md: bounds may be derived
	// when a child is aspect-locked).
	void LayoutSelf(ui::UIContext& ctx) override;

	const std::vector<Character>* m_roster = nullptr;
	size_t m_member = 0;
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

// One framed dock: an optional HEADER (title, and a minimize button when it has
// a flag to set) over one content widget. It fills its FloatingPanel and lays
// out its own header and content inside its padding. Minimizing sets the
// panel's hidden flag; the panel is then not laid out or drawn at all, and the
// HUD's tray offers it back.
//
// SCALED through the inherited fontScale: the floating layer sets it to the
// panel's scale, and everything inside a dock measures its detail in EM rather
// than rem, so the boxes, the gaps and the text grow together. (Rem is the
// HUD's grid and does not move with fontScale - by design - which is why the
// widgets in here use Em.)
class HudDock : public ui::Widget {
public:
	// `title` empty = no header. `hidden` null = no minimize button (a headerless
	// dock minimizes through its panel's Ctrl button instead). `look` null =
	// scale 1, opaque.
	HudDock(std::string title, bool* hidden, std::function<void()> onHideChanged,
			const HudPanelLook* look);

	float Scale() const { return m_look ? m_look->scale : 1.0f; }

	// The content widget, added by the owner after construction so its bounds
	// resolve against this dock.
	template <typename T, typename... Args> T* SetContent(Args&&... args) {
		T* w = Add<T>(std::forward<Args>(args)...);
		m_content = w;
		return w;
	}

	// The minimize button's face (the square "-" box; null = the text "-") and
	// its tooltip.
	void SetMinimizeIcon(const gfx::Texture* icon, std::string tooltip);

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
	ui::Button* m_minimize = nullptr;
	ui::Widget* m_content = nullptr;
	const HudPanelLook* m_look = nullptr;
};

// The three docks, built as floating panels on `layer`.
struct HudDocks {
	ui::FloatingPanel* move = nullptr;
	ui::FloatingPanel* hands = nullptr; // null when !deps.withHands
	ui::FloatingPanel* magic = nullptr;
	SpellbookPanel* spellbook = nullptr;
	// The default TOP of the slot under Movement (pixels) - where the Hands
	// dock starts, or what takes its place (the Minimal layout's cards).
	std::function<float(ui::UIContext&)> handsTop;
};
HudDocks BuildHudDocks(ui::FloatingLayer& layer, const ControlBarDeps& deps);

} // namespace dungeon::game
