// ============================================================================
// Game/Effect/SupplyEffect.cpp — see SupplyEffect.h.
// ============================================================================
#include "Game/Effect/SupplyEffect.h"

namespace dungeon::game::fx {

// Both deal the `starve` damage type — its own entry in damagetypes.cat, not
// physical and belonging to no school, so by default NOTHING resists it and
// nothing is immune. That is deliberate: armour should not answer hunger, and a
// fire-drinking monster should not be fed by it either. A project that wants a
// creature which does not eat can still author `starve 1.0` on it, which is the
// right way to say that and reads as exactly what it means.
//
// Stacking is Refresh, the house rule, and it matters more than usual here: the
// supply tick re-applies while the meter is empty, so anything that piled up
// would multiply the bite every frame.

StarvingEffect::StarvingEffect()
	: DotEffect("starving", "effect.starving", "starve") {
	m_school = SpellSymbol::Earth; // the HUD tint; hunger has no element
	m_applyParty = "log.starving";
	m_applyMonster = "log.monster_starving";
}

ParchedEffect::ParchedEffect() : DotEffect("parched", "effect.parched", "starve") {
	m_school = SpellSymbol::Water; // thirst reads blue, for what it is missing
	m_applyParty = "log.parched";
	m_applyMonster = "log.monster_parched";
}

} // namespace dungeon::game::fx
