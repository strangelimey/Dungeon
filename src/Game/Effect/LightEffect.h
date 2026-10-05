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
	// FIRE (lighting-updates Phase 6c): the power a light needs to kindle a
	// brazier (a sconce takes any), and its scorch - every `scorch_every`
	// seconds, `scorch_damage` x (power / scale_power) of fire on each monster
	// beside the party.
	float KindleBrazierPower() const { return m_kindleBrazierPower; }
	float ScorchEvery() const { return m_scorchEvery; }
	float ScorchDamage() const { return m_scorchDamage; }
	// WATER (6d): how much faster stamina comes back in it (`soothe`, 1 = twice
	// as fast), and how much haze it clears round the party (`clear_haze`, the
	// turbidity it takes away at the centre).
	float Soothe() const { return m_soothe; }
	float ClearHaze() const { return m_clearHaze; }
	// AIR (6e): every `crackle_every` seconds a shock of `crackle_damage` x
	// (power / scale_power) at the nearest monster in its reach and sight; while
	// a monster near has noticed the party its flicker runs `warn_rate` x fast.
	float CrackleEvery() const { return m_crackleEvery; }
	float CrackleDamage() const { return m_crackleDamage; }
	float WarnRate() const { return m_warnRate; }
	// EARTH (6f): the item whose model the set-down stone draws as (`stone_item`,
	// glowing in the light's colour; absent from the project = the light alone).
	std::string_view StoneItem() const { return m_stoneItem; }

private:
	// Indexed by school (Fire, Earth, Air, Water - the SpellSymbol order).
	std::array<std::string, 4> m_schoolNames;
	float m_scalePower = 8.0f;
	float m_kindleBrazierPower = 14.0f;
	float m_scorchEvery = 1.5f;
	float m_scorchDamage = 1.5f;
	float m_soothe = 1.0f;
	float m_clearHaze = 2.0f;
	float m_crackleEvery = 2.0f;
	float m_crackleDamage = 2.0f;
	float m_warnRate = 3.0f;
	std::string m_stoneItem = "rock";
};

class DazzleEffect : public EffectKind {
public:
	DazzleEffect();
};

} // namespace dungeon::game::fx
