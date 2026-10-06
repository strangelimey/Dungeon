// ============================================================================
// Game/Spell/WardSpell.h — the shared Protect form: a ward on the caster.
//
// The four Protect-form spells land the same way - the ward kind sharing the
// spell's id (fx::StoneskinEffect ...) on the CASTER through CastServices::
// applyEffect, magnitude = the skill-scaled power, replacing only that kind's
// existing ward (wards stack across schools) - so the landing lives once,
// here. What the ward then DOES is its effect class's pipeline hook
// (Effect/WardEffect.h: earth = physical resist, fire = retaliation, water =
// absorb pool, air = deflect charges; see docs/spells.md "Protect"). A
// concrete ward spell just constructs with its numbers.
// ============================================================================
#pragma once

#include "Game/Spell/Spell.h"

namespace dungeon::game {

class WardSpell : public Spell {
public:
	WardSpell(std::string id, std::vector<SpellSymbol> sequence, float power,
			  float mana, float duration);

	void Cast(CastContext& ctx) const override;
	void ApplyOverrides(const CatalogEntry& e) override; // + duration

	// This ward, at `power`, on `target` - the caster for a plain cast, every
	// standing member for a ward cast with Ingwaz (ModifiedSpell).
	void WardOn(CastContext& ctx, Character& target, float power) const;

protected:
	float m_duration; // ward lifetime in seconds (water/air may SPEND out early)
};

} // namespace dungeon::game
