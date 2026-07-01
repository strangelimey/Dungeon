// ============================================================================
// Game/EntityInspector.h — the editor's per-INSTANCE monster inspector.
//
// A concrete InstanceInspector (see that header): the common Facing dropdown +
// modal chrome + Save/Close come from the base; this class adds the monster-
// specific content — a tabbed panel (ui::TabControl) editing the .ent overrides
// (asleep, leash, archetype + dependent params) and the patrol route. Edits
// apply live (onApply, every edit); Save persists the level .ent (onSave);
// Close/Esc reverts via onApply(original).
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"
#include "Game/MonsterAI.h" // ai::Archetype

#include <functional>
#include <string>
#include <vector>

namespace dungeon::ui {
class TabControl;
}

namespace dungeon::game {

class EntityInspector : public InstanceInspector {
public:
	// The placed monster being edited, keyed by its stable runtimeId. The behaviour
	// fields carry the EFFECTIVE values (per-instance override, else the type
	// default); the world turns edits back into overrides.
	struct Config {
		u32 runtimeId = 0;
		std::string type; // monster kind catalog id (the title)
		bool asleep = false;
		float leashRange = 0.0f;
		ai::Archetype archetype = ai::Archetype::Brute;
		float keepRange = 4.0f;
		float fleeBelow = 0.0f;
		std::string spell;
		Direction facing = Direction::South;
		int patrolCount = 0; // waypoints on the current route (display only)
	};

	explicit EntityInspector(gfx::GraphicsDevice& device) : InstanceInspector(device) {}

	void Open(const Config& cfg, const std::vector<std::string>& spellIds);

	std::function<void(const Config&)> onApply;
	std::function<void(const Config&)> onSave;
	// Patrol-route authoring (grid-click). onEditRoute closes the inspector and puts
	// the editor into route-laying mode for this monster; onClearRoute wipes it.
	std::function<void(u32 runtimeId)> onEditRoute;
	std::function<void(u32 runtimeId)> onClearRoute;

protected:
	std::string Title() const override;
	gfx::Rect Panel() const override { return {0.30f, 0.13f, 0.40f, 0.74f}; }
	void BuildContent(const gfx::Rect& content) override;
	void ApplyLive() override;
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;      // working copy
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::string> m_spellIds; // caster spell dropdown options
	ui::TabControl* m_tabs = nullptr;
};

} // namespace dungeon::game
