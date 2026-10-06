// ============================================================================
// Game/Spell/Gust.h - Puff of Wind (Ansuz alone): a breeze from the hand.
//
// NOT a bolt (spell-updates, Michael): it fans the wall torch or brazier the
// party faces into a brief flare, and at `push_power` (spells.cat) or more it
// also shoves back whatever stands in the square ahead (`push` squares) and
// meets the shots flying at the party - a gust of its power weakens a stronger
// shot, its blast and what it leaves burning with it, and flings back one it
// outweighs (ProjectileSystem::Repel). "Other than that, it's not much use."
// ============================================================================
#pragma once

#include "Game/Spell/HandSpell.h"

namespace dungeon::game::spells {

class Gust : public HandSpell {
public:
	Gust();
	void Cast(CastContext& ctx) const override;
	void ApplyOverrides(const CatalogEntry& e) override; // + push_power, push

private:
	float m_pushPower;
	int m_pushCells;
};

} // namespace dungeon::game::spells
