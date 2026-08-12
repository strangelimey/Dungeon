// ============================================================================
// Game/Blast.h — how an area effect fills the dungeon.
//
// Michael's model (2026-08-11), and it is not a radius: A BLAST HAS A FORCE,
// measured in SQUARES, and stone consumes none of it. The blast floods outward
// from where it went off, 4-cardinally — the grid's own rule, which LoS, movement
// and projectiles all hold to — spending one of its force per square it fills,
// and it keeps going until the force is used up. So the SAME blast fills a room
// nine squares wide and runs eight squares down a dead-end corridor: the force
// that would have gone into the walls goes down the corridor instead.
//
// Then the second half of the rule, for the case where even expanding cannot
// spend it: force with nowhere left to go CONCENTRATES on what the blast did
// reach. Sealed into a single cell, a nine-square blast puts all nine squares'
// worth into that one square. Which is what a confined explosion does.
//
// Damage falls off with distance from the centre, so the far end of that corridor
// is a scorch and the near end is lethal.
//
// PURE on purpose, like Game/Defense.h: no map, no catalogs, no combatants — the
// caller says which cells a blast may enter and this says where it lands and how
// hard. That is what lets tools/RollTest measure the spread rather than a copy of
// it, and this algorithm is far too geometric to trust unmeasured.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <array>
#include <functional>

namespace dungeon::game::blast {

// The most squares one blast may fill. A CEILING, not a balance knob: the spread
// is bounded so it can run on a fixed array and allocate nothing on a detonation
// (docs/ARCHITECTURE.md "Memory strategy"), and 64 squares is already an absurd
// blast in a 28x24 dungeon. Force past this is clamped, and Result::clamped says
// so rather than letting it pass silently.
inline constexpr int kMaxCells = 64;

// One square the blast reached.
struct Cell {
	int x = 0, z = 0;
	int distance = 0;    // 4-cardinal steps from the centre, THROUGH open squares
	float damage = 0.0f; // what lands here, falloff and concentration applied
};

// Where a blast landed and how hard.
struct Result {
	std::array<Cell, kMaxCells> cells{};
	int count = 0;
	// Force that had nowhere to go — non-zero only when the blast ran out of
	// reachable squares before it ran out of force, i.e. it was sealed in.
	int leftover = 0;
	// What that leftover became: the multiplier applied to every square's damage.
	// 1.0 whenever the blast had room to expand, which is the common case.
	float concentration = 1.0f;
	bool clamped = false; // force exceeded kMaxCells
};

// May the blast enter this square? The caller's business — a wall, off-map, and a
// closed door all say no. (The centre is asked too: a bolt that burst against
// stone has its centre INSIDE the wall, and nothing can be standing there.)
using PassableFn = std::function<bool(int x, int z)>;

// Flood `force` squares' worth outward from (cx, cz).
//
// `full` is the damage at the centre and `falloff` what each step of distance
// takes off it, both in the same units as any other damage. A square whose
// falloff has eaten the whole figure still counts as filled and still spends its
// force — the blast reached it, it just did nothing there.
//
// Cells come back in BFS order, so cells[0] is the centre whenever the centre is
// passable, and distance is non-decreasing.
Result Spread(int cx, int cz, int force, float full, float falloff,
			  const PassableFn& passable);

} // namespace dungeon::game::blast
