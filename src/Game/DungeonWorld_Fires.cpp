// ============================================================================
// Game/DungeonWorld_Fires.cpp - fires as LIVE state: a wall torch or brazier lit
// or put out in play, a gust fanning one, and the smoke a fire leaves when it
// goes out. The authored `lit` on the map record never changes here (DungeonMap
// WallSconce::Burning); the save keeps the difference.
//
// THE SMOKE GOES THROUGH THE EFFECTS SYSTEM (Michael): a fire put out takes its
// kind's on_douse effects (fixtures.cat, `smoke <power> <seconds>`) onto its own
// effect list, and the haze over its square is READ OFF that list each frame -
// power x the share of its time left, so it thins to nothing - the way a
// burning body's plume is read off `plume`. Nothing here stores a puff.
//
// Everything here can run inside a cast, a frame the steady-state allocation
// guard watches: the flame is re-lit in its reserved buffer (FireEffect::
// Ignite), the effect list was reserved at build, the haze grid is refilled in
// place and copied into its texture by the next scene pass
// (RefreshTurbidityGrid / Texture::UpdateLevel0), and the puffs ride the frame
// constants.
// ============================================================================
#include "Game/DungeonWorld.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// How long a flare takes to die away.
constexpr float kFlareSeconds = 1.2f;
// How far a doused fire's smoke spreads from its square's centre, in squares.
constexpr float kSmokeRadiusCells = 1.5f;
} // namespace

DungeonWorld::Fire* DungeonWorld::FindFire(int x, int z, int wall) {
	for (Fire& f : m_fires)
		if (f.x == x && f.z == z && f.wall == wall && f.brazier == (wall < 0)) return &f;
	return nullptr;
}

bool DungeonWorld::SetFireBurning(int x, int z, int wall, bool burning, bool smoke) {
	Fire* fire = FindFire(x, z, wall);
	// A bowl with no fuel never catches (fixtures.cat `flame = 0`).
	if (burning && fire && fire->kind && fire->kind->flameless) return false;
	if (!m_map.SetFixtureBurning(x, z, wall, burning)) return false;
	if (fire) {
		const bool wasLit = fire->lit;
		fire->lit = burning;
		fire->flare = 0.0f;
		fire->effect.SetFlare(0.0f);
		if (burning) {
			// Re-lit in its own reserved buffer, pre-warmed so it is not cold.
			fire->effect.Ignite(fire->flamePos, static_cast<u32>(fire->phase * 977.0f));
		} else {
			fire->effect.Clear();
			// What going out leaves behind, through the effects system: the
			// kind's on_douse effects land on the fire itself (its smoke).
			if (smoke && wasLit && fire->kind)
				for (const fx::Proc& p : fire->kind->onDouse)
					if (const fx::EffectKind* k = m_effects.Find(p.id.View()))
						fx::Apply(fire->effects, *k, k->DefaultSchool(), p.magnitude,
								  p.duration);
		}
	}
	// Its smoke ring came or went with it (DungeonMap::RebuildTurbidity ran).
	RefreshTurbidityGrid();
	return true;
}

bool DungeonWorld::FlareFire(int x, int z, int wall) {
	Fire* fire = FindFire(x, z, wall);
	if (!fire || !fire->lit) return false;
	fire->flare = 1.0f;
	return true;
}

void DungeonWorld::SyncFiresFromMap() {
	// After the map's burning states were set wholesale (a new game, a reset):
	// each live fire takes its record's state, relit or put out to match, with
	// nothing left hanging from before.
	const auto sync = [&](int x, int z, int wall, bool burning) {
		Fire* fire = FindFire(x, z, wall);
		if (!fire) return;
		const bool lit = burning && !(fire->kind && fire->kind->flameless);
		fire->flare = 0.0f;
		fire->effect.SetFlare(0.0f);
		fire->effects.clear();
		if (lit == fire->lit) return;
		fire->lit = lit;
		if (lit) fire->effect.Ignite(fire->flamePos, static_cast<u32>(fire->phase * 977.0f));
		else fire->effect.Clear();
	};
	for (const WallSconce& s : m_map.Sconces())
		sync(s.x, s.z, static_cast<int>(s.wall), s.Burning());
	for (const FloorBrazier& b : m_map.Braziers()) sync(b.x, b.z, -1, b.Burning());
	RefreshTurbidityGrid();
}

void DungeonWorld::UpdateFireTransients(float dt) {
	for (Fire& f : m_fires) {
		if (f.flare > 0.0f) {
			f.flare = std::max(0.0f, f.flare - dt / kFlareSeconds);
			f.effect.SetFlare(f.flare);
		}
		// A fire's effects AGE like any bearer's. There is no TickEffects here
		// because a fire is not an fx::ITarget - nothing can wound it as a fire
		// (smashing one is its FixtureBreak's business) - so nothing it carries
		// can bite; it only runs out.
		if (f.effects.empty()) continue;
		for (fx::Inst& e : f.effects) e.timeLeft -= dt;
		std::erase_if(f.effects, [](const fx::Inst& e) { return e.timeLeft <= 0.0f; });
	}
}

void DungeonWorld::GatherDustPuffs(gfx::Atmosphere& atmo) const {
	// The strongest haze effects any fire carries, at most kMaxDustPuffs: power x
	// the share of its time left, centred on the fire's square.
	std::array<float, gfx::kMaxDustPuffs> held{};
	for (const Fire& f : m_fires)
		for (const fx::Inst& e : f.effects) {
			if (!e.kind || !e.kind->Haze() || e.duration <= 0.0f) continue;
			const float strength = e.magnitude * std::clamp(e.timeLeft / e.duration, 0.0f, 1.0f);
			if (strength <= 0.0f) continue;
			// Into the weakest slot, if it beats it.
			size_t weakest = 0;
			for (size_t i = 1; i < held.size(); ++i)
				if (held[i] < held[weakest]) weakest = i;
			if (strength <= held[weakest]) continue;
			held[weakest] = strength;
			const Vec3 c = m_map.CellCenter(f.x, f.z);
			atmo.dustPuffs[weakest] = {c.x, c.z, kSmokeRadiusCells * kCellSize, strength};
		}
}

} // namespace dungeon::game
