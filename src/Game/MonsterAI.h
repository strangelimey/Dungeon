// ============================================================================
// Game/MonsterAI.h — the monster brain, run ASYNCHRONOUSLY off the main thread.
//
// This is the single home for monster AI, walled off from the rest of the game
// like MagicSystem: the brain knows nothing about DungeonWorld, the Monster
// struct, the map, the party, the HUD, or audio. It reaches the world through
// one small read-only interface (ai::IWorldView) and flat data structs.
//
// THINKING vs ACTING are split, and THINKING runs on its OWN THREADS so a heavy
// re-plan never stalls the render/sim pipeline:
//   * Brain::Think + Brain::FindPath decide a monster's STANDING ORDERS — an
//     ai::Intent (idle / engage-toward-a-cell) plus a full chase PATH. Both are
//     PURE: they only read an immutable ai::Snapshot through an IWorldView and
//     write to outputs. That purity is what makes them safe to run on workers.
//   * ai::AsyncDirector owns one worker thread PER IQ BUCKET, each waking on its
//     own cadence (ai::Scheduler: 4 Hz down to 0.5 Hz). Each worker reads the
//     latest snapshot the main thread published and posts ai::Plan batches.
//   * The MAIN thread publishes the snapshot once per frame (cheap) and EXECUTES
//     the latest plans every frame at each monster's own move/attack cadence —
//     popping path cells, validating against LIVE occupancy, committing the
//     step, resolving attacks. All world mutation stays serial on the main
//     thread; the workers never touch live state.
// So a dim monster (slow bucket) still moves and swings at full speed; only its
// CHANGE OF MIND lags, and the cost of re-planning is paid on another core.
// ============================================================================
#pragma once

#include "Core/ThreadManager.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_set>
#include <vector>

namespace dungeon::ai {

// A grid cell, the unit of a path.
struct Cell {
	int x = 0;
	int z = 0;
};

// ----------------------------------------------------------------------------
// The world seam. Pathing needs only two read-only spatial questions; the
// snapshot-backed SnapshotView (below) answers them for the workers without
// touching the live world. `selfId` is unused by the snapshot view (a monster
// never re-enters its own start cell) but kept for interface generality.
// ----------------------------------------------------------------------------
class IWorldView {
public:
	virtual ~IWorldView() = default;
	// A cell a monster may step into: in bounds, map-walkable, not the party
	// cell, not held by another live monster.
	virtual bool CellFreeForMonster(int x, int z, int selfId) const = 0;
	// A cell is map-walkable. The goal cell qualifies even if occupied, so the
	// BFS can still target a monster's last-known party cell.
	virtual bool IsWalkable(int x, int z) const = 0;
};

// ----------------------------------------------------------------------------
// The monster as the brain sees it — a flat snapshot of the few fields thinking
// and pathing need. Everything else (timers, glide, facing, audio) the host
// owns and runs on the main thread.
// ----------------------------------------------------------------------------
struct Agent {
	u32 id = 0;              // host's STABLE monster id (never an array index — so a
							 // plan always matches the right monster, never a neighbour
							 // shifted in by an erase/compaction; 0 = none)
	int x = 0, z = 0;        // logical grid cell
	float aggroRange = 6.0f; // Chebyshev cells of party distance to engage at
	float iq = 100.0f;       // think-rate stat -> bucket (Scheduler::BucketForIq)
};

// ----------------------------------------------------------------------------
// An immutable picture of the world the main thread publishes each frame for the
// workers to read. Everything here is value/owned data (or an immutable shared
// grid) so a worker can read it on another thread with zero synchronisation.
// ----------------------------------------------------------------------------
struct Snapshot {
	int partyX = 0, partyZ = 0;  // the party's grid cell
	int mapW = 0, mapH = 0;      // map dimensions
	// Static walkability, shared across frames and only rebuilt when the map's
	// revision changes (so publishing a snapshot copies a pointer, not the grid).
	std::shared_ptr<const std::vector<uint8_t>> walkable;
	// Cells a monster may NOT enter beyond unwalkable: the party cell and every
	// live monster's cell. Indexed cell = z*mapW + x.
	std::unordered_set<int> blocked;
	std::vector<Agent> monsters; // the agents to think for (each carries a stable id)
};

// ----------------------------------------------------------------------------
// The monster's standing orders, computed by a worker and consumed by the main
// thread. `path` is the chase route from the monster's snapshot cell toward the
// target (excluding the start cell); the host pops it step by step, re-validating
// each cell against live occupancy. Between plan updates the monster keeps
// executing its cached path — that is what decouples move speed from think rate.
// ----------------------------------------------------------------------------
struct Intent {
	enum class Mode { Idle, Engage }; // Idle: hold position; Engage: chase + attack
	Mode mode = Mode::Idle;
	int targetX = 0, targetZ = 0; // chase goal: party cell at think time
};
struct Plan {
	u32 id = 0;  // STABLE id of the monster this plan is for (matched on consume;
				 // a plan whose monster is gone simply finds no match and is dropped)
	Intent intent;
	std::vector<Cell> path; // steps toward the target (start cell excluded)
};

// ----------------------------------------------------------------------------
// IQ-driven think-rate bucketing. Smart monsters land in a fast bucket (think
// often, react crisply), dim ones in a slow bucket. Acting is NOT bucketed —
// only thinking is — so move/attack speed stays governed by the monster's own
// cooldowns. Bucket 0 is fastest (~kFastestHz); slower buckets step down. The
// cadences are PRIME-millisecond (cicada-style coprime) values near each tier,
// so the buckets' fire times almost never coincide — see BucketInterval. Pure
// static helpers: workers self-pace by BucketInterval, the host maps IQ -> bucket.
// ----------------------------------------------------------------------------
struct Scheduler {
	static constexpr int kBucketCount = 4;
	static constexpr float kFastestHz = 4.0f; // nominal fastest tier (~4x/second)

	// Which bucket a monster with this IQ belongs to (0 = fastest).
	static int BucketForIq(float iq);
	// Seconds between re-thinks for bucket b: a coprime prime-ms value near the
	// tier rate (251/499/997/1999 ms), so buckets don't resonate (huge LCM).
	static float BucketInterval(int b);
};

// ----------------------------------------------------------------------------
// One monster's reasoning. Stateless except the BFS scratch it owns, so each
// worker thread keeps its OWN Brain (the scratch must not be shared).
// ----------------------------------------------------------------------------
class Brain {
public:
	// THINK (cheap, no pathing): pick the standing orders from what is perceived.
	Intent Think(const Agent& a, int partyX, int partyZ) const;

	// PATH (the meaty part): full 4-connected BFS route from the agent toward
	// (targetX,targetZ) over walkable, monster-free cells. Fills outPath with the
	// steps after the start cell; empty if already there or no path exists.
	// Polls `stop` periodically so a worst-case full-map BFS bails out PROMPTLY when
	// the worker is asked to stop (a Restart/Kill) — without that, a huge BFS can't
	// be cancelled cooperatively and the supervisor would force-terminate the thread
	// mid-allocation (the CRT-heap-lock deadlock StopOrTerminate warns about).
	void FindPath(const Agent& a, int targetX, int targetZ, int mapW, int mapH,
				  const IWorldView& world, const std::stop_token& stop,
				  std::vector<Cell>& outPath);

private:
	std::vector<int> m_pathFrom; // BFS predecessor scratch, reused across calls
};

// ----------------------------------------------------------------------------
// AsyncDirector — the AI's client of the engine thread manager (Core/Thread-
// Manager.h). It spawns one named worker per IQ bucket ("ai.bucket0".."3") at
// the bucket's cadence; each worker reads the latest published snapshot, thinks
// + paths its monsters, and publishes an immutable plan batch. The main thread
// Publish()es snapshots and TakePlans() to execute. Owning the threads through
// the manager means they are inspectable / killable / throttleable like every
// other engine thread. Handoffs are immutable shared_ptr swaps under brief
// mutexes, so the main thread never blocks on a worker's compute.
// ----------------------------------------------------------------------------
class AsyncDirector {
public:
	explicit AsyncDirector(threads::Manager& manager);
	~AsyncDirector(); // stops its workers before its captured state dies
	AsyncDirector(const AsyncDirector&) = delete;
	AsyncDirector& operator=(const AsyncDirector&) = delete;

	// Main thread: hand the workers the freshest view of the world.
	void Publish(std::shared_ptr<const Snapshot> snap);

	// Main thread: the most recent plan batch for a bucket, plus a sequence
	// number that increments on each publish (so the caller can tell new from
	// already-applied). Either field may be null/0 before the first compute.
	struct Batch {
		std::shared_ptr<const std::vector<Plan>> plans;
		uint64_t seq = 0;
	};
	Batch TakePlans(int bucket) const;

private:
	// One bucket's compute pass — the body the worker runs each tick (reads the
	// snapshot, thinks + paths this bucket's monsters, publishes a plan batch).
	// `brain` is per-worker scratch (the BFS buffer must not be shared). Checks
	// `stop` between monsters (and the BFS checks it internally) so a stop request
	// abandons the in-flight tick promptly, making cooperative Restart/Kill work
	// even under a heavy bucket — no force-terminate of an allocating worker.
	void ComputeBucket(int bucket, Brain& brain, const std::stop_token& stop);

	threads::Manager& m_manager;
	threads::WorkerId m_workers[Scheduler::kBucketCount];

	mutable std::mutex m_snapMutex;
	std::shared_ptr<const Snapshot> m_snapshot;

	mutable std::mutex m_planMutex;
	std::shared_ptr<const std::vector<Plan>> m_plans[Scheduler::kBucketCount];
	uint64_t m_planSeq[Scheduler::kBucketCount] = {};
};

// ----------------------------------------------------------------------------
// SnapshotView — an IWorldView backed by an immutable Snapshot, so a worker can
// path without ever touching the live world. Cheap to construct (holds a ref).
// ----------------------------------------------------------------------------
class SnapshotView : public IWorldView {
public:
	explicit SnapshotView(const Snapshot& s) : m_snap(s) {}
	bool CellFreeForMonster(int x, int z, int selfId) const override;
	bool IsWalkable(int x, int z) const override;

private:
	const Snapshot& m_snap;
};

} // namespace dungeon::ai
