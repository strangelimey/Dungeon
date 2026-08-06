// ============================================================================
// Game/DungeonMap.h — the STATIC layer of a level: everything that never
// changes during play, so a save system never needs to store it.
//
// Levels are ASCII grid text files under the project's levels/ folder
// (assets/projects/<name>/levels/<stem>.map — see Project::MapPath; the glyph
// legend: '#' rock, '.' floor, 'D' dust, 'T' sconce, 'F' brazier, 'P' start;
// ';' lines are comments). Lines starting with a lowercase letter are records
// — grid glyphs are never lowercase, so the two can't collide. The whole
// grammar, in the constructor's dispatch order:
//   palette <wall|floor|ceiling> <id> [...]  surface palette (catalog ids)
//   fixture <id> <x> <z> [facing] [lit=] [bright=] [turb=]
//                                     a fixtures.cat kind — `mount = wall`
//                                     puts it on a wall (facing names which),
//                                     anything else stands on the floor
//   niche [<type>] <x> <z> [facing] [name=] [hidden=]
//                                     recessed pocket on a solid wall
//   bore [<type>] <x> <z> <axis>      see-through hole THROUGH a wall block
//   stairs <type> <x> <z> [facing] dest= destx= destz= [destfacing=]
//   variant <wall|floor|ceiling> <x> <z> <index>   per-cell surface override
//   atmosphere [dust=] [haze=] [ambient=]  the level's mood knobs
//   decoration <type> <x> <z> [facing] [wall=]
//                                     static entity (Entity.h) — and the
//                                     FALL-THROUGH: any record matching none
//                                     of the above is parsed as one
// A niche/bore whose wall no longer suits it is DROPPED with a warning
// (tolerant, since the editor can repaint around it); a decoration that lands
// in rock is a hard assert. Both take an optional leading type token,
// distinguished from the x coordinate by whether it parses as an integer, so
// pre-typing records still load.
// The 'T'/'F' glyphs are terse shorthand for a single auto-faced default
// sconce/brazier (FixtureTypes carries those ids); the fixture record names
// kinds explicitly, so several can share a cell (e.g. two sconces on different
// walls) — records always allow that.
// Every level declares its texture palette; the game loads only those sets
// (and their worn block meshes), nothing else. Dynamic content (monsters,
// items, buttons, doors) lives in the .ent file beside the .map — see
// DungeonEntities.h. The constructor parses and validates the file — unknown
// glyphs, ragged rows, a missing start cell, or a missing/duplicate texture
// palette fail hard with the file name and position.
//
// Cell (x, z) maps to world space as center ((x+0.5), 0, (z+0.5)) * kCellSize
// with +X = east and +Z = south (row index grows southward). The geometry
// builder, fires, turbidity grid, and entity placement all derive from this.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Game/Entity.h"

#include <algorithm>
#include <string>
#include <vector>

namespace dungeon::game {

// THE SCALE AUTHORITY. Metres per dungeon square — and the square is a CUBE,
// so this one number is the cell footprint AND the floor-to-ceiling height.
//
// Every model on disk (assets/models/*.gltf, procedural and imported alike) is
// authored in UNITS, where 1.0 = one square: a hand-built column 1.0 tall
// reaches the ceiling exactly. The engine multiplies by kUnit at the handful of
// places a mesh becomes world geometry (DungeonMeshBuilder::StampCell, the
// decoration/fixture/stair/door/button transforms, the monster and item draws).
// So changing this number rescales the whole world coherently with NO asset
// rebake — see docs/authoring-scale.md.
inline constexpr float kUnit = 2.5f;
// World-space dimensions shared by geometry, lighting, and the camera. All
// three derive from the unit; nothing else should hardcode a metre.
inline constexpr float kCellSize = kUnit;   // square cells, one unit on a side
inline constexpr float kWallHeight = kUnit; // floor to ceiling (the cell is a cube)
inline constexpr float kEyeHeight = 0.62f * kUnit; // camera height above the floor

// The scale that turns a unit-space authored mesh into world geometry. Every
// mesh-to-world transform in the engine starts with this (see the seam list in
// docs/authoring-scale.md). `extra` is the per-kind multiplier a catalog may
// add on top — monsters' `modelscale`, an item's placeholder scale.
inline DirectX::XMMATRIX UnitScale(float extra = 1.0f) {
	const float s = kUnit * extra;
	return DirectX::XMMatrixScaling(s, s, s);
}

enum class Cell : u8 { Wall, Floor };

// Per-torch light/smoke defaults (also the "don't write it" baseline for the
// .map fixture record — only non-default values are serialized).
// Turbidity defaults were halved in the lighting mood pass (0.28/0.55 →
// 0.16/0.38): fire smoke used to SUM linearly across neighbours, so a
// sconce-lined hall saturated to 'D'-cell thickness and the in-scatter wash
// buried every surface shadow (~1.5% mean pixel effect with shadows off).
inline constexpr float kSconceBrightness = 3.0f; // light reach, in cells
inline constexpr float kSconceTurbidity = 0.16f; // smokiness added to cell + ring
// Floor for a fixture record's bright= value: a zero-radius lit light would
// feed 1/radius = inf into the shadow pass, so the parser clamps up to this.
inline constexpr float kFixtureMinBrightness = 0.25f;

// A wall-mounted torch sconce: its cell plus the wall it hangs on (the
// direction from the cell to the solid neighbour it mounts against). Several
// sconces may share a cell on different walls. `lit` gates its light, flame
// particles and smoke; `brightness` is the light reach in cells; `turbidity`
// is how much haze it adds to its cell and neighbours. `type` is the
// fixtures.cat id this instance renders as (the parser fills the default for
// glyph shorthand, so it is never empty).
struct WallSconce {
	int x = 0, z = 0;
	Direction wall = Direction::North;
	bool lit = true;
	float brightness = kSconceBrightness;
	float turbidity = kSconceTurbidity;
	std::string type = "sconce";
};

// Per-brazier defaults (bigger reach + more smoke than a sconce), same "don't
// write the default" rule on the .map record.
inline constexpr float kBrazierBrightness = 6.0f; // light reach, in cells
inline constexpr float kBrazierTurbidity = 0.38f;

// A floor-standing brazier: its cell plus the same light/smoke knobs a sconce
// has (no wall — it stands at the cell centre). `type` as on WallSconce.
struct FloorBrazier {
	int x = 0, z = 0;
	bool lit = true;
	float brightness = kBrazierBrightness;
	float turbidity = kBrazierTurbidity;
	std::string type = "brazier";
};

// A wall NICHE: a recessed pocket carved into one solid wall of a walkable cell
// (`x`,`z` = the cell; `wall` = the direction to the solid neighbour, like a
// WallSconce). It is not a discrete prop — the mesh builder stamps the
// wall_niche panel instead of the plain wall panel on that edge (see
// wall-details.md Phase 2). `type` is the wallfeatures.cat id.
struct WallNiche {
	int x = 0, z = 0;
	Direction wall = Direction::North;
	std::string type = "niche";
	// A secret niche starts CLOSED (a blank wall) and is opened by a button whose
	// target= names it, or the editor's inspector. `name` is that target id (""
	// = not button-toggleable). `hidden` is the authored start state; `open` is
	// the runtime visibility (initialised to !hidden). The mesh builder stamps
	// the niche panel only when `open`, else the plain wall panel.
	std::string name;
	bool hidden = false;
	bool open = true;
};

// A wall WINDOW/bore: a see-through hole THROUGH a solid wall block (Phase 3).
// Stored on the SOLID cell it bores; `axis` is the see-through direction
// (0 = X / east-west, 1 = Z / north-south — the axis whose two flanking cells
// are floor). LoS + projectiles pass through it; movement does not (the cell
// stays solid). The mesh builder stamps a bore panel on the two flanking faces.
struct WallBore {
	int x = 0, z = 0;
	int axis = 0;
	std::string type = "window"; // wallfeatures.cat id (its bore mesh / shape)
};

// A FLOOR FEATURE: the WallNiche idea laid flat. A recess carved into a walkable
// cell's floor — the mesh builder stamps the feature's tile instead of the plain
// floor block for that cell, into the same variant bucket, so it wears the
// cell's floor texture. `type` is the floorfeatures.cat id.
//
// No `wall` and no facing: a floor has one orientation. And no `hidden`/`open`
// pair — a secret niche has a blank wall to hide behind, whereas a hole in the
// floor either is there or is not. If a covered pit ever wants one, it should
// arrive as a trapdoor with real state, not as a mesh swap.
struct FloorFeature {
	int x = 0, z = 0;
	std::string type = "recess";
};

// How the parser routes a `fixture <id> ...` record and the 'T'/'F' glyphs
// without knowing the fixtures catalog: ids listed in `wallMount` become
// WallSconces (everything else stands on the floor), and the glyphs resolve
// to the project's default ids. DungeonWorld builds one of these from the
// live Project (FixtureTypesOf); the defaults keep a bare DungeonMap(path)
// working for the classic two kinds.
struct FixtureTypes {
	std::vector<std::string> wallMount{"sconce"};
	std::string sconceDefault = "sconce";  // the 'T' glyph's id
	std::string brazierDefault = "brazier"; // the 'F' glyph's id
};

// A stair/portal on a floor cell that, when the party steps onto it, transitions
// to another level (P6). `type` is a stairs.cat catalog id (the prop model);
// dest* name the arrival level + cell + facing.
struct StairLink {
	int x = 0, z = 0;
	Direction facing = Direction::South;
	std::string type;
	std::string destLevel;
	int destX = 0, destZ = 0;
	Direction destFacing = Direction::South;
};

// Grid-based dungeon. Coordinates: x = column, z = row; world position of a
// cell center is ((x + 0.5) * kCellSize, 0, (z + 0.5) * kCellSize).
class DungeonMap {
public:
	// Loads and validates a .map file; failures are fatal with a clear message.
	// `fixtures` routes fixture records by catalog id (see FixtureTypes).
	explicit DungeonMap(const std::string& path, FixtureTypes fixtures = {});

	int Width() const { return m_width; }
	int Height() const { return m_height; }

	bool IsWalkable(int x, int z) const;
	Cell At(int x, int z) const;

	// --- editing seam (in-game map editor) ----------------------------------
	// Sets a cell's type, bumping Revision() when the value actually changes so
	// the world can rebuild geometry. Out-of-bounds writes are ignored (the
	// grid is fixed-size). Fixtures/turbidity are NOT recomputed here — the
	// editor owns those edits separately; this is the structural layer only.
	void SetCell(int x, int z, Cell cell);
	// Monotonic edit counter; 0 at load. A geometry watcher rebuilds when it
	// changes (see DungeonWorld).
	u32 Revision() const { return m_revision; }

	// --- per-cell surface variant overrides (editor) -------------------------
	// Each floor cell normally picks its wall/floor/ceiling texture variant by a
	// position hash (DungeonMeshBuilder). The editor can pin a cell to a specific
	// palette index; -1 (the default) means "use the hash". Stored on the static
	// layer (a save never needs them — they live in the .map). Setters bump
	// Revision() on change so the geometry rebuilds; out-of-bounds is ignored.
	int WallVariant(int x, int z) const { return VariantAt(m_wallVar, x, z); }
	int FloorVariant(int x, int z) const { return VariantAt(m_floorVar, x, z); }
	int CeilingVariant(int x, int z) const { return VariantAt(m_ceilingVar, x, z); }
	void SetWallVariant(int x, int z, int v) { SetVariant(m_wallVar, x, z, v); }
	void SetFloorVariant(int x, int z, int v) { SetVariant(m_floorVar, x, z, v); }
	void SetCeilingVariant(int x, int z, int v) { SetVariant(m_ceilingVar, x, z, v); }

	// Whether a cell was authored dusty (the 'D' glyph), for the .map writer —
	// distinct from runtime Turbidity(), which also folds in nearby fires.
	bool AuthoredDusty(int x, int z) const {
		if (x < 0 || z < 0 || x >= m_width || z >= m_height) return false;
		return m_dusty[static_cast<size_t>(z) * m_width + x] != 0;
	}

	// Air turbidity 0 (clear) .. 1 (thick dust) for a cell; walls return 0.
	float Turbidity(int x, int z) const {
		if (x < 0 || z < 0 || x >= m_width || z >= m_height) return 0.0f;
		return m_turbidity[static_cast<size_t>(z) * m_width + x];
	}

	// --- per-level atmosphere (the lighting mood knobs) ----------------------
	// Overrides for the level: dust in-scatter density, the dust's ambient
	// pickup, and the ambient-fill scale. < 0 = unset (the world applies its
	// global defaults — gfx::Atmosphere{} and ambient scale 1). Parsed from
	// the .map's `atmosphere` record (`atmosphere dust=0.06 haze=0.8
	// ambient=1.1`, any subset of keys); the editor's Level settings dialog
	// writes them, and the .map writer emits only set values.
	float DustDensity() const { return m_dustDensity; }
	float HazeAmbient() const { return m_hazeAmbient; }
	float AmbientScale() const { return m_ambientScale; }
	void SetAtmosphere(float dust, float haze, float ambient) {
		m_dustDensity = dust;
		m_hazeAmbient = haze;
		m_ambientScale = ambient;
	}

	Vec3 CellCenter(int x, int z, float y = 0.0f) const {
		return {(static_cast<float>(x) + 0.5f) * kCellSize, y,
				(static_cast<float>(z) + 0.5f) * kCellSize};
	}

	// --- live fixture placement (editor) ------------------------------------
	// A sconce auto-mounts on the first FREE solid neighbour wall (fails if the
	// cell has none left); a brazier stands on the floor cell (one per cell).
	// Both add their dust and bump Revision(). Return false on an invalid cell.
	// DungeonWorld rebuilds the fires/dust after; the .map writer reads these
	// lists, so placements persist.
	// `type` is the fixtures.cat id the instance renders as; `lit` seeds the
	// record's light flag (a flameless kind places unlit, so the turbidity
	// grid — which only smokes LIT fixtures — stays truthful).
	bool AddSconce(int x, int z, std::string type = "sconce", bool lit = true);
	// Explicit-face mount: hang it on THIS wall of the cell (the editor's
	// edge-pick names one). False if that neighbour isn't solid or the face is
	// already taken. The overload above auto-picks when no face is named.
	bool AddSconce(int x, int z, std::string type, bool lit, Direction wall);
	bool AddBrazier(int x, int z, std::string type = "brazier", bool lit = true);
	// Re-hang the sconce at (x,z) currently on `from` onto `to` (which must be a
	// solid neighbour wall). Bumps Revision(); false if no such sconce or `to`
	// isn't solid. The caller rebuilds fires/turbidity (DungeonWorld::RemountSconce).
	bool SetSconceWall(int x, int z, Direction from, Direction to);
	// Sets a sconce's per-torch light/smoke properties (identified by cell + wall),
	// then recomputes the turbidity grid. Bumps Revision(); false if not found.
	bool SetSconceProps(int x, int z, Direction wall, bool lit, float brightness,
						float turbidity);
	bool SetBrazierProps(int x, int z, bool lit, float brightness, float turbidity);
	// The brazier standing on (x,z), or null. At most one per cell (AddBrazier
	// rejects duplicates).
	const FloorBrazier* BrazierAt(int x, int z) const;
	// Removes a fixture at (x,z): the brazier first (the centre marker an erase
	// click aims at), else the first sconce there by any wall. Bumps Revision() +
	// recomputes turbidity. Returns false if the cell has no fixture.
	bool RemoveFixtureAt(int x, int z);
	// Removes the sconce on ONE named face. A cell can hold a sconce per wall,
	// so the cell-wide call above picks arbitrarily; this takes the face the
	// editor's edge-pick pointed at.
	bool RemoveSconceAt(int x, int z, Direction wall);
	// After a structural edit at (x,z): a cell painted solid buries the fixtures
	// standing on it (removed), a wall painted open strands the sconces hanging
	// on it (re-mounted on a free solid wall like the 'T' glyph, else removed).
	// Without this the .map writer persists records the loader then asserts on.
	// Recomputes turbidity + bumps Revision(); true if anything changed.
	bool PruneFixturesForCell(int x, int z);
	// Recomputes the whole air-turbidity grid from scratch: the authored dusty base
	// ('D' cells) plus every LIT brazier and every LIT sconce's own smoke. Called at
	// load and whenever a fixture's smoke/lit state changes.
	void RebuildTurbidity();

	int StartX() const { return m_startX; }
	int StartZ() const { return m_startZ; }
	const std::vector<WallSconce>& Sconces() const { return m_torches; }
	const std::vector<FloorBrazier>& Braziers() const { return m_braziers; }

	// Wall niches (recessed pockets). The mesh builder reads NicheAt per edge.
	const std::vector<WallNiche>& Niches() const { return m_niches; }
	// The niche on cell (x,z) whose wall faces (dx,dz), or null — the mesh
	// builder's per-edge query (its `type` resolves to the panel mesh to stamp).
	const WallNiche* NicheAt(int x, int z, int dx, int dz) const;
	// Places a niche on the first free solid wall of (x,z) (the sconce mount
	// rule); false if no free solid wall. Bumps Revision().
	bool AddNiche(int x, int z, std::string type);
	// Carves the niche into THIS wall of the cell (the editor's edge-pick names
	// one). False if that neighbour isn't solid or the face already holds a
	// niche. Bumps Revision().
	bool AddNiche(int x, int z, std::string type, Direction wall);
	// Re-carves the niche at (x,z) from one face onto another (the inspector's
	// Face dropdown), keeping its shape/name/secret state. False if `to` isn't
	// solid, already holds a niche, or no niche sits on `from`. Bumps Revision();
	// the caller moves any items in the pocket (DungeonWorld::RemountNiche).
	bool SetNicheWall(int x, int z, Direction from, Direction to);
	// Adds a niche with an explicit wall (the parser + remote/stash editing).
	// Bumps Revision().
	void AddNicheRecord(WallNiche n) {
		m_niches.push_back(std::move(n));
		++m_revision;
	}
	// Removes the niche on (x,z) facing `wall`. Bumps Revision(); false if none.
	bool RemoveNiche(int x, int z, Direction wall);
	// Removes the first niche carved into solid wall block (wx,wz) — from any
	// adjacent floor cell facing it (the erase tool selects niches by their wall,
	// like the inspector). Bumps Revision(); false if (wx,wz) isn't such a wall.
	bool RemoveNicheFacingWall(int wx, int wz);
	// The walls of every niche on (x,z) — several niches may share a cell (a
	// dead-end has one per solid wall). Drives per-face selection.
	std::vector<Direction> NicheWallsAt(int x, int z) const;
	// Sets the (x,z)/`wall` niche's authored props (inspector Save): name, the
	// hidden start state (resets open = !hidden), and type. Bumps Revision().
	bool SetNichePropsAt(int x, int z, Direction wall, std::string name, bool hidden,
						 std::string type);
	// Sets the (x,z)/`wall` niche's runtime open state (save/load restore of a
	// revealed secret niche). Bumps Revision() if it changed; false if no match.
	bool SetNicheOpenAt(int x, int z, Direction wall, bool open);
	// Resets every niche to its authored open default (open = !hidden) — a new
	// game re-hides any secret niche opened this session. True if any changed.
	bool ResetNicheOpen();
	// Flips `open` on every niche named `name` (a button press). Returns the cells
	// touched so the caller can rebuild their chunks. Bumps Revision().
	std::vector<std::pair<int, int>> ToggleNichesNamed(const std::string& name);
	// Distinct non-empty niche names on the level (the button inspector's targets).
	std::vector<std::string> NicheNames() const;

	// Floor features (recesses sunk into a cell's floor). The mesh builder reads
	// FloorFeatureAt per cell and stamps the tile in place of the floor block.
	const std::vector<FloorFeature>& FloorFeatures() const { return m_floorFeatures; }
	// The feature on walkable cell (x,z), or null — the builder's per-cell query.
	const FloorFeature* FloorFeatureAt(int x, int z) const;
	// Sinks a feature into (x,z). False if the cell isn't walkable or already has
	// one. Bumps Revision().
	bool AddFloorFeature(int x, int z, std::string type);
	// Adds one unchecked (the parser + remote/stash editing). Bumps Revision().
	void AddFloorFeatureRecord(FloorFeature f) {
		m_floorFeatures.push_back(std::move(f));
		++m_revision;
	}
	// Removes the feature on (x,z). Bumps Revision(); false if none.
	bool RemoveFloorFeature(int x, int z);

	// Wall windows (see-through bores through a solid block). The mesh builder
	// reads WallBoredAlong per stamped face; LoS/projectiles read it per cell.
	const std::vector<WallBore>& Bores() const { return m_bores; }
	// The bore on solid cell (x,z) with the given `axis` (0 = X, 1 = Z), or null —
	// the mesh builder resolves its `type` to the bore mesh to stamp.
	const WallBore* BoreAlong(int x, int z, int axis) const;
	// True if solid cell (x,z) is bored along `axis` (LoS/projectiles).
	bool WallBoredAlong(int x, int z, int axis) const { return BoreAlong(x, z, axis); }
	// Bores solid cell (x,z) with `type` along whichever axis has floor on both
	// sides (a 1-block wall between two spaces). False if it isn't such a wall /
	// already bored. Bumps Revision().
	bool AddBore(std::string type, int x, int z);
	// Bores along an EXPLICIT axis (0 = X/east-west, 1 = Z/north-south) — the
	// editor derives it from the face pointed at, so a block with floor on all
	// four sides can be bored either way instead of always taking X. False if
	// that axis doesn't open into floor at both ends.
	bool AddBore(std::string type, int x, int z, int axis);
	// Adds a bore with an explicit axis (the parser). Bumps Revision().
	void AddBoreRecord(WallBore b) {
		m_bores.push_back(std::move(b));
		++m_revision;
	}
	// Removes the first bore on (x,z). Bumps Revision(); false if none.
	bool RemoveBoreAt(int x, int z);

	// Static decoration records (banners, rubble, ...) from the .map file.
	const std::vector<Entity>& Decorations() const { return m_decorations; }
	// Replaces the decoration records wholesale. The static-map stash syncs the
	// LIVE editor placements back into records before stashing the map, so
	// LoadDecorations rebuilds them on return (DungeonWorld::StashStaticMap).
	void SetDecorationRecords(std::vector<Entity> records) {
		m_decorations = std::move(records);
	}
	// Record-level decoration edits, for REMOTE-level editing (a non-active
	// level has no live instances — its stash's records are the truth).
	void AddDecorationRecord(Entity record) {
		m_decorations.push_back(std::move(record));
	}
	// Removes the first decoration record on (x,z) (the erase tool: one per
	// click, like the live path). False if the cell has none.
	bool RemoveDecorationRecordAt(int x, int z);
	// Removes every decoration record on (x,z) (a cell painted solid buries
	// them all). Returns the number removed.
	size_t RemoveDecorationRecordsAt(int x, int z);

	// Stair/portal links from the .map "stairs" records (P6 multi-level).
	const std::vector<StairLink>& Stairs() const { return m_stairs; }

	// --- live stair placement (editor) ---------------------------------------
	// One stair per cell (like braziers: every per-instance surface addresses a
	// stair by its cell). The .map writer reads m_stairs, so placements persist.
	// The paired return stair on the DESTINATION level is DungeonWorld's job
	// (that level isn't loaded here). Returns false on a solid/OOB/occupied cell.
	bool AddStair(const StairLink& link);
	// The stair link on (x,z), or null.
	const StairLink* StairAt(int x, int z) const;
	// Removes the stair link at (x,z), copying it into `removed` first (so the
	// caller can clean up its paired return stair). False if the cell has none.
	bool RemoveStair(int x, int z, StairLink* removed = nullptr);
	// Repoints every stair whose dest names `oldStem` (a level rename — the
	// dest strings are the cross-level references that would go stale).
	// Returns the number touched.
	size_t RenameStairDest(const std::string& oldStem, const std::string& newStem);

	// Surface palettes from the level's "palette" records — lists of CATALOG
	// IDs (project catalog/walls.cat, floors.cat, ceilings.cat). Order defines
	// the variant index everywhere (texture arrays, worn block meshes, geometry
	// buckets); DungeonWorld resolves each id to its texture set + height scale.
	const std::vector<std::string>& WallPalette() const { return m_wallPalette; }
	const std::vector<std::string>& FloorPalette() const { return m_floorPalette; }
	const std::vector<std::string>& CeilingPalette() const { return m_ceilingPalette; }
	// Editor: APPEND a catalog id to a palette (false = already there). Append
	// only — the `variant` records store the palette INDEX, so inserting or
	// removing in the middle would silently repaint every cell above it. The
	// .map writer emits the palettes from here, so a placement persists.
	// The caller reloads that surface's textures/worn meshes (DungeonWorld::
	// AddPaletteEntry); no revision bump, since no cell changed.
	bool AddToWallPalette(std::string id) {
		return AddPaletteId(m_wallPalette, std::move(id));
	}
	bool AddToFloorPalette(std::string id) {
		return AddPaletteId(m_floorPalette, std::move(id));
	}
	bool AddToCeilingPalette(std::string id) {
		return AddPaletteId(m_ceilingPalette, std::move(id));
	}

	// Which family of records a type sweep walks (see SweepTypeRefs). One per
	// catalog category that a .map record can name.
	enum class TypeRecords {
		WallPalette, FloorPalette, CeilingPalette,
		Decoration, Fixture, WallFeature, Stair
	};
	// Editor type rename/delete: counts this level's references to catalog id
	// `id` within one record family and, when `newId` is given, rewrites them.
	// A palette entry is rewritten IN PLACE — the position is the variant index,
	// so reordering would repaint every cell that pinned it.
	int SweepTypeRefs(TypeRecords records, std::string_view id,
					  const std::string* newId);

private:
	void ParsePaletteRecord(const std::string& record, const std::string& path);
	void ParseStairRecord(const std::string& record, const std::string& path);
	void ParseVariantRecord(const std::string& record, const std::string& path);
	void AddFireTurbidity(int x, int z, float amount);
	// First solid neighbour wall of (x,z) with no sconce on it yet (the 'T'-glyph
	// mount rule, skipping occupied walls). False if the cell has no free wall.
	bool FreeSconceWall(int x, int z, Direction& out) const;
	// First solid neighbour wall of (x,z) with no niche on it yet.
	bool FreeNicheWall(int x, int z, Direction& out) const;

	// Shared body of the palette appenders (one list per surface).
	static bool AddPaletteId(std::vector<std::string>& list, std::string id) {
		if (id.empty() || std::find(list.begin(), list.end(), id) != list.end())
			return false;
		list.push_back(std::move(id));
		return true;
	}

	// Shared body of the variant getters/setters (one grid per surface).
	int VariantAt(const std::vector<int>& grid, int x, int z) const {
		if (x < 0 || z < 0 || x >= m_width || z >= m_height) return -1;
		return grid[static_cast<size_t>(z) * m_width + x];
	}
	void SetVariant(std::vector<int>& grid, int x, int z, int v) {
		if (x < 0 || z < 0 || x >= m_width || z >= m_height) return;
		int& slot = grid[static_cast<size_t>(z) * m_width + x];
		if (slot == v) return;
		slot = v;
		++m_revision;
	}

	int m_width = 0;
	int m_height = 0;
	int m_startX = 1;
	int m_startZ = 1;
	u32 m_revision = 0; // bumped by SetCell; 0 at load

	std::vector<Cell> m_cells;
	std::vector<float> m_turbidity; // parallel to m_cells
	std::vector<u8> m_dusty;        // authored 'D' cells (for the writer)
	// Per-level atmosphere overrides (< 0 = unset; see SetAtmosphere).
	float m_dustDensity = -1.0f, m_hazeAmbient = -1.0f, m_ambientScale = -1.0f;
	// Per-cell variant overrides, parallel to m_cells; -1 = use the hash default.
	std::vector<int> m_wallVar, m_floorVar, m_ceilingVar;
	std::vector<WallSconce> m_torches;
	std::vector<FloorBrazier> m_braziers;
	std::vector<WallNiche> m_niches;
	std::vector<WallBore> m_bores;
	std::vector<FloorFeature> m_floorFeatures;
	std::vector<Entity> m_decorations;
	std::vector<StairLink> m_stairs;
	std::vector<std::string> m_wallPalette;   // catalog ids (walls.cat)
	std::vector<std::string> m_floorPalette;  // catalog ids (floors.cat)
	std::vector<std::string> m_ceilingPalette; // catalog ids (ceilings.cat)
};

} // namespace dungeon::game
