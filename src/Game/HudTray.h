// ============================================================================
// Game/HudTray.h - the closed-panels tray (ui-updates Phase 8).
//
// Michael: "When a panel is minimized (movement, magic, etc.), instead of
// collapsing into a rectangle, it should be added to a new 'closed windows' set
// of buttons ... pressing it will bring the panel back and remove itself from
// the button list."
//
// The tray is a floating panel of its own (kHudPanelFields "tray": Ctrl-drag,
// snapping, scale, opacity, reset, like every other) holding one stone button
// per panel that CAN be minimized - built once, with the HUD - each showing
// only while its panel is minimized. A button's face is the panel's glyph
// (assets/ui/glyph_panel_<id>.png) and its tooltip the panel's name; clicking
// clears the flag, and the panel is back the next layout. The tray shows only
// while it has a button to show.
//
// A minimized panel the layout would not show anyway - Magic before anyone
// knows a symbol - gets no button: pressing it would bring back nothing.
// Nothing is rebuilt by any of it - a flag flips and the layout follows - so
// minimizing and restoring are free inside an armed frame (bar the settings
// save, which excuses itself).
// ============================================================================
#pragma once

#include "UI/Controls.h"
#include "UI/FloatingPanel.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class HudTray : public ui::Widget {
public:
	// `opacity` is the tray's own panel opacity (Settings -> UI), read live.
	explicit HudTray(const float* opacity);

	// A panel that can be minimized: its button, hidden until the panel is.
	// `onRestore` runs after the flag clears (the app saves).
	void AddPanel(const ui::FloatingPanel* panel, bool* hidden, const gfx::Texture* glyph,
				  std::string name, std::function<void()> onRestore);

	// How many buttons show right now.
	size_t ShownCount() const;
	// The tray's size in pixels for `count` buttons at this em.
	static Vec2 Size(size_t count, float em);

private:
	struct Entry {
		const ui::FloatingPanel* panel = nullptr;
		bool* hidden = nullptr;
		ui::Button* button = nullptr;
	};
	bool Wanted(const Entry& e) const;
	void LayoutSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	static constexpr float kPad = 0.8f;  // em: the face's inset, the docks' look
	static constexpr float kCell = 2.0f; // em: one button's side
	static constexpr float kGap = 0.3f;  // em: between buttons

	std::vector<Entry> m_entries; // built once; nothing grows it per frame
	const float* m_opacity;
};

} // namespace dungeon::game
