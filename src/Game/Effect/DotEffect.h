// ============================================================================
// Game/Effect/DotEffect.h — damage over time: poison, bleed, burn.
//
// The three share one behaviour — magnitude is damage per second, ticked while
// the timer runs — and differ only in what they are called, what they look
// like, and (in P2) the damage TYPE they deal, which is what a resist reads.
// So they share a base and separate only where they must:
//
//   poison — the blob's venom            (earth green by the tint convention)
//   bleed  — the lurker's opened wound   (fire red, ditto)
//   burn   — an enchanted weapon's fire  (its school's colour, and a plume)
//
// Until P2 the party's poison/bleed still tick in DungeonWorld's effect-aging
// loop and burn still lives in Monster's own slot; P3 is where burn becomes an
// instance like the others and the monster side stops being special.
// ============================================================================
#pragma once

#include "Game/Effect/Effect.h"

namespace dungeon::game::fx {

class DotEffect : public EffectKind {
public:
	DotEffect(std::string id, std::string nameKey);
};

class PoisonEffect : public DotEffect {
public:
	PoisonEffect();
};

class BleedEffect : public DotEffect {
public:
	BleedEffect();
};

class BurnEffect : public DotEffect {
public:
	BurnEffect();
};

} // namespace dungeon::game::fx
