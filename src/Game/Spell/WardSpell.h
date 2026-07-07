// ============================================================================
// Game/Spell/WardSpell.h — the shared Protect form: a ward on the caster.
//
// The four Protect-form spells land the same way — a StatusEffect ward on the
// CASTER, magnitude = the skill-scaled power, replacing only that SCHOOL's
// existing ward (wards stack across schools) — so the landing lives once,
// here. What the ward then DOES is the school's business, keyed off the
// effect by the status-effect system (earth = armor, fire = retaliation,
// water = absorb pool, air = deflect charges; see docs/spells.md "Protect").
// A concrete ward spell just constructs with its numbers.
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

protected:
	float m_duration; // ward lifetime in seconds (water/air may SPEND out early)
};

} // namespace dungeon::game
