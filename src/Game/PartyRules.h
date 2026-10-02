// ============================================================================
// Game/PartyRules.h - the arithmetic of making a party member, kept PURE
// (docs/party-creation-plan.md).
//
// Party creation has a few numbers that must agree everywhere a member can be
// made - the creation page, the `newparty` dev command and the tests: what a
// race does to the five stats, how many free points there are to spend on top,
// how far a stat may fall, and how far a picked starting skill is boosted. They
// live here, with no Character, catalog or UI in sight, so RollTest links this
// header and checks the rules directly (the Defense.h / Resource.h bargain).
//
// There are NO CLASSES (Michael): a member is their race plus the points they
// spent plus the skills they picked, and after that whatever they do.
// ============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace dungeon::game::party {

// The five stats in Character / kStats order: STR, DEX, VIT, WIL, INT.
inline constexpr size_t kStats = 5;
using StatArray = std::array<int, kStats>;

// Every stat starts here before the race touches it.
inline constexpr int kStatBase = 10;
// No race modifier or spending can leave a stat below this.
inline constexpr int kStatFloor = 3;
// Free points every member spends on top of the race (a race may add more:
// RaceStats::extraPoints - the human's versatility).
inline constexpr int kFreePoints = 5;
// Starting skills: this many picks, each boosted to this LEVEL. Level is
// floor(sqrt(xp)) (Character::LevelForXp), so the xp is the level squared.
inline constexpr int kSkillPicks = 2;
inline constexpr int kSkillBoostLevel = 2;
inline constexpr float kSkillBoostXp =
	static_cast<float>(kSkillBoostLevel * kSkillBoostLevel);
// Starting items each member picks.
inline constexpr int kItemPicks = 2;
// A party is one to four members.
inline constexpr size_t kMinMembers = 1, kMaxMembers = 4;

// What a race does to the numbers (races.cat; the rest of a race - pace, bases,
// resists - is applied by the Game, which has the catalogs).
struct RaceStats {
	StatArray mods{};    // added to kStatBase, may be negative
	int extraPoints = 0; // on top of kFreePoints
};

// The points a member of this race has to spend.
inline int PointBudget(const RaceStats& race) { return kFreePoints + race.extraPoints; }

// Points spent so far (only additions: a stat is never bought down).
inline int PointsSpent(const StatArray& spent) {
	int n = 0;
	for (int s : spent) n += s;
	return n;
}

// The stat a member ends up with: base + race + spent, never under the floor.
inline int StatValue(const RaceStats& race, const StatArray& spent, size_t stat) {
	const int v = kStatBase + race.mods[stat] + spent[stat];
	return v < kStatFloor ? kStatFloor : v;
}

inline StatArray Stats(const RaceStats& race, const StatArray& spent) {
	StatArray out{};
	for (size_t i = 0; i < kStats; ++i) out[i] = StatValue(race, spent, i);
	return out;
}

// Whether `spent` is a legal spending for this race: no negative entries, and no
// more than the budget in total.
inline bool SpendingValid(const RaceStats& race, const StatArray& spent) {
	for (int s : spent)
		if (s < 0) return false;
	return PointsSpent(spent) <= PointBudget(race);
}

// Whether one more point can go into `stat` / come back out of it.
inline bool CanSpend(const RaceStats& race, const StatArray& spent) {
	return PointsSpent(spent) < PointBudget(race);
}
inline bool CanRefund(const StatArray& spent, size_t stat) { return spent[stat] > 0; }

// A starting-skill pick is legal when there are at most kSkillPicks of them,
// none repeated and none empty.
inline bool SkillPicksValid(const std::vector<std::string>& picks) {
	if (picks.size() > static_cast<size_t>(kSkillPicks)) return false;
	for (size_t i = 0; i < picks.size(); ++i) {
		if (picks[i].empty()) return false;
		for (size_t j = i + 1; j < picks.size(); ++j)
			if (picks[i] == picks[j]) return false;
	}
	return true;
}

// ONE MEMBER AS CHOSEN - what the creation page edits, what `newparty` parses,
// what Game::BuildMember turns into a Character. Plain data, so a page and a
// script describe a member identically.
struct MemberSpec {
	std::string name;
	std::string race;      // races.cat id
	std::string portrait;  // portraits.cat id
	std::array<float, 4> color{0.3f, 0.3f, 0.3f, 1.0f};
	bool colorSet = false; // false = a premade member keeps its own colour
	StatArray spent{};     // free points per stat, on top of the race
	std::vector<std::string> skills; // up to kSkillPicks, boosted to kSkillBoostLevel
	std::vector<std::string> items;  // up to kItemPicks, from the world's start list
	// A PREMADE member: an index into CreateDefaultParty, whose authored stats,
	// bases, pace and kit are used as they are (the default four - the eval
	// suites measure exactly them). -1 = built from race + points. Only the name,
	// portrait and colour of a premade member come from this spec, and each only
	// when given (an empty name / portrait, or colorSet false, keeps its own).
	int premade = -1;
};

// A name a member may have: 1..kMaxNameLength characters, no underscore (a save
// line stores a name with its spaces as underscores) and not all spaces.
inline constexpr size_t kMaxNameLength = 16;
inline bool NameValid(const std::string& name) {
	if (name.empty() || name.size() > kMaxNameLength) return false;
	bool any = false;
	for (char c : name) {
		if (c == '_' || c == '\t' || c == '\n' || c == '\r') return false;
		if (c != ' ') any = true;
	}
	return any;
}

} // namespace dungeon::game::party
