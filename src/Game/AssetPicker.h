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
	// One tile: the asset's image over its name and badge, owning its own hover
	// and clicks. A nested class so it can read the picker's model directly —
	// they are one unit, and the alternative is a bundle of callbacks that only
	// re-states the coupling. It holds an INDEX into m_shown and re-resolves
	// every frame (the Repeater rule), so a filter change cannot dangle it.
	class AssetTile : public ui::Widget {
	public:
		AssetTile(const gfx::Rect& rect, AssetPicker& owner, size_t shownIndex)
			: m_owner(owner), m_index(shownIndex) {
			bounds = rect;
		}
		// The name this tile stands for, or "" once the filter has moved past it.
		const std::string& Name() const;
		size_t ShownIndex() const { return m_index; }

	protected:
		void UpdateSelf(ui::UIContext& ctx) override;
		void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	private:
		AssetPicker& m_owner;
		size_t m_index;
		bool m_hot = false;
	};

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

	void Rebuild();          // (re)builds the whole widget tree
	// Refills the grid's rows from m_shown. Separate from Rebuild because the
	// filter changes on every KEYSTROKE: rebuilding the tree would destroy the
	// search field being typed into, and the second character would go nowhere.
	void RebuildTiles();
	void ApplyFilter();      // recomputes m_shown from the search text + chips
	void SelectIndex(int i); // selects a shown-row index, loads its preview
	// A tile was clicked: select it, and choose it on the second click.
	void OnTileClicked(size_t shownIndex);
	void RefreshPreview();   // the selected asset on the block mesh / itself
	void RefreshFacts();     // the details lines (decodes the normal map once)
	// Loads a tile image: the set's .dds trimmed to its small mips. Null when
	// the set has no baked chain (a PNG-only set falls back to the full PNG).
	std::unique_ptr<gfx::Texture> LoadThumb(const std::string& name) const;
	const gfx::Texture* ThumbFor(const std::string& name); // caches + marks seen
	void EvictThumbs();                                    // over the cap, oldest first

	// The tiles currently inside the grid's view — the deferred loaders' work
	// list. Read from the tiles' own PIXEL rects rather than re-derived from a
	// scroll offset, so there is no second copy of the grid's geometry to drift.
	std::vector<AssetTile*> VisibleTiles() const;
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
	double m_lastClickTime = 0.0; // double-click detection on a tile
	int m_lastClickTile = -1;
	double m_time = 0.0;
	// Both applied by Update AFTER m_ui.Update, and for the same reason: a
	// content-sized Stack writes its height during ITS layout, which runs after
	// the ScrollArea has already clamped the scroll for the frame. Set either
	// before that clamp and it is clamped to zero against a grid the area does
	// not yet know is tall.
	bool m_scrollToSelected = false; // Open: show the current value
	float m_restoreScroll = -1.0f;   // Rebuild: keep the position across a rebuild
	bool m_tilesDirty = false;       // the filter moved; refill the grid's rows

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
	// The grid: a ScrollArea holding a content-sized Stack of tile rows. It owns
	// the scrolling, the clip and the scrollbar — the picker used to have its
	// own copy of all three, the last one in the codebase.
	ui::ScrollArea* m_grid = nullptr;
	ui::Stack* m_tileRows = nullptr;   // the grid's rows; refilled by RebuildTiles
	std::vector<AssetTile*> m_tiles;   // in m_shown order; dead after a Clear
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
