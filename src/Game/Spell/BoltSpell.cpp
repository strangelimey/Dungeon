// ============================================================================
// Game/Spell/BoltSpell.cpp — see BoltSpell.h.
// ============================================================================
#include "Game/Spell/BoltSpell.h"

#include "Game/Balance.h"
#include "Game/Catalog.h"
#include "Game/Character.h"

namespace dungeon::game {

BoltSpell::BoltSpell(std::string id, std::vector<SpellSymbol> sequence,
					 float power, float mana, float speed, float range,
					 int push)
	: Spell(std::move(id), std::move(sequence), power, mana), m_speed(speed),
	  m_range(range), m_push(push) {}

ProjectileSpec BoltSpell::MakeBolt(const Vec3& origin, const Vec3& dir,
								   float power, float accuracy,
								   TargetSide target) const {
	ProjectileSpec bolt;
	bolt.pos = origin;
	bolt.dir = dir;
	bolt.speed = m_speed;
	bolt.range = m_range;
	// A damaging spell's type is its SCHOOL (docs/combat.md part 1) — the
	// defender resists a fire bolt as fire. One line types every bolt, party-
	// and monster-cast alike.
	bolt.atk = {power, accuracy,
				m_types ? m_types->ForSchool(School()) : DamageType{}};
	const Vec4 g = ElementColor(School());
	bolt.color = {g.x * 1.7f, g.y * 1.7f, g.z * 1.7f, 0.0f}; // bright additive
	bolt.size = 0.2f;
	bolt.target = target;
	bolt.push = m_push; // displacement rides the bolt (party impacts only —
						// the monster-bolt hit path never shoves)
	return bolt;
}

void BoltSpell::Cast(CastContext& ctx) const {
	// The bonus rides school skill and the caster's stat, both already curved
	// by the host (CastContext::attackBonus); power arrives skill-scaled too.
	ProjectileSpec bolt = MakeBolt(ctx.origin, ctx.dir, ctx.power,
								   ctx.attackBonus, TargetSide::Monsters);
	bolt.attacker = ctx.casterIndex; // the impact credits its caster (threat)
	ctx.services.spawnBolt(bolt);
}

std::optional<ProjectileSpec> BoltSpell::MonsterBolt(const Vec3& origin,
													 const Vec3& dir,
													 float accuracy) const {
	// Monsters cast at base power — no school skill — at their own accuracy.
	return MakeBolt(origin, dir, m_power, accuracy, TargetSide::Party);
}

void BoltSpell::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_speed = e.GetFloat("speed", m_speed);
	m_range = e.GetFloat("range", m_range);
	m_push = static_cast<int>(e.GetFloat("push", static_cast<float>(m_push)));
}

} // namespace dungeon::game
