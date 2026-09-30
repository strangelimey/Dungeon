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

} // namespace dungeon::game
