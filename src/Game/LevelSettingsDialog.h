// ============================================================================
// Game/LevelSettingsDialog.h — the editor's per-level settings modal.
//
// Opened by the editor toolbar's Level button, for the level the map viewport
// is SHOWING (active or browsed). Three numeric knobs — the lighting mood
// pass's console trio, promoted to authored per-level data:
//   • dust    — in-scatter density (gfx::Atmosphere::density)
//   • haze    — how much ambient light the dust catches (hazeAmbient)
//   • ambient — scales the base unlit fill (DungeonWorld::SetAmbientScale)
// ...plus the level's TAGS: the tag words it is built from (DungeonMap::Tags,
// matched against each catalog entry's `tags`). It rides this dialog because it
// is the same kind of fact as the mood knobs — a property of the level as a
// whole rather than of anything placed in it. And the UI MATERIAL the chrome is
// cut from here (DungeonMap::UiStone; "the dungeon's" = no override), the same
// kind of fact again (more-ui-updates).
//
// Edits to the NUMBERS fire onApply live (the owner applies them to the world
// only while the dialog's level is the ACTIVE one — a browsed level can't be
// seen anyway). The tags have no live preview to give: it changes how the
// palette RANKS, which the editor re-reads from the level once Save commits it.
// Save fires onSave (the owner writes everything into the level's map/stash —
// they persist as the .map `atmosphere` and `tags` records on the next
// savemap). Close/Esc reverts the numbers via onApply(original); an uncommitted
// tags edit is simply dropped with the dialog.
// ============================================================================
#pragma once

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
class TextField; // Controls.h — only a pointer here (the rename field)
struct Skin;     // Skin.h — the sample strip's
class Widget;    // Widget.h — the sample strip's row
}

namespace dungeon::game {

class LevelSettingsDialog {
public:
	LevelSettingsDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	// Opens on the level's EFFECTIVE values (its overrides, or the world
	// defaults where unset — DungeonWorld::EffectiveAtmosphere). `tags` is the
	// level's tags as one space-separated string (DungeonMap::Tags). `uiStone`
	// is the level's UI material override, empty = its dungeon's
	// (DungeonMap::UiStone); `dungeonStone` is that dungeon's, for the "the
	// dungeon's" row's picture (empty = none).
	void Open(const std::string& stem, float dust, float haze, float ambient,
			  const std::string& tags, const std::string& uiStone,
			  const std::string& dungeonStone);
	// Every way out ends the material preview (Save commits first, so ending it
	// then shows the saved choice; anything else shows what was there before).
	void Close() {
		if (m_open && onPreviewStone) onPreviewStone(std::string());
		m_open = false;
	}

	const std::string& Level() const { return m_stem; }

	// --- the harness's hands (`editor levelsettings`) -------------------------
	// What typing `text` into number row `row` (0 dust, 1 haze, 2 ambient) does:
	// the field's text set and its own onChange fired, so a parseable value is
	// previewed live. False = not open or no such row.
	static constexpr size_t kNumberRows = 3;
	bool TypeNumber(size_t row, const std::string& text);
	// The working copy the rows hold (what Save would commit).
	float Dust() const { return m_dust; }
	float Haze() const { return m_haze; }
	float Ambient() const { return m_ambient; }
	// A press on the n-th drop-down the dialog shows (opened at the next
	// Update), and whether one is open - what Esc asks first (code-review C81).
	void OpenPopup(int n) { m_ui.OpenPopupNext(n); }
	bool PopupOpen() const { return m_ui.PopupOpen(); }

	// Modal input: routes to the widget tree and handles Esc (revert + close),
	// once no drop-down is open to take it.
	void Update(const Input& input, float width, float height);
	// Dim wash + panel frame + title + the widget tree.
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Live preview on every valid edit (and the original on a revert). Numbers
	// only — the tags have nothing to preview in the 3D scene.
	std::function<void(float dust, float haze, float ambient)> onApply;
	// The Save button: commit the values to the level (map or stash). `tags` is
	// the raw field text; the owner parses it (game::ParseTags). `uiStone` is
	// empty for "the dungeon's".
	std::function<void(float dust, float haze, float ambient, const std::string& tags,
					   const std::string& uiStone)>
		onSave;
	// Renaming: clicking the stem in the title opens an inline edit; Enter
	// commits through this. The owner does the real work (files, stashes,
	// stair dests, manifest) and returns success — false keeps the edit open
	// (the owner logs why). The dialog adopts the new stem on true.
	std::function<bool(const std::string& oldStem, const std::string& newStem)>
		onRename;
	// A UI material's thumbnail by stem (GameUI::StoneThumb), for the material
	// dropdown's pictures. Null = no picture for that row.
	std::function<const gfx::Texture*(const std::string& stone)> thumbFor;
	// The materials in the order to list them (GameUI::StoneOrder: grouped by
	// kind). Unset = the folder's, alphabetical.
	std::function<std::vector<std::string>()> stoneOrder;
	// The material list's category buttons and each material's mask for them
	// (GameUI::StoneFilterLabels / StoneFilterBits). Unset = no buttons.
	std::function<std::vector<std::string>()> stoneFilterLabels;
	std::function<unsigned(const std::string& stone)> stoneFilterBits;
	std::function<std::vector<Vec4>()> stoneFilterColors; // the buttons' chips
	// The UI material PREVIEW (Michael: picking one shows it at once; closing
	// without Save puts the old one back). Called with the material the level
	// would wear on Open and on every pick, and with "" to end the preview -
	// which Close does on every way out.
	std::function<void(const std::string& stone)> onPreviewStone;
	// The game chrome's skin (GameUI::GameSkin) for the sample strip under the
	// material row: the editor's dialogs are drawn flat, so without a sample
	// the preview would change nothing visible while the editor is up.
	const ui::Skin* sampleSkin = nullptr;

private:
	void BuildUI();
	void DrawSample(gfx::SpriteBatch& batch);
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
	// The tags row's raw text (space-separated tags). Kept as typed rather than
	// parsed per keystroke: mid-word is not a tag list yet, and only Save reads it.
	std::string m_tags;
	// The UI material row: the choices (InstalledUiStones, read at Open) and
	// the picked stem, empty = the dungeon's. Like the tags, only Save reads it.
	std::vector<std::string> m_stones;
	std::string m_uiStone;
	std::string m_dungeonStone; // the level's dungeon's material (the first row)
	// What the level would wear with the current pick ("the dungeon's" = its).
	const std::string& PreviewName() const {
		return m_uiStone.empty() ? m_dungeonStone : m_uiStone;
	}
	// The sample strip's area (a Box row); valid until the next Clear.
	ui::Widget* m_sampleBox = nullptr;
	// The number rows' fields, for TypeNumber; valid until the next Clear.
	std::array<ui::TextField*, kNumberRows> m_numberFields{};

	// Inline name edit (click the stem). The rebuild after entering/leaving
	// edit mode is DEFERRED to the next Update when triggered from a widget
	// callback — UIContext::Clear from inside one dangles the caller (the
	// m_pendingLanguage convention).
	bool m_editName = false;    // title row is the edit field
	bool m_uiRebuild = false;   // deferred BuildUI request
	ui::TextField* m_nameField = nullptr; // valid until the next Clear
};

} // namespace dungeon::game
