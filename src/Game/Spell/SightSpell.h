// ============================================================================
// Game/Spell/SightSpell.h — the shared Sight form: see through the wall ahead.
//
// The four Sight-form spells land the same way — a StatusEffect of kind Sight
// on the CASTER, magnitude = the skill-scaled power, replacing only that
// SCHOOL's existing Sight (Sights stack across schools) — so the landing lives
// once, here. The effect is a MARKER: the world (DungeonWorld) reads any active
// Sight and ghosts the wall block directly ahead in the first-person view; the
// SCHOOL keys what the peek shows (fire lights the beyond, air sees deeper,
// earth maps + lingers, water reveals the hidden — see docs/spells.md "Sight").
// A concrete Sight spell just constructs with its numbers.
// ============================================================================
#pragma once

#include "Game/Spell/Spell.h"

namespace dungeon::game {

class SightSpell : public Spell {
public:
	SightSpell(std::string id, std::vector<SpellSymbol> sequence, float power,
			   float mana, float duration);

	void Cast(CastContext& ctx) const override;
	void ApplyOverrides(const CatalogEntry& e) override; // + duration

protected:
	float m_duration; // seconds the peek stays open
};

} // namespace dungeon::game
