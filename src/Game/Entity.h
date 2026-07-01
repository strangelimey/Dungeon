// ============================================================================
// Game/Entity.h — the entity record shared by both level data layers.
//
// Levels store non-structural content as one-line text records:
//
//   <kind> <type> <x> <z> [facing] [key=value ...]
//   e.g.  monster skeleton 6 1
//         button lever 9 5 east target=door_a
//
// Static records (decorations) live in the .map file below the grid; dynamic
// records (monsters, items, buttons) live in the .ent file — see DungeonMap.h
// and DungeonEntities.h for the split. One Entity struct carries both: a
// kind, a free-form type name (resolved to assets/behavior by the Game
// layer), a grid cell, an optional facing (defaults to south), and
// open-ended key=value params for wiring (button targets etc.).
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dungeon::game {

// Grid-aligned facing. +X = east, +Z = south (row index grows southward, see
// DungeonMap.h), so north is -Z.
enum class Direction : u8 { North, East, South, West };

int DirDX(Direction d);
int DirDZ(Direction d);
// Parses "north"/"east"/"south"/"west" into `out`; false on anything else.
bool ParseDirection(std::string_view token, Direction& out);
// Yaw under the camera convention forward = (sin yaw, 0, cos yaw):
// south = 0, east = +pi/2, north = pi, west = -pi/2.
float DirYaw(Direction d);
// The loc key naming a Direction ("facing.north" ...), for UI dropdowns/labels.
const char* FacingLocKey(Direction d);

enum class EntityKind : u8 { Monster, Item, Button, Decoration };

struct Entity {
	EntityKind kind = EntityKind::Item;
	std::string type;                  // e.g. "skeleton", "lever", "banner"
	int x = 0, z = 0;                  // grid cell
	Direction facing = Direction::South;
	std::vector<std::pair<std::string, std::string>> params; // key=value extras

	// Stable spawn id, assigned in .ent file order by DungeonEntities (which
	// then sorts by cell, scrambling file order). The save system keys per-
	// entity overrides off this so a save survives the sort. -1 = not a
	// dynamic-layer entity (decorations from the .map keep the default).
	int id = -1;

	// Returns the value for `key`, or nullptr when the record doesn't have it.
	const std::string* Param(std::string_view key) const;
};

// Parses one record line (no comment/blank lines). `where` names the source
// file for error messages. Unknown kinds, bad coordinates, or stray tokens
// fail hard.
Entity ParseEntityRecord(std::string_view line, std::string_view where);

// Splits a level text file into data lines: strips '\r', drops blank lines
// and ';' comment lines. Shared by the .map and .ent loaders.
std::vector<std::string> ReadLevelLines(const std::vector<u8>& bytes);

// Splits one record line into whitespace-separated tokens (views into `line`).
std::vector<std::string_view> SplitRecordTokens(std::string_view line);

} // namespace dungeon::game
