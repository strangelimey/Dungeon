// ============================================================================
// Game/Blast.cpp — see Blast.h.
// ============================================================================
#include "Game/Blast.h"

#include <algorithm>

namespace dungeon::game::blast {

namespace {
// The 4-cardinal steps. ORTHOGONAL ONLY — the same rule LoS, movement and
// projectiles hold to, so a blast cannot round a corner diagonally either.
constexpr int kDX[4] = {0, 0, -1, 1};
constexpr int kDZ[4] = {-1, 1, 0, 0};
} // namespace

Result Spread(int cx, int cz, int force, float full, float falloff,
			  const PassableFn& passable) {
	Result out;
	if (force <= 0) return out;
	if (force > kMaxCells) {
		force = kMaxCells;
		out.clamped = true;
	}

	// A breadth-first flood, which is what makes `distance` the number of steps
	// THROUGH OPEN SQUARES rather than the straight-line distance: a blast that
	// has to come round a corner arrives weakened by the journey, not by how close
	// it looks on the map. The frontier is the result array itself — BFS order is
	// exactly the order cells were taken — so there is no queue and no allocation.
	const auto seen = [&out](int x, int z) {
		for (int i = 0; i < out.count; ++i)
			if (out.cells[i].x == x && out.cells[i].z == z) return true;
		return false;
	};

	if (passable && !passable(cx, cz)) {
		// The centre is solid — a bolt that burst against stone. Nothing stands
		// there, so it is not a filled square and spends no force; the blast still
		// starts from it, so its open neighbours are at distance 1.
		for (int d = 0; d < 4 && out.count < force; ++d) {
			const int nx = cx + kDX[d], nz = cz + kDZ[d];
			if (!passable(nx, nz) || seen(nx, nz)) continue;
			out.cells[out.count++] = {nx, nz, 1, 0.0f};
		}
	} else {
		out.cells[out.count++] = {cx, cz, 0, 0.0f};
	}

	// Expand. Every square taken spends one of the force; stone spends none, which
	// is the whole mechanic — the blast reaches further precisely because it was
	// hemmed in.
	for (int i = 0; i < out.count && out.count < force; ++i) {
		const Cell here = out.cells[i];
		for (int d = 0; d < 4 && out.count < force; ++d) {
			const int nx = here.x + kDX[d], nz = here.z + kDZ[d];
			if (passable && !passable(nx, nz)) continue;
			if (seen(nx, nz)) continue;
			out.cells[out.count++] = {nx, nz, here.distance + 1, 0.0f};
		}
	}

	// CONCENTRATION: force that could not be spent, because there was nowhere left
	// to expand into, lands on what the blast did reach. Sealed into one square, a
	// nine-square blast puts nine squares' worth into it.
	out.leftover = std::max(0, force - out.count);
	if (out.count > 0 && out.leftover > 0)
		out.concentration =
			1.0f + static_cast<float>(out.leftover) / static_cast<float>(out.count);

	// Falloff last, so concentration multiplies the figure a square would have
	// taken at its own distance rather than a flat share.
	for (int i = 0; i < out.count; ++i) {
		Cell& c = out.cells[i];
		c.damage = std::max(0.0f, full - falloff * static_cast<float>(c.distance)) *
				   out.concentration;
	}
	return out;
}

} // namespace dungeon::game::blast
