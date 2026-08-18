// ============================================================================
// Game/Generate.h — rough out a level from a handful of knobs.
//
// The point is NOT to produce a finished dungeon. It is to skip the tedious
// half of starting one: carving a plausible shape, so the work becomes tweaking
// knobs and hitting regenerate until something is roughly right, then refining
// it by hand with the ordinary brushes.
//
// WHAT IT EMITS IS ORDINARY CONTENT. A generated level is a normal grid plus
// normal .ent records — no marker, no "generated" flag, no second code path.
// That is what makes every tool already built work on the output on day one
// (brushes, inspectors, undo, the checker, level browsing, save), and what makes
// a generated level diff in git like a hand-built one.
//
// THE LOCK/KEY ORDERING IS BUILT BY CONSTRUCTION, NEVER BY RETRY. Each key is
// placed in the region that is already reachable when its door goes down, so
// Game/Validate.h's fixpoint passes by design. A generate-check-reject loop
// would be slow and, worse, could fail to converge on exactly the tight
// parameters that most need it — so the checker stays a CHECK and never becomes
// a filter this leans on.
//
// PURE and DETERMINISTIC: same params (seed included) => same level, every time.
// That is what makes "regenerate" a knob rather than a dice roll you cannot get
// back, and it is why the seed is a parameter rather than a hidden clock read.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Game/Entity.h"

#include <string>
#include <vector>

namespace dungeon::game::generate {

// The knobs. Everything the shape depends on lives here, so a level is exactly
// reproducible from this struct — see the determinism note above.
struct Params {
	int width = 32, height = 32;
	// Roughly how many rooms to aim for. The carver may fall short on a small
	// map: rooms are placed by rejection, and it stops trying rather than
	// shrinking them into cupboards.
	int rooms = 8;
	// 0 = the rooms chain nearly straight through; 1 = the tree branches hard,
	// so most rooms hang off side passages rather than sitting on the way.
	float branching = 0.5f;
	// How many locked doors to author. Each takes a key, placed where it is
	// reachable before its own door (see the header note).
	int locks = 1;
	float difficulty = 0.5f; // monster density
	float reward = 0.5f;     // loot density
	u32 seed = 1;
	// Tag-matched content pools, resolved by the CALLER (which owns the
	// catalogs) and handed in as plain id lists. The generator picks from these
	// and never looks a catalog up, which is what keeps it pure.
	std::vector<std::string> monsterIds;
	std::vector<std::string> lootIds;
	std::vector<std::string> keyIds; // door/key pairs draw from these, in order
};

// A generated level, in the two layers the project already speaks: a grid, and
// a list of entity records.
struct Level {
	int width = 0, height = 0;
	// One byte per cell, row-major: 1 = floor, 0 = rock.
	std::vector<u8> floor;
	int startX = 0, startZ = 0;
	// The far end of the dungeon — where the caller puts the stair onward. Not
	// authored here because a stair names a DESTINATION LEVEL, which is a fact
	// about the project rather than about this level.
	int exitX = 0, exitZ = 0;
	std::vector<Entity> entities; // monsters, items, doors

	bool At(int x, int z) const {
		if (x < 0 || z < 0 || x >= width || z >= height) return false;
		return floor[static_cast<size_t>(z) * width + x] != 0;
	}
};

// Rough out a level. Never fails: a hostile set of parameters yields a small
// dungeon rather than an empty one, because an editor tool that sometimes
// produces nothing teaches you to distrust the button.
Level Run(const Params& params);

} // namespace dungeon::game::generate
