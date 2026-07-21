// ============================================================================
// Game/NicheInspector.h — the editor's per-INSTANCE wall-niche editor.
//
// A concrete InstanceInspector (see that header), opened by Select-clicking a
// niche's cell. A niche's wall is fixed at placement, so there is no Facing row.
// The body edits the niche's AUTHORED state: its shape Type (wallfeatures.cat),
// whether it Starts closed (a secret niche, blank wall until a button whose
// target= names it opens it — DungeonWorld::ToggleNichesNamed), and that Name.
// Save persists the level (.map); Close/Esc reverts.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class NicheInspector : public InstanceInspector {
public:
	struct Config {
		int x = 0, z = 0;                  // the niche's floor cell
		Direction wall = Direction::North; // the wall face it is carved into
		std::string type = "niche";        // wallfeatures.cat id (shape)
		bool hidden = false;               // starts closed (secret)
		std::string name;                  // button-target id ("" = unwired)
	};

	explicit NicheInspector(gfx::GraphicsDevice& device) : InstanceInspector(device) {}

	// `types` are the selectable niche shapes as (id, display) pairs.
	void Open(const Config& cfg, std::vector<std::pair<std::string, std::string>> types,
			  PreviewSpec preview = {});

	std::function<void(const Config&)> onApply; // push to the live niche + record
	std::function<void()> onSave;               // persist the level (.map)

protected:
	std::string Title() const override;
	gfx::Rect Panel() const override { return {0.33f, 0.20f, 0.34f, 0.54f}; }
	std::vector<Direction> FacingChoices() const override { return {}; } // wall is fixed
	void BuildContent(const gfx::Rect& content) override;
	void ApplyLive() override {}
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::pair<std::string, std::string>> m_types; // (id, display)
};

} // namespace dungeon::game
