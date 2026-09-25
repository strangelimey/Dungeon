// ============================================================================
// Game/Generate.cpp — see Generate.h.
// ============================================================================
#include "Game/Generate.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <queue>
#include <random>
#include <tuple>

namespace dungeon::game::generate {

namespace {

// A room: its bounding box, and a SHAPE within it (docs/level-building.md P3).
// Every shape keeps its floor at least two squares thick, so a room square is
// always part of some 2x2 block of floor — the property that tells a room from a
// corridor (which is one square wide), and that LevelBuildTest measures by.
enum class Shape { Rect, L, Cross, Pillars };

struct Room {
	int x = 0, z = 0, w = 0, h = 0;
	Shape shape = Shape::Rect;
	int cut = 0; // L: which corner is missing (0..3); Cross: the arm inset
	int cutW = 0, cutH = 0;

	int cx() const { return x + w / 2; }
	int cz() const { return z + h / 2; }

	// Is (px,pz) floor of this room?
	bool Has(int px, int pz) const {
		const int lx = px - x, lz = pz - z;
		if (lx < 0 || lz < 0 || lx >= w || lz >= h) return false;
		switch (shape) {
		case Shape::Rect: return true;
		case Shape::L: {
			// The missing corner: `cut` picks which, cutW x cutH of it.
			const bool inX = (cut & 1) ? lx >= w - cutW : lx < cutW;
			const bool inZ = (cut & 2) ? lz >= h - cutH : lz < cutH;
			return !(inX && inZ);
		}
		case Shape::Cross: {
			// All four corners missing, cutW x cutH each: a plus sign.
			const bool edgeX = lx < cutW || lx >= w - cutW;
			const bool edgeZ = lz < cutH || lz >= h - cutH;
			return !(edgeX && edgeZ);
		}
		case Shape::Pillars:
			// A pillar every third square, TWO squares in from each wall — so
			// the floor between two pillars, and between a pillar and a wall, is
			// two squares wide and still a room. (One square in left a one-wide
			// strip along the wall: a corridor running round inside the room,
			// which the measurement caught before anyone saw it.)
			return !(lx >= 2 && lz >= 2 && lx <= w - 3 && lz <= h - 3 &&
					 (lx - 2) % 3 == 0 && (lz - 2) % 3 == 0);
		}
		return true;
	}

	// A floor square as near the middle as the shape allows — where the exit
	// goes, and what a kept-open square's corridor aims at. The centre of an L
	// or a cross can be rock; a pillar can stand on it.
	std::pair<int, int> Centre() const {
		std::pair<int, int> best{x, z};
		int bestD = -1;
		for (int pz = z; pz < z + h; ++pz)
			for (int px = x; px < x + w; ++px) {
				if (!Has(px, pz)) continue;
				const int d = std::abs(px - cx()) + std::abs(pz - cz());
				if (bestD < 0 || d < bestD) bestD = d, best = {px, pz};
			}
		return best;
	}
};

int Roll(std::mt19937& rng, int lo, int hi) { // inclusive
	if (hi <= lo) return lo;
	return std::uniform_int_distribution<int>(lo, hi)(rng);
}

bool Chance(std::mt19937& rng, float p) {
	return p > 0.0f && std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < p;
}

void Carve(Level& lv, int x, int z) {
	if (x < 1 || z < 1 || x >= lv.width - 1 || z >= lv.height - 1) return;
	lv.floor[static_cast<size_t>(z) * lv.width + x] = 1;
}

// --- the root room ------------------------------------------------------------
// With an ENTRY square the first room is built around it, so the entry is the
// start and the root of the tree. Without one it goes somewhere random. Always a
// plain rectangle: the entry has to land on floor, not on a pillar or a cut.
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

// Give a fresh room an irregular shape, with probability `irregular`. Each shape
// has a minimum size that keeps every part of it two squares thick.
void Reshape(Room& r, float irregular, std::mt19937& rng) {
	if (!Chance(rng, irregular)) return;
	switch (Roll(rng, 0, 2)) {
	case 0: // L: a corner removed, leaving both arms at least two thick
		if (r.w < 4 || r.h < 4) return;
		r.shape = Shape::L;
		r.cut = Roll(rng, 0, 3);
		r.cutW = Roll(rng, 1, r.w - 2);
		r.cutH = Roll(rng, 1, r.h - 2);
		return;
	case 1: // Cross: all four corners removed, arms at least two thick
		if (r.w < 5 || r.h < 5) return;
		r.shape = Shape::Cross;
		r.cutW = Roll(rng, 1, (r.w - 2) / 2);
		r.cutH = Roll(rng, 1, (r.h - 2) / 2);
		return;
	default: // A pillared hall: needs room for pillars with floor round them
		if (r.w < 5 || r.h < 5) return;
		r.shape = Shape::Pillars;
		return;
	}
}

// --- growing the tree ---------------------------------------------------------
// Every room after the root hangs off one already placed, by a corridor leaving
// a side of its parent. A candidate is kept only if neither its corridor nor its
// room comes within one square of anything already carved (the parent excepted,
// which the corridor has to touch). That is what keeps the layout a true TREE:
// two rooms never fuse, and a corridor never grazes a neighbour and opens a
// shortcut. So "three branches" is three branches, not three that happen to have
// run into each other — and a LOOP is only ever one that was asked for.
//
// A corridor is straight, or with probability `winding` it JOGS: a staircase of
// forward runs and sideways steps, always turning the same way. Always the same
// way matters: a staircase that never doubles back can never put two of its own
// squares side by side, so it stays one square wide and never forms a 2x2 block
// — it cannot be mistaken for a room, by the player or by the measurement.
//
// The old generator scattered rooms and then joined them with L-shaped
// corridors, which crossed rooms and one another freely. Its `branching` could
// only lean the shape one way or another, never promise a count.
struct Grower {
	Level& lv;
	std::mt19937& rng;
	const Params& p;
	std::vector<Room> rooms;
	std::vector<int> spineCorridor; // cell indices of the main path's corridors
	std::vector<int> treeCorridor;  // every tree corridor's cells: where locks may go
	std::vector<int> stubCells;     // dead ends: no lock, no key

	// The four ways out of a room, as unit steps.
	static constexpr int kDx[4] = {0, 1, 0, -1};
	static constexpr int kDz[4] = {-1, 0, 1, 0};

	int Idx(int x, int z) const { return z * lv.width + x; }
	bool Inside(int x, int z) const {
		return x >= 1 && z >= 1 && x < lv.width - 1 && z < lv.height - 1;
	}

	void CarveRoom(const Room& r) {
		for (int z = r.z; z < r.z + r.h; ++z)
			for (int x = r.x; x < r.x + r.w; ++x)
				if (r.Has(x, z)) Carve(lv, x, z);
	}

	// A corridor leaving `parent` in direction `dir`, `len` squares of forward
	// travel in all: straight, or jogging sideways when the winding roll says
	// so. Empty when the side square chosen is not floor of the parent (an L's
	// cut or a cross's corner). `jogged` reports which it came out as.
	std::vector<std::pair<int, int>> Corridor(const Room& parent, int dir, int len,
											  bool& jogged) {
		std::vector<std::pair<int, int>> out;
		const int dx = kDx[dir], dz = kDz[dir];
		// Leave from a random point along the side, from floor of the parent.
		int x, z;
		if (dx != 0) {
			z = Roll(rng, parent.z, parent.z + parent.h - 1);
			x = dx > 0 ? parent.x + parent.w : parent.x - 1;
			if (!parent.Has(x - dx, z)) return out;
		} else {
			x = Roll(rng, parent.x, parent.x + parent.w - 1);
			z = dz > 0 ? parent.z + parent.h : parent.z - 1;
			if (!parent.Has(x, z - dz)) return out;
		}
		jogged = Chance(rng, p.winding);
		// Jogs: 1, plus up to two more as winding rises. Forward runs between
		// them are at least one square, so the corridor still arrives head-on —
		// and the FIRST is at least two when it jogs: a sideways step one square
		// out from the room would run along its wall and widen it into a notch.
		const int jogs = jogged ? 1 + Roll(rng, 0, static_cast<int>(p.winding * 2.0f + 0.5f)) : 0;
		const int firstRun = jogs > 0 ? 2 : 1;
		len = std::max(len, jogs + firstRun);
		const int side = Roll(rng, 0, 1) ? 1 : 3; // turn right or left, always the same
		const int sx = kDx[(dir + side) % 4], sz = kDz[(dir + side) % 4];
		// Split the forward length into jogs+1 runs of at least one square.
		std::vector<int> runs(static_cast<size_t>(jogs + 1), 1);
		runs[0] = firstRun;
		for (int extra = len - (jogs + firstRun); extra > 0; --extra)
			++runs[static_cast<size_t>(Roll(rng, 0, jogs))];
		for (int j = 0; j <= jogs; ++j) {
			for (int i = 0; i < runs[static_cast<size_t>(j)]; ++i) {
				out.push_back({x, z});
				x += dx, z += dz;
			}
			if (j == jogs) break;
			// The sideways step. The square it leaves from was the run's last,
			// so step off from there: back up one, then go sideways.
			x -= dx, z -= dz;
			const int steps = Roll(rng, 1, 1 + static_cast<int>(p.winding * 3.0f + 0.5f));
			for (int i = 0; i < steps; ++i) {
				x += sx, z += sz;
				out.push_back({x, z});
			}
			x += dx, z += dz;
		}
		return out;
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
			bool jogged = false;
			const std::vector<std::pair<int, int>> corridor =
				Corridor(parent, dir, Roll(rng, 2, 5), jogged);
			if (corridor.empty()) continue;
			// The child's near side meets the corridor's last square head-on.
			const auto [ex, ez] = corridor.back();
			Room c;
			c.w = Roll(rng, 3, 7);
			c.h = Roll(rng, 3, 7);
			if (kDx[dir] != 0) {
				c.x = kDx[dir] > 0 ? ex + 1 : ex - c.w;
				c.z = ez - Roll(rng, 0, c.h - 1);
			} else {
				c.z = kDz[dir] > 0 ? ez + 1 : ez - c.h;
				c.x = ex - Roll(rng, 0, c.w - 1);
			}
			Reshape(c, p.irregular, rng);
			// ...and meets FLOOR there, not an L's cut or a cross's corner.
			if (!c.Has(ex + kDx[dir], ez + kDz[dir])) continue;
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
			if (!Clear({&parent}, corridor, &c)) continue;
			for (const auto& [x, z] : corridor) {
				Carve(lv, x, z);
				treeCorridor.push_back(Idx(x, z));
				if (spine) spineCorridor.push_back(Idx(x, z));
			}
			CarveRoom(c);
			rooms.push_back(c);
			if (jogged) ++lv.report.windingGot;
			if (c.shape != Shape::Rect) ++lv.report.irregularGot;
			++lv.report.corridors;
			return static_cast<int>(rooms.size()) - 1;
		}
		return -1;
	}

	// A DEAD END: a corridor off a room that leads nowhere. Same clearance as
	// any other corridor, so it cannot become a shortcut by grazing its way into
	// something. False when no room offers one after a bounded number of tries.
	bool DeadEnd() {
		for (int t = 0; t < 60; ++t) {
			const Room& r = rooms[static_cast<size_t>(Roll(
				rng, 0, static_cast<int>(rooms.size()) - 1))];
			bool jogged = false;
			const std::vector<std::pair<int, int>> stub =
				Corridor(r, Roll(rng, 0, 3), Roll(rng, 2, 5), jogged);
			if (stub.empty() || !Clear({&r}, stub, nullptr)) continue;
			for (const auto& [x, z] : stub) {
				Carve(lv, x, z);
				stubCells.push_back(Idx(x, z));
			}
			return true;
		}
		return false;
	}

	// Would carving `corridor` (+ `room`, when there is one) touch anything
	// already carved, other than the rooms in `allowed`? Every candidate square
	// must be rock and inside the rim, and every square around it rock, part of
	// the candidate, or floor of an allowed room.
	bool Clear(std::initializer_list<const Room*> allowed,
			   const std::vector<std::pair<int, int>>& corridor, const Room* room) const {
		if (room && (room->x < 1 || room->z < 1 || room->x + room->w > lv.width - 1 ||
					 room->z + room->h > lv.height - 1))
			return false;
		auto candidate = [&](int x, int z) {
			if (room && room->Has(x, z)) return true;
			return std::find(corridor.begin(), corridor.end(), std::pair{x, z}) !=
				   corridor.end();
		};
		auto excused = [&](int x, int z) {
			for (const Room* a : allowed)
				if (a->Has(x, z)) return true;
			return false;
		};
		auto check = [&](int x, int z) {
			// Rock itself, first: the neighbourhood test below waves candidate
			// squares through, so an already-carved one would pass it.
			if (!Inside(x, z) || lv.At(x, z)) return false;
			for (int dz = -1; dz <= 1; ++dz)
				for (int dx = -1; dx <= 1; ++dx) {
					const int nx = x + dx, nz = z + dz;
					if (!lv.At(nx, nz) || candidate(nx, nz) || excused(nx, nz)) continue;
					return false;
				}
			return true;
		};
		for (const auto& [x, z] : corridor)
			if (!check(x, z)) return false;
		if (room)
			for (int z = room->z; z < room->z + room->h; ++z)
				for (int x = room->x; x < room->x + room->w; ++x)
					if (room->Has(x, z) && !check(x, z)) return false;
		return true;
	}

	// A LOOP: a second corridor between two rooms that are already joined some
	// other way, so there are two routes round. `region` labels every floor
	// square by the lock region it falls in (doors shut); a loop only joins two
	// rooms of the SAME region, so it can never be a way round a locked door —
	// the lock/key construction stays proven. Tries each pair of rooms that
	// are not already neighbours in the tree, nearest first, with a corridor
	// leaving each room's facing side (the Corridor machinery aimed at a room
	// instead of open rock). False when none fits.
	bool Loop(const std::vector<int>& region, std::vector<std::pair<int, int>>& joined) {
		struct Pair {
			int a, b, d;
		};
		std::vector<Pair> pairs;
		for (int a = 0; a < static_cast<int>(rooms.size()); ++a)
			for (int b = a + 1; b < static_cast<int>(rooms.size()); ++b) {
				const auto [ax, az] = rooms[static_cast<size_t>(a)].Centre();
				const auto [bx, bz] = rooms[static_cast<size_t>(b)].Centre();
				if (region[static_cast<size_t>(Idx(ax, az))] !=
					region[static_cast<size_t>(Idx(bx, bz))])
					continue;
				if (std::find(joined.begin(), joined.end(), std::pair{a, b}) != joined.end())
					continue;
				pairs.push_back({a, b, std::abs(ax - bx) + std::abs(az - bz)});
			}
		std::shuffle(pairs.begin(), pairs.end(), rng);
		std::stable_sort(pairs.begin(), pairs.end(),
						 [](const Pair& l, const Pair& r) { return l.d < r.d; });
		for (const Pair& pr : pairs) {
			const Room& A = rooms[static_cast<size_t>(pr.a)];
			const Room& B = rooms[static_cast<size_t>(pr.b)];
			// A straight corridor needs the rooms to face each other across a
			// gap: overlapping spans on one axis, a gap of two or more on the
			// other. Try each shared coordinate until one is floor at both ends.
			for (int axis = 0; axis < 2; ++axis) {
				const bool alongX = axis == 0; // corridor runs along x
				const int lo = alongX ? std::max(A.z, B.z) : std::max(A.x, B.x);
				const int hi = alongX ? std::min(A.z + A.h, B.z + B.h) - 1
									  : std::min(A.x + A.w, B.x + B.w) - 1;
				if (lo > hi) continue;
				const Room& L = alongX ? (A.x < B.x ? A : B) : (A.z < B.z ? A : B);
				const Room& R = &L == &A ? B : A;
				const int from = alongX ? L.x + L.w : L.z + L.h;
				const int to = alongX ? R.x - 1 : R.z - 1;
				if (to - from + 1 < 2) continue; // too close to need a corridor
				std::vector<int> coords;
				for (int c = lo; c <= hi; ++c) coords.push_back(c);
				std::shuffle(coords.begin(), coords.end(), rng);
				for (const int c : coords) {
					std::vector<std::pair<int, int>> corridor;
					for (int s = from; s <= to; ++s)
						corridor.push_back(alongX ? std::pair{s, c} : std::pair{c, s});
					const auto [fx, fz] = corridor.front();
					const auto [tx, tz] = corridor.back();
					const bool ends = alongX ? L.Has(fx - 1, fz) && R.Has(tx + 1, tz)
											 : L.Has(fx, fz - 1) && R.Has(tx, tz + 1);
					if (!ends || !Clear({&A, &B}, corridor, nullptr)) continue;
					for (const auto& [x, z] : corridor) Carve(lv, x, z);
					joined.push_back({pr.a, pr.b});
					++lv.report.corridors;
					return true;
				}
			}
		}
		return false;
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
	Grower g{lv, rng, q, {}, {}, {}, {}};
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

	// DEAD ENDS: stubs off any room, leading nowhere. After the tree, so they
	// fit around it rather than claiming room the path and branches needed.
	lv.report.deadEndsWanted = std::max(0, p.deadEnds);
	for (int i = 0; i < lv.report.deadEndsWanted; ++i)
		if (g.DeadEnd()) ++lv.report.deadEndsGot;

	// Each kept-open square joins the nearest room by a corridor of its own, so
	// it is floor AND reachable whatever shape the rooms took around it.
	for (const auto& [x, z] : q.keepOpen) {
		std::pair<int, int> nearest = rooms[0].Centre();
		int best = -1;
		for (const Room& r : rooms) {
			const auto [rx, rz] = r.Centre(); // floor, whatever the room's shape
			const int dx = rx - x, dz = rz - z;
			if (const int d = dx * dx + dz * dz; best < 0 || d < best) {
				best = d;
				nearest = {rx, rz};
			}
		}
		CarveCorridor(lv, x, z, nearest.first, nearest.second, (rng() & 1) != 0);
	}

	// --- ends ----------------------------------------------------------------
	// Start in the root — ON the entry square when there is one, which the root
	// was built around; exit at the END OF THE MAIN PATH, so the dungeon is
	// walked rather than stepped across and the branches are side trips.
	lv.startX = entry ? q.entryX : rooms[0].cx();
	lv.startZ = entry ? q.entryZ : rooms[0].cz();
	// (The room's middle FLOOR square: an L or a cross can have rock there.)
	std::tie(lv.exitX, lv.exitZ) = rooms[static_cast<size_t>(spine.back())].Centre();

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
		// same corner of the map for a given shape. Only squares of the TREE's
		// corridors: the gap between two pillars is doorway-shaped too, and a
		// door there shuts nothing off; a dead end's door guards nothing.
		std::vector<int> cands;
		for (const int cell : g.treeCorridor) {
			const int x = cell % lv.width, z = cell / lv.width;
			if (IsDoorway(lv, x, z) && cell != idx(lv.startX, lv.startZ) &&
				!isReserved(x, z) && std::find(shut.begin(), shut.end(), cell) == shut.end())
				cands.push_back(cell);
		}
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
				// Nor down a dead end: a key hidden in a stub is a key at the
				// end of a corridor built to lead nowhere, which reads as a
				// trick rather than a find.
				bool taken = (kx == lv.startX && kz == lv.startZ) || isReserved(kx, kz) ||
							 std::find(g.stubCells.begin(), g.stubCells.end(),
									   static_cast<int>(i)) != g.stubCells.end();
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

	// --- loops, AFTER the locks -----------------------------------------------
	// A loop joins two rooms of the SAME lock region, so the doors are placed
	// first and the regions read off them: flood from the start with every door
	// shut, then from each floor square not yet reached. A loop can then never
	// be a way round a locked door, and the construction proof the locks rest
	// on is untouched — the checker has nothing new to find.
	lv.report.loopsWanted = std::max(0, p.loops);
	if (lv.report.loopsWanted > 0) {
		std::vector<int> region(lv.floor.size(), -1);
		int label = 0;
		auto fill = [&](int sx, int sz) {
			const std::vector<u8> reach = FloodFrom(lv, sx, sz, shut);
			for (size_t i = 0; i < reach.size(); ++i)
				if (reach[i] && region[i] < 0) region[i] = label;
			++label;
		};
		fill(lv.startX, lv.startZ);
		for (size_t i = 0; i < lv.floor.size(); ++i)
			if (lv.floor[i] && region[i] < 0 &&
				std::find(shut.begin(), shut.end(), static_cast<int>(i)) == shut.end())
				fill(static_cast<int>(i) % lv.width, static_cast<int>(i) / lv.width);
		std::vector<std::pair<int, int>> joined;
		for (int i = 0; i < lv.report.loopsWanted; ++i)
			if (g.Loop(region, joined)) ++lv.report.loopsGot;
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
