// ============================================================================
// Game/MonsterAI.cpp — the monster brain implementation. See MonsterAI.h.
// ============================================================================
#include "Game/MonsterAI.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace dungeon::ai {

// ----------------------------------------------------------------------------
// Scheduler — IQ-driven decision-rate bucketing. See MonsterAI.h.
// ----------------------------------------------------------------------------

float Scheduler::BucketInterval(int b) {
	// Bucket 0 runs at kFastestHz; each step down halves the rate (doubles the
	// interval): 0.25s, 0.5s, 1.0s, 2.0s for the default 4-bucket / 4 Hz setup.
	return (1.0f / kFastestHz) * static_cast<float>(1 << b);
}

int Scheduler::BucketForIq(float iq) {
	// Smarter monsters think more often (a lower/faster bucket). Thresholds are
	// deliberately coarse and easy to retune — the default ~100 monster lands in
	// bucket 1 (2 Hz). Tune alongside the catalog `iq` values later.
	if (iq >= 130.0f) return 0;
	if (iq >= 100.0f) return 1;
	if (iq >= 70.0f) return 2;
	return kBucketCount - 1;
}

Scheduler::Scheduler() {
	// Phase-stagger the accumulators so the buckets don't all fire on the same
	// frame (a small thundering-herd guard); they drift apart from here. Detail
	// to refine later — e.g. spreading monsters WITHIN a bucket across frames.
	for (int b = 0; b < kBucketCount; ++b)
		m_acc[b] = BucketInterval(b) * static_cast<float>(b) / kBucketCount;
}

unsigned Scheduler::Tick(float dt) {
	unsigned due = 0;
	for (int b = 0; b < kBucketCount; ++b) {
		const float iv = BucketInterval(b);
		m_acc[b] += dt;
		if (m_acc[b] >= iv) {
			due |= 1u << b;
			m_acc[b] -= iv;            // carry the remainder so the rate holds
			if (m_acc[b] >= iv) m_acc[b] = 0.0f; // but never let a hitch back up
		}
	}
	return due;
}

Intent Brain::Think(const Agent& a, const Perception& p) {
	// Cheap, infrequent: just pick the standing orders. Engage when the party is
	// within aggro range, and lock the chase goal to the party's CURRENT cell —
	// execution will keep pathing toward this snapshot until the next think, so a
	// dim monster lumbers toward where the party WAS. No pathfinding here.
	Intent it;
	const int dx = std::abs(a.x - p.partyX);
	const int dz = std::abs(a.z - p.partyZ);
	const int dist = std::max(dx, dz); // Chebyshev cells to the party
	if (static_cast<float>(dist) <= a.aggroRange) {
		it.mode = Intent::Mode::Engage;
		it.targetX = p.partyX;
		it.targetZ = p.partyZ;
	}
	return it;
}

bool Brain::NextStep(const Agent& a, int targetX, int targetZ, int mapW, int mapH,
					 const IWorldView& world, int& outX, int& outZ) {
	const int W = mapW, H = mapH;
	if (W <= 0 || H <= 0) return false;
	if (a.x == targetX && a.z == targetZ) return false; // already at the goal
	const int startIdx = a.z * W + a.x;
	const int goalIdx = targetZ * W + targetX;

	m_pathFrom.assign(static_cast<size_t>(W) * H, -1);
	std::queue<int> open;
	m_pathFrom[startIdx] = startIdx; // self-parent = visited sentinel
	open.push(startIdx);

	static constexpr int kDX[4] = {1, -1, 0, 0};
	static constexpr int kDZ[4] = {0, 0, 1, -1};
	bool found = false;
	while (!open.empty()) {
		const int cur = open.front();
		open.pop();
		if (cur == goalIdx) { found = true; break; }
		const int cx = cur % W, cz = cur / W;
		for (int dir = 0; dir < 4; ++dir) {
			const int nx = cx + kDX[dir], nz = cz + kDZ[dir];
			if (nx < 0 || nz < 0 || nx >= W || nz >= H) continue;
			const int nidx = nz * W + nx;
			if (m_pathFrom[nidx] != -1) continue; // already visited
			// The goal (last-known party cell) is reachable as the BFS target even
			// if occupied now; every other cell must be free for a monster to walk.
			if (nidx != goalIdx && !world.CellFreeForMonster(nx, nz, a.id)) continue;
			if (nidx == goalIdx && !world.IsWalkable(nx, nz)) continue;
			m_pathFrom[nidx] = cur;
			open.push(nidx);
		}
	}
	if (!found) return false;

	// Walk the predecessors back from the goal to the first step off the start.
	int cur = goalIdx;
	while (m_pathFrom[cur] != startIdx) cur = m_pathFrom[cur];
	outX = cur % W;
	outZ = cur / W;
	return true;
}

} // namespace dungeon::ai
