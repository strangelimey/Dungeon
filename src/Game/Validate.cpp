// ============================================================================
// Game/Validate.cpp — see Validate.h.
// ============================================================================
#include "Game/Validate.h"

#include <algorithm>
#include <format>
#include <queue>
#include <unordered_map>

namespace dungeon::game::validate {

namespace {

// --- the snapshot, indexed ---------------------------------------------------
// Everything the flood needs about one level, gathered once so the inner loop is
// map lookups rather than record scans. A cell key packs (x,z) into one int;
// levels are addressed by INDEX, because a stair names its destination as a stem
// and resolving that string on every step would dominate the walk.
struct Doorway {
	std::string name; // button-target id ("" = unwired)
	std::string key;  // item id that unlocks it ("" = not locked)
	bool open = false;
};

struct Level {
	const LevelView* view = nullptr;
	std::unordered_map<int, Doorway> doors;
	std::unordered_map<int, std::vector<std::string>> items;
	std::unordered_map<int, std::string> buttons;
	std::unordered_map<int, const StairLink*> stairs;
};

int CellKey(int x, int z) { return (z << 16) | (x & 0xFFFF); }
int KeyX(int k) { return static_cast<i16>(k & 0xFFFF); }
int KeyZ(int k) { return k >> 16; }

// A door is passable when it is not locked, was authored open, or we carry its
// key. A wired BUTTON also bypasses the lock, but that is a fact about what the
// flood has reached rather than about the door, so the caller adds it.
bool DoorPassable(const Doorway& d, const std::unordered_set<std::string>& held) {
	if (d.open || d.key.empty()) return true;
	return held.count(d.key) > 0;
}

// The world-tier checks (docs/world-map.md). Separate from the reachability
// fixpoint because they answer a different question: the fixpoint asks whether
// a dungeon can be FINISHED, this asks whether the tiers agree about what
// exists. Findings carry no level or cell — see the note on Run in the header.
void CheckWorld(const WorldView& world, const std::vector<LevelView>& levels,
				std::vector<Issue>& issues) {
	if (!world.map) return; // no overworld authored: nothing to disagree about

	std::unordered_set<std::string> stems;
	for (const LevelView& l : levels) stems.insert(l.stem);

	// A level may belong to at most one dungeon: two owners means the same
	// rooms are reachable from two places, and the save's per-dungeon state
	// would have to be in two places at once.
	std::unordered_map<std::string, std::string> ownerOf;
	for (const DungeonView& d : world.dungeons) {
		for (const std::string& stem : d.levels) {
			if (!stems.count(stem))
				issues.push_back({Severity::Error, "", -1, -1,
								  "map.check.dungeonnolevel", d.id, stem});
			else if (const auto it = ownerOf.find(stem); it != ownerOf.end())
				issues.push_back({Severity::Error, "", -1, -1,
								  "map.check.levelshared", stem, it->second});
			else ownerOf[stem] = d.id;
		}
		if (d.levels.empty())
			issues.push_back({Severity::Error, "", -1, -1,
							  "map.check.dungeonnolevels", d.id});
	}

	// Locations, and what they point at.
	std::unordered_set<std::string> reached;
	for (const WorldMap::Location& l : world.map->Locations()) {
		const std::string where = std::format("{},{}", l.x, l.z);
		if (l.kind == "dungeon") {
			const auto it = std::find_if(
				world.dungeons.begin(), world.dungeons.end(),
				[&](const DungeonView& d) { return d.id == l.Dungeon(); });
			if (it == world.dungeons.end())
				issues.push_back({Severity::Error, "", -1, -1,
								  "map.check.worldnodungeon", l.Dungeon(), where});
			else {
				reached.insert(it->id);
				// A DOORWAY MUST SAY WHERE IT LEADS. Since a dungeon has no
				// start of its own, an unauthored `level` is not a default any
				// more — it is a hole, and the party would arrive wherever the
				// first level happens to be.
				if (l.level.empty())
					issues.push_back({Severity::Error, "", -1, -1,
									  "map.check.locationnolevel", l.id});
				else if (std::find(it->levels.begin(), it->levels.end(), l.level) ==
						 it->levels.end())
					issues.push_back({Severity::Error, "", -1, -1,
									  "map.check.locationlevel", l.id, l.level});
			}
		}
		// A location the party can never stand on is unreachable however sound
		// the dungeon behind it is.
		if (!world.map->Passable(l.x, l.z))
			issues.push_back({Severity::Error, "", -1, -1, "map.check.worldblocked",
							  l.id, world.map->TerrainAt(l.x, l.z).id});
	}

	for (const DungeonView& d : world.dungeons)
		if (!reached.count(d.id))
			issues.push_back({Severity::Warning, "", -1, -1,
							  "map.check.dungeonunreached", d.id});

	// A level no dungeon claims is not an error — the editor's gallery levels
	// are exactly that — but it is worth saying once, because the usual cause
	// is a level added to the manifest and never wired into its dungeon.
	for (const LevelView& l : levels)
		if (!ownerOf.count(l.stem))
			issues.push_back({Severity::Warning, "", -1, -1,
							  "map.check.levelorphan", l.stem});

	// QUEST HOOKS. An item naming a quest, a stage or a place that does not
	// exist is content that will silently do nothing when it is picked up —
	// and picking it up is the only way anyone would ever find out.
	for (const ItemHookView& h : world.itemHooks) {
		if (!h.quest.empty()) {
			const auto q = std::find_if(
				world.quests.begin(), world.quests.end(),
				[&](const QuestView& v) { return v.id == h.quest; });
			if (q == world.quests.end())
				issues.push_back({Severity::Error, "", -1, -1,
								  "map.check.itemnoquest", h.item, h.quest});
			else if (std::find(q->stages.begin(), q->stages.end(), h.stage) ==
					 q->stages.end())
				issues.push_back({Severity::Error, "", -1, -1,
								  "map.check.itemnostage", h.item, h.stage});
		}
		if (!h.reveals.empty()) {
			bool found = false;
			for (const WorldMap::Location& l : world.map->Locations())
				if (l.id == h.reveals) found = true;
			if (!found)
				issues.push_back({Severity::Error, "", -1, -1,
								  "map.check.itemnoplace", h.item, h.reveals});
		}
	}
	// A quest nothing can reach is content that cannot happen. A WARNING, not
	// an error: a quest may legitimately be moved by something other than an
	// item once there is anything else to move it.
	for (const QuestView& q : world.quests) {
		bool reachable = false;
		for (const ItemHookView& h : world.itemHooks)
			if (h.quest == q.id) reachable = true;
		if (!reachable)
			issues.push_back({Severity::Warning, "", -1, -1,
							  "map.check.questunreachable", q.id});
	}

	// The party has to be able to stand where a new game puts it.
	if (!world.map->Passable(world.map->StartX(), world.map->StartZ()))
		issues.push_back(
			{Severity::Error, "", -1, -1, "map.check.worldstart",
			 std::format("{},{}", world.map->StartX(), world.map->StartZ())});
}

} // namespace

std::vector<Issue> Run(const std::vector<LevelView>& levels,
					   const std::string& startLevel, const Rules& rules,
					   const WorldView& world) {
	std::vector<Issue> issues;
	if (levels.empty()) {
		issues.push_back({Severity::Error, "", -1, -1, "map.check.nolevels"});
		return issues;
	}

	// --- index the snapshot --------------------------------------------------
	std::vector<Level> lv(levels.size());
	std::unordered_map<std::string, int> byStem;
	for (size_t i = 0; i < levels.size(); ++i) {
		lv[i].view = &levels[i];
		byStem[levels[i].stem] = static_cast<int>(i);
		if (levels[i].ents) {
			for (const Entity& e : levels[i].ents->All()) {
				const int k = CellKey(e.x, e.z);
				if (e.kind == EntityKind::Door) {
					Doorway d;
					if (const std::string* n = e.Param("name")) d.name = *n;
					if (const std::string* p = e.Param("key")) d.key = *p;
					if (const std::string* o = e.Param("open")) d.open = (*o == "1");
					lv[i].doors[k] = std::move(d);
				} else if (e.kind == EntityKind::Item) {
					lv[i].items[k].push_back(e.type);
				} else if (e.kind == EntityKind::Button) {
					if (const std::string* t = e.Param("target")) lv[i].buttons[k] = *t;
				}
			}
		}
		if (levels[i].map)
			for (const StairLink& s : levels[i].map->Stairs())
				lv[i].stairs[CellKey(s.x, s.z)] = &s;
	}

	const auto startIt = byStem.find(startLevel);
	const int startIdx = startIt == byStem.end() ? 0 : startIt->second;
	const DungeonMap* startMap = lv[startIdx].view->map;
	if (!startMap) {
		issues.push_back({Severity::Error, startLevel, -1, -1, "map.check.nostart"});
		return issues;
	}

	// EVERY WAY IN IS A WAY IN. The flood used to start only at the first
	// level's own start cell, because a stair was the only way between levels.
	// The world map is another: a level a doorway opens onto is reached by
	// WALKING THERE, and the checker calling it unreachable would be reporting a
	// fault that only exists inside its own model — which is worse than no check
	// at all, since the report is what people act on.
	std::vector<std::pair<int, int>> seeds{
		{startIdx, CellKey(startMap->StartX(), startMap->StartZ())}};
	if (world.map)
		for (const WorldMap::Location& l : world.map->Locations()) {
			const auto d = byStem.find(l.level);
			if (d == byStem.end() || !lv[d->second].view->map) continue;
			const DungeonMap* dm = lv[d->second].view->map;
			// An unauthored cell means the level's own start, the same fallback
			// the game applies when it opens the door.
			const int cx = l.entryX >= 0 ? l.entryX : dm->StartX();
			const int cz = l.entryZ >= 0 ? l.entryZ : dm->StartZ();
			seeds.push_back({d->second, CellKey(cx, cz)});
		}

	// --- the fixpoint --------------------------------------------------------
	// Flood with what is currently held, pick up whatever that reaches, and go
	// again. A round that opens nothing new is the answer. Re-flooding from
	// scratch each round rather than resuming is deliberate: a newly held key can
	// open a door far behind the frontier, and continuing forward would never
	// look back through it.
	std::vector<std::unordered_set<int>> seen(lv.size());
	std::unordered_set<std::string> held;
	std::unordered_set<std::string> pressable; // door names a reached button opens

	for (;;) {
		const size_t heldBefore = held.size(), pressBefore = pressable.size();
		size_t reachedBefore = 0;
		for (const auto& s : seen) reachedBefore += s.size();

		for (auto& s : seen) s.clear();
		std::queue<std::pair<int, int>> q; // (level index, cell key)
		for (const auto& seed : seeds) q.push(seed);

		while (!q.empty()) {
			const std::pair<int, int> cur = q.front();
			q.pop();
			const int li = cur.first, ck = cur.second;
			if (!seen[li].insert(ck).second) continue;
			const Level& L = lv[li];
			const int x = KeyX(ck), z = KeyZ(ck);

			// Pick up what is here, and note a button we can now press.
			if (const auto it = L.items.find(ck); it != L.items.end())
				for (const std::string& id : it->second)
					if (rules.keyItems.count(id)) held.insert(id);
			if (const auto it = L.buttons.find(ck); it != L.buttons.end())
				pressable.insert(it->second);

			// A traversable stair carries us to another level.
			if (const auto it = L.stairs.find(ck); it != L.stairs.end()) {
				const StairLink* s = it->second;
				if (rules.traversableStairs.count(s->type)) {
					const auto d = byStem.find(s->destLevel);
					if (d != byStem.end() && lv[d->second].view->map &&
						lv[d->second].view->map->IsWalkable(s->destX, s->destZ))
						q.push({d->second, CellKey(s->destX, s->destZ)});
				}
			}

			// The four neighbours (the grid is 4-cardinal — never diagonal).
			constexpr int dx[4] = {0, 1, 0, -1};
			constexpr int dz[4] = {-1, 0, 1, 0};
			for (int i = 0; i < 4; ++i) {
				const int nx = x + dx[i], nz = z + dz[i];
				if (!L.view->map->IsWalkable(nx, nz)) continue;
				const int nk = CellKey(nx, nz);
				if (seen[li].count(nk)) continue;
				if (const auto it = L.doors.find(nk); it != L.doors.end()) {
					const Doorway& d = it->second;
					const bool byButton = !d.name.empty() && pressable.count(d.name) > 0;
					if (!DoorPassable(d, held) && !byButton) continue;
				}
				q.push({li, nk});
			}
		}

		size_t reachedAfter = 0;
		for (const auto& s : seen) reachedAfter += s.size();
		if (held.size() == heldBefore && pressable.size() == pressBefore &&
			reachedAfter == reachedBefore)
			break;
	}

	// --- report --------------------------------------------------------------
	for (size_t i = 0; i < lv.size(); ++i) {
		const Level& L = lv[i];
		const std::string& stem = L.view->stem;
		if (!L.view->map) continue;

		// A level nothing reaches at all — the loudest form of the fault, and
		// worth saying once instead of as one issue per stranded object.
		//
		// It suppresses the findings DERIVED from reachability (every locked door
		// on an unreachable level is trivially unopenable, and every item on it
		// trivially lost — one issue per object, all of them restating this one).
		// It deliberately does NOT suppress the STRUCTURAL checks below: stair
		// pairing and button wiring are facts about the records, true or false
		// whether or not anything can walk there, and hiding them behind a
		// reachability fault would mean fixing the first fault only to discover
		// the next. Found by mutation — locking one door stranded two levels and
		// silently took their pairing warnings with it.
		const bool stranded = seen[i].empty();
		if (stranded)
			issues.push_back({Severity::Error, stem, -1, -1, "map.check.levellost"});

		// Doors that can never be opened. THE interesting failure: the key is
		// behind the door it opens, so no round of the fixpoint ever holds it.
		for (const auto& [ck, d] : L.doors) {
			if (stranded) break; // see above: derived from reachability
			if (d.key.empty() || d.open) continue;
			if (held.count(d.key)) continue;
			if (!d.name.empty() && pressable.count(d.name)) continue;
			issues.push_back({Severity::Error, stem, KeyX(ck), KeyZ(ck),
							  "map.check.doorlocked", d.key});
		}

		// A button wired to a name no door carries: a rename or delete that did
		// not sweep. Silent in play — the lever throws and nothing happens.
		for (const auto& [ck, target] : L.buttons) {
			if (target.empty()) continue;
			bool found = false;
			for (const Level& other : lv) {
				for (const auto& [dk, d] : other.doors)
					if (d.name == target) { found = true; break; }
				if (found) break;
			}
			if (!found)
				issues.push_back({Severity::Warning, stem, KeyX(ck), KeyZ(ck),
								  "map.check.buttondead", target});
		}

		// Stair pairing, checked explicitly so DRIFT is named as the cause: the
		// pair is auto-authored on placement, so a broken one means a hand edit,
		// a rename or a cross-level delete rather than a design choice.
		for (const StairLink& s : L.view->map->Stairs()) {
			if (rules.exitStairs.count(s.type)) {
				// An exit's `dest` names a world LOCATION, not a level. Empty
				// is fine (it surfaces wherever the party came in); a name that
				// matches nothing is the same drift the pair check catches.
				if (!s.destLevel.empty() && s.destLevel != "-" && world.map) {
					bool found = false;
					for (const WorldMap::Location& l : world.map->Locations())
						if (l.id == s.destLevel) found = true;
					if (!found)
						issues.push_back({Severity::Error, stem, s.x, s.z,
										  "map.check.exitnolocation", s.destLevel});
				}
				continue;
			}
			const auto d = byStem.find(s.destLevel);
			if (d == byStem.end()) {
				issues.push_back({Severity::Error, stem, s.x, s.z,
								  "map.check.stairnolevel", s.destLevel});
				continue;
			}
			const DungeonMap* dm = lv[d->second].view->map;
			if (!dm || !dm->IsWalkable(s.destX, s.destZ)) {
				issues.push_back({Severity::Error, stem, s.x, s.z,
								  "map.check.stairblocked", s.destLevel});
				continue;
			}
			// THE RULE: a stair leads to its OWN coordinates on the other level,
			// where its counterpart stands. That is what makes going down and
			// back up return you to the square you left — and it is what
			// AddStairAt authors, so anything else is drift from a hand edit.
			//
			// Checked directly rather than inferred from the far side, because
			// this says precisely what is wrong: the destination is the wrong
			// SQUARE, which is a one-line fix, where "no matching stair" sends
			// you looking for a missing record that may not be the problem.
			if (s.destX != s.x || s.destZ != s.z) {
				issues.push_back({Severity::Error, stem, s.x, s.z,
								  "map.check.stairoffset", s.destLevel});
				continue;
			}
			// And the counterpart has to actually be there. An ERROR now, not a
			// warning: the earlier reasoning — that a one-way drop might be
			// deliberate — was wrong. A pit authors its pit_ceiling half at the
			// same cell pointing back (it is merely `traverse = 0`, scenery you
			// cannot climb), so a correct one-way drop still PASSES this. Nothing
			// legitimate is left that fails it.
			const auto back = lv[d->second].stairs.find(CellKey(s.destX, s.destZ));
			if (back == lv[d->second].stairs.end() ||
				back->second->destLevel != stem || back->second->destX != s.x ||
				back->second->destZ != s.z)
				issues.push_back({Severity::Error, stem, s.x, s.z,
								  "map.check.stairunpaired", s.destLevel});
		}

		// Items nothing can reach, counted rather than listed: a walled-off wing
		// would otherwise bury every other finding under one issue per square,
		// and the COUNT is what says how big the hole is.
		int lost = 0, lx = -1, lz = -1;
		for (const auto& [ck, ids] : (stranded ? decltype(L.items){} : L.items))
			if (!seen[i].count(ck)) {
				lost += static_cast<int>(ids.size());
				if (lx < 0) { lx = KeyX(ck); lz = KeyZ(ck); }
			}
		if (lost > 0)
			issues.push_back({Severity::Warning, stem, lx, lz, "map.check.itemslost",
							  std::to_string(lost)});
	}

	CheckWorld(world, levels, issues);

	std::stable_sort(issues.begin(), issues.end(),
					 [](const Issue& a, const Issue& b) {
						 return a.severity < b.severity; // errors first
					 });
	return issues;
}

} // namespace dungeon::game::validate
