// ============================================================================
// Game/MonsterConfigDialog.h — the editor's "configure a monster type" modal.
//
// Opened by right-clicking a monster in the editor palette. A centered panel
// over the editor with two checkbox columns: the full list of CreatureStates
// (left, checkbox = the type's supported-state set) and, for the selected state,
// the monster model's available animation clips (right, checkbox = which clips
// that state draws from). It authors the data-driven animation table built in
// Animation/CreatureState.h + DungeonWorld's MonsterKind.
//
// Self-drawn (custom rows + recorded hit-rects, like the dev-console panels)
// rather than a ui::UIContext — two scrollable checkbox lists are simpler drawn
// directly. The dialog edits an in-memory working copy and fires callbacks; the
// owner (Game) applies them live (onApply, on every toggle) and persists them to
// the catalog (onSave, the Save button). Close/Esc reverts via onApply(original).
// ============================================================================
#pragma once

#include "Animation/CreatureState.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h" // ui::Theme

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class MonsterConfigDialog {
public:
	static constexpr int N = anim::kCreatureStateCount;
	using Support = std::array<bool, N>;
	using Clips = std::array<std::vector<std::string>, N>;

	// The working copy handed to the callbacks.
	struct Config {
		std::string type;
		Support supported{};
		Clips clips{};
	};

	explicit MonsterConfigDialog(gfx::GraphicsDevice& device);

	bool IsOpen() const { return m_open; }
	// Opens for a monster type: its id, display name, current supported set + clip
	// table, and the full pool of clip names from its model.
	void Open(const std::string& type, const std::string& display,
			  const Support& supported, const Clips& clips,
			  const std::vector<std::string>& modelClips);
	void Close() { m_open = false; }

	// Modal input: toggles a state/clip checkbox, selects a state, scrolls the clip
	// column, or hits Save/Close. Esc closes (reverting). Fires onApply on edits.
	void Update(const Input& input, float width, float height);
	// Dim wash + panel + the two columns + footer buttons.
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Push the working config to the live kind (every edit; and the original on a
	// revert). onSave persists it to the catalog (the Save button).
	std::function<void(const Config&)> onApply;
	std::function<void(const Config&)> onSave;

private:
	// Shared layout (built by Update for hit-test and Render for draw) so the two
	// always agree. Clip rows carry their model-clip index; rows outside the clip
	// column viewport are dropped (the column scrolls).
	struct Layout {
		gfx::Rect panel{}, statesCol{}, clipsCol{}, save{}, close{};
		float rowH = 0.0f;
		std::array<gfx::Rect, N> stateRow{};
		std::array<gfx::Rect, N> stateCheck{};
		std::vector<gfx::Rect> clipRow;
		std::vector<int> clipOf; // model-clip index per visible clipRow
		float clipContentH = 0.0f;
	};
	Layout BuildLayout(float width, float height) const;
	void Apply() { if (onApply) onApply(m_cfg); }

	gfx::GraphicsDevice& m_device;
	ui::Font m_font;

	bool m_open = false;
	std::string m_display;
	Config m_cfg;          // working copy
	Config m_original;     // snapshot for revert on Close/Esc
	std::vector<std::string> m_modelClips; // the clip pool offered per state
	int m_selState = static_cast<int>(anim::CreatureState::Idle);
	float m_clipScroll = 0.0f;
};

} // namespace dungeon::game
