// ============================================================================
// UI/Widget.cpp — the tree walk. See Widget.h for the ordering rules.
// ============================================================================
#include "UI/Widget.h"

#include "UI/UIContext.h"

namespace dungeon::ui {

void Widget::Layout(const gfx::Rect& container, UIContext& ctx) {
	m_pixel = {container.x + bounds.x * container.w,
			   container.y + bounds.y * container.h, bounds.w * container.w,
			   bounds.h * container.h};
	// Self first: a container sizes itself, clamps its scroll, or assigns its
	// children's bounds here, so the recursion below sees the final values.
	LayoutSelf(ctx);
	const gfx::Rect content = ContentRect();
	for (auto& child : m_children) {
		if (!child->visible || !ChildActive(*child)) continue;
		child->Layout(content, ctx);
	}
}

void Widget::Update(UIContext& ctx) {
	if (!visible) return;
	// Children first, in reverse add order: the topmost child owning a pixel
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
	for (auto& child : m_children) {
		if (!child->visible || !ChildActive(*child)) continue;
		child->Draw(ctx, batch);
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
