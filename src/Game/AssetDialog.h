// ============================================================================
// Game/AssetDialog.h — the editor's "create a new type" modal, opened by a
// palette category's "+ New..." row.
//
// Three ways to make a type, because importing a fresh download is only one of
// them (CreateRequest::Source):
//   * Import   — browse to a texture folder / model file and run AssetBaker.
//   * Installed— bind an asset ALREADY in the pool (assets/textures, assets/
//                models). No bake when its worn meshes exist: "another wall
//                type off cobblestone" is a five-second job, where before it
//                meant re-importing the folder under a second name. A set
//                painted as ANOTHER surface kind is refused in the form
//                (installedRefusal; one worn mesh per set - code-review C407).
//   * Duplicate— copy an existing catalog entry of this category under a new id.
//
// A type whose MODEL its category could not load is refused in the form too
// (modelRefusal, code-review C301): the load is a LoadModelOrDie, so writing
// one would abort the next level that uses it.
//
// The id is validated as you type (record files are whitespace-tokenised, and
// Catalog::Add REPLACES by id — an unchecked name silently overwrote a type
// every level used). The panel refuses to Create until the form is valid and
// says why.
//
// For an import, the dialog reports what AssetBaker will find BEFORE the bake:
// assets::DiscoverPbrMaps (the baker's own role detection, shared) lists the
// maps it recognised, warns when the height map is missing, and offers the
// flip-green override for GL-convention normals whose filename lacks the token.
// The preview pane shows the picked mesh, or a block wearing the picked texture
// set — including one that is still just PNGs in a download folder.
//
// The form gathers into a CreateRequest and fires onCreate; the owner (Game)
// runs the bake, writes the catalog entry from the category's SCHEMA defaults,
// and — for a surface — adds the new type to the level's palette so the brush
// can reach it.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Assets/PbrMaps.h"
#include "Game/DialogLayout.h" // PreviewPane
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h" // MaterialParams
#include "Graphics/SpriteBatch.h"
#include "Platform/Window.h"
#include "UI/UIContext.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dungeon::ui {
class TextField;
class Label;
} // namespace dungeon::ui

namespace dungeon::game {

class ReasonLine; // the footer's wrapping reason line (AssetDialog.cpp)

class AssetDialog {
public:
	// Where the new type's asset comes from (see the header note).
	enum class Source { Import, Installed, Duplicate };

	// The gathered form, handed to onCreate.
	struct CreateRequest {
		std::string category;   // display label, e.g. "Decoration"
		std::string catalogKey;  // which project catalog ("decorations", "walls"...)
		bool textureSet = false; // true = a folder of texture maps, else a model
		Source source = Source::Import;
		std::string name;        // catalog id / asset name the user typed
		std::string group;       // optional `category` field (palette sub-accordion)
		std::string sourcePath;  // Import: the picked model file or texture folder
		std::string asset;       // Installed: pool asset name; Duplicate: source id
		bool flipGreen = false;  // Import: force the normal map's green channel flip
		gfx::MaterialParams material; // metallic/roughness/height + fallback color
		// Which sliders the user MOVED from their opening values. Untouched ones
		// are not written to the catalog, so an imported model's own maps stay
		// authoritative unless deliberately overridden (FinishBake reads these).
		bool metallicSet = false;
		bool roughnessSet = false;
		bool heightSet = false;
		bool colorSet = false;

		// Installed/Duplicate need no AssetBaker run - the asset is already baked
		// (but an installed set adopted as a surface for the first time has no
		// worn meshes yet: the owner bakes those, Game::AdoptSurfaceSet).
		bool NeedsBake() const { return source == Source::Import; }
	};

	AssetDialog(gfx::GraphicsDevice& device, Window& window);

	bool IsOpen() const { return m_open; }
	// Opens for a category (display label + project catalog key); textureSet
	// picks the folder vs file browse mode. `existing` are that catalog's ids
	// (the Duplicate list AND the duplicate-name check). `theme` styles the form.
	// `source`/`asset` preset the form — the type editor's Duplicate button opens
	// straight onto Duplicate-of-that-entry; the name is NEVER prefilled, since a
	// clone still has to be told what it is.
	void Open(const std::string& category, const std::string& catalogKey,
			  bool textureSet, std::vector<std::string> existing,
			  const ui::Theme& theme, Source source = Source::Import,
			  const std::string& asset = {});
	void Close() { m_open = false; m_busy = false; }

	// The owner sets this while the bake subprocess runs: the form is frozen and
	// a "baking…" notice covers it.
	void SetBusy(bool busy) { m_busy = busy; }
	// A failed bake: shown in the form (with the baker's exit code) instead of
	// vanishing into the log, so the dialog stays open to fix and retry.
	void SetError(std::string message) {
		m_error = std::move(message);
		m_busy = false;
	}

	// Modal input (the owner routes nothing else while open) + preview spin. Esc
	// closes, as the close box does, unless a list is open to take it.
	void Update(const Input& input, float width, float height, float dt);
	// A press on the n-th drop-down (or the colour picker) the form shows
	// (`editor newasset popup <n>`; opened at the next Update), and whether one
	// is open - what Esc asks before it closes the dialog (code-review C81).
	void OpenPopup(int n) { m_ui->OpenPopupNext(n); }
	bool PopupOpen() const { return m_ui->PopupOpen(); }
	// Draws the dim wash, panel, and form. The owner draws the preview image at
	// PreviewRect afterwards (it owns the ModelPreview render target).
	void Render(gfx::SpriteBatch& batch, float width, float height);

	// The console's way through the form (`newasset`), the harness cannot click:
	// the id typed into the name field (filtered as typing is), the footer's
	// Create clicked, and the line under the form - why Create is refused, or the
	// last failure; "" when the form is ready.
	void TypeName(const std::string& id);
	// The folder (a texture set) or model file Browse's native picker returned,
	// taken as Browse takes it: what the import will find, the flip-green guess,
	// a default name and the preview (`newasset <category> import`).
	void PickSource(const std::string& path);
	void ClickCreate() { Create(); }
	std::string Problem() const { return m_error.empty() ? Validate() : m_error; }
	// What the form holds now, read before a Create to say what it made.
	const std::string& CatalogKey() const { return m_catalogKey; }
	const std::string& Asset() const { return m_asset; }
	const std::string& SourcePath() const { return m_sourcePath; }
	std::string TypedName() const;

	// Live preview source (null until a model or texture set is picked).
	bool HasPreview() const { return m_previewMesh != nullptr; }
	const gfx::Mesh& PreviewMesh() const { return *m_previewMesh; }
	const gfx::MaterialParams& PreviewMaterial() const { return m_material; }
	float Orbit() const { return m_orbit; }
	// Where the previewed model stands and turns (a rig's root; zero otherwise).
	Vec3 PreviewPivot() const { return m_previewModel.skeleton.RootRest(); }
	gfx::Rect PreviewRect(float width, float height) const;

	// Fired by the Create button with the gathered form.
	std::function<void(const CreateRequest&)> onCreate;
	// "Use installed" picks from the POOL, which is the asset picker's job — the
	// owner opens it (textures vs models, on `current`) and calls `apply` with
	// the chosen name. Same seam as the type editor's asset fields.
	std::function<void(bool textures, const std::string& current,
					   std::function<void(const std::string&)> apply)>
		onPickAsset;
	// Reads one field off a catalog entry — (catalogKey, id, field) -> value, ""
	// when unknown. The dialog holds ids, not the project, and Duplicate picks an
	// ID where Installed picks a pool ASSET: this is how it resolves the entry it
	// is about to copy down to the texture set or model to preview.
	std::function<std::string(const std::string&, const std::string&,
							  const std::string&)>
		fieldOfType;
	// Why a POOL texture set may not be adopted into this catalog - (catalogKey,
	// asset) -> "" when it may. Asked once per pick (it reads every world's
	// surface catalogs), for "Use installed" on a texture-set category only; the
	// answer refuses Create the way a missing name does. A pick can go stale (a
	// type renamed onto the set after it was judged), which is why the owner's
	// onCreate asks the rule again and refuses through SetError.
	std::function<std::string(const std::string&, const std::string&)> installedRefusal;
	// Why the type a MODEL pick makes could not load its model - (catalogKey,
	// source, asset) -> "" when it could (code-review C301). Asked once per pick
	// for an Installed or Duplicate model, and answered like installedRefusal:
	// it refuses Create, and the owner asks again of the entry it builds
	// (Game::CreateCatalogEntry), for a pick gone stale and for an import, whose
	// model its bake makes.
	std::function<std::string(const std::string&, Source, const std::string&)> modelRefusal;

private:
	void Rebuild(const ui::Theme& theme); // (re)builds the form widgets
	void Browse();                        // native picker -> load preview
	// The footer's Create: gathers the form into a CreateRequest for onCreate,
	// unless Validate refuses. Stays open on a bake or on an error onCreate set.
	void Create();
	// Re-asks installedRefusal or modelRefusal for the current pick
	// (m_assetRefusal).
	void CheckAsset();
	// Loads the preview for the current source (a model mesh, or the shared
	// block mesh wearing the picked/imported texture set). Drains the GPU first:
	// in-flight frames may still reference the old resources.
	void RefreshPreview();
	// "" when the form is ready to Create, else why it isn't (drawn under the
	// name field, and the Create button is disabled).
	std::string Validate() const;

	gfx::GraphicsDevice& m_device;
	Window& m_window;
	std::unique_ptr<ui::UIContext> m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil
	ui::Theme m_theme; // kept for the deferred rebuild (Clear kills the caller)

	bool m_open = false;
	bool m_busy = false;      // a bake is running; freeze the form
	bool m_uiRebuild = false; // deferred Rebuild (never Clear inside a callback)
	std::string m_error;      // last failure, shown in the form
	std::string m_category;
	std::string m_catalogKey;
	bool m_textureSet = false;
	Source m_source = Source::Import;
	std::string m_name;
	std::string m_group;
	std::string m_sourcePath;
	std::string m_asset;     // Installed: pool asset; Duplicate: source catalog id
	std::string m_assetRefusal; // CheckAsset's answer for m_asset ("" = none)
	bool m_flipGreen = false;
	std::vector<std::string> m_existing; // this catalog's ids
	// (no pool listing here any more — "Use installed" browses it in AssetPicker)
	assets::PbrMapSet m_found;           // Import: what the folder holds
	gfx::MaterialParams m_material;
	gfx::MaterialParams m_neutral; // opening values; Create diffs against these
	float m_orbit = 0.0f;

	assets::ModelData m_previewModel; // kept alive for the mesh
	std::unique_ptr<gfx::Mesh> m_previewMesh;
	// Texture-set preview: the maps bound into m_material (owned here, since
	// they may be loose PNGs from a download folder that nothing else knows).
	std::unique_ptr<gfx::Texture> m_previewAlbedo, m_previewNormal, m_previewMr;

	// Widgets the callbacks read/update (owned by m_ui).
	ui::TextField* m_nameField = nullptr;
	ui::TextField* m_groupField = nullptr;
	ui::Label* m_pathLabel = nullptr;
	// Rewritten every frame in Update: the reason Create is refused follows what
	// is typed, and typing does not rebuild the tree. It wraps (a refusal names a
	// set, a type and a world, and can be longer than the footer is wide).
	ReasonLine* m_problemLine = nullptr;
	// The preview pane widget; PreviewRect hands out its rect.
	PreviewPane* m_pane = nullptr;
};

} // namespace dungeon::game
