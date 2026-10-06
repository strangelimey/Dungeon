// ============================================================================
// Game/MonsterAI.cpp — the monster brain + the async threading. See MonsterAI.h.
// ============================================================================
#include "Game/MonsterAI.h"

#include "Core/AllocTrack.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>
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
	if (!IsWalkable(x, z)) return false; // also rejects out-of-bounds cells
	const size_t idx = static_cast<size_t>(z) * m_snap.mapW + x;
	// Hard blocks (the party cell, solid decorations, shut doors) are never
	// enterable. Flat grids, indexed like `walkable` (the size guards only cover
	// a snapshot published before the grids were shaped — normally both span the
	// map, and IsWalkable has already bounds-checked the cell).
	if (idx < m_snap.blocked.size() && m_snap.blocked[idx] != 0) return false;
	// Empty cell: any single-cell monster fits. Occupied: only same-size groups
	// (matching capacity) admit a newcomer, and only until their slots fill. A
	// monster never re-enters its own start cell (the BFS marks it visited), so
	// self-exclusion isn't needed here.
	if (idx >= m_snap.occ.size()) return true;
	const CellOcc& o = m_snap.occ[idx];
	if (o.count == 0) return true;
	return o.capacity == capacity && o.count < o.capacity;
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

	// LEASH (per-instance): a monster pulled beyond leashRange cells from its anchor
	// breaks off — stay idle here so the host walks it home. Applies even to an aware
	// monster, so it can't be dragged across the level.
	if (a.leashRange > 0.0f) {
		const int fromAnchor = std::max(std::abs(a.x - a.leashX), std::abs(a.z - a.leashZ));
		if (static_cast<float>(fromAnchor) > a.leashRange) return it; // beyond leash: idle
	}

	// AMBUSH: a lurker archetype OR a per-instance `asleep` monster stays dormant
	// until the party comes within a short range (not full aggro); once sprung
	// (aware) it pursues at full aggro — relentless, unlike a short-aggro brute that
	// disengages when the party backs off. Everything else uses aggro from the start.
	constexpr float kAmbushTrigger = 2.0f; // cells: how close the prey must get
	const bool dormant = (a.archetype == Archetype::Lurker || a.asleep) && !a.aware;
	const float triggerRange = dormant ? kAmbushTrigger : a.aggroRange;
	if (static_cast<float>(dist) > triggerRange) return it; // out of range: idle

	bool perceived = a.aware; // sticky awareness engages regardless of sight
	if (!perceived && world.HasLineOfSight(a.x, a.z, partyX, partyZ)) {
		if (!a.directional) {
			perceived = true; // omnidirectional sensing, but the line must be clear
		} else {
			// Angle between the monster's facing and the direction to the party. Yaw
			// convention: forward = (sin yaw, cos yaw), so the bearing is atan2(dx,dz).
			constexpr float kPi = 3.14159265358979f;
			// A sentry is watchful — a wide ±90° (180° FOV) cone; others ±60° (120°).
			const float cone = a.archetype == Archetype::Sentry ? kPi * 0.5f : kPi / 3.0f;
			const float bearing = std::atan2(static_cast<float>(partyX - a.x),
											 static_cast<float>(partyZ - a.z));
			float d = bearing - a.facingYaw;
			while (d > kPi) d -= 2.0f * kPi;
			while (d < -kPi) d += 2.0f * kPi;
			perceived = std::abs(d) <= cone;
		}
	}
	if (perceived) {
		// A wounded monster below its flee threshold breaks off and RUNS, whatever
		// its archetype — a change of mind that's IQ-gated like any other (a dim
		// monster is slow to realise it should run). Flee wins over engage/kite.
		if (a.fleeBelow > 0.0f && a.hpFrac < a.fleeBelow) {
			it.mode = Intent::Mode::Flee;
		} else {
			// The archetype picks HOW it engages. A brute closes to melee toward its
			// ASSIGNED attack cell (the host's formation pass spreads monsters around
			// the party — surround; an unassigned monster targets the party cell). A
			// skirmisher OR caster kites: it holds at range and shoots (a bolt / a
			// spell), needing no chase path — the host keep-distance executor works
			// straight from live party position.
			const bool kites = a.archetype == Archetype::Skirmisher ||
							   a.archetype == Archetype::Caster;
			it.mode = kites ? Intent::Mode::Kite : Intent::Mode::Engage;
		}
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

	// Both scratch buffers at the map's full size, so neither grows inside the
	// search - a no-op once they have met a map this big (Reserve; C62).
	const size_t cells = static_cast<size_t>(W) * H;
	Reserve(cells);
	m_pathFrom.assign(cells, -1);
	m_open.clear();
	m_pathFrom[startIdx] = startIdx; // self-parent = visited sentinel
	m_open.push_back(startIdx);
	size_t head = 0; // the queue's front: m_open[head..] is still to expand

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
	while (head < m_open.size()) {
		if (++sinceStopCheck >= kStopCheckStride) {
			sinceStopCheck = 0;
			if (stop.stop_requested()) return;
		}
		const int cur = m_open[head++];
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
			m_open.push_back(nidx);
		}
	}
	if (!found) return;

	// Reconstruct goal -> start via predecessors, then emit start -> goal
	// (excluding the start cell) so the host can pop steps front-to-back.
	for (int cur = goalIdx; cur != startIdx; cur = m_pathFrom[cur])
		outPath.push_back({cur % W, cur / W});
	std::reverse(outPath.begin(), outPath.end());
}

void Brain::Reserve(size_t cells) {
	m_pathFrom.reserve(cells);
	m_open.reserve(cells);
}

// ----------------------------------------------------------------------------
// AsyncDirector — a client of the engine thread manager.
// ----------------------------------------------------------------------------

AsyncDirector::AsyncDirector(threads::Manager& manager) : m_manager(manager) {
	// Every plan pool at its full depth BEFORE any worker exists, so no tick - a
	// worker's, or an inline one in a guarded frame - ever has to add a batch
	// (see kPlanPoolDepth / kInlinePoolDepth). The inline batches' plans and
	// paths are sized later, for the level (ReserveInline).
	const auto fill = [](PlanPool& pool, size_t depth, const char* owner) {
		pool.owner = owner;
		pool.batches.reserve(depth + 1); // room for one growth without a regrow
		for (size_t i = 0; i < depth; ++i)
			pool.batches.push_back(std::make_shared<PlanBatch>());
	};
	for (int b = 0; b < Scheduler::kBucketCount; ++b) {
		fill(m_planPool[b], kPlanPoolDepth, "worker");
		fill(m_inlinePool[b], kInlinePoolDepth, "inline");
	}
	// One named worker per IQ bucket, ticking at that bucket's cadence. Each owns
	// a Brain (its BFS scratch) captured by value into the job, so the per-worker
	// state lives for the worker's lifetime without sharing.
	for (int b = 0; b < Scheduler::kBucketCount; ++b) {
		const float hz = 1.0f / Scheduler::BucketInterval(b);
		m_workers[b] = m_manager.Spawn(
			[this, b, brain = Brain{}](const threads::Tick& tick) mutable {
				ComputeBucket(b, brain, m_planPool[b], tick.stop);
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
	Batch out;
	out.seq = m_planSeq[bucket];
	// The reader's mark, taken under the lock that publishes: a producer that
	// has since published another batch sees this take (HandBack). The view is
	// of the slots the publishing tick filled, not every slot the batch holds.
	if (const PlanBatch* shown = m_published[bucket]) {
		shown->mark.Take();
		out.m_mark = &shown->mark;
		out.m_keep = m_plans[bucket];
		out.plans = std::span<const Plan>(shown->plans.data(), shown->count);
	}
	return out;
}

void AsyncDirector::SetLockstep(bool on) {
	if (on == m_lockstep) return;
	m_lockstep = on;
	if (on) m_inlineStats = {}; // `lockstep stats` counts from here
	// PAUSE, not stop-and-respawn: the workers keep their identities, iteration
	// counts and diagnostic rings, so a run that toggled lockstep still reads as
	// one thread's history in the console THREADS panel rather than as four
	// reboots. Pause only SETS A FLAG, though: a worker already mid-tick finishes
	// that tick, and can publish after the first inline compute (C63, batch 34).
	// It cannot write a buffer the inline compute is using - the inline compute
	// draws its plans from pools of its own - but "paused" is not yet "quiet".
	for (int b = 0; b < Scheduler::kBucketCount; ++b) {
		if (on) m_manager.Pause(m_workers[b]);
		else m_manager.Resume(m_workers[b]);
	}
}

void AsyncDirector::ReserveInline(size_t cells,
								  const std::array<size_t, Scheduler::kBucketCount>& slots) {
	// Main thread only, and so is everything this touches: the inline brain and
	// the inline pools are the main thread's alone (no worker ever sees them),
	// so sizing them here races nobody. A batch on show keeps its plans exactly
	// as published - PlanBatch::Reserve adds slots past its `count`.
	m_inlineBrain.Reserve(cells);
	for (int b = 0; b < Scheduler::kBucketCount; ++b)
		for (const auto& batch : m_inlinePool[b].batches) batch->Reserve(slots[b], cells);
}

void AsyncDirector::ComputeInline(int bucket) {
	// Guarded rather than asserted, and the guard is the point: a caller that
	// forgot to enable lockstep gets NOTHING, instead of a main-thread compute
	// racing four live workers for the snapshot and the plans on show.
	if (!m_lockstep) return;
	// This runs on the main thread, in frames the allocation guard arms while
	// resting, so everything it touches was sized for the level (ReserveInline):
	// the brain's scratch and this bucket's inline batches. A buffer that grows
	// anyway says so ONCE, in whichever frame it landed - which is what lets
	// AllocTest see a missing pre-size even when the first growth falls outside
	// its window (a `rest on` typed into the console, say).
	PlanPool& pool = m_inlinePool[bucket];
	const auto poolCapacity = [&pool] {
		size_t sum = 0;
		for (const auto& batch : pool.batches) sum += batch->Capacity();
		return sum;
	};
	const size_t brainBefore = m_inlineBrain.Capacity();
	const size_t poolBefore = poolCapacity();
	const PlanBatch* built =
		ComputeBucket(bucket, m_inlineBrain, pool, m_neverStops.get_token());
	if (m_inlineBrain.Capacity() != brainBefore && !m_inlineBrainWarned) {
		m_inlineBrainWarned = true;
		alloc::Excused excuse; // the report's own formatting, not the growth
		log::Warn("AI pool grew: the inline brain's search scratch, in bucket {}'s "
				  "compute (sized at level load - AsyncDirector::ReserveInline)",
				  bucket);
	}
	if (poolCapacity() != poolBefore)
		WarnPlanPoolGrew(pool, bucket, "its plans or their paths grew");
	if (!built) return;
	++m_inlineStats.ticks;
	for (const Plan& plan : std::span<const Plan>(built->plans.data(), built->count)) {
		++m_inlineStats.plans;
		if (plan.path.empty()) continue;
		++m_inlineStats.paths;
		m_inlineStats.longest = std::max(m_inlineStats.longest, plan.path.size());
	}
}

void AsyncDirector::WarnPlanPoolGrew(PlanPool& pool, int bucket, const char* what) {
	if (pool.warned) return;
	pool.warned = true;
	// The report excuses ITSELF (log::Write formats a string); the growth it
	// reports does not, so a guarded frame still counts it.
	alloc::Excused excuse;
	log::Warn("AI pool grew: bucket {}'s {} plan pool - {} (pool of {} batches; "
			  "see ai::AsyncDirector's plan pools)",
			  bucket, pool.owner, what, pool.batches.size());
}

void AsyncDirector::PlanBatch::Reserve(size_t slots, size_t cells) {
	// Growing the slots past `count` leaves the plans a consumer sees alone, so
	// a batch on show may be sized too; it is the main thread's, as its consumer is.
	if (plans.size() < slots) plans.resize(slots);
	for (Plan& p : plans) p.path.reserve(cells);
}

size_t AsyncDirector::PlanBatch::Capacity() const {
	size_t sum = plans.size() + plans.capacity();
	for (const Plan& p : plans) sum += p.path.capacity();
	return sum;
}

const AsyncDirector::PlanBatch* AsyncDirector::ComputeBucket(int bucket, Brain& brain,
															 PlanPool& pool,
															 const std::stop_token& stop) {
	// One tick: grab the freshest snapshot (a cheap shared_ptr copy), think +
	// path this bucket's monsters, publish the batch. The manager owns the loop
	// and the cadence; this is just the unit of work.
	std::shared_ptr<const Snapshot> snap;
	{
		std::lock_guard<std::mutex> lk(m_snapMutex);
		snap = m_snapshot;
		// The snapshot's mark, for the length of this tick: taken under the lock
		// the publisher replaces it under, so the publisher cannot think it free
		// while this tick reads it (HandBack). Given back on EVERY way out,
		// including a stop request's early return.
		if (snap) snap->mark.Take();
	}
	if (!snap) return nullptr;
	struct GiveBack {
		const HandBack& mark;
		~GiveBack() { mark.Give(); }
	} const reading{snap->mark};

	SnapshotView view(*snap);
	// Reuse a pooled batch that is not the one out on show and that no reader
	// still marks (HandBack - the consumer gives its mark back when it drops its
	// Batch). Plan slots are overwritten in place, so each plan's path vector
	// keeps its capacity across ticks - a steady-state tick allocates nothing.
	// `pool` is this producer's alone (see the plan pools), so no other thread
	// can be filling the batch picked here.
	const PlanBatch* shown = nullptr;
	{
		std::lock_guard<std::mutex> lk(m_planMutex);
		shown = m_published[bucket];
	}
	std::shared_ptr<PlanBatch> out;
	for (const auto& p : pool.batches)
		if (p.get() != shown && p->mark.Idle()) {
			out = p;
			break;
		}
	if (!out) {
		// Past the pool's depth: a reader is holding more than the one batch the
		// depth allows for, or never gave one back. Grow, and say so once - the
		// growth is NOT excused, so a guard sees it.
		out = std::make_shared<PlanBatch>();
		pool.batches.push_back(out);
		WarnPlanPoolGrew(pool, bucket, "a batch added (a reader kept its mark - see ai::HandBack)");
	}
	std::vector<Plan>& plans = out->plans;
	size_t used = 0;
	for (const Agent& m : snap->monsters) {
		// Abandon the whole tick on a stop request — don't publish a partial batch.
		// Combined with the BFS's own poll, this caps how long a worker can ignore a
		// Restart/Kill (one monster's BFS), so the supervisor never has to force-
		// terminate a heavy bucket mid-allocation (the heap-lock deadlock hazard).
		if (stop.stop_requested()) return nullptr;
		if (Scheduler::BucketForIq(m.iq) != bucket) continue;
		// A slot beyond every one the batch has had: made here, which allocates
		// (see PlanBatch::plans). The inline batches were given a slot per monster
		// at level load, so for them this is the growth ComputeInline reports.
		if (plans.size() == used) plans.emplace_back();
		Plan& plan = plans[used++];
		plan.id = m.id;
		plan.intent = brain.Think(m, snap->partyX, snap->partyZ, view);
		plan.path.clear();
		if (plan.intent.mode == Intent::Mode::Engage)
			brain.FindPath(m, plan.intent.targetX, plan.intent.targetZ, snap->mapW,
						   snap->mapH, view, stop, plan.path);
	}
	// The slots this tick did not use STAY, with their paths' capacity: a
	// consumer sees only the first `count` (TakePlans), and a shrink would free
	// what the next bigger tick must then allocate again.
	out->count = used;
	std::lock_guard<std::mutex> lk(m_planMutex);
	// An ALIASING pointer to the vector inside the pooled batch: it shares the
	// batch's control block, so publishing allocates nothing and the vector a
	// consumer reads keeps its batch alive.
	m_plans[bucket] = std::shared_ptr<const std::vector<Plan>>(out, &out->plans);
	m_published[bucket] = out.get();
	++m_planSeq[bucket];
	return out.get();
}

} // namespace dungeon::ai
