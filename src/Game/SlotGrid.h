// ============================================================================
// Game/SlotGrid.h — monster size classes and sub-cell slot geometry.
//
// A monster has a SIZE CLASS that decides how much of a cell it occupies and
// how many fit. The 2.4 m cell is subdivided into a square sub-grid; a monster
// stands at the centre of one slot rather than the cell centre, so several
// like-sized monsters share a cell (a "group" — see docs/movement.md):
//
//   Huge   2x2 CELLS, one occupant   (1-wide corridors exclude it)
//   Large  1 cell,  1x1 slot grid    (slot 0 == CellCenter; today's default)
//   Medium 1 cell,  2x2 = 4 slots    (human-sized; one per quarter)
//   Small  1 cell,  3x3 = 9 slots
//   Tiny   1 cell,  4x4 = 16 slots
//
// Slot index is a plain WORLD-SPACE grid position (slot = row*dim + col, row
// along +Z/south, col along +X/east); RANK (front/flank/rear relative to the
// party) is derived elsewhere, not baked into the index. These helpers are pure
// and header-only so both the world (occupancy/placement) and tools can use
// them; the async AI layer stays decoupled and only sees primitives.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/DungeonMap.h" // kCellSize

#include <string>

namespace dungeon::game {

enum class SizeClass { Huge, Large, Medium, Small, Tiny };

// Edge length, in CELLS, of a size's footprint. Only Huge spans more than one
// cell (2x2); every other size lives within a single cell.
inline constexpr int FootprintCells(SizeClass s) {
	return s == SizeClass::Huge ? 2 : 1;
}

// Edge length, in SLOTS, of the size's sub-grid within a single cell. Huge fills
// its 2x2-cell block as one occupant, so its in-cell sub-grid is 1.
inline constexpr int SlotDim(SizeClass s) {
	switch (s) {
		case SizeClass::Medium: return 2;
		case SizeClass::Small:  return 3;
		case SizeClass::Tiny:   return 4;
		case SizeClass::Huge:   // one occupant over a 2x2-cell block
		case SizeClass::Large:  // one occupant centred in the cell
		default:                return 1;
	}
}

// How many monsters of this size fit in one cell (its anchor cell, for Huge).
inline constexpr int SlotsPerCell(SizeClass s) {
	const int d = SlotDim(s);
	return d * d;
}

// World-space centre of a slot. (cellX,cellZ) is the cell (the anchor/NW cell
// for Huge); slot is row*dim+col on the size's sub-grid. For Huge the occupant
// sits at the centre of the 2x2 block; for Large slot 0 this equals CellCenter.
inline Vec3 SlotCenter(int cellX, int cellZ, SizeClass s, int slot, float y = 0.0f) {
	if (s == SizeClass::Huge) {
		// Centre of the 2x2 block anchored at (cellX,cellZ): the shared corner
		// between the anchor cell and its +X/+Z neighbours.
		return {(static_cast<float>(cellX) + 1.0f) * kCellSize, y,
				(static_cast<float>(cellZ) + 1.0f) * kCellSize};
	}
	const int dim = SlotDim(s);
	const int col = slot % dim;
	const int row = slot / dim;
	const float cx = (static_cast<float>(cellX) + 0.5f) * kCellSize;
	const float cz = (static_cast<float>(cellZ) + 0.5f) * kCellSize;
	const float off = 1.0f / static_cast<float>(dim);
	return {cx + ((static_cast<float>(col) + 0.5f) * off - 0.5f) * kCellSize, y,
			cz + ((static_cast<float>(row) + 0.5f) * off - 0.5f) * kCellSize};
}

// True for the sub-cell sizes that sit in a corner quarter (Medium/Small/Tiny) —
// the ones that gain from re-centring when alone. Large fills the whole cell and
// Huge its 2x2 block, so they are already centred and never slide (item 8).
inline constexpr bool IsSubCellSize(SizeClass s) {
	return s == SizeClass::Medium || s == SizeClass::Small || s == SizeClass::Tiny;
}

// Parse a monsters.cat `size=` value; unknown/empty -> Large (today's default).
inline SizeClass ParseSizeClass(const std::string& v) {
	if (v == "huge")   return SizeClass::Huge;
	if (v == "medium") return SizeClass::Medium;
	if (v == "small")  return SizeClass::Small;
	if (v == "tiny")   return SizeClass::Tiny;
	return SizeClass::Large;
}

} // namespace dungeon::game
