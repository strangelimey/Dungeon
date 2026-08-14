// ============================================================================
// Game/Effect/AllEffects.cpp — the effect registry: every concrete kind,
// constructed at its class defaults. EffectBook::Build starts from this list,
// then lays the project's effects.cat overrides on top. Adding an effect: its
// class in this folder, one line here, one in Game's CMakeLists, and the
// effect.<id> lang keys ×5.
// (MakeAllEffects is declared in Effect.h — no separate header for one list.)
// ============================================================================
#include "Game/Effect/Effect.h"

#include "Game/Effect/DotEffect.h"
#include "Game/Effect/SightEffect.h"
#include "Game/Effect/SupplyEffect.h"
#include "Game/Effect/WardEffect.h"

#include <memory>

namespace dungeon::game::fx {

std::vector<std::unique_ptr<EffectKind>> MakeAllEffects() {
	std::vector<std::unique_ptr<EffectKind>> all;
	// The Protect wards — one per school, four different guards.
	all.push_back(std::make_unique<StoneskinEffect>());
	all.push_back(std::make_unique<FireshieldEffect>());
	all.push_back(std::make_unique<WaterveilEffect>());
	all.push_back(std::make_unique<WindwardEffect>());
	// Damage over time.
	all.push_back(std::make_unique<PoisonEffect>());
	all.push_back(std::make_unique<BleedEffect>());
	all.push_back(std::make_unique<BurnEffect>());
	// An empty supply meter (docs/health-and-healing.md). DoTs like the three
	// above, but held open by the METER rather than by a timer.
	all.push_back(std::make_unique<StarvingEffect>());
	all.push_back(std::make_unique<ParchedEffect>());
	// The see-through mark (all four Sight spells).
	all.push_back(std::make_unique<SightEffect>());
	return all;
}

} // namespace dungeon::game::fx
