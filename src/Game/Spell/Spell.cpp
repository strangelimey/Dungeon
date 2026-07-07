// ============================================================================
// Game/Spell/Spell.cpp — see Spell.h.
// ============================================================================
#include "Game/Spell/Spell.h"

#include "Game/Catalog.h"

namespace dungeon::game {

Spell::Spell(std::string id, std::vector<SpellSymbol> sequence, float power,
			 float mana)
	: m_id(std::move(id)), m_nameKey("spell." + m_id),
	  m_descKey(m_nameKey + ".desc"), m_sequence(std::move(sequence)),
	  m_power(power), m_mana(mana) {}

std::optional<ProjectileSpec> Spell::MonsterBolt(const Vec3&, const Vec3&,
												 float) const {
	return std::nullopt; // no thrown form — the monster falls back to its shot
}

void Spell::ApplyOverrides(const CatalogEntry& e) {
	m_nameKey = e.Get("name", m_nameKey);
	m_power = e.GetFloat("power", m_power);
	m_mana = e.GetFloat("mana", m_mana);
}

} // namespace dungeon::game
