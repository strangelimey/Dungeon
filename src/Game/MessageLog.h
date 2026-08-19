// ============================================================================
// Game/MessageLog.h — the full-width message footer (the "You descend into
// the dungeon..." log).
//
// Anchored flush to the bottom of the screen and spanning the full width.
// Collapsed it shows ~2 lines; hovering expands it upward so the player can
// scroll back through history (aging freezes while expanded so messages stay
// readable). Each message holds at full opacity, then fades over a few seconds;
// once every message has faded and the pointer is away, the whole footer fades
// out, cross-fading into a small translucent button that brings it back. A new
// line (AddLine) fades the footer back in automatically.
//
// Screen-anchored: the footer's rect comes from the window (full width, bottom
// edge) and its height animates, so LayoutSelf writes that rect back into
// `bounds` each frame rather than the bounds being authored. What it occupies
// is therefore what it claims — it used to sit at {0,0,1,1}, spanning the whole
// window while drawing in one corner, which made it the topmost widget under
// every pixel on screen (see docs/ui-hierarchy.md). Time-based work (fades,
// height/opacity easing) runs in Tick(dt), called once per frame; Update(ctx)
// handles hover, the wheel scroll, and the restore-button click. AddLine/Clear
// match ui::TextOutput so it drops in where the old log lived.
//
// PRINTING A MESSAGE ALLOCATES NOTHING (docs/message-allocation.md). The
// history is a FIXED RING whose slots are built once at construction, each
// holding its text in an inline loc::Line rather than a std::string — so a
// steady stream of messages neither grows a container nor reaches the heap,
// from the FIRST line rather than after a warm-up. A ring of owned strings
// would only have been free once every slot had grown big enough, which is a
// weaker property and a harder one to state. It matters because the
// steady-state allocation guard (Core/AllocTrack) reports any allocation in a
// settled frame, and a log that allocated per line made ordinary play — walking
// into a wall — look like a leak.
// ============================================================================
#pragma once

#include "Core/Loc.h"
#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <optional>
#include <string>
#include <vector>

namespace dungeon::game {

class MessageLog : public ui::Widget {
public:
	// Starts as an empty strip on the bottom edge; LayoutSelf sizes it from the
	// first frame on. (Never {0,0,1,1} — see the header note.)
	MessageLog() { bounds = {0.0f, 1.0f, 1.0f, 0.0f}; }

	// `color` tints the line (a character's identity color for lines about
	// them); unset draws the theme's text ink. Fades apply either way.
	//
	// Takes a VIEW and copies into the ring's own slot: the caller's text is
	// almost always a loc::Line living on its stack, so there is nothing to take
	// ownership of and nothing to allocate.
	void AddLine(std::string_view line, std::optional<Vec4> color = std::nullopt);
	void Clear();

	// Advances per-message fades and the height/opacity animation. Drive once
	// per frame with the real frame dt (UI animation, not world time).
	void Tick(float dt);

	void LayoutSelf(ui::UIContext& ctx) override;
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Caption for the translucent restore button (the UI layer has no language
	// table, so the owner localizes it).
	std::string restoreLabel = "Log";

private:
	struct Msg {
		loc::Line text;            // inline, so a slot never reaches the heap
		std::optional<Vec4> color; // line tint; unset = theme text ink
		float age = 0.0f;          // seconds since added (frozen while expanded)
	};

	static constexpr size_t kMaxLines = 200; // ring capacity = history depth

	gfx::Rect FooterRect(ui::UIContext& ctx) const;  // animated, bottom-anchored
	gfx::Rect RestoreRect(ui::UIContext& ctx) const; // small bottom-left button
	float MsgAlpha(const Msg& msg) const;            // per-message fade [0,1]
	// Faded out: only the restore button is live, and it is what `bounds` holds.
	bool Dormant() const { return m_chromeAlpha < 0.5f && !m_expanded; }

	// The ring, oldest-first: index 0 is the oldest line held, Count()-1 the
	// newest. Callers index in that order and never see the wrap, so the draw
	// and scroll maths read exactly as they did against the old deque.
	size_t Count() const { return m_count; }
	Msg& At(size_t i) { return m_ring[(m_head + i) % kMaxLines]; }
	const Msg& At(size_t i) const { return m_ring[(m_head + i) % kMaxLines]; }

	std::vector<Msg> m_ring = std::vector<Msg>(kMaxLines); // one allocation, ever
	size_t m_head = 0;  // ring index of the OLDEST line
	size_t m_count = 0; // lines held (saturates at kMaxLines)

	// Target footer heights as fractions of the window height (resolution-
	// independent, font-proportional); recomputed in Update, eased in Tick.
	float m_collapsedFrac = 0.0f;
	float m_expandedFrac = 0.0f;
	float m_heightFrac = 0.0f;   // animated current height (fraction of H)
	float m_chromeAlpha = 0.0f;  // animated footer opacity (1 shown, 0 dormant)
	float m_scroll = 0.0f;       // lines scrolled back (0 = newest)

	bool m_expanded = false;     // grown to show history (hover/restore)
	bool m_hovered = false;      // pointer over the footer this frame
	bool m_restoreHot = false;   // pointer over the restore button this frame
	float m_shrinkTimer = 0.0f;  // counts up once the pointer leaves
};

} // namespace dungeon::game
