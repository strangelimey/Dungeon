// ============================================================================
// Game/Effect/AllEffects.cpp — the effect registry: every concrete kind,
// constructed at its class defaults. EffectBook::Build starts from this list,
// then lays the project's effects.cat overrides on top. Adding an effect: its
// class in this folder, one line here, its term in the ceiling's count below
// when a party member can carry it, one in Game's CMakeLists, and the
// effect.<id> lang keys ×5.
// (MakeAllEffects is declared in Effect.h — no separate header for one list.)
// ============================================================================
#include "Game/Effect/Effect.h"

#include "Game/Effect/DotEffect.h"
#include "Game/Effect/LightEffect.h"
#include "Game/Effect/SightEffect.h"
#include "Game/Effect/SmokeEffect.h"
#include "Game/Effect/SupplyEffect.h"
#include "Game/Effect/WardEffect.h"
#include "Game/Resource.h" // resource::Supply (one deprivation effect per meter)
#include "Game/Spells.h"   // kSchoolCount

#include <memory>

namespace dungeon::game::fx {

// --- the ceiling, counted -----------------------------------------------------
// The most instances ONE bearer can hold in play, which is what kMaxEffects has
// to cover (an Apply past it evicts). The worst bearer is a PARTY MEMBER: a
// monster carries no supply, sight or light effect (a dazzle is its only extra),
// and a fire or a breakable only smoke and what burns it. Each term is a kind
// above that a member can carry, counted the way its stacking policy lets it
// pile up: a Refresh kind once, a RefreshPerSchool kind once per school it can
// land in. A new kind a member can carry adds its term here, and this fails to
// compile when the sum reaches past the ceiling - rather than an effect being
// quietly evicted mid-fight. It is only as good as that habit: a kind added
// below WITHOUT its term leaves this green, which is why EffectBook::Build
// also bounds every kind registered here (looser - smoke, dazzle and earth's
// light count too - but needing nobody to remember it).
namespace {
// One ward kind per school, each refreshing only itself - all four at once.
constexpr size_t kWardsHeld = kSchoolCount;
// poison, bleed, burn: one each (Refresh), whatever lit or landed them.
constexpr size_t kDotsHeld = 3;
// starving / parched: one per supply meter, held open while it is empty.
constexpr size_t kSupplyHeld = static_cast<size_t>(resource::Supply::Count);
// sight stacks per school (one peephole flavour each).
constexpr size_t kSightsHeld = kSchoolCount;
// light stacks per school, but EARTH's is not carried: Stonelight sets a stone
// down in the square instead (Spell/Stonelight.cpp, placeLightStone).
constexpr size_t kLightsHeld = kSchoolCount - 1;

constexpr size_t kWorstCaseHeld =
	kWardsHeld + kDotsHeld + kSupplyHeld + kSightsHeld + kLightsHeld;
} // namespace
static_assert(kWorstCaseHeld <= kMaxEffects,
			  "a party member can carry more effects than fx::kMaxEffects - Apply "
			  "would evict one in play; raise the ceiling (Effect.h)");

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
	// A doused fire's smoke, borne by the fire itself (haze).
	all.push_back(std::make_unique<SmokeEffect>());
	// A Sowilo light on its caster (one per school; earth's is set down as a
	// stone instead), and the dazzle its flare leaves on a monster
	// (lighting-updates Phase 6).
	all.push_back(std::make_unique<LightEffect>());
	all.push_back(std::make_unique<DazzleEffect>());
	return all;
}

} // namespace dungeon::game::fx
