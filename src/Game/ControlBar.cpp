// ============================================================================
// Game/ControlBar.cpp - see ControlBar.h.
// ============================================================================
#include "Game/ControlBar.h"

#include "Game/GuardSlider.h"
#include "Game/PartyHudDraw.h" // MutedIdentity

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

// Hands. The gap between two members' framed pairs is tight on purpose: the
// carved frame already separates them (ui-updates).
constexpr float kSetGap = 0.003f;
constexpr float kSetW = (kInnerW - kSetGap) / 2.0f;
constexpr float kHandGap = 0.0025f;

// Spacing, in EMs of the column's own type. Stated here rather than scattered
// through the layout code, because these are the numbers Michael tunes by eye
// and they should be findable in one place.
constexpr float kSideMargin = 0.5f;  // total, down both sides of a grid
constexpr float kHandRowGap = 0.3f;  // between one member's row and the next
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
	const float em = Em(1.0f);
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
				   const ControlBarDeps& deps)
	: m_roster(deps.roster), m_member(member) {
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
			},
			[onMiddle = deps.onHandMiddle, member, hand] {
				if (onMiddle) onMiddle(member, static_cast<size_t>(hand));
			});
		if (deps.handSetUse)
			slot->setUse = [setUse = deps.handSetUse, member, hand] {
				return setUse(member, static_cast<size_t>(hand));
			};
		slot->useIcons = deps.useIcons;
		slot->glow = deps.glow;
		m_slots[hand] = slot;
	}
	// ONE stance for the character, spanning both boxes - the fighter decides
	// how hard to press, and the two hands then guard with whatever each holds.
	m_guard = Add<GuardSlider>(gfx::Rect{0, 0.85f, 1.0f, 0.15f}, deps.roster,
							   member, deps.onGuardChange, deps.exertMax);
}

// How tall one pair must be for boxes of the largest square its width allows.
// Static and public because ControlBar has to ask it BEFORE laying the grid
// out - the grid's height is a consequence of the column's width, and only
// this function knows the shape of that consequence.
float HandPair::NeededHeight(float widthPx, float emPx) {
	return SquareSide(widthPx, emPx) + BandHeight(emPx) + 2.0f * FramePad(emPx);
}

float HandPair::SquareSide(float widthPx, float emPx) {
	// The frame's padding down both sides, the authored sliver between the
	// boxes, and the rest split in two. WIDTH ALONE decides - the height then
	// follows from it, which is the whole point: a box clamped by the height it
	// was given comes out tiny the moment the parent is short.
	const float gap = widthPx * (kHandGap / kSetW);
	const float avail = widthPx - 2.0f * FramePad(emPx) - gap;
	return std::max(0.0f, avail * 0.5f);
}

// The member frame is a groove kGrooveW wide, kGrooveIn in from the pair's edge,
// and the contents sit kFramePad in (the groove plus a breath of stone inside
// it). In EM, so the frame grows with the dock's scale.
namespace {
// Tight outside, roomier inside (Michael): the groove hugs the pair's edge and
// the hands stand a clear breath of stone in from it.
constexpr float kGrooveIn = 0.04f;
constexpr float kGrooveW = 0.17f;
constexpr float kFramePad = 0.45f;
static_assert(kGrooveIn + kGrooveW < kFramePad, "the groove must clear the hands");
} // namespace

float HandPair::FramePad(float emPx) { return emPx * kFramePad; }

void HandPair::DrawSelf(ui::UIContext&, gfx::SpriteBatch& batch) {
	const Character* c = RosterMember(m_roster, m_member);
	if (!c) return;
	const gfx::Rect& px = Pixel();
	const float in = Em(kGrooveIn);
	const gfx::Rect frame{px.x + in, px.y + in, px.w - 2.0f * in, px.h - 2.0f * in};
	if (frame.w <= 0.0f || frame.h <= 0.0f) return;
	// CARVED, not lit (Michael: the glowing border was far too bright) - a groove
	// round both hands and the effort meter whose floor is the member's colour
	// muted into the stone; the portrait wears the same frame. A member who is
	// down keeps it, darker: whose hands these are does not change.
	ui::DrawCarvedGroove(batch, frame, Em(kGrooveW),
						 MutedIdentity(c->portraitColor, !c->IsAlive()));
}

// The slider's WHOLE band (GuardSlider::kBandRem): the gap under the boxes, the
// ANGRY bar's thickness - an over-exerted stance swells toward it, and the swell
// must be the slider's own area rather than paint over the hands above - and a
// little slack below. All of it is the slider's grab zone (Michael, 2026-09-28:
// the bar alone was too thin to start a drag on); the gap that used to sit
// between the boxes and the slider is now the top of that zone.
float HandPair::BandHeight(float emPx) { return emPx * GuardSlider::kBandRem; }

void HandPair::LayoutSelf(ui::UIContext&) {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f || px.h <= 0.0f) return;

	const float em = Em(1.0f);
	const float gap = px.w * (kHandGap / kSetW);
	const float side = SquareSide(px.w, em);
	const float band = BandHeight(em);
	const float pad = FramePad(em);
	if (side <= 0.0f) return;

	// SQUARE IN PIXELS, which is why the two axes are divided by different
	// extents: `bounds` are fractions of the parent per axis, so equal
	// fractions are only a square when the parent happens to be square.
	for (int hand = 0; hand < 2; ++hand) {
		if (!m_slots[hand]) continue;
		m_slots[hand]->bounds = {(pad + (side + gap) * static_cast<float>(hand)) / px.w,
								 pad / px.h, side / px.w, side / px.h};
	}
	// Spans both boxes and the gap between them - the visual claim that it
	// governs the pair rather than either hand.
	if (m_guard)
		m_guard->bounds = {pad / px.w, (pad + side) / px.h, (side * 2.0f + gap) / px.w,
						   band / px.h};
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
	const float em = Em(1.0f);
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
				 std::function<void()> onCollapseChanged, const HudPanelLook* look)
	: m_collapsed(collapsed), m_look(look) {
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
	const float em = Em(1.0f);
	const float head = HasHeader() ? HeaderHeight(em) : 0.0f;
	const float gap = HasHeader() ? HeaderGap(em) : 0.0f;

	// The header: the title, and the minimize button square at its right end.
	// The button shows the ACTION (the editor's play-pause convention): "-"
	// while there is something to minimize, "+" while there is not - as the
	// square boxes when installed, the text otherwise. A one-character
	// assignment and a pointer, so it never allocates in a guarded frame.
	const float btn = m_toggle ? head : 0.0f;
	if (m_title)
		m_title->bounds = {0.0f, 0.0f, std::max(0.0f, inner.w - btn - em * 0.25f) / inner.w,
						   head / inner.h};
	if (m_toggle) {
		m_toggle->bounds = {(inner.w - btn) / inner.w, 0.0f, btn / inner.w, head / inner.h};
		m_toggle->text = Collapsed() ? "+" : "-";
		m_toggle->icon = Collapsed() ? m_icoExpand : m_icoCollapse;
	}
	// The content fills what is left, and is not there at all while minimized.
	if (m_content) {
		m_content->visible = !Collapsed();
		m_content->bounds = {0.0f, (head + gap) / inner.h, 1.0f,
							 std::max(0.0f, inner.h - head - gap) / inner.h};
	}
}

void HudDock::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	ui::DrawPanelFace(ctx, batch, Pixel(), m_look ? m_look->opacity : 1.0f);
}

// --- the docks as floating panels --------------------------------------------

namespace {

// A dock's pixel metrics at a width and em - what its size, and the default
// spots of the docks below it, are computed from.
struct DockMetrics {
	float pad = 0.0f, innerW = 0.0f, head = 0.0f, headGap = 0.0f;
	float Minimized() const { return 2 * pad + head; } // a header strip and its padding
};

DockMetrics MetricsFor(float w, float em, bool header) {
	DockMetrics m;
	m.pad = HudDock::Pad(w);
	m.innerW = std::max(0.0f, w - 2 * m.pad);
	m.head = header ? HudDock::HeaderHeight(em) : 0.0f;
	m.headGap = header ? HudDock::HeaderGap(em) : 0.0f;
	return m;
}

float MoveHeight(float w, float em) {
	const DockMetrics m = MetricsFor(w, em, true);
	return m.Minimized() + m.headGap + MovementPad::NeededHeight(m.innerW, em);
}

float HandsHeight(float w, float em, size_t rows) {
	const DockMetrics m = MetricsFor(w, em, false);
	return m.Minimized() + HandsArea::NeededHeight(m.innerW, em, rows);
}

} // namespace

HudDocks BuildHudDocks(ui::FloatingLayer& layer, const ControlBarDeps& deps) {
	HudDocks out;
	auto makePanel = [&](const char* name, HudPanelLook* look) {
		auto* panel = layer.Add<ui::FloatingPanel>();
		panel->debugName = name;
		panel->posX = &look->x;
		panel->posY = &look->y;
		panel->scale = &look->scale;
		panel->locked = deps.locked;
		panel->onChanged = deps.onPlacementChanged;
		return panel;
	};
	ui::FloatingPanel* move = out.move = makePanel("MovePanel", deps.moveLook);
	ui::FloatingPanel* hands = out.hands =
		deps.withHands ? makePanel("HandsPanel", deps.handsLook) : nullptr;
	ui::FloatingPanel* magic = out.magic = makePanel("MagicPanel", deps.magicLook);

	auto* moveDock = move->Add<HudDock>(deps.moveLabel, deps.moveCollapsed,
										deps.onCollapseChanged, deps.moveLook);
	moveDock->bounds = {0, 0, 1, 1};
	moveDock->debugName = "MoveDock";
	moveDock->SetToggleIcons(deps.boxPlus, deps.boxMinus);
	moveDock->SetContent<MovementPad>(gfx::Rect{0, 0, 1, 1}, deps);

	if (hands) {
		auto* handsDock = hands->Add<HudDock>(std::string(), nullptr, nullptr, deps.handsLook);
		handsDock->bounds = {0, 0, 1, 1};
		handsDock->debugName = "HandsDock";
		handsDock->SetContent<HandsArea>(gfx::Rect{0, 0, 1, 1}, deps);
	}

	auto* magicDock = magic->Add<HudDock>(deps.magicLabel, deps.magicCollapsed,
										  deps.onCollapseChanged, deps.magicLook);
	magicDock->bounds = {0, 0, 1, 1};
	magicDock->debugName = "MagicDock";
	magicDock->SetToggleIcons(deps.boxPlus, deps.boxMinus);
	out.spellbook = magicDock->SetContent<SpellbookPanel>(gfx::Rect{0, 0, 1, 1},
														  deps.roster, deps.icons);

	// Everything below is asked every layout, in pixels. A dock is the column's
	// width times its scale; its detail is in ITS em (the font at its scale,
	// asked exactly as Widget::Layout will ask it for the dock's subtree), so
	// the heights measured here are the heights the docks then lay out to.
	const size_t rows = HandRows(MemberCount(deps));
	const float columnW = deps.columnW, margin = deps.columnMargin;
	const HudPanelLook* moveLook = deps.moveLook;
	const HudPanelLook* handsLook = deps.handsLook;
	const HudPanelLook* magicLook = deps.magicLook;
	const bool* moveCollapsed = deps.moveCollapsed;
	const bool* magicCollapsed = deps.magicCollapsed;
	const std::function<float(ui::UIContext&)> columnTop = deps.columnTop;
	const std::function<float(ui::UIContext&)> columnBottom = deps.columnBottom;
	auto width = [columnW](ui::UIContext& ctx, float s) { return ctx.Width() * columnW * s; };
	auto right = [margin](ui::UIContext& ctx) { return ctx.Width() * (1.0f - margin); };
	auto gap = [move](ui::UIContext& ctx) { return move->EmAt(ctx, 1.0f) * kDockGap; };
	// The DEFAULT tops: the column top, then each dock below the one above at
	// that one's EXPANDED height and current scale - no reflow when one is
	// minimized (the header's rule).
	auto moveTop = [columnTop](ui::UIContext& ctx) { return columnTop ? columnTop(ctx) : 0.0f; };
	auto handsTop = [=](ui::UIContext& ctx) {
		const float s = moveLook->scale;
		return moveTop(ctx) + MoveHeight(width(ctx, s), move->EmAt(ctx, s)) + gap(ctx);
	};
	// (The hands' em is any dock's at that scale: all three share the HUD's font.)
	auto magicTop = [=](ui::UIContext& ctx) {
		const float s = handsLook->scale;
		return handsTop(ctx) + HandsHeight(width(ctx, s), move->EmAt(ctx, s), rows) + gap(ctx);
	};
	out.handsTop = handsTop;

	move->size = [=](ui::UIContext& ctx, float s) {
		const float w = width(ctx, s), em = move->EmAt(ctx, s);
		const bool collapsed = moveCollapsed && *moveCollapsed;
		return Vec2{w, collapsed ? MetricsFor(w, em, true).Minimized() : MoveHeight(w, em)};
	};
	move->defaultPos = [=](ui::UIContext& ctx) {
		return Vec2{right(ctx) - width(ctx, moveLook->scale), moveTop(ctx)};
	};

	if (hands) {
		hands->size = [=](ui::UIContext& ctx, float s) {
			const float w = width(ctx, s);
			return Vec2{w, HandsHeight(w, hands->EmAt(ctx, s), rows)};
		};
		hands->defaultPos = [=](ui::UIContext& ctx) {
			return Vec2{right(ctx) - width(ctx, handsLook->scale), handsTop(ctx)};
		};
	}

	// Magic's height at scale 1 is what the default column leaves below the
	// other two at THEIR scale 1 - a fixed number, so resizing the hands does
	// not resize the magic dock - and it scales from there like the others. The
	// Minimal layout says both its height and its spot itself.
	const std::function<float(ui::UIContext&)> magicHeight1 = deps.magicHeight1;
	const std::function<Vec2(ui::UIContext&)> magicDefaultPos = deps.magicDefaultPos;
	magic->size = [=](ui::UIContext& ctx, float s) {
		const float w = width(ctx, s), em = magic->EmAt(ctx, s);
		const DockMetrics m = MetricsFor(w, em, true);
		if (magicCollapsed && *magicCollapsed) return Vec2{w, m.Minimized()};
		if (magicHeight1) return Vec2{w, std::max(m.Minimized(), magicHeight1(ctx) * s)};
		const float w1 = width(ctx, 1.0f), em1 = magic->EmAt(ctx, 1.0f);
		const float top1 = moveTop(ctx) + MoveHeight(w1, em1) + gap(ctx) +
						   HandsHeight(w1, em1, rows) + gap(ctx);
		const float bottom = columnBottom ? columnBottom(ctx) : ctx.Height();
		return Vec2{w, std::max(m.Minimized(), (bottom - top1) * s)};
	};
	magic->defaultPos = [=](ui::UIContext& ctx) {
		if (magicDefaultPos) return magicDefaultPos(ctx);
		return Vec2{right(ctx) - width(ctx, magicLook->scale), magicTop(ctx)};
	};
	// Magic appears once ANY member knows a symbol - not before, and not by a
	// flag set when one is learned, which a load or a roster change would miss.
	const std::vector<Character>* roster = deps.roster;
	magic->shownWhen = [roster] {
		if (!roster) return false;
		for (const Character& c : *roster)
			if (c.knownSymbols != 0) return true;
		return false;
	};
	return out;
}

} // namespace dungeon::game
