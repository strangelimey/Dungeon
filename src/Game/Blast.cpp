// ============================================================================
// Game/Blast.cpp — see Blast.h.
// ============================================================================
#include "Game/Blast.h"

#include <algorithm>

namespace dungeon::game::blast {

namespace {
// The 4 cardinal steps. ORTHOGONAL ONLY — the rule LoS, movement and projectiles
// all hold to, so a blast cannot round a corner diagonally either.
constexpr int kDX[4] = {0, 0, -1, 1};
constexpr int kDZ[4] = {-1, 1, 0, 0};
// The two directions perpendicular to each — where a blocked unit DEFLECTS to.
constexpr int kPerp[4][2] = {{2, 3}, {2, 3}, {0, 1}, {0, 1}};
// Straight back the way it came, for a unit with nowhere to deflect.
constexpr int kBack[4] = {1, 0, 3, 2};

// One square the blast occupies, and where its units are heading.
struct Occupied {
	int x = 0, z = 0;
	int distance = 0;
	int units = 0; // concentration: how much blast is in this square
	// The direction of travel that brought the blast here, or -1 at the detonation
	// square. A front does NOT go back the way it came — that is what makes an open
	// room a ring EXPANDING rather than a wave sloshing back and forth over the
	// same squares. Reversal is reflection's job, and only reflection's.
	int from = -1;
};
} // namespace

Result Propagate(int cx, int cz, const Rules& rules, const PassableFn& passable) {
	Result out;
	if (!rules.Any()) return out;

	int force = rules.force;
	if (force > kMaxCells) {
		force = kMaxCells;
		out.clamped = true;
	}

	// Every square the blast has ever touched, with its shortest open-path
	// distance. DISTANCE IS RECORDED ON FIRST ARRIVAL and never revised, which is
	// what keeps a reflected firewall dangerous: a square one step out that the
	// front returns to on tick 5 is still at distance 1, so it takes near-full
	// damage rather than the far-end scorch that tick-based falloff would give it.
	std::array<Occupied, kMaxCells> known{};
	int knownCount = 0;
	const auto find = [&](int x, int z) -> Occupied* {
		for (int i = 0; i < knownCount; ++i)
			if (known[i].x == x && known[i].z == z) return &known[i];
		return nullptr;
	};

	const auto record = [&](int x, int z, int tick, int distance, int arrivals) {
		if (out.count >= static_cast<int>(out.hits.size())) {
			out.clamped = true;
			return;
		}
		const float figure =
			std::max(0.0f, rules.damage - rules.falloff * static_cast<float>(distance));
		out.hits[out.count++] = {x, z, tick, distance, arrivals,
								 figure * static_cast<float>(arrivals)};
	};

	// --- detonation ---------------------------------------------------------
	// The centre may be solid (a bolt that burst against stone). Nothing stands in
	// a wall, so it takes no effect and spends no force — but the blast still
	// starts from it, so the squares beyond are at distance 1.
	const bool centreOpen = !passable || passable(cx, cz);
	std::array<Occupied, kMaxCells> frontier{};
	int frontCount = 0;
	if (centreOpen) {
		known[knownCount++] = {cx, cz, 0, 1};
		frontier[frontCount++] = {cx, cz, 0, 1};
		--force;
		record(cx, cz, 0, 0, 1);
	} else {
		// A phantom frontier at the centre: it carries units outward but is never
		// itself a square anything is standing in.
		frontier[frontCount++] = {cx, cz, 0, 1};
	}

	// --- expansion ----------------------------------------------------------
	for (int tick = 1; tick <= kMaxTicks && force > 0 && frontCount > 0; ++tick) {
		// Units arriving this tick, tallied per destination — several converging on
		// one square is that square's MULTIPLIER, which is the point of reflection.
		std::array<Occupied, kMaxCells> arriving{};
		int arriveCount = 0;
		// Several units landing on one square MERGE, and the tally is that square's
		// multiplier for the tick — Michael's rule: six converging is a x6 tick. The
		// first arrival's direction of travel is the one the merged front carries on
		// in; a deterministic pick, and any of them would do.
		const auto arrive = [&](int x, int z, int distance, int dir) {
			for (int i = 0; i < arriveCount; ++i)
				if (arriving[i].x == x && arriving[i].z == z) {
					++arriving[i].units;
					return;
				}
			if (arriveCount < kMaxCells)
				arriving[arriveCount++] = {x, z, distance, 1, dir};
			else
				out.clamped = true;
		};

		for (int f = 0; f < frontCount; ++f) {
			const Occupied src = frontier[f];
			// ONE unit per direction per SQUARE — emphatically not one per unit
			// already in it. Letting a square re-emit its whole concentration in
			// every direction creates blast from nothing: measured, a force-9 fire
			// in a dead-end corridor reached x360 and 5040 damage, because four
			// units bouncing became sixteen, then sixty-four. The multiplier is what
			// CONVERGENCE buys (six units landing on one square is x6, Michael's
			// rule); it is not something a square then spends again.
			for (int d = 0; d < 4; ++d) {
				// Never straight back where it came from: an expanding front, not a
				// sloshing one. Measured before this rule existed, a force-9 fire in
				// an OPEN room re-burned its own detonation square at x4, because
				// the whole ring pushed half its units back inward.
				if (src.from >= 0 && d == kBack[src.from]) continue;
				const int nx = src.x + kDX[d], nz = src.z + kDZ[d];
				if (!passable || passable(nx, nz)) {
					arrive(nx, nz, src.distance + 1, d);
					continue;
				}
				// BLOCKED. Deflect into whichever perpendiculars are open — BOTH
				// when both are, so a unit meeting a wall head-on in a junction
				// splits rather than picking a side (there is no honest handedness
				// to pick).
				bool deflected = false;
				for (const int p : kPerp[d]) {
					const int px = src.x + kDX[p], pz = src.z + kDZ[p];
					if (passable && !passable(px, pz)) continue;
					arrive(px, pz, src.distance + 1, p);
					deflected = true;
				}
				if (deflected) continue;
				// Nowhere sideways either: REFLECT straight back. This is the dead
				// end turning a blast into a firewall coming home — and the ONE
				// place a front is allowed to reverse, which is why the reversal
				// carries kBack[d] as its new direction of travel.
				const int back = kBack[d];
				const int bx = src.x + kDX[back], bz = src.z + kDZ[back];
				if (!passable || passable(bx, bz))
					arrive(bx, bz, src.distance + 1, back);
			}
		}
		if (arriveCount == 0) break; // entombed: nothing left to reach

		// Spend force on the arrivals, and settle each destination's distance.
		frontCount = 0;
		bool anySpent = false;
		for (int i = 0; i < arriveCount && force > 0; ++i) {
			Occupied& a = arriving[i];
			// One force per square entered, however many units land in it: force
			// buys REACH, and the multiplier is what converging buys instead.
			--force;
			anySpent = true;

			Occupied* prior = find(a.x, a.z);
			if (!prior) {
				if (knownCount >= kMaxCells) {
					out.clamped = true;
					break;
				}
				known[knownCount++] = {a.x, a.z, a.distance, a.units};
				prior = &known[knownCount - 1];
			} else {
				// Re-entered. Distance keeps its FIRST value (see above).
				prior->units = rules.persistence == Persistence::Persistent
								   ? prior->units + a.units // concentration builds
								   : a.units;               // the front merely passes
			}
			record(a.x, a.z, tick, prior->distance, a.units);
			if (frontCount < kMaxCells)
				frontier[frontCount++] = {a.x, a.z, prior->distance, prior->units,
										  a.from};
		}
		out.ticks = tick;
		if (!anySpent) break;

		// A PERSISTENT medium keeps every filled square biting, so the whole cloud
		// re-applies next tick, not just the newest arrivals — and a square that
		// units have re-entered bites harder, because its concentration went up.
		// (A transient front does not: the squares behind it have vacated.)
		if (rules.persistence == Persistence::Persistent && force > 0) {
			frontCount = 0;
			for (int i = 0; i < knownCount && frontCount < kMaxCells; ++i)
				frontier[frontCount++] = known[i];
		}
	}

	out.spent = std::min(rules.force, kMaxCells) - std::max(0, force);
	out.leftover = std::max(0, force);
	if (out.ticks >= kMaxTicks) out.clamped = true;
	return out;
}

} // namespace dungeon::game::blast
