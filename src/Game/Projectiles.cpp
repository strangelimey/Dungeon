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
					  where, kMaxPayloadProcs, p.id);
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
	m_items.push_back(it);
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

void ProjectileSystem::Expire(const Item& it, ExpiryCause cause) {
	if (!onExpire) return;
	onExpire({it.pos, it.dir, cause, it.target, it.atk, it.payload, it.attacker,
			  it.shooter});
}

void ProjectileSystem::Update(float dt) {
	// Age the impact/fizzle sparks (drift out + slight gravity, then expire).
	for (Spark& s : m_sparks) {
		s.age += dt;
		s.pos = Add(s.pos, Scale(s.vel, dt));
		s.vel.y -= 3.5f * dt;
	}
	std::erase_if(m_sparks, [](const Spark& s) { return s.age >= s.life; });

	// Fly each item: a wall/out-of-range EXPIRES it, a target on its side in its
	// cell takes a strike. Both moments deliver the payload — the owner decides
	// what each means. rangeLeft < 0 marks an item spent (erased below).
	for (Item& it : m_items) {
		const float step = it.speed * dt;
		it.pos = Add(it.pos, Scale(it.dir, step));
		it.rangeLeft -= step;

		if (isBlocked && isBlocked(it.pos, it.dir)) { // hit a wall (or left the map)
			SpawnSparkBurst(it.pos, it.color, 8);
			Expire(it, ExpiryCause::Wall);
			it.rangeLeft = -1.0f;
			continue;
		}

		if (resolveHit &&
			resolveHit(it.target, {it.pos, it.dir, it.atk, it.push, it.attacker,
								   it.shooter, it.payload})) { // struck a target
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
	for (const Item& it : m_items) out.push_back({it.pos, it.size, it.color});
	for (const Spark& s : m_sparks) {
		const float fade = 1.0f - s.age / s.life; // dim as it ages
		out.push_back(
			{s.pos, s.size, {s.color.x * fade, s.color.y * fade, s.color.z * fade, 0.0f}});
	}
}

} // namespace dungeon::game
