// ============================================================================
// Game/Spell/HandSpell.h - the shared tier-1 form: a small thing IN THE HAND.
//
// One school rune on its own is not a weapon (spell-updates, Michael): it is a
// puff of flame, a pebble, a breeze, a handful of water, cast from the hand and
// working on what is in the OTHER hand or on what the party faces. The four
// spells (Flame, Rock, Gust, Splash) each say what that is in their Cast(); the
// hand arithmetic they share lives here, once.
// ============================================================================
#pragma once

#include "Game/Spell/Spell.h"

#include <array>

namespace dungeon::game {

class HandSpell : public Spell {
public:
	using Spell::Spell;

protected:
	// The hands a spell working on "the other hand" looks in, in order: the one
	// that did not cast it - or, for a cast from no hand (the spellbook, the
	// console), both, right first. Returns how many of `out` it filled.
	static int OtherHands(const CastContext& ctx, std::array<int, 2>& out);
	// The hands a conjured item may land in, in order: the casting hand, then
	// the other (right, then left, for a cast from no hand).
	static std::array<int, 2> LandingHands(const CastContext& ctx);
	// A line about the caster, through the cast services.
	static void Say(const CastContext& ctx, std::string_view line);
};

} // namespace dungeon::game
