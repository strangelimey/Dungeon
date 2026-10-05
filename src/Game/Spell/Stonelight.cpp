// ============================================================================
// Game/Spell/Stonelight.cpp - see Stonelight.h.
// ============================================================================
#include "Game/Spell/Stonelight.h"

#include "Core/Loc.h"
#include "Game/Character.h"

namespace dungeon::game::spells {

Stonelight::Stonelight()
	: LightSpell("stonelight", {SpellSymbol::Earth, SpellSymbol::Light}, /*power=*/8.0f,
				 /*mana=*/5.0f, /*duration=*/150.0f) {}

void Stonelight::LightOn(CastContext& ctx, float power, float durationPower) const {
	// A host that cannot set a stone down (none today) still gets a light.
	if (!ctx.services.placeLightStone) {
		LightSpell::LightOn(ctx, power, durationPower);
		return;
	}
	ctx.services.placeLightStone(power, DurationFor(durationPower));
	ctx.services.message(ctx.caster, loc::FormatLine("log.light_stone", ctx.caster.name));
}

} // namespace dungeon::game::spells
