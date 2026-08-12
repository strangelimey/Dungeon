// ============================================================================
// Game/ControlBar.cpp — see ControlBar.h.
// ============================================================================
#include "Game/ControlBar.h"

#include "Game/GuardSlider.h"

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

// Spacing, in EMs of the bar's own type. Stated here rather than scattered
// through the layout code, because these are the numbers Michael tunes by eye
// and they should be findable in one place.
constexpr float kSideMargin = 0.5f;  // total, down both sides of a grid
constexpr float kSliderGap = 0.25f;  // hand boxes -> the stance slider
constexpr float kHandRowGap = 0.5f;  // between one member's row and the next

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
	// Placeholder bounds: LayoutSelf computes square cells once the pixel
	// width is known (see HandPair — a square cannot be authored as a pair of
	// independent axis fractions).
	for (size_t i = 0; i < std::size(moves); ++i) {
		auto* btn = Add<ui::Button>(
			gfx::Rect{0, 0, 0.3f, 0.5f}, moves[i].glyph,
			[onMove = deps.onMove, action = moves[i].action] { onMove(action); });
		btn->icon = moves[i].turn ? deps.chevron2 : deps.chevron;
		btn->iconTurns = moves[i].quarters;
	}
}

float MovementPad::CellSide(float widthPx, float emPx) {
	// Three across: kSideMargin of margin in total, two gaps between the
	// cells, and the rest split three ways. WIDTH alone decides.
	const float gap = widthPx * (kMoveGap / kInnerW);
	return std::max(0.0f, (widthPx - emPx * kSideMargin - gap * 2.0f) / 3.0f);
}

float MovementPad::NeededHeight(float widthPx, float emPx) {
	const float gap = widthPx * (kMoveGap / kInnerW);
	return CellSide(widthPx, emPx) * 2.0f + gap; // two rows
}

void MovementPad::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f) return;
	const float em = Rem(1.0f);
	const float gap = px.w * (kMoveGap / kInnerW);
	const float side = CellSide(px.w, em);
	if (side <= 0.0f) return;

	size_t i = 0;
	for (const auto& child : Children()) {
		const float col = static_cast<float>(i % 3), row = static_cast<float>(i / 3);
		// Divided by DIFFERENT extents per axis, which is what makes it square
		// in pixels rather than merely equal in fractions.
		child->bounds = {(em * kSideMargin * 0.5f + (side + gap) * col) / px.w,
						 ((side + gap) * row) / px.h, side / px.w, side / px.h};
		++i;
	}
}

// --- HandPair --------------------------------------------------------------

HandPair::HandPair(const gfx::Rect& rect, size_t member,
				   const ControlBarDeps& deps) {
	bounds = rect;
	debugName = "HandPair";
	// Bounds here are placeholders: LayoutSelf computes the real ones once the
	// pixel rect is known, because a SQUARE box cannot be expressed as a pair
	// of independent axis fractions.
	for (int hand = 0; hand < 2; ++hand) {
		m_slots[hand] = Add<HandSlot>(
			gfx::Rect{0, 0, 0.5f, 1.0f}, deps.roster, member, hand, deps.icons,
			[onLeft = deps.onHandLeft, member, hand] {
				onLeft(member, static_cast<size_t>(hand));
			},
			[onRight = deps.onHandRight, member, hand] {
				onRight(member, static_cast<size_t>(hand));
			});
	}
	// ONE stance for the character, spanning both boxes — the fighter decides
	// how hard to press, and the two hands then guard with whatever each holds.
	m_guard = Add<GuardSlider>(gfx::Rect{0, 0.85f, 1.0f, 0.15f}, deps.roster,
							   member, deps.onGuardChange);
}

// How tall one pair must be for boxes of the largest square its width allows.
// Static and public because ControlBar has to ask it BEFORE laying the grid
// out — the grid's height is a consequence of the bar's width, and only this
// function knows the shape of that consequence.
float HandPair::NeededHeight(float widthPx, float emPx) {
	return SquareSide(widthPx, emPx) + emPx * kSliderGap + BandHeight(emPx);
}

float HandPair::SquareSide(float widthPx, float emPx) {
	// kSideMargin of margin in total, the authored sliver between the boxes,
	// and the rest split in two. WIDTH ALONE decides — the height then follows
	// from it, which is the whole point: a box clamped by the height it was
	// given comes out tiny the moment the parent is short.
	const float gap = widthPx * (kHandGap / kSetW);
	const float avail = widthPx - emPx * kSideMargin - gap;
	return std::max(0.0f, avail * 0.5f);
}

float HandPair::BandHeight(float emPx) { return emPx * 0.25f; }

void HandPair::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f) return;

	const float em = Rem(1.0f);
	const float gap = px.w * (kHandGap / kSetW);
	const float side = SquareSide(px.w, em);
	const float band = BandHeight(em);
	if (side <= 0.0f) return;

	// SQUARE IN PIXELS, which is why the two axes are divided by different
	// extents: `bounds` are fractions of the parent per axis, so equal
	// fractions are only a square when the parent happens to be square.
	for (int hand = 0; hand < 2; ++hand) {
		if (!m_slots[hand]) continue;
		m_slots[hand]->bounds = {
			(em * kSideMargin * 0.5f + (side + gap) * static_cast<float>(hand)) / px.w,
			0.0f, side / px.w, side / px.h};
	}
	// Spans both boxes and the gap between them — the visual claim that it
	// governs the pair rather than either hand.
	if (m_guard)
		m_guard->bounds = {em * kSideMargin * 0.5f / px.w,
						   (side + em * kSliderGap) / px.h,
						   (side * 2.0f + gap) / px.w, band / px.h};
}

// --- HandsArea -------------------------------------------------------------

HandsArea::HandsArea(const gfx::Rect& rect, const ControlBarDeps& deps) {
	bounds = rect;
	debugName = "HandsArea";
	const size_t members = MemberCount(deps);
	const float w = kSetW / kInnerW, gap = kSetGap / kInnerW;
	// Vertical placement is LayoutSelf's: a row's height depends on the pixel
	// width (square boxes), and the gap between rows is in ems, so neither is
	// known here.
	for (size_t i = 0; i < members; ++i)
		Add<HandPair>(gfx::Rect{(w + gap) * static_cast<float>(i % 2), 0.0f, w,
								1.0f},
					  i, deps);
}

float HandsArea::NeededHeight(float widthPx, float emPx, size_t rows) {
	if (rows == 0) return 0.0f;
	const float setW = widthPx * (kSetW / kInnerW);
	const float rowH = HandPair::NeededHeight(setW, emPx);
	// The gap goes BETWEEN rows, not after the last one — trailing space here
	// would push the Magic panel down for nothing.
	return rowH * static_cast<float>(rows) +
		   emPx * kHandRowGap * static_cast<float>(rows - 1);
}

void HandsArea::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f) return;
	const float em = Rem(1.0f);
	const float setW = px.w * (kSetW / kInnerW);
	const float rowH = HandPair::NeededHeight(setW, em);
	const float pitch = rowH + em * kHandRowGap;

	size_t i = 0;
	for (const auto& child : Children()) {
		child->bounds.y = pitch * static_cast<float>(i / 2) / px.h;
		child->bounds.h = rowH / px.h;
		++i;
	}
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

	m_pad = Add<MovementPad>(gfx::Rect{0.0f, 0.0f, 1.0f, padH}, deps);
	// handsH here is only a starting guess; LayoutSelf replaces it with the
	// height the square boxes actually need once the pixel width is known.
	m_hands = Add<HandsArea>(gfx::Rect{0.0f, handsTop, 1.0f, handsH}, deps);
	m_magic = Add<MagicArea>(gfx::Rect{0.0f, magicTop, 1.0f, 1.0f - magicTop}, deps);
	m_rows = HandRows(MemberCount(deps));
}

void ControlBar::LayoutSelf(ui::UIContext&) {
	if (!m_hands || !m_magic) return;
	const gfx::Rect inner = ContentRect();
	if (inner.w <= 0.0f || inner.h <= 0.0f) return;

	// Walk the same nesting the widgets do, in PIXELS, to find how tall one
	// row of square boxes has to be: the interior splits into member sets, a
	// set into two boxes plus the stance band beneath them.
	const float em = Rem(1.0f);
	const float padH = MovementPad::NeededHeight(inner.w, em);
	const float handsH = HandsArea::NeededHeight(inner.w, em, m_rows);
	const float clearPx = inner.w * (kHandsClear / kInnerW);

	if (m_pad) m_pad->bounds.h = padH / inner.h;
	m_hands->bounds.y = (padH + clearPx) / inner.h;
	m_hands->bounds.h = handsH / inner.h;
	m_magic->bounds.y = m_hands->bounds.y + m_hands->bounds.h;
	m_magic->bounds.h = std::max(0.0f, 1.0f - m_magic->bounds.y);
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
