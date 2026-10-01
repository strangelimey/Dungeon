// ============================================================================
// Game/DungeonWorld_Census.cpp - what a world HOLDS, as numbers the editor can
// show (docs/tool-refinement-plan.md): each monster kind's power (Phase 2).
//
// A monster's power is its derived threat unless the catalog overrides it
// (Game/Power.h). The palette draws a band per monster row every frame, so the
// whole project's powers are worked out together and kept until something
// could have changed them: an edit (the revision moves - a type save calls
// NoteEdit) or a Balance change (InvalidatePowers).
// ============================================================================
#include "Game/DungeonWorld.h"

namespace dungeon::game {

const DungeonWorld::PowerCache& DungeonWorld::Powers() const {
	if (m_powers.valid && m_powers.revision == m_editRevision) return m_powers;
	m_powers.kinds.clear();
	m_powers.range = {};
	for (const CatalogEntry& e : m_project.monsters.Entries()) {
		PowerCache::Kind k;
		k.derived = threat::Of(e, ThreatProfile(e)).threat;
		k.resolved = power::Resolve(k.derived, e.GetFloat("power", 0.0f));
		m_powers.kinds[e.id] = k;
		// The band's scale is what a designer can place: hidden kinds are
		// internal and would stretch it with numbers nobody sees.
		if (!CatalogBool(&e, "hidden", false)) m_powers.range.Add(k.resolved);
	}
	m_powers.revision = m_editRevision;
	m_powers.valid = true;
	return m_powers;
}

double DungeonWorld::MonsterPower(const CatalogEntry& monster) const {
	// A kind of this project answers from the cache; anything else (a template's
	// entry, scored for the wizard) is worked out on the spot, the same way.
	if (const CatalogEntry* own = m_project.monsters.Find(monster.id); own == &monster) {
		const auto& kinds = Powers().kinds;
		if (const auto it = kinds.find(monster.id); it != kinds.end()) return it->second.resolved;
	}
	return power::Resolve(DerivedPower(monster), monster.GetFloat("power", 0.0f));
}

double DungeonWorld::DerivedPower(const CatalogEntry& monster) const {
	if (const CatalogEntry* own = m_project.monsters.Find(monster.id); own == &monster) {
		const auto& kinds = Powers().kinds;
		if (const auto it = kinds.find(monster.id); it != kinds.end()) return it->second.derived;
	}
	return threat::Of(monster, ThreatProfile(monster)).threat;
}

int DungeonWorld::MonsterBand(const std::string& id) const {
	const PowerCache& c = Powers();
	const auto it = c.kinds.find(id);
	return it == c.kinds.end() ? 0 : power::Band(it->second.resolved, c.range);
}

power::Range DungeonWorld::MonsterPowerRange() const { return Powers().range; }

// --- the census ----------------------------------------------------------------

// A quest item is one whose TYPE hooks the world on pickup (Game::OnItemFound):
// it advances a quest, sets a flag or reveals a place.
bool DungeonWorld::IsQuestItem(const CatalogEntry* e) {
	return e && (!e->Get("quest", "").empty() || !e->Get("flag", "").empty() ||
				 !e->Get("reveals", "").empty());
}

const std::vector<DungeonWorld::LevelCensus>& DungeonWorld::Census() {
	// The active level's monsters are the LIVE list (below), which a kill or an
	// editor placement changes; so its size and the level it belongs to are part
	// of the cache's key, beside the edit revision.
	if (m_census.valid && m_census.revision == m_editRevision &&
		m_census.liveMonsters == m_monsters.size() && m_census.level == m_currentLevel)
		return m_census.levels;
	m_census.levels.clear();
	const PowerCache& powers = Powers();
	auto countMonster = [&](LevelCensus& c, const std::string& type) {
		++c.monsters;
		const auto it = powers.kinds.find(type);
		if (it == powers.kinds.end()) return; // a kind the catalog lost
		const int band = power::Band(it->second.resolved, powers.range);
		++c.bands[static_cast<size_t>(band - 1)];
		if (it->second.resolved > c.strongestPower) {
			c.strongestPower = it->second.resolved;
			c.strongest = type;
		}
	};
	for (const std::string& stem : m_project.levels) {
		// The level as it IS now: live if active, else its unsaved stash, else
		// the files read-only (Validate's walk - never stash to read).
		const DungeonMap* map = nullptr;
		const DungeonEntities* ents = nullptr;
		if (stem == m_currentLevel) {
			map = &m_map;
			ents = &m_entities;
		} else {
			const auto ms = m_levelMaps.find(stem);
			const auto es = m_levelEnts.find(stem);
			const ReadOnlyLevel* ro = nullptr;
			if (ms == m_levelMaps.end() || es == m_levelEnts.end()) ro = &ReadOnlyLevelOf(stem);
			map = ms != m_levelMaps.end() ? ms->second.get() : ro->map.get();
			ents = es != m_levelEnts.end() ? es->second.get() : ro->ents.get();
		}
		LevelCensus c;
		c.stem = stem;
		if (const CatalogEntry* d = m_project.DungeonOfLevel(stem)) c.dungeon = d->id;
		// Monsters: on the ACTIVE level the live list - an editor-placed one has
		// no record, and it is what a save writes (ActiveEntText) - elsewhere the
		// records.
		const bool active = stem == m_currentLevel;
		if (active)
			for (const Monster& m : m_monsters)
				if (m.kind) countMonster(c, m.kind->name);
		for (const Entity& e : ents->All()) {
			switch (e.kind) {
			case EntityKind::Monster:
				if (!active) countMonster(c, e.type);
				break;
			case EntityKind::Item:
				++c.items;
				if (IsQuestItem(m_project.FindItem(e.type))) {
					++c.questItems;
					c.questPlaced.push_back({e.type, e.x, e.z});
				}
				break;
			case EntityKind::Door:
				++c.doors;
				if (const std::string* key = e.Param("key"); key && !key->empty())
					++c.lockedDoors;
				break;
			case EntityKind::Button: ++c.buttons; break;
			default: break;
			}
		}
		for (const StairLink& s : map->Stairs())
			if (CatalogBool(m_project.stairs.Find(s.type), "traverse", true)) ++c.stairs;
		m_census.levels.push_back(std::move(c));
	}
	m_census.revision = m_editRevision;
	m_census.liveMonsters = m_monsters.size();
	m_census.level = m_currentLevel;
	m_census.valid = true;
	return m_census.levels;
}

} // namespace dungeon::game
