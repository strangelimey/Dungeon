// ============================================================================
// Game/Effect/DotEffect.h — damage over time: poison, bleed, burn.
//
// The three share one behaviour — magnitude is damage per second, ticked while
// the timer runs — and differ only in what they are called, what they look
// like, and the damage TYPE they deal, which is what a resist reads. So they
// share a base and separate only where they must:
//
//   poison — the blob's venom            (earth green by the tint convention)
//   bleed  — the lurker's opened wound   (fire red, ditto)
//   burn   — an enchanted weapon's fire  (its school's colour, and a plume)
//
// The bite is not a hook here: DungeonWorld::TickEffects ages every list,
// member, monster and breakable alike, sums each DoT's RAW magnitude by its
// DamageTypeOf and deals each type as one fx::DamageEvent::Tick - resisted as
// it bites, so a ward raised mid-burn helps at once. Starving and parched
// (SupplyEffect.h) are DoTs on this base too, held open by their meter
// instead of a timer.
// ============================================================================
#pragma once

#include "Game/Effect/Effect.h"

namespace dungeon::game::fx {

class DotEffect : public EffectKind {
public:
	// `typeId` names a damagetypes.cat entry rather than an enumerator: these
	// classes are constructed before any project is loaded, so a DoT can only
	// say what it burns AS and let ApplyOverrides resolve which index that is.
	DotEffect(std::string id, std::string nameKey, std::string typeId);
};

// Venom in the blood — earth's nature damage. (Its HUD tint is earth green
// too, but that is a coincidence of the palette convention, not the reason.)
class PoisonEffect : public DotEffect {
public:
	PoisonEffect();
};

// An open wound: PIERCE damage, even though it rides fire red in the HUD.
// Which is exactly why a DoT's damage type is authored rather than derived
// from its school.
class BleedEffect : public DotEffect {
public:
	BleedEffect();
};

// Alight. The one DoT whose damage type is PER INSTANCE: a burn lit by a fire
// weapon is fire, one lit by a frost weapon is water — same kind, same plume
// (recoloured), different thing to resist.
class BurnEffect : public DotEffect {
public:
	BurnEffect();
	DamageType DamageTypeOf(const Inst& inst) const override;
};

} // namespace dungeon::game::fx
