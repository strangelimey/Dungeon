// ============================================================================
// Game/Projectiles.cpp — see Projectiles.h.
// ============================================================================
#include "Game/Projectiles.h"

#include "Core/Log.h"

#include <algorithm> // std::erase_if

namespace dungeon::game {

ProjectilePayload PackPayload(std::span<const fx::Proc> procs,
							  std::string_view where) {
	ProjectilePayload out;
	for (const fx::Proc& p : procs)
		if (!out.Add(p))
			log::Warn("{} authors more than {} on-hit effects; '{}' and any "
					  "after it are dropped",
					  where, kMaxPayloadProcs, p.id.View());
	return out;
}

void ProjectileSystem::Spawn(const ProjectileSpec& spec) {
	Item it;
	it.id = m_nextId++;
	it.pos = spec.pos;
	it.dir = spec.dir;
	it.speed = spec.speed;
	it.rangeLeft = spec.range;
	it.atk = spec.atk;
	it.color = spec.color;
	it.size = spec.size;
	it.target = spec.target;
	it.push = spec.push;
	it.attacker = spec.attacker;
	it.shooter = spec.shooter;
	it.payload = spec.payload;
	it.cargo = spec.cargo;
	it.cargoCharge = spec.cargoCharge;
	m_items.push_back(it);
}

void ProjectileSystem::LandCargo() {
	for (Item& it : m_items)
		if (it.cargo) {
			Expire(it, ExpiryCause::Range);
			it.rangeLeft = -1.0f;
		}
	std::erase_if(m_items, [](const Item& it) { return it.rangeLeft <= 0.0f; });
}

std::vector<ProjectileInfo> ProjectileSystem::Live() const {
	std::vector<ProjectileInfo> out;
	out.reserve(m_items.size());
	for (const Item& it : m_items)
		out.push_back({it.id, it.pos, it.dir, it.speed, it.rangeLeft, it.atk,
					   it.target, it.payload});
	return out;
}

bool ProjectileSystem::Find(u32 id, ProjectileInfo& out) const {
	for (const Item& it : m_items)
		if (it.id == id) {
			out = {it.id,  it.pos,       it.dir,     it.speed,
				   it.rangeLeft, it.atk, it.target, it.payload};
			return true;
		}
	return false;
}

bool ProjectileSystem::Remove(u32 id) {
	// A thrown item is landed where it is, never dismissed into nothing.
	for (const Item& it : m_items)
		if (it.id == id && it.cargo) Expire(it, ExpiryCause::Range);
	return std::erase_if(m_items, [id](const Item& it) { return it.id == id; }) > 0;
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

void ProjectileSystem::Puff(const Vec3& pos, const Vec4& color, int count, float spread,
							 float life, float size, float jitter, const Vec3& drift) {
	auto r = [&] { return (static_cast<float>(m_rng() & 0xFFFF) / 32768.0f) - 1.0f; };
	for (int i = 0; i < count; ++i) {
		Spark s;
		s.pos = {pos.x + r() * jitter, pos.y + r() * jitter * (0.25f / 0.6f), pos.z + r() * jitter};
		s.vel = {drift.x + r() * spread, drift.y + 0.15f + r() * spread * 0.3f,
				 drift.z + r() * spread};
		s.color = {color.x, color.y, color.z, 0.0f}; // additive
		s.life = life * (0.75f + 0.25f * (r() + 1.0f));
		s.size = size;
		s.fall = -0.2f; // drifts up, as warm air or a cloud does
		s.swell = true;
		m_sparks.push_back(s);
	}
}

void ProjectileSystem::Splash(const Vec3& pos, const Vec3& dir, const Vec4& color, int count,
							  float speed, float life, float size) {
	auto r = [&] { return (static_cast<float>(m_rng() & 0xFFFF) / 32768.0f) - 1.0f; };
	for (int i = 0; i < count; ++i) {
		Spark s;
		s.pos = {pos.x + r() * 0.04f, pos.y + r() * 0.04f, pos.z + r() * 0.04f};
		// Out in every direction but mostly forward and up, so it reads as a
		// flung handful rather than a burst.
		const float lean = 0.5f + 0.5f * (r() + 1.0f) * 0.5f;
		s.vel = {(dir.x * lean + r() * 0.6f) * speed, (0.7f + 0.5f * r()) * speed,
				 (dir.z * lean + r() * 0.6f) * speed};
		s.color = {color.x, color.y, color.z, 0.0f}; // additive
		s.life = life * (0.75f + 0.25f * (r() + 1.0f));
		s.size = size * (0.7f + 0.3f * (r() + 1.0f) * 0.5f);
		s.fall = 7.0f; // drops fall, and fall fast
		m_sparks.push_back(s);
	}
}

void ProjectileSystem::Expire(const Item& it, ExpiryCause cause) {
	if (!onExpire) return;
	onExpire({it.pos, it.dir, cause, it.target, it.atk, it.payload, it.attacker,
			  it.shooter, it.cargo, it.cargoCharge});
}

void ProjectileSystem::Update(float dt) {
	// Age the impact/fizzle sparks (drift out + slight gravity, then expire).
	for (Spark& s : m_sparks) {
		s.age += dt;
		s.pos = Add(s.pos, Scale(s.vel, dt));
		s.vel.y -= s.fall * dt;
	}
	std::erase_if(m_sparks, [](const Spark& s) { return s.age >= s.life; });

	// Fly each item: a wall/out-of-range EXPIRES it, a target on its side in its
	// cell takes a strike. Both moments deliver the payload — the owner decides
	// what each means. rangeLeft < 0 marks an item spent (erased below).
	for (Item& it : m_items) {
		const float step = it.speed * dt;
		it.pos = Add(it.pos, Scale(it.dir, step));
		it.rangeLeft -= step;
		it.age += dt;

		if (isBlocked && isBlocked(it.pos, it.dir)) { // hit a wall (or left the map)
			SpawnSparkBurst(it.pos, it.color, 8);
			Expire(it, ExpiryCause::Wall);
			it.rangeLeft = -1.0f;
			continue;
		}

		if (resolveHit &&
			resolveHit(it.target, {it.pos, it.dir, it.atk, it.push, it.attacker,
								   it.shooter, it.payload, it.cargo, it.cargoCharge})) { // struck a target
			SpawnSparkBurst(it.pos, it.color, 14);
			it.rangeLeft = -1.0f;
			continue;
		}

		if (it.rangeLeft <= 0.0f) { // ran out of reach in open air
			SpawnSparkBurst(it.pos, it.color, 6);
			Expire(it, ExpiryCause::Range);
		}
	}
	std::erase_if(m_items, [](const Item& it) { return it.rangeLeft <= 0.0f; });
}

void ProjectileSystem::AppendBillboards(std::vector<gfx::ParticleInstance>& out) const {
	// A thrown item draws as itself (the host's ForEachCargo), not a glow.
	for (const Item& it : m_items)
		if (!it.cargo) out.push_back({it.pos, it.size, it.color});
	for (const Spark& s : m_sparks) {
		const float t = s.age / s.life;
		const float fade = 1.0f - t; // dim as it ages
		// A puff fades in over its first fifth and swells as it thins.
		const float in = s.swell ? std::min(1.0f, t * 5.0f) : 1.0f;
		const float size = s.swell ? s.size * (0.6f + 0.8f * t) : s.size;
		out.push_back({s.pos, size,
					   {s.color.x * fade * in, s.color.y * fade * in, s.color.z * fade * in, 0.0f}});
	}
}

} // namespace dungeon::game
