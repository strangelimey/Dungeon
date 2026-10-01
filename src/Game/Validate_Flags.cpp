// ============================================================================
// Game/Validate_Flags.cpp - the FLAG checks (docs/tool-refinement-plan.md
// Phase 4; see CheckFlags in Validate.h).
//
// A flag is written in one place and read in another, often on another level,
// so a mistake in either half is invisible from the other: a door waiting on
// a flag no lever sets looks exactly like a door waiting on one that does.
// What is checked:
//   - a door, lever or stair waiting on a flag NOTHING sets can never be used
//     (an error, boxed where it stands - the doorlocked check's shape);
//   - a flag in flags.cat that nothing sets or reads, and no item type
//     hooks, is dead content;
//   - a record naming a flag flags.cat lacks cannot be picked in the editor
//     and is usually a typo;
//   - a DUNGEON's flag set or read in another dungeon is local state leaking.
//
// WHAT SETS A FLAG: a lever's `sets=` / `toggles=` (a `clears=` only turns
// one off), and an item whose type carries `flag` - but only once it is
// PLACED somewhere, since an item nobody can find sets nothing.
//
// Deliberately NOT here: a flag-waiting door in the reachability flood. The
// flood treats it as passable, as it treats a wired door; deciding whether the
// setter lies on the right side of it is a fixpoint of its own, and the
// "nothing sets it" error already names the unusable case.
// ============================================================================
#include "Game/Validate.h"

#include <format>
#include <unordered_map>
#include <unordered_set>

namespace dungeon::game::validate {

namespace {
// One use of a flag on a level: what it is, where, and whether it reads it.
struct Use {
	std::string flag;
	std::string level;
	int x = -1, z = -1;
	const char* what = ""; // "door", "lever", "stair", "item"
	bool reads = false;     // waits on it (a door/lever/stair flag=)
	bool sets = false;      // can turn it on
};

// "k=v" -> "k"; a bare name is itself.
std::string FlagKey(const std::string& v) {
	const size_t eq = v.find('=');
	return eq == std::string::npos ? v : v.substr(0, eq);
}
} // namespace

void CheckFlags(const std::vector<LevelView>& levels, const WorldView& world,
				std::vector<Issue>& issues) {
	// Which dungeon each level belongs to (first claim wins, as Project's does).
	std::unordered_map<std::string, std::string> dungeonOf;
	for (const DungeonView& d : world.dungeons)
		for (const std::string& stem : d.levels) dungeonOf.emplace(stem, d.id);
	std::unordered_map<std::string, std::string> scopeOf; // flag -> its dungeon
	for (const FlagView& f : world.flags) scopeOf[f.id] = f.dungeon;
	std::unordered_map<std::string, std::string> itemFlag; // item type -> flag
	for (const ItemHookView& h : world.itemHooks)
		if (!h.flag.empty()) itemFlag[h.item] = h.flag;

	std::vector<Use> uses;
	for (const LevelView& l : levels) {
		if (l.ents)
			for (const Entity& e : l.ents->All()) {
				Use u{.level = l.stem, .x = e.x, .z = e.z};
				if (e.kind == EntityKind::Door) {
					if (const std::string* f = e.Param("flag"); f && !f->empty()) {
						u.flag = *f;
						u.what = "door";
						u.reads = true;
						uses.push_back(u);
					}
				} else if (e.kind == EntityKind::Button) {
					u.what = "lever";
					if (const std::string* f = e.Param("flag"); f && !f->empty()) {
						Use r = u;
						r.flag = *f;
						r.reads = true;
						uses.push_back(r);
					}
					for (const auto& [k, v] : e.params)
						if ((k == "sets" || k == "toggles" || k == "clears") && !v.empty()) {
							Use w = u;
							w.flag = v;
							w.sets = k != "clears";
							uses.push_back(w);
						}
				} else if (e.kind == EntityKind::Item) {
					if (const auto it = itemFlag.find(e.type); it != itemFlag.end()) {
						u.flag = FlagKey(it->second);
						u.what = "item";
						u.sets = true;
						uses.push_back(u);
					}
				}
			}
		if (l.map)
			for (const StairLink& s : l.map->Stairs())
				if (!s.flag.empty())
					uses.push_back({s.flag, l.stem, s.x, s.z, "stair", true, false});
	}

	std::unordered_set<std::string> set, read;
	for (const Use& u : uses) {
		if (u.sets) set.insert(u.flag);
		if (u.reads) read.insert(u.flag);
	}

	std::unordered_set<std::string> unknownSaid; // one finding per unknown name
	for (const Use& u : uses) {
		// A wait nothing can end: the thing is dead weight however it is placed.
		if (u.reads && !set.count(u.flag))
			issues.push_back({Severity::Error, u.level, u.x, u.z, "map.check.flagwaits",
							  u.flag});
		const auto scope = scopeOf.find(u.flag);
		if (scope == scopeOf.end()) {
			// Hand-authored, or a flag since deleted: the editor cannot list it.
			if (unknownSaid.insert(u.flag).second)
				issues.push_back({Severity::Warning, u.level, u.x, u.z,
								  "map.check.flagunknown", u.flag});
			continue;
		}
		// A dungeon's own flag used from outside it.
		const auto owner = dungeonOf.find(u.level);
		const std::string here = owner == dungeonOf.end() ? std::string() : owner->second;
		if (!scope->second.empty() && scope->second != here)
			issues.push_back({Severity::Warning, u.level, u.x, u.z, "map.check.flagscope",
							  u.flag, scope->second});
	}

	// Authored flags nothing touches at all. One that is read but never set has
	// already been named at each reader above. An item TYPE hooking the flag
	// counts as touching it whether or not it is placed yet: a quest item
	// written but not yet put down is ordinary work in progress (a level
	// regenerated under it is another way to get there), not dead content.
	std::unordered_set<std::string> hooked;
	for (const auto& [item, flag] : itemFlag) hooked.insert(FlagKey(flag));
	for (const FlagView& f : world.flags)
		if (!set.count(f.id) && !read.count(f.id) && !hooked.count(f.id))
			issues.push_back({Severity::Warning, "", -1, -1, "map.check.flagunused", f.id});
}

} // namespace dungeon::game::validate
