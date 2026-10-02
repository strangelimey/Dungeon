// ============================================================================
// Game/Spell/Gust.cpp - see Gust.h.
// ============================================================================
#include "Game/Spell/Gust.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"

namespace dungeon::game::spells {

Gust::Gust()
	: HandSpell("gust", {SpellSymbol::Air}, /*power=*/5.0f, /*mana=*/2.0f),
	  m_pushPower(8.0f), m_pushCells(1) {}

void Gust::Cast(CastContext& ctx) const {
	bool did = false;
	if (ctx.services.flareAhead()) {
		Say(ctx, loc::FormatLine("log.gust_flare"));
		did = true;
	}
	if (ctx.power >= m_pushPower) {
		if (ctx.services.shoveAhead(m_pushCells)) {
			Say(ctx, loc::FormatLine("log.gust_shove"));
			did = true;
		}
		const ProjectileSystem::Repelled r = ctx.services.repelAhead(ctx.power, ctx.casterIndex);
		if (r.turned > 0) Say(ctx, loc::FormatLine("log.gust_repel"));
		if (r.weakened > 0) Say(ctx, loc::FormatLine("log.gust_slow"));
		did |= r.turned + r.weakened > 0;
	}
	if (!did) Say(ctx, loc::FormatLine("log.gust_nothing", ctx.caster.name));
}

void Gust::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_pushPower = e.GetFloat("push_power", m_pushPower);
	m_pushCells = static_cast<int>(e.GetFloat("push", static_cast<float>(m_pushCells)));
}

} // namespace dungeon::game::spells
