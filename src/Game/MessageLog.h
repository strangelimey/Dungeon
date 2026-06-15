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
// Self-contained: it computes its own rect from the context's window size each
// frame (full width, bottom-anchored) rather than its normalized bounds,
// because the height animates. Time-based work (fades, height/opacity easing)
// runs in Tick(dt), called once per frame; Update(ctx) handles hover, the wheel
// scroll, and the restore-button click. AddLine/Clear match ui::TextOutput so
// it drops in where the old log lived.
// ============================================================================
#pragma once

#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <deque>
#include <string>

namespace dungeon::game {

class MessageLog : public ui::Widget {
public:
	MessageLog() = default;

	void AddLine(std::string line);
	void Clear();

	// Advances per-message fades and the height/opacity animation. Drive once
	// per frame with the real frame dt (UI animation, not world time).
	void Tick(float dt);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Caption for the translucent restore button (the UI layer has no language
	// table, so the owner localizes it).
	std::string restoreLabel = "Log";

private:
	struct Msg {
		std::string text;
		float age = 0.0f; // seconds since added (frozen while expanded)
	};

	gfx::Rect FooterRect(ui::UIContext& ctx) const;  // animated, bottom-anchored
	gfx::Rect RestoreRect(ui::UIContext& ctx) const; // small bottom-left button
	float MsgAlpha(const Msg& msg) const;            // per-message fade [0,1]

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
