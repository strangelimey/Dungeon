// ============================================================================
// Game/DoorInspector.h — the editor's per-INSTANCE door editor.
//
// A concrete InstanceInspector (see that header). A door's orientation is the
// doorway's own (auto-detected at placement), so there is no Facing row. The
// body edits the door's AUTHORED state: Open (the panel slides live as the
// checkbox flips, and the record's open= param follows) and the key item it
// requires (items.cat entries with category=key; a keyed door opens to the
// party's click only while a member carries the item — see
// DungeonWorld::ToggleDoorAhead — while wired buttons bypass the lock).
// Save persists the level; Close/Esc reverts.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class DoorInspector : public InstanceInspector {
public:
	struct Config {
		int x = 0, z = 0;
		bool open = false;   // authored initial state (the live door follows)
		std::string key;     // items.cat id required to open by hand ("" = none)
		std::string name;    // button-target id ("" = unwired); record-safe chars
	};

	DoorInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
		: InstanceInspector(device, fonts) {}

	// `keys` are the selectable key items as (id, display) pairs; the dialog
	// prepends the "None" row itself.
	void Open(const Config& cfg, std::vector<std::pair<std::string, std::string>> keys,
			  PreviewSpec preview = {});

	// Push the working state to the live door + its .ent record (both edits are
	// in-memory until savemap, like every other editor edit).
	std::function<void(const Config&)> onApply;
	std::function<void()> onSave; // persist the level (.ent)

protected:
	std::string Title() const override;
	gfx::Rect Panel() const override { return {0.31f, 0.21f, 0.40f, 0.54f}; }
	// No facing row: the doorway's flanking walls fix the orientation.
	std::vector<Direction> FacingChoices() const override { return {}; }
	void BuildContent(ui::Stack& content) override;
	void ApplyLive() override {} // no common-strip edits (no facing)
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::pair<std::string, std::string>> m_keys; // (id, display)
};

} // namespace dungeon::game
