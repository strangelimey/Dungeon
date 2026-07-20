// ============================================================================
// Game/WallStyleDialog.h — the editor's per-surface-type geometry style modal.
//
// Opened by right-clicking a Walls / Floors / Ceilings row in the editor
// palette. Knobs that drive the worn-block bake:
//   • wear    — displacement amount (0 = flat panel … 1 = full worn relief),
//               folded into the parallax depth too so flat reads flat
//   • columns — the wall's edge pillars / border strips (WALLS only; hidden for
//               floors/ceilings)
// The geometry is BAKED (a worn_<texture>_*.gltf per set), so there is no live
// preview: Save fires onSave and the owner writes the catalog fields, re-bakes,
// and swaps the meshes in live. While that async bake runs the owner sets the
// dialog busy (a "baking…" notice freezes it), then closes it on completion.
// Close/Esc dismisses (nothing applied until Save). Modelled on
// LevelSettingsDialog + AssetDialog's busy overlay.
// ============================================================================
#pragma once

#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>

namespace dungeon::game {

class WallStyleDialog {
public:
	explicit WallStyleDialog(gfx::GraphicsDevice& device);

	bool IsOpen() const { return m_open; }
	// Opens for a surface catalog entry: `id` is the catalog id, `catalogKey` its
	// catalog ("walls"/"floors"/"ceilings", echoed back to onSave for routing),
	// `display` the title, `texture` the set whose worn mesh is baked, wear/columns
	// its current values, and `showColumns` whether the Columns knob applies
	// (walls only — pillars are a wall-block feature).
	void Open(const std::string& id, const std::string& catalogKey,
			  const std::string& display, const std::string& texture, bool showColumns,
			  float wear, bool columns);
	void Close() {
		m_open = false;
		m_busy = false;
	}

	// The owner sets this while the async rebake runs: the form freezes behind a
	// "baking…" notice until the owner closes the dialog on completion.
	void SetBusy(bool busy) { m_busy = busy; }

	// Modal input: routes to the widget tree and handles Esc (close).
	void Update(const Input& input, float width, float height);
	// Dim wash + panel frame + title + the widget tree (+ the busy notice).
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// The Save button: commit the style to the surface type (catalog write +
	// worn rebake + live reload — the owner does all of that).
	std::function<void(const std::string& id, const std::string& catalogKey,
					   const std::string& texture, float wear, bool columns)>
		onSave;

private:
	void BuildUI();

	gfx::GraphicsDevice& m_device;
	ui::Font m_font;    // the dialog's own text (title)
	ui::UIContext m_ui; // slider + checkbox + footer buttons

	bool m_open = false;
	bool m_busy = false;       // a rebake is running; freeze the form
	std::string m_id;          // catalog id being edited
	std::string m_catalogKey;  // its catalog (echoed to onSave)
	std::string m_display;     // title text
	std::string m_texture;     // texture set to rebake
	bool m_showColumns = true; // Columns applies (walls only)
	float m_wear = 1.0f;       // working copy
	bool m_columns = true;     // working copy
};

} // namespace dungeon::game
