// ============================================================================
// Game/DungeonWorld_PickProbe.cpp - `pickprobe`: is every click target where it
// is DRAWN? (code-review C258 / C359)
//
// The 3D view's clicks are tested against stand-ins for what is drawn: a floor
// item by its QUARTER, measured at its drawn height (PickItemIndex), a niche item
// by its pocket's ball, a door's hand-hold and a wall torch by balls of their
// own (Camera::Ray::HitsSphere). A stand-in can drift from the drawing without
// anyone seeing it - the floor pick was measured at the model's height in MODEL
// units, laid flat or not, so a torch's quarter was tested 7 cm above the torch
// and a click near its near edge fell in the quarter before it - so this shoots
// at each target through the screen, with the real click tests, from where it
// is drawn:
//   * a floor item: five points across a quarter-sized square centred on its
//     DRAWN box (the middle and just inside each edge), at the box's middle
//     height - every one a hit;
//   * a niche item: the middle of its drawn box;
//   * the door ahead's hand-hold and the wall torch ahead: the middle of their
//     model's box, placed as their draw places it (OpenerPos / Fire::world).
// An item's drawn box is its model's bounds (the tablet's, for a kind with no
// model) through its pose's matrix - the draw's own mesh and matrix. It is NOT
// what the pick reads (the kind's cached floorHeight, the slot's hardcoded
// quarter): a probe built from the pick's own inputs projects a point and
// intersects it at the same height, and hits for any camera. So a cached height
// that drifts from the lay, or a slot layout the draw and the pick read
// differently, both show as misses here.
// And each has a point that must MISS - the drawn middle mirrored through the
// square's centre (the quarter across) for a floor item, a metre above a round
// target - since a test that hit everything would pass the first half.
// Allocates freely: a dev readout, typed by hand.
// ============================================================================
#include "Game/DungeonWorld.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dungeon::game {

namespace {
// Where world point `p` shows on the screen, as 0..1 of the view: the inverse
// of Camera::ScreenRay with a 1 x 1 viewport. False behind the eye.
bool ScreenPoint(const gfx::Camera& camera, const Vec3& p, float& mx, float& my) {
	const Mat4 vp = camera.ViewProj();
	const float x = p.x * vp._11 + p.y * vp._21 + p.z * vp._31 + vp._41;
	const float y = p.x * vp._12 + p.y * vp._22 + p.z * vp._32 + vp._42;
	const float w = p.x * vp._14 + p.y * vp._24 + p.z * vp._34 + vp._44;
	if (w <= 1e-4f) return false;
	mx = (x / w + 1.0f) * 0.5f;
	my = (1.0f - y / w) * 0.5f;
	return true;
}

// The middle of a model's first mesh's box (what a prop's draw draws), in its
// own space. False for a model with no vertices.
bool MeshMiddle(const assets::ModelData* model, Vec3& mid) {
	if (!model || model->meshes.empty() || model->meshes[0].vertices.empty()) return false;
	Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	for (const assets::Vertex& v : model->meshes[0].vertices) {
		lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y),
			  std::min(lo.z, v.position.z)};
		hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y),
			  std::max(hi.z, v.position.z)};
	}
	mid = {0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)};
	return true;
}

// A point through a world matrix (row vectors: v' = v * M).
Vec3 Through(const Mat4& m, const Vec3& p) {
	return {p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41,
			p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42,
			p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43};
}

// The world box a model-space box covers through `world`: its eight corners
// through the matrix, boxed again.
void BoxThrough(const Vec3& lo, const Vec3& hi, const Mat4& world, Vec3& outLo, Vec3& outHi) {
	outLo = {1e9f, 1e9f, 1e9f};
	outHi = {-1e9f, -1e9f, -1e9f};
	for (int i = 0; i < 8; ++i) {
		const Vec3 p = Through(world, {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
									   (i & 4) ? hi.z : lo.z});
		outLo = {std::min(outLo.x, p.x), std::min(outLo.y, p.y), std::min(outLo.z, p.z)};
		outHi = {std::max(outHi.x, p.x), std::max(outHi.y, p.y), std::max(outHi.z, p.z)};
	}
}

constexpr float kAbove = 0.4f * kUnit; // "a metre above": a point that must miss
} // namespace

std::vector<std::string> DungeonWorld::ProbePicks() const {
	std::vector<std::string> out;
	int floors = 0, niches = 0, openers = 0, sconces = 0, wrong = 0;
	const int px = m_party.GridX(), pz = m_party.GridZ();
	// The item a click at world point `p` picks (-1 none, -2 behind the eye).
	const auto pickAt = [&](const Vec3& p) {
		float mx = 0.0f, my = 0.0f;
		return ScreenPoint(m_camera, p, mx, my) ? PickItemIndex(mx, my, 1.0f, 1.0f) : -2;
	};
	// The click ray through world point `p` (false behind the eye).
	const auto rayAt = [&](const Vec3& p, gfx::Camera::Ray& ray) {
		float mx = 0.0f, my = 0.0f;
		if (!ScreenPoint(m_camera, p, mx, my)) return false;
		ray = m_camera.ScreenRay(mx, my, 1.0f, 1.0f);
		return true;
	};

	// --- items in reach, on the floor or in an open niche -----------------------
	for (size_t i = 0; i < m_items.size(); ++i) {
		const Item& item = m_items[i];
		if (std::abs(item.x - px) + std::abs(item.z - pz) > 1 || !IsSeen(item.x, item.z)) continue;
		ItemPose pose;
		if (!FloorItemPose(item, pose)) continue;
		const int self = static_cast<int>(i);
		// Its drawn box: what the floor draw draws (its model, or the tablet),
		// through the matrix it draws with.
		const bool tablet = !item.kind->model;
		Vec3 lo{}, hi{};
		BoxThrough(tablet ? m_runeBoundsMin : item.kind->model->boundsMin,
				   tablet ? m_runeBoundsMax : item.kind->model->boundsMax, pose.world, lo, hi);
		const Vec3 mid{0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)};
		if (item.niche >= 0) {
			++niches;
			const bool hit = pickAt(mid) == self;
			const bool above = pickAt({mid.x, mid.y + kAbove, mid.z}) == self;
			if (!hit || above) ++wrong;
			out.push_back(std::format(
				"pickprobe: niche {} at {},{} {} - drawn about {:.3f} {:.3f} {:.3f}: its middle {}, "
				"a metre above {}",
				item.kind->id, item.x, item.z, DirToken(static_cast<Direction>(item.niche)), mid.x,
				mid.y, mid.z, hit ? "hits" : "MISSES", above ? "HITS" : "misses"));
			continue;
		}
		++floors;
		// A quarter of the square, centred where it is drawn, probed at its drawn
		// middle height: the middle, and 3% of the quarter in from each edge.
		const float q = 0.5f * kCellSize;
		constexpr float kProbes[5][2] = {{0.5f, 0.5f}, {0.03f, 0.5f}, {0.97f, 0.5f},
										 {0.5f, 0.03f}, {0.5f, 0.97f}};
		int hits = 0;
		std::string missed;
		for (const auto& f : kProbes) {
			if (pickAt({mid.x + (f[0] - 0.5f) * q, mid.y, mid.z + (f[1] - 0.5f) * q}) == self) {
				++hits;
				continue;
			}
			missed += std::format("{}({:.2f},{:.2f})", missed.empty() ? " missed at " : " ", f[0], f[1]);
		}
		// Mirrored through the square's centre, at the same height - the quarter
		// across from where it is drawn: never this item.
		const Vec3 centre = m_map.CellCenter(item.x, item.z);
		const bool acrossHit = pickAt({2.0f * centre.x - mid.x, mid.y, 2.0f * centre.z - mid.z}) == self;
		if (hits != 5 || acrossHit) ++wrong;
		out.push_back(std::format(
			"pickprobe: floor {} at {},{} slot {} - drawn {:.3f}..{:.3f} m about {:.2f},{:.2f}, "
			"picked at {:.3f} m: {} of 5 probes hit{}, the quarter across {}",
			item.kind->id, item.x, item.z, item.slot, lo.y, hi.y, mid.x, mid.z, pose.midY, hits,
			missed, acrossHit ? "HITS" : "misses"));
	}

	// --- the door ahead's hand-hold -----------------------------------------------
	const Direction faced = static_cast<Direction>(m_party.Facing());
	if (const Door* door = DoorAt(px + DirDX(faced), pz + DirDZ(faced)); door && door->opener) {
		++openers;
		// The face on the party's side: the nearer of the two copies.
		const Vec3 eye = PartyEye();
		const auto dist2 = [&](const Vec3& p) {
			const Vec3 d = Sub(p, eye);
			return d.x * d.x + d.y * d.y + d.z * d.z;
		};
		const float face = dist2(OpenerPos(*door, 1.0f)) <= dist2(OpenerPos(*door, -1.0f)) ? 1.0f : -1.0f;
		Vec3 local{};
		MeshMiddle(door->opener->model.get(), local);
		const Vec3 mid = OpenerPos(*door, face, local);
		gfx::Camera::Ray ray;
		const bool hit = rayAt(mid, ray) && OpenerUnderRay(*door, ray);
		const bool above = rayAt({mid.x, mid.y + kAbove, mid.z}, ray) && OpenerUnderRay(*door, ray);
		if (!hit || above) ++wrong;
		out.push_back(std::format(
			"pickprobe: opener {} on {} at {},{} - its drawn middle ({:.3f} m up) {}, a metre above {}",
			door->opener->id, door->type, door->x, door->z, mid.y, hit ? "hits" : "MISSES",
			above ? "HITS" : "misses"));
	}

	// --- the wall torch ahead ---------------------------------------------------------
	if (int x = 0, z = 0, wall = -1; FireAheadCell(x, z, wall) && wall >= 0) {
		for (const Fire& f : m_fires) {
			if (f.x != x || f.z != z || f.wall != wall || !f.kind) continue;
			++sconces;
			Vec3 local{};
			MeshMiddle(f.kind->model.get(), local);
			const Vec3 mid = Through(f.world, local);
			float mx = 0.0f, my = 0.0f;
			int sx = 0, sz = 0, sw = -1;
			const bool hit = ScreenPoint(m_camera, mid, mx, my) &&
							 SconceUnderCursor(mx, my, 1.0f, 1.0f, sx, sz, sw);
			const bool above = ScreenPoint(m_camera, {mid.x, mid.y + kAbove, mid.z}, mx, my) &&
							   SconceUnderCursor(mx, my, 1.0f, 1.0f, sx, sz, sw);
			if (!hit || above) ++wrong;
			out.push_back(std::format(
				"pickprobe: sconce {} at {},{} {} - its drawn middle ({:.3f} m up) {}, a metre above {}",
				f.kind->id, f.x, f.z, DirToken(static_cast<Direction>(f.wall)), mid.y,
				hit ? "hits" : "MISSES", above ? "HITS" : "misses"));
			break;
		}
	}

	const int targets = floors + niches + openers + sconces;
	out.push_back(std::format(
		"pickprobe RESULT={} targets={} wrong={} floor={} niche={} opener={} sconce={}",
		targets == 0 ? "NONE" : wrong == 0 ? "PASS" : "FAIL", targets, wrong, floors, niches,
		openers, sconces));
	return out;
}

} // namespace dungeon::game
