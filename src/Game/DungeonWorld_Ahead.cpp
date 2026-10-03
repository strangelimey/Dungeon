// ============================================================================
// Game/DungeonWorld_Ahead.cpp - the world IN FRONT OF THE PARTY, as the cast
// services hand it to a spell (Spell/Spell.h CastServices): the fire the party
// faces, the floor at its feet, the monster and the shots in the square ahead,
// and a blast spreading round its own square. The hand spells (Puff of Flame,
// Pebble, Puff of Wind, Splash) and the third-tier ward burst reach the world
// through these and nothing else, so the magic module stays walled off.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Game/Spell/Spell.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

bool DungeonWorld::FireAheadCell(int& x, int& z, int& wall) const {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	const Direction facing = static_cast<Direction>(m_party.Facing());
	// A wall torch hangs on a wall OF the party's own square: the one it faces.
	for (const WallSconce& s : m_map.Sconces())
		if (s.x == px && s.z == pz && s.wall == facing) {
			x = s.x, z = s.z, wall = static_cast<int>(s.wall);
			return true;
		}
	// A brazier blocks its square, so the nearest one can be is the next.
	if (const FloorBrazier* b = m_map.BrazierAt(px + DirDX(facing), pz + DirDZ(facing))) {
		x = b->x, z = b->z, wall = -1;
		return true;
	}
	return false;
}

FireAhead DungeonWorld::FireAheadOfParty() const {
	int x = 0, z = 0, wall = -1;
	if (!FireAheadCell(x, z, wall)) return {};
	const auto canBurn = [this](const std::string& type) {
		const auto it = m_fixtureKinds.find(type);
		return it == m_fixtureKinds.end() || !it->second->flameless;
	};
	if (wall >= 0) {
		for (const WallSconce& s : m_map.Sconces())
			if (s.x == x && s.z == z && static_cast<int>(s.wall) == wall)
				return {FireAhead::Kind::WallTorch, s.Burning(), canBurn(s.type) && !s.empty,
						s.empty};
		return {};
	}
	const FloorBrazier* b = m_map.BrazierAt(x, z);
	return {FireAhead::Kind::Brazier, b->Burning(), canBurn(b->type)};
}

bool DungeonWorld::SetFireAhead(bool burning) {
	int x = 0, z = 0, wall = -1;
	return FireAheadCell(x, z, wall) && SetFireBurning(x, z, wall, burning);
}

bool DungeonWorld::FlareFireAhead() {
	int x = 0, z = 0, wall = -1;
	return FireAheadCell(x, z, wall) && FlareFire(x, z, wall);
}

float DungeonWorld::FireAheadHaze() const {
	int x = 0, z = 0, wall = -1;
	if (!FireAheadCell(x, z, wall)) return 0.0f;
	float haze = 0.0f;
	for (const Fire& f : m_fires) {
		if (f.x != x || f.z != z || f.wall != wall) continue;
		for (const fx::Inst& e : f.effects)
			if (e.kind && e.kind->Haze() && e.duration > 0.0f)
				haze += e.magnitude * std::clamp(e.timeLeft / e.duration, 0.0f, 1.0f);
	}
	return haze;
}

float DungeonWorld::FireAheadFlare() const {
	int x = 0, z = 0, wall = -1;
	if (!FireAheadCell(x, z, wall)) return 0.0f;
	for (const Fire& f : m_fires)
		if (f.x == x && f.z == z && f.wall == wall) return f.flare;
	return 0.0f;
}

std::string DungeonWorld::ItemIdsAt(int x, int z) const {
	std::string ids;
	for (const Item& it : m_items) {
		if (it.collected || !it.kind || it.x != x || it.z != z) continue;
		if (!ids.empty()) ids += ' ';
		ids += it.kind->id;
	}
	return ids;
}

std::string_view DungeonWorld::RenameHeldItem(ItemSlot& slot,
											  std::string ItemKind::*becomes) {
	if (slot.Empty()) return {};
	const std::string& to = ItemKindFor(slot.typeId).*becomes;
	if (to.empty()) return {};
	// Into the slot's own buffer, keeping its charge (a relit torch has what it
	// had left); the new kind's name for the caster's line.
	slot.typeId.assign(to);
	return ItemKindFor(slot.typeId).nameKey;
}

void DungeonWorld::DropAtPartyFeet(std::string_view itemId) {
	// Through a kept buffer: a cast lands in a guarded frame, and constructing
	// a string there allocates in the debug CRT whatever its length.
	m_dropIdScratch.assign(itemId);
	DropItemInCell(m_dropIdScratch, m_party.GridX(), m_party.GridZ());
}

bool DungeonWorld::ShoveAhead(int cells) {
	const Direction facing = static_cast<Direction>(m_party.Facing());
	const int dx = DirDX(facing), dz = DirDZ(facing);
	const int ax = m_party.GridX() + dx, az = m_party.GridZ() + dz;
	// Every body in the square, each as far as it will go: a pack of small
	// things is blown back together, not just the one in front.
	bool moved = false;
	for (size_t i = 0; i < m_monsters.size(); ++i) {
		const Monster& m = m_monsters[i];
		if (!m.Alive() || m.x != ax || m.z != az) continue;
		moved |= ShoveMonster(i, dx, dz, cells) > 0;
	}
	return moved;
}

ProjectileSystem::Repelled DungeonWorld::RepelAhead(float power, int casterIndex) {
	const Direction facing = static_cast<Direction>(m_party.Facing());
	const int px = m_party.GridX(), pz = m_party.GridZ();
	const int ax = px + DirDX(facing), az = pz + DirDZ(facing);
	// The zone is the party's square and the one it faces: a shot already that
	// close is one the breeze can still catch. Flung back, it has at least the
	// two squares it came through plus two more to fly home in.
	return m_projectiles.Repel(
		[&](const Vec3& p) {
			const int cx = static_cast<int>(std::floor(p.x / kCellSize));
			const int cz = static_cast<int>(std::floor(p.z / kCellSize));
			return (cx == px && cz == pz) || (cx == ax && cz == az);
		},
		power, casterIndex, 4.0f * kCellSize);
}

void DungeonWorld::SpawnBoltAfter(const ProjectileSpec& spec, float delay) {
	if (delay <= 0.0f) {
		Launch(spec);
		return;
	}
	// A full queue means a volley larger than any spell can make: the bolt goes
	// now rather than vanishing.
	if (m_pendingBoltCount >= m_pendingBolts.size()) {
		Launch(spec);
		return;
	}
	// Dressed NOW: its light and trail names are borrowed from the spell, which
	// a catalog reload could replace before the bolt's turn comes. It is still
	// no light until it launches - a bolt waiting here makes none.
	PendingBolt& p = m_pendingBolts[m_pendingBoltCount++];
	p = {spec, delay};
	DressFlight(p.spec);
}

void DungeonWorld::UpdatePendingBolts(float dt) {
	for (size_t i = 0; i < m_pendingBoltCount;) {
		PendingBolt& p = m_pendingBolts[i];
		p.delay -= dt;
		if (p.delay > 0.0f) {
			++i;
			continue;
		}
		Launch(p.spec);
		p = m_pendingBolts[--m_pendingBoltCount]; // swap-remove; order is the delays'
	}
}

void DungeonWorld::BlastAroundParty(const ProjectilePayload& payload, SpellSymbol school,
									int casterIndex) {
	Detonate(m_party.GridX(), m_party.GridZ(), payload, m_damageTypes.ForSchool(school),
			 casterIndex, /*spareCentre*/ true);
}

void DungeonWorld::HandPuff(SpellSymbol school, const Vec3& origin, const Vec3& dir) {
	// Under half a square ahead of the eye and a little below it, where the
	// caster's hands would be: near enough to read as theirs, far enough that
	// the motes are not clipped by the near plane.
	const Vec3 at{origin.x + dir.x * kCellSize * 0.55f, origin.y - 0.1f * kUnit,
				  origin.z + dir.z * kCellSize * 0.55f};
	const Vec4& c = ElementColor(school);
	// A hot core of small motes and a dimmer bloom round it - the blast puff's
	// flare (LandBlastHit) shrunk to a fist and quicker.
	m_projectiles.Puff(at, {c.x * 1.8f, c.y * 1.6f, c.z * 1.4f, 0.0f}, 10, 0.25f, 0.35f,
					   0.03f * kUnit, 0.03f * kUnit);
	m_projectiles.Puff(at, {c.x * 0.6f, c.y * 0.45f, c.z * 0.35f, 0.0f}, 4, 0.35f, 0.5f,
					   0.06f * kUnit, 0.05f * kUnit);
	// And it lights what is round it for a moment.
	HandGlow* slot = &m_handGlows[0];
	for (HandGlow& g : m_handGlows)
		if (g.timeLeft < slot->timeLeft) slot = &g;
	*slot = {at, {c.x, c.y, c.z}, 0.5f, 0.5f};
}

void DungeonWorld::TickHandGlows(float dt) {
	for (HandGlow& g : m_handGlows) g.timeLeft = std::max(0.0f, g.timeLeft - dt);
}

} // namespace dungeon::game
