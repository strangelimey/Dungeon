// ============================================================================
// Game/Projectiles.cpp — see Projectiles.h.
// ============================================================================
#include "Game/Projectiles.h"

#include "Core/Log.h"

#include <algorithm> // std::erase_if
#include <cmath>

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
	it.light = spec.light;
	it.lightColor = spec.lightColor;
	it.trail = spec.trail;
	m_items.push_back(it);
}

bool ProjectileSystem::AddSpark(const Spark& s) {
	if (m_sparks.size() < kReservedSparks) {
		m_sparks.push_back(s);
		return true;
	}
	// Full: the trail particle furthest through its life makes room - for a
	// hit's spark, a blast's puff, or a fresher trail particle alike.
	Spark* oldest = nullptr;
	float most = -1.0f;
	for (Spark& p : m_sparks)
		if (p.trail && p.age / p.life > most) {
			most = p.age / p.life;
			oldest = &p;
		}
	if (!oldest) {
		++m_refusedNow;
		return false;
	}
	*oldest = s;
	++m_recycledNow;
	return true;
}

void ProjectileSystem::ShedTrail(Item& it, float step) {
	const trail::Spec& t = it.trail;
	if (!t.Any() || step <= 0.0f || trailSquare <= 0.0f) return;
	// Full near the eye, thinning to a quarter by twelve squares off - and
	// nothing at all within arm's reach of it: a bolt leaves from beside the
	// eye, and a particle a hand's width from the lens is a blurred orb filling
	// a corner of the screen, not a trail.
	const Vec3 d = Sub(it.pos, m_eye);
	const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (dist < 0.5f * trailSquare) return;
	const float nearM = 3.0f * trailSquare, farM = 12.0f * trailSquare;
	const float thin =
		dist <= nearM ? 1.0f : std::max(0.25f, 1.0f - 0.75f * (dist - nearM) / (farM - nearM));
	it.trailDebt += t.rate * thin * step / trailSquare;

	auto r = [&] { return (static_cast<float>(m_rng() & 0xFFFF) / 32768.0f) - 1.0f; };
	const Vec3 c = t.hasColor ? t.color : it.lightColor;
	int shed = 0;
	while (it.trailDebt >= 1.0f && shed < kMaxShedPerFrame) {
		it.trailDebt -= 1.0f;
		++shed;
		Spark s;
		// Anywhere along the stretch it just flew, so a fast bolt's trail is a
		// line rather than beads at each frame's end.
		const float back = step * 0.5f * (r() + 1.0f);
		s.pos = {it.pos.x - it.dir.x * back + r() * t.size, it.pos.y - it.dir.y * back + r() * t.size,
				 it.pos.z - it.dir.z * back + r() * t.size};
		s.vel = {r() * t.spread, r() * t.spread * 0.5f, r() * t.spread};
		// A spark is flung BACK off the bolt, the way a struck flint's are.
		if (t.shape == trail::Shape::Spark) {
			s.vel.x -= it.dir.x * 1.5f;
			s.vel.z -= it.dir.z * 1.5f;
		}
		s.color = {c.x * 1.2f, c.y * 1.2f, c.z * 1.2f, 0.0f}; // additive
		s.life = t.life * (0.75f + 0.25f * (r() + 1.0f));
		s.size = t.size;
		s.fall = t.fall;
		s.swell = t.swell;
		s.trail = true;
		s.flicker = t.flicker;
		s.swirl = r() < 0.0f ? -t.swirl : t.swirl;
		s.phase = (r() + 1.0f) * 3.14159f;
		if (!AddSpark(s)) break;
	}
	// A frame that owed more than its cap forgives the rest.
	if (shed >= kMaxShedPerFrame) it.trailDebt = std::min(it.trailDebt, 1.0f);
}

void ProjectileSystem::LeaveFlash(const Item& it) {
	if (it.light < 0 || it.cargo) return; // a thrown thing lands and keeps its light
	Flash* slot = &m_flashes[0];
	for (Flash& f : m_flashes) {
		if (f.light < 0 || f.age >= kFlashSeconds) {
			slot = &f;
			break;
		}
		if (f.age > slot->age) slot = &f; // all busy: the oldest
	}
	*slot = {it.pos, it.lightColor, it.light, 0.0f};
}

ProjectileSystem::PoolStats ProjectileSystem::Stats() const {
	PoolStats s;
	s.live = m_sparks.size();
	s.capacity = kReservedSparks;
	for (const Spark& p : m_sparks)
		if (p.trail) ++s.trail;
	s.flights = m_items.size();
	for (const Item& it : m_items)
		if (it.trail.Any()) ++s.shedding;
	s.recycled = m_recycledLast;
	s.refused = m_refusedLast;
	return s;
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
		if (!AddSpark(s)) return;
	}
}

void ProjectileSystem::Puff(const Vec3& pos, const Vec4& color, int count, float spread,
							 float life, float size, float jitter) {
	auto r = [&] { return (static_cast<float>(m_rng() & 0xFFFF) / 32768.0f) - 1.0f; };
	for (int i = 0; i < count; ++i) {
		Spark s;
		s.pos = {pos.x + r() * jitter, pos.y + r() * jitter * (0.25f / 0.6f), pos.z + r() * jitter};
		s.vel = {r() * spread, 0.15f + r() * spread * 0.3f, r() * spread};
		s.color = {color.x, color.y, color.z, 0.0f}; // additive
		s.life = life * (0.75f + 0.25f * (r() + 1.0f));
		s.size = size;
		s.fall = -0.2f; // drifts up, as warm air or a cloud does
		s.swell = true;
		if (!AddSpark(s)) return;
	}
}

void ProjectileSystem::Expire(const Item& it, ExpiryCause cause) {
	if (!onExpire) return;
	onExpire({it.pos, it.dir, cause, it.target, it.atk, it.payload, it.attacker,
			  it.shooter, it.cargo, it.cargoCharge});
}

void ProjectileSystem::Update(float dt) {
	// Pool pressure, per whole second.
	m_statClock += dt;
	if (m_statClock >= 1.0f) {
		m_statClock -= std::floor(m_statClock);
		m_recycledLast = m_recycledNow;
		m_refusedLast = m_refusedNow;
		m_recycledNow = m_refusedNow = 0;
	}
	for (Flash& f : m_flashes)
		if (f.light >= 0 && (f.age += dt) >= kFlashSeconds) f.light = -1;

	// Age the particles (drift out + slight gravity, then expire). A mote's
	// drift turns about the vertical as it goes.
	for (Spark& s : m_sparks) {
		s.age += dt;
		s.pos = Add(s.pos, Scale(s.vel, dt));
		s.vel.y -= s.fall * dt;
		if (s.swirl != 0.0f) {
			const float a = s.swirl * dt, ca = std::cos(a), sa = std::sin(a);
			s.vel = {s.vel.x * ca - s.vel.z * sa, s.vel.y, s.vel.x * sa + s.vel.z * ca};
		}
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
			LeaveFlash(it);
			Expire(it, ExpiryCause::Wall);
			it.rangeLeft = -1.0f;
			continue;
		}

		if (resolveHit &&
			resolveHit(it.target, {it.pos, it.dir, it.atk, it.push, it.attacker,
								   it.shooter, it.payload, it.cargo, it.cargoCharge})) { // struck a target
			SpawnSparkBurst(it.pos, it.color, 14);
			LeaveFlash(it);
			it.rangeLeft = -1.0f;
			continue;
		}

		ShedTrail(it, step);
		if (it.rangeLeft <= 0.0f) { // ran out of reach in open air
			SpawnSparkBurst(it.pos, it.color, 6);
			LeaveFlash(it);
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
		float in = s.swell ? std::min(1.0f, t * 5.0f) : 1.0f;
		const float size = s.swell ? s.size * (0.6f + 0.8f * t) : s.size;
		// An ember flickers as it drifts.
		if (s.flicker > 0.0f)
			in *= 1.0f - s.flicker * 0.5f * (1.0f + std::sin(s.age * 25.0f + s.phase));
		out.push_back({s.pos, size,
					   {s.color.x * fade * in, s.color.y * fade * in, s.color.z * fade * in, 0.0f}});
	}
}

} // namespace dungeon::game
