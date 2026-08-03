// ============================================================================
// Game/LevelSettingsDialog.h — the editor's per-level atmosphere modal.
//
// Opened by the editor toolbar's Level button, for the level the map viewport
// is SHOWING (active or browsed). Three numeric knobs — the lighting mood
// pass's console trio, promoted to authored per-level data:
//   • dust    — in-scatter density (gfx::Atmosphere::density)
//   • haze    — how much ambient light the dust catches (hazeAmbient)
//   • ambient — scales the base unlit fill (DungeonWorld::SetAmbientScale)
// Edits fire onApply live (the owner applies them to the world only while the
// dialog's level is the ACTIVE one — a browsed level can't be seen anyway);
// Save fires onSave (the owner writes the values into the level's map/stash —
// they persist as the .map `atmosphere` record on the next savemap).
// Close/Esc reverts via onApply(original).
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
	// defaults where unset — DungeonWorld::EffectiveAtmosphere).
	void Open(const std::string& stem, float dust, float haze, float ambient);
	void Close() { m_open = false; }

	const std::string& Level() const { return m_stem; }

	// Modal input: routes to the widget tree and handles Esc (revert + close).
	void Update(const Input& input, float width, float height);
	// Dim wash + panel frame + title + the widget tree.
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Live preview on every valid edit (and the original on a revert).
	std::function<void(float dust, float haze, float ambient)> onApply;
	// The Save button: commit the values to the level (map or stash).
	std::function<void(float dust, float haze, float ambient)> onSave;
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
	std::unique_ptr<gfx::Texture> m_closeIcon; // shared top-right close box

	bool m_open = false;
	std::string m_stem; // the level being edited (title + the owner's routing)
	float m_dust = 0.0f, m_haze = 0.0f, m_ambient = 1.0f;    // working copy
	float m_oDust = 0.0f, m_oHaze = 0.0f, m_oAmbient = 1.0f; // revert snapshot

	// Inline name edit (click the stem). The rebuild after entering/leaving
	// edit mode is DEFERRED to the next Update when triggered from a widget
	// callback — UIContext::Clear from inside one dangles the caller (the
	// m_pendingLanguage convention).
	bool m_editName = false;    // title row is the edit field
	bool m_uiRebuild = false;   // deferred BuildUI request
	bool m_nameHover = false;   // stem hover (Update tracks, Render styles)
	ui::TextField* m_nameField = nullptr; // valid until the next Clear
	// The stem's pixel rect within the title line (Update-computed hit box).
	gfx::Rect StemRect(float w, float h);
};

} // namespace dungeon::game
