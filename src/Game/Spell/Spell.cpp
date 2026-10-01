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
	// (`on_hit = burn 3 6 0.5`) - parsed and packed once here, at load, so a cast
	// copies a ready payload into its carrier instead of building one per shot
	// (and the "more than kMaxPayloadProcs" warning fires once, at load). An
	// absent field keeps the class default, like every other override.
	if (e.Find("on_hit")) {
		const std::string where = "spells.cat [" + m_id + "]";
		std::vector<fx::Proc> procs;
		fx::ParseProcs(e.Get("on_hit", ""), procs, where);
		const BlastSpec blast = m_payload.blast;
		m_payload = PackPayload(procs, where);
		m_payload.blast = blast;
	}
	ReadBlastRules(e, m_payload.blast);
}

// The AREA blast, if an entry authors one. `blast_force` in squares is the gate;
// the rest only mean anything alongside it. `blast_rate` is the EXPANSION SPEED
// in seconds per tick — small for a fireball, large for a creeping gas — and
// `blast_persist` decides whether squares vacate behind the front (fire) or fill
// and keep biting (gas), `blast_linger` how long a gas then hangs where it
// spread, and `blast_color` the glow it is seen as. A spell and a thrown item
// (ui-updates Phase 10: a poison gas flask) both author it this way.
void ReadBlastRules(const CatalogEntry& e, BlastSpec& spec) {
	if (CatalogColor(&e, "blast_color", spec.color)) spec.hasColor = true;
	blast::Rules& b = spec.rules;
	b.force = static_cast<int>(e.GetFloat("blast_force", static_cast<float>(b.force)));
	b.damage = e.GetFloat("blast_damage", b.damage);
	b.falloff = e.GetFloat("blast_falloff", b.falloff);
	b.rate = e.GetFloat("blast_rate", b.rate);
	b.linger = e.GetFloat("blast_linger", b.linger);
	if (e.GetBool("blast_persist", b.persistence == blast::Persistence::Persistent))
		b.persistence = blast::Persistence::Persistent;
}

} // namespace dungeon::game
