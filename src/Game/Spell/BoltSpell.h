// ============================================================================
// Game/Spell/BoltSpell.h — the shared thrown form: a spell that flies a bolt.
//
// The directed spells (the four tier-1 element bolts and the Project-form
// casts) differ only in numbers — speed, range, power, and the air school's
// shove — so the bolt behaviour lives once, here: Cast() builds the party
// bolt (accuracy from the caster's intelligence + school skill, colour from
// the school) and spawns it through the services; MonsterBolt() is the same
// projectile thrown by a monster caster at ITS accuracy. A concrete bolt
// spell usually just constructs with its numbers — and overrides Cast() the
// day it grows behaviour beyond the flight (Flame igniting sconces, Splash
// filling vials: call the base, then add).
// ============================================================================
#pragma once

#include "Game/Spell/Spell.h"

namespace dungeon::game {

class BoltSpell : public Spell {
public:
	BoltSpell(std::string id, std::vector<SpellSymbol> sequence, float power,
			  float mana, float speed, float range, int push = 0);

	void Cast(CastContext& ctx) const override;
	std::optional<ProjectileSpec> MonsterBolt(const Vec3& origin,
											  const Vec3& dir,
											  float accuracy) const override;
	void ApplyOverrides(const CatalogEntry& e) override; // + speed/range/push

protected:
	// One bolt spec, shared by the party and monster doors — only the power,
	// accuracy, and target side differ between them.
	ProjectileSpec MakeBolt(const Vec3& origin, const Vec3& dir, float power,
							float accuracy, TargetSide target) const;

	float m_speed;  // travel speed (m/s)
	float m_range;  // reach (m) before it fizzles
	int m_push;     // cells a struck survivor is shoved (the air-school shove)
};

} // namespace dungeon::game
