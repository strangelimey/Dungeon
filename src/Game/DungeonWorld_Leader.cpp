// ============================================================================
// Game/DungeonWorld_Leader.cpp - the party leader (ui-updates Phase 9).
//
// The leader is the member who does what the mouse does in the world: lifts an
// item off the floor, works a door's hand-hold or a lever, throws. It is a
// roster index, party state like the free-look offset, and is picked by a click
// on a member's name in the party bar (or on their card).
//
// THE LEAD PASSES, and does not come back (Michael): a leader who is down or
// dead hands it to the next STANDING member in roster order, and getting up
// again does not take it back. Checked every frame, which costs one health test
// and covers every way a member can fall - a blow, a DoT, over-exertion, a pit -
// without a hook in each.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"

namespace dungeon::game {

const Character* DungeonWorld::LeaderMember() const {
	if (!m_roster || m_leader < 0 || m_leader >= static_cast<int>(m_roster->size()))
		return nullptr;
	const Character& c = (*m_roster)[static_cast<size_t>(m_leader)];
	return c.IsAlive() ? &c : nullptr;
}

std::string_view DungeonWorld::LeaderName() const {
	if (!m_roster || m_leader < 0 || m_leader >= static_cast<int>(m_roster->size())) return {};
	return (*m_roster)[static_cast<size_t>(m_leader)].name;
}

bool DungeonWorld::SetLeader(int member) {
	if (!m_roster || member < 0 || member >= static_cast<int>(m_roster->size())) return false;
	const Character& c = (*m_roster)[static_cast<size_t>(member)];
	if (member == m_leader) return true;
	if (!c.IsAlive()) {
		if (onMessage) onMessage(loc::FormatLine("log.leader_cannot", c.name));
		return false;
	}
	m_leader = member;
	MemberMessage(c, loc::FormatLine("log.leader_new", c.name));
	return true;
}

void DungeonWorld::PassLeadIfDown(bool announce) {
	if (!m_roster || m_roster->empty()) return;
	const int n = static_cast<int>(m_roster->size());
	if (m_leader < 0 || m_leader >= n) m_leader = 0;
	if ((*m_roster)[static_cast<size_t>(m_leader)].IsAlive()) return;
	// The next one standing, in roster order after the fallen leader. Nobody
	// standing: the lead stays put, and nobody acts (LeaderMember is null).
	for (int step = 1; step < n; ++step) {
		const int i = (m_leader + step) % n;
		const Character& c = (*m_roster)[static_cast<size_t>(i)];
		if (!c.IsAlive()) continue;
		m_leader = i;
		if (announce) MemberMessage(c, loc::FormatLine("log.leader_passes", c.name));
		return;
	}
}

} // namespace dungeon::game
