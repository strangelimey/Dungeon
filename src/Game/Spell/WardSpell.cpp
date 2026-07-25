// ============================================================================
// Game/Spell/WardSpell.cpp — see WardSpell.h.
// ============================================================================
#include "Game/Spell/WardSpell.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"

namespace dungeon::game {

WardSpell::WardSpell(std::string id, std::vector<SpellSymbol> sequence,
					 float power, float mana, float duration)
	: Spell(std::move(id), std::move(sequence), power, mana),
	  m_duration(duration) {}

void WardSpell::Cast(CastContext& ctx) const {
	// The ward wraps the CASTER. Each school's ward is its OWN effect kind,
	// sharing this spell's id — so wards still STACK across schools (all four
	// may be up at once) while a recast refreshes only its own. The stacking
	// rule lives in the kind now, not in a RemoveWard call here.
	ctx.services.applyEffect(ctx.caster, Id(), School(), ctx.power, m_duration);
	ctx.services.message(ctx.caster,
						 loc::Format("log.shield_up", ctx.caster.name));
}

void WardSpell::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_duration = e.GetFloat("duration", m_duration);
}

} // namespace dungeon::game
