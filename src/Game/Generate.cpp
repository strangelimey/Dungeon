// ============================================================================
// Game/Generate.cpp — see Generate.h.
// ============================================================================
#include "Game/Generate.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <queue>
#include <random>

namespace dungeon::game::generate {

namespace {

struct Room {
	int x = 0, z = 0, w = 0, h = 0;
	int cx() const { return x + w / 2; }
	int cz() const { return z + h / 2; }
};

int Roll(std::mt19937& rng, int lo, int hi) { // inclusive
	if (hi <= lo) return lo;
	return std::uniform_int_distribution<int>(lo, hi)(rng);
}

void Carve(Level& lv, int x, int z) {
	if (x < 1 || z < 1 || x >= lv.width - 1 || z >= lv.height - 1) return;
	lv.floor[static_cast<size_t>(z) * lv.width + x] = 1;
}

// --- the root room ------------------------------------------------------------
// With an ENTRY square the first room is built around it, so the entry is the
// start and the root of the tree. Without one it goes somewhere random.
Room RootRoom(const Params& p, int width, int height, std::mt19937& rng) {
	Room r;
	// Capped so the room fits inside the rim of even the smallest map.
	r.w = std::min(Roll(rng, 3, 7), width - 2);
	r.h = std::min(Roll(rng, 3, 7), height - 2);
	if (p.entryX >= 0 && p.entryZ >= 0) {
		// Anywhere that still covers the entry, inside the one-cell rock rim.
		r.x = std::clamp(p.entryX - Roll(rng, 0, r.w - 1), 1, width - 1 - r.w);
		r.z = std::clamp(p.entryZ - Roll(rng, 0, r.h - 1), 1, height - 1 - r.h);
	} else {
		r.x = Roll(rng, 1, width - 1 - r.w);
		r.z = Roll(rng, 1, height - 1 - r.h);
	}
	return r;
}

// --- growing the tree ---------------------------------------------------------
// Every room after the root hangs off one already placed, by a STRAIGHT
// corridor leaving a side of its parent. A candidate is kept only if neither
// its corridor nor its room comes within one square of anything already carved
// (the parent excepted, which the corridor has to touch). That is what keeps
// the layout a true TREE: two rooms never fuse, and a corridor never grazes a
// neighbour and opens a shortcut. So "three branches" is three branches, not
// three that happen to have run into each other.
//
// The old generator scattered rooms and then joined them with L-shaped
// corridors, which crossed rooms and one another freely. Its `branching` could
// only lean the shape one way or another, never promise a count.
struct Grower {
	Level& lv;
	std::mt19937& rng;
	std::vector<Room> rooms;
	std::vector<int> spineCorridor; // cell indices of the main path's corridors

	// The four ways out of a room, as unit steps.
	static constexpr int kDx[4] = {0, 1, 0, -1};
	static constexpr int kDz[4] = {-1, 0, 1, 0};

	int Idx(int x, int z) const { return z * lv.width + x; }
	bool Inside(int x, int z) const {
		return x >= 1 && z >= 1 && x < lv.width - 1 && z < lv.height - 1;
	}
	static bool InRoom(const Room& r, int x, int z) {
		return x >= r.x && x < r.x + r.w && z >= r.z && z < r.z + r.h;
	}

	void CarveRoom(const Room& r) {
		for (int z = r.z; z < r.z + r.h; ++z)
			for (int x = r.x; x < r.x + r.w; ++x) Carve(lv, x, z);
	}

	// Hang one room off `from`. `awayFrom` (a room index, or -1) biases the
	// direction: the main path grows AWAY from the start so it is walked
	// rather than wound on itself. Returns the new room's index, or -1 when
	// nothing fits after a bounded number of tries.
	int Attach(int from, int awayFrom, bool spine) {
		const Room parent = rooms[static_cast<size_t>(from)];
		constexpr int kTries = 40;
		for (int t = 0; t < kTries; ++t) {
			const int dir = Roll(rng, 0, 3);
			const int len = Roll(rng, 2, 5);
			Room c;
			c.w = Roll(rng, 3, 7);
			c.h = Roll(rng, 3, 7);
			// The corridor leaves the parent's side at a random point along it,
			// and the child is placed so the corridor enters its near side.
			std::vector<std::pair<int, int>> corridor;
			if (kDx[dir] != 0) {
				const int z = Roll(rng, parent.z, parent.z + parent.h - 1);
				const int x0 = kDx[dir] > 0 ? parent.x + parent.w : parent.x - 1;
				for (int i = 0; i < len; ++i) corridor.push_back({x0 + kDx[dir] * i, z});
				c.x = kDx[dir] > 0 ? x0 + len : x0 - len - c.w + 1;
				c.z = z - Roll(rng, 0, c.h - 1);
			} else {
				const int x = Roll(rng, parent.x, parent.x + parent.w - 1);
				const int z0 = kDz[dir] > 0 ? parent.z + parent.h : parent.z - 1;
				for (int i = 0; i < len; ++i) corridor.push_back({x, z0 + kDz[dir] * i});
				c.z = kDz[dir] > 0 ? z0 + len : z0 - len - c.h + 1;
				c.x = x - Roll(rng, 0, c.w - 1);
			}
			// Outward, for the main path: for the first half of the tries only
			// accept a child further from `awayFrom` than its parent is. The
			// second half takes any direction, so a path boxed in by the map
			// edge bends rather than stopping.
			if (awayFrom >= 0 && t < kTries / 2) {
				const Room& a = rooms[static_cast<size_t>(awayFrom)];
				auto dist = [&](int x, int z) {
					return std::abs(x - a.cx()) + std::abs(z - a.cz());
				};
				if (dist(c.cx(), c.cz()) <= dist(parent.cx(), parent.cz())) continue;
			}
			if (!Clear(parent, corridor, c)) continue;
			for (const auto& [x, z] : corridor) {
				Carve(lv, x, z);
				if (spine) spineCorridor.push_back(Idx(x, z));
			}
			CarveRoom(c);
			rooms.push_back(c);
			return static_cast<int>(rooms.size()) - 1;
		}
		return -1;
	}

	// Would carving `corridor` + `room` touch anything already carved, other
	// than `parent`? Every candidate square must be inside the rock rim, and
	// every square around it rock, part of the candidate, or the parent.
	bool Clear(const Room& parent, const std::vector<std::pair<int, int>>& corridor,
			   const Room& room) const {
		if (room.x < 1 || room.z < 1 || room.x + room.w > lv.width - 1 ||
			room.z + room.h > lv.height - 1)
			return false;
		auto candidate = [&](int x, int z) {
			if (InRoom(room, x, z)) return true;
			return std::find(corridor.begin(), corridor.end(), std::pair{x, z}) !=
				   corridor.end();
		};
		auto check = [&](int x, int z) {
			// Rock itself, first: the neighbourhood test below waves candidate
			// squares through, so an already-carved one would pass it.
			if (!Inside(x, z) || lv.At(x, z)) return false;
			for (int dz = -1; dz <= 1; ++dz)
				for (int dx = -1; dx <= 1; ++dx) {
					const int nx = x + dx, nz = z + dz;
					if (!lv.At(nx, nz) || candidate(nx, nz) || InRoom(parent, nx, nz))
						continue;
					return false;
				}
			return true;
		};
		for (const auto& [x, z] : corridor)
			if (!check(x, z)) return false;
		for (int z = room.z; z < room.z + room.h; ++z)
			for (int x = room.x; x < room.x + room.w; ++x)
				if (!check(x, z)) return false;
		return true;
	}
};

// --- corridors ---------------------------------------------------------------
// An L bend, axis order chosen by the caller so the two legs are not always the
// same way round (which reads as a grid of identical elbows). Only the kept-open
// squares (a regenerated level's other stairs) are joined this way now: they
// sit wherever the floor next door put them, so they cannot wait for a room to
// happen to grow toward them.
void CarveCorridor(Level& lv, int ax, int az, int bx, int bz, bool xFirst) {
	if (xFirst) {
		for (int x = std::min(ax, bx); x <= std::max(ax, bx); ++x) Carve(lv, x, az);
		for (int z = std::min(az, bz); z <= std::max(az, bz); ++z) Carve(lv, bx, z);
	} else {
		for (int z = std::min(az, bz); z <= std::max(az, bz); ++z) Carve(lv, ax, z);
		for (int x = std::min(ax, bx); x <= std::max(ax, bx); ++x) Carve(lv, x, bz);
	}
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
	// An entry square the map must contain (inside its rock rim) grows the map
	// to fit, rather than being dropped: it is the floor above's stair.
	Params q = p;
	const bool entry = p.entryX >= 0 && p.entryZ >= 0;
	if (entry) {
		q.entryX = std::clamp(p.entryX, 1, 126);
		q.entryZ = std::clamp(p.entryZ, 1, 126);
	}
	for (auto& [x, z] : q.keepOpen) {
		x = std::clamp(x, 1, 126);
		z = std::clamp(z, 1, 126);
	}
	int needW = p.width, needH = p.height;
	if (entry) needW = std::max(needW, q.entryX + 2), needH = std::max(needH, q.entryZ + 2);
	for (const auto& [x, z] : q.keepOpen)
		needW = std::max(needW, x + 2), needH = std::max(needH, z + 2);
	lv.width = std::clamp(needW, 8, 128);
	lv.height = std::clamp(needH, 8, 128);
	// Squares nothing may be placed on: the entry and the kept-open ones are
	// stairs, and a door or a key on a stair is nonsense.
	std::vector<std::pair<int, int>> reserved = q.keepOpen;
	if (entry) reserved.push_back({q.entryX, q.entryZ});
	auto isReserved = [&](int x, int z) {
		return std::find(reserved.begin(), reserved.end(), std::pair{x, z}) !=
			   reserved.end();
	};
	lv.floor.assign(static_cast<size_t>(lv.width) * lv.height, 0);
	std::mt19937 rng(p.seed);

	// --- shape ---------------------------------------------------------------
	// THE MAIN PATH first: the root, then `path` rooms in all, each hung off the
	// last and grown away from the start. Its last room is the exit, so the
	// path IS the way through. Then the side BRANCHES, each hung off a room of
	// the path (never the exit — a branch there is just a longer path) and
	// grown `branchMin..branchMax` rooms deep. Anything that does not fit is
	// not forced: the report says what was asked and what was built.
	Grower g{lv, rng, {}, {}};
	g.rooms.push_back(RootRoom(q, lv.width, lv.height, rng));
	g.CarveRoom(g.rooms[0]);
	std::vector<int> spine{0};
	const int wantPath = std::max(1, p.path);
	while (static_cast<int>(spine.size()) < wantPath) {
		const int r = g.Attach(spine.back(), 0, true);
		if (r < 0) break; // boxed in: the path ends here, and the report says so
		spine.push_back(r);
	}
	lv.report.pathWanted = wantPath;
	lv.report.pathGot = static_cast<int>(spine.size());

	const int bMin = std::max(1, std::min(p.branchMin, p.branchMax));
	const int bMax = std::max(bMin, std::max(p.branchMin, p.branchMax));
	lv.report.branchesWanted = std::max(0, p.branches);
	// Where branches may leave: every room of the path but the exit — or the
	// root alone, when the path is a single room.
	const int anchors = std::max(1, static_cast<int>(spine.size()) - 1);
	for (int b = 0; b < lv.report.branchesWanted; ++b) {
		// SPREAD along the path — branch b prefers the anchor at its fair share
		// of the way along, so three branches do not all sprout from the start
		// — then any other anchor, nearest first, if that one is boxed in.
		const int preferred = (b * anchors + anchors / 2) / std::max(1, p.branches);
		std::vector<int> order(static_cast<size_t>(anchors));
		for (int i = 0; i < anchors; ++i) order[static_cast<size_t>(i)] = i;
		std::stable_sort(order.begin(), order.end(), [&](int a, int c) {
			return std::abs(a - preferred) < std::abs(c - preferred);
		});
		const int depth = Roll(rng, bMin, bMax);
		int head = -1;
		for (const int a : order)
			if ((head = g.Attach(spine[static_cast<size_t>(a)], -1, false)) >= 0) break;
		if (head < 0) continue; // nowhere left to sprout: reported as missing
		int built = 1;
		while (built < depth) {
			const int r = g.Attach(head, -1, false);
			if (r < 0) break;
			head = r;
			++built;
		}
		lv.report.branchRooms.push_back(built);
	}
	lv.report.branchesGot = static_cast<int>(lv.report.branchRooms.size());
	const std::vector<Room>& rooms = g.rooms;

	// Each kept-open square joins the nearest room by a corridor of its own, so
	// it is floor AND reachable whatever shape the rooms took around it.
	for (const auto& [x, z] : q.keepOpen) {
		const Room* nearest = &rooms[0];
		int best = -1;
		for (const Room& r : rooms) {
			const int dx = r.cx() - x, dz = r.cz() - z;
			if (const int d = dx * dx + dz * dz; best < 0 || d < best) {
				best = d;
				nearest = &r;
			}
		}
		CarveCorridor(lv, x, z, nearest->cx(), nearest->cz(), (rng() & 1) != 0);
	}

	// --- ends ----------------------------------------------------------------
	// Start in the root — ON the entry square when there is one, which the root
	// was built around; exit at the END OF THE MAIN PATH, so the dungeon is
	// walked rather than stepped across and the branches are side trips.
	lv.startX = entry ? q.entryX : rooms[0].cx();
	lv.startZ = entry ? q.entryZ : rooms[0].cz();
	const Room& last = rooms[static_cast<size_t>(spine.back())];
	lv.exitX = last.cx();
	lv.exitZ = last.cz();

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
	// WANTED is what was asked, not what the key pool allows: a project with one
	// key asked for three locks should read 1/3, which says why.
	lv.report.locksWanted = std::max(0, p.locks);
	for (int lock = 0; lock < wantLocks; ++lock) {
		// Candidate doorways, in a shuffled order so the choice is not always the
		// same corner of the map for a given shape.
		std::vector<int> cands;
		for (int z = 1; z < lv.height - 1; ++z)
			for (int x = 1; x < lv.width - 1; ++x)
				if (IsDoorway(lv, x, z) && idx(x, z) != idx(lv.startX, lv.startZ) &&
					!isReserved(x, z) &&
					std::find(shut.begin(), shut.end(), idx(x, z)) == shut.end())
					cands.push_back(idx(x, z));
		std::shuffle(cands.begin(), cands.end(), rng);
		// The MAIN PATH's corridors first: a lock there gates PROGRESS, so the
		// exit is behind it and its key is somewhere the path has not reached —
		// usually down a branch, which is what gives a branch a reason to be
		// walked. Only when the path offers nothing does a branch get the lock.
		std::stable_partition(cands.begin(), cands.end(), [&](int c) {
			return std::find(g.spineCorridor.begin(), g.spineCorridor.end(), c) !=
				   g.spineCorridor.end();
		});

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
				bool taken = (kx == lv.startX && kz == lv.startZ) || isReserved(kx, kz);
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
			++lv.report.locksGot;
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
			if ((x == lv.startX && z == lv.startZ) || isReserved(x, z)) continue;
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
			++lv.report.monsters;
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
			++lv.report.loot;
		}
	}
	return lv;
}

} // namespace dungeon::game::generate
