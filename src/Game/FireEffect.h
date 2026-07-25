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
	FireEffect(const Vec3& origin, float scale, u32 seed);

	void Update(float dt);
	void AppendParticles(std::vector<gfx::ParticleInstance>& out) const;

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
	std::mt19937 m_rng;
	std::vector<Particle> m_particles;
	float m_flameAccum = 0.0f;
	float m_smokeAccum = 0.0f;
	float m_sparkAccum = 0.0f;
};

} // namespace dungeon::game
