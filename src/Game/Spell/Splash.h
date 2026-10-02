// ============================================================================
// Game/Spell/Splash.h - Splash (Laguz alone): a handful of water.
//
// NOT a bolt (spell-updates, Michael): in this order -
//   * a container in the OTHER hand takes some water (its `fill_as`: an empty
//     waterskin becomes a half-full one, a half-full one full),
//   * else the burning wall torch the party faces is put out,
//   * else the burning brazier ahead - only at `brazier_power` (spells.cat).
// A fire put out leaves its smoke (its kind's on_douse, through the effects
// system). Otherwise the water just splashes.
// ============================================================================
#pragma once

#include "Game/Spell/HandSpell.h"

namespace dungeon::game::spells {

class Splash : public HandSpell {
public:
	Splash();
	void Cast(CastContext& ctx) const override;
	void ApplyOverrides(const CatalogEntry& e) override; // + brazier_power

private:
	float m_brazierPower;
};

} // namespace dungeon::game::spells
