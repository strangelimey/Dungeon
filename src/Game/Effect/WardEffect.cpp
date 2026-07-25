// ============================================================================
// Game/Effect/WardEffect.cpp — see WardEffect.h.
// ============================================================================
#include "Game/Effect/WardEffect.h"

namespace dungeon::game::fx {

WardEffect::WardEffect(std::string id, std::string nameKey)
	: EffectKind(std::move(id), Category::Ward, std::move(nameKey),
				 Stacking::Refresh) {
	// A ward wears the Protect rune tablet's face in the HUD strip; the school
	// tint around it tells the four apart (effects.cat can override).
	m_iconItem = "rune_protect";
}

StoneskinEffect::StoneskinEffect() : WardEffect("stoneskin", "spell.stoneskin") {}
FireshieldEffect::FireshieldEffect() : WardEffect("fireshield", "spell.fireshield") {}
WaterveilEffect::WaterveilEffect() : WardEffect("waterveil", "spell.waterveil") {}
WindwardEffect::WindwardEffect() : WardEffect("windward", "spell.windward") {}

} // namespace dungeon::game::fx
