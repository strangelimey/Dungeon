// ============================================================================
// Game/LevelSettingsDialog.h — the editor's per-level settings modal.
//
// Opened by the editor toolbar's Level button, for the level the map viewport
// is SHOWING (active or browsed). Three numeric knobs — the lighting mood
// pass's console trio, promoted to authored per-level data:
//   • dust    — in-scatter density (gfx::Atmosphere::density)
//   • haze    — how much ambient light the dust catches (hazeAmbient)
//   • ambient — scales the base unlit fill (DungeonWorld::SetAmbientScale)
// ...plus the level's THEME: the tag words it is built from (DungeonMap::Theme,
// matched against each catalog entry's `tags`). It rides this dialog because it
// is the same kind of fact as the mood knobs — a property of the level as a
// whole rather than of anything placed in it.
//
// Edits to the NUMBERS fire onApply live (the owner applies them to the world
// only while the dialog's level is the ACTIVE one — a browsed level can't be
// seen anyway). The theme has no live preview to give: it changes how the
// palette RANKS, which the editor re-reads from the level once Save commits it.
// Save fires onSave (the owner writes everything into the level's map/stash —
// they persist as the .map `atmosphere` and `theme` records on the next
// savemap). Close/Esc reverts the numbers via onApply(original); an uncommitted
// theme edit is simply dropped with the dialog.
// ============================================================================
#pragma once

#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>

namespace dungeon::ui {
class TextField; // Controls.h — only a pointer here (the rename field)
}

namespace dungeon::game {

class LevelSettingsDialog {
public:
	LevelSettingsDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	// Opens on the level's EFFECTIVE values (its overrides, or the world
	// defaults where unset — DungeonWorld::EffectiveAtmosphere). `theme` is the
	// level's tags as one space-separated string (DungeonMap::Theme).
	void Open(const std::string& stem, float dust, float haze, float ambient,
			  const std::string& theme);
	void Close() { m_open = false; }

	const std::string& Level() const { return m_stem; }

	// Modal input: routes to the widget tree and handles Esc (revert + close).
	void Update(const Input& input, float width, float height);
	// Dim wash + panel frame + title + the widget tree.
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Live preview on every valid edit (and the original on a revert). Numbers
	// only — the theme has nothing to preview in the 3D scene.
	std::function<void(float dust, float haze, float ambient)> onApply;
	// The Save button: commit the values to the level (map or stash). `theme` is
	// the raw field text; the owner parses it (game::ParseTags).
	std::function<void(float dust, float haze, float ambient, const std::string& theme)>
		onSave;
	// Renaming: clicking the stem in the title opens an inline edit; Enter
	// commits through this. The owner does the real work (files, stashes,
	// stair dests, manifest) and returns success — false keeps the edit open
	// (the owner logs why). The dialog adopts the new stem on true.
	std::function<bool(const std::string& oldStem, const std::string& newStem)>
		onRename;

private:
	void BuildUI();
	void Apply() {
		if (onApply) onApply(m_dust, m_haze, m_ambient);
	}

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui; // labels + numeric fields + footer buttons
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	bool m_open = false;
	std::string m_stem; // the level being edited (title + the owner's routing)
	float m_dust = 0.0f, m_haze = 0.0f, m_ambient = 1.0f;    // working copy
	float m_oDust = 0.0f, m_oHaze = 0.0f, m_oAmbient = 1.0f; // revert snapshot
	// The theme row's raw text (space-separated tags). Kept as typed rather than
	// parsed per keystroke: mid-word is not a tag list yet, and only Save reads it.
	std::string m_theme;

	// Inline name edit (click the stem). The rebuild after entering/leaving
	// edit mode is DEFERRED to the next Update when triggered from a widget
	// callback — UIContext::Clear from inside one dangles the caller (the
	// m_pendingLanguage convention).
	bool m_editName = false;    // title row is the edit field
	bool m_uiRebuild = false;   // deferred BuildUI request
	ui::TextField* m_nameField = nullptr; // valid until the next Clear
};

} // namespace dungeon::game
