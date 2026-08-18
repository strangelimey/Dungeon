// ============================================================================
// Game/Placement.cpp — see Placement.h.
// ============================================================================
#include "Game/Placement.h"

#include "Game/Catalog.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Per-category defaults, so an entry only authors `mount` when it differs from
// what its category obviously is. Doors are the case worth naming: a door is
// ALWAYS a doorway, so the type never says so and could not usefully disagree.
Mount DefaultMount(std::string_view catalogKey) {
	if (catalogKey == "doors") return Mount::Doorway;
	// Floor items come FOUR to a cell (the Medium 2x2 quarter grid), so the cell
	// is not the unit for them and the pointer's position inside it matters.
	// All three item catalogs are one runtime kind, so all three agree here.
	if (catalogKey == "items" || catalogKey == "weapons" || catalogKey == "armor")
		return Mount::FloorSlot;
	// Both wall features resolve from a face: a niche carves the face itself, a
	// bore tunnels through the block behind it (the face gives the axis).
	if (catalogKey == "wallfeatures") return Mount::Wall;
	return Mount::Floor;
}
} // namespace

Mount MountFor(std::string_view catalogKey, const CatalogEntry* entry) {
	const Mount fallback = DefaultMount(catalogKey);
	// NAME COLLISION, and the reason this is a hard `return` and not a comment:
	// doors.cat ALREADY has a `mount` field meaning something else entirely — a
	// CatalogRef to a static sub-part drawn with the door but never moved (the
	// socket a chain is drawn out of, see CatalogSchema.cpp). Reading it here
	// would parse "door_chain_socket" as a mount token. It happens to land on the
	// right answer via the unknown-token fallback below, which is exactly the
	// kind of accident that survives until someone names a door part "wall".
	// A door is always a doorway and could not usefully say otherwise, so the
	// field is never consulted for that catalog.
	if (catalogKey == "doors") return fallback;
	const std::string tok = CatalogGet(entry, "mount", "");
	if (tok.empty()) return fallback;
	if (tok == "wall") return Mount::Wall;
	if (tok == "doorway") return Mount::Doorway;
	if (tok == "floor") return Mount::Floor;
	// An unknown token is authored content the schema doesn't know yet. Fall
	// back rather than refuse: a typo should misplace a prop, not make a whole
	// category unplaceable with no way to see why from inside the editor.
	return fallback;
}

Placement Resolve(const DungeonMap& map, Mount mount, int cx, int cz,
				  const WallFace& face, float fx, float fz) {
	Placement p;
	p.mount = mount;
	p.x = cx;
	p.z = cz;

	switch (mount) {
	case Mount::Wall:
		// The click names one wall of one cell, so a corridor's two walls (and a
		// lone block's four faces) are each reachable, and repeat clicks do not
		// march around the cell in N/E/S/W order. Both sides of a boundary
		// resolve to the same face, which is why the cell can move here.
		if (!face.valid) {
			p.refusalKey = "map.place.nowall";
			return p;
		}
		p.x = face.x;
		p.z = face.z;
		p.facing = face.wall;
		p.facingDerived = true;
		p.valid = true;
		return p;

	case Mount::Doorway:
		// Shares DungeonWorld's detection rather than re-deriving it — the whole
		// point of resolving in one place is that the preview cannot drift from
		// the commit.
		if (!DungeonMap::DoorwayFacing(map, cx, cz, p.facing)) {
			p.refusalKey = "map.place.nodoorway";
			return p;
		}
		p.facingDerived = true;
		p.valid = true;
		return p;

	case Mount::FloorSlot:
		// Same floor rule, plus the sub-cell point the pointer named. Carried as
		// a WORLD position rather than a quarter index so the editor can hand it
		// straight to the existing nearest-free-quarter search.
		if (!map.IsWalkable(cx, cz)) {
			p.refusalKey = "map.place.nofloor";
			return p;
		}
		p.subX = (static_cast<float>(cx) + std::clamp(fx, 0.0f, 1.0f)) * kCellSize;
		p.subZ = (static_cast<float>(cz) + std::clamp(fz, 0.0f, 1.0f)) * kCellSize;
		p.valid = true;
		return p;

	case Mount::Floor:
	default:
		// A square you could not stand in cannot hold a standing thing. This is
		// the check the placement calls make anyway; hoisting it here is what
		// lets the ghost refuse before the click rather than after it.
		if (!map.IsWalkable(cx, cz)) {
			p.refusalKey = "map.place.nofloor";
			return p;
		}
		p.valid = true;
		return p;
	}
}

} // namespace dungeon::game
