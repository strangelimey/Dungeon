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
	// The AREA burst, if this spell is one. `blast_force` in squares is the gate;
	// the other two only mean anything alongside it.
	m_blast.force = static_cast<int>(
		e.GetFloat("blast_force", static_cast<float>(m_blast.force)));
	m_blast.damage = e.GetFloat("blast_damage", m_blast.damage);
	m_blast.falloff = e.GetFloat("blast_falloff", m_blast.falloff);
}

ProjectilePayload Spell::MakePayload() const {
	ProjectilePayload out = PackPayload(m_procs, "spells.cat [" + m_id + "]");
	out.blast = m_blast;
	return out;
}

} // namespace dungeon::game
