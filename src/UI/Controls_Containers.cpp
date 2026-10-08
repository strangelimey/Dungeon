// ============================================================================
// UI/Controls_Containers.cpp - the containers: ScrollArea (scroll, clip and the
// ui::ScrollBar), TabControl and Repeater.
// Split by widget family by code-review C127; declared in UI/Controls.h.
// ============================================================================
#include "UI/Controls.h"
#include "UI/Controls_Detail.h"

#include <algorithm>

namespace dungeon::ui {

// --- ScrollArea ------------------------------------------------------------

gfx::Rect ScrollArea::ViewRect() const {
	const gfx::Rect& px = Pixel();
	const float pad = Rem(padding), gut = Rem(gutter);
	return {px.x + pad, px.y + pad, px.w - pad - gut, px.h - 2.0f * pad};
}

gfx::Rect ScrollArea::ContentRect() const {
	const gfx::Rect view = ViewRect();
	return {view.x, view.y - m_bar.Offset(), view.w, view.h};
}

float ScrollArea::ContentFraction() const {
	float maxBottom = 1.0f;
	for (const auto& child : Children())
		if (child->visible)
			maxBottom = std::max(maxBottom, child->bounds.y + child->bounds.h);
	return maxBottom;
}

float ScrollArea::MaxScroll() const {
	return (ContentFraction() - 1.0f) * ViewRect().h;
}

void ScrollArea::ScrollIntoView(const Widget& child) {
	const gfx::Rect view = ViewRect();
	if (view.h <= 0.0f) return;
	// The child's rect is already shifted by the current scroll; undo that to get
	// where it sits in the CONTENT, then move the window the shortest way.
	const float scroll = m_bar.Offset();
	const float top = child.Pixel().y - view.y + scroll;
	const float bottom = top + child.Pixel().h;
	if (top < scroll) m_bar.SetOffset(top);
	else if (bottom > scroll + view.h) m_bar.SetOffset(bottom - view.h);
	m_bar.Clamp(MaxScroll());
}

// The bar sits in the reserved gutter, a hair in from the area's edge.
ScrollBar::Span ScrollArea::BarSpan() const {
	return ScrollBar::SpanIn(Pixel(), Rem(0.08f), Rem(), ViewRect().h, MaxScroll());
}

ScrollProbe ScrollArea::ProbeScroll() const {
	const ScrollBar::Span span = BarSpan();
	return {span, m_bar.Thumb(span), ViewRect(), m_bar.Offset(), WheelStep(), m_bar.Dragging()};
}

// Clamp the scroll and cache the clip before the children resolve against
// ContentRect() — which is the view box shifted by exactly this scroll.
void ScrollArea::LayoutSelf(UIContext&) {
	m_bar.Clamp(MaxScroll());
	m_clip = ViewRect();
	// Clip only while there is something to scroll: sliders draw their labels
	// slightly above their bounds, and a static page shouldn't crop them.
	m_clipping = MaxScroll() > 0.0f;
}

const gfx::Rect* ScrollArea::ChildClip() const {
	return m_clipping ? &m_clip : nullptr;
}

// Skip children scrolled fully out of the view, in every pass.
bool ScrollArea::ChildActive(const Widget& child) const {
	const float view = ViewRect().h;
	const float top = child.bounds.y * view - m_bar.Offset();
	const float bottom = (child.bounds.y + child.bounds.h) * view - m_bar.Offset();
	return bottom > 0.0f && top < view;
}

// Runs after the children (the tree walk's order), so an open popup — which
// consumes the mouse — can't be scrolled out from under the user.
void ScrollArea::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();
	// The thumb takes a pointer nothing under it claimed - a drag under way it
	// follows wherever the pointer goes.
	const ScrollBar::Span bar = BarSpan();
	if (m_bar.UpdatePointer(*input, bar, !ctx.IsMouseConsumed())) ctx.ConsumeMouse();
	if (!bar.Scrolls()) return;

	// Mouse wheel anywhere over the area — including over the controls INSIDE
	// it, which is the whole point of the wheel having its own claim: a slider
	// under the cursor wants the click, not the scroll.
	if (!ctx.IsWheelConsumed() && Pixel().Contains(mx, my) &&
		input->WheelDelta() != 0.0f) {
		m_bar.Wheel(input->WheelDelta(), WheelStep(), bar.maxScroll);
		ctx.ConsumeWheel();
	}
}

// The track sits in the reserved gutter, outside ContentRect, so drawing it
// before the children (the tree walk's order) never puts it under them.
void ScrollArea::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	m_bar.Draw(batch, ctx.GetTheme(), BarSpan());
}

// --- TabControl ------------------------------------------------------------

size_t TabControl::AddTab(std::string label) {
	// The page fills ContentRect() (the area below the strip) and is this
	// control's Nth child, matching the tab's index.
	ScrollArea* page = Add<ScrollArea>(gfx::Rect{0, 0, 1, 1});
	page->debugName = "TabPage";
	m_tabs.push_back({std::move(label), page});
	return m_tabs.size() - 1;
}

void TabControl::SetActiveTab(int index) {
	if (index >= 0 && index < static_cast<int>(m_tabs.size())) m_active = index;
}

gfx::Rect TabControl::ContentRect() const { return PageRect(); }

void TabControl::LayoutSelf(UIContext& ctx) {
	LayoutStrip(ctx); // size the strip + control before any rect math
	// Only the active tab's page takes part in the walk.
	for (size_t i = 0; i < m_tabs.size(); ++i)
		m_tabs[i].page->visible = static_cast<int>(i) == m_active;
}

void TabControl::LayoutStrip(UIContext&) {
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();
	const float count = static_cast<float>(std::max<size_t>(m_tabs.size(), 1));
	const float evenW = px.w / count;
	const float padX = Rem(0.8f); // breathing room each side of the label
	m_tabWidths.resize(m_tabs.size());
	float total = 0.0f;
	for (size_t i = 0; i < m_tabs.size(); ++i) {
		// Never below the even split, so short labels keep the original look;
		// a long one (e.g. "Controls") widens just its own tab.
		m_tabWidths[i] = std::max(evenW, font.MeasureWidth(m_tabs[i].label) + 2.0f * padX);
		total += m_tabWidths[i];
	}
	// Grow the control to the strip total and recenter on the authored center,
	// so it expands symmetrically rather than off to one side.
	m_effRect = {px.x - (total - px.w) * 0.5f, px.y, total, px.h};
}

gfx::Rect TabControl::TabRect(size_t index) const {
	// Fallback to an even split before LayoutStrip has run (e.g. first frame).
	if (m_tabWidths.size() != m_tabs.size()) {
		const gfx::Rect& px = Pixel();
		const float tabW = px.w / static_cast<float>(std::max<size_t>(m_tabs.size(), 1));
		return {px.x + tabW * static_cast<float>(index), px.y, tabW, m_tabHeight * px.h};
	}
	float x = m_effRect.x;
	for (size_t i = 0; i < index; ++i) x += m_tabWidths[i];
	return {x, m_effRect.y, m_tabWidths[index], m_tabHeight * m_effRect.h};
}

gfx::Rect TabControl::PageRect() const {
	const gfx::Rect base = m_effRect.w > 0.0f ? m_effRect : Pixel();
	const float stripH = m_tabHeight * base.h;
	return {base.x, base.y + stripH, base.w, base.h - stripH};
}

// The pages (and everything on them) are walked by the tree before this runs,
// so the strip only gets the mouse when nothing on the page claimed it.
void TabControl::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hover = -1;
	if (ctx.IsMouseConsumed()) return;
	const float mx = input->MouseX(), my = input->MouseY();
	for (size_t i = 0; i < m_tabs.size(); ++i) {
		if (!TabRect(i).Contains(mx, my)) continue;
		m_hover = static_cast<int>(i);
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_active = static_cast<int>(i);
		break;
	}
}

// Frame and strip only — the active page and its widgets are children, drawn
// by the tree straight after this.
void TabControl::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();

	// Page frame first so the active tab can open into it.
	const gfx::Rect page = PageRect();
	const bool skinned = PanelSkin(ctx) != nullptr;
	DrawPanelFace(ctx, batch, page);

	for (size_t i = 0; i < m_tabs.size(); ++i) {
		const gfx::Rect rect = TabRect(i);
		const bool active = static_cast<int>(i) == m_active;
		if (skinned) {
			// Skinned tabs are button faces riding the page's top edge; the
			// flat mode's border-erase trick can't merge a texture, so the
			// active tab reads by its held state + accent label instead.
			DrawButtonFace(batch, font, rect, "", theme,
						   static_cast<int>(i) == m_hover, active, true,
						   ctx.GetSkin());
		} else {
			batch.DrawRect(rect, active ? theme.panel
										: (static_cast<int>(i) == m_hover
											   ? theme.controlHot
											   : theme.control));
			DrawBorder(batch, rect, theme.panelBorder);
			if (active) // erase the tab's bottom edge and the page's top border
				batch.DrawRect({rect.x + 1, rect.y + rect.h - 1, rect.w - 2, 2},
							   theme.panel);
		}

		const std::string& label = m_tabs[i].label;
		const float textW = font.MeasureWidth(label);
		font.Draw(batch, label, rect.x + (rect.w - textW) * 0.5f,
				  rect.y + (rect.h - font.Height()) * 0.5f,
				  active ? theme.accent : theme.text);
	}
}

// --- Repeater ------------------------------------------------------------

// Grow the pool to the live count, then place and reveal exactly that many.
// Runs before the tree lays the children out, so the bounds set here are the
// ones they resolve with this frame. Growing BUILDS widgets, so a repeater whose
// count can rise in play is Warm()ed to its ceiling when it is made (the party
// bar's effect strip, code-review C219); here growth is the cold path.
void Repeater::LayoutSelf(UIContext&) {
	m_live = m_count ? m_count() : 0;
	while (Children().size() < m_live) {
		std::unique_ptr<Widget> child = m_factory(Children().size());
		if (!child) break; // factory declined — don't spin
		AddChild(std::move(child));
	}
	m_live = std::min(m_live, Children().size());
	const auto& kids = Children();
	for (size_t i = 0; i < kids.size(); ++i) {
		Widget& child = *kids[i];
		child.visible = i < m_live;
		if (child.visible && m_place) child.bounds = m_place(i);
	}
}

void Repeater::Warm(size_t n) {
	while (Children().size() < n) {
		std::unique_ptr<Widget> child = m_factory(Children().size());
		if (!child) break;
		child->visible = false; // LayoutSelf reveals it once the count reaches it
		AddChild(std::move(child));
	}
}

} // namespace dungeon::ui
