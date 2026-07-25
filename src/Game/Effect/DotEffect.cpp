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
//
// The two apply lines are the ones each side has always printed; the SCHOOL is
// the HUD tint a source lends when it has no element of its own (the palette
// convention: poison green, bleeding red).
PoisonEffect::PoisonEffect()
	: DotEffect("poison", "effect.poison", DamageType::Earth) {
	m_school = SpellSymbol::Earth;
	m_applyParty = "log.poisoned";
	m_applyMonster = "log.monster_poisoned";
}

BleedEffect::BleedEffect()
	: DotEffect("bleed", "effect.bleed", DamageType::Pierce) {
	m_school = SpellSymbol::Fire;
	m_applyParty = "log.bleeding";
	m_applyMonster = "log.monster_bleeding";
}

BurnEffect::BurnEffect() : DotEffect("burn", "effect.burn", DamageType::Fire) {
	m_plume = true; // a burning body is visibly on fire
	m_applyParty = "log.member_ignites";
	m_applyMonster = "log.monster_ignites";
}

DamageType BurnEffect::DamageTypeOf(const Inst& inst) const {
	// Whatever lit it: a fire weapon burns, a water one chills.
	return SchoolDamageType(inst.school);
}

} // namespace dungeon::game::fx
