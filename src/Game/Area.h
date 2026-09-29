// ============================================================================
// Game/Area.h - "the connected room or corridor" as a set of cells, for the
// editor's area fill (docs/editor-updates-plan.md, P1).
//
// A walkable cell is OPEN when it sits in any 2x2 block of walkable cells, and
// NARROW otherwise. An area is the 4-connected run of walkable cells of the
// SAME class as the one clicked, so:
//   - a room fills out to its doorways and stops;
//   - a 1-wide corridor fills as one run, bends and T-junctions included, and
//     stops where it opens into a room;
//   - a doorway cell is narrow, so it is the boundary between two rooms.
//
// Why not DungeonMap::DoorwayFacing, the existing "narrow" test: it asks for
// solid walls flanking exactly ONE axis, which a corridor's bend does not have
// (its walls are on two adjacent sides), so it would break every corridor at
// its first corner. The 2x2 rule has no notion of direction at all.
//
// Consequence, accepted (Michael, 2026-09-29): a corridor two cells wide
// contains 2x2 blocks, so it counts as ROOM and fills with the rooms it joins.
//
// Pure: a DungeonMap in, cells out. No world, no catalogs, no editor.
// ============================================================================
#pragma once

#include "Game/DungeonMap.h"

#include <span>
#include <utility>
#include <vector>

namespace dungeon::game::area {

using CellXZ = std::pair<int, int>;

// True for a walkable cell that sits in some 2x2 block of walkable cells.
bool IsOpen(const DungeonMap& map, int x, int z);

// The room or corridor holding walkable cell (x,z): every walkable cell
// 4-connected to it through cells of its own class, the start included.
// Empty when (x,z) is out of bounds or solid.
std::vector<CellXZ> Region(const DungeonMap& map, int x, int z);

// The solid cells 4-adjacent to `region`, each once: the wall blocks whose
// faces the region sees. (A wall block wears ONE texture on all four faces, so
// painting these also retextures whatever is on their far side - accepted for
// this branch, see the plan's decisions.)
std::vector<CellXZ> Walls(const DungeonMap& map, std::span<const CellXZ> region);

} // namespace dungeon::game::area
