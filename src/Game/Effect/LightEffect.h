// ============================================================================
// Game/Effect/LightEffect.h - the light a Sowilo spell leaves on its caster,
// and the dazzle its flare leaves on a monster (lighting-updates Phase 6).
//
// LIGHT: one kind, four flavours, as Sight is. The mark does nothing to its
// bearer; the world reads it each frame and lights the party from the school's
// profile, sized by the instance's magnitude (the cast's power) against
// `scale_power`, and does what that school's light does beyond its colour
// (DungeonWorld_SpellLight.cpp). Stacks per school: a recast refreshes its own.
// It names itself by school ("spell.firelight" ...), so the sheet shows the
// spell that cast it and finds its description.
//
// DAZZLE: a monster blinded by a flare. A marker too - the monster tick reads
// it and the monster does nothing while it lasts (it still animates and can be
// hit), the same seam the eval harness's `freeze` uses.
// ============================================================================
#pragma once

#include "Game/Effect/Effect.h"

#include <array>

namespace dungeon::game::fx {

class LightEffect : public EffectKind {
public:
	LightEffect();

	std::string_view NameKey(const Inst& inst) const override;
	void ApplyOverrides(const CatalogEntry& e, const DamageTypeBook& types) override;

	// The power at which the light is its profile's own size; a stronger one is
	// bigger (as the square root of the ratio, within limits).
	float ScalePower() const { return m_scalePower; }

private:
	// Indexed by school (Fire, Earth, Air, Water - the SpellSymbol order).
	std::array<std::string, 4> m_schoolNames;
	float m_scalePower = 8.0f;
};

class DazzleEffect : public EffectKind {
public:
	DazzleEffect();
};

} // namespace dungeon::game::fx
