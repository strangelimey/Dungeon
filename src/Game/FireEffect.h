// ============================================================================
// Game/FireEffect.h — one burning fire (sconce torch or brazier).
//
// Simulates three particle kinds rising from a flame origin:
//   Flame — short-lived additive licks, bright orange cooling to red
//   Spark — rare fast embers on gravity arcs
//   Smoke — slow dark alpha puffs that grow and fade
// Update() advances and respawns; AppendParticles() emits premultiplied
// ParticleInstances for the gfx::ParticleBatch. `scale` sizes everything
// (sconces burn smaller than braziers). Deterministic per seed.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Graphics/ParticleBatch.h"

#include <random>
#include <vector>

namespace dungeon::game {

class FireEffect {
public:
	FireEffect() = default;
	// Reserve(scale) then Ignite(origin, seed): a fire that is burning now.
	FireEffect(const Vec3& origin, float scale, u32 seed);

	// AN UNLIT FIRE OF THIS SIZE, its particle buffer taken at its full
	// ceiling (CapacityFor) and nothing burning yet. For a fire that comes and
	// goes - a monster's plume - so catching alight costs no allocation: that
	// happens in the middle of a fight, in a frame the steady-state guard
	// watches, and a plume allocated on ignition allocated every time any
	// monster caught fire again (tools\AllocTest.ps1 -Impact found it).
	void Reserve(float scale);
	// Light it: particles cleared, the spawn clocks restarted, reseeded, and
	// pre-warmed so it is not cold when first seen. The buffer is reused.
	void Ignite(const Vec3& origin, u32 seed);
	// Put it out: every particle gone, the buffer kept for the next Ignite.
	void Clear();

	void Update(float dt);
	void AppendParticles(std::vector<gfx::ParticleInstance>& out) const;

	// How many particles this fire settles at — each kind's spawn rate times its
	// mean lifetime, summed. The caller that owns the buffer AppendParticles
	// writes into reserves from this (DungeonWorld::ReserveParticleScratch), so
	// a steady-state frame never grows it. It is a MEAN and the live count
	// wanders above it (spawn times and lifetimes are both random), which is why
	// that caller adds headroom rather than trusting this number flat.
	int SteadyCount() const { return SteadyCountFor(m_scale); }
	// The same estimate for a fire that does not exist yet — a monster's plume is
	// created only when it catches alight, and the reserve has to cover it before
	// then. Constructing one to ask would run the ctor's 30-tick pre-warm.
	static int SteadyCountFor(float scale);
	// The HARD ceiling on live particles: the steady count plus headroom for
	// the random wander above it. A spawn that would pass it is dropped, so the
	// buffer reserved at this size never grows - a mean with headroom is an
	// estimate, a ceiling is a promise. (It used to be a flat reserve(64) with
	// no cap, and a plume's pre-warm alone grew past it.) Sized so a drop is a
	// multi-sigma event, never something that thins a fire you can see.
	static int CapacityFor(float scale);
	int Capacity() const { return m_capacity; }

	// Move the emitter (a burning MONSTER walks around; a sconce never does).
	// Only new particles spawn at the new origin — the ones already in the air
	// keep their own velocity, so the plume trails the body.
	void SetOrigin(const Vec3& origin) { m_origin = origin; }
	// Recolour the flames and sparks: a multiplier over the authored orange
	// palette, so {1,1,1} is an ordinary fire and a cold blue makes it a
	// freezing one. Smoke is left alone (smoke is smoke).
	void SetTint(const Vec3& tint) { m_tint = tint; }

private:
	enum class Kind { Flame, Spark, Smoke };
	struct Particle {
		Vec3 pos;
		Vec3 vel;
		float age = 0.0f;
		float life = 1.0f;
		float size = 0.1f;
		Kind kind = Kind::Flame;
	};

	float Rand(float lo, float hi);
	void Spawn(Kind kind);

	Vec3 m_origin{};
	Vec3 m_tint{1.0f, 1.0f, 1.0f};
	float m_scale = 1.0f;
	// minstd, not mt19937: particle jitter needs no more than this, and the
	// Mersenne Twister's 5 KB of state was the whole reason a monster's plume
	// had to be heap-held and allocated on ignition. Now one rides in every
	// monster by value.
	std::minstd_rand m_rng;
	int m_capacity = 0; // CapacityFor(m_scale); 0 = never reserved, spawns nothing
	std::vector<Particle> m_particles;
	float m_flameAccum = 0.0f;
	float m_smokeAccum = 0.0f;
	float m_sparkAccum = 0.0f;
};

} // namespace dungeon::game
