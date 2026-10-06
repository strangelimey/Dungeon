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

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace dungeon::ai {

// A grid cell, the unit of a path.
struct Cell {
	int x = 0;
	int z = 0;
};

// ----------------------------------------------------------------------------
// THE HAND-BACK MARK a pooled buffer carries (code-review C70). A pool reuses a
// buffer once nobody else is reading it. That used to be read off
// `shared_ptr::use_count() == 1`, which the standard gives NO ordering: seeing
// the count drop says nothing about whether the reader's last loads of the
// buffer happen before the owner's next writes. (MSVC's interlocked decrement
// made it safe in practice; a rule the code relies on is not the toolchain's to
// keep.) Now a reader TAKES the mark while it is handed the buffer - under the
// hand-off mutex, so the take is ordered before any later publish - and GIVES it
// back with a RELEASE when it is done, and the owner reuses a buffer only once
// an ACQUIRE load reads the mark back at zero. That pair is exactly the
// happens-before a reuse needs, every reader's last read before the owner's
// first write. It COUNTS rather than flags because one snapshot is read by
// several workers at once. shared_ptr still carries the LIFETIME (a buffer the
// pool has let go of stays alive while a reader holds it); it no longer decides
// reuse. A force-killed worker never gives its mark back, so that buffer stays
// marked for good and its pool grows by one - and says so (see the pools).
// ----------------------------------------------------------------------------
class HandBack {
public:
	void Take() const { m_readers.fetch_add(1, std::memory_order_relaxed); }
	void Give() const { m_readers.fetch_sub(1, std::memory_order_release); }
	bool Idle() const { return m_readers.load(std::memory_order_acquire) == 0; }

private:
	mutable std::atomic<int> m_readers{0};
};

// ----------------------------------------------------------------------------
// The world seam. Pathing + perception need only read-only spatial questions;
// the snapshot-backed SnapshotView (below) answers them for the workers without
// touching the live world. `selfId` is unused by the snapshot view (a monster
// never re-enters its own start cell) but kept for interface generality.
// ----------------------------------------------------------------------------
class IWorldView {
public:
	virtual ~IWorldView() = default;
	// A cell a monster may step into: in bounds, map-walkable, not the party cell,
	// and with a free SLOT for a monster of this `capacity` (slots/cell of its size
	// — see Game/SlotGrid.h). A cell already holding a DIFFERENT size (different
	// capacity) is full to this monster; same-size cells admit it until their slots
	// fill. `capacity` is passed as a primitive so the AI layer stays game-agnostic.
	virtual bool CellFreeForMonster(int x, int z, int selfId, int capacity) const = 0;
	// A cell is map-walkable. The goal cell qualifies even if occupied, so the
	// BFS can still target a monster's last-known party cell.
	virtual bool IsWalkable(int x, int z) const = 0;
	// True if an unobstructed ORTHOGONAL line runs from (x0,z0) to (x1,z1): the two
	// must share a row or column (the grid is 4-directional — no diagonal sight),
	// with no map-blocking cell strictly BETWEEN them (endpoints never block).
	// Perception's "walls block sight" test; a monster sees/shoots only cardinally.
	virtual bool HasLineOfSight(int x0, int z0, int x1, int z1) const = 0;
};

// ----------------------------------------------------------------------------
// The behaviour STRATEGY a monster runs, selected as data (monsters.cat
// `archetype`). The brain picks an Intent mode from this; the host runs the
// matching executor. Keep Brute == 0 so an unset/legacy monster defaults to it.
//   Brute      — close and melee (the baseline).
//   Skirmisher — hold at range and shoot a plain bolt (kite).
//   Caster     — kite, but the bolt is a named spell (monsters.cat `spell`).
//   Swarm      — brute movement/melee, but senses OMNIDIRECTIONALLY (no blind
//                spot); meant for many small weak bodies.
//   Lurker     — ambush: dormant until the party comes within a SHORT trigger
//                range (or it's hit), then pursues at full aggro — relentless,
//                unlike a short-aggro brute that gives up when you back off.
//   Sentry     — a watchful guard: brute melee, but a WIDE sight cone; pairs with
//                a per-instance patrol route + leash (it walks a beat and returns).
// ----------------------------------------------------------------------------
enum class Archetype { Brute, Skirmisher, Caster, Swarm, Lurker, Sentry };

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
	int capacity = 1;        // slots per cell for this monster's size (Game/SlotGrid.h);
							 // pathing uses it to test whether a cell has a free slot
	int footprint = 1;       // edge length in CELLS (2 = Huge's 2x2 block); the BFS
							 // requires the whole footprint clear and self-excludes it
	bool aware = false;      // already noticed the party (sticky) — engages on range
							 // alone; an unaware directional monster needs the sight cone
	bool directional = true; // has a front (faces the party). false = omnidirectional
							 // sensing (a radially-symmetric blob has no blind spot)
	float facingYaw = 0.0f;  // current facing, for the sight-cone perception test
	int targetX = 0, targetZ = 0; // assigned chase goal (an attack cell around the
								  // party, or the party cell when unassigned) — the
								  // host's formation pass sets it; the BFS routes here
	Archetype archetype = Archetype::Brute; // behaviour strategy — picks Engage vs
											// Kite once the party is perceived
	float hpFrac = 1.0f;      // current hp / max hp — drives the flee decision
	float fleeBelow = 0.0f;   // flees when hpFrac drops below this (0 = never flees)
	// Per-instance overrides (.ent). asleep: dormant until close/hit (a per-placement
	// lurker). leashRange: cells from (leashX,leashZ) it will engage within; beyond
	// that it disengages (0 = unleashed).
	bool asleep = false;
	int leashX = 0, leashZ = 0;
	float leashRange = 0.0f;
};

// ----------------------------------------------------------------------------
// An immutable picture of the world the main thread publishes each frame for the
// workers to read. Everything here is value/owned data (or an immutable shared
// grid) so a worker can read it on another thread with zero synchronisation.
// ----------------------------------------------------------------------------
// Sub-cell occupancy of a single cell: how many monsters stand in it and the
// per-cell capacity of their (homogeneous) size. A cell is full to a newcomer
// when count == capacity, or when the newcomer's size differs (capacity mismatch).
struct CellOcc {
	uint8_t count = 0;
	uint8_t capacity = 1;
};

struct Snapshot {
	int partyX = 0, partyZ = 0;  // the party's grid cell
	int mapW = 0, mapH = 0;      // map dimensions
	// Static walkability, shared across frames and only rebuilt when the map's
	// revision changes (so publishing a snapshot copies a pointer, not the grid).
	std::shared_ptr<const std::vector<uint8_t>> walkable;
	// Hard-blocked cells a monster may NEVER enter (the party cell + solid
	// decorations and shut doors, which live outside the map so they can't ride
	// the cached walkable grid). A FLAT byte grid like `walkable` (mapW*mapH,
	// indexed z*mapW + x, 1 = blocked): the publisher zero-fills it in place so
	// a pooled snapshot's buffer is reused with no per-publish allocation (node
	// containers here freed on clear() and re-allocated every insert), and the
	// workers index it instead of hashing. Monster-vs-monster crowding is
	// capacity-based via `occ`, not a hard block, so several fit one cell.
	std::vector<uint8_t> blocked;
	// Live monster occupancy per cell (count + size capacity), for the slot-aware
	// CellFreeForMonster check — a flat grid shaped like `blocked`, zero-filled
	// per publish for the same reason. count == 0 = no monsters there.
	std::vector<CellOcc> occ;
	std::vector<Agent> monsters; // the agents to think for (each carries a stable id)
	// Taken by each worker tick (and the inline compute) that reads this snapshot,
	// for the length of the tick; the publisher reuses a pooled snapshot only once
	// it is unpublished and this reads idle (HandBack, C70).
	HandBack mark;
};

// ----------------------------------------------------------------------------
// The monster's standing orders, computed by a worker and consumed by the main
// thread. `path` is the chase route from the monster's snapshot cell toward the
// target (excluding the start cell); the host pops it step by step, re-validating
// each cell against live occupancy. Between plan updates the monster keeps
// executing its cached path — that is what decouples move speed from think rate.
// ----------------------------------------------------------------------------
struct Intent {
	// Idle: hold position. Engage: chase to melee (brute). Kite: hold at range and
	// attack (skirmisher/caster). Flee: break off and run from the party (a wounded
	// monster below its fleeBelow threshold, any archetype). Kite and Flee carry NO
	// path — the host executors drive them directly from live party position.
	enum class Mode { Idle, Engage, Kite, Flee };
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
// worker thread keeps its OWN Brain (the scratch must not be shared). A search
// allocates nothing once the scratch has met a map that size.
// ----------------------------------------------------------------------------
class Brain {
public:
	// THINK (cheap, no pathing): pick the standing orders from what is perceived.
	// Reads `world` for the line-of-sight perception test (walls block fresh sight);
	// stays pure (only reads the snapshot-backed view).
	Intent Think(const Agent& a, int partyX, int partyZ, const IWorldView& world) const;

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

	// Sizes the BFS scratch for a map of `cells` squares now, so no search on a
	// map that size grows it. FindPath grows it on its own at a first bigger
	// map; this is for the INLINE brain, whose searches run on the main thread
	// in guarded frames while resting (code-review C62), sized at level load.
	void Reserve(size_t cells);
	// Both scratch buffers' capacity, summed: it only ever grows, so a change
	// across a search means the search grew one (AsyncDirector::ComputeInline).
	size_t Capacity() const { return m_pathFrom.capacity() + m_open.capacity(); }

private:
	std::vector<int> m_pathFrom; // BFS predecessor scratch, reused across calls
	// The BFS open list, walked by a head index rather than popped. Every cell
	// enters it at most once (it is marked visited as it is pushed), so the
	// map's cell count is its true ceiling and, reserved, it never grows mid-
	// search. It was a std::queue, a deque that allocated on every search (C62).
	std::vector<int> m_open;
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
	// already-applied). `seq` is 0, and `plans` empty, before the first compute.
	// A Batch HOLDS its plans' hand-back mark (HandBack) until it is destroyed,
	// so the bucket's producer cannot refill them under a reader: keep one only
	// as long as it is being read. Move-only for that reason.
	//
	// `plans` is a VIEW of that tick's plans, not the vector they sit in: a
	// pooled batch keeps every plan slot it has ever filled, past the tick's
	// count, because destroying a plan frees its path and making one allocates
	// (in a debug build even an empty vector allocates its iterator proxy) -
	// which, in the inline compute, would land in a guarded frame. The view is
	// good while the Batch lives: it keeps the batch alive.
	struct Batch {
		std::span<const Plan> plans;
		uint64_t seq = 0;

		Batch() = default;
		Batch(Batch&& o) noexcept
			: plans(std::exchange(o.plans, {})), seq(o.seq), m_keep(std::move(o.m_keep)),
			  m_mark(std::exchange(o.m_mark, nullptr)) {}
		Batch& operator=(Batch&& o) noexcept {
			if (this != &o) {
				if (m_mark) m_mark->Give();
				plans = std::exchange(o.plans, {});
				seq = o.seq;
				m_keep = std::move(o.m_keep);
				m_mark = std::exchange(o.m_mark, nullptr);
			}
			return *this;
		}
		Batch(const Batch&) = delete;
		Batch& operator=(const Batch&) = delete;
		// Given back BEFORE `m_keep` lets go, while the batch is still alive.
		~Batch() {
			if (m_mark) m_mark->Give();
		}

	private:
		friend class AsyncDirector;
		std::shared_ptr<const std::vector<Plan>> m_keep; // the batch `plans` views
		const HandBack* m_mark = nullptr;
	};
	Batch TakePlans(int bucket) const;

	// Main thread, at level load and whenever a monster is added: size the
	// INLINE compute for a map of `cells` squares with `slots[b]` monsters in
	// bucket b - its brain's BFS scratch (Brain::Reserve) and its own plan
	// batches, a slot per monster with its path at the map's cell count (a
	// route cannot revisit a square, so that is the true ceiling). Lockstep runs
	// that compute in guarded frames - rest forces lockstep - so nothing it
	// touches may first grow there (code-review C62); a buffer that grows anyway
	// logs `AI pool grew:` once (ComputeInline). Never call it while holding a
	// Batch: it can move the plans one views. The WORKER pools are not sized
	// here: a worker grows its own on its own thread, which no guard arms, and
	// sizing them from this one would race the worker that owns them.
	void ReserveInline(size_t cells, const std::array<size_t, Scheduler::kBucketCount>& slots);

	// What the inline compute has done since lockstep last came on (`lockstep
	// stats`): AllocTest -Rest refuses a PASS unless the AI thought on the main
	// thread inside its window, and -RestReach unless a think found a path.
	struct InlineStats {
		uint64_t ticks = 0;  // bucket computes run inline
		uint64_t plans = 0;  // plans they published
		uint64_t paths = 0;  // of which carried a path
		size_t longest = 0;  // the longest of those paths, in squares
	};
	const InlineStats& Inline() const { return m_inlineStats; }

	// --- LOCKSTEP (the eval harness; docs/eval-harness.md) -------------------
	// OFF, the four bucket workers tick on WALL-CLOCK at their prime-millisecond
	// cadences. That is right for a game and useless for measurement: an eval
	// running thirty sim-seconds inside one frame would let its monsters think
	// perhaps twice, then report a confident number for a fight that never
	// happened.
	//
	// ON, the workers are PAUSED and the host drives ComputeInline() itself at
	// those same cadences counted in SIM time. It runs the very same
	// ComputeBucket the worker runs — not a reimplementation — so the two modes
	// cannot drift apart in WHAT they decide, only in when.
	//
	// BE HONEST ABOUT WHAT THIS IS. It is not a bit-exact reproduction of the
	// async mode and it cannot be: async plan latency depends on thread
	// scheduling, so "the same as async" is not a well-defined target to aim at.
	// This is the REPRODUCIBLE version — the same decisions at the same sim
	// cadence, with the wall-clock jitter taken out.
	void SetLockstep(bool on);
	bool Lockstep() const { return m_lockstep; }
	// Main thread, lockstep only: run one bucket's compute NOW. A no-op unless
	// lockstep is on, so a stray call can never race a running worker.
	void ComputeInline(int bucket);

private:
	// A pooled plan batch and the mark its readers hold (HandBack). Published as
	// an ALIASING shared_ptr to `plans`, which shares this object's control block
	// - no allocation - so the vector the consumer reads keeps the batch alive.
	struct PlanBatch {
		// Every plan SLOT the batch has ever had; the tick that built it filled
		// the first `count`, and a consumer sees only those (Batch::plans). The
		// vector NEVER SHRINKS: a tick planning for fewer monsters than the last
		// leaves the tail's plans standing, paths and all, because destroying one
		// frees its path and making one allocates - even an empty vector does in
		// a debug build, its iterator proxy - so a slot sized once stays sized.
		std::vector<Plan> plans;
		size_t count = 0;
		HandBack mark;

		// At least `slots` plan slots, each path holding `cells` squares
		// (ReserveInline). Main thread, never while a Batch of it is held: it can
		// move the plans, and a Batch views them.
		void Reserve(size_t slots, size_t cells);
		// How much the batch holds: its slots plus every buffer's capacity. None
		// of it ever shrinks, so a change across a tick means the tick grew - or
		// made - something.
		size_t Capacity() const;
	};

	// A bucket's plan batches and the producer that owns them: a pool is
	// touched by ONE thread only - so it needs no lock - and the batches it
	// can refill are the ones not on show whose mark reads idle.
	struct PlanPool {
		std::vector<std::shared_ptr<PlanBatch>> batches;
		const char* owner = ""; // "worker" / "inline", for its growth warning
		bool warned = false;    // that warning has been given
	};

	// One bucket's compute pass - the body the worker runs each tick (reads the
	// snapshot, thinks + paths this bucket's monsters, publishes a plan batch
	// drawn from `pool`, which it returns; null when it published nothing).
	// `brain` is per-worker scratch (the BFS buffer must not be shared). Checks
	// `stop` between monsters (and the BFS checks it internally) so a stop request
	// abandons the in-flight tick promptly, making cooperative Restart/Kill work
	// even under a heavy bucket — no force-terminate of an allocating worker.
	const PlanBatch* ComputeBucket(int bucket, Brain& brain, PlanPool& pool,
								   const std::stop_token& stop);
	// Logs `AI pool grew:` for a plan pool, once per pool; `what` says what grew.
	void WarnPlanPoolGrew(PlanPool& pool, int bucket, const char* what);

	threads::Manager& m_manager;
	threads::WorkerId m_workers[Scheduler::kBucketCount];

	// Lockstep state. The inline Brain is the director's OWN — a Brain carries
	// BFS scratch that must not be shared, and although the workers are paused
	// while lockstep is on, borrowing one of theirs would make that safety
	// depend on the pause actually having taken effect. The stop_source is never
	// requested: ComputeBucket wants a token so a worker can be cancelled
	// mid-BFS, and an inline call on the main thread has nobody to cancel it.
	bool m_lockstep = false;
	Brain m_inlineBrain;
	bool m_inlineBrainWarned = false; // its scratch grew in a compute, and said so
	std::stop_source m_neverStops;
	InlineStats m_inlineStats;

	mutable std::mutex m_snapMutex;
	std::shared_ptr<const Snapshot> m_snapshot;

	mutable std::mutex m_planMutex;
	std::shared_ptr<const std::vector<Plan>> m_plans[Scheduler::kBucketCount];
	uint64_t m_planSeq[Scheduler::kBucketCount] = {};
	// The batch m_plans[b] shows, and its mark, under m_planMutex: TakePlans
	// takes the mark there, and the producer reads which batch is out there.
	// Either pool's: a bucket's batches come from its worker's or the inline one.
	const PlanBatch* m_published[Scheduler::kBucketCount] = {};

	// The plan-batch pools, mirroring the host's snapshot pool: a producer
	// reuses a batch that is not the published one and whose mark is idle,
	// instead of make_shared-ing one per tick, and plan slots are overwritten in
	// place, so a steady-state tick allocates nothing. TWO per bucket, because a
	// pool must have ONE producer and a bucket has two: its worker, and the
	// inline compute lockstep runs on the main thread. They never share a batch -
	// not even when lockstep begins while the worker is still mid-tick, which it
	// can be, since Pause only sets a flag (C63, batch 34) - so neither pool
	// needs a lock or a claim, and the inline one can be sized at level load
	// from the main thread (ReserveInline) without racing anybody.
	//  * The WORKER's: filled in the constructor to kPlanPoolDepth - the
	//    published batch, one a consumer is still reading, the one being built.
	//    Its plans and paths grow as they first need to, on the worker's thread.
	//  * The INLINE one: kInlinePoolDepth - the published batch and the one being
	//    built. No consumer can be reading a third: the consumer is the main
	//    thread too, and ConsumeAIPlans drops every Batch it takes before the
	//    next frame's compute. Sized for the level's monsters (ReserveInline).
	// A pool that has to add a batch, or an inline one whose plans or paths
	// grow, warns once (`AI pool grew:`).
	static constexpr size_t kPlanPoolDepth = 3;
	static constexpr size_t kInlinePoolDepth = 2;
	PlanPool m_planPool[Scheduler::kBucketCount];
	PlanPool m_inlinePool[Scheduler::kBucketCount];
};

// ----------------------------------------------------------------------------
// SnapshotView — an IWorldView backed by an immutable Snapshot, so a worker can
// path without ever touching the live world. Cheap to construct (holds a ref).
// ----------------------------------------------------------------------------
class SnapshotView : public IWorldView {
public:
	explicit SnapshotView(const Snapshot& s) : m_snap(s) {}
	bool CellFreeForMonster(int x, int z, int selfId, int capacity) const override;
	bool IsWalkable(int x, int z) const override;
	bool HasLineOfSight(int x0, int z0, int x1, int z1) const override;

private:
	const Snapshot& m_snap;
};

} // namespace dungeon::ai
