// ============================================================================
// UI/FloatingPanel.cpp - see FloatingPanel.h.
// ============================================================================
#include "UI/FloatingPanel.h"

#include "Core/MathTypes.h" // kPi
#include "Platform/Input.h" // vk::Control
#include "UI/Controls.h"
#include "UI/Skin.h"
#include "UI/UIContext.h"

#include <algorithm>
#include <cmath>

namespace dungeon::ui {

namespace {
constexpr float kGripRem = 1.2f;  // a grip's side
constexpr float kSnapRem = 0.5f;  // how near an edge has to come to catch
}

float FloatingPanel::EmAt(UIContext& ctx, float s) const {
	const FontRole role = ResolvedRole();
	return s == 1.0f ? ctx.FontFor(role).Height()
					 : ctx.FontAt(role, ctx.DesignHeight() * s).Height();
}

gfx::Rect FloatingPanel::GripRect(int corner) const {
	const gfx::Rect& px = Pixel();
	const float g = std::min({Rem(kGripRem), px.w * 0.5f, px.h * 0.5f});
	const float x = (corner == 1 || corner == 3) ? px.x + px.w - g : px.x;
	const float y = corner >= 2 ? px.y + px.h - g : px.y;
	return {x, y, g, g};
}

bool FloatingPanel::CanHide() const {
	return hidden && m_layer && m_layer->onHideChanged;
}

gfx::Rect FloatingPanel::ResetRect() const {
	gfx::Rect r = GripRect(1);
	// Beside minimize, a hairline apart; and never over the move cross.
	if (CanHide()) r.x = std::max(Pixel().x + r.w, r.x - r.w - 1.0f);
	return r;
}

void FloatingPanel::StartDrag(Drag kind, float mx, float my) {
	const gfx::Rect& px = Pixel();
	const gfx::Rect& win = ContainerRect();
	m_drag = kind;
	m_grabX = mx;
	m_grabY = my;
	m_startX = px.x - win.x;
	m_startY = px.y - win.y;
	m_startW = std::max(1.0f, px.w);
	m_startH = std::max(1.0f, px.h);
	m_startScale = Scale();
	// PIN the spot the panel is at: one still on its default would otherwise
	// follow its default rule while growing (a dock anchored to the right edge
	// would grow leftward, its top-left sliding away from the pointer).
	if (posX && posY && win.w > 0.0f && win.h > 0.0f) {
		*posX = m_startX / win.w;
		*posY = m_startY / win.h;
	}
}

// The window first, then every other shown panel of this layer and of its
// peer. Rects are absolute pixels - both contexts are window-sized.
template <typename Fn>
void FloatingPanel::ForEachSnapTarget(Fn&& fn) const {
	fn(ContainerRect(), true);
	auto walk = [&](const FloatingLayer* layer) {
		if (!layer) return;
		for (const auto& child : layer->Children()) {
			const auto* panel = dynamic_cast<const FloatingPanel*>(child.get());
			if (!panel || panel == this || !panel->visible) continue;
			const gfx::Rect& r = panel->Pixel();
			if (r.w > 0.0f && r.h > 0.0f) fn(r, false);
		}
	};
	walk(m_layer);
	if (m_layer && m_layer->snapPeer) walk(m_layer->snapPeer());
}

// A hairline along `edge` spanning both rects - what the snap lined up.
static gfx::Rect GuideV(float edge, float top0, float bottom0, const gfx::Rect& other) {
	const float top = std::min(top0, other.y), bottom = std::max(bottom0, other.y + other.h);
	return {std::floor(edge), top, 1.0f, bottom - top};
}
static gfx::Rect GuideH(float edge, float left0, float right0, const gfx::Rect& other) {
	const float left = std::min(left0, other.x), right = std::max(right0, other.x + other.w);
	return {left, std::floor(edge), right - left, 1.0f};
}

void FloatingPanel::SnapMove(float& x, float& y, float w, float h, float reach) {
	struct Best {
		float dist, delta = 0.0f, edge = 0.0f;
		gfx::Rect other{};
		bool hit = false, window = false;
	};
	Best bx{reach + 0.001f}, by{reach + 0.001f};
	ForEachSnapTarget([&](const gfx::Rect& q, bool isWindow) {
		// Either of this panel's edges onto either of the other's: lining up
		// (left on left) and butting up (left on right) both catch.
		for (const float edge : {q.x, q.x + q.w})
			for (const float mine : {x, x + w}) {
				const float d = edge - mine;
				if (std::abs(d) < bx.dist) bx = {std::abs(d), d, edge, q, true, isWindow};
			}
		for (const float edge : {q.y, q.y + q.h})
			for (const float mine : {y, y + h}) {
				const float d = edge - mine;
				if (std::abs(d) < by.dist) by = {std::abs(d), d, edge, q, true, isWindow};
			}
	});
	if (bx.hit) x += bx.delta;
	if (by.hit) y += by.delta;
	// The window's own edge needs no guide - it is the edge of the screen.
	m_guideX = bx.hit && !bx.window ? GuideV(bx.edge, y, y + h, bx.other) : gfx::Rect{};
	m_guideY = by.hit && !by.window ? GuideH(by.edge, x, x + w, by.other) : gfx::Rect{};
}

// A resize is uniform about the pinned top-left, so only the right and bottom
// edges move. The nearer edge in reach wins; the scale that puts it there is
// SOLVED, since a panel's size is its content's (a dock's height steps with its
// font). A solve that cannot land within a pixel and a half - the party bar
// stops widening at 1, a font step jumps the target - is no snap.
float FloatingPanel::SnapResize(UIContext& ctx, float s, float reach) {
	m_guideX = m_guideY = {};
	if (!size) return s;
	const gfx::Rect& win = ContainerRect();
	const float ox = win.x + m_startX, oy = win.y + m_startY;
	const Vec2 now = size(ctx, s);
	struct Best {
		float dist, target = 0.0f;
		gfx::Rect other{};
		bool hit = false, window = false;
	};
	Best bx{reach + 0.001f}, by{reach + 0.001f};
	ForEachSnapTarget([&](const gfx::Rect& q, bool isWindow) {
		for (const float edge : {q.x, q.x + q.w}) {
			const float d = std::abs(edge - (ox + now.x));
			if (d < bx.dist) bx = {d, edge, q, true, isWindow};
		}
		for (const float edge : {q.y, q.y + q.h}) {
			const float d = std::abs(edge - (oy + now.y));
			if (d < by.dist) by = {d, edge, q, true, isWindow};
		}
	});
	auto solve = [&](bool alongX, float target) {
		const float want = target - (alongX ? ox : oy);
		if (want <= 1.0f) return -1.0f;
		float t = s;
		for (int i = 0; i < 4; ++i) {
			const Vec2 z = size(ctx, t);
			const float cur = alongX ? z.x : z.y;
			if (cur <= 0.0f) return -1.0f;
			const float next = std::clamp(t * want / cur, minScale, maxScale);
			if (std::abs(next - t) < 1e-5f) break;
			t = next;
		}
		const Vec2 z = size(ctx, t);
		return std::abs((alongX ? z.x : z.y) - want) <= 1.5f ? t : -1.0f;
	};
	// Nearer axis first; if that one cannot land, the other may.
	const bool xFirst = bx.hit && (!by.hit || bx.dist <= by.dist);
	for (int pass = 0; pass < 2; ++pass) {
		const bool alongX = (pass == 0) == xFirst;
		const Best& b = alongX ? bx : by;
		if (!b.hit) continue;
		const float t = solve(alongX, b.target);
		if (t < 0.0f) continue;
		const Vec2 z = size(ctx, t);
		if (!b.window) {
			if (alongX) m_guideX = GuideV(b.target, oy, oy + z.y, b.other);
			else m_guideY = GuideH(b.target, ox, ox + z.x, b.other);
		}
		return t;
	}
	return s;
}

void FloatingPanel::UpdateBeforeChildren(UIContext& ctx) {
	m_cursor = 0;
	m_arranging = false;
	m_resetHot = false;
	m_hideHot = false;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();
	const gfx::Rect& px = Pixel();
	const gfx::Rect& win = ContainerRect();
	const float reach = Rem(kSnapRem);

	// A drag in progress owns the pointer until the button lifts - wherever it
	// has wandered, and before any child can see it. Ctrl may already be up.
	if (m_drag != Drag::None) {
		if (!input->IsMouseDown(MouseButton::Left)) {
			m_drag = Drag::None;
			m_guideX = m_guideY = {};
			if (onChanged) onChanged();
			return;
		}
		if (m_drag == Drag::Move && posX && posY && win.w > 0.0f && win.h > 0.0f) {
			const float maxX = std::max(0.0f, win.w - px.w), maxY = std::max(0.0f, win.h - px.h);
			float ax = win.x + std::clamp(m_startX + (mx - m_grabX), 0.0f, maxX);
			float ay = win.y + std::clamp(m_startY + (my - m_grabY), 0.0f, maxY);
			SnapMove(ax, ay, px.w, px.h, reach);
			*posX = std::clamp(ax - win.x, 0.0f, maxX) / win.w;
			*posY = std::clamp(ay - win.y, 0.0f, maxY) / win.h;
		} else if (m_drag == Drag::Resize && scale) {
			// Uniform: the pointer's travel as a share of the panel, both axes
			// averaged, so a diagonal pull and a sideways one both read.
			const float f = 1.0f + 0.5f * ((mx - m_grabX) / m_startW +
										   (my - m_grabY) / m_startH);
			*scale = SnapResize(ctx, std::clamp(m_startScale * f, minScale, maxScale), reach);
		}
		m_arranging = true;
		m_cursor = m_drag == Drag::Move ? 1 : 2;
		ctx.ConsumeMouse();
		return;
	}

	// ARRANGING: Ctrl held over the panel. It takes the whole pointer before
	// its content does - reset button, resize wedge, and everywhere else moves.
	if (!input->IsKeyDown(vk::Control) || !Unlocked() || ctx.IsMouseConsumed() ||
		!px.Contains(mx, my))
		return;
	m_arranging = true;
	ctx.ConsumeMouse();
	const bool pressed = input->WasMousePressed(MouseButton::Left);
	if (CanHide() && HideRect().Contains(mx, my)) {
		m_hideHot = true;
		if (pressed) {
			*hidden = true;
			m_layer->onHideChanged();
		}
		return;
	}
	if (m_layer && m_layer->onResetAll && ResetRect().Contains(mx, my)) {
		m_resetHot = true;
		if (pressed) m_layer->onResetAll();
		return;
	}
	if (scale && GripRect(3).Contains(mx, my)) {
		m_cursor = 2;
		if (pressed) StartDrag(Drag::Resize, mx, my);
		return;
	}
	m_cursor = 1;
	if (pressed) StartDrag(Drag::Move, mx, my);
}

void FloatingPanel::UpdateSelf(UIContext& ctx) {
	if (m_drag != Drag::None) return;
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	// An opaque surface: nothing behind it - the 3D view - gets a click on what
	// is left of it after the content had its look.
	if (Pixel().Contains(input->MouseX(), input->MouseY())) ctx.ConsumeMouse();
}

// A circular arrow: an arc of short rotated bars, most of the way round, with
// a head at its end pointing on round - "back to where it started".
void FloatingPanel::DrawResetGlyph(gfx::SpriteBatch& batch, const gfx::Rect& r,
								   const Vec4& ink) const {
	const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
	const float radius = r.w * 0.24f, thick = std::max(1.0f, r.w * 0.09f);
	const float a0 = -kPi * 0.5f + 0.75f;       // just right of the top
	const float a1 = a0 + 2.0f * kPi - 1.5f;     // round to just left of it
	constexpr int kSegs = 12;
	auto at = [&](float a) { return Vec2{cx + radius * std::cos(a), cy + radius * std::sin(a)}; };
	for (int i = 0; i < kSegs; ++i) {
		const Vec2 p = at(a0 + (a1 - a0) * i / kSegs), q = at(a0 + (a1 - a0) * (i + 1) / kSegs);
		const float dx = q.x - p.x, dy = q.y - p.y;
		const float len = std::sqrt(dx * dx + dy * dy);
		batch.DrawRectRotated({(p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f}, {len + thick * 0.5f, thick},
							  std::atan2(dy, dx), ink);
	}
	// The head: along the tangent at the arc's end (screen y is down, so a
	// growing angle runs clockwise and the tangent is (-sin, cos)).
	const Vec2 end = at(a1);
	const Vec2 tan{-std::sin(a1), std::cos(a1)}, out{std::cos(a1), std::sin(a1)};
	const float head = thick * 2.2f, half = thick * 1.6f;
	batch.DrawTriangle({end.x + tan.x * head, end.y + tan.y * head},
					   {end.x + out.x * half, end.y + out.y * half},
					   {end.x - out.x * half, end.y - out.y * half}, ink);
}

void FloatingPanel::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!Unlocked() || !m_arranging) return;
	const Theme& theme = ctx.GetTheme();
	const Skin* skin = ctx.GetSkin();
	const Vec4 ink = theme.text;
	const bool dragging = m_drag != Drag::None;
	auto button = [&](const gfx::Rect& r, bool down) {
		if (skin && skin->button.texture) {
			DrawFace(batch, r, *skin, down ? Face::ButtonDown : Face::Button, {1, 1, 1, 1});
		} else {
			batch.DrawRect(r, theme.control);
			DrawBorder(batch, r, theme.panelBorder);
		}
	};

	// Which panel Ctrl has: an accent outline round the whole of it.
	DrawBorder(batch, Pixel(), theme.accent);

	// The snap guides: the edge this drag caught, across both panels.
	if (m_guideX.h > 0.0f) batch.DrawRect(m_guideX, theme.accent);
	if (m_guideY.w > 0.0f) batch.DrawRect(m_guideY, theme.accent);

	// MOVE: a small raised button with a four-way arrow - the whole panel
	// moves, this only says so.
	if (!dragging || m_drag == Drag::Move) {
		const gfx::Rect m = GripRect(0);
		button(m, m_drag == Drag::Move);
		const float cx = m.x + m.w * 0.5f, cy = m.y + m.h * 0.5f;
		const float a = m.w * 0.30f; // arrow tip distance from the centre
		const float b = m.w * 0.12f; // arrow half-width
		const float c = m.w * 0.14f; // arrow base distance from the centre
		batch.DrawTriangle({cx, cy - a}, {cx + b, cy - c}, {cx - b, cy - c}, ink);
		batch.DrawTriangle({cx, cy + a}, {cx - b, cy + c}, {cx + b, cy + c}, ink);
		batch.DrawTriangle({cx - a, cy}, {cx - c, cy - b}, {cx - c, cy + b}, ink);
		batch.DrawTriangle({cx + a, cy}, {cx + c, cy + b}, {cx + c, cy - b}, ink);
		batch.DrawRect({cx - c, cy - b * 0.35f, c * 2.0f, b * 0.7f}, ink);
		batch.DrawRect({cx - b * 0.35f, cy - c, b * 0.7f, c * 2.0f}, ink);
	}

	// RESIZE: the classic corner wedge, in the accent while dragging.
	if (scale && (!dragging || m_drag == Drag::Resize)) {
		const gfx::Rect r = GripRect(3);
		const Vec4 wedge = m_drag == Drag::Resize ? theme.accent : Vec4{ink.x, ink.y, ink.z, 0.75f};
		const float in = r.w * 0.18f;
		batch.DrawTriangle({r.x + r.w - in, r.y + in}, {r.x + r.w - in, r.y + r.h - in},
						   {r.x + in, r.y + r.h - in}, wedge);
	}

	// MINIMIZE and RESET, the top-right pair. Not while dragging - the drag
	// owns the pointer.
	if (dragging || !m_layer) return;
	// A button's tooltip, below it (above when that runs off the screen) and
	// pulled in from the right edge, the hand tooltip's rule.
	auto tooltip = [&](const gfx::Rect& r, const std::string& tip) {
		if (tip.empty()) return;
		const Font& font = TextFont();
		const float padX = Em(0.6f), padY = Em(0.35f), gapY = Em(0.3f);
		const float w = font.MeasureWidth(tip) + 2.0f * padX;
		const float h = font.Height() + 2.0f * padY;
		const gfx::Rect box = PlaceTooltip(r, w, h, {0, 0, ctx.Width(), ctx.Height()},
										   TipSide::Below, gapY, padX, TipAlign::End);
		batch.DrawRect(box, {0.10f, 0.10f, 0.13f, 0.97f});
		DrawBorder(batch, box, theme.panelBorder);
		font.Draw(batch, tip, box.x + padX, box.y + padY, theme.text);
	};
	if (CanHide()) {
		// A bar low in the box: the window minimized to a line.
		const gfx::Rect r = HideRect();
		button(r, false);
		const float bw = r.w * 0.48f, bh = std::max(2.0f, std::round(r.h * 0.12f));
		batch.DrawRect({std::round(r.x + (r.w - bw) * 0.5f), std::round(r.y + r.h * 0.62f), bw, bh},
					   m_hideHot ? theme.accent : ink);
	}
	if (m_layer->onResetAll) {
		const gfx::Rect r = ResetRect();
		button(r, false);
		DrawResetGlyph(batch, r, m_resetHot ? theme.accent : ink);
		if (m_resetHot) tooltip(r, m_layer->resetTip);
	}
	if (m_hideHot) tooltip(HideRect(), m_layer->hideTip);
}

void FloatingLayer::LayoutSelf(UIContext& ctx) {
	const gfx::Rect& win = Pixel();
	if (win.w <= 0.0f || win.h <= 0.0f) return;
	for (const auto& child : Children()) {
		auto* panel = dynamic_cast<FloatingPanel*>(child.get());
		if (!panel || !panel->size) continue;
		panel->m_layer = this;
		panel->visible = (!panel->shownWhen || panel->shownWhen()) && !panel->Hidden();
		if (!panel->visible) {
			// Put away mid-arrange: nothing of the arranging state may outlive it.
			panel->m_drag = FloatingPanel::Drag::None;
			panel->m_arranging = false;
			continue;
		}
		const float s = panel->Scale();
		if (panel->scalesText) panel->fontScale = s;
		Vec2 size = panel->size(ctx, s);
		size.x = std::clamp(size.x, 1.0f, win.w);
		size.y = std::clamp(size.y, 1.0f, win.h);
		Vec2 pos{0.0f, 0.0f};
		if (panel->posX && panel->posY && *panel->posX >= 0.0f && *panel->posY >= 0.0f)
			pos = {*panel->posX * win.w, *panel->posY * win.h};
		else if (panel->defaultPos)
			pos = panel->defaultPos(ctx);
		// Inside the window, whatever was saved: a smaller window, or a panel
		// grown past the edge, never leaves one unreachable.
		pos.x = std::clamp(pos.x, 0.0f, win.w - size.x);
		pos.y = std::clamp(pos.y, 0.0f, win.h - size.y);
		panel->bounds = {pos.x / win.w, pos.y / win.h, size.x / win.w, size.y / win.h};
	}
}

} // namespace dungeon::ui
