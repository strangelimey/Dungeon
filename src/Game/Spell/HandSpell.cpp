// ============================================================================
// Game/Spell/HandSpell.cpp - see HandSpell.h.
// ============================================================================
#include "Game/Spell/HandSpell.h"

namespace dungeon::game {

int HandSpell::OtherHands(const CastContext& ctx, std::array<int, 2>& out) {
	if (ctx.hand == 0 || ctx.hand == 1) {
		out[0] = 1 - ctx.hand;
		return 1;
	}
	out = {1, 0};
	return 2;
}

std::array<int, 2> HandSpell::LandingHands(const CastContext& ctx) {
	if (ctx.hand == 0 || ctx.hand == 1) return {ctx.hand, 1 - ctx.hand};
	return {1, 0};
}

void HandSpell::Say(const CastContext& ctx, std::string_view line) {
	if (ctx.services.message) ctx.services.message(ctx.caster, line);
}

} // namespace dungeon::game
