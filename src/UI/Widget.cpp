// ============================================================================
// UI/Widget.cpp — the tree walk. See Widget.h for the ordering rules.
// ============================================================================
#include "UI/Widget.h"

#include "UI/UIContext.h"

#include <algorithm>

namespace dungeon::ui {

namespace {

// The clip currently in force during the draw walk (null = none). Drawing is a
// single pass on one thread, so one file-static is the whole stack: a nested
// clip intersects this and restores it on the way out.
const gfx::Rect* g_clip = nullptr;
gfx::Rect g_clipRect{};

gfx::Rect Intersect(const gfx::Rect& a, const gfx::Rect& b) {
	const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
	const float x1 = std::min(a.x + a.w, b.x + b.w);
	const float y1 = std::min(a.y + a.h, b.y + b.h);
	return {x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f)};
}

} // namespace

ScopedClip::ScopedClip(gfx::SpriteBatch& batch, const gfx::Rect& rect)
	: m_batch(batch), m_outer(g_clip), m_outerRect(g_clipRect) {
	g_clipRect = m_outer ? Intersect(rect, m_outerRect) : rect;
	g_clip = &g_clipRect;
	m_batch.SetScissor(g_clip);
}

ScopedClip::~ScopedClip() {
	g_clip = m_outer;
	g_clipRect = m_outerRect;
	m_batch.SetScissor(g_clip);
}

void Widget::Layout(const gfx::Rect& container, UIContext& ctx,
					std::optional<FontRole> inheritedRole,
					float inheritedScale) {
	m_pixel = {container.x + bounds.x * container.w,
			   container.y + bounds.y * container.h, bounds.w * container.w,
			   bounds.h * container.h};
	m_rem = ctx.GetFont().Height(); // this context's root font size (Units.h)

	// Resolve the face BEFORE LayoutSelf, so a container that sizes itself from
	// its own text measures in the face it will actually draw in. Either axis
	// set here wins; otherwise inherit the parent's; the root takes the
	// context's own role at its authored size.
	m_resolvedRole = fontRole ? *fontRole : inheritedRole.value_or(ctx.RootRole());
	const float scale = fontScale ? *fontScale : inheritedScale;
	// Ask for the size off DesignHeight, not GetFont().Height(): the library
	// applies the role's optical scale inside Get, so multiplying an
	// already-scaled height would apply it a second time.
	m_font = scale == 1.0f ? &ctx.FontFor(m_resolvedRole)
						   : &ctx.FontAt(m_resolvedRole,
										 ctx.DesignHeight() * scale);
	m_em = m_font->Height();

	// Self first: a container sizes itself, clamps its scroll, or assigns its
	// children's bounds here, so the recursion below sees the final values.
	LayoutSelf(ctx);
	const gfx::Rect content = ContentRect();
	for (auto& child : m_children) {
		if (!child->visible || !ChildActive(*child)) continue;
		child->m_resolvedRole = m_resolvedRole; // inherited unless it sets one
		child->Layout(content, ctx, m_resolvedRole, scale);
	}
}

void Widget::Update(UIContext& ctx) {
	if (!visible) return;
	UpdateBeforeChildren(ctx);
	// Children next, in reverse add order: the topmost child owning a pixel
	// claims the mouse before this widget's own hit test sees it.
	for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
		Widget& child = **it;
		if (!child.visible || !ChildActive(child)) continue;
		child.Update(ctx);
	}
	UpdateSelf(ctx);
}

void Widget::Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!visible) return;
	DrawSelf(ctx, batch); // parent behind its children
	if (m_children.empty()) return;

	// Push this widget's child clip (intersected with whatever is already in
	// force), draw the children, then restore.
	const gfx::Rect* outer = g_clip;
	const gfx::Rect outerRect = g_clipRect;
	if (const gfx::Rect* clip = ChildClip()) {
		g_clipRect = outer ? Intersect(*clip, outerRect) : *clip;
		g_clip = &g_clipRect;
		batch.SetScissor(g_clip);
	}
	for (auto& child : m_children) {
		if (!child->visible || !ChildActive(*child)) continue;
		child->Draw(ctx, batch);
	}
	if (g_clip != outer) {
		g_clip = outer;
		g_clipRect = outerRect;
		batch.SetScissor(g_clip);
	}
}

void Widget::DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!visible) return;
	DrawOverlaySelf(ctx, batch);
	for (auto& child : m_children) {
		if (!child->visible || !ChildActive(*child)) continue;
		child->DrawOverlay(ctx, batch);
	}
}

} // namespace dungeon::ui
