#pragma once
// ============================================================================
// Game/Arrival.h - WHICH LEVEL a way in lands on (code-review C136).
//
// Three ways into a level name it from outside: a stair (its `dest=`, a stem
// outright), the game's OPENING (project.ini's start_level) and a world-map
// DOORWAY (a location's dungeon and `level`). The last two RESOLVE - an unset
// opening level is the project's first, a doorway naming a level its dungeon
// does not list opens the dungeon's first - and that resolution used to be
// written out at each place that asked: starting a new game, entering a
// doorway, a reroll keeping the squares the ways in land on (Game::ArrivalsOn)
// and a stair move or resize carrying them along (DungeonWorld::RemapArrivals).
// The reroll's copy had drifted from the others - it ignored the opening when
// start_level was empty and lowercased the dungeon's level list - so a reroll
// could fill the square a party arrives on with rock. This is the one copy.
//
// PURE (the standard library only), so RollTest checks it as it ships.
// Project::OpeningLevel / DoorwayLevel are the adapters every caller uses.
// ============================================================================

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

namespace dungeon::game::arrival {

// The level the game's OPENING lands on: the manifest's `start_level`, else the
// project's first level, else "level1" (a project with no level list at all).
inline std::string OpeningLevel(std::string_view startLevel,
								std::span<const std::string> projectLevels) {
	if (!startLevel.empty()) return std::string(startLevel);
	return projectLevels.empty() ? std::string("level1") : projectLevels.front();
}

// The level a world-map DOORWAY opens onto, as entering it does: its `level`
// when its dungeon lists that level, else the dungeon's first. "" when the
// dungeon lists none (entering refuses; nothing lands anywhere). `dungeonLevels`
// is the dungeon's list AS WRITTEN (SplitIds), case and all.
inline std::string DoorwayLevel(std::string_view level,
								std::span<const std::string> dungeonLevels) {
	if (dungeonLevels.empty()) return {};
	const bool listed = std::find(dungeonLevels.begin(), dungeonLevels.end(), level) !=
						dungeonLevels.end();
	return listed ? std::string(level) : dungeonLevels.front();
}

} // namespace dungeon::game::arrival
