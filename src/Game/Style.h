// ============================================================================
// Game/Style.h - what a STYLE is (docs/tool-refinement-plan.md Phase 5).
//
// A style is one named decision about how a stretch of dungeon looks, is shaped
// and what lives there - "small crypt", "dirt tunnels", "guard barracks" - so a
// builder makes it once instead of again on every level (Michael's "reduce
// repetition"). A styles.cat entry:
//
//   [dirt_tunnels]
//   display = Dirt Tunnels
//   room = dirt_cave            ; themes.cat id: the look of a room
//   corridor = dirt_tunnel      ; themes.cat id: the look of a passage
//   knobs = width:40 ...        ; the generator's settings line (genpresets.cat)
//   corridor_width = 1          ; squares; 2 for a grand hall
//   tags = cave vermin          ; what content fits
//   monsters = giant_spider 2, centipede 2, blob 1
//
// The monster list is `<id> [weight]` entries split by commas; an absent
// weight is 1. A monster's POWER is not in it - that is the monster's own
// (DungeonWorld::MonsterPower), so a list is never stale against a Balance pass.
//
// PURE: strings in, strings out, no catalog and no world, so it can be tested
// without a game - the Power.h bargain.
// ============================================================================
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game::style {

// One entry of a style's monster list.
struct Pick {
	std::string id;
	float weight = 1.0f;
};

// "giant_spider 2, centipede, blob 0.5" -> its picks, in order. Malformed
// weights read as 1; a weight of 0 or less drops the entry (it could never be
// chosen); a repeated id keeps its first entry.
std::vector<Pick> ParseMonsters(std::string_view text);
// The other way: "giant_spider 2, centipede, blob 0.5" (a weight of 1 is left
// unwritten, so a hand-written list round-trips as written).
std::string FormatMonsters(const std::vector<Pick>& picks);
// Renames one id in a list, keeping every other entry and its weight as it
// was. Returns how many entries named it (0 or 1).
int RenameMonster(std::vector<Pick>& picks, std::string_view from, std::string_view to);

} // namespace dungeon::game::style
