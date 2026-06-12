#include "Game/FireEffect.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
// Spawns per second at scale 1 (a brazier); sconces run at ~0.55.
constexpr float kFlameRate = 26.0f;
constexpr float kSmokeRate = 4.5f;
constexpr float kSparkRate = 2.0f;
} // namespace

FireEffect::FireEffect(const Vec3& origin, float scale, u32 seed)
	: m_origin(origin), m_scale(scale), m_rng(seed) {
	m_particles.reserve(64);
	// Pre-warm so fires aren't cold when first seen.
	for (int i = 0; i < 30; ++i) Update(0.1f);
}

float FireEffect::Rand(float lo, float hi) {
	return lo + (hi - lo) * std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
}

void FireEffect::Spawn(Kind kind) {
	Particle p;
	p.kind = kind;
	const float s = m_scale;
	const float jitter = 0.07f * s;
	p.pos = {m_origin.x + Rand(-jitter, jitter), m_origin.y,
			 m_origin.z + Rand(-jitter, jitter)};

	switch (kind) {
	case Kind::Flame:
		p.life = Rand(0.40f, 0.70f);
		p.vel = {Rand(-0.12f, 0.12f), Rand(0.65f, 1.05f) * s, Rand(-0.12f, 0.12f)};
		p.size = Rand(0.10f, 0.16f) * s;
		break;
	case Kind::Spark:
		p.life = Rand(0.40f, 0.90f);
		p.vel = {Rand(-0.8f, 0.8f), Rand(1.6f, 2.6f) * s, Rand(-0.8f, 0.8f)};
		p.size = 0.022f * s;
		break;
	case Kind::Smoke:
		p.life = Rand(1.6f, 2.6f);
		p.vel = {Rand(-0.06f, 0.06f), Rand(0.30f, 0.45f), Rand(-0.06f, 0.06f)};
		p.size = Rand(0.08f, 0.12f) * s;
		p.pos.y += 0.15f * s; // smoke starts above the flame
		break;
	}
	m_particles.push_back(p);
}

void FireEffect::Update(float dt) {
	// Spawning via rate accumulators (frame-rate independent).
	m_flameAccum += kFlameRate * m_scale * dt;
	m_smokeAccum += kSmokeRate * m_scale * dt;
	m_sparkAccum += kSparkRate * m_scale * dt;
	while (m_flameAccum >= 1.0f) { Spawn(Kind::Flame); m_flameAccum -= 1.0f; }
	while (m_smokeAccum >= 1.0f) { Spawn(Kind::Smoke); m_smokeAccum -= 1.0f; }
	while (m_sparkAccum >= 1.0f) { Spawn(Kind::Spark); m_sparkAccum -= 1.0f; }

	for (size_t i = 0; i < m_particles.size();) {
		Particle& p = m_particles[i];
		p.age += dt;
		if (p.age >= p.life) {
			p = m_particles.back(); // swap-erase, order is irrelevant
			m_particles.pop_back();
			continue;
		}
		switch (p.kind) {
		case Kind::Flame:
			p.vel.y += 0.4f * dt; // hot air accelerates upward
			break;
		case Kind::Spark:
			p.vel.y -= 3.5f * dt; // gravity arc
			break;
		case Kind::Smoke:
			p.pos.x += std::sin(p.age * 1.7f) * 0.10f * dt; // lazy drift
			break;
		}
		p.pos = Add(p.pos, Scale(p.vel, dt));
		++i;
	}
}

void FireEffect::AppendParticles(std::vector<gfx::ParticleInstance>& out) const {
	for (const Particle& p : m_particles) {
		const float t = p.age / p.life;
		gfx::ParticleInstance instance;
		instance.position = p.pos;

		switch (p.kind) {
		case Kind::Flame: {
			// Bright orange core cooling to deep red, shrinking as it rises.
			const Vec3 hot{1.15f, 0.55f, 0.16f}, cool{0.50f, 0.07f, 0.02f};
			const float glow = 1.2f * (1.0f - t) + 0.15f;
			const Vec3 c = Lerp(hot, cool, t);
			instance.color = {c.x * glow, c.y * glow, c.z * glow, 0.0f}; // additive
			instance.size = p.size * (1.0f - 0.55f * t);
			break;
		}
		case Kind::Spark: {
			const float glow = 1.0f - t;
			instance.color = {1.3f * glow, 0.75f * glow, 0.25f * glow, 0.0f};
			instance.size = p.size;
			break;
		}
		case Kind::Smoke: {
			// Fade in quickly, out slowly; grow the whole time.
			const float fade = t < 0.25f ? t / 0.25f : 1.0f - (t - 0.25f) / 0.75f;
			const float alpha = 0.22f * fade;
			instance.color = {0.06f * alpha, 0.055f * alpha, 0.05f * alpha, alpha};
			instance.size = p.size * (1.0f + 2.2f * t);
			break;
		}
		}
		out.push_back(instance);
	}
}

} // namespace dungeon::game
