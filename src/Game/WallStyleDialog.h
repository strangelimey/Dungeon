// ============================================================================
// Game/WallStyleDialog.h — the editor's per-wall-type geometry style modal.
//
// Opened by right-clicking a Walls row in the editor palette. Two knobs that
// drive the worn-block bake (walls.cat `wear`/`columns`):
//   • wear    — displacement amount (0 = flat panel … 1 = full worn relief)
//   • columns — the wall's edge pillars / border strips
// The geometry is BAKED (a worn_<texture>_*.gltf per wall texture), so there is
// no live preview: Save fires onSave, and the owner writes the catalog fields,
// re-bakes that texture's worn meshes, and swaps them in live. Close/Esc just
// dismisses (nothing is applied until Save). Modelled on LevelSettingsDialog.
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
	// Opens for a wall catalog entry: `id` is the walls.cat id (routing +
	// onSave), `display` the title, `texture` the set whose worn mesh is baked,
	// and wear/columns the entry's current values.
	void Open(const std::string& id, const std::string& display,
			  const std::string& texture, float wear, bool columns);
	void Close() { m_open = false; }

	// Modal input: routes to the widget tree and handles Esc (close).
	void Update(const Input& input, float width, float height);
	// Dim wash + panel frame + title + the widget tree.
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// The Save button: commit the style to the wall type (catalog write +
	// worn rebake + live reload — the owner does all of that).
	std::function<void(const std::string& id, const std::string& texture,
					   float wear, bool columns)>
		onSave;

private:
	void BuildUI();

	gfx::GraphicsDevice& m_device;
	ui::Font m_font;    // the dialog's own text (title)
	ui::UIContext m_ui; // slider + checkbox + footer buttons

	bool m_open = false;
	std::string m_id;      // walls.cat id being edited
	std::string m_display; // title text
	std::string m_texture; // texture set to rebake
	float m_wear = 1.0f;   // working copy
	bool m_columns = true; // working copy
};

} // namespace dungeon::game
