// ============================================================================
// tools/ThreadStress/Main.cpp — a stress harness for the engine thread system.
//
// Drives the REAL Core/ThreadManager + Game/MonsterAI AsyncDirector (the four
// IQ-bucket worker threads) the same way DungeonWorld does: publish an immutable
// Snapshot each "frame", let the workers think + path, drain the plans. The only
// thing synthetic is the world — we fabricate snapshots with a controlled number
// of monsters PER IQ BUCKET and a controlled BFS cost (map size + reachability),
// so we can load each bucket independently and watch how the manager holds up:
// per-tick timing, effective think-rate vs the bucket's nominal cadence, watchdog
// stalls, the global governor, and cooperative kill + supervised restart.
//
// Faithful to the live wiring (DungeonWorld::BuildAISnapshot):
//   * one IQ per bucket DERIVED from Scheduler::BucketForIq, never a copy of
//     its thresholds;
//   * every monster aware with a huge aggroRange, so every one engages and
//     pays a BFS, chasing the PARTY'S CELL (the formation pass aims monsters
//     at that cell or a side of it);
//   * `blocked` holds the party cell, and the monsters go into `occ` (count +
//     size capacity), the way the game crowds them - not into `blocked`.
// Every plan batch the workers publish is AUDITED against the snapshot it was
// thought from, twice over. WHOLE: a batch of this snapshot plans every monster
// of its bucket exactly once, nothing else, and every bucket that has monsters
// publishes one. PATHS: in a reachable phase each plan is an engage with a real
// path (4-connected, over walkable cells, round other monsters, ending on the
// party); in a walled-off phase none finds one. Before this the targets were
// never set, so every monster chased (0,0) - a border wall - and every search
// explored the whole map and failed: the "reachable" phases measured the worst
// case, and path reconstruction and the pooled path vectors never ran.
//
// NOTE on safety: the AI workers carry watchdogMs=100, autoRestart=true, so the
// supervisor reboots any tick that runs past 5x the watchdog (500 ms). If that
// tick cannot stop cooperatively, the reboot's StopOrTerminate path falls through
// to TerminateThread — and terminating a thread mid-allocation leaks the CRT heap
// lock and can deadlock the process (see ThreadManager StopOrTerminate's warning).
// Now that Brain::FindPath + AsyncDirector::ComputeBucket poll their stop token,
// an overloaded tick bails within one BFS, so the reboot is cooperative. Phase D
// DELIBERATELY drives bucket 0 past the 500 ms line to exercise that path and
// asserts the worker reboots cleanly (restart counter climbs, never quarantined,
// no "FORCE-TERMINATED" warning) — it would have risked the deadlock before.
//
// Phase G enters LOCKSTEP while bucket 0 is caught mid-tick (code-review C63):
// SetLockstep must return only once every worker holds at its pause point, no
// worker may publish after it, and a worker rebooted under lockstep must come
// back still paused.
// ============================================================================
#include "Common/Verdict.h"
#include "Game/MonsterAI.h"
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/ThreadManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace dungeon;
using Clock = std::chrono::steady_clock;
static double SinceMs(Clock::time_point t) {
	return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static constexpr int kBuckets = ai::Scheduler::kBucketCount;

// ---------------------------------------------------------------------------
// The verdict. This harness used to compute every pass/fail condition below —
// including the one it exists for, "was the worker force-terminated" — print it
// as prose, and then `return 0` regardless. A test that cannot fail guards
// nothing, and this one was in the tree looking like it did. Each condition is
// now asserted, counted, and rolled into one machine-readable line so a suite
// can run it unattended - the shared one (tools/Common/Verdict.h), which also
// keeps the failed labels --self-test compares against kSelfTestFails.
// ---------------------------------------------------------------------------
static void Check(bool ok, const std::string& what) { verdict::Check(ok, what, "    "); }

// ---------------------------------------------------------------------------
// THE SELF-TEST'S EXPECTED FAILURES, by label (the tools/SpellTest.py rule: the
// self-test passes only when exactly these fail and every other check passes).
// --self-test injects two faults, and each must be caught where it lands:
//   * a worker that ignores its stop token, so the supervisor force-terminates
//     it - only the health-record check can see that (the state scans read a
//     flag Restart clears), so it is the one listed for it;
//   * C418's bug put back: every monster's chase target left at (0,0), a border
//     wall. Every REACHABLE path audit must then find plans engaged with no
//     path; the walled-off ones still find none, as they should, and every
//     batch is still whole - so all of those stay green.
// A label listed twice would be a check that runs twice and must fail both.
// ---------------------------------------------------------------------------
static constexpr const char* kSelfTestFails[] = {
	"A: every plan engaged, with a real path to the party",
	"B: every plan engaged, with a real path to the party",
	"D: every recovery plan engaged, with a real path to the party",
	"E: every plan engaged, with a real path to the party",
	"F: every plan engaged, with a real path to the party",
	"nothing was force-terminated all run (from the health record)",
};

// The second injected fault (see above): leave Agent::targetX/Z at (0,0).
static bool g_oldTargets = false;

// ---------------------------------------------------------------------------
// One IQ per think bucket, DERIVED from Scheduler::BucketForIq rather than
// copied from it: scan the IQ range and take the middle of each bucket's band,
// so a retuned threshold carries these along. (They were a hand copy -
// 140/110/80/50 - that would have quietly loaded the wrong buckets the day the
// thresholds moved.) False if a bucket has no IQ in the scanned range, or its
// band's middle does not map back into it (a non-monotone mapping).
// ---------------------------------------------------------------------------
static float g_bucketIq[kBuckets] = {};

static bool DeriveBucketIqs() {
	constexpr int kMaxIq = 400;
	int lo[kBuckets], hi[kBuckets];
	for (int b = 0; b < kBuckets; ++b) lo[b] = hi[b] = -1;
	for (int q = 0; q <= kMaxIq; ++q) {
		const int b = ai::Scheduler::BucketForIq(static_cast<float>(q));
		if (b < 0 || b >= kBuckets) continue;
		if (lo[b] < 0) lo[b] = q;
		hi[b] = q;
	}
	bool ok = true;
	for (int b = 0; b < kBuckets; ++b) {
		if (lo[b] < 0) {
			ok = false;
			continue;
		}
		g_bucketIq[b] = 0.5f * static_cast<float>(lo[b] + hi[b]);
		if (ai::Scheduler::BucketForIq(g_bucketIq[b]) != b) ok = false;
	}
	return ok;
}

// ---------------------------------------------------------------------------
// Synthetic world. Build an immutable ai::Snapshot with `counts[b]` monsters in
// IQ bucket b, on a WxH open map, every one chasing the party's cell.
// `reachable=false` walls the party's 8 neighbours so that cell can never be
// reached => every engaged monster pays a FULL-map BFS (the worst case) and
// finds nothing. Monsters scatter on a lattice (step >= 2) so they never box
// each other or the party in, and each engages (aware, aggroRange huge).
//
// Agent ids come from one counter for the whole run, so no two snapshots share
// an id: a batch a worker thought from an EARLIER snapshot names monsters the
// current one does not have, and the audit can tell it apart (DungeonWorld drops
// such a plan, its monster being gone - see AuditBatch for when it is allowed).
// ---------------------------------------------------------------------------
static u32 g_nextAgentId = 1;

static std::shared_ptr<const ai::Snapshot>
BuildSnapshot(int W, int H, const int counts[4], bool reachable) {
	auto snap = std::make_shared<ai::Snapshot>();
	snap->mapW = W;
	snap->mapH = H;
	snap->partyX = W / 2;
	snap->partyZ = H / 2;

	auto walk = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(W) * H, uint8_t{1});
	auto at = [&](int x, int z) -> uint8_t& { return (*walk)[static_cast<size_t>(z) * W + x]; };
	for (int x = 0; x < W; ++x) { at(x, 0) = 0; at(x, H - 1) = 0; }      // border walls
	for (int z = 0; z < H; ++z) { at(0, z) = 0; at(W - 1, z) = 0; }
	if (!reachable) // isolate the party cell so the BFS explores the whole map and fails
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				if (dx || dz) at(snap->partyX + dx, snap->partyZ + dz) = 0;
	snap->walkable = walk;

	// The flat per-publish grids (sized like `walkable` - see ai::Snapshot). The
	// party cell is the one hard block here; the monsters go into `occ` below.
	snap->blocked.assign(static_cast<size_t>(W) * H, 0);
	snap->occ.assign(static_cast<size_t>(W) * H, ai::CellOcc{});
	snap->blocked[static_cast<size_t>(snap->partyZ) * W + snap->partyX] = 1; // party cell
	int total = 0;
	for (int b = 0; b < 4; ++b) total += counts[b];
	if (total <= 0) return snap;

	// Scatter on a coarse lattice across the interior so monsters stay apart.
	const int step = std::max(2, static_cast<int>(std::sqrt(
								   static_cast<double>(W - 2) * (H - 2) / (total + 1))));
	snap->monsters.reserve(total);
	int b = 0, placedInB = 0;
	for (int z = 1; z < H - 1 && static_cast<int>(snap->monsters.size()) < total; z += step)
		for (int x = 1; x < W - 1 && static_cast<int>(snap->monsters.size()) < total; x += step) {
			if ((*walk)[static_cast<size_t>(z) * W + x] == 0) continue;
			if (x == snap->partyX && z == snap->partyZ) continue;
			while (b < 4 && placedInB >= counts[b]) { ++b; placedInB = 0; }
			if (b >= 4) break;
			ai::Agent a;
			a.id = g_nextAgentId++;
			a.x = x;
			a.z = z;
			a.iq = g_bucketIq[b];
			// The chase goal: the party's cell. DungeonWorld's formation pass
			// aims an unaware monster there and an aware one at a side of the
			// party; the party's cell is the one goal every monster here can
			// share. Left at (0,0) - a border wall FindPath can never accept -
			// every search explored the whole map and failed (C418; the
			// self-test's fault).
			if (!g_oldTargets) {
				a.targetX = snap->partyX;
				a.targetZ = snap->partyZ;
			}
			a.aggroRange = 1e9f; // force engage => every monster runs a BFS
			// AND `aware`, which is what actually makes that true now. This
			// harness was written when a huge aggroRange was enough; ai::Agent
			// has since grown a perception model (archetypes, a sight cone,
			// `directional`), and an UNAWARE directional monster facing yaw 0
			// must pass the cone test before it engages. Almost none did — so
			// the load phases were driving 4488 monsters that paid no BFS at
			// all, at 0.4 ms a tick, while printing a table that looked fine.
			// Nothing caught it because this harness always returned 0.
			a.aware = true; // sticky "has noticed the party": engages on range alone
			snap->monsters.push_back(a);
			// Crowding is capacity-based, as in the game: the cell's occupant
			// count, tagged with the size's slots per cell. A full cell (here one
			// single-slot monster) turns other monsters away through
			// CellFreeForMonster's occupancy test, not through `blocked`.
			ai::CellOcc& o = snap->occ[static_cast<size_t>(z) * W + x];
			o.capacity = static_cast<uint8_t>(a.capacity);
			++o.count;
			++placedInB;
		}
	return snap;
}

// ---------------------------------------------------------------------------
// The audit: every new plan batch, judged against the snapshot it was meant
// for, as a WHOLE and plan by plan.
//
// THE BATCH. A plan is matched to its monster by id. A batch of this snapshot
// must plan every monster of its bucket exactly once and nothing else - no
// monster twice, none of another bucket's, none missing, and no plan of an
// earlier snapshot beside them (a pooled batch that kept a bigger tick's tail).
// A batch with nothing of this snapshot in it was thought from an earlier one,
// or is empty; either is allowed only as the FIRST after a baseline (a tick in
// flight when the snapshot changed - a worker runs one tick at a time, and any
// tick that starts later reads the new snapshot), or when the bucket has no
// monsters here. And every bucket that has monsters must publish at least one
// whole batch, so a bucket that publishes nothing, or only empties, fails too.
//
// THE PLAN. A path counts as REACHING the party only if it is a real walk: each
// step one orthogonal move from the last, over walkable cells, never through
// the party's cell or another monster's square before its end, and ending on
// the party.
// ---------------------------------------------------------------------------
struct PathAudit {
	uint64_t lastSeq[kBuckets] = {}; // the last batch audited, per bucket
	bool mayBeStale[kBuckets] = {};  // the next batch may predate the snapshot
	bool owes[kBuckets] = {};        // an audited snapshot had monsters in this bucket

	// Plans, from every batch: those matched to a monster of the snapshot.
	int plans = 0;
	int engaged = 0;   // Intent::Engage - the only mode that paths
	int reached = 0;   // a real path ending on the party
	int empty = 0;     // engaged, no path
	int malformed = 0; // a path, but not a real walk to the party

	// Batches, each judged whole.
	int whole[kBuckets] = {}; // planned every monster of its bucket, each once
	int stale = 0;            // the first after a baseline, of an earlier snapshot
	int bad = 0;              // anything else; the first is described below
	char firstBad[200] = {};
	std::vector<uint8_t> seen; // scratch: which monsters a batch has planned

	// About to drive a new snapshot: the batch standing now is not audited, and
	// the next one may still be the old snapshot's.
	void Baseline(int b, uint64_t seq) {
		lastSeq[b] = seq;
		mayBeStale[b] = true;
	}
};

// Across the whole run: a plan must come from the bucket its monster's IQ maps
// to, or the director thought for the wrong worker.
static int g_auditedPlans = 0;
static int g_wrongBucket = 0;

static const ai::Agent* AgentFor(const ai::Snapshot& s, u32 id) {
	if (s.monsters.empty()) return nullptr;
	const u32 first = s.monsters.front().id; // ids are contiguous per snapshot
	if (id < first) return nullptr;
	const size_t i = id - first;
	return i < s.monsters.size() && s.monsters[i].id == id ? &s.monsters[i] : nullptr;
}

static bool PathReachesParty(const ai::Snapshot& s, const ai::Agent& a,
							 const std::vector<ai::Cell>& path) {
	if (path.empty()) return false;
	int px = a.x, pz = a.z;
	for (size_t i = 0; i < path.size(); ++i) {
		const ai::Cell c = path[i];
		if (std::abs(c.x - px) + std::abs(c.z - pz) != 1) return false;
		if (c.x < 0 || c.z < 0 || c.x >= s.mapW || c.z >= s.mapH) return false;
		const size_t idx = static_cast<size_t>(c.z) * s.mapW + c.x;
		if (!s.walkable || (*s.walkable)[idx] == 0) return false;
		const bool last = i + 1 == path.size();
		if (!last && (s.blocked[idx] != 0 || s.occ[idx].count != 0)) return false;
		px = c.x;
		pz = c.z;
	}
	return px == s.partyX && pz == s.partyZ;
}

static void AuditBatch(const ai::Snapshot& s, int bucket,
					   const ai::AsyncDirector::Batch& batch, PathAudit& pa) {
	// What this bucket owes: one plan per monster of it in this snapshot. Noted
	// on every call, not only when a batch arrives - a bucket that never
	// publishes owes just the same.
	int owed = 0;
	for (const ai::Agent& m : s.monsters)
		if (ai::Scheduler::BucketForIq(m.iq) == bucket) ++owed;
	if (owed > 0) pa.owes[bucket] = true;

	if (batch.seq == 0 || batch.seq == pa.lastSeq[bucket]) return; // none yet / seen
	pa.lastSeq[bucket] = batch.seq;
	const bool mayBeStale = pa.mayBeStale[bucket];
	pa.mayBeStale[bucket] = false;

	const std::span<const ai::Plan> plans = batch.plans;
	pa.seen.assign(s.monsters.size(), 0);
	int fresh = 0, older = 0, twice = 0, foreign = 0;
	for (const ai::Plan& plan : plans) {
		const ai::Agent* a = AgentFor(s, plan.id);
		if (!a) {
			++older; // an earlier snapshot's monster
			continue;
		}
		++fresh;
		uint8_t& seen = pa.seen[static_cast<size_t>(a - s.monsters.data())];
		if (seen) ++twice;
		seen = 1;
		++pa.plans;
		++g_auditedPlans;
		if (ai::Scheduler::BucketForIq(a->iq) != bucket) {
			++g_wrongBucket;
			++foreign;
		}
		if (plan.intent.mode != ai::Intent::Mode::Engage) continue;
		++pa.engaged;
		if (plan.path.empty()) ++pa.empty;
		else if (PathReachesParty(s, *a, plan.path)) ++pa.reached;
		else ++pa.malformed;
	}

	// The batch, whole.
	const char* why = nullptr;
	if (fresh == 0) {
		if (owed == 0 && plans.empty()) return; // owed nothing, planned nothing
		if (mayBeStale) {
			++pa.stale; // a tick that was in flight when the snapshot changed
			return;
		}
		why = plans.empty() ? "is empty, though its bucket has monsters"
							: "is an earlier snapshot's, though this one was already out";
	} else if (older > 0) {
		why = "mixes this snapshot's plans with an earlier one's";
	} else if (twice > 0) {
		why = "plans a monster more than once";
	} else if (foreign > 0) {
		why = "plans another bucket's monsters";
	} else if (fresh != owed) {
		why = "misses some of its bucket's monsters";
	}
	if (!why) {
		++pa.whole[bucket];
		return;
	}
	if (pa.bad++ == 0)
		std::snprintf(pa.firstBad, sizeof(pa.firstBad),
					  "bucket %d batch #%llu %s (%d plans: %d of this snapshot, %d older, "
					  "%d repeats, %d foreign; %d owed)",
					  bucket, static_cast<unsigned long long>(batch.seq), why,
					  static_cast<int>(plans.size()), fresh, older, twice, foreign, owed);
}

// Every bucket that owed plans published a whole batch, and at least one did.
static bool Covered(const PathAudit& pa) {
	bool any = false;
	for (int b = 0; b < kBuckets; ++b) {
		if (!pa.owes[b]) continue;
		if (pa.whole[b] == 0) return false;
		any = true;
	}
	return any;
}

static void ReportAudit(const PathAudit& pa, const char* what = "paths") {
	std::printf("  %s: %d plans audited - %d engaged, %d reached the party, %d empty, "
				"%d malformed\n",
				what, pa.plans, pa.engaged, pa.reached, pa.empty, pa.malformed);
	std::printf("    batches: whole b0=%d b1=%d b2=%d b3=%d, %d stale skipped, %d bad\n",
				pa.whole[0], pa.whole[1], pa.whole[2], pa.whole[3], pa.stale, pa.bad);
	for (int b = 0; b < kBuckets; ++b)
		if (pa.owes[b] && pa.whole[b] == 0)
			std::printf("    bucket %d has monsters and published no whole batch\n", b);
	if (pa.bad > 0) std::printf("    first bad: %s\n", pa.firstBad);
}

// The batch verdict: every batch whole, and every bucket that owed plans
// published one - so every monster was planned, once, by its own bucket.
static void CheckBatches(const PathAudit& pa, const std::string& label) {
	Check(Covered(pa) && pa.bad == 0, label);
}

// The two path verdicts, over the plans. Both demand plans were audited at all
// and that every plan engaged (each monster is aware with a huge aggroRange), so
// neither can pass on an empty run. With CheckBatches they say it of every
// monster.
static void CheckReached(const PathAudit& pa, const std::string& label) {
	Check(pa.plans > 0 && pa.engaged == pa.plans && pa.reached == pa.plans, label);
}
static void CheckNoPath(const PathAudit& pa, const std::string& label) {
	Check(pa.plans > 0 && pa.engaged == pa.plans && pa.empty == pa.plans, label);
}

// ---------------------------------------------------------------------------
// Resolve the four bucket worker ids by name (AsyncDirector names them
// "ai.bucket0".."ai.bucket3"). They are the only workers in this harness.
// ---------------------------------------------------------------------------
static void ResolveBuckets(threads::Manager& mgr, threads::WorkerId out[4]) {
	for (int b = 0; b < 4; ++b) out[b] = threads::kInvalidWorker;
	for (const auto& w : mgr.SnapshotAll()) {
		const std::string want = "ai.bucket";
		if (w.name.rfind(want, 0) == 0) {
			const int b = w.name.back() - '0';
			if (b >= 0 && b < 4) out[b] = w.id;
		}
	}
}

struct PhaseStats {
	long long iterStart[4] = {};
	uint64_t seqStart[4] = {};
	double peakLast[4] = {};
	int stallSamples[4] = {};
	int samples = 0;
};

// Publish `snap` every ~5 ms (a ~200 Hz "frame loop"), drain and audit plans, and
// sample each bucket's live Inspect() to catch transient stalls and peak tick
// times. The batch already standing when the phase starts is not audited: it was
// thought from the previous snapshot (and the next may be too - see AuditBatch).
static void DrivePhase(threads::Manager& mgr, ai::AsyncDirector& dir,
					   const threads::WorkerId ids[4],
					   std::shared_ptr<const ai::Snapshot> snap, double seconds,
					   PhaseStats& st, PathAudit& pa) {
	for (int b = 0; b < 4; ++b) {
		const auto info = mgr.Inspect(ids[b]);
		st.iterStart[b] = static_cast<long long>(info.iterations);
		st.seqStart[b] = dir.TakePlans(b).seq;
		st.peakLast[b] = 0.0;
		pa.Baseline(b, st.seqStart[b]);
	}
	const auto t0 = Clock::now();
	while (SinceMs(t0) < seconds * 1000.0) {
		dir.Publish(snap);
		for (int b = 0; b < 4; ++b) {
			const auto info = mgr.Inspect(ids[b]);
			if (info.state == threads::State::Stalled) st.stallSamples[b]++;
			st.peakLast[b] = std::max(st.peakLast[b], info.lastMs);
			AuditBatch(*snap, b, dir.TakePlans(b), pa);
		}
		st.samples++;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

static void ReportPhase(threads::Manager& mgr, ai::AsyncDirector& dir,
						const threads::WorkerId ids[4], const int counts[4],
						double seconds, const PhaseStats& st) {
	std::printf("  bucket  nomHz  monst |  ticks  effHz  avgMs  peakMs  maxMs | "
				"plans/s  stalls  state    re\n");
	for (int b = 0; b < 4; ++b) {
		const auto info = mgr.Inspect(ids[b]);
		const long long ticks = static_cast<long long>(info.iterations) - st.iterStart[b];
		const uint64_t plans = dir.TakePlans(b).seq - st.seqStart[b];
		const double nomHz = 1.0 / ai::Scheduler::BucketInterval(b);
		std::printf("    %d    %5.2f  %5d | %6lld  %5.2f  %5.1f  %6.1f  %5.1f | "
					"%6.2f  %6d  %-8s %2u\n",
					b, nomHz, counts[b], ticks, ticks / seconds, info.avgMs,
					st.peakLast[b], info.maxMs, plans / seconds, st.stallSamples[b],
					threads::StateName(info.state), info.restarts);
	}
}

int main(int argc, char** argv) {
	// This harness prints em-dashes; a console decodes bytes in its own code
	// page unless told otherwise (Core/Log.h).
	dungeon::log::UseUtf8Console();
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// --self-test INVERTS THE VERDICT, like AllocTest / Bc7Test / HealthTest: it
	// plants a worker that CANNOT be stopped cooperatively, so the supervisor's
	// reboot falls through to TerminateThread - the exact outcome this harness
	// exists to rule out - and it puts C418's bug back (every chase target at
	// (0,0), a wall), which the path audits exist to rule out. The run must come
	// back FAIL, and fail exactly the checks in kSelfTestFails: a checker nobody
	// has watched fail is a checker nobody should trust, and this harness spent a
	// long time returning 0 unconditionally while a whole phase measured nothing.
	bool selfTest = false;
	for (int i = 1; i < argc; ++i)
		if (std::string(argv[i]) == "--self-test") selfTest = true;
	g_oldTargets = selfTest;

	std::printf("=== Thread-system stress harness =================================\n");
	std::printf("HW concurrency: %u threads\n", std::thread::hardware_concurrency());
	if (selfTest)
		std::printf("SELF-TEST: a wedged worker + every chase target at (0,0); expecting "
					"exactly %d named checks to FAIL\n",
					static_cast<int>(std::size(kSelfTestFails)));
	std::printf("\n");

	// The IQ each bucket's monsters carry, from the live mapping.
	const bool iqsOk = DeriveBucketIqs();
	std::printf("bucket IQs (from Scheduler::BucketForIq): %.1f/%.1f/%.1f/%.1f\n",
				g_bucketIq[0], g_bucketIq[1], g_bucketIq[2], g_bucketIq[3]);
	Check(iqsOk, "every think bucket has an IQ that BucketForIq maps into it");
	std::printf("\n");

	// The health record is where the durable evidence of a force-terminate
	// lives; see the final check.
	diag::Init();

	threads::Manager mgr;

	if (selfTest) {
		// Ignores its stop token, so cooperative stop cannot end it. The
		// supervisor reboots it once its tick passes 5x the watchdog, and the
		// reboot's 250 ms grace expires, and it is force-terminated.
		//
		// Safe to do deliberately: this job only sleeps. It holds no lock and
		// allocates nothing, so terminating it leaks nothing that matters — the
		// hazard StopOrTerminate warns about is a thread killed mid-malloc.
		mgr.Spawn(
			[](const threads::Tick&) {
				while (true) std::this_thread::sleep_for(std::chrono::milliseconds(50));
			},
			{"stress.wedged", 1.0f, /*watchdogMs=*/200, /*autoRestart=*/true});
		std::printf("  planted 'stress.wedged' (ignores its stop token)\n\n");
	}
	ai::AsyncDirector dir(mgr);
	threads::WorkerId ids[4];
	ResolveBuckets(mgr, ids);
	std::printf("AI workers: bucket0=#%u bucket1=#%u bucket2=#%u bucket3=#%u\n",
				ids[0], ids[1], ids[2], ids[3]);
	std::printf("nominal cadences: %.0f/%.0f/%.0f/%.0f ms (%.2f/%.2f/%.2f/%.2f Hz)\n\n",
				ai::Scheduler::BucketInterval(0) * 1000, ai::Scheduler::BucketInterval(1) * 1000,
				ai::Scheduler::BucketInterval(2) * 1000, ai::Scheduler::BucketInterval(3) * 1000,
				1 / ai::Scheduler::BucketInterval(0), 1 / ai::Scheduler::BucketInterval(1),
				1 / ai::Scheduler::BucketInterval(2), 1 / ai::Scheduler::BucketInterval(3));

	// `tag` is the phase's letter, which prefixes its check labels.
	auto phase = [&](const char* tag, const char* title, int W, int H, const int counts[4],
					 bool reach, double secs) {
		std::printf("--- %s %s  (map %dx%d, %s) ---\n", tag, title, W, H,
					reach ? "reachable" : "UNREACHABLE/full-BFS");
		auto snap = BuildSnapshot(W, H, counts, reach);
		PhaseStats st;
		PathAudit pa;
		DrivePhase(mgr, dir, ids, snap, secs, st, pa);
		ReportPhase(mgr, dir, ids, counts, secs, st);
		ReportAudit(pa);
		// The floor every load phase has to clear: no amount of work may end with
		// a worker force-terminated. Quarantine means cooperative stop failed,
		// which is the deadlock hazard this whole design avoids.
		bool anyQuarantined = false;
		for (int b = 0; b < 4; ++b)
			if (mgr.Inspect(ids[b]).state == threads::State::Quarantined)
				anyQuarantined = true;
		const std::string t = std::string(tag) + ": ";
		Check(!anyQuarantined, t + "no bucket was force-terminated under this load");
		// And the work was the work the phase claims: every monster planned, once,
		// by its own bucket - with a real path where the party can be reached, and
		// none where it is walled off.
		CheckBatches(pa, t + "every bucket's batches planned each of its monsters exactly once");
		if (reach) CheckReached(pa, t + "every plan engaged, with a real path to the party");
		else CheckNoPath(pa, t + "every plan engaged and found no path to the walled-off party");
		std::printf("\n");
	};

	// Phase A — baseline: a handful of monsters per bucket, small reachable map.
	{ int c[4] = {2, 2, 2, 2}; phase("A", "baseline", 24, 24, c, true, 3.0); }

	// Phase B — ASYMMETRIC load: each bucket gets a very different monster count,
	// to prove the per-bucket isolation + coprime cadences (one hot bucket must
	// not starve the others). Reachable, medium map.
	{ int c[4] = {120, 8, 40, 4}; phase("B", "asymmetric per-bucket", 64, 64, c, true, 4.0); }

	// Phase C — heavy but bounded: a big map, full-BFS (unreachable), moderate
	// counts. Each engaged monster explores the whole reachable region.
	{ int c[4] = {30, 30, 30, 30}; phase("C", "heavy full-BFS", 96, 96, c, false, 4.0); }

	// Phase D — push bucket 0 PAST the knee, INTO the supervisor's force-reboot zone
	// (a tick over 5x watchdog = 500 ms), to prove the BFS now stops COOPERATIVELY.
	// Before the stop token was threaded through ComputeBucket/FindPath, a worst-case
	// full-map BFS could not be cancelled mid-flight, so the supervisor's reboot path
	// (StopOrTerminate) fell through its 250 ms grace and FORCE-terminated the worker
	// — and a TerminateThread on a thread mid-allocation leaks the CRT heap lock and
	// can deadlock the whole process. That is why this ramp USED to stop short of
	// 500 ms. Now an overloaded tick polls its stop token (every BFS + between
	// monsters) and bails within one BFS, so the supervisor reboots the slot CLEANLY:
	// the restart counter climbs, the state never reaches 'quarantd', and NO
	// "FORCE-TERMINATED" warning is logged. We ramp until a tick clears the 500 ms
	// reboot line AND the supervisor has rebooted bucket 0, then confirm recovery.
	std::printf("--- D ramp bucket0 PAST the knee into the 500ms reboot zone (160x160, full-BFS) ---\n");
	std::printf("  monst |  effHz  avgMs  peakMs | stalls  re  state\n");
	{
		const int W = 160, H = 160;
		const double rebootZoneMs = 500.0; // 5x watchdog = the supervisor force-reboot line
		const u32 reStart = mgr.Inspect(ids[0]).restarts;
		bool rebooted = false, quarantined = false;
		PathAudit ramp; // every step's batches, each against its own snapshot
		int n = 20;
		for (int stepi = 0; stepi < 16; ++stepi) {
			int c[4] = {n, 0, 0, 0};
			auto snap = BuildSnapshot(W, H, c, false);
			PhaseStats st;
			DrivePhase(mgr, dir, ids, snap, 2.0, st, ramp);
			const auto info = mgr.Inspect(ids[0]);
			const long long ticks = static_cast<long long>(info.iterations) - st.iterStart[0];
			const double effHz = ticks >= 0 ? ticks / 2.0 : 0.0;
			rebooted = info.restarts > reStart;
			quarantined = info.state == threads::State::Quarantined;
			std::printf("  %5d |  %5.2f  %5.1f  %6.1f | %6d  %2u  %-8s\n", n, effHz,
						info.avgMs, st.peakLast[0], st.stallSamples[0], info.restarts,
						threads::StateName(info.state));
			// Stop once a tick has crossed the reboot line AND the supervisor has
			// actually rebooted the worker (cooperatively, we expect) — or if it ever
			// gets quarantined (the failure we are guarding against).
			if ((st.peakLast[0] > rebootZoneMs && rebooted) || quarantined) break;
			n = (n < 80) ? n + 20 : static_cast<int>(n * 1.4);
		}
		ReportAudit(ramp, "ramp paths");
		// Recover on a trivial load so the final state isn't caught mid-reboot, and
		// confirm plans flow again after the overload (a wedged/quarantined worker
		// would produce none) - and are REAL plans: paths again, on a map where the
		// party can be reached. The window is at least the old 0.8 s and runs on
		// until bucket 0 has published a whole batch for THIS snapshot, capped at
		// 5 s: a tick still finishing the ramp's last snapshot publishes a batch
		// for monsters that are gone, which the audit skips as stale.
		uint64_t recoverPlans = 0;
		double recoverMs = 0.0;
		PathAudit recovery;
		{
			int cc[4] = {2, 0, 0, 0};
			auto light = BuildSnapshot(24, 24, cc, true);
			const uint64_t seq0 = dir.TakePlans(0).seq;
			for (int b = 0; b < 4; ++b) recovery.Baseline(b, dir.TakePlans(b).seq);
			const auto t0 = Clock::now();
			while (true) {
				recoverMs = SinceMs(t0);
				if (recoverMs >= 5000.0 || (recoverMs >= 800.0 && Covered(recovery))) break;
				dir.Publish(light);
				for (int b = 0; b < 4; ++b) AuditBatch(*light, b, dir.TakePlans(b), recovery);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			recoverPlans = dir.TakePlans(0).seq - seq0;
		}
		const auto info = mgr.Inspect(ids[0]);
		std::printf("  verdict: bucket0 restarts %u->%u, final state '%s', recovery plans/%.1fs=%llu\n",
					reStart, info.restarts, threads::StateName(info.state), recoverMs / 1000.0,
					static_cast<unsigned long long>(recoverPlans));
		ReportAudit(recovery, "recovery paths");
		// The facts this phase exists to establish, in order of severity.
		Check(!quarantined,
			  "D: bucket0 was NOT force-terminated (cooperative stop survived the overload)");
		Check(rebooted, "D: the ramp reached the reboot zone and the supervisor rebooted it");
		Check(recoverPlans > 0, "D: plans resumed after the reboot (the worker really recovered)");
		CheckBatches(ramp, "D: the ramp's batches planned each of bucket0's monsters exactly once");
		CheckNoPath(ramp, "D: every ramp plan engaged and found no path to the walled-off party");
		CheckBatches(recovery,
					 "D: the recovered worker's batches planned each of its monsters exactly once");
		CheckReached(recovery, "D: every recovery plan engaged, with a real path to the party");
		std::printf("\n");
	}

	// Phase E — global governor under load: re-run the asymmetric load, then clamp
	// every cadence with SetGlobalThrottle and show the think-rates scale down
	// (and recover). This is the frame-loop's "ease all background work" lever.
	{
		std::printf("--- E governor throttle under load (map 64x64, reachable) ---\n");
		int c[4] = {120, 8, 40, 4};
		auto snap = BuildSnapshot(64, 64, c, true);
		const float scales[4] = {1.0f, 0.5f, 0.25f, 1.0f};
		double b3Hz[4] = {}; // bucket 3 is the least loaded, so its rate tracks the
							 // governor rather than the work — the cleanest signal
		PathAudit pa;
		for (int i = 0; i < 4; ++i) {
			// Count from BEFORE the throttle call. SetGlobalThrottle wakes every
			// worker, which ticks at once and then sleeps the new interval; counted
			// from after the call, that wake tick lands in the window or not by a
			// race with DrivePhase's own baseline. Bucket 3 makes one or two ticks a
			// window, so the race alone could read 1.0x and 0.25x both as 0.40 Hz
			// - and did, once its ticks got cheap enough (real paths) to finish
			// before the baseline was read.
			long long before[4];
			for (int b = 0; b < 4; ++b)
				before[b] = static_cast<long long>(mgr.Inspect(ids[b]).iterations);
			mgr.SetGlobalThrottle(scales[i]);
			PhaseStats st;
			DrivePhase(mgr, dir, ids, snap, 2.5, st, pa);
			std::printf("  throttle %.2fx -> effHz", scales[i]);
			for (int b = 0; b < 4; ++b) {
				const auto info = mgr.Inspect(ids[b]);
				const long long ticks = static_cast<long long>(info.iterations) - before[b];
				const double hz = ticks / 2.5;
				if (b == 3) b3Hz[i] = hz;
				std::printf("  b%d=%.2f", b, hz);
			}
			std::printf("\n");
		}
		mgr.SetGlobalThrottle(1.0f);
		ReportAudit(pa);
		// The governor has to actually govern, and has to let go again. Asserted
		// loosely (0.7x / 0.6x rather than exact ratios) because these are real
		// threads on a shared machine, and a flaky check gets ignored, which is
		// worse than no check.
		Check(b3Hz[2] < b3Hz[0] * 0.7, "E: clamping to 0.25x measurably slowed the cadence");
		Check(b3Hz[3] > b3Hz[2] * 1.5 || b3Hz[3] > b3Hz[0] * 0.6,
			  "E: releasing the governor restored it");
		CheckBatches(pa, "E: every bucket's batches planned each of its monsters exactly once");
		CheckReached(pa, "E: every plan engaged, with a real path to the party");
		std::printf("\n");
	}

	// Phase F — cooperative kill + supervised restart. Under LIGHT load (workers
	// mostly asleep) a Kill wakes the cadence sleep and the worker exits cleanly
	// (no force-terminate). Restart reboots the slot; the restart counter climbs
	// and plans flow again — proving the lifecycle controls work mid-session.
	{
		std::printf("--- F kill + restart (light load) ---\n");
		int c[4] = {4, 4, 4, 4};
		auto snap = BuildSnapshot(24, 24, c, true);
		dir.Publish(snap);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		std::printf("  killing bucket2 and bucket3...\n");
		const u32 reBefore[2] = {mgr.Inspect(ids[2]).restarts, mgr.Inspect(ids[3]).restarts};
		mgr.Kill(ids[2]);
		mgr.Kill(ids[3]);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		for (int b = 2; b <= 3; ++b)
			std::printf("    bucket%d after kill: %s (re %u)\n", b,
						threads::StateName(mgr.Inspect(ids[b]).state),
						mgr.Inspect(ids[b]).restarts);
		// A kill under LIGHT load must be cooperative: the worker is asleep in its
		// cadence wait, the kill wakes it, and it exits on its own. Reaching
		// Quarantined here would mean the 250 ms grace expired on an idle thread.
		Check(mgr.Inspect(ids[2]).state != threads::State::Quarantined &&
				  mgr.Inspect(ids[3]).state != threads::State::Quarantined,
			  "F: an idle worker stopped cooperatively rather than being terminated");
		std::printf("  restarting them...\n");
		// The audit's baseline is read BEFORE the restarts. A rebooted worker
		// ticks at once, and for bucket 3 (a 2 s cadence) that tick is the only
		// one inside the 1.5 s window: read after Restart, the baseline could
		// already include it, by a race, and the bucket would owe a batch it
		// had published. Nothing else moves meanwhile - the snapshot has been
		// out since before the kill.
		PathAudit pa;
		for (int b = 0; b < 4; ++b) pa.Baseline(b, dir.TakePlans(b).seq);
		mgr.Restart(ids[2]);
		mgr.Restart(ids[3]);
		// Let the rebooted workers tick a few times, auditing what every bucket
		// thinks meanwhile.
		const auto t0 = Clock::now();
		while (SinceMs(t0) < 1500.0) {
			dir.Publish(snap);
			for (int b = 0; b < 4; ++b) AuditBatch(*snap, b, dir.TakePlans(b), pa);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		ReportAudit(pa);
		bool bothAlive = true, bothCounted = true;
		for (int b = 2; b <= 3; ++b) {
			const auto info = mgr.Inspect(ids[b]);
			std::printf("    bucket%d after restart: %s  it=%llu  re=%u\n", b,
						threads::StateName(info.state),
						static_cast<unsigned long long>(info.iterations), info.restarts);
			if (info.state == threads::State::Dead ||
				info.state == threads::State::Quarantined)
				bothAlive = false;
			if (info.iterations == 0 || info.restarts <= reBefore[b - 2]) bothCounted = false;
		}
		Check(bothAlive, "F: both rebooted workers came back alive");
		Check(bothCounted, "F: both ticked again and their restart counters climbed");
		CheckBatches(pa, "F: every bucket's batches planned each of its monsters exactly once");
		CheckReached(pa, "F: every plan engaged, with a real path to the party");
		std::printf("\n");
	}

	// Phase G - LOCKSTEP BEGINS WHILE A WORKER IS MID-TICK (code-review C63).
	// SetLockstep(true) used to Pause the workers and return at once, and Pause
	// only sets a flag: a worker in the middle of a tick finished it and published
	// AFTER the first inline compute - a plan from the wall-clock side, in a run
	// that promises its seed's decisions. So: load bucket 0 until a tick lasts tens
	// of milliseconds, catch it just after a tick begins, enter lockstep, and
	// demand that the call returned only with every worker holding at its pause
	// point (the in-flight tick already out), that nothing a worker thought was
	// published after it, that the inline compute's batch then stands, that a
	// worker the supervisor or a `boot` reboots meanwhile comes back STILL paused,
	// and that leaving lockstep sets them all running again.
	{
		std::printf("--- G lockstep entered while bucket0 is mid-tick (96x96, full-BFS) ---\n");
		const int W = 96, H = 96;
		// Sized on THIS machine: a tick near 40 ms is long enough to be caught in
		// the middle of and far short of the supervisor's 500 ms reboot line. The
		// last tick's time (not the average, which still remembers the ramp).
		constexpr double kTargetMs = 40.0;
		int n = 10;
		double tickMs = 0.0;
		for (int i = 0; i < 4; ++i) {
			int c[4] = {n, 0, 0, 0};
			auto probe = BuildSnapshot(W, H, c, false);
			const u64 it0 = mgr.Inspect(ids[0]).iterations;
			const auto t0 = Clock::now();
			while (SinceMs(t0) < 3000.0 && mgr.Inspect(ids[0]).iterations < it0 + 3) {
				dir.Publish(probe);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			tickMs = mgr.Inspect(ids[0]).lastMs;
			std::printf("  sizing: %d monsters -> a %.1f ms tick\n", n, tickMs);
			if (tickMs > kTargetMs * 0.6 && tickMs < kTargetMs * 2.0) break;
			n = std::clamp(static_cast<int>(n * kTargetMs / std::max(tickMs, 0.5)), 4, 600);
		}
		int c[4] = {n, 0, 0, 0};
		auto snap = BuildSnapshot(W, H, c, false);
		dir.Publish(snap);
		// The inline compute sized for this map and these monsters, as level load
		// sizes it in the game (ReserveAIPools), so it has no growth to report.
		dir.ReserveInline(static_cast<size_t>(W) * H,
						  {static_cast<size_t>(n) + 2, 2, 2, 2});

		// Catch bucket 0 just after a tick BEGINS (Running - or Stalled, Inspect's
		// overlay on a long Running tick - with its heartbeat younger than half a
		// tick), so most of that tick is still to run when lockstep is asked for.
		bool caught = false;
		u64 itBefore = 0;
		{
			const auto t0 = Clock::now();
			while (SinceMs(t0) < 5000.0) {
				const auto info = mgr.Inspect(ids[0]);
				const bool running = info.state == threads::State::Running ||
									 info.state == threads::State::Stalled;
				if (running && info.heartbeatAgeMs < tickMs * 0.5) {
					itBefore = info.iterations;
					caught = true;
					break;
				}
				std::this_thread::yield();
			}
		}
		const auto tLock = Clock::now();
		dir.SetLockstep(true);
		const double lockMs = SinceMs(tLock);
		// What stood the moment SetLockstep returned.
		uint64_t seqAtReturn[4];
		bool allPaused = true;
		for (int b = 0; b < 4; ++b) {
			seqAtReturn[b] = dir.PlanSeq(b);
			if (mgr.Inspect(ids[b]).state != threads::State::Paused) allPaused = false;
		}
		const u64 itAtReturn = mgr.Inspect(ids[0]).iterations;
		// Longer than the in-flight tick: a worker that was not quiet publishes
		// within this.
		std::this_thread::sleep_for(std::chrono::milliseconds(700));
		uint64_t seqLater[4];
		bool latePublish = false;
		for (int b = 0; b < 4; ++b) {
			seqLater[b] = dir.PlanSeq(b);
			if (seqLater[b] != seqAtReturn[b]) latePublish = true;
		}
		const u64 itLater = mgr.Inspect(ids[0]).iterations;
		std::printf("  caught=%d at tick %llu; SetLockstep took %.1f ms; bucket0 ticks %llu -> "
					"%llu at return -> %llu later; seq b0 %llu -> %llu\n",
					caught ? 1 : 0, static_cast<unsigned long long>(itBefore), lockMs,
					static_cast<unsigned long long>(itBefore),
					static_cast<unsigned long long>(itAtReturn),
					static_cast<unsigned long long>(itLater),
					static_cast<unsigned long long>(seqAtReturn[0]),
					static_cast<unsigned long long>(seqLater[0]));
		// The setup, held to account: a lockstep that began between ticks proves
		// nothing about one that begins inside one.
		Check(caught && itLater == itBefore + 1,
			  "G: lockstep began while bucket0 was mid-tick, and that tick finished");
		Check(allPaused && itAtReturn == itBefore + 1,
			  "G: entering lockstep returned only once every worker held at its pause point");
		Check(!latePublish, "G: no worker published anything after lockstep began");

		// The inline compute's batch for this snapshot is the one that stands: each
		// bucket publishes exactly one more, and bucket 0's plans each of its
		// monsters once - a fresh batch, not one the audit may skip as stale.
		PathAudit pa;
		for (int b = 0; b < 4; ++b) {
			pa.Baseline(b, seqLater[b]);
			pa.mayBeStale[b] = false;
		}
		bool onePerCompute = true;
		for (int b = 0; b < 4; ++b) {
			dir.ComputeInline(b);
			if (dir.PlanSeq(b) != seqLater[b] + 1) onePerCompute = false;
			AuditBatch(*snap, b, dir.TakePlans(b), pa);
		}
		ReportAudit(pa, "inline paths");
		Check(onePerCompute, "G: each inline compute published exactly one batch");
		CheckBatches(pa, "G: the inline batches planned each of bucket0's monsters exactly once");
		CheckNoPath(pa, "G: every inline plan engaged and found no path to the walled-off party");

		// A reboot while lockstep holds the workers: it must come back paused. A
		// rebooted worker used to have its pause cleared, so the supervisor
		// recovering a stalled bucket set it thinking beside the inline compute.
		const u32 reBefore = mgr.Inspect(ids[1]).restarts;
		const uint64_t seq1 = dir.PlanSeq(1);
		mgr.Restart(ids[1]);
		std::this_thread::sleep_for(std::chrono::milliseconds(600)); // past its 499 ms cadence
		const auto rebooted = mgr.Inspect(ids[1]);
		std::printf("  bucket1 after a reboot under lockstep: %s%s re=%u, seq %llu -> %llu\n",
					threads::StateName(rebooted.state), rebooted.paused ? " (paused)" : "",
					rebooted.restarts, static_cast<unsigned long long>(seq1),
					static_cast<unsigned long long>(dir.PlanSeq(1)));
		Check(rebooted.restarts > reBefore && rebooted.state == threads::State::Paused &&
				  rebooted.paused && dir.PlanSeq(1) == seq1,
			  "G: a worker rebooted during lockstep comes back paused and publishes nothing");

		// And out again: every worker runs.
		u64 itOut[4];
		for (int b = 0; b < 4; ++b) itOut[b] = mgr.Inspect(ids[b]).iterations;
		dir.SetLockstep(false);
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
		bool allRan = true;
		for (int b = 0; b < 4; ++b) {
			const auto info = mgr.Inspect(ids[b]);
			if (info.paused || info.iterations <= itOut[b]) allRan = false;
		}
		Check(allRan, "G: leaving lockstep set every worker running again");
		std::printf("\n");
	}

	// THE DURABLE CHECK, and the one that actually holds the line. The per-phase
	// scans above read State::Quarantined, which is a state Restart CLEARS on
	// its way to relaunching — so a worker force-terminated by the supervisor is
	// back to Running a moment later and the state scan sees nothing. The health
	// record does not forget: StopOrTerminate writes a Killed event against the
	// victim's timeline before the flag is cleared, so this is the assertion that
	// survives the very recovery it is meant to notice.
	std::printf("--- verdict ---\n");
	// Every plan the audits matched came from the worker whose bucket its
	// monster's IQ maps to (the IQs are derived from that same mapping above).
	std::printf("  %d plans audited all run, %d from the wrong bucket\n", g_auditedPlans,
				g_wrongBucket);
	Check(g_auditedPlans > 0 && g_wrongBucket == 0,
		  "every audited plan came from its monster's IQ bucket");
	const u64 killed = diag::ProcessTotals().Count(diag::Kind::Killed);
	Check(killed == 0, "nothing was force-terminated all run (from the health record)");
	if (killed > 0) {
		diag::EventView ev[8];
		diag::Slot slots[8];
		const int n = diag::ReadAllEvents(ev, 8, slots);
		diag::ThreadHealth th[diag::kMaxThreads];
		const int tn = diag::SnapshotThreads(th, diag::kMaxThreads);
		for (int i = 0; i < n; ++i) {
			if (ev[i].kind != diag::Kind::Killed) continue;
			const char* who = "?";
			for (int j = 0; j < tn; ++j)
				if (th[j].slot == slots[i]) who = th[j].name;
			std::printf("      killed: '%s' — %s\n", who, ev[i].message);
		}
	}

	std::printf("\n=== done — manager teardown joins/terminates all workers ===\n");
	// The harness must catch both faults WHERE they land: exactly the checks in
	// kSelfTestFails fail and every other check passes. "Anything failed" passed
	// a broken run as readily as a caught fault. Then the shared last line.
	std::printf("\n");
	const bool caught = selfTest && verdict::CaughtExactly(kSelfTestFails);
	return verdict::Finish("threadstress", selfTest, caught);
}
