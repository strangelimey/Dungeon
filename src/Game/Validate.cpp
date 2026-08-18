// ============================================================================
// Game/Validate.cpp — see Validate.h.
// ============================================================================
#include "Game/Validate.h"

#include <algorithm>
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

} // namespace

std::vector<Issue> Run(const std::vector<LevelView>& levels,
					   const std::string& startLevel, const Rules& rules) {
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
		q.push({startIdx, CellKey(startMap->StartX(), startMap->StartZ())});

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
			// The far side should hold a stair pointing back. A WARNING only: a
			// one-way drop is legitimate content (a pit), and this check cannot
			// tell that from a broken pair — so it reports what it sees and lets
			// the author judge, rather than crying wolf about a working dungeon.
			const auto back = lv[d->second].stairs.find(CellKey(s.destX, s.destZ));
			if (back == lv[d->second].stairs.end() ||
				back->second->destLevel != stem || back->second->destX != s.x ||
				back->second->destZ != s.z)
				issues.push_back({Severity::Warning, stem, s.x, s.z,
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

	std::stable_sort(issues.begin(), issues.end(),
					 [](const Issue& a, const Issue& b) {
						 return a.severity < b.severity; // errors first
					 });
	return issues;
}

} // namespace dungeon::game::validate
