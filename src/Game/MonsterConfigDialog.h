// ============================================================================
// Game/MonsterConfigDialog.h — the editor's "configure a monster type" modal.
//
// Opened by right-clicking a monster in the editor palette. A centered panel over
// the editor, TABBED like the settings page (ui::TabControl):
//   • Behavior — the AI archetype (dropdown) + its typed params (keep range, flee
//     threshold, caster spell), shown/hidden by archetype. Writes monsters.cat's
//     archetype/keeprange/fleebelow/spell fields + the live MonsterKind.
//   • Animation — two columns of rows: the CreatureStates (checkbox = the type's
//     supported set, click the row to pick one) and, for the picked state, the
//     model's clips (checkbox = which the state draws from). Authors the
//     data-driven animation table (Animation/CreatureState.h + MonsterKind).
// A live 3D preview of the selected clip sits to the right of the tabs (the owner
// blits into PreviewRect). The tab content is a ui::UIContext of real widgets; the
// panel frame, title, and preview backing are drawn by the dialog around it.
//
// The dialog edits an in-memory working copy and fires callbacks; the owner (Game)
// applies them live (onApply, every edit) and persists them to the catalog
// (onSave, the Save button). Close/Esc reverts via onApply(original).
// ============================================================================
#pragma once

#include "Animation/CreatureState.h"
#include "Game/MonsterAI.h" // ai::Archetype
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace dungeon::ui {
class TabControl;
}

namespace dungeon::game {

class MonsterConfigDialog {
public:
	static constexpr int N = anim::kCreatureStateCount;
	using Support = std::array<bool, N>;
	using Clips = std::array<std::vector<std::string>, N>;

	// The working copy handed to the callbacks: the animation table + the AI
	// behaviour fields.
	struct Config {
		std::string type;
		Support supported{};
		Clips clips{};
		ai::Archetype archetype = ai::Archetype::Brute;
		float keepRange = 4.0f;
		float fleeBelow = 0.0f;
		std::string spell;
	};

	explicit MonsterConfigDialog(gfx::GraphicsDevice& device);

	bool IsOpen() const { return m_open; }
	// Opens for a monster type: its id + display name, current supported set + clip
	// table, behaviour fields, the model's full clip pool, and the project's spell
	// ids (the caster dropdown's options).
	void Open(const std::string& type, const std::string& display,
			  const Support& supported, const Clips& clips, ai::Archetype archetype,
			  float keepRange, float fleeBelow, const std::string& spell,
			  const std::vector<std::string>& modelClips,
			  const std::vector<std::string>& spellIds);
	void Close() { m_open = false; }

	// Modal input: routes to the widget tree, handles Esc (revert+close), and does
	// any deferred rebuild queued by a widget callback (archetype/state change).
	void Update(const Input& input, float width, float height);
	// Dim wash + panel frame + title + the widget tree + the preview backing box.
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// The monster type being configured, the clip selected for preview (empty =
	// none), and the on-screen rect to blit the live preview into.
	const std::string& SelectedType() const { return m_cfg.type; }
	const std::string& PreviewClip() const { return m_selClip; }
	gfx::Rect PreviewRect(float width, float height) const;

	// Push the working config to the live kind (every edit; and the original on a
	// revert). onSave persists it to the catalog (the Save button).
	std::function<void(const Config&)> onApply;
	std::function<void(const Config&)> onSave;

private:
	// (Re)builds the whole widget tree from m_cfg (tabs + rows + footer). Called on
	// Open and whenever a deferred rebuild is queued (archetype/state selection
	// changes which rows exist). Restores the active tab.
	void BuildUI();
	void BuildBehaviorTab(size_t tab);
	void BuildAnimationTab(size_t tab);
	void Apply() { if (onApply) onApply(m_cfg); }

	// The preview pane's normalized (0..1 of window) rect — right of the tabs. Both
	// Render (backing box) and PreviewRect (owner blit) resolve against it.
	gfx::Rect PreviewNorm() const;
	// Whether a clip belongs to `state`'s column (name-encoded <state>__… or already
	// an assigned clip), and the selected state's first candidate clip (auto-preview).
	bool ClipBelongs(const std::string& name, int state) const;
	std::string FirstClipOf(int state) const;

	gfx::GraphicsDevice& m_device;
	ui::Font m_font;      // the dialog's own text (title / preview header / hints)
	ui::UIContext m_ui;   // the tab content + footer buttons

	bool m_open = false;
	std::string m_display;
	Config m_cfg;      // working copy
	Config m_original; // snapshot for revert on Close/Esc
	std::vector<std::string> m_modelClips; // clip pool offered per state
	std::vector<std::string> m_spellIds;   // caster spell dropdown options
	int m_selState = static_cast<int>(anim::CreatureState::Idle);
	std::string m_selClip; // clip selected for preview ("" = none)

	ui::TabControl* m_tabs = nullptr; // owned by m_ui; kept to restore the tab
	int m_activeTab = 0;
	bool m_rebuild = false; // a widget callback queued a rebuild (done after Update)
};

} // namespace dungeon::game
