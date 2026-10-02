// ============================================================================
// Game/Effect/SmokeEffect.h - the smoke a fire leaves when it goes out.
//
// Lands on the FIRE itself (DungeonWorld::Fire::effects) the moment it is put
// out, by whatever put it out - a splash, a smashed sconce - through the fixture
// kind's `on_douse = smoke <power> <seconds>` (fixtures.cat). It does nothing at
// any pipeline stage; it is HAZE (effects.cat `haze = 1`): while it lasts its
// square's air is thicker by `power` x the share of its time it has left, so it
// thins away to nothing over `seconds`. The world reads that off the effect
// list each frame, the way a burning body's plume is read off `plume`, so the
// list stays the one truth of what is happening.
// ============================================================================
#pragma once

#include "Game/Effect/Effect.h"

namespace dungeon::game::fx {

class SmokeEffect : public EffectKind {
public:
	SmokeEffect();
};

} // namespace dungeon::game::fx
