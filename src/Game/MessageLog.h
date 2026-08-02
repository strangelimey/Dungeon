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
// ============================================================================
#pragma once

#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <deque>
#include <optional>
#include <string>

namespace dungeon::game {

class MessageLog : public ui::Widget {
public:
	// Starts as an empty strip on the bottom edge; LayoutSelf sizes it from the
	// first frame on. (Never {0,0,1,1} — see the header note.)
	MessageLog() { bounds = {0.0f, 1.0f, 1.0f, 0.0f}; }

	// `color` tints the line (a character's identity color for lines about
	// them); unset draws the theme's text ink. Fades apply either way.
	void AddLine(std::string line, std::optional<Vec4> color = std::nullopt);
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
		std::string text;
		std::optional<Vec4> color; // line tint; unset = theme text ink
		float age = 0.0f;          // seconds since added (frozen while expanded)
	};

	gfx::Rect FooterRect(ui::UIContext& ctx) const;  // animated, bottom-anchored
	gfx::Rect RestoreRect(ui::UIContext& ctx) const; // small bottom-left button
	float MsgAlpha(const Msg& msg) const;            // per-message fade [0,1]
	// Faded out: only the restore button is live, and it is what `bounds` holds.
	bool Dormant() const { return m_chromeAlpha < 0.5f && !m_expanded; }

	std::deque<Msg> m_msgs;

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
