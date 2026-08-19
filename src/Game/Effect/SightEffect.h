// ============================================================================
// Game/Effect/SightEffect.h — the see-through mark left by the Sight form.
//
// One kind, four flavours. Unlike the wards, the four Sight spells don't
// behave differently HERE — the mark carries no numbers and does nothing to
// its bearer. The world reads its presence and branches on the school when it
// bores the peephole through the wall ahead (DungeonWorld::UpdateLights:
// fire lights the room beyond, air tunnels deeper, earth maps it, water opens
// a wider window). So one class, and the four spells are told apart by the
// school on the instance — which is also how it NAMES itself, so the sheet
// still shows "Ember Sight" and its description.
//
// Stacking is per-school: a member may hold several sights at once and a
// same-school recast refreshes only its own (the rule the cast site used to
// spell out by hand).
// ============================================================================
#pragma once

#include "Game/Effect/Effect.h"

#include <array>

namespace dungeon::game::fx {

class SightEffect : public EffectKind {
public:
	SightEffect();

	// "spell.embersight" / "farsight" / "stonesight" / "scrying" by school —
	// a view into m_schoolNames, never a temporary (the HUD calls it per frame).
	std::string_view NameKey(const Inst& inst) const override;

	void ApplyOverrides(const CatalogEntry& e, const DamageTypeBook& types) override;

private:
	// Indexed by school (Fire, Earth, Air, Water — the SpellSymbol order).
	std::array<std::string, 4> m_schoolNames;
};

} // namespace dungeon::game::fx
