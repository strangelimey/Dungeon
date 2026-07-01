// ============================================================================
// Game/DungeonMap.h — the STATIC layer of a level: everything that never
// changes during play, so a save system never needs to store it.
//
// Levels are ASCII grid text files under assets/maps/ (see level1.map for
// the glyph legend: '#' rock, '.' floor, 'D' dust, 'T' sconce, 'F' brazier,
// 'P' start; ';' lines are comments). Lines starting with a lowercase letter
// are records — grid glyphs are never lowercase, so the two can't collide:
//   palette <wall|floor|ceiling> <id> [...]     surface palette (catalog ids)
//   decoration <type> <x> <z> [facing]          static entity (Entity.h)
//   fixture <sconce|brazier> <x> <z> [facing]   wall sconce / floor brazier
// The 'T'/'F' glyphs are terse shorthand for a single auto-faced sconce/
// brazier; the fixture record places them explicitly, so several can share a
// cell (e.g. two sconces on different walls) — records always allow that.
// Every level declares its texture palette; the game loads only those sets
// (and their worn block meshes), nothing else. Dynamic content (monsters,
// items, buttons) lives in the .ent file next to the .map — see
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

#include <string>
#include <vector>

namespace dungeon::game {

// World-space dimensions shared by geometry, lighting, and the camera.
inline constexpr float kCellSize = 2.4f;   // square cells, 2.4m on a side
// Block meshes are authored at this exact size (AssetBaker's kCellHalf in
// tools/AssetBaker/ModelBaker.cpp must equal kCellSize/2) — changing it
// means rebaking models: AssetBaker models <assets>.
inline constexpr float kWallHeight = 2.5f; // floor to ceiling
inline constexpr float kEyeHeight = 1.55f; // camera height above the floor

enum class Cell : u8 { Wall, Floor };

// A wall-mounted torch sconce: its cell plus the wall it hangs on (the
// direction from the cell to the solid neighbour it mounts against). Several
// sconces may share a cell on different walls.
struct WallSconce {
	int x = 0, z = 0;
	Direction wall = Direction::North;
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
	explicit DungeonMap(const std::string& path);

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

	Vec3 CellCenter(int x, int z, float y = 0.0f) const {
		return {(static_cast<float>(x) + 0.5f) * kCellSize, y,
				(static_cast<float>(z) + 0.5f) * kCellSize};
	}

	// --- live fixture placement (editor) ------------------------------------
	// A sconce auto-mounts on the first solid neighbour wall (fails if the cell
	// has none); a brazier stands on the floor cell. Both add their dust and bump
	// Revision(). Return false on an invalid cell. DungeonWorld rebuilds the
	// fires/dust after; the .map writer reads these lists, so placements persist.
	bool AddSconce(int x, int z);
	bool AddBrazier(int x, int z);
	// Re-hang the sconce at (x,z) currently on `from` onto `to` (which must be a
	// solid neighbour wall). Bumps Revision(); false if no such sconce or `to`
	// isn't solid. The caller rebuilds fires/turbidity (DungeonWorld::RemountSconce).
	bool SetSconceWall(int x, int z, Direction from, Direction to);

	int StartX() const { return m_startX; }
	int StartZ() const { return m_startZ; }
	const std::vector<WallSconce>& Sconces() const { return m_torches; }
	const std::vector<std::pair<int, int>>& BrazierCells() const { return m_braziers; }

	// Static decoration records (banners, rubble, ...) from the .map file.
	const std::vector<Entity>& Decorations() const { return m_decorations; }

	// Stair/portal links from the .map "stairs" records (P6 multi-level).
	const std::vector<StairLink>& Stairs() const { return m_stairs; }

	// Surface palettes from the level's "palette" records — lists of CATALOG
	// IDs (project catalog/walls.cat, floors.cat, ceilings.cat). Order defines
	// the variant index everywhere (texture arrays, worn block meshes, geometry
	// buckets); DungeonWorld resolves each id to its texture set + height scale.
	const std::vector<std::string>& WallPalette() const { return m_wallPalette; }
	const std::vector<std::string>& FloorPalette() const { return m_floorPalette; }
	const std::vector<std::string>& CeilingPalette() const { return m_ceilingPalette; }

private:
	void ParsePaletteRecord(const std::string& record, const std::string& path);
	void ParseStairRecord(const std::string& record, const std::string& path);
	void ParseVariantRecord(const std::string& record, const std::string& path);
	void AddFireTurbidity(int x, int z, float amount);

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
	// Per-cell variant overrides, parallel to m_cells; -1 = use the hash default.
	std::vector<int> m_wallVar, m_floorVar, m_ceilingVar;
	std::vector<WallSconce> m_torches;
	std::vector<std::pair<int, int>> m_braziers;
	std::vector<Entity> m_decorations;
	std::vector<StairLink> m_stairs;
	std::vector<std::string> m_wallPalette;   // catalog ids (walls.cat)
	std::vector<std::string> m_floorPalette;  // catalog ids (floors.cat)
	std::vector<std::string> m_ceilingPalette; // catalog ids (ceilings.cat)
};

} // namespace dungeon::game
