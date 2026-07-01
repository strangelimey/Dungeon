// ============================================================================
// Game/Projectiles.cpp — see Projectiles.h.
// ============================================================================
#include "Game/Projectiles.h"

namespace dungeon::game {

void ProjectileSystem::Spawn(const ProjectileSpec& spec) {
	Item it;
	it.pos = spec.pos;
	it.dir = spec.dir;
	it.speed = spec.speed;
	it.rangeLeft = spec.range;
	it.atk = spec.atk;
	it.color = spec.color;
	it.size = spec.size;
	it.target = spec.target;
	m_items.push_back(it);
}

void ProjectileSystem::SpawnSparkBurst(const Vec3& pos, const Vec4& color, int count) {
	for (int i = 0; i < count; ++i) {
		Spark s;
		s.pos = pos;
		auto r = [&] { return (static_cast<float>(m_rng() & 0xFFFF) / 32768.0f) - 1.0f; };
		s.vel = {r() * 2.2f, r() * 2.2f + 0.6f, r() * 2.2f};
		s.color = {color.x, color.y, color.z, 0.0f}; // additive
		s.age = 0.0f;
		s.life = 0.25f + (static_cast<float>(m_rng() & 0xFF) / 255.0f) * 0.2f;
		s.size = 0.1f;
		m_sparks.push_back(s);
	}
}

void ProjectileSystem::Update(float dt) {
	// Age the impact/fizzle sparks (drift out + slight gravity, then expire).
	for (Spark& s : m_sparks) {
		s.age += dt;
		s.pos = Add(s.pos, Scale(s.vel, dt));
		s.vel.y -= 3.5f * dt;
	}
	std::erase_if(m_sparks, [](const Spark& s) { return s.age >= s.life; });

	// Fly each item: a wall/out-of-range fizzles, a target on its side in its cell
	// takes a strike. rangeLeft < 0 marks an item spent (erased below).
	for (Item& it : m_items) {
		const float step = it.speed * dt;
		it.pos = Add(it.pos, Scale(it.dir, step));
		it.rangeLeft -= step;

		if (isBlocked && isBlocked(it.pos)) { // hit a wall (or left the map)
			SpawnSparkBurst(it.pos, it.color, 8);
			if (onFizzle) onFizzle(it.pos);
			it.rangeLeft = -1.0f;
			continue;
		}

		if (resolveHit && resolveHit(it.target, it.pos, it.atk)) { // struck a target
			SpawnSparkBurst(it.pos, it.color, 14);
			it.rangeLeft = -1.0f;
			continue;
		}

		if (it.rangeLeft <= 0.0f) { // ran out of reach in open air
			SpawnSparkBurst(it.pos, it.color, 6);
			if (onFizzle) onFizzle(it.pos);
		}
	}
	std::erase_if(m_items, [](const Item& it) { return it.rangeLeft <= 0.0f; });
}

void ProjectileSystem::AppendBillboards(std::vector<gfx::ParticleInstance>& out) const {
	for (const Item& it : m_items) out.push_back({it.pos, it.size, it.color});
	for (const Spark& s : m_sparks) {
		const float fade = 1.0f - s.age / s.life; // dim as it ages
		out.push_back(
			{s.pos, s.size, {s.color.x * fade, s.color.y * fade, s.color.z * fade, 0.0f}});
	}
}

} // namespace dungeon::game
