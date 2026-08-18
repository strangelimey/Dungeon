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
// manifest's first level); its start cell seeds the flood.
std::vector<Issue> Run(const std::vector<LevelView>& levels,
					   const std::string& startLevel, const Rules& rules);

} // namespace dungeon::game::validate
