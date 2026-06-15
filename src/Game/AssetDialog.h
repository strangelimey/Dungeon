// ============================================================================
// Game/AssetDialog.h — the editor's "create a new asset" modal (plan phase P4).
//
// Opened from the palette's per-category "+ New". A centered panel over the
// editor: a form (name, a Browse button that fires the native file/folder
// picker, metallic/roughness/height sliders, a fallback color) on the left, a
// live 3D preview of the picked source model on the right. The form is a
// ui::UIContext; the preview itself is a gfx::ModelPreview the owner (Game)
// renders into — this dialog only exposes the picked mesh/material/orbit and
// the screen rect to blit it into.
//
// Create gathers the inputs into a CreateRequest and fires onCreate; wiring
// that to AssetBaker (the actual bake + catalog write) is P4c — until then the
// owner stubs it. All edits stay in memory; nothing is written here.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h" // MaterialParams
#include "Graphics/SpriteBatch.h"
#include "Platform/Window.h"
#include "UI/UIContext.h"

#include <functional>
#include <memory>
#include <string>

namespace dungeon::ui {
class TextField;
class Label;
} // namespace dungeon::ui

namespace dungeon::game {

class AssetDialog {
public:
	// The gathered form, handed to onCreate (P4c bakes + writes the catalog).
	struct CreateRequest {
		std::string category;   // display label, e.g. "Decoration"
		std::string catalogKey;  // which project catalog ("decorations", "walls"...)
		bool textureSet = false; // true = a folder of texture maps, else a model
		std::string name;        // catalog id / asset name the user typed
		std::string sourcePath;  // picked model file or texture folder
		gfx::MaterialParams material; // metallic/roughness/height + fallback color
	};

	AssetDialog(gfx::GraphicsDevice& device, Window& window);

	bool IsOpen() const { return m_open; }
	// Opens for a category (display label + project catalog key); textureSet
	// picks the folder vs file browse mode. `theme` styles the form.
	void Open(const std::string& category, const std::string& catalogKey,
			  bool textureSet, const ui::Theme& theme);
	void Close() { m_open = false; m_busy = false; }

	// The owner sets this while the bake subprocess runs: the form is frozen and
	// a "baking…" notice covers it.
	void SetBusy(bool busy) { m_busy = busy; }

	// Modal input (the owner routes nothing else while open) + preview spin.
	void Update(const Input& input, float width, float height, float dt);
	// Draws the dim wash, panel, and form. The owner draws the preview image at
	// PreviewRect afterwards (it owns the ModelPreview render target).
	void Render(gfx::SpriteBatch& batch, float width, float height);

	// Live preview source (null until a model is picked).
	bool HasPreview() const { return m_previewMesh != nullptr; }
	const gfx::Mesh& PreviewMesh() const { return *m_previewMesh; }
	const gfx::MaterialParams& PreviewMaterial() const { return m_material; }
	float Orbit() const { return m_orbit; }
	gfx::Rect PreviewRect(float width, float height) const;

	// Fired by the Create button with the gathered form (P4c implements it).
	std::function<void(const CreateRequest&)> onCreate;

private:
	void Rebuild(const ui::Theme& theme); // (re)builds the form widgets
	void Browse();                        // native picker -> load preview model

	gfx::GraphicsDevice& m_device;
	Window& m_window;
	std::unique_ptr<ui::UIContext> m_ui;

	bool m_open = false;
	bool m_busy = false; // a bake is running; freeze the form
	std::string m_category;
	std::string m_catalogKey;
	bool m_textureSet = false;
	std::string m_name;
	std::string m_sourcePath;
	gfx::MaterialParams m_material;
	float m_orbit = 0.0f;

	assets::ModelData m_previewModel; // kept alive for the mesh
	std::unique_ptr<gfx::Mesh> m_previewMesh;

	// Widgets the callbacks read/update (owned by m_ui).
	ui::TextField* m_nameField = nullptr;
	ui::Label* m_pathLabel = nullptr;
};

} // namespace dungeon::game
