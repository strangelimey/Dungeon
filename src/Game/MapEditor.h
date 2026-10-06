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
#include "Game/Carve.h"           // carve::Shape (the shape brushes)
#include "Game/DungeonWorld.h"     // DungeonWorld::MoveTarget (the drag in progress)
#include "Game/Entity.h"           // Direction, WallFace
#include "Game/Placement.h"        // Mount, Placement
#include "Graphics/SpriteBatch.h"  // gfx::Rect, gfx::SpriteBatch
#include "UI/Controls.h"           // ui::Swatch
#include "UI/UIContext.h"          // ui::Theme

#include <array>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dungeon {
class Input; // Platform/Input.h — the filter box consumes TypedChars
}

namespace dungeon::game {

class MapView;
class DungeonWorld;
class StyleLibrary;
struct GameSettings;

class MapEditor {
public:
	// Left-dock palette categories, drawn as a collapsible accordion.
	// Walls/Floors/Ceilings are THE structural brushes: they pin a surface
	// variant AND convert the cell type to match (a wall texture on a floor
	// square raises the wall, a floor/ceiling texture carves rock walkable) —
	// the old Structure Wall/Floor rows folded into them; the default
	// hash-mix look is still reachable per cell via the middle-click erase
	// ladder's variant reset. The rest place catalog entities. Left-click
	// paints/places the armed brush (nothing armed until a row is picked);
	// the former Select/Erase tools live on the mouse instead — right-CLICK
	// inspects a cell (InspectAt), middle-click erases (EraseAt). Keep Count
	// last (it sizes the per-category open-state array).
	enum class PaletteCat {
		Walls, Floors, Ceilings,
		// Surface THEMES (themes.cat): a paint brush like the three above,
		// but one that sets a square's whole look - floor and ceiling on open
		// ground, the wall on a solid block - by REFERENCE, so editing the
		// theme later repaints every square it was painted on.
		Themes,
		Decorations, Fixtures, Monsters, Buttons, Doors, Stairs,
		Items, Weapons, Armor, WallFeatures, SurfaceFeatures,
		Effects, // authored + tuned, never placed (see CategoryPlaceable)
		// The WORLD tier's catalogs (docs/world-editor-plan.md W2). Like
		// Effects they are never placed in a level — but unlike Effects they
		// are pure data, so they DO offer "+ New...": there is no class behind
		// a dungeon to write first.
		Dungeons, Terrain, Quests,
		// QUEST ITEMS & FLAGS (MapEditor_Quests.cpp): the flags (flags.cat) and
		// the items that hook a quest, a flag or a reveal, in two groups - THIS
		// DUNGEON (the viewed level's) and WORLD. A flag row opens its editor; an
		// item row arms that item's brush and says where it lies, with a link
		// there. "+ New..." makes a flag.
		Flags,
		// STYLES (MapEditor_Styles.cpp, Game/Style.h): this world's, then the
		// shared library's it lacks. A world style's row ARMS it as the current
		// style (again = off), which ranks the Monsters section by its list; a
		// library row adds it to the world (onAddStyle).
		Styles,
		// SHAPES (shapes.cat, Game/Carve.h): the Stamp brush's grids. A row ARMS
		// it as the current stamp and picks the Stamp tool (again = off).
		Shapes,
		// LIGHTS (lights.cat, Game/LightProfile.h): what each kind of light
		// looks like. Never placed - a row opens its editor; a fixture or item
		// names one with its own `light`.
		Lights,
		// TRAILS (trails.cat, Game/Trail.h): what a thing in flight sheds. Never
		// placed - a row opens its editor; a spell or item names one.
		Trails,
		Count
	};

	MapEditor(MapView& view, GameSettings& settings);
	// Rebound with the view's world (see MapView::SetWorld).
	void SetWorld(DungeonWorld* world) { m_world = world; }
	// The palette's square box icons (tools/BuildToolIcons.py), BORROWED from
	// the shared caches by MapView, which has the device this does not: the
	// filter's clear box (the shared close box), and the accordion's expand /
	// collapse boxes (collapse also serves collapse-all). Null = the old text face.
	void SetIcons(const gfx::Texture* clear, const gfx::Texture* expand,
				  const gfx::Texture* collapse) {
		m_icoClear = clear;
		m_icoExpand = expand;
		m_icoCollapse = collapse;
	}

	// --- the category bar (MapEditor_Categories.cpp) ---------------------------
	// A row or two of square icon buttons at the top of the palette body: pick a
	// GROUP and the accordion below lists only that group's sections. Two ways of
	// grouping the same sections, flipped by the bar's first button (Michael:
	// "both, with a toggle to switch back and forth"):
	//   Stage - the workflow's order: World / Build / Furnishings / Populate.
	//   Kind  - what a thing is: Surfaces, Structure, Furnishings, Creatures,
	//           Items, World.
	// Both are one table each (kStageGroups / kKindGroups), so regrouping is a
	// table edit. The FILTER ignores the bar and searches every section, since a
	// search that only looked where you already are would find nothing new.
	// Effects are in neither table: they are tuning, not building, and live in
	// the Balance dialog now. Grouping + the picked group per grouping persist
	// in settings.ini (map_palette_group / _stage / _kind).
	enum class Grouping : u8 { Stage, Kind, Count };
	// The bar's button capacity: the toggle plus the larger grouping's groups.
	static constexpr size_t kMaxBarButtons = 8;
	Grouping PaletteGrouping() const;
	void SetPaletteGrouping(Grouping g);
	static int GroupCount(Grouping g);
	// "world", "build", ... - the console's names and the icon files' suffixes
	// (icon_tb_cat_<name>); the tooltip is map.group.<name>.
	static const char* GroupName(Grouping g, int group);
	static std::span<const PaletteCat> GroupCategories(Grouping g, int group);
	// The picked group in the current grouping.
	int ActiveGroup() const;
	void SetActiveGroup(int group);
	// Whether the palette lists this category at all (false for Effects).
	static bool CategoryListed(PaletteCat cat);
	// The sections the accordion lists right now, in order, each with how many
	// of its items show: the picked group's sections, or while filtering EVERY
	// section that has a match (a section with none drops out).
	struct ShownSection {
		PaletteCat cat;
		int items;
	};
	std::vector<ShownSection> ShownSections() const;
	// The filter text, for the harness (the box types it for a person).
	void SetFilter(std::string_view text);
	const std::string& Filter() const { return m_filter; }
	// The [-] button: every section and sub-group shut. And its opposite, which
	// no button offers - every section of the shown group open, every sub-group
	// in them too - so a harness can see the rows (`editor palette expand`; the
	// swatch survey, whose rows sit in shut sub-groups otherwise).
	void CollapseAll();
	void ExpandShown();
	// The bar's icons, in CategoryIconNames() order; MapView loads them (it has
	// the device) and hands them over in SetEditor. Null = a text face.
	static std::span<const char* const> CategoryIconNames();
	void SetCategoryIcons(std::span<const gfx::Texture* const> icons);
	// Draws what must sit above everything else in the editor: the category
	// bar's tooltip, which opens beside the dock over the tool strip and grid.
	void RenderOverlay(gfx::SpriteBatch& batch, const ui::Theme& theme,
					   const gfx::Rect& panel);

	// Fired when a category's "+ New..." row is clicked (the owner opens the
	// asset-creation dialog for that category).
	std::function<void(PaletteCat)> onNewAsset;
	// Fired when a configurable palette item is right-clicked (the owner opens the
	// per-type config dialog). Currently only Monsters are configurable.
	std::function<void(PaletteCat, const std::string& id)> onConfigure;
	// Fired when the Select tool clicks a cell holding a monster (the owner opens the
	// per-INSTANCE entity inspector). The cell is passed; the owner finds the monster.
	std::function<void(int cx, int cz)> onInspect;
	// Fired when the Stairs brush has just placed a WAY OUT on the active level:
	// the owner asks where it leads (the stair inspector, opened on it). An exit
	// placed on a browsed level is not asked about - the inspectors need the
	// level active - and keeps the dest it landed with.
	std::function<void(int cx, int cz)> onExitPlaced;
	// --- styles (MapEditor_Styles.cpp) --------------------------------------
	// The shared library the Styles section offers from (Game owns it).
	void SetLibrary(const StyleLibrary* library) { m_library = library; }
	// The CURRENT style: what a world style's row arms. "" = none. The Monsters
	// section ranks by its list while one is armed (its monsters first, a
	// divider, the rest - the tags lens, since the odd one out is often the
	// memorable one). Not saved: it is this session's working choice.
	const std::string& CurrentStyle() const { return m_style; }
	void SetCurrentStyle(std::string id) { m_style = std::move(id); }
	// What clicking the Styles row for `id` does: a world style toggles as the
	// current one; a library style is added to the world (onAddStyle). False
	// when no row lists it.
	bool UseStyleRow(const std::string& id);
	// Fired by a library style's row: the owner copies it into the world.
	std::function<void(const std::string& id)> onAddStyle;

	// --- the shape brushes (MapEditor_Shapes.cpp, Game/Carve.h) ----------------
	// Each lays shape on the VIEWED level in the current style: the corridor
	// and room themes, the corridor width, the knobs (winding; a region's whole
	// recipe). No style armed = plain carving, one square wide, no paint.
	// The tool strip's Corridor / Room / Region tools drag from one square to
	// another; Stamp clicks the CURRENT STAMP (a Shapes palette row) down,
	// turned with R. Every gesture previews exactly what its release commits.
	carve::Shape CorridorShape(int ax, int az, int bx, int bz) const;
	carve::Shape RoomShape(int ax, int az, int bx, int bz) const;
	carve::Shape StampShape(int cx, int cz) const;
	// Runs the generator in the rectangle (too slow to preview: the box shows
	// instead). Empty when the rectangle is under 6x6.
	carve::Shape RegionShape(int ax, int az, int bx, int bz) const;
	struct ShapeResult {
		int opened = 0, raised = 0, painted = 0;
	};
	// Commits a shape as ONE undo step: opens its squares, raises a stamp's
	// solid ones (never onto the party), paints the style's themes on what
	// opened and on the rock around it. Advances the shape seed, so the next
	// winding corridor or region differs from this one.
	ShapeResult ApplyShape(const carve::Shape& shape);
	// A Shapes row's click: arm `id` as the current stamp and pick the Stamp
	// tool - or, when it already is the stamp, put it down. False if no row.
	bool UseShapeRow(const std::string& id);
	const std::string& CurrentStamp() const { return m_stamp; }
	void SetCurrentStamp(std::string id) { m_stamp = std::move(id); }
	int StampTurns() const { return m_stampTurns; }
	void TurnStamp() { m_stampTurns = (m_stampTurns + 1) % 4; }
	void SetShapeSeed(u32 seed) { m_shapeSeed = seed; }
	// What the tool strip previews while a shape gesture is under way (MapView
	// draws it; empty = nothing).
	carve::Shape preview;

	// Fired by a palette row's go-to link (a quest item's placement): browse to
	// that level and select the square.
	std::function<void(const std::string& level, int cx, int cz)> onGoTo;
	// Fired for each grid cell clicked while LAYING a patrol route (grid-click route
	// authoring). Carries the monster's runtimeId + the cell; the owner appends it.
	std::function<void(u32 runtimeId, int cx, int cz)> onRouteWaypoint;

	// Patrol-route laying mode: while active, a grid click appends a waypoint to the
	// monster's route (via onRouteWaypoint) instead of painting the armed brush.
	// THE ROUTE BELONGS TO A LIVE MONSTER, so laying it is DERIVED, not latched
	// (code-review C232): a respawn - a type saved, an undo, a load, a level
	// change - gives every monster a new runtimeId, and the route ends with the
	// one it named. Ids are never reused within a world; a world switch ends it
	// outright (ResetSession), since the next world numbers from 1 again.
	void BeginRoute(u32 runtimeId) { m_routeId = runtimeId; }
	void EndRoute() { m_routeId = 0; }
	bool LayingRoute() const;
	u32 RouteId() const { return LayingRoute() ? m_routeId : 0; }
	// Everything this editor holds about the world it edits, let go: the route,
	// a stroke or a move under way, the selection and the shape preview. For a
	// world being unloaded (code-review C233) - the next world's monsters, cells
	// and undo history are not these.
	void ResetSession();

	// The currently SELECTED square (Select tool), for the highlight + route overlay.
	// -1 = nothing selected; SelectedMonster is the creature there at select time (0
	// = none), captured so its route still draws after it walks off the square.
	// Select a square from outside (the check report's click-to-jump). Only the
	// highlight — it deliberately does NOT open an inspector, because the report
	// is naming a place, not an object, and half the findings are about a cell
	// that holds nothing at all.
	void SelectCell(int x, int z) {
		m_selX = x;
		m_selZ = z;
		m_selMonster = 0;
	}
	bool HasSelection() const { return m_selX >= 0; }
	int SelX() const { return m_selX; }
	int SelZ() const { return m_selZ; }
	u32 SelectedMonster() const { return m_selMonster; }

	// Category metadata (one source of truth, see kCategoryInfo): the display loc
	// key, the project catalog it authors into ("" = not creatable), and whether
	// that catalog is a texture set (folder import) vs a model.
	static const char* CategoryNameKey(PaletteCat cat);
	static const char* CategoryCatalogKey(PaletteCat cat);
	static bool CategoryTextureSet(PaletteCat cat);
	// Whether this category's types go INTO the level. False for the ones you
	// only author and tune (Effects): their rows open the type editor rather
	// than arming a brush, and they offer no "+ New...".
	static bool CategoryPlaceable(PaletteCat cat);
	// Can a type of this category be created from the palette with nothing but
	// a name? True for the world tier's pure-data catalogs; false for anything
	// asset-backed (which goes through the asset dialog) and for Effects (which
	// needs a class written first).
	static bool CategoryAuthorable(PaletteCat cat);
	// The reverse lookup, for code that starts from a catalog key (the asset
	// dialog's request); Count when no category owns it.
	static PaletteCat CatForCatalogKey(std::string_view catalogKey);
	// The surface categories (walls/floors/ceilings): the ones whose palette is
	// a per-LEVEL subset of the catalog, so the "Catalogue" toggle applies and a
	// paint may have to enrol the type in the level first.
	static bool SurfaceCat(PaletteCat cat) {
		return cat == PaletteCat::Walls || cat == PaletteCat::Floors ||
			   cat == PaletteCat::Ceilings;
	}

	// One resolved palette item, for display and dispatch. `id` is the catalog id
	// (entity categories) or surface-palette id; empty for built-in tools.
	// `group` is the entry's free-form `category` field ("" = ungrouped): items
	// sharing one collapse under a sub-accordion within their palette category,
	// so a growing catalog stays navigable. Data-driven — any catalog groups
	// the moment its entries carry the field (items.cat already does).
	// `icon` (surface rows) is the entry's loaded albedo, drawn as the row
	// swatch so the palette shows the same texture the map fill does; null
	// falls back to the flat `swatch` color. Swatch() hands both to
	// ui::DrawSwatch, which is how the palette draws a row's square.
	// `onTags` is the viewed level's tags lens (DungeonMap::Tags vs the
	// entry's `tags`): on-tag items list FIRST in their run, off-tag ones
	// after a divider. Ranking only — every type stays clickable, because the
	// one-off that breaks the tags is usually the memorable thing in a dungeon.
	// True for everything when the level has no tags.
	struct PaletteItem {
		std::string label;
		Vec4 swatch{1, 1, 1, 1};
		std::string id;
		std::string group;
		const gfx::Texture* icon = nullptr;
		bool onTags = true;
		// A monster's power BAND (Game/Power.h), drawn as pips at the row's end:
		// 1..5 filled of five. 0 = no pips (everything that is not a monster).
		int band = 0;
		// A row standing for ANOTHER catalog's type (the Quest items & flags
		// section lists items): that catalog's key, which its click and its
		// right-click act on. "" = the section's own catalog.
		std::string ref;
		// Where the thing lies, when the row can take you there ("" = nowhere):
		// the row ends in a link that browses to that level and square.
		std::string gotoLevel;
		int gotoX = -1, gotoZ = -1;
		ui::Swatch Swatch() const { return {icon, swatch}; }
	};
	// A section's rows exactly as the accordion resolves them (label, id,
	// band...), for the harness - `editor palette items <catalog>`.
	std::vector<PaletteItem> Items(PaletteCat cat) const { return CategoryItems(cat); }
	// One surface type (a Walls/Floors/Ceilings category) as the palette shows
	// it: display name, group, the swatch and the flat fallback colour. Public
	// so a dialog listing surface types (a theme's members) shows them exactly
	// as the palette does. The swatch is the type's LINEAR thumbnail
	// (DungeonWorld::SurfaceSwatchForId - never the scene's sRGB albedo, which
	// drew too dark, code-review C158).
	PaletteItem SurfaceItem(PaletteCat cat, const std::string& id) const;
	// Just that swatch, for a row that asks for it every time it draws and must
	// never keep it (code-review C235: it was the world's albedo once, which a
	// quality change reloaded under an open dialog). Lookup only, safe
	// mid-frame - a thumbnail not loaded yet is the flat colour, and asked for.
	ui::Swatch SurfaceSwatch(PaletteCat cat, const std::string& id) const;
	// Loads the thumbnail swatch for a surface type now. Uploads, so from
	// Update only: a list about to show catalogue types (a theme's member
	// lists) asks for them first rather than a frame after they draw.
	void LoadSurfaceSwatch(PaletteCat cat, const std::string& id);
	// The texture swatches the palette DREW last frame, with their rects (device
	// px - what a photograph of the window holds) - the swatch brightness
	// survey (`editor palette swatches`, C158). Recorded by the draw into
	// reserved room, so it costs the frame nothing.
	struct DrawnSwatch {
		const gfx::Texture* texture = nullptr;
		gfx::Rect rect{};
	};
	std::span<const DrawnSwatch> DrawnSwatches() const { return m_drawnSwatches; }

	// --- surface palette membership ------------------------------------------
	// Appends `id` to the viewed level's palette (live world or browsed stash),
	// as one undo step, and arms it as the brush. Reports through onMessage.
	// The "+ New" flow calls this so a freshly created surface type is paintable
	// at once; the "Catalogue" view instead enrols a type lazily, on first paint.
	void AddToPalette(PaletteCat cat, const std::string& id);
	// Arms the palette row for `id` in `cat`, as clicking it would. False when
	// the category does not list it. For the harness (`editor place`), which
	// cannot click a palette row but must drive the real brush path.
	bool Arm(PaletteCat cat, const std::string& id);

	// --- palette controls row (filter box + clear + collapse-all) ------------
	// A fixed strip at the top of the dock body, above the scrolled accordion.
	// The FILTER restricts the palette to items whose label contains the text
	// (case-insensitive); while active, matching items show flat under their
	// category header regardless of accordion state, and empty categories
	// drop out. Clicking the box focuses it: typed characters land here and
	// the GAME's keyboard is captured (Game gates the party/M/Esc on
	// KeyboardCaptured), Esc/Enter release it, Backspace edits.
	void HandleTyping(const Input& input);
	// Hover tracking for the controls row (called per Update with the live
	// mouse — render styles by identity across the window/device px split).
	void TrackMouse(float mx, float my, const gfx::Rect& panel);
	bool KeyboardCaptured() const { return m_filterFocused; }
	void DropFilterFocus() { m_filterFocused = false; } // grid click steals focus

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
	// (only paint brushes act on a drag; placement acts on the click only). A
	// no-op until a palette row is armed.
	// `pre` is the placement the HOVER GHOST already resolved and drew this
	// frame. Passing it is what makes the commit literally the previewed pose
	// rather than a second resolve that agrees by construction — the pointer has
	// not moved between the two, but handing the answer over removes the
	// question. Null re-resolves at the cell centre (the rect/flood fallbacks,
	// which have no pointer inside a cell to speak of).
	void Paint(int cx, int cz, bool dragging, const WallFace& face = {},
			   const Placement* pre = nullptr) {
		ApplyBrush(cx, cz, dragging, face, pre);
	}
	// A paint STROKE - the left press to its release - is ONE undo step. MapView
	// opens it on the press and closes it on the release, so the step is decided
	// by the WHOLE stroke: a drag whose first cell already had the texture used
	// to drop its step on the press and leave every later cell unundoable. The
	// release is also the "an edit just ended" moment the live checker waits for.
	// A Paint outside a stroke (the console's `editor place`) brackets itself.
	void BeginStroke();
	void EndStroke();
	bool StrokeOpen() const { return m_strokeOpen; }
	// True when something is armed AND it is a thing that gets PLACED (not a
	// surface paint, not a non-placeable category). The hover ghost keys off
	// this: a wall-texture brush has no pose to preview, only a cell to fill.
	bool ArmedPlaceable() const {
		return m_sel.index >= 0 && CategoryPlaceable(m_sel.cat) &&
			   !PaintableCat(m_sel.cat);
	}
	// The armed brush's mount — what it attaches to (Placement.h). Data-driven:
	// the type's own `mount` field, or its category's default.
	Mount BrushMount() const;
	// True when the armed brush hangs on a WALL FACE rather than occupying a
	// square: a niche, or any catalog kind declaring `mount = wall` (the sconce
	// today, any future wall decoration for free). MapView keys its edge-pick and
	// hover highlight off this, so adding a wall-mounted kind is a catalog edit,
	// not a code change.
	bool BrushIsWallMounted() const;
	// Where the armed brush would land for this cell + picked face, and whether
	// it may land at all. THE hover preview and THE commit both call this — see
	// the header note in Placement.h about why there may be only one resolver.
	// `fx`/`fz` are the pointer's fractional position inside the cell, which a
	// sub-cell (FloorSlot) mount needs and the rest ignore.
	Placement ResolveBrush(int cx, int cz, const WallFace& face, float fx = 0.5f,
						   float fz = 0.5f) const;
	// Modifier gestures (MapView routes by the modifier held at the press).
	// All three work on the paint brushes (the surface categories);
	// rect/flood fall back to a normal click for the placement categories.
	// Shift+click: fill the rectangle spanned by the LAST painted cell (the
	// anchor every plain paint and gesture leaves behind) and this one — the
	// Photoshop shift-line idiom. One undo step.
	void PaintRect(int cx, int cz);
	// Ctrl+click: contiguous flood fill (4-connected) of the clicked region —
	// same cell type, and for surface brushes the same RESOLVED variant
	// (override-or-hash, so it matches what the 3D scene shows). One undo step.
	void FloodFill(int cx, int cz);
	// The AREA fill: paint the room or corridor holding walkable square (cx,cz)
	// (Game/Area.h - it stops where narrow meets open). A floor or ceiling brush
	// paints the area's squares, a wall brush the wall blocks around it. Unlike
	// flood it ignores what the squares wear now, which is the point: it
	// unifies a mixed corridor. A solid square has no area; says so. One undo
	// step, one chunk rebuild each.
	void AreaFill(int cx, int cz);
	// Fill level: the armed surface brush on EVERY square of its kind on the
	// viewed level (walls on every solid square, floors/ceilings on every
	// walkable one). One undo step.
	void FillLevel();
	// The TOOL STRIP's tools (MapView draws the strip beside the palette). The
	// picked tool decides what a left press on the grid does with the armed
	// brush; holding Shift / Ctrl / Alt borrows Rectangle / Flood / Eyedropper
	// for that one click, so the old gestures keep working as shortcuts. A
	// placement brush places on a click whatever tool is picked. Persisted as
	// settings.ini `map_tool`.
	// The SHAPE tools (MapEditor_Shapes.cpp, Game/Carve.h) follow: they need no
	// armed brush - they carve in the CURRENT STYLE (its themes, its corridor
	// width, its shape knobs), and carve plainly when none is armed.
	enum class Tool : u8 { Paint, Rect, Flood, Area, Pick, Corridor, Room, Stamp, Region, Count };
	static bool ShapeTool(Tool t) { return t >= Tool::Corridor && t < Tool::Count; }
	Tool ActiveTool() const;
	void SetTool(Tool t);
	// "paint", "rect", ... - the console's names and the icon files' suffixes.
	static const char* ToolName(Tool t);
	// The Rectangle tool's drag: fill the box between two corners, one undo
	// step (Shift+click is this from the last painted square).
	void PaintRectBetween(int ax, int az, int bx, int bz);
	// True when the armed brush PAINTS squares - the only brushes the tools
	// change the meaning of.
	bool ArmedPaints() const { return m_sel.index >= 0 && PaintableCat(m_sel.cat); }
	// Alt+click: eyedropper — arms the brush from the clicked square (a solid
	// square arms its wall texture, a floor square its floor texture; ceilings
	// are picked while the Ceilings brush is armed, since they share the floor
	// square). Never mutates.
	void PickAt(int cx, int cz);
	// The armed brush's palette category, or Count when nothing is armed.
	// MapView reads it to flip the grid's textured cell fill to the surface
	// being painted (Walls/Floors/Ceilings show their textures while armed).
	PaletteCat ArmedCat() const {
		return m_sel.index >= 0 ? m_sel.cat : PaletteCat::Count;
	}
	// The armed row's catalog id ("" = nothing armed) - for the console.
	std::string ArmedId() const;
	// The former Select tool, now on right-CLICK (a right-drag still pans):
	// reports the cell's contents, selects the square (highlight + patrol-route
	// overlay), and opens the inspector immediately when it holds an editable
	// object (onInspect — the owner picks the dialog, via the multi-object
	// chooser when several share the cell).
	void InspectAt(int cx, int cz);
	// The former Erase tool, now on middle-click: the removal ladder (stair pair
	// → entity → fixture → reset surface variants), one undo step per click.
	// `face` (when valid) is the wall face under the pointer, so a niche on a
	// specific face erases precisely; invalid falls back to the cell-wide ladder.
	// Returns whether anything changed: a square with nothing on it and no
	// surface override erases nothing, and takes no undo step for it (the
	// `editor erase` command reports the truth from this - code-review C442).
	bool EraseAt(int cx, int cz, const WallFace& face = {});

	// --- drag-and-drop move (play-test #2) -------------------------------------
	// With NO brush armed, a left-drag moves the top thing on a square
	// (DungeonWorld::TopMovableAt / MoveObject). BeginMove picks it up and says
	// what it took (false = nothing to take, or a browsed level); EndMove drops it
	// as one undo step and says whether it moved (false = dropped where it was,
	// or the square refused it); CancelMove lets go without moving anything.
	bool BeginMove(int cx, int cz);
	bool EndMove(int cx, int cz);
	void CancelMove() { m_moving = false; }
	bool Moving() const { return m_moving; }
	int MoveFromX() const { return m_move.x; }
	int MoveFromZ() const { return m_move.z; }
	// Puts the brush down ("but we'll need a way to 'disarm' the brush"): Esc,
	// or clicking the armed palette row again. False when nothing was armed.
	bool Disarm();
	// Draws the accordion inside the left dock body. MapView draws the dock frame,
	// collapse button and "Brushes" header around it; this scissors to the body.
	void RenderBody(gfx::SpriteBatch& batch, const ui::Theme& theme,
					const gfx::Rect& panel);

private:
	// The armed palette entry: a category plus an item index within it.
	// index -1 = nothing armed yet (left-click does nothing until a row is
	// picked — the mouse-button inspect/erase work regardless).
	struct Selection {
		PaletteCat cat = PaletteCat::Walls;
		int index = -1;
	};

	// Accordion layout, shared by hit-test and draw: one row per category header,
	// per visible item, per group sub-header, and per empty-expanded placeholder.
	// Rects are in panel pixel space with the scroll already applied.
	// Divider: the tags lens's boundary inside one run of items — everything
	// below it is off-tag. Emitted only when both sides are non-empty, so an
	// untagged level (or a fully on-tag one) never sees one.
	struct PaletteRow {
		enum class Kind { Header, NewButton, SubHeader, Item, Empty, Divider } kind;
		PaletteCat cat;
		int index; // item index for Kind::Item
		gfx::Rect rect;
		std::string group; // SubHeader: the group it toggles; Item: its group
	};

	// Sub-accordion expand state, keyed "<cat>/<group>" (transient, like
	// m_catOpen). Groups default COLLAPSED — the whole point is a short list.
	static std::string GroupKey(PaletteCat cat, const std::string& group) {
		return std::to_string(static_cast<int>(cat)) + "/" + group;
	}
	bool GroupOpen(PaletteCat cat, const std::string& group) const {
		const auto it = m_groupOpen.find(GroupKey(cat, group));
		// The Quest items & flags and Styles sections' groups ARE the section
		// (this dungeon / world, this world / library), so they start open.
		if (it == m_groupOpen.end())
			return cat == PaletteCat::Flags || cat == PaletteCat::Styles;
		return it->second;
	}

	// --- the Quest items & flags section (MapEditor_Quests.cpp) ---------------
	// Its rows: this dungeon's and the world's flags, then the quest items whose
	// scope is each (an item's is the scope of the flag it sets; the world's
	// when it sets none - a quest and a reveal are world-tier).
	std::vector<PaletteItem> QuestSectionItems() const;
	// A left click on one of its rows: a flag opens its editor; an item arms
	// that item's brush (again = disarm), or on its link goes to where it lies.
	void QuestRowClick(const PaletteRow& row, float mx, float my);
	void UseQuestRow(const PaletteItem& item, bool link);

public:
	// What clicking the Quest items & flags row for `id` does - on its link when
	// `link` - for the harness, which cannot click. False if no row lists it.
	bool UseQuestRow(const std::string& id, bool link);

private:
	// The go-to link at a row's end: a square the row's height.
	static gfx::Rect GoToRect(const gfx::Rect& row) {
		return {row.x + row.w - row.h, row.y, row.h, row.h};
	}
	// The category a row's click acts on: its `ref` catalog's, else its own.
	// Count for a row with no catalog of the world's behind it (a library
	// style, ref = kLibraryRef).
	static PaletteCat RowCat(PaletteCat cat, const PaletteItem& item) {
		return item.ref.empty() ? cat : CatForCatalogKey(item.ref);
	}
	static constexpr const char* kLibraryRef = "library";

	// --- the Styles section (MapEditor_Styles.cpp) ------------------------------
	// Its rows: the world's styles, then the library's the world lacks.
	std::vector<PaletteItem> StyleSectionItems() const;
	// A style's weighted monster ids, when `id` names one of the world's (else
	// empty): the Monsters section's lens.
	std::vector<std::string> StyleMonsters(const std::string& id) const;
	const StyleLibrary* m_library = nullptr;
	std::string m_style; // the current style (see CurrentStyle)
	// The shape brushes' state: the armed stamp, its turn, and the seed the next
	// winding corridor / region rolls from.
	std::string m_stamp;
	int m_stampTurns = 0;
	u32 m_shapeSeed = 1;
	// The current style's entry ("" style = null).
	const CatalogEntry* StyleEntry() const;

	// Every category authors new assets — each gets a "+ New..." row that
	// opens the asset dialog.
	// "+ New..." appears only where the editor can actually author a new type.
	// An effect can't be created from data alone — it needs a C++ class — so
	// its category offers browse-and-edit only.
	// Which categories offer "+ New...": the placeable ones (through the asset
	// dialog) and the pure-data ones (directly, by name).
	static bool Creatable(PaletteCat cat) {
		return CategoryPlaceable(cat) || CategoryAuthorable(cat);
	}

	// The items of a category, resolved from the project's catalogs / the
	// level palette (Walls/Floors/Ceilings/entities).
	std::vector<PaletteItem> CategoryItems(PaletteCat cat) const;

	// The category bar's buttons: the grouping toggle first, then one per group
	// of the current grouping, wrapping onto a second row when the dock is
	// narrow. `group` -1 = the toggle. A fixed array - at most the toggle plus
	// the larger grouping's groups.
	struct BarButton {
		gfx::Rect rect;
		int group;
	};
	struct BarLayout {
		std::array<BarButton, kMaxBarButtons> buttons{};
		size_t count = 0;
		gfx::Rect area; // every row of the bar
	};
	BarLayout CategoryBar(const gfx::Rect& panel) const;
	// The sections the accordion walks before the filter drops the empty ones:
	// the picked group's, or every listed section while filtering.
	std::vector<PaletteCat> CandidateSections() const;
	// Makes sure the picked group shows SOMETHING: if none of its sections is
	// open, the first one opens. Run after every change of group or grouping.
	void OpenSomethingInGroup();
	// Switches to the group holding `cat` (in the current grouping) and opens
	// its section - so a brush armed from outside the bar (an eyedropper, a new
	// palette entry) is visible where it was armed.
	void RevealCategory(PaletteCat cat);
	// The bar button under a point: its group, -1 the toggle, -2 none.
	int BarButtonAt(float mx, float my, const gfx::Rect& panel) const;
	// A click in the bar: flips the grouping or picks a group. True when the
	// click landed anywhere in the bar (a gap between buttons still claims it).
	bool OnBarClick(float mx, float my, const gfx::Rect& panel);
	void RenderCategoryBar(gfx::SpriteBatch& batch, const ui::Theme& theme,
						   const gfx::Rect& panel);
	int m_hotBar = -2; // the hovered bar button's group (-1 toggle, -2 none)
	// The palette row under the pointer (TrackMouse, window px), and - when
	// that row's name had to be trimmed to fit - its full name and the row's
	// rect as RenderBody drew it (device px), for RenderOverlay's tooltip.
	// Reset at the top of every RenderBody, so a tip never outlives its row.
	Selection m_hoverItem{PaletteCat::Count, -1};
	std::string m_rowTip;
	gfx::Rect m_rowTipAt{};
	// DrawnSwatches: cleared at the top of every RenderBody, filled as it draws
	// a row's texture swatch, never past the room reserved at construction.
	static constexpr size_t kDrawnSwatchRoom = 96;
	std::vector<DrawnSwatch> m_drawnSwatches;
	std::array<const gfx::Texture*, 16> m_icoCats{}; // see SetCategoryIcons

	// Controls-row geometry (all derived from the panel like the dock chrome):
	// [filter box............][x][-] on one line under the category bar; the
	// accordion lays out in the remainder (AccordionBody).
	gfx::Rect ControlsRow(const gfx::Rect& panel) const;
	gfx::Rect FilterBoxRect(const gfx::Rect& panel) const;
	gfx::Rect FilterClearRect(const gfx::Rect& panel) const;
	gfx::Rect CollapseAllRect(const gfx::Rect& panel) const;
	// The "Catalogue" checkbox, a second controls line below the filter row.
	// Unchecked: surfaces list only the level's palette. Checked: they list the
	// whole surface catalog, and painting a type the level lacks enrols it.
	gfx::Rect CatalogToggleRect(const gfx::Rect& panel) const;
	gfx::Rect AccordionBody(const gfx::Rect& panel) const;
	bool MatchesFilter(const std::string& label) const;

	std::string m_filter;         // case-insensitive substring, "" = off
	bool m_filterFocused = false; // typed chars land in the box
	// The filter's length in CHARACTERS (UTF-8, C383), for the box and for
	// SetFilter alike: a byte cut could keep half a letter that never matches.
	static constexpr size_t kFilterMaxChars = 24;
	// Whether surfaces show the whole catalog vs the level's palette lives on
	// GameSettings (m_settings.mapShowCatalog) so it PERSISTS in settings.ini
	// like the dock-collapse flags — a workflow preference, not per-session
	// state. Toggling it Save()s (MapEditor holds a GameSettings&).
	// Which control the mouse is over (hover styling; None = neither).
	enum class HotCtrl { None, Filter, Clear, Collapse, Catalog, Bar };
	HotCtrl m_hotCtrl = HotCtrl::None;
	const gfx::Texture *m_icoClear = nullptr, *m_icoExpand = nullptr,
					   *m_icoCollapse = nullptr; // see SetIcons
	void BuildPaletteRows(const gfx::Rect& panel, std::vector<PaletteRow>& out,
						  float& contentHeight) const;
	// Applies the armed selection to cell (cx,cz): structural/variant paints, tool
	// actions, or entity placement.
	void ApplyBrush(int cx, int cz, bool dragging, const WallFace& face = {},
					const Placement* pre = nullptr);
	// True for the brushes that PAINT cells (rect/flood/drag apply): the three
	// surfaces and the themes. The placement categories act per click only.
	static bool PaintableCat(PaletteCat cat) {
		return SurfaceCat(cat) || cat == PaletteCat::Themes;
	}
	// A theme's paint on one square: its floor and ceiling mixes on open
	// ground, its wall mix on a solid block (PaintCell's theme half). A
	// theme RECOLOURS - it never changes the square's type.
	void PaintThemeCell(int cx, int cz, bool remote, const std::string& stem);
	// The same with the theme NAMED and the square's openness SAID rather than
	// read - a shape brush paints squares it has just changed, which a browsed
	// level's snapshot does not show yet.
	void PaintThemeAs(const std::string& id, int cx, int cz, bool open, bool remote,
					  const std::string& stem);
	// One structural/surface application of the armed brush to a cell — the
	// shared inner body of ApplyBrush/PaintRect/FloodFill. No undo bracketing
	// or change detection (callers bracket a whole gesture as one step).
	void PaintCell(int cx, int cz, bool remote, const std::string& stem);
	// PaintCell over a whole set of squares on the viewed level as ONE undo
	// step and one chunk batch - the body the area and level fills share.
	void PaintCells(std::span<const std::pair<int, int>> cells);
	// The viewed cell's RESOLVED surface variant (override else the mesh
	// builder's hash — exactly what the scene shows): the flood fill's region
	// key and the eyedropper's pick. -1 when the palette is empty.
	int ResolvedVariant(int cx, int cz, int sel) const; // sel = SurfaceSel
	int m_lastX = -1, m_lastZ = -1; // rect anchor: the last painted cell
	bool m_strokeOpen = false;    // BeginStroke .. EndStroke
	bool m_strokeChanged = false; // anything in the open stroke changed

	MapView& m_view;          // the viewport (layout helpers, shared font)
	DungeonWorld* m_world = nullptr; // see SetWorld
	GameSettings& m_settings; // owns the palette-collapse flag (read for layout)

	Selection m_sel; // armed palette entry
	// Per-category accordion expand state; Walls opens by default.
	std::array<bool, static_cast<size_t>(PaletteCat::Count)> m_catOpen{};
	std::map<std::string, bool> m_groupOpen; // sub-accordions (see GroupKey)
	float m_paletteScroll = 0.0f; // left-dock vertical scroll (pixels)
	u32 m_routeId = 0;            // monster whose patrol route is being laid (0 = none)
	int m_selX = -1, m_selZ = -1; // selected square (-1 = none)
	u32 m_selMonster = 0;         // creature selected there (0 = none), for its route
	// The drag in progress (BeginMove .. EndMove): what was picked up, and where.
	DungeonWorld::MoveTarget m_move;
	bool m_moving = false;
};

} // namespace dungeon::game
