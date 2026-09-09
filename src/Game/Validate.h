// ============================================================================
// Game/Validate.h — is this dungeon actually playable?
//
// The editor can build a level that looks finished and cannot be finished: a
// stair whose far side nothing reaches, a key behind the door it opens. Those
// are exactly the faults a playthrough finds and a glance does not, which is
// why they are worth a check rather than care.
//
// THE THREE REQUESTED CHECKS ARE REALLY ONE. "Every locked door has its key
// before it" is a REACHABILITY FIXPOINT: flood from the start with locked doors
// treated as walls, collect the keys in what you reached, open what those keys
// open, and repeat until nothing new opens. Entrance/exit existence and stair
// pairing fall out of it — an unpaired stair is simply a region nothing reaches.
// So the fixpoint is the engine and the rest are its by-products.
//
// Stair pairing still gets its OWN check, for a different reason: the pair is
// auto-authored on placement, so a broken one means DRIFT — a hand-edited
// record, a rename, a cross-level delete — and saying that is worth more to
// whoever has to fix it than "unreachable".
//
// IT SPANS THE WHOLE PROJECT, not one level. A key may legitimately live a floor
// away from its door, so a per-level check would report false failures for
// correct content, which is worse than no check: a report that cries wolf gets
// turned off.
//
// PURE, and deliberately so — it takes a snapshot of levels and answers
// questions about it, touching no world, no renderer and no catalogs. It is
// therefore testable without a game, and cannot itself perturb what it measures.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Game/DungeonEntities.h"
#include "Game/DungeonMap.h"
#include "Game/WorldMap.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace dungeon::game::validate {

// One level's two layers, as the checker needs to see them. Non-owning: the
// caller keeps the maps and record lists alive for the call.
struct LevelView {
	std::string stem;
	const DungeonMap* map = nullptr;
	const DungeonEntities* ents = nullptr;
};

// What the checker needs to know that lives in the catalogs, passed in rather
// than looked up so the module stays free of Project.
struct Rules {
	// Item ids that count as keys (items.cat `category = key`). A door's `key=`
	// names one of these.
	std::unordered_set<std::string> keyItems;
	// Stair type ids that DO transition when stepped on (stairs.cat
	// `traverse` != 0). A pit's ceiling half is scenery and must not be read as
	// a way up, or the reachability answer is nonsense.
	std::unordered_set<std::string> traversableStairs;
	// Stair types that LEAVE the dungeon for the world map (stairs.cat
	// `exit`). They author no destination, so the dest checks must not read
	// their empty one as a broken link — and for reachability they are a dead
	// end, not a way on: the world is not a level the flood can reach.
	std::unordered_set<std::string> exitStairs;
};

// One dungeon, as the checker needs to see it: the level stems it claims and
// the one a party arriving from the world map lands on. Resolved from
// dungeons.cat by the caller, like Rules above.
// A dungeon is a NAMED GROUP OF LEVELS and nothing more — it has no start of
// its own, because every way in (a world-map location, or the game's own
// opening) carries its own destination.
struct DungeonView {
	std::string id;
	std::vector<std::string> levels;
};

// The WORLD tier, when the project has one (docs/world-map.md). Non-owning, and
// OPTIONAL: `map` stays null for a project with no world, and every world check
// is then skipped rather than reporting a project-wide fault — a game that is
// all dungeon and no overworld is a legitimate shape, not an unfinished one.
struct WorldView {
	const WorldMap* map = nullptr;
	std::vector<DungeonView> dungeons;
};

enum class Severity : u8 { Error, Warning };

// One finding. `level` + `x`/`z` locate it so the report can jump there;
// x < 0 means the finding is about the level or project as a whole.
struct Issue {
	Severity severity = Severity::Error;
	std::string level;
	int x = -1, z = -1;
	std::string messageKey; // loc key, formatted with the args below
	std::string a, b;
};

// Runs every check over the project. `startLevel` is where play begins (the
// manifest's first level); its start cell seeds the flood. `world` adds the
// world-tier checks when the project has an overworld.
//
// A WORLD finding carries NO level and NO cell (level "", x -1) even when it is
// about a location standing on a known world square. The report's coordinates
// are what the editor JUMPS to, and they are read as a LEVEL cell — sending it
// to 10,10 of whatever level happens to be open would be worse than sending it
// nowhere. The world coordinates go in the message text instead.
std::vector<Issue> Run(const std::vector<LevelView>& levels,
					   const std::string& startLevel, const Rules& rules,
					   const WorldView& world = {});

} // namespace dungeon::game::validate
