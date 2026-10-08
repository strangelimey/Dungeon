// ============================================================================
// UI/ScrollBar.h - the ONE scrollbar: a track down the right of a scrolling
// box, a thumb sized by how much of the content shows, its drag, the wheel's
// step and its draw (code-review C127).
//
// NOT a widget. ScrollArea holds one, and so does DropDown's open list, which
// draws in the OVERLAY pass and so cannot host a child ScrollArea: the list
// used to carry a line-for-line copy of ScrollArea's bar - track, thumb, drag
// and draw, down to the thumb's minimum - after docs/ui-hierarchy.md had called
// SlotList's copy the last. An owner hands the bar a Span each frame (where the
// track runs, how tall the view is, how far the content scrolls) and decides
// for itself what claims the pointer and the wheel - a list open over a page
// takes both whatever the bar says; a page takes the wheel only when nothing
// under the pointer did. The bar keeps the offset and the drag.
//
// The wheel's STEP is the owner's too, and the two differ on purpose: an open
// list moves one row a notch, a page 1.75 rem.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"

namespace dungeon {
class Input;
}

namespace dungeon::ui {

struct Theme;

class ScrollBar {
public:
	// The bar's width and its thumb's shortest, in REM (UI/Units.h), so the
	// chrome tracks the text it scrolls.
	static constexpr float kWidthRem = 0.35f;
	static constexpr float kMinThumbRem = 0.9f;

	// One frame's geometry, in pixels.
	struct Span {
		gfx::Rect track;        // where the bar runs
		float view = 0.0f;      // the visible height of what scrolls
		float maxScroll = 0.0f; // how far it scrolls; <= 0 = nothing to scroll, no bar
		float minThumb = 0.0f;  // the thumb's shortest
		bool Scrolls() const { return maxScroll > 0.0f; }
	};
	// A bar kWidthRem wide down the right edge of `box`, `inset` px in from its
	// top, right and bottom edges, for a view `view` tall that scrolls
	// `maxScroll`. `rem` is the owner's (Widget::Rem()).
	static Span SpanIn(const gfx::Rect& box, float inset, float rem, float view,
					   float maxScroll);

	// Pixels scrolled down. SetOffset is unclamped (a list reopening on its
	// selection, a page handed back its old position); the next Clamp holds it.
	float Offset() const { return m_offset; }
	void SetOffset(float pixels) { m_offset = pixels; }
	void Clamp(float maxScroll);

	// The thumb: as long as the view's share of the content (never under
	// minThumb), as far down the track as the offset is down the content.
	gfx::Rect Thumb(const Span& span) const;

	// The pointer, once a frame: a drag ends when the button is up, the thumb
	// lights under the pointer, a press on it starts a drag and a drag follows
	// the pointer. `mayTake` says the pointer is this bar's to take; a drag
	// under way is followed regardless. True while the bar holds the pointer -
	// over the thumb or dragging it - for the owner to consume. With nothing
	// to scroll the bar lets go of everything.
	bool UpdatePointer(const Input& input, const Span& span, bool mayTake);
	// A wheel turn of `delta` notches (positive = up), `step` px a notch.
	void Wheel(float delta, float step, float maxScroll);
	// Drops a drag and the hover (the list closing under it).
	void Release();

	bool Hot() const { return m_hot; }
	bool Dragging() const { return m_dragging; }

	// The track and the thumb, lit while hovered or dragged. Nothing when the
	// span does not scroll.
	void Draw(gfx::SpriteBatch& batch, const Theme& theme, const Span& span) const;

private:
	float m_offset = 0.0f;
	bool m_hot = false;
	bool m_dragging = false;
	float m_grab = 0.0f; // the pointer's offset within the thumb while dragging
};

// What a scrolling control shows of its bar right now - for a check that drives
// one (`scrollpoke`, Game/ScrollPoke.h): the span, the thumb, the box its rows
// scroll in, the offset, and one wheel notch.
struct ScrollProbe {
	ScrollBar::Span span;
	gfx::Rect thumb;
	gfx::Rect box;
	float offset = 0.0f;
	float step = 0.0f;
	bool dragging = false;
};

} // namespace dungeon::ui
