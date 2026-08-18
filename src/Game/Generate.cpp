// ============================================================================
// Game/Generate.cpp — see Generate.h.
// ============================================================================
#include "Game/Generate.h"

#include <algorithm>
#include <format>
#include <queue>
#include <random>

namespace dungeon::game::generate {

namespace {

struct Room {
	int x = 0, z = 0, w = 0, h = 0;
	int cx() const { return x + w / 2; }
	int cz() const { return z + h / 2; }
	bool Overlaps(const Room& o, int pad) const {
		return x - pad < o.x + o.w && x + w + pad > o.x && z - pad < o.z + o.h &&
			   z + h + pad > o.z;
	}
};

int Roll(std::mt19937& rng, int lo, int hi) { // inclusive
	if (hi <= lo) return lo;
	return std::uniform_int_distribution<int>(lo, hi)(rng);
}

void Carve(Level& lv, int x, int z) {
	if (x < 1 || z < 1 || x >= lv.width - 1 || z >= lv.height - 1) return;
	lv.floor[static_cast<size_t>(z) * lv.width + x] = 1;
}

// --- rooms ------------------------------------------------------------------
// Placed by REJECTION: try a random rect, keep it if it clears the others by a
// one-cell margin, give up after a bounded number of tries. Bounded rather than
// exhaustive because the failure mode of pushing harder is rooms shrinking into
// cupboards, and "fewer, decent rooms" beats "the requested number, all tiny".
std::vector<Room> PlaceRooms(const Params& p, std::mt19937& rng) {
	std::vector<Room> rooms;
	const int tries = std::max(40, p.rooms * 12);
	for (int i = 0; i < tries && static_cast<int>(rooms.size()) < p.rooms; ++i) {
		Room r;
		r.w = Roll(rng, 3, 7);
		r.h = Roll(rng, 3, 7);
		r.x = Roll(rng, 1, std::max(1, p.width - r.w - 2));
		r.z = Roll(rng, 1, std::max(1, p.height - r.h - 2));
		bool clash = false;
		for (const Room& o : rooms)
			if (r.Overlaps(o, 1)) { clash = true; break; }
		if (!clash) rooms.push_back(r);
	}
	return rooms;
}

// --- corridors ---------------------------------------------------------------
// An L bend, axis order chosen by the caller so the two legs are not always the
// same way round (which reads as a grid of identical elbows).
void CarveCorridor(Level& lv, int ax, int az, int bx, int bz, bool xFirst) {
	if (xFirst) {
		for (int x = std::min(ax, bx); x <= std::max(ax, bx); ++x) Carve(lv, x, az);
		for (int z = std::min(az, bz); z <= std::max(az, bz); ++z) Carve(lv, bx, z);
	} else {
		for (int z = std::min(az, bz); z <= std::max(az, bz); ++z) Carve(lv, ax, z);
		for (int x = std::min(ax, bx); x <= std::max(ax, bx); ++x) Carve(lv, x, bz);
	}
}

int Dist2(const Room& a, const Room& b) {
	const int dx = a.cx() - b.cx(), dz = a.cz() - b.cz();
	return dx * dx + dz * dz;
}

// The connection TREE over rooms, grown nearest-first from room 0.
//
// `branching` reshapes it rather than adding edges: at 0 each new room attaches
// to the one added most recently, which chains them into a line; at 1 it
// attaches to the nearest room already in the tree, which fans out into side
// passages. A tree (not a graph) is deliberate — it is what makes "everything
// beyond this door" a well-defined set, which is what the lock placement needs.
std::vector<std::pair<int, int>> BuildTree(const std::vector<Room>& rooms,
										   const Params& p, std::mt19937& rng) {
	std::vector<std::pair<int, int>> edges;
	if (rooms.size() < 2) return edges;
	std::vector<int> in{0};
	std::vector<bool> used(rooms.size(), false);
	used[0] = true;
	for (size_t n = 1; n < rooms.size(); ++n) {
		int best = -1, bestFrom = in.back(), bestD = 0;
		for (size_t i = 0; i < rooms.size(); ++i) {
			if (used[i]) continue;
			const int d = Dist2(rooms[in.back()], rooms[i]);
			if (best < 0 || d < bestD) { best = static_cast<int>(i); bestD = d; }
		}
		if (best < 0) break;
		// Fan out: attach to the nearest tree member rather than the newest one.
		if (std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < p.branching) {
			int nd = 0;
			for (const int cand : in) {
				const int d = Dist2(rooms[cand], rooms[best]);
				if (bestFrom == in.back() || d < nd) { bestFrom = cand; nd = d; }
			}
		}
		edges.push_back({bestFrom, best});
		in.push_back(best);
		used[best] = true;
	}
	return edges;
}

// --- doorway hunting ---------------------------------------------------------
// A door needs a cell with solid walls flanking exactly ONE axis — the same rule
// DungeonMap::DoorwayFacing applies, restated here rather than shared because
// this module deliberately owns no map. A corridor cell is the natural fit.
bool IsDoorway(const Level& lv, int x, int z, Direction* facing = nullptr) {
	if (!lv.At(x, z)) return false;
	const bool ew = !lv.At(x - 1, z) && !lv.At(x + 1, z);
	const bool ns = !lv.At(x, z - 1) && !lv.At(x, z + 1);
	if (ew == ns) return false;
	// Walls east+west => the panel spans them and travel runs north-south. The
	// same mapping DungeonMap::DoorwayFacing uses, and it has to be authored
	// rather than left at the default: a door's facing IS its travel axis, so a
	// wrong one gives a panel that slides across the corridor instead of along
	// the wall. (The first generated levels had every door facing south.)
	if (facing) *facing = ew ? Direction::North : Direction::East;
	return true;
}

// Every floor cell reachable from (sx,sz) without crossing `blocked`.
std::vector<u8> FloodFrom(const Level& lv, int sx, int sz,
						  const std::vector<int>& blocked) {
	std::vector<u8> seen(lv.floor.size(), 0);
	if (!lv.At(sx, sz)) return seen;
	auto idx = [&](int x, int z) { return static_cast<size_t>(z) * lv.width + x; };
	std::queue<std::pair<int, int>> q;
	q.push({sx, sz});
	seen[idx(sx, sz)] = 1;
	while (!q.empty()) {
		const auto [x, z] = q.front();
		q.pop();
		constexpr int dx[4] = {0, 1, 0, -1};
		constexpr int dz[4] = {-1, 0, 1, 0};
		for (int i = 0; i < 4; ++i) {
			const int nx = x + dx[i], nz = z + dz[i];
			if (!lv.At(nx, nz) || seen[idx(nx, nz)]) continue;
			if (std::find(blocked.begin(), blocked.end(),
						  static_cast<int>(idx(nx, nz))) != blocked.end())
				continue;
			seen[idx(nx, nz)] = 1;
			q.push({nx, nz});
		}
	}
	return seen;
}

} // namespace

Level Run(const Params& p) {
	Level lv;
	lv.width = std::clamp(p.width, 8, 128);
	lv.height = std::clamp(p.height, 8, 128);
	lv.floor.assign(static_cast<size_t>(lv.width) * lv.height, 0);
	std::mt19937 rng(p.seed);

	// --- shape ---------------------------------------------------------------
	std::vector<Room> rooms = PlaceRooms(p, rng);
	if (rooms.empty()) { // a map too small for even one room still gets a cell
		lv.startX = lv.exitX = lv.width / 2;
		lv.startZ = lv.exitZ = lv.height / 2;
		Carve(lv, lv.startX, lv.startZ);
		return lv;
	}
	for (const Room& r : rooms)
		for (int z = r.z; z < r.z + r.h; ++z)
			for (int x = r.x; x < r.x + r.w; ++x) Carve(lv, x, z);

	const std::vector<std::pair<int, int>> edges = BuildTree(rooms, p, rng);
	for (const auto& [a, b] : edges)
		CarveCorridor(lv, rooms[a].cx(), rooms[a].cz(), rooms[b].cx(), rooms[b].cz(),
					  (rng() & 1) != 0);

	// --- ends ----------------------------------------------------------------
	// Start in room 0 (the tree's root); exit in whichever room is FURTHEST from
	// it, so the dungeon is walked rather than stepped across.
	lv.startX = rooms[0].cx();
	lv.startZ = rooms[0].cz();
	size_t far = 0;
	int farD = -1;
	for (size_t i = 1; i < rooms.size(); ++i)
		if (const int d = Dist2(rooms[0], rooms[i]); d > farD) { farD = d; far = i; }
	lv.exitX = rooms[far].cx();
	lv.exitZ = rooms[far].cz();

	// --- locks, BY CONSTRUCTION ----------------------------------------------
	// For each lock: find a doorway whose closure strands some floor but NOT the
	// start, put the door there, and put its key on a cell that is still
	// reachable with that door shut. Each successive lock treats the doors placed
	// so far as ALSO shut, so a key never ends up behind a lock authored later —
	// which is the whole nesting problem, solved by construction rather than by
	// generating and checking.
	auto idx = [&](int x, int z) { return static_cast<int>(z) * lv.width + x; };
	std::vector<int> shut; // cell indices of doors placed so far
	const int wantLocks = std::min<int>(p.locks, static_cast<int>(p.keyIds.size()));
	for (int lock = 0; lock < wantLocks; ++lock) {
		// Candidate doorways, in a shuffled order so the choice is not always the
		// same corner of the map for a given shape.
		std::vector<int> cands;
		for (int z = 1; z < lv.height - 1; ++z)
			for (int x = 1; x < lv.width - 1; ++x)
				if (IsDoorway(lv, x, z) && idx(x, z) != idx(lv.startX, lv.startZ) &&
					std::find(shut.begin(), shut.end(), idx(x, z)) == shut.end())
					cands.push_back(idx(x, z));
		std::shuffle(cands.begin(), cands.end(), rng);

		for (const int cell : cands) {
			std::vector<int> trial = shut;
			trial.push_back(cell);
			const std::vector<u8> before = FloodFrom(lv, lv.startX, lv.startZ, trial);
			if (!before[static_cast<size_t>(idx(lv.startX, lv.startZ))]) continue;
			// It has to actually shut something off, or the "lock" is scenery.
			int strandedCells = 0;
			for (size_t i = 0; i < lv.floor.size(); ++i)
				if (lv.floor[i] && !before[i]) ++strandedCells;
			if (strandedCells < 4) continue;

			// The key goes anywhere still reachable — and NOT on the door cell,
			// which would be reachable but absurd.
			std::vector<int> open;
			for (size_t i = 0; i < before.size(); ++i) {
				if (!before[i] || static_cast<int>(i) == cell) continue;
				// Not on top of anything already placed — the first generated
				// levels put both keys on the SAME square, because this loop
				// only ever consulted reachability.
				const int kx = static_cast<int>(i) % lv.width;
				const int kz = static_cast<int>(i) / lv.width;
				bool taken = (kx == lv.startX && kz == lv.startZ);
				for (const Entity& e : lv.entities)
					if (e.x == kx && e.z == kz) { taken = true; break; }
				if (!taken) open.push_back(static_cast<int>(i));
			}
			if (open.empty()) continue;
			const int keyCell = open[static_cast<size_t>(Roll(
				rng, 0, static_cast<int>(open.size()) - 1))];

			const std::string keyId = p.keyIds[static_cast<size_t>(lock)];
			Entity door;
			door.kind = EntityKind::Door;
			door.type = "wooden_door"; // the caller retypes if it wants another
			door.x = cell % lv.width;
			door.z = cell / lv.width;
			Direction axis = Direction::North;
			IsDoorway(lv, door.x, door.z, &axis);
			door.facing = axis;
			door.params.emplace_back("name", std::format("gen_door{}", lock + 1));
			door.params.emplace_back("key", keyId);
			lv.entities.push_back(std::move(door));

			Entity key;
			key.kind = EntityKind::Item;
			key.type = keyId;
			key.x = keyCell % lv.width;
			key.z = keyCell / lv.width;
			lv.entities.push_back(std::move(key));

			shut.push_back(cell);
			break;
		}
	}

	// --- population ----------------------------------------------------------
	// Density scales with the knobs against the FLOOR AREA, so a bigger dungeon
	// is not automatically a harder one — difficulty is per square, not per level.
	int floorCells = 0;
	for (const u8 c : lv.floor) floorCells += c ? 1 : 0;
	auto freeCell = [&](int tries) -> int {
		for (int i = 0; i < tries; ++i) {
			const int x = Roll(rng, 1, lv.width - 2), z = Roll(rng, 1, lv.height - 2);
			if (!lv.At(x, z)) continue;
			if (x == lv.startX && z == lv.startZ) continue;
			bool taken = false;
			for (const Entity& e : lv.entities)
				if (e.x == x && e.z == z) { taken = true; break; }
			if (!taken) return idx(x, z);
		}
		return -1;
	};

	if (!p.monsterIds.empty()) {
		const int n = static_cast<int>(static_cast<float>(floorCells) * 0.02f *
									   std::clamp(p.difficulty, 0.0f, 1.0f) * 2.0f);
		for (int i = 0; i < n; ++i) {
			const int c = freeCell(30);
			if (c < 0) break;
			Entity m;
			m.kind = EntityKind::Monster;
			m.type = p.monsterIds[static_cast<size_t>(
				Roll(rng, 0, static_cast<int>(p.monsterIds.size()) - 1))];
			m.x = c % lv.width;
			m.z = c / lv.width;
			lv.entities.push_back(std::move(m));
		}
	}
	if (!p.lootIds.empty()) {
		const int n = static_cast<int>(static_cast<float>(floorCells) * 0.015f *
									   std::clamp(p.reward, 0.0f, 1.0f) * 2.0f);
		for (int i = 0; i < n; ++i) {
			const int c = freeCell(30);
			if (c < 0) break;
			Entity it;
			it.kind = EntityKind::Item;
			it.type = p.lootIds[static_cast<size_t>(
				Roll(rng, 0, static_cast<int>(p.lootIds.size()) - 1))];
			it.x = c % lv.width;
			it.z = c / lv.width;
			lv.entities.push_back(std::move(it));
		}
	}
	return lv;
}

} // namespace dungeon::game::generate
