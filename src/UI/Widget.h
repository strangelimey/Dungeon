// ============================================================================
// UI/Widget.h — base class for all controls, and the CONTROL TREE.
//
// Every widget owns its children, and a child's `bounds` are fractions (0..1)
// of its PARENT's ContentRect() — never of the window. Layout resolves the
// whole tree recursively from the root (the window) down, so moving or resizing
// a parent carries every descendant with it and no authoring site ever
// multiplies a parent chain out by hand. See docs/ui-hierarchy.md.
//
// The tree walk lives HERE, once: Layout/Update/Draw/DrawOverlay are
// non-virtual. A subclass overrides the *Self methods and handles ONLY itself,
// so a container physically cannot forget to visit its children, and the walk
// order is fixed in one place:
//   Layout  self, then children (each against this widget's ContentRect)
//   Update  children in REVERSE add order, then self — so the topmost child
//           owning a pixel claims the mouse before its parent sees it
//   Draw    self, then children in add order (painter's: parent behind)
// Input consumption is unchanged: a widget that uses the mouse calls
// ConsumeMouse() and everything visited later ignores the same event.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"

#include <memory>
#include <utility>
#include <vector>

namespace dungeon::ui {

class UIContext;

// Clips drawing to `rect` INTERSECTED with whatever clip the draw walk already
// has in force, and restores that clip on destruction. A widget that clips its
// own content (a text log, a list of rows) uses this rather than calling
// SpriteBatch::SetScissor directly — a bare SetScissor(nullptr) would drop an
// ancestor's clip and let the content spill out of a scrolled page.
class ScopedClip {
public:
	ScopedClip(gfx::SpriteBatch& batch, const gfx::Rect& rect);
	~ScopedClip();

	ScopedClip(const ScopedClip&) = delete;
	ScopedClip& operator=(const ScopedClip&) = delete;

private:
	gfx::SpriteBatch& m_batch;
	const gfx::Rect* m_outer;
	gfx::Rect m_outerRect;
};

class Widget {
public:
	virtual ~Widget() = default;

	// --- authoring ----------------------------------------------------------

	// Creates a CHILD widget; this widget owns it. The returned pointer follows
	// the same contract as UIContext::Add — valid until the subtree is rebuilt.
	template <typename T, typename... Args>
	T* Add(Args&&... args) {
		auto widget = std::make_unique<T>(std::forward<Args>(args)...);
		T* raw = widget.get();
		m_children.push_back(std::move(widget));
		return raw;
	}

	// Adopts an already-built child (what a Repeater's factory hands back).
	Widget* AddChild(std::unique_ptr<Widget> child) {
		Widget* raw = child.get();
		m_children.push_back(std::move(child));
		return raw;
	}

	// Destroys every child. Same rule as UIContext::Clear: raw pointers handed
	// out by Add are dead afterwards, and it must NEVER run from inside a
	// callback that is on the Update stack — defer the rebuild a frame (the
	// m_pendingLanguage / m_videoRebuildPending pattern).
	void ClearChildren() { m_children.clear(); }

	const std::vector<std::unique_ptr<Widget>>& Children() const {
		return m_children;
	}

	// --- tree walk (non-virtual — see the header comment) --------------------

	void Layout(const gfx::Rect& container, UIContext& ctx);
	void Update(UIContext& ctx);
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch);
	void DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch);

	// --- state --------------------------------------------------------------

	gfx::Rect bounds; // fractions of the parent's ContentRect (0..1)
	bool visible = true;
	// Optional label for the tree inspector; the class name is the fallback.
	const char* debugName = nullptr;

	// The pixel rect resolved by the most recent Layout().
	const gfx::Rect& Pixel() const { return m_pixel; }

	// --- typographic units (UI/Units.h has the model) -----------------------
	// 1rem = the context's root font size, captured at Layout so that even a
	// const rect helper can ask for it without being handed a UIContext. Use
	// these for the DETAIL inside a control — padding, row heights, a
	// scrollbar's width — while bounds stay [0..1] of the parent.
	float Rem(float n = 1.0f) const { return m_rem * n; }
	float Em(float n = 1.0f) const { return Rem(n); }

	// The rect CHILDREN resolve against. Override to inset the children (a
	// padded panel), to move them (a scrolled page), or to hand back a
	// sub-region (a tab's page below its strip). Defaults to the whole rect.
	virtual gfx::Rect ContentRect() const { return m_pixel; }

protected:
	// --- what a subclass implements: itself, never its children --------------

	// Runs BEFORE this widget's children are updated — the container's first
	// look at the mouse, while ConsumeMouse() still reflects only what lies
	// OUTSIDE this subtree. A panel whose children cover most of it latches its
	// own hover here (asking afterwards would see its own child's claim and
	// read as "not hovered"). Claiming the mouse here takes it from the
	// children, so only a modal should.
	virtual void UpdateBeforeChildren(UIContext&) {}
	virtual void UpdateSelf(UIContext&) {}
	virtual void DrawSelf(UIContext&, gfx::SpriteBatch&) {}
	virtual void DrawOverlaySelf(UIContext&, gfx::SpriteBatch&) {}
	// Runs after this widget's own pixel rect is resolved and BEFORE its
	// children are laid out — where a container sizes itself from the font,
	// clamps a scroll offset, or assigns its children's bounds by index.
	virtual void LayoutSelf(UIContext&) {}

	// Lets a container skip a child in every pass (scrolled out of a page,
	// past a repeater's live count) without touching the child's own `visible`.
	virtual bool ChildActive(const Widget&) const { return true; }

	// Non-null clips this widget's CHILDREN to the given pixel rect while they
	// draw (a scrolling page). Nesting is handled by the walk: an inner clip
	// intersects the one already in force and the outer is restored after, so a
	// scroll area inside a scrolled page cannot widen its parent's clip.
	virtual const gfx::Rect* ChildClip() const { return nullptr; }

private:
	gfx::Rect m_pixel{};
	float m_rem = 16.0f; // root font size in pixels, refreshed every Layout
	std::vector<std::unique_ptr<Widget>> m_children;
};

} // namespace dungeon::ui
