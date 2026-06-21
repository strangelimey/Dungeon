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
// Faithful to the live wiring: BucketForIq thresholds (140/110/80/50 land in
// buckets 0/1/2/3), aggroRange set high so every monster engages (=> every
// monster pays a BFS), and `blocked` carrying the party cell + every monster
// cell exactly like ai::Snapshot in DungeonWorld.
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
// ============================================================================
#include "Game/MonsterAI.h"
#include "Core/ThreadManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace dungeon;
using Clock = std::chrono::steady_clock;
static double SinceMs(Clock::time_point t) {
	return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ---------------------------------------------------------------------------
// Synthetic world. Build an immutable ai::Snapshot with `counts[b]` monsters in
// IQ bucket b, on a WxH open map. `reachable=false` walls the party's 8
// neighbours so the chase target can never be reached => every engaged monster
// pays a FULL-map BFS (the worst case). Monsters scatter on a lattice so they do
// not box each other in, and each engages (aggroRange huge).
// ---------------------------------------------------------------------------
static const float kBucketIq[ai::Scheduler::kBucketCount] = {140.f, 110.f, 80.f, 50.f};

static std::shared_ptr<const ai::Snapshot>
BuildSnapshot(int W, int H, const int counts[4], bool reachable) {
	auto snap = std::make_shared<ai::Snapshot>();
	snap->mapW = W;
	snap->mapH = H;
	snap->partyX = W / 2;
	snap->partyZ = H / 2;

	auto walk = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(W) * H, 1);
	auto at = [&](int x, int z) -> uint8_t& { return (*walk)[static_cast<size_t>(z) * W + x]; };
	for (int x = 0; x < W; ++x) { at(x, 0) = 0; at(x, H - 1) = 0; }      // border walls
	for (int z = 0; z < H; ++z) { at(0, z) = 0; at(W - 1, z) = 0; }
	if (!reachable) // isolate the party cell so the BFS explores the whole map and fails
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				if (dx || dz) at(snap->partyX + dx, snap->partyZ + dz) = 0;
	snap->walkable = walk;

	snap->blocked.insert(snap->partyZ * W + snap->partyX); // party cell is blocked
	int total = 0;
	for (int b = 0; b < 4; ++b) total += counts[b];
	if (total <= 0) return snap;

	// Scatter on a coarse lattice across the interior so monsters stay apart.
	const int step = std::max(2, static_cast<int>(std::sqrt(
								   static_cast<double>(W - 2) * (H - 2) / (total + 1))));
	snap->monsters.reserve(total);
	u32 id = 1;
	int b = 0, placedInB = 0;
	for (int z = 1; z < H - 1 && static_cast<int>(snap->monsters.size()) < total; z += step)
		for (int x = 1; x < W - 1 && static_cast<int>(snap->monsters.size()) < total; x += step) {
			if ((*walk)[static_cast<size_t>(z) * W + x] == 0) continue;
			if (x == snap->partyX && z == snap->partyZ) continue;
			while (b < 4 && placedInB >= counts[b]) { ++b; placedInB = 0; }
			if (b >= 4) break;
			ai::Agent a;
			a.id = id++;
			a.x = x;
			a.z = z;
			a.iq = kBucketIq[b];
			a.aggroRange = 1e9f; // force engage => every monster runs a BFS
			snap->monsters.push_back(a);
			snap->blocked.insert(z * W + x);
			++placedInB;
		}
	return snap;
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

// Publish `snap` every ~5 ms (a ~200 Hz "frame loop"), drain plans, and sample
// each bucket's live Inspect() to catch transient stalls and peak tick times.
static void DrivePhase(threads::Manager& mgr, ai::AsyncDirector& dir,
					   const threads::WorkerId ids[4],
					   std::shared_ptr<const ai::Snapshot> snap, double seconds,
					   PhaseStats& st) {
	for (int b = 0; b < 4; ++b) {
		const auto info = mgr.Inspect(ids[b]);
		st.iterStart[b] = static_cast<long long>(info.iterations);
		st.seqStart[b] = dir.TakePlans(b).seq;
		st.peakLast[b] = 0.0;
	}
	const auto t0 = Clock::now();
	while (SinceMs(t0) < seconds * 1000.0) {
		dir.Publish(snap);
		for (int b = 0; b < 4; ++b) {
			const auto info = mgr.Inspect(ids[b]);
			if (info.state == threads::State::Stalled) st.stallSamples[b]++;
			st.peakLast[b] = std::max(st.peakLast[b], info.lastMs);
			dir.TakePlans(b);
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

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("=== Thread-system stress harness =================================\n");
	std::printf("HW concurrency: %u threads\n\n", std::thread::hardware_concurrency());

	threads::Manager mgr;
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

	auto phase = [&](const char* title, int W, int H, const int counts[4], bool reach,
					 double secs) {
		std::printf("--- %s  (map %dx%d, %s) ---\n", title, W, H,
					reach ? "reachable" : "UNREACHABLE/full-BFS");
		auto snap = BuildSnapshot(W, H, counts, reach);
		PhaseStats st;
		DrivePhase(mgr, dir, ids, snap, secs, st);
		ReportPhase(mgr, dir, ids, counts, secs, st);
		std::printf("\n");
	};

	// Phase A — baseline: a handful of monsters per bucket, small reachable map.
	{ int c[4] = {2, 2, 2, 2}; phase("A baseline", 24, 24, c, true, 3.0); }

	// Phase B — ASYMMETRIC load: each bucket gets a very different monster count,
	// to prove the per-bucket isolation + coprime cadences (one hot bucket must
	// not starve the others). Reachable, medium map.
	{ int c[4] = {120, 8, 40, 4}; phase("B asymmetric per-bucket", 64, 64, c, true, 4.0); }

	// Phase C — heavy but bounded: a big map, full-BFS (unreachable), moderate
	// counts. Each engaged monster explores the whole reachable region.
	{ int c[4] = {30, 30, 30, 30}; phase("C heavy full-BFS", 96, 96, c, false, 4.0); }

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
		int n = 20;
		for (int stepi = 0; stepi < 16; ++stepi) {
			int c[4] = {n, 0, 0, 0};
			auto snap = BuildSnapshot(W, H, c, false);
			PhaseStats st;
			DrivePhase(mgr, dir, ids, snap, 2.0, st);
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
		// Recover on a trivial load so the final state isn't caught mid-reboot, and
		// confirm plans flow again after the overload (a wedged/quarantined worker
		// would produce none).
		uint64_t recoverPlans = 0;
		{
			int cc[4] = {2, 0, 0, 0};
			auto light = BuildSnapshot(24, 24, cc, true);
			const uint64_t seq0 = dir.TakePlans(0).seq;
			const auto t0 = Clock::now();
			while (SinceMs(t0) < 800.0) {
				dir.Publish(light);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			recoverPlans = dir.TakePlans(0).seq - seq0;
		}
		const auto info = mgr.Inspect(ids[0]);
		std::printf("  verdict: bucket0 restarts %u->%u, final state '%s', recovery plans/0.8s=%llu\n",
					reStart, info.restarts, threads::StateName(info.state),
					static_cast<unsigned long long>(recoverPlans));
		std::printf("           %s\n",
					quarantined ? "QUARANTINED — force-terminated (BAD: cooperative stop failed)"
					: (rebooted && recoverPlans > 0)
						? "rebooted cleanly + plans resumed — no force-terminate (cooperative stop works)"
					: rebooted ? "rebooted but no recovery plans (investigate)"
							   : "never reached the reboot zone (raise the ramp)");
		std::printf("\n");
	}

	// Phase E — global governor under load: re-run the asymmetric load, then clamp
	// every cadence with SetGlobalThrottle and show the think-rates scale down
	// (and recover). This is the frame-loop's "ease all background work" lever.
	{
		std::printf("--- E governor throttle under load (map 64x64, reachable) ---\n");
		int c[4] = {120, 8, 40, 4};
		auto snap = BuildSnapshot(64, 64, c, true);
		for (float scale : {1.0f, 0.5f, 0.25f, 1.0f}) {
			mgr.SetGlobalThrottle(scale);
			PhaseStats st;
			DrivePhase(mgr, dir, ids, snap, 2.5, st);
			std::printf("  throttle %.2fx -> effHz", scale);
			for (int b = 0; b < 4; ++b) {
				const auto info = mgr.Inspect(ids[b]);
				const long long ticks =
					static_cast<long long>(info.iterations) - st.iterStart[b];
				std::printf("  b%d=%.2f", b, ticks / 2.5);
			}
			std::printf("\n");
		}
		mgr.SetGlobalThrottle(1.0f);
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
		mgr.Kill(ids[2]);
		mgr.Kill(ids[3]);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		for (int b = 2; b <= 3; ++b)
			std::printf("    bucket%d after kill: %s (re %u)\n", b,
						threads::StateName(mgr.Inspect(ids[b]).state),
						mgr.Inspect(ids[b]).restarts);
		std::printf("  restarting them...\n");
		mgr.Restart(ids[2]);
		mgr.Restart(ids[3]);
		// Let the rebooted workers tick a few times.
		const auto t0 = Clock::now();
		while (SinceMs(t0) < 1500.0) {
			dir.Publish(snap);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		for (int b = 2; b <= 3; ++b) {
			const auto info = mgr.Inspect(ids[b]);
			std::printf("    bucket%d after restart: %s  it=%llu  re=%u\n", b,
						threads::StateName(info.state),
						static_cast<unsigned long long>(info.iterations), info.restarts);
		}
		std::printf("\n");
	}

	std::printf("=== done — manager teardown joins/terminates all workers ===\n");
	return 0;
}
