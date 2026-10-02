// ============================================================================
// Game/Spell/Rock.h - Pebble (Berkano alone): a small stone in the hand.
//
// NOT a bolt (spell-updates, Michael): it conjures `conjures` (spells.cat,
// default the throwable `pebble`) into the casting hand if that is empty, else
// the other; with both full it drops at the caster's feet.
// ============================================================================
#pragma once

#include "Game/Spell/HandSpell.h"

namespace dungeon::game::spells {

class Rock : public HandSpell {
public:
	Rock();
	void Cast(CastContext& ctx) const override;
	void ApplyOverrides(const CatalogEntry& e) override; // + conjures

private:
	std::string m_conjures;
};

} // namespace dungeon::game::spells
