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
	// What the cast LEAVES BEHIND, authored exactly as a weapon's is
	// (`on_hit = burn 3 6 0.5`) — parsed once here, at load, so a cast copies a
	// ready list into its carrier instead of parsing per shot. An absent field
	// keeps the class default, like every other override.
	if (e.Find("on_hit")) {
		m_procs.clear();
		fx::ParseProcs(e.Get("on_hit", ""), m_procs, "spells.cat [" + m_id + "]");
	}
}

ProjectilePayload Spell::MakePayload() const {
	return PackPayload(m_procs, "spells.cat [" + m_id + "]");
}

} // namespace dungeon::game
