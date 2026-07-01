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
	// Cicada-style cadences: PRIME-millisecond intervals near each tier (~4/2/1/
	// 0.5 Hz) instead of power-of-two ones. Power-of-two intervals (0.25/0.5/1/2s)
	// are harmonic — their LCM is 2s, so all four buckets fire together every 2s.
	// Prime, mutually-coprime intervals have an enormous LCM, so the buckets'
	// fire times almost never coincide — no periodic thundering herd. (Each runs
	// on its own thread, so this is cheap insurance more than a hot fix; the
	// global governor scales these uniformly and so preserves the coprimality.)
	static constexpr int kMs[kBucketCount] = {251, 499, 997, 1999};
	return kMs[std::clamp(b, 0, kBucketCount - 1)] / 1000.0f;
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

bool SnapshotView::CellFreeForMonster(int x, int z, int /*selfId*/, int capacity) const {
	if (!IsWalkable(x, z)) return false;
	const int idx = z * m_snap.mapW + x;
	// Hard blocks (the party cell) are never enterable.
	if (m_snap.blocked.find(idx) != m_snap.blocked.end()) return false;
	// Empty cell: any single-cell monster fits. Occupied: only same-size groups
	// (matching capacity) admit a newcomer, and only until their slots fill. A
	// monster never re-enters its own start cell (the BFS marks it visited), so
	// self-exclusion isn't needed here.
	auto it = m_snap.occ.find(idx);
	if (it == m_snap.occ.end()) return true;
	return it->second.capacity == capacity && it->second.count < it->second.capacity;
}

bool SnapshotView::HasLineOfSight(int x0, int z0, int x1, int z1) const {
	// ORTHOGONAL-only sight: the dungeon is a 4-directional grid, so a monster sees
	// (and shoots) the party only straight down a shared row or column — never
	// diagonally (matches the party's cardinal spell bolts and orthogonal melee). A
	// non-axis-aligned pair has no line at all. On a shared axis, every cell BETWEEN
	// the endpoints must be walkable; the endpoints themselves never block.
	if (x0 == x1 && z0 == z1) return true;
	if (x0 == x1) {
		const int s = z0 < z1 ? 1 : -1;
		for (int z = z0 + s; z != z1; z += s)
			if (!IsWalkable(x0, z)) return false;
		return true;
	}
	if (z0 == z1) {
		const int s = x0 < x1 ? 1 : -1;
		for (int x = x0 + s; x != x1; x += s)
			if (!IsWalkable(x, z0)) return false;
		return true;
	}
	return false; // not on a shared row/column — no orthogonal line
}

// ----------------------------------------------------------------------------
// Brain — pure thinking + pathing.
// ----------------------------------------------------------------------------

Intent Brain::Think(const Agent& a, int partyX, int partyZ, const IWorldView& world) const {
	// Cheap, infrequent: engage when the party is PERCEIVED, and lock the chase
	// goal to its CURRENT cell. Execution keeps pathing toward this snapshot until
	// the next think, so a dim monster lumbers toward where the party WAS.
	//
	// Perception (the sneak mechanic): being in range is necessary but not always
	// sufficient. An already-aware monster (sticky, set by a prior notice or a hit)
	// engages on range alone — it has noticed and gives chase even around corners.
	// FRESH detection instead needs an unobstructed line to the party (walls block
	// sight) AND, for a directional monster, the party inside its frontal sight
	// cone (±kSightCone); an omnidirectional monster (no blind spot) still needs
	// the clear line. So the party can creep up from behind a group, or stay behind
	// a wall, and they remain oblivious until they're seen or take a swing.
	Intent it;
	const int dist = std::max(std::abs(a.x - partyX), std::abs(a.z - partyZ));
	if (static_cast<float>(dist) > a.aggroRange) return it; // out of range: idle

	bool perceived = a.aware; // sticky awareness engages regardless of sight
	if (!perceived && world.HasLineOfSight(a.x, a.z, partyX, partyZ)) {
		if (!a.directional) {
			perceived = true; // omnidirectional sensing, but the line must be clear
		} else {
			// Angle between the monster's facing and the direction to the party. Yaw
			// convention: forward = (sin yaw, cos yaw), so the bearing is atan2(dx,dz).
			constexpr float kPi = 3.14159265358979f;
			constexpr float kSightCone = kPi / 3.0f; // ±60° → a 120° frontal field of view
			const float bearing = std::atan2(static_cast<float>(partyX - a.x),
											 static_cast<float>(partyZ - a.z));
			float d = bearing - a.facingYaw;
			while (d > kPi) d -= 2.0f * kPi;
			while (d < -kPi) d += 2.0f * kPi;
			perceived = std::abs(d) <= kSightCone;
		}
	}
	if (perceived) {
		// The archetype picks HOW it engages. A brute closes to melee toward its
		// ASSIGNED attack cell (the host's formation pass spreads monsters around the
		// party — surround; an unassigned monster targets the party cell itself). A
		// skirmisher kites: it holds at range and shoots, so it needs no chase path —
		// the host keep-distance executor works straight from live party position.
		it.mode = a.archetype == Archetype::Skirmisher ? Intent::Mode::Kite
													   : Intent::Mode::Engage;
		it.targetX = a.targetX;
		it.targetZ = a.targetZ;
	}
	return it;
}

// True if a monster with agent `a`'s footprint can anchor at (ax,az): every cell
// of its f x f block is free for it — EXCEPT cells inside the agent's CURRENT
// footprint, which are itself. A Huge's 2x2 footprint overlaps between adjacent
// anchors, so without this self-exclusion it could never take a step. For a
// single-cell monster (f == 1) this is exactly one CellFreeForMonster check.
static bool FootprintFree(const Agent& a, int ax, int az, const IWorldView& world) {
	const int f = a.footprint < 1 ? 1 : a.footprint;
	for (int fz = az; fz < az + f; ++fz)
		for (int fx = ax; fx < ax + f; ++fx) {
			const bool self = fx >= a.x && fx < a.x + f && fz >= a.z && fz < a.z + f;
			if (self) continue;
			if (!world.CellFreeForMonster(fx, fz, static_cast<int>(a.id), a.capacity))
				return false;
		}
	return true;
}

void Brain::FindPath(const Agent& a, int targetX, int targetZ, int mapW, int mapH,
					 const IWorldView& world, const std::stop_token& stop,
					 std::vector<Cell>& outPath) {
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
	// Poll the stop token every kStopCheckStride pops, not every pop: stop_requested
	// is a cheap atomic load, but a worst-case BFS expands tens of thousands of
	// cells, so amortise it. On a stop request, abandon the search at once (outPath
	// stays empty — the plan is simply not produced) so the worker can exit its tick
	// and be cooperatively rebooted instead of force-terminated mid-allocation.
	constexpr int kStopCheckStride = 1024;
	int sinceStopCheck = 0;
	while (!open.empty()) {
		if (++sinceStopCheck >= kStopCheckStride) {
			sinceStopCheck = 0;
			if (stop.stop_requested()) return;
		}
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
			if (nidx == goalIdx) {
				if (!world.IsWalkable(nx, nz)) continue;
			} else if (!FootprintFree(a, nx, nz, world)) {
				continue;
			}
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
			[this, b, brain = Brain{}](const threads::Tick& tick) mutable {
				ComputeBucket(b, brain, tick.stop);
			},
			{"ai.bucket" + std::to_string(b), hz, /*watchdogMs=*/100,
			 /*autoRestart=*/true, /*priority=*/-1}); // below-normal: AI is background work
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

void AsyncDirector::ComputeBucket(int bucket, Brain& brain, const std::stop_token& stop) {
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
		// Abandon the whole tick on a stop request — don't publish a partial batch.
		// Combined with the BFS's own poll, this caps how long a worker can ignore a
		// Restart/Kill (one monster's BFS), so the supervisor never has to force-
		// terminate a heavy bucket mid-allocation (the heap-lock deadlock hazard).
		if (stop.stop_requested()) return;
		if (Scheduler::BucketForIq(m.iq) != bucket) continue;
		Plan plan;
		plan.id = m.id;
		plan.intent = brain.Think(m, snap->partyX, snap->partyZ, view);
		if (plan.intent.mode == Intent::Mode::Engage)
			brain.FindPath(m, plan.intent.targetX, plan.intent.targetZ, snap->mapW,
						   snap->mapH, view, stop, plan.path);
		out->push_back(std::move(plan));
	}
	std::lock_guard<std::mutex> lk(m_planMutex);
	m_plans[bucket] = std::move(out);
	++m_planSeq[bucket];
}

} // namespace dungeon::ai
