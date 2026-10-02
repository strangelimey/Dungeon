// ============================================================================
// Game/Spell/Splash.cpp - see Splash.h.
// ============================================================================
#include "Game/Spell/Splash.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"

namespace dungeon::game::spells {

Splash::Splash()
	: HandSpell("splash", {SpellSymbol::Water}, /*power=*/7.0f, /*mana=*/2.0f),
	  m_brazierPower(12.0f) {}

void Splash::Cast(CastContext& ctx) const {
	std::array<int, 2> hands{};
	const int n = OtherHands(ctx, hands);
	for (int i = 0; i < n; ++i) {
		ItemSlot& slot = ctx.caster.inventory.Hand(hands[static_cast<size_t>(i)]);
		if (const std::string_view now = ctx.services.fillItem(slot); !now.empty()) {
			Say(ctx, loc::FormatLine("log.splash_fill", ctx.caster.name, loc::View(now)));
			return;
		}
	}
	const FireAhead f = ctx.services.fireAhead();
	if (f.kind == FireAhead::Kind::WallTorch && f.lit) {
		ctx.services.setFireAhead(false);
		Say(ctx, loc::FormatLine("log.splash_torch"));
		return;
	}
	if (f.kind == FireAhead::Kind::Brazier && f.lit) {
		if (ctx.power >= m_brazierPower) {
			ctx.services.setFireAhead(false);
			Say(ctx, loc::FormatLine("log.splash_brazier"));
		} else {
			Say(ctx, loc::FormatLine("log.splash_brazier_weak"));
		}
		return;
	}
	Say(ctx, loc::FormatLine("log.splash_nothing", ctx.caster.name));
}

void Splash::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_brazierPower = e.GetFloat("brazier_power", m_brazierPower);
}

} // namespace dungeon::game::spells
