// ============================================================================
// Game/Spell/Flame.h - Puff of Flame (Kenaz alone): a puff of flame in the hand.
//
// NOT a bolt (spell-updates, Michael): it lights things, in this order -
//   * an unlit torch in the OTHER hand (its `lit_as`),
//   * else the unlit wall torch the party faces,
//   * else the unlit brazier ahead - but only at `brazier_power` (spells.cat):
//     a bowl of coals takes more than a flicker to catch.
// Otherwise it flickers and goes out. It does no harm to anyone.
// ============================================================================
#pragma once

#include "Game/Spell/HandSpell.h"

namespace dungeon::game::spells {

class Flame : public HandSpell {
public:
	Flame();
	void Cast(CastContext& ctx) const override;
	void ApplyOverrides(const CatalogEntry& e) override; // + brazier_power

private:
	float m_brazierPower;
};

} // namespace dungeon::game::spells
