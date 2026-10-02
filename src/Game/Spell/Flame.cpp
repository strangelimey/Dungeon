// ============================================================================
// Game/Spell/Flame.cpp - see Flame.h.
// ============================================================================
#include "Game/Spell/Flame.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"

namespace dungeon::game::spells {

Flame::Flame()
	: HandSpell("flame", {SpellSymbol::Fire}, /*power=*/8.0f, /*mana=*/2.0f),
	  m_brazierPower(14.0f) {}

void Flame::Cast(CastContext& ctx) const {
	// A torch in the other hand first: the flame is right there.
	std::array<int, 2> hands{};
	const int n = OtherHands(ctx, hands);
	for (int i = 0; i < n; ++i) {
		ItemSlot& slot = ctx.caster.inventory.Hand(hands[static_cast<size_t>(i)]);
		if (const std::string_view lit = ctx.services.lightItem(slot); !lit.empty()) {
			Say(ctx, loc::FormatLine("log.flame_held", ctx.caster.name, loc::View(lit)));
			return;
		}
	}
	// Then what the party faces.
	const FireAhead f = ctx.services.fireAhead();
	if (f.kind == FireAhead::Kind::WallTorch && f.canBurn && !f.lit) {
		ctx.services.setFireAhead(true);
		Say(ctx, loc::FormatLine("log.flame_torch"));
		return;
	}
	if (f.kind == FireAhead::Kind::Brazier && f.canBurn && !f.lit) {
		if (ctx.power >= m_brazierPower) {
			ctx.services.setFireAhead(true);
			Say(ctx, loc::FormatLine("log.flame_brazier"));
		} else {
			Say(ctx, loc::FormatLine("log.flame_brazier_weak"));
		}
		return;
	}
	Say(ctx, loc::FormatLine("log.flame_nothing", ctx.caster.name));
}

void Flame::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_brazierPower = e.GetFloat("brazier_power", m_brazierPower);
}

} // namespace dungeon::game::spells
