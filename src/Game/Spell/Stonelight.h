// ============================================================================
// Game/Spell/Stonelight.h - Stonelight (earth,light): a steady amber stone SET DOWN where it is cast
// rather than following the party; it LASTS LONGEST, MAPS what it shows and
// SHOWS the TRACKS monsters have left (Michael, lighting-updates Phase 6).
// So it overrides LightOn: nothing lands on the caster, the stone goes down
// through the `placeLightStone` cast service (DungeonWorld_SpellLight.cpp).
// ============================================================================
#pragma once

#include "Game/Spell/LightSpell.h"

namespace dungeon::game::spells {

class Stonelight : public LightSpell {
public:
	Stonelight();

	void LightOn(CastContext& ctx, float power, float durationPower) const override;
};

} // namespace dungeon::game::spells
