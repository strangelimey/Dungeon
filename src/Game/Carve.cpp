// ============================================================================
// Game/Carve.cpp - see Carve.h.
// ============================================================================
#include "Game/Carve.h"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <set>

namespace dungeon::game::carve {

namespace {
// Builds a Shape with each square once: the first role a square is given wins
// (a corridor's square that a later step re-crosses stays what it was).
struct Builder {
	Shape shape;
	std::set<std::pair<int, int>> seen;

	void Open(int x, int z, Role role) {
		if (seen.insert({x, z}).second) shape.open.push_back({x, z, role});
	}
	// A corridor `width` wide is widened ACROSS its way of travel (toward +z
	// while it runs along x, toward +x while it runs along z), so it ends at
	// its far square rather than overshooting it; a full block fills a BEND.
	void Across(int x, int z, int width, bool alongX, Role role) {
		for (int i = 0; i < width; ++i) Open(alongX ? x : x + i, alongX ? z + i : z, role);
	}
	void Block(int x, int z, int width, Role role) {
		for (int dz = 0; dz < width; ++dz)
			for (int dx = 0; dx < width; ++dx) Open(x + dx, z + dz, role);
	}
};

int Sign(int v) { return (v > 0) - (v < 0); }

// L-bent: along x then z when `xFirst`, else z then x.
void LBend(Builder& b, int ax, int az, int bx, int bz, bool xFirst, int width, Role role) {
	int x = ax, z = az;
	const auto walkX = [&] { while (x != bx) { x += Sign(bx - x); b.Across(x, z, width, true, role); } };
	const auto walkZ = [&] { while (z != bz) { z += Sign(bz - z); b.Across(x, z, width, false, role); } };
	// The first leg's own widening, from the start square.
	b.Across(x, z, width, xFirst ? bx != ax : bz == az, role);
	if (xFirst) {
		walkX();
		if (z != bz) b.Block(x, z, width, role); // the bend
		walkZ();
	} else {
		walkZ();
		if (x != bx) b.Block(x, z, width, role);
		walkX();
	}
}
} // namespace

bool Shape::Opens(int x, int z) const {
	return std::any_of(open.begin(), open.end(),
					   [&](const Square& s) { return s.x == x && s.z == z; });
}

Shape Corridor(int ax, int az, int bx, int bz, int width, float winding, u32 seed) {
	Builder b;
	width = std::clamp(width, 1, 3);
	std::mt19937 rng(seed);
	if (winding < 0.5f) {
		LBend(b, ax, az, bx, bz, (rng() & 1) != 0, width, Role::Corridor);
		return std::move(b.shape);
	}
	// THE MEANDER: each step goes toward the far end along an axis it is not yet
	// level on, or - with a chance that rises with winding - sideways across it.
	// It never steps back, so every step that is not sideways closes the gap, and
	// the sideways chance falls as the steps mount up: it always arrives.
	std::uniform_real_distribution<float> roll(0.0f, 1.0f);
	const float sideways = std::clamp((winding - 0.4f) * 0.8f, 0.0f, 0.45f);
	int x = ax, z = az;
	b.Block(x, z, width, Role::Corridor);
	const int budget = 4 * (std::abs(bx - ax) + std::abs(bz - az)) + 8;
	bool wasX = true, first = true;
	for (int step = 0; (x != bx || z != bz) && step < budget; ++step) {
		const int gx = bx - x, gz = bz - z;
		// Toward: along whichever axis has gap left (the longer one more often).
		const bool alongX = gz == 0 || (gx != 0 && roll(rng) * (std::abs(gx) + std::abs(gz)) <
														 static_cast<float>(std::abs(gx)));
		int sx = alongX ? Sign(gx) : 0, sz = alongX ? 0 : Sign(gz);
		// ...or sideways: across the chosen axis, either way, while the walk is
		// still young enough that it can afford to.
		if (roll(rng) < sideways * (1.0f - static_cast<float>(step) / static_cast<float>(budget))) {
			const int dir = (rng() & 1) ? 1 : -1;
			if (alongX) sx = 0, sz = dir;
			else sx = dir, sz = 0;
		}
		// A turn fills its corner square whole; a straight step widens across.
		const bool nowX = sx != 0;
		if (!first && nowX != wasX) b.Block(x, z, width, Role::Corridor);
		x += sx, z += sz;
		b.Across(x, z, width, nowX, Role::Corridor);
		wasX = nowX;
		first = false;
	}
	// A walk the budget cut short finishes as a bend (it never has, but an
	// endless corridor is not a failure this may risk).
	if (x != bx || z != bz) LBend(b, x, z, bx, bz, true, width, Role::Corridor);
	return std::move(b.shape);
}

Shape Room(int ax, int az, int bx, int bz) {
	Builder b;
	for (int z = std::min(az, bz); z <= std::max(az, bz); ++z)
		for (int x = std::min(ax, bx); x <= std::max(ax, bx); ++x) b.Open(x, z, Role::Room);
	return std::move(b.shape);
}

int Stamp::Width() const {
	size_t w = 0;
	for (const std::string& r : rows) w = std::max(w, r.size());
	return static_cast<int>(w);
}

Stamp ParseStamp(std::string_view text) {
	Stamp s;
	while (!text.empty()) {
		const size_t bar = text.find('|');
		std::string_view row = text.substr(0, bar);
		while (!row.empty() && row.front() == ' ') row.remove_prefix(1);
		while (!row.empty() && row.back() == ' ') row.remove_suffix(1);
		if (!row.empty()) s.rows.emplace_back(row);
		text = bar == std::string_view::npos ? std::string_view() : text.substr(bar + 1);
	}
	return s;
}

Stamp Turned(const Stamp& s) {
	// Clockwise: the new row r is the old column r read from the bottom up.
	Stamp t;
	const int w = s.Width(), h = s.Height();
	for (int r = 0; r < w; ++r) {
		std::string row;
		for (int c = h - 1; c >= 0; --c) {
			const std::string& src = s.rows[static_cast<size_t>(c)];
			row += r < static_cast<int>(src.size()) ? src[static_cast<size_t>(r)] : '-';
		}
		t.rows.push_back(std::move(row));
	}
	return t;
}

Shape StampAt(const Stamp& s, int cx, int cz, int turns) {
	Stamp g = s;
	for (int i = 0; i < ((turns % 4) + 4) % 4; ++i) g = Turned(g);
	Builder b;
	const int x0 = cx - g.Width() / 2, z0 = cz - g.Height() / 2;
	for (int r = 0; r < g.Height(); ++r) {
		const std::string& row = g.rows[static_cast<size_t>(r)];
		for (int c = 0; c < static_cast<int>(row.size()); ++c) {
			if (row[static_cast<size_t>(c)] == '.') b.Open(x0 + c, z0 + r, Role::Room);
			else if (row[static_cast<size_t>(c)] == '#') b.shape.solid.push_back({x0 + c, z0 + r});
		}
	}
	return std::move(b.shape);
}

Shape Region(const std::vector<u8>& floor, int w, int h, int x0, int z0,
			 const std::function<bool(int, int)>& isOpen, u32 seed) {
	Builder b;
	std::vector<std::pair<int, int>> generated;
	for (int z = 0; z < h; ++z)
		for (int x = 0; x < w; ++x)
			if (floor[static_cast<size_t>(z) * w + x]) {
				b.Open(x0 + x, z0 + z, Role::Room);
				generated.push_back({x0 + x, z0 + z});
			}
	if (generated.empty()) return std::move(b.shape);
	// THE JOIN. Walk the border inside the region; a border square with an open
	// square just outside it is a touch point, and a RUN of them (adjacent along
	// the border) is one opening - so a wide room beside the region gets one
	// corridor in, not one per square of wall it shares.
	std::vector<std::pair<int, int>> border;
	for (int x = 0; x < w; ++x) border.push_back({x, 0});
	for (int z = 1; z < h; ++z) border.push_back({w - 1, z});
	for (int x = w - 2; x >= 0; --x) border.push_back({x, h - 1});
	for (int z = h - 2; z >= 1; --z) border.push_back({0, z});
	const auto touches = [&](int lx, int lz) {
		const int x = x0 + lx, z = z0 + lz;
		return (lz == 0 && isOpen(x, z - 1)) || (lz == h - 1 && isOpen(x, z + 1)) ||
			   (lx == 0 && isOpen(x - 1, z)) || (lx == w - 1 && isOpen(x + 1, z));
	};
	std::mt19937 rng(seed);
	std::vector<std::pair<int, int>> run;
	const auto join = [&] {
		if (run.empty()) return;
		const auto [lx, lz] = run[run.size() / 2]; // the middle of the opening
		const int sx = x0 + lx, sz = z0 + lz;
		std::pair<int, int> best = generated.front();
		int bestD = -1;
		for (const auto& [gx, gz] : generated)
			if (const int d = std::abs(gx - sx) + std::abs(gz - sz); bestD < 0 || d < bestD)
				bestD = d, best = {gx, gz};
		LBend(b, sx, sz, best.first, best.second, (rng() & 1) != 0, 1, Role::Corridor);
		run.clear();
	};
	for (const auto& [lx, lz] : border) {
		if (touches(lx, lz)) run.push_back({lx, lz});
		else join();
	}
	join();
	return std::move(b.shape);
}

std::vector<Square> Rim(const Shape& s) {
	std::set<std::pair<int, int>> open;
	for (const Square& q : s.open) open.insert({q.x, q.z});
	std::vector<Square> out;
	std::set<std::pair<int, int>> seen;
	for (const Square& q : s.open)
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx) {
				const std::pair<int, int> n{q.x + dx, q.z + dz};
				if (open.count(n) || !seen.insert(n).second) continue;
				out.push_back({n.first, n.second, q.role});
			}
	return out;
}

} // namespace dungeon::game::carve
