// ============================================================================
// Game/ControlBar.cpp - see ControlBar.h.
// ============================================================================
#include "Game/ControlBar.h"

#include "Game/GuardSlider.h"

#include <algorithm>

namespace dungeon::game {

namespace {

// The panel's layout in the WINDOW fractions it was authored in, when it was
// one framed box. Nothing below uses these directly as bounds: they survive as
// RATIOS (a gap as a share of the interior width, say), which is what keeps the
// movement cells and hand boxes the size they always were.
constexpr float kBarW = 0.156f;
constexpr float kPad = 0.009f;  // inset inside the bar
constexpr float kInnerW = kBarW - 2 * kPad;

// Movement pad.
constexpr float kMoveGap = 0.005f;

// Hands.
constexpr float kSetGap = 0.005f;
constexpr float kSetW = (kInnerW - kSetGap) / 2.0f;
constexpr float kHandGap = 0.0025f;

// Spacing, in EMs of the column's own type. Stated here rather than scattered
// through the layout code, because these are the numbers Michael tunes by eye
// and they should be findable in one place.
constexpr float kSideMargin = 0.5f;  // total, down both sides of a grid
constexpr float kSliderGap = 0.25f;  // hand boxes -> the stance slider
constexpr float kHandRowGap = 0.5f;  // between one member's row and the next
constexpr float kDockGap = 0.5f;     // between one dock and the next
constexpr float kHeaderH = 1.4f;     // a dock's title strip (and its button)
constexpr float kHeaderGap = 0.25f;  // title strip -> content

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
	// width is known (see HandPair - a square cannot be authored as a pair of
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
		HandSlot* slot = Add<HandSlot>(
			gfx::Rect{0, 0, 0.5f, 1.0f}, deps.roster, member, hand, deps.icons,
			[onLeft = deps.onHandLeft, member, hand] {
				onLeft(member, static_cast<size_t>(hand));
			},
			[onRight = deps.onHandRight, member, hand] {
				onRight(member, static_cast<size_t>(hand));
			});
		if (deps.handSetUse)
			slot->setUse = [setUse = deps.handSetUse, member, hand] {
				return setUse(member, static_cast<size_t>(hand));
			};
		m_slots[hand] = slot;
	}
	// ONE stance for the character, spanning both boxes - the fighter decides
	// how hard to press, and the two hands then guard with whatever each holds.
	m_guard = Add<GuardSlider>(gfx::Rect{0, 0.85f, 1.0f, 0.15f}, deps.roster,
							   member, deps.onGuardChange);
}

// How tall one pair must be for boxes of the largest square its width allows.
// Static and public because ControlBar has to ask it BEFORE laying the grid
// out - the grid's height is a consequence of the column's width, and only
// this function knows the shape of that consequence.
float HandPair::NeededHeight(float widthPx, float emPx) {
	return SquareSide(widthPx, emPx) + emPx * kSliderGap + BandHeight(emPx);
}

float HandPair::SquareSide(float widthPx, float emPx) {
	// kSideMargin of margin in total, the authored sliver between the boxes,
	// and the rest split in two. WIDTH ALONE decides - the height then follows
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
	// Spans both boxes and the gap between them - the visual claim that it
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
	// The gap goes BETWEEN rows, not after the last one - trailing space here
	// would leave the hands dock taller than its boxes for nothing.
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

// --- HudDock ---------------------------------------------------------------

HudDock::HudDock(std::string title, bool* collapsed,
				 std::function<void()> onCollapseChanged)
	: m_collapsed(collapsed) {
	debugName = "HudDock";
	// Placeholder bounds throughout: the column places the dock, and LayoutSelf
	// places the header and content once the dock's pixel rect is known.
	if (!title.empty()) {
		m_title = Add<ui::Label>(gfx::Rect{}, std::move(title));
		m_title->centerV = true;
	}
	if (collapsed)
		m_toggle = Add<ui::Button>(gfx::Rect{}, "-",
			[this, onChanged = std::move(onCollapseChanged)] {
				*m_collapsed = !*m_collapsed;
				if (onChanged) onChanged();
			});
}

float HudDock::Pad(float widthPx) { return widthPx * (kPad / kBarW); }
float HudDock::HeaderHeight(float emPx) { return emPx * kHeaderH; }
float HudDock::HeaderGap(float emPx) { return emPx * kHeaderGap; }

gfx::Rect HudDock::ContentRect() const {
	const gfx::Rect& px = Pixel();
	const float pad = Pad(px.w);
	return {px.x + pad, px.y + pad, std::max(0.0f, px.w - 2 * pad),
			std::max(0.0f, px.h - 2 * pad)};
}

void HudDock::LayoutSelf(ui::UIContext&) {
	const gfx::Rect inner = ContentRect();
	if (inner.w <= 0.0f || inner.h <= 0.0f) return;
	const float em = Rem(1.0f);
	const float head = HasHeader() ? HeaderHeight(em) : 0.0f;
	const float gap = HasHeader() ? HeaderGap(em) : 0.0f;

	// The header: the title, and the minimize button square at its right end.
	// The button shows the ACTION (the editor's play-pause convention): "-"
	// while there is something to minimize, "+" while there is not. A
	// one-character assignment, so it never allocates in a guarded frame.
	const float btn = m_toggle ? head : 0.0f;
	if (m_title)
		m_title->bounds = {0.0f, 0.0f, std::max(0.0f, inner.w - btn - em * 0.25f) / inner.w,
						   head / inner.h};
	if (m_toggle) {
		m_toggle->bounds = {(inner.w - btn) / inner.w, 0.0f, btn / inner.w, head / inner.h};
		m_toggle->text = Collapsed() ? "+" : "-";
	}
	// The content fills what is left, and is not there at all while minimized.
	if (m_content) {
		m_content->visible = !Collapsed();
		m_content->bounds = {0.0f, (head + gap) / inner.h, 1.0f,
							 std::max(0.0f, inner.h - head - gap) / inner.h};
	}
}

void HudDock::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	ui::DrawPanelFace(ctx, batch, Pixel());
}

// --- ControlBar ------------------------------------------------------------

ControlBar::ControlBar(const gfx::Rect& rect, const ControlBarDeps& deps)
	: m_roster(deps.roster) {
	bounds = rect;
	debugName = "ControlBar";
	m_moveDock = Add<HudDock>(deps.moveLabel, deps.moveCollapsed, deps.onCollapseChanged);
	m_moveDock->debugName = "MoveDock";
	m_moveDock->SetContent<MovementPad>(gfx::Rect{0, 0, 1, 1}, deps);

	m_handsDock = Add<HudDock>(std::string(), nullptr, nullptr);
	m_handsDock->debugName = "HandsDock";
	m_handsDock->SetContent<HandsArea>(gfx::Rect{0, 0, 1, 1}, deps);

	m_magicDock = Add<HudDock>(deps.magicLabel, deps.magicCollapsed, deps.onCollapseChanged);
	m_magicDock->debugName = "MagicDock";
	m_spellbook = m_magicDock->SetContent<SpellbookPanel>(gfx::Rect{0, 0, 1, 1},
														  deps.roster, deps.icons);
	m_rows = HandRows(MemberCount(deps));
}

void ControlBar::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f) return;

	// Everything in PIXELS first, then fractions of this column. The docks share
	// the column's width, so their padded interiors all have the same width, and
	// that width is what sizes the square cells and boxes inside them.
	const float em = Rem(1.0f);
	const float pad = HudDock::Pad(px.w);
	const float innerW = std::max(0.0f, px.w - 2 * pad);
	const float head = HudDock::HeaderHeight(em);
	const float headGap = HudDock::HeaderGap(em);
	const float dockGap = em * kDockGap;
	const float minimized = 2 * pad + head; // a header strip and its padding

	const float moveH = 2 * pad + head + headGap + MovementPad::NeededHeight(innerW, em);
	const float handsH = 2 * pad + HandsArea::NeededHeight(innerW, em, m_rows);

	auto place = [&](HudDock* dock, float y, float h) {
		dock->bounds = {0.0f, y / px.h, 1.0f, h / px.h};
	};
	// NO REFLOW: each dock's TOP comes from the others' EXPANDED heights, so
	// minimizing one leaves a gap rather than pulling the next one up.
	float y = 0.0f;
	place(m_moveDock, y, m_moveDock->Collapsed() ? minimized : moveH);
	y += moveH + dockGap;
	place(m_handsDock, y, handsH);
	y += handsH + dockGap;
	place(m_magicDock, y,
		  m_magicDock->Collapsed() ? minimized : std::max(minimized, px.h - y));

	// Magic appears once ANY member knows a symbol - not before, and not by a
	// flag set when one is learned, which a load or a roster change would miss.
	bool anySymbols = false;
	if (m_roster)
		for (const Character& c : *m_roster) anySymbols = anySymbols || c.knownSymbols != 0;
	m_magicDock->visible = anySymbols;
}

} // namespace dungeon::game
