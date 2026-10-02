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

#include <cmath>

namespace dungeon::game {

FireAhead DungeonWorld::FireAheadOfParty() const {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	const Direction facing = static_cast<Direction>(m_party.Facing());
	const auto canBurn = [this](const std::string& type) {
		const auto it = m_fixtureKinds.find(type);
		return it == m_fixtureKinds.end() || !it->second->flameless;
	};
	// A wall torch hangs on a wall OF the party's own square: the one it faces.
	for (const WallSconce& s : m_map.Sconces())
		if (s.x == px && s.z == pz && s.wall == facing)
			return {FireAhead::Kind::WallTorch, s.lit, canBurn(s.type)};
	// A brazier blocks its square, so the nearest one can be is the next.
	if (const FloorBrazier* b =
			m_map.BrazierAt(px + DirDX(facing), pz + DirDZ(facing)))
		return {FireAhead::Kind::Brazier, b->lit, canBurn(b->type)};
	return {};
}

void DungeonWorld::DropAtPartyFeet(std::string_view itemId) {
	DropItemInCell(std::string(itemId), m_party.GridX(), m_party.GridZ());
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

void DungeonWorld::BlastAroundParty(const ProjectilePayload& payload, SpellSymbol school,
									int casterIndex) {
	Detonate(m_party.GridX(), m_party.GridZ(), payload, m_damageTypes.ForSchool(school),
			 casterIndex, /*spareCentre*/ true);
}

} // namespace dungeon::game
