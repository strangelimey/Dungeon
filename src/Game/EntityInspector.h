// ============================================================================
// Game/EntityInspector.h — the editor's per-INSTANCE monster inspector.
//
// Select-click a placed monster in the editor to edit the overrides on its .ent
// record (asleep, leash range) — a small tabbed panel (ui::TabControl) like the
// monster-config dialog, but scoped to one placed monster rather than the type.
// Edits apply live to that monster (onApply, every edit); Save persists the level
// .ent (onSave). Close/Esc reverts via onApply(original). Patrol (grid-authored)
// joins as a second tab in P3b.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Game/MonsterAI.h" // ai::Archetype
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::ui {
class TabControl;
}

namespace dungeon::game {

class EntityInspector {
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
	};

	explicit EntityInspector(gfx::GraphicsDevice& device);

	bool IsOpen() const { return m_open; }
	void Open(const Config& cfg, const std::vector<std::string>& spellIds);
	void Close() { m_open = false; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	std::function<void(const Config&)> onApply;
	std::function<void(const Config&)> onSave;

private:
	void BuildUI();
	void Apply() { if (onApply) onApply(m_cfg); }

	gfx::GraphicsDevice& m_device;
	ui::Font m_font;    // the dialog's title text
	ui::UIContext m_ui; // the tab content + footer buttons

	bool m_open = false;
	Config m_cfg;      // working copy
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::string> m_spellIds; // caster spell dropdown options
	ui::TabControl* m_tabs = nullptr;
	bool m_rebuild = false; // archetype change queued dependent-field rebuild
};

} // namespace dungeon::game
