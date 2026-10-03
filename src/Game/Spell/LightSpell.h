// ============================================================================
// Game/Spell/LightSpell.h - the shared Light form: Sowilo, "make light"
// (lighting-updates Phase 6, docs/lighting-updates-plan.md).
//
// A school rune + Sowilo is a LIGHT. The four land the same way - a `light`
// effect on the CASTER, magnitude = the cast's power, its school on the
// instance - so the landing lives once, here, as Sight's does. The effect is a
// MARKER the world reads each frame: it lights the party from the school's
// lights.cat profile (`spell_<school>`), bigger with the power, dimming over its
// last tenth like a torch, and does what that school's light does beyond its
// colour (DungeonWorld_SpellLight.cpp - Michael's answers: fire kindles and
// scorches, water clears the haze, soothes and quenches, air crackles and warns,
// earth is SET DOWN as a stone instead, which is why Cast is virtual).
//
// Its DURATION grows with the power too: `duration` seconds at the spell's own
// power, more for a stronger caster. Lights of different schools stack; a recast
// of the same school replaces its own (the effect kind's stacking).
//
// The modifiers (ModifiedSpell): Ingwaz casts ONE BIGGER LIGHT - the same light
// at `grow` x the power for the duration the plain power would buy - and
// Hagalaz a DAZZLING FLARE: no lasting light, a flash round the party that
// dazzles the monsters near and does the school's thing once (Flare).
// ============================================================================
#pragma once

#include "Game/Spell/Spell.h"

namespace dungeon::game {

class LightSpell : public Spell {
public:
	LightSpell(std::string id, std::vector<SpellSymbol> sequence, float power, float mana,
			   float duration);

	void Cast(CastContext& ctx) const override;
	void ApplyOverrides(const CatalogEntry& e) override; // + duration

	// The light at `power` (its size and what it does), lasting what
	// `durationPower` buys - the plain cast passes the same power twice, Ingwaz a
	// grown one and the plain one.
	virtual void LightOn(CastContext& ctx, float power, float durationPower) const;
	// Hagalaz: the flare at `power` (CastServices::lightFlare).
	void Flare(CastContext& ctx, float power) const;

protected:
	// Seconds `power` keeps the light: `duration` at the spell's own power,
	// in proportion beyond it (and never under half).
	float DurationFor(float power) const;

	float m_duration; // seconds at the spell's own power
};

} // namespace dungeon::game
