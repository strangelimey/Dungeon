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
	float m_scale = 1.0f;
	std::mt19937 m_rng;
	std::vector<Particle> m_particles;
	float m_flameAccum = 0.0f;
	float m_smokeAccum = 0.0f;
	float m_sparkAccum = 0.0f;
};

} // namespace dungeon::game
