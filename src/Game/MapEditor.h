// ============================================================================
// Game/MapEditor.h — the in-game dungeon editor's brush palette + tools.
//
// Extracted from MapView so the shared map VIEWPORT (pan/zoom, cell/marker/party
// render, fog, the symbol key) stays small and the EDITOR can grow without
// bloating the player map. MapEditor is a collaborator of MapView, not a
// subclass: MapView owns the viewport and, while in Editor mode, drives this
// class for the left-dock palette and cell painting. Owning them separately
// keeps MapView's in-place mode flip (Player <-> Editor) trivial — Game just
// constructs both and points the view at the editor (MapView::SetEditor).
//
// Responsibilities here: the left-dock accordion palette (categories, item
// selection, "+ New..." asset creation), and applying the armed brush to a
// grid cell (structural/variant paint, Select/Erase tools, entity placement).
// MapView still draws the dock FRAME, collapse button and "Brushes" header and
// performs the grid hit-test; this class fills the body and resolves the brush.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"        // Vec4
#include "Graphics/SpriteBatch.h"  // gfx::Rect, gfx::SpriteBatch
#include "UI/UIContext.h"          // ui::Theme

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class MapView;
class DungeonWorld;
struct GameSettings;

class MapEditor {
public:
	// Left-dock palette categories, drawn as a collapsible accordion. Tools is the
	// built-in Select/Erase group; Structure toggles a cell solid/floor; Walls/
	// Floors/Ceilings pin a surface variant on the clicked floor cell; the rest
	// place catalog entities. A selection is always armed — left paints/places.
	// Keep Count last (it sizes the per-category open-state array).
	enum class PaletteCat {
		Tools, Structure, Walls, Floors, Ceilings,
		Decorations, Fixtures, Monsters, Doors, Stairs, Items, Count
	};

	MapEditor(MapView& view, DungeonWorld& world, GameSettings& settings);

	// Fired when a category's "+ New..." row is clicked (the owner opens the
	// asset-creation dialog for that category).
	std::function<void(PaletteCat)> onNewAsset;
	// Fired when a configurable palette item is right-clicked (the owner opens the
	// per-type config dialog). Currently only Monsters are configurable.
	std::function<void(PaletteCat, const std::string& id)> onConfigure;

	// Category metadata (one source of truth, see kCategoryInfo): the display loc
	// key, the project catalog it authors into ("" = not creatable), and whether
	// that catalog is a texture set (folder import) vs a model.
	static const char* CategoryNameKey(PaletteCat cat);
	static const char* CategoryCatalogKey(PaletteCat cat);
	static bool CategoryTextureSet(PaletteCat cat);

	// Mouse wheel over the (expanded) left dock scrolls the accordion.
	void OnWheel(float delta, const gfx::Rect& panel);
	// A click inside the palette body: toggle a category, arm an item, or fire
	// onNewAsset. Returns true if a row was hit (the caller already treats any
	// click in the dock body as consumed).
	bool OnClick(float mx, float my, const gfx::Rect& panel);
	// A right-click inside the palette body: on a configurable item (Monsters),
	// fire onConfigure with its catalog id. Returns true if a row was hit.
	bool OnRightClick(float mx, float my, const gfx::Rect& panel);
	// Paint/place the armed brush at grid cell (cx,cz). MapView calls this once on
	// a fresh press and per-frame while held; `dragging` is true for held strokes
	// (only paint brushes act on a drag; tools/placement act on the click only).
	void Paint(int cx, int cz, bool dragging) { ApplyBrush(cx, cz, dragging); }
	// Draws the accordion inside the left dock body. MapView draws the dock frame,
	// collapse button and "Brushes" header around it; this scissors to the body.
	void RenderBody(gfx::SpriteBatch& batch, const ui::Theme& theme,
					const gfx::Rect& panel);

private:
	// The armed palette entry: a category plus an item index within it.
	struct Selection {
		PaletteCat cat = PaletteCat::Tools;
		int index = 0;
	};

	// One resolved palette item, for display and dispatch. `id` is the catalog id
	// (entity categories) or surface-palette id; empty for built-in tools.
	struct PaletteItem {
		std::string label;
		Vec4 swatch{1, 1, 1, 1};
		std::string id;
	};

	// Accordion layout, shared by hit-test and draw: one row per category header,
	// per visible item, and per empty-expanded placeholder. Rects are in panel
	// pixel space with the scroll already applied.
	struct PaletteRow {
		enum class Kind { Header, NewButton, Item, Empty } kind;
		PaletteCat cat;
		int index; // item index for Kind::Item
		gfx::Rect rect;
	};

	// Categories that can author new assets (everything but the built-in tools and
	// structure brushes) get a "+ New..." row that opens the asset dialog.
	static bool Creatable(PaletteCat cat) {
		return cat != PaletteCat::Tools && cat != PaletteCat::Structure;
	}

	// The items of a category: built-in (Tools/Structure) or resolved from the
	// project's catalogs / the level palette (Walls/Floors/Ceilings/entities).
	std::vector<PaletteItem> CategoryItems(PaletteCat cat) const;
	void BuildPaletteRows(const gfx::Rect& panel, std::vector<PaletteRow>& out,
						  float& contentHeight) const;
	// Applies the armed selection to cell (cx,cz): structural/variant paints, tool
	// actions, or entity placement.
	void ApplyBrush(int cx, int cz, bool dragging);

	MapView& m_view;          // the viewport (layout helpers, shared font)
	DungeonWorld& m_world;
	GameSettings& m_settings; // owns the palette-collapse flag (read for layout)

	Selection m_sel; // armed palette entry
	// Per-category accordion expand state; Tools + Structure + Walls open by default.
	std::array<bool, static_cast<size_t>(PaletteCat::Count)> m_catOpen{};
	float m_paletteScroll = 0.0f; // left-dock vertical scroll (pixels)
};

} // namespace dungeon::game
