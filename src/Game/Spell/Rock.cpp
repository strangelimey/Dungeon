// ============================================================================
// Game/Spell/Rock.cpp - see Rock.h.
// ============================================================================
#include "Game/Spell/Rock.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"

namespace dungeon::game::spells {

Rock::Rock()
	: HandSpell("rock", {SpellSymbol::Earth}, /*power=*/8.0f, /*mana=*/2.0f),
	  m_conjures("pebble") {}

void Rock::Cast(CastContext& ctx) const {
	// The pebble comes out of a puff of dust.
	if (ctx.services.handPuff) ctx.services.handPuff(School(), ctx.origin, ctx.dir);
	for (const int h : LandingHands(ctx)) {
		ItemSlot& slot = ctx.caster.inventory.Hand(h);
		if (!slot.Empty()) continue;
		// Into the slot's own buffer (a hand's string keeps its capacity).
		slot.typeId.assign(m_conjures);
		slot.charge = kNoCharge;
		Say(ctx, loc::FormatLine("log.pebble_hand", ctx.caster.name));
		return;
	}
	ctx.services.dropAtFeet(m_conjures);
	Say(ctx, loc::FormatLine("log.pebble_feet", ctx.caster.name));
}

void Rock::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_conjures = e.Get("conjures", m_conjures);
}

} // namespace dungeon::game::spells
