// ============================================================================
// UI/Widget.cpp — the tree walk. See Widget.h for the ordering rules.
// ============================================================================
#include "UI/Widget.h"

#include "Platform/Input.h"
#include "UI/UIContext.h"

#include <algorithm>

namespace dungeon::ui {

namespace {

// The clip currently in force during the draw OR update walk (null = none).
// Each is a single pass on one thread and they never overlap, so one file-static
// is the whole stack: a nested clip intersects this and restores it on the way
// out. Input honours it for the same reason drawing does — see Widget::Update.
const gfx::Rect* g_clip = nullptr;
gfx::Rect g_clipRect{};

gfx::Rect Intersect(const gfx::Rect& a, const gfx::Rect& b) {
	const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
	const float x1 = std::min(a.x + a.w, b.x + b.w);
	const float y1 = std::min(a.y + a.h, b.y + b.h);
	return {x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f)};
}

bool Intersects(const gfx::Rect& a, const gfx::Rect& b) {
	const gfx::Rect i = Intersect(a, b);
	return i.w > 0.0f && i.h > 0.0f;
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
	m_container = container;
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
	// INPUT IS CLIPPED LIKE DRAWING. A widget that hit-tests a pixel it does not
	// paint is claiming someone else's area, and a scrolled subtree is full of
	// them: the asset picker's grid scrolls its top row up under the search box,
	// whose clicks the (invisible) tiles were taking. ChildActive covers a scroll
	// area's DIRECT children; a row inside a stack inside the area is a
	// grandchild, and only the clip knows where it really shows.
	//
	// Two parts, because a HALF-clipped widget is the hard case: skip a widget
	// clipped away entirely, and — for the rest — suppress the pointer through
	// any subtree the cursor is outside the clip of, so the visible half of a
	// tile stays clickable while its hidden half does not.
	if (g_clip && !Intersects(m_pixel, *g_clip)) return;

	UpdateBeforeChildren(ctx);
	const gfx::Rect* outer = g_clip;
	const gfx::Rect outerRect = g_clipRect;
	bool suppressed = false;
	if (const gfx::Rect* clip = ChildClip()) {
		g_clipRect = outer ? Intersect(*clip, outerRect) : *clip;
		g_clip = &g_clipRect;
		const Input* input = ctx.CurrentInput();
		if (input && !g_clip->Contains(input->MouseX(), input->MouseY()) &&
			!ctx.IsMouseConsumed()) {
			suppressed = true; // only ever un-suppressed below
			ctx.ConsumeMouse();
		}
	}
	// Children in reverse add order: the topmost child owning a pixel claims the
	// mouse before this widget's own hit test sees it.
	for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
		Widget& child = **it;
		if (!child.visible || !ChildActive(child)) continue;
		child.Update(ctx);
	}
	if (suppressed) ctx.SetMouseConsumed(false);
	if (g_clip != outer) {
		g_clip = outer;
		g_clipRect = outerRect;
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
