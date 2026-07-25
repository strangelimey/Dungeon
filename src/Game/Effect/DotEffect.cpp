// ============================================================================
// Game/Effect/DotEffect.cpp — see DotEffect.h.
// ============================================================================
#include "Game/Effect/DotEffect.h"

namespace dungeon::game::fx {

DotEffect::DotEffect(std::string id, std::string nameKey, DamageType type)
	: EffectKind(std::move(id), Category::Dot, std::move(nameKey),
				 Stacking::Refresh) {
	m_damageType = type;
}

// No icon art yet — a DoT draws as its school-tinted square in the HUD strip,
// which is what these have always looked like. effects.cat `icon` adopts one
// the day the art exists, with no code change.
PoisonEffect::PoisonEffect()
	: DotEffect("poison", "effect.poison", DamageType::Earth) {}

BleedEffect::BleedEffect()
	: DotEffect("bleed", "effect.bleed", DamageType::Pierce) {}

BurnEffect::BurnEffect() : DotEffect("burn", "effect.burn", DamageType::Fire) {
	m_plume = true; // a burning body is visibly on fire
}

DamageType BurnEffect::DamageTypeOf(const Inst& inst) const {
	// Whatever lit it: a fire weapon burns, a water one chills.
	return SchoolDamageType(inst.school);
}

} // namespace dungeon::game::fx
