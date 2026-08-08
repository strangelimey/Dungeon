// ============================================================================
// UI/Layout.cpp — see Layout.h.
// ============================================================================
#include "UI/Layout.h"

#include "UI/UIContext.h"

#include <algorithm>

namespace dungeon::ui {

gfx::Rect Stack::ContentRect() const {
	const gfx::Rect& px = Pixel();
	const float p = Rem(padRem);
	gfx::Rect content{px.x + p, px.y + p, std::max(px.w - 2 * p, 0.0f),
					  std::max(px.h - 2 * p, 0.0f)};
	// Content-sized: hand back the MEASURED extent, so the rows land correctly
	// on the frame they were measured on. The pixel rect gets there next frame,
	// once the parent has read the bounds this wrote.
	if (fitContent) {
		if (horizontal) content.w = m_fitExtent;
		else content.h = m_fitExtent;
	}
	return content;
}

void Stack::LayoutSelf(UIContext&) {
	// Runs after this widget's own rect is resolved and before its children are
	// laid out (Widget.h), which is exactly when the font — and therefore rem —
	// is known. That is the whole point: the rows are sized in type, at the size
	// the type will actually be drawn at, rather than in fractions guessed when
	// the dialog was written.
	const auto& kids = Children();
	const size_t n = std::min(kids.size(), m_lens.size());
	const float gap = Rem(gapRem);
	const float pad = Rem(padRem);

	// Fixed rows take their rem; the fills divide what is left. An invisible row
	// takes no room at all — a stack with an optional row closes over it rather
	// than leaving a hole (the facing strip a fixture without facings omits).
	size_t shown = 0;
	float fixed = 0.0f, fills = 0.0f;
	for (size_t i = 0; i < n; ++i) {
		if (!kids[i]->visible) continue;
		++shown;
		fixed += Rem(m_lens[i].rem);
		fills += m_lens[i].fill;
	}
	const float gaps = shown > 1 ? gap * static_cast<float>(shown - 1) : 0.0f;

	// Content-sized: the extent is what the rows add up to, and it goes back
	// into `bounds` so the container (a ScrollArea) knows how far to scroll.
	if (fitContent) {
		m_fitExtent = fixed + gaps;
		const gfx::Rect& box = ContainerRect();
		const float axis = horizontal ? box.w : box.h;
		if (axis > 0.0f) {
			const float frac = (m_fitExtent + 2 * pad) / axis;
			if (horizontal) bounds.w = frac;
			else bounds.h = frac;
		}
	}

	const gfx::Rect content = ContentRect();
	const float span = horizontal ? content.w : content.h;
	if (span <= 0.0f) return;
	const float free = std::max(span - fixed - gaps, 0.0f);
	// A stack whose FIXED rows want more than it has: shrink them all to fit
	// rather than letting the tail run out of the box. Overflowing is the one
	// outcome this class exists to prevent — a row past the end lands on
	// whatever the parent put after the stack (the level dialog's Save button,
	// under its own settings rows), and no sibling check can see it because the
	// colliding pair are not siblings. Squeezed rows look wrong; overrunning
	// ones look broken and hide their cause.
	// (A content-sized stack never squeezes: its span IS the sum of its rows.)
	const float squeeze =
		fitContent || fixed <= 0.0f
			? 1.0f
			: std::min(1.0f, std::max(span - gaps, 0.0f) / fixed);

	// Bounds are fractions of ContentRect, since that is what the walk resolves
	// the children against.
	float at = 0.0f;
	for (size_t i = 0; i < n; ++i) {
		Widget& child = *kids[i];
		if (!child.visible) continue;
		const float extent = m_lens[i].fill > 0.0f
								 ? free * (m_lens[i].fill / fills)
								 : Rem(m_lens[i].rem) * squeeze;
		child.bounds = horizontal
						   ? gfx::Rect{at / content.w, 0.0f, extent / content.w, 1.0f}
						   : gfx::Rect{0.0f, at / content.h, 1.0f, extent / content.h};
		at += extent + gap;
	}
}

} // namespace dungeon::ui
