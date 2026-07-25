// ============================================================================
// Game/Spell/SightSpell.cpp — see SightSpell.h.
// ============================================================================
#include "Game/Spell/SightSpell.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"

namespace dungeon::game {

SightSpell::SightSpell(std::string id, std::vector<SpellSymbol> sequence,
					   float power, float mana, float duration)
	: Spell(std::move(id), std::move(sequence), power, mana),
	  m_duration(duration) {}

void SightSpell::Cast(CastContext& ctx) const {
	// The peek marks the CASTER; the world reads any active Sight to ghost the
	// wall ahead. All four sight spells share ONE effect kind (they differ only
	// in flavour, which the school carries) and it stacks per school — so a
	// recast refreshes just its own entry, the rule the kind now owns.
	ctx.services.applyEffect(ctx.caster, "sight", School(), ctx.power, m_duration);
	ctx.services.message(ctx.caster,
						 loc::Format("log.sight_up", ctx.caster.name));
}

void SightSpell::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_duration = e.GetFloat("duration", m_duration);
}

} // namespace dungeon::game
