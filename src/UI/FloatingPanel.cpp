// ============================================================================
// UI/FloatingPanel.cpp - see FloatingPanel.h.
// ============================================================================
#include "UI/FloatingPanel.h"

#include "UI/Controls.h"
#include "UI/Skin.h"
#include "UI/UIContext.h"

#include <algorithm>

namespace dungeon::ui {

namespace {
constexpr float kGripRem = 1.2f; // a grip's side
}

float FloatingPanel::EmAt(UIContext& ctx, float s) const {
	const FontRole role = ResolvedRole();
	return s == 1.0f ? ctx.FontFor(role).Height()
					 : ctx.FontAt(role, ctx.DesignHeight() * s).Height();
}

gfx::Rect FloatingPanel::MoveGrip() const {
	const gfx::Rect& px = Pixel();
	const float g = std::min({Rem(kGripRem), px.w * 0.5f, px.h * 0.5f});
	return {px.x, px.y, g, g};
}

gfx::Rect FloatingPanel::ResizeGrip() const {
	const gfx::Rect& px = Pixel();
	const float g = std::min({Rem(kGripRem), px.w * 0.5f, px.h * 0.5f});
	return {px.x + px.w - g, px.y + px.h - g, g, g};
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

void FloatingPanel::UpdateBeforeChildren(UIContext& ctx) {
	m_cursor = 0;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();
	const gfx::Rect& px = Pixel();
	const gfx::Rect& win = ContainerRect();

	// A drag in progress owns the pointer until the button lifts - wherever it
	// has wandered, and before any child can see it.
	if (m_drag != Drag::None) {
		if (!input->IsMouseDown(MouseButton::Left)) {
			m_drag = Drag::None;
			if (onChanged) onChanged();
			return;
		}
		if (m_drag == Drag::Move && posX && posY && win.w > 0.0f && win.h > 0.0f) {
			const float nx = std::clamp(m_startX + (mx - m_grabX), 0.0f,
										std::max(0.0f, win.w - px.w));
			const float ny = std::clamp(m_startY + (my - m_grabY), 0.0f,
										std::max(0.0f, win.h - px.h));
			*posX = nx / win.w;
			*posY = ny / win.h;
		} else if (m_drag == Drag::Resize && scale) {
			// Uniform: the pointer's travel as a share of the panel, both axes
			// averaged, so a diagonal pull and a sideways one both read.
			const float f = 1.0f + 0.5f * ((mx - m_grabX) / m_startW +
										   (my - m_grabY) / m_startH);
			*scale = std::clamp(m_startScale * f, minScale, maxScale);
		}
		m_cursor = m_drag == Drag::Move ? 1 : 2;
		ctx.ConsumeMouse();
		return;
	}

	m_hover = !ctx.IsMouseConsumed() && px.Contains(mx, my);
	if (!m_hover || !Unlocked()) return;
	// The grips take the pointer over their own squares only - everything
	// else on the panel is its content's first.
	if (MoveGrip().Contains(mx, my)) {
		m_cursor = 1;
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) StartDrag(Drag::Move, mx, my);
	} else if (scale && ResizeGrip().Contains(mx, my)) {
		m_cursor = 2;
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) StartDrag(Drag::Resize, mx, my);
	}
}

void FloatingPanel::UpdateSelf(UIContext& ctx) {
	if (m_drag != Drag::None) return;
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	const float mx = input->MouseX(), my = input->MouseY();
	if (!Pixel().Contains(mx, my)) return;
	// An opaque surface: nothing behind it - the 3D view - gets this click.
	// What is left of it after the content had its look is background, and a
	// press there moves the panel.
	ctx.ConsumeMouse();
	if (Unlocked() && input->WasMousePressed(MouseButton::Left))
		StartDrag(Drag::Move, mx, my);
}

void FloatingPanel::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!Unlocked() || !(m_hover || m_drag != Drag::None)) return;
	const Theme& theme = ctx.GetTheme();
	const Skin* skin = ctx.GetSkin();
	const Vec4 ink = theme.text;

	// MOVE: a small raised button with a four-way arrow.
	const gfx::Rect m = MoveGrip();
	if (skin && skin->button.texture) {
		DrawFace(batch, m, *skin, m_drag == Drag::Move ? Face::ButtonDown : Face::Button,
				 {1, 1, 1, 1});
	} else {
		batch.DrawRect(m, theme.control);
		DrawBorder(batch, m, theme.panelBorder);
	}
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

	// RESIZE: the classic corner wedge, in the accent while dragging.
	if (scale) {
		const gfx::Rect r = ResizeGrip();
		const Vec4 wedge = m_drag == Drag::Resize ? theme.accent : Vec4{ink.x, ink.y, ink.z, 0.75f};
		const float in = r.w * 0.18f;
		batch.DrawTriangle({r.x + r.w - in, r.y + in}, {r.x + r.w - in, r.y + r.h - in},
						   {r.x + in, r.y + r.h - in}, wedge);
	}
}

void FloatingLayer::LayoutSelf(UIContext& ctx) {
	const gfx::Rect& win = Pixel();
	if (win.w <= 0.0f || win.h <= 0.0f) return;
	for (const auto& child : Children()) {
		auto* panel = dynamic_cast<FloatingPanel*>(child.get());
		if (!panel || !panel->size) continue;
		if (panel->shownWhen) panel->visible = panel->shownWhen();
		if (!panel->visible) continue;
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
