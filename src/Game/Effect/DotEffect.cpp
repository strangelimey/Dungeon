// ============================================================================
// Game/Effect/DotEffect.cpp — see DotEffect.h.
// ============================================================================
#include "Game/Effect/DotEffect.h"

namespace dungeon::game::fx {

DotEffect::DotEffect(std::string id, std::string nameKey)
	: EffectKind(std::move(id), Category::Dot, std::move(nameKey),
				 Stacking::Refresh) {}

// No icon art yet — a DoT draws as its school-tinted square in the HUD strip,
// which is what these have always looked like. effects.cat `icon` adopts one
// the day the art exists, with no code change.
PoisonEffect::PoisonEffect() : DotEffect("poison", "effect.poison") {}
BleedEffect::BleedEffect() : DotEffect("bleed", "effect.bleed") {}
BurnEffect::BurnEffect() : DotEffect("burn", "effect.burn") {}

} // namespace dungeon::game::fx
