// ============================================================================
// Game/MonsterAI.cpp — the monster brain + the async threading. See MonsterAI.h.
// ============================================================================
#include "Game/MonsterAI.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <string>

namespace dungeon::ai {

// ----------------------------------------------------------------------------
// Scheduler — IQ -> think-rate mapping (pure helpers).
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

// ----------------------------------------------------------------------------
// SnapshotView — IWorldView over an immutable Snapshot.
// ----------------------------------------------------------------------------

bool SnapshotView::IsWalkable(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_snap.mapW || z >= m_snap.mapH) return false;
	const auto& w = m_snap.walkable;
	return w && (*w)[static_cast<size_t>(z) * m_snap.mapW + x] != 0;
}

bool SnapshotView::CellFreeForMonster(int x, int z, int /*selfId*/) const {
	if (!IsWalkable(x, z)) return false;
	// `blocked` already carries the party cell and every live monster's cell. A
	// monster never re-enters its own start cell (the BFS marks it visited), so
	// self-exclusion isn't needed here.
	return m_snap.blocked.find(z * m_snap.mapW + x) == m_snap.blocked.end();
}

// ----------------------------------------------------------------------------
// Brain — pure thinking + pathing.
// ----------------------------------------------------------------------------

Intent Brain::Think(const Agent& a, int partyX, int partyZ) const {
	// Cheap, infrequent: engage when the party is within aggro range and lock the
	// chase goal to its CURRENT cell. Execution keeps pathing toward this snapshot
	// until the next think, so a dim monster lumbers toward where the party WAS.
	Intent it;
	const int dist = std::max(std::abs(a.x - partyX), std::abs(a.z - partyZ));
	if (static_cast<float>(dist) <= a.aggroRange) {
		it.mode = Intent::Mode::Engage;
		it.targetX = partyX;
		it.targetZ = partyZ;
	}
	return it;
}

void Brain::FindPath(const Agent& a, int targetX, int targetZ, int mapW, int mapH,
					 const IWorldView& world, std::vector<Cell>& outPath) {
	outPath.clear();
	const int W = mapW, H = mapH;
	if (W <= 0 || H <= 0) return;
	// Defensive bounds check: a worker must never index its scratch out of range,
	// even if a snapshot hands it a cell outside the (possibly just-swapped) map.
	if (a.x < 0 || a.x >= W || a.z < 0 || a.z >= H) return;
	if (targetX < 0 || targetX >= W || targetZ < 0 || targetZ >= H) return;
	if (a.x == targetX && a.z == targetZ) return; // already at the goal
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
			// The goal (last-known party cell) is reachable as the target even if
			// occupied now; every other cell must be free for a monster to walk.
			if (nidx != goalIdx && !world.CellFreeForMonster(nx, nz, a.id)) continue;
			if (nidx == goalIdx && !world.IsWalkable(nx, nz)) continue;
			m_pathFrom[nidx] = cur;
			open.push(nidx);
		}
	}
	if (!found) return;

	// Reconstruct goal -> start via predecessors, then emit start -> goal
	// (excluding the start cell) so the host can pop steps front-to-back.
	for (int cur = goalIdx; cur != startIdx; cur = m_pathFrom[cur])
		outPath.push_back({cur % W, cur / W});
	std::reverse(outPath.begin(), outPath.end());
}

// ----------------------------------------------------------------------------
// AsyncDirector — a client of the engine thread manager.
// ----------------------------------------------------------------------------

AsyncDirector::AsyncDirector(threads::Manager& manager) : m_manager(manager) {
	// One named worker per IQ bucket, ticking at that bucket's cadence. Each owns
	// a Brain (its BFS scratch) captured by value into the job, so the per-worker
	// state lives for the worker's lifetime without sharing.
	for (int b = 0; b < Scheduler::kBucketCount; ++b) {
		const float hz = 1.0f / Scheduler::BucketInterval(b);
		m_workers[b] = m_manager.Spawn(
			[this, b, brain = Brain{}](const threads::Tick&) mutable {
				ComputeBucket(b, brain);
			},
			{"ai.bucket" + std::to_string(b), hz, /*watchdogMs=*/100});
	}
}

AsyncDirector::~AsyncDirector() {
	// Stop (and JOIN) our workers before this object's captured state dies — the
	// job closures reference `this`. Stop blocks, so once it returns no worker
	// can call ComputeBucket again.
	for (int b = 0; b < Scheduler::kBucketCount; ++b)
		m_manager.Stop(m_workers[b]);
}

void AsyncDirector::Publish(std::shared_ptr<const Snapshot> snap) {
	std::lock_guard<std::mutex> lk(m_snapMutex);
	m_snapshot = std::move(snap);
}

AsyncDirector::Batch AsyncDirector::TakePlans(int bucket) const {
	std::lock_guard<std::mutex> lk(m_planMutex);
	return {m_plans[bucket], m_planSeq[bucket]};
}

void AsyncDirector::ComputeBucket(int bucket, Brain& brain) {
	// One tick: grab the freshest snapshot (a cheap shared_ptr copy), think +
	// path this bucket's monsters, publish the batch. The manager owns the loop
	// and the cadence; this is just the unit of work.
	std::shared_ptr<const Snapshot> snap;
	{
		std::lock_guard<std::mutex> lk(m_snapMutex);
		snap = m_snapshot;
	}
	if (!snap) return;

	SnapshotView view(*snap);
	auto out = std::make_shared<std::vector<Plan>>();
	for (const Agent& m : snap->monsters) {
		if (Scheduler::BucketForIq(m.iq) != bucket) continue;
		Plan plan;
		plan.id = m.id;
		plan.gen = snap->gen;
		plan.intent = brain.Think(m, snap->partyX, snap->partyZ);
		if (plan.intent.mode == Intent::Mode::Engage)
			brain.FindPath(m, plan.intent.targetX, plan.intent.targetZ, snap->mapW,
						   snap->mapH, view, plan.path);
		out->push_back(std::move(plan));
	}
	std::lock_guard<std::mutex> lk(m_planMutex);
	m_plans[bucket] = std::move(out);
	++m_planSeq[bucket];
}

} // namespace dungeon::ai
