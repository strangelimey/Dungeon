// ============================================================================
// Game/ButtonInspector.h — the editor's per-INSTANCE button (lever) editor.
//
// A concrete InstanceInspector (see that header). A button's orientation is
// its mount wall (auto-picked at placement), so there is no Facing row for
// now. The body edits the wiring: a Target dropdown over the ACTIVE level's
// door NAMES (set in the door inspector's Name field) plus None — pressing
// the button toggles every door whose name matches. Then its FLAGS
// (flags.cat): one it waits on before it will move at all, and what a press
// does to one (sets / clears / toggles it). Save persists the level;
// Close/Esc reverts.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"
#include "Game/WorldMap.h" // FlagOp

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class ButtonInspector : public InstanceInspector {
public:
	struct Config {
		int x = 0, z = 0;
		std::string target; // wired door name ("" = unwired)
		std::string needs;  // the flag it waits on ("" = none)
		std::string sets;   // the flag a press acts on ("" = none)
		FlagOp op = FlagOp::None;
	};

	ButtonInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
		: InstanceInspector(device, fonts) {}

	// `doorNames` are the level's wired-up door names (the dropdown's choices);
	// `flags` the project's flags.
	void Open(const Config& cfg, std::vector<std::string> doorNames, FlagChoices flags,
			  PreviewSpec preview = {});

	// Push the working target to the live button + its .ent record (in-memory
	// until savemap, like every other editor edit).
	std::function<void(const Config&)> onApply;
	std::function<void()> onSave; // persist the level (.ent)

protected:
	std::string Title() const override;
	gfx::Rect Panel() const override { return {0.29f, 0.12f, 0.44f, 0.74f}; }
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
	FlagChoices m_flags;
};

} // namespace dungeon::game
