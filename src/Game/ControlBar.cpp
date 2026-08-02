// ============================================================================
// Game/ControlBar.cpp — see ControlBar.h.
// ============================================================================
#include "Game/ControlBar.h"

#include <algorithm>

namespace dungeon::game {

namespace {

// The panel's layout in the WINDOW fractions it was authored in. Nothing below
// uses these directly as bounds — each child divides them through by its own
// parent's span, so the tree reproduces the original pixel layout exactly while
// every level measures against its parent. Keeping the source numbers here (and
// the divisions visible) is what makes that correspondence checkable.
constexpr float kBarW = 0.156f;
constexpr float kBarH = 0.786f; // panel bottom (0.929) - the bar's top (0.143)
constexpr float kPad = 0.009f;  // inset inside the bar
constexpr float kTopPad = 0.013f; // the content starts a little lower than kPad
constexpr float kInnerW = kBarW - 2 * kPad;
constexpr float kInnerH = kBarH - kTopPad - kPad;

// Movement pad.
constexpr float kMoveGap = 0.005f;
constexpr float kMoveW = (kInnerW - 2 * kMoveGap) / 3.0f;
constexpr float kPadH = 2 * kMoveW + kMoveGap;

// Hands. The original placed the hand grid 0.016 below the pad's LAST ROW plus
// the row gap, so the clearance is that gap again on top of the 0.016.
constexpr float kHandsClear = kMoveGap + 0.016f;
constexpr float kSetGap = 0.005f;
constexpr float kSetW = (kInnerW - kSetGap) / 2.0f;
constexpr float kHandGap = 0.0025f;
constexpr float kHandW = (kSetW - kHandGap) / 2.0f;
constexpr float kSetH = kHandW + 0.009f; // a pair's row pitch

// Magic.
constexpr float kMagicLabelTop = 0.009f;
constexpr float kMagicLabelH = 0.022f;
constexpr float kBookTop = 0.036f;

// Rows the hand grid needs for `count` members, two per row.
size_t HandRows(size_t count) { return (std::min<size_t>(count, 4) + 1) / 2; }

size_t MemberCount(const ControlBarDeps& deps) {
	return deps.roster ? std::min<size_t>(deps.roster->size(), 4) : 0;
}

} // namespace

// --- MovementPad -----------------------------------------------------------

MovementPad::MovementPad(const gfx::Rect& rect, const ControlBarDeps& deps) {
	bounds = rect;
	debugName = "MovementPad";
	const struct {
		const char* glyph;
		MoveAction action;
		bool turn;
		int quarters;
	} moves[] = {
		{"«", MoveAction::TurnLeft, true, 2},  {"^", MoveAction::Forward, false, 3},
		{"»", MoveAction::TurnRight, true, 0}, {"<", MoveAction::StrafeLeft, false, 2},
		{"v", MoveAction::Back, false, 1},     {">", MoveAction::StrafeRight, false, 0},
	};
	// Cells as fractions of the pad: three across, two down, gaps between.
	const float w = kMoveW / kInnerW, gapX = kMoveGap / kInnerW;
	const float h = kMoveW / kPadH, gapY = kMoveGap / kPadH;
	for (size_t i = 0; i < std::size(moves); ++i) {
		auto* btn = Add<ui::Button>(
			gfx::Rect{(w + gapX) * static_cast<float>(i % 3),
					  (h + gapY) * static_cast<float>(i / 3), w, h},
			moves[i].glyph,
			[onMove = deps.onMove, action = moves[i].action] { onMove(action); });
		btn->icon = moves[i].turn ? deps.chevron2 : deps.chevron;
		btn->iconTurns = moves[i].quarters;
	}
}

// --- HandPair --------------------------------------------------------------

HandPair::HandPair(const gfx::Rect& rect, size_t member,
				   const ControlBarDeps& deps) {
	bounds = rect;
	debugName = "HandPair";
	// Two boxes side by side; each is square in the ORIGINAL authoring's terms
	// (equal window-width and -height fractions), so the pair's own box is a
	// little taller than the boxes and they sit at its top.
	const float w = kHandW / kSetW, gap = kHandGap / kSetW;
	const float h = kHandW / kSetH;
	for (int hand = 0; hand < 2; ++hand) {
		Add<HandSlot>(
			gfx::Rect{(w + gap) * static_cast<float>(hand), 0.0f, w, h},
			deps.roster, member, hand, deps.icons,
			[onLeft = deps.onHandLeft, member, hand] {
				onLeft(member, static_cast<size_t>(hand));
			},
			[onRight = deps.onHandRight, member, hand] {
				onRight(member, static_cast<size_t>(hand));
			});
	}
}

// --- HandsArea -------------------------------------------------------------

HandsArea::HandsArea(const gfx::Rect& rect, const ControlBarDeps& deps) {
	bounds = rect;
	debugName = "HandsArea";
	const size_t members = MemberCount(deps);
	const float rows = static_cast<float>(HandRows(members));
	const float w = kSetW / kInnerW, gap = kSetGap / kInnerW;
	const float h = 1.0f / rows; // the area is sized to exactly its rows
	for (size_t i = 0; i < members; ++i)
		Add<HandPair>(gfx::Rect{(w + gap) * static_cast<float>(i % 2),
								h * static_cast<float>(i / 2), w, h},
					  i, deps);
}

// --- MagicArea -------------------------------------------------------------

MagicArea::MagicArea(const gfx::Rect& rect, const ControlBarDeps& deps) {
	bounds = rect;
	debugName = "MagicArea";
	// The area's own height in window fractions, so its children can divide by
	// it (it depends on the hand-row count, which is why it isn't a constant).
	const float areaH = rect.h * kInnerH;
	Add<ui::Label>(gfx::Rect{0.0f, kMagicLabelTop / areaH, 1.0f,
							 kMagicLabelH / areaH},
				   deps.magicLabel);
	m_bookBounds = {0.0f, kBookTop / areaH, 1.0f, 1.0f - kBookTop / areaH};
	m_spellbook = Add<SpellbookPanel>(m_bookBounds, deps.roster, deps.icons);
}

void MagicArea::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	ui::DrawPanelFace(ctx, batch,
					  {px.x + m_bookBounds.x * px.w, px.y + m_bookBounds.y * px.h,
					   m_bookBounds.w * px.w, m_bookBounds.h * px.h});
}

// --- ControlBar ------------------------------------------------------------

ControlBar::ControlBar(const gfx::Rect& rect, const ControlBarDeps& deps) {
	bounds = rect;
	debugName = "ControlBar";
	// Each area's height as a fraction of the padded interior. The pad and the
	// hand grid take what they need; magic fills the remainder.
	const float padH = kPadH / kInnerH;
	const float handsTop = (kPadH + kHandsClear) / kInnerH;
	const float handsH =
		kSetH * static_cast<float>(HandRows(MemberCount(deps))) / kInnerH;
	const float magicTop = handsTop + handsH;

	Add<MovementPad>(gfx::Rect{0.0f, 0.0f, 1.0f, padH}, deps);
	Add<HandsArea>(gfx::Rect{0.0f, handsTop, 1.0f, handsH}, deps);
	m_magic = Add<MagicArea>(gfx::Rect{0.0f, magicTop, 1.0f, 1.0f - magicTop}, deps);
}

// The interior every area resolves against: inset by the bar's padding, a
// little more at the top, as fractions of the bar itself.
gfx::Rect ControlBar::ContentRect() const {
	const gfx::Rect& px = Pixel();
	const float x = kPad / kBarW, w = kInnerW / kBarW;
	const float y = kTopPad / kBarH, h = kInnerH / kBarH;
	return {px.x + x * px.w, px.y + y * px.h, w * px.w, h * px.h};
}

void ControlBar::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	ui::DrawPanelFace(ctx, batch, Pixel());
}

} // namespace dungeon::game
