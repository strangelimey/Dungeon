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
	// Dim wash + panel + the three columns + footer buttons. The owner (Game) draws
	// the live animation into PreviewRect afterwards (it owns the render target).
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// The monster type being configured, the clip currently selected for preview
	// (empty = none), and the on-screen rect to blit the preview into. The owner
	// drives an Animator on this type playing this clip and blits into PreviewRect.
	const std::string& SelectedType() const { return m_cfg.type; }
	const std::string& PreviewClip() const { return m_selClip; }
	gfx::Rect PreviewRect(float width, float height) const;

	// Push the working config to the live kind (every edit; and the original on a
	// revert). onSave persists it to the catalog (the Save button).
	std::function<void(const Config&)> onApply;
	std::function<void(const Config&)> onSave;

private:
	// Shared layout (built by Update for hit-test and Render for draw) so the two
	// always agree. Clip rows carry their model-clip index; rows outside the clip
	// column viewport are dropped (the column scrolls).
	struct Layout {
		gfx::Rect panel{}, statesCol{}, clipsCol{}, previewCol{}, save{}, close{};
		float rowH = 0.0f;
		std::array<gfx::Rect, N> stateRow{};
		std::array<gfx::Rect, N> stateCheck{};
		std::vector<gfx::Rect> clipRow;
		std::vector<gfx::Rect> clipCheck; // checkbox sub-rect per visible clipRow
		std::vector<int> clipOf;          // model-clip index per visible clipRow
		float clipContentH = 0.0f;
	};
	Layout BuildLayout(float width, float height) const;
	void Apply() { if (onApply) onApply(m_cfg); }
	// The selected state's first candidate clip (for auto-preview), or "" if none.
	std::string FirstClipOf(int state) const;
	// Whether a clip belongs to `state`'s column: its name encodes the state
	// (<state>__… or the bare token) OR it is already an assigned clip for that
	// state — so hand-authored / oddly-named clips already in the catalog stay
	// visible and editable rather than vanishing from the dialog.
	bool ClipBelongs(const std::string& name, int state) const;

	gfx::GraphicsDevice& m_device;
	ui::Font m_font;

	bool m_open = false;
	std::string m_display;
	Config m_cfg;          // working copy
	Config m_original;     // snapshot for revert on Close/Esc
	std::vector<std::string> m_modelClips; // the clip pool offered per state
	int m_selState = static_cast<int>(anim::CreatureState::Idle);
	std::string m_selClip;                 // clip selected for preview ("" = none)
	float m_clipScroll = 0.0f;
	// The last-built preview column rect, so PreviewRect (called by the owner each
	// frame for the blit + aspect) reads it instead of rebuilding the whole layout.
	mutable gfx::Rect m_previewColCache{};
};

} // namespace dungeon::game
