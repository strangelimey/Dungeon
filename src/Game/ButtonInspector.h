// ============================================================================
// Game/ButtonInspector.h — the editor's per-INSTANCE button (lever) editor.
//
// A concrete InstanceInspector (see that header). A button's orientation is
// its mount wall (auto-picked at placement), so there is no Facing row for
// now. The body edits the wiring: a Target dropdown over the ACTIVE level's
// door NAMES (set in the door inspector's Name field) plus None — pressing
// the button toggles every door whose name matches. Save persists the level;
// Close/Esc reverts.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class ButtonInspector : public InstanceInspector {
public:
	struct Config {
		int x = 0, z = 0;
		std::string target; // wired door name ("" = unwired)
	};

	ButtonInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
		: InstanceInspector(device, fonts) {}

	// `doorNames` are the level's wired-up door names (the dropdown's choices).
	void Open(const Config& cfg, std::vector<std::string> doorNames,
			  PreviewSpec preview = {});

	// Push the working target to the live button + its .ent record (in-memory
	// until savemap, like every other editor edit).
	std::function<void(const Config&)> onApply;
	std::function<void()> onSave; // persist the level (.ent)

protected:
	std::string Title() const override;
	gfx::Rect Panel() const override { return {0.31f, 0.26f, 0.40f, 0.42f}; }
	// No facing row: the mount wall was auto-picked at placement.
	std::vector<Direction> FacingChoices() const override { return {}; }
	void BuildContent(ui::Stack& content) override;
	void ApplyLive() override {} // no common-strip edits
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::string> m_doorNames;
};

} // namespace dungeon::game
