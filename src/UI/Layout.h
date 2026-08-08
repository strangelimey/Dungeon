// ============================================================================
// UI/Layout.h — containers that COMPUTE their children's areas.
//
// Widget.h gives every widget an area: its Pixel() rect, resolved from bounds
// that are fractions of its parent. Nothing in that model stops two SIBLINGS
// from being authored on top of one another, and hand-authored fractions drift
// the moment a font, a language or a panel size changes — which is how a
// dialog's title ended up under its first row and a checkbox's label ended up
// beneath the preview pane. Nudging those fractions fixes one dialog and
// leaves the next one to be found by eye.
//
// A Stack removes the possibility instead of policing it. Rows are added in
// order and their positions are COMPUTED at layout time, each taking the extent
// it asked for out of what is left. Two rows of one Stack cannot overlap — at
// any window size, in any language, at any font scale — because no authoring
// site ever writes their coordinates. What a site DOES write is how much room a
// row needs, which is the part a human can actually get right.
//
// Extents are typographic (UI/Units.h): Len::Fixed(n) is n REM, so a row sized
// to the text it holds tracks the window like every other typographic detail,
// and Len::Fill(w) shares whatever the fixed rows left over. The normal shape
// is fixed chrome top and bottom with one filling middle.
//
// A Stack nests: a row can itself be a horizontal Stack (a label beside a
// control beside a toggle), and the same guarantee holds along that axis. What
// it does NOT police is a child that draws OUTSIDE its own area — a Label whose
// text is wider than the room it was given still paints over its neighbour.
// That is what Widget::InkRect and the `uioverlap` audit (UI/TreeInspector.h)
// are for: the Stack makes the areas disjoint, the audit proves the drawing
// stayed inside them.
// ============================================================================
#pragma once

#include "UI/Widget.h"

#include <vector>

namespace dungeon::ui {

// How much of the stacking axis one row takes.
struct Len {
	float rem = 0.0f;  // fixed extent, in rem (0 unless Fixed)
	float fill = 0.0f; // share of the leftover (0 unless Fill)

	// n rem along the axis — the row is sized to the type it holds. One line of
	// text is 1.25rem (Font::LineAdvance); a control with comfortable air around
	// it is ~1.75rem (UI/Units.h).
	static Len Fixed(float rem) { return {rem, 0.0f}; }
	// A share of what the fixed rows left over, weighted against the other
	// fills. One Fill row takes everything that remains.
	static Len Fill(float weight = 1.0f) { return {0.0f, weight}; }
};

// A plain container: no look of its own, just an area for children to resolve
// against. What a Stack row holds when the row IS the sub-layout.
class Box : public Widget {
public:
	explicit Box(const gfx::Rect& rect) { bounds = rect; }
};

// Lays its children out along one axis, in add order, each getting the full
// extent of the cross axis. See the header comment for why this exists.
class Stack : public Widget {
public:
	explicit Stack(const gfx::Rect& rect, bool horizontal = false)
		: horizontal(horizontal) {
		bounds = rect;
	}

	// Adds the next row. `len` is its extent along the stacking axis; the rest
	// of the arguments are the widget's own, minus the leading rect every
	// control takes — a Stack row does not get to say where it is.
	template <typename T, typename... Args>
	T* Row(Len len, Args&&... args) {
		T* row = Add<T>(gfx::Rect{0, 0, 1, 1}, std::forward<Args>(args)...);
		m_lens.push_back(len);
		return row;
	}

	// Reserves an empty row — a gap between groups, or a hole left for
	// something drawn outside the tree (which then has an area the audit can
	// see, instead of being invisible to the layout).
	Box* Space(Len len) { return Row<Box>(len); }

	bool horizontal = false;
	float gapRem = 0.0f; // space between rows
	float padRem = 0.0f; // inset around the whole stack

	gfx::Rect ContentRect() const override;

protected:
	void LayoutSelf(UIContext& ctx) override;

private:
	std::vector<Len> m_lens; // parallel to Children(), by add order
};

} // namespace dungeon::ui
