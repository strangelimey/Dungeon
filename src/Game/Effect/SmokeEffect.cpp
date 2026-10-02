// ============================================================================
// Game/Effect/SmokeEffect.cpp - see SmokeEffect.h.
// ============================================================================
#include "Game/Effect/SmokeEffect.h"

namespace dungeon::game::fx {

SmokeEffect::SmokeEffect() : EffectKind("smoke", Category::Marker, "effect.smoke") {
	m_haze = true; // what it IS: air thickened by what is left of it
}

} // namespace dungeon::game::fx
