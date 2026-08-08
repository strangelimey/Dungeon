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
// Input consumption: a widget that uses the mouse calls ConsumeMouse() and
// everything visited later ignores the same event. The WHEEL is a SEPARATE
// claim (UIContext::ConsumeWheel) — a control that merely wants the click under
// the cursor must not also stop the page beneath it scrolling.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"
#include "UI/FontLibrary.h"

#include <memory>
#include <optional>
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

	// `inheritedRole`/`inheritedScale` are the face and size this widget's PARENT
	// resolved; unset/1 at the root, where the context's own are used. Callers
	// outside the walk pass nothing — the recursion supplies them. The ROLE is
	// threaded rather than the resolved Font* because a child that changes only
	// its SIZE still has to re-resolve its parent's face at the new size.
	void Layout(const gfx::Rect& container, UIContext& ctx,
				std::optional<FontRole> inheritedRole = std::nullopt,
				float inheritedScale = 1.0f);
	void Update(UIContext& ctx);
	void Draw(UIContext& ctx, gfx::SpriteBatch& batch);
	void DrawOverlay(UIContext& ctx, gfx::SpriteBatch& batch);

	// --- state --------------------------------------------------------------

	gfx::Rect bounds; // fractions of the parent's ContentRect (0..1)
	bool visible = true;
	// Optional label for the tree inspector; the class name is the fallback.
	const char* debugName = nullptr;

	// Which typeface this widget's text is drawn in, INHERITED by its whole
	// subtree exactly like CSS font-family: unset means "whatever my parent
	// resolved", and the root falls back to the context's own role. Set it on a
	// container to re-face everything inside it — a scroll panel in Script, a
	// heading row in Display — without touching a single leaf.
	//
	// This changes the FACE, not the layout grid: see Rem/Em below.
	std::optional<FontRole> fontRole;

	// How BIG this widget's text is, as a multiple of the context's authored
	// size — INHERITED by its whole subtree exactly like fontRole. Unset means
	// "whatever my parent resolved", and the root is 1 = the document size. Set
	// it to lift a heading row, or to hold one control back while everything
	// around it grows (the editor dialogs enlarge their form text but pin the
	// numeric readouts, which are sized to their digits).
	//
	// Where fontRole changes the FACE this changes the SIZE, and the pairing is
	// deliberate: a role carries no size, so anything bigger than the document
	// has to say how much bigger. It moves em — detail belonging to THIS
	// widget's own text follows it — but deliberately NOT rem, which stays the
	// document's grid so enlarging one label cannot re-space its neighbours.
	std::optional<float> fontScale;

	// The pixel rect resolved by the most recent Layout().
	const gfx::Rect& Pixel() const { return m_pixel; }

	// --- typographic units (UI/Units.h has the model) -----------------------
	// Both are captured at Layout so even a const rect helper can ask without
	// being handed a UIContext. Use them for the DETAIL inside a control —
	// padding, row heights, a scrollbar's width — while bounds stay [0..1] of
	// the parent.
	//
	// 1rem = the CONTEXT's root font size. It is the document's grid and does
	// NOT move when a widget changes role: if it did, re-facing one label would
	// silently re-space every control around it.
	// 1em  = THIS widget's own font size. Detail that belongs to the widget's
	// own text — the gap beside a Script label — should follow the face it sits
	// next to. Identical to rem for any widget that did not change role.
	float Rem(float n = 1.0f) const { return m_rem * n; }
	float Em(float n = 1.0f) const { return m_em * n; }

	// The font this widget draws in: its own role's, or the nearest ancestor
	// that set one, or the context's. Resolved every Layout. A widget's DrawSelf
	// uses THIS rather than ctx.GetFont(), which is what makes fontRole
	// inherit — ctx.GetFont() is the document root and ignores roles.
	const Font& TextFont() const { return *m_font; }
	// What fontRole actually resolved to, for the tree inspector.
	FontRole ResolvedRole() const { return m_resolvedRole; }

	// The rect CHILDREN resolve against. Override to inset the children (a
	// padded panel), to move them (a scrolled page), or to hand back a
	// sub-region (a tab's page below its strip). Defaults to the whole rect.
	virtual gfx::Rect ContentRect() const { return m_pixel; }

	// The rect this widget actually PAINTS INTO — its area as the eye sees it,
	// where Pixel() is its area as the layout sees it. They differ whenever a
	// widget draws text it was not given room for: a Label draws its whole
	// string from its top-left corner however narrow its bounds, so a label too
	// long for its row lands on whatever sits to the right of it. Overriding
	// this is what lets the `uioverlap` audit (UI/TreeInspector.h) see that as
	// the collision it is instead of passing the pair as disjoint.
	//
	// Defaults to Pixel(); override in any widget whose drawing is measured
	// rather than bounded.
	virtual gfx::Rect InkRect() const { return m_pixel; }

	// True if this widget clips its children (a scroll area, a tab page). Its
	// children are then MEANT to run past its bounds — the excess paints on
	// nothing — which is why the overlap audit does not count that as an escape.
	bool ClipsChildren() const { return ChildClip() != nullptr; }

	// Opts this widget out of the overlap audit: it is deliberately UNDER (or
	// over) its siblings — a Panel used as a backdrop for the rows added after
	// it, a popup that draws in the overlay pass. Not a licence to overlap by
	// accident; every use should be a thing that is meant to be layered.
	bool overlapOk = false;

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

	// The rect this widget's `bounds` were resolved against — its parent's
	// ContentRect, from the most recent Layout. A container that sizes ITSELF
	// from its content needs it: `bounds` is a fraction, so writing a measured
	// height back means dividing by the height it was a fraction OF.
	const gfx::Rect& ContainerRect() const { return m_container; }

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
	gfx::Rect m_container{}; // what bounds resolved against (ContainerRect)
	float m_rem = 16.0f; // context root font size, refreshed every Layout
	float m_em = 16.0f;  // THIS widget's font size, ditto
	// Resolved every Layout, so it is valid for the Update/Draw that follow.
	// Widgets skipped by ChildActive are never updated or drawn either, so a
	// stale pointer here is unreachable.
	const Font* m_font = nullptr;
	FontRole m_resolvedRole = FontRole::Body;
	std::vector<std::unique_ptr<Widget>> m_children;
};

} // namespace dungeon::ui
