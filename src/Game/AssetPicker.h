// ============================================================================
// Game/AssetPicker.h — browse the asset pool: a searchable thumbnail grid of
// installed texture sets (or models), with what each one actually holds.
//
// It replaces the type editor's `texture` / `model` DROPDOWNS. A dropdown was
// the wrong instrument once the pool passed a handful of entries: you cannot
// see what a texture looks like, cannot tell whether it is installed at 1k/2k/
// 4k, cannot tell whether it has a normal or ORM map, and cannot search. Here
// the grid shows the texture itself, each tile carries its resolutions, and the
// details pane answers the questions a file listing can't — is the height map
// real or flat, where was it imported from.
//
// It CHOOSES, it does not commit: onChoose hands the name back to the field
// that opened it, and the type dialog still writes on its own Save (`texture`
// stays a rebaking field). Modal over whatever opened it, in the house style —
// dim wash, panel, the shared close box top-right, Esc closes.
//
// Thumbnails are the installed .dds MIP CHAIN with its big levels dropped (the
// first level <= kThumbPx and everything under it), so a tile costs ~16 KB
// instead of the 5.6 MB the full 2k set would — no thumbnail bake, no new file
// format. They load only while visible and evict least-recently-seen, because
// the pool only ever grows. Model tiles have no image of their own until the
// owner bakes one (bakeModelIcons — the map-icon path), so they show the name
// alone until it arrives.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h" // PreviewPane, the card chrome
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h" // MaterialParams
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dungeon::ui {
class TextField;
} // namespace dungeon::ui

namespace dungeon::game {

class AssetPicker {
public:
	// Which pool is being browsed. The two differ only in where a tile's image
	// comes from and what the details pane can say about it.
	enum class Mode { Textures, Models };

	AssetPicker(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	// Opens on `current` (selected and scrolled into view; empty = nothing
	// selected). `label` titles the panel — the field's own name, so the dialog
	// says what is being chosen rather than "asset".
	void Open(Mode mode, const std::string& current, const std::string& label,
			  const ui::Theme& theme);
	void Close() { m_open = false; }

	Mode CurrentMode() const { return m_mode; }
	const std::string& Selected() const { return m_selected; }

	// Modal input: search box, grid, filters, footer. Also ages the thumbnail
	// cache (one tick per frame).
	void Update(const Input& input, float width, float height, float dt);
	// Dim wash, panel, search row, grid, details. The owner blits the rendered
	// preview into PreviewRect afterwards, as it does for AssetDialog.
	void Render(gfx::SpriteBatch& batch, float width, float height);

	// --- the shared 3D preview seam (AssetDialog's, verbatim) ----------------
	bool HasPreview() const { return m_previewMesh != nullptr; }
	const gfx::Mesh& PreviewMesh() const { return *m_previewMesh; }
	const gfx::MaterialParams& PreviewMaterial() const { return m_material; }
	float Orbit() const { return m_orbit; }
	gfx::Rect PreviewRect(float width, float height) const;

	// The Choose button (and a double-click on a tile): the picked name.
	std::function<void(const std::string&)> onChoose;
	// Which pool assets this project's catalogs already bind — the "in use"
	// filter. Asked once per Open, since it can't change while the picker is up.
	std::function<std::vector<std::string>()> usedAssets;
	// Where an asset came from (the project's imports.cat), "" when unrecorded.
	std::function<std::string(const std::string&)> sourceOf;

	// --- model thumbnails (the owner records the bake) -----------------------
	// A model tile's icon is RENDERED, not loaded, so it takes two steps in two
	// different places. Update loads the mesh and creates the target (both stall
	// the GPU — gfx::Texture::RenderTarget drains — so they must happen OUTSIDE
	// the frame's recording; doing it mid-list corrupted the next bake in the
	// same frame). The owner's icon pass then records the draw and marks it done.
	struct PendingBake {
		std::string name;
		const gfx::Mesh* mesh = nullptr;
		gfx::Texture* target = nullptr;
		Vec3 lo, hi; // model bounds, for the whole-model fit
	};
	std::vector<PendingBake> PendingBakes(size_t max) const;
	void MarkBaked(const std::string& name);

private:
	// A tile's image plus when it was last on screen (the eviction key). A model
	// tile also holds the mesh its icon was baked from — the bake only RECORDS a
	// draw, so the mesh must outlive the frame, and both die together.
	struct Thumb {
		std::unique_ptr<gfx::Texture> texture;
		std::unique_ptr<gfx::Mesh> mesh; // models only: kept alive for the bake
		Vec3 lo{}, hi{};                 // models only: bounds for the fit
		bool needsBake = false;          // target + mesh ready, draw not recorded
		u64 lastSeen = 0;
		bool tried = false; // a failed load isn't retried every frame
	};

	// Readies up to `max` model tiles for baking: loads the mesh, measures it and
	// creates the icon target. Called from Update — never while a frame is being
	// recorded (see PendingBake).
	void PrepareModelIcons(size_t max);

	void Rebuild();          // (re)builds the widget tree (search box, buttons)
	void ApplyFilter();      // recomputes m_shown from the search text + chips
	void SelectIndex(int i); // selects a shown-row index, loads its preview
	void RefreshPreview();   // the selected asset on the block mesh / itself
	void RefreshFacts();     // the details lines (decodes the normal map once)
	// Loads a tile image: the set's .dds trimmed to its small mips. Null when
	// the set has no baked chain (a PNG-only set falls back to the full PNG).
	std::unique_ptr<gfx::Texture> LoadThumb(const std::string& name) const;
	const gfx::Texture* ThumbFor(const std::string& name); // caches + marks seen
	void EvictThumbs();                                    // over the cap, oldest first

	// Geometry (window fractions resolved per draw, so the panel scales).
	gfx::Rect GridRect(float w, float h) const;
	gfx::Rect TileRect(float w, float h, size_t shownIndex) const;
	int TileAt(float w, float h, float mx, float my) const; // -1 = none
	float MaxScroll(float w, float h) const;
	// [first, end) shown-row indices on screen — the loader's work list.
	std::pair<size_t, size_t> VisibleRange() const;
	// Loads at most `max` on-screen tile images (textures) — called from Update,
	// never from the draw: each load reads a .dds and uploads it, which drains
	// the GPU, and doing a screenful at once is a visible stall.
	void LoadVisibleThumbs(size_t max);

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil
	ui::Theme m_theme; // kept for the deferred rebuild

	bool m_open = false;
	bool m_uiRebuild = false; // deferred Rebuild (never Clear inside a callback)
	Mode m_mode = Mode::Textures;
	std::string m_label;    // what is being chosen ("Texture" / "Model")
	std::string m_selected; // the picked name ("" = nothing)

	std::vector<AssetInfo> m_items;   // the pool, as listed at Open
	std::vector<size_t> m_shown;      // indices into m_items, after filtering
	std::vector<std::string> m_used;  // what the project binds (the chip)
	std::string m_search;
	bool m_onlyUsed = false;    // chip: only assets some catalog already binds
	bool m_onlySurface = false; // chip: only sets with worn block meshes
	float m_scroll = 0.0f;
	bool m_scrollDragging = false;
	float m_scrollGrab = 0.0f;
	int m_hover = -1;           // hovered shown-row index
	double m_lastClickTime = 0.0; // double-click detection on a tile
	int m_lastClickTile = -1;
	double m_time = 0.0;

	// Details for the selected asset, built on selection (not per frame). Both
	// are DEFERRED a frame: the preview loads three textures and a mesh, and the
	// facts decode a normal map to answer real-vs-flat height — doing either
	// during Open is a stall you can see, so the dialog draws first and fills in.
	std::vector<std::string> m_facts;
	bool m_previewDirty = false;
	bool m_factsDirty = false;

	std::unordered_map<std::string, Thumb> m_thumbs;
	u64 m_frame = 0; // ages the cache

	// Widgets the picker reads back (all owned by m_ui, dead after a Clear).
	ui::TextField* m_searchField = nullptr;
	// The area reserved for the tile grid: GridRect hands out its rect and every
	// tile metric is measured off it, so the grid and the layout cannot disagree.
	ui::Box* m_gridBox = nullptr;
	ui::Label* m_countLabel = nullptr; // "n of m", refreshed by ApplyFilter
	ui::Label* m_nameLabel = nullptr;  // the selected asset, over its facts
	PreviewPane* m_pane = nullptr;     // PreviewRect hands out its rect

	// Preview resources (the AssetDialog pattern: the dialog owns them, the
	// owner renders them into its RT and blits).
	assets::ModelData m_previewModel;
	std::unique_ptr<gfx::Mesh> m_previewMesh;
	std::unique_ptr<gfx::Texture> m_previewAlbedo, m_previewNormal, m_previewMr;
	gfx::MaterialParams m_material;
	float m_orbit = 0.0f;
};

} // namespace dungeon::game
