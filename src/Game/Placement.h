// ============================================================================
// Game/Placement.h — where a type may be placed, and where exactly it lands.
//
// THE PROBLEM THIS SOLVES. Snapping and surface rules should be automatic, not
// manual — the editor should never ask for precision the content does not need.
// The world is a discrete grid, so the pixel-nudging of other level editors
// cannot happen here by construction; the fiddliness that CAN happen lives in
// the three places where a cell is not the unit: WALL FACES, FACING, and the
// sub-cell slots. Those are the whole surface area, and this is where they are
// decided.
//
// Before this, every rule was hardcoded per kind — a sconce resolved its mount
// wall, the door brush auto-detected its axis, wall decorations edge-picked a
// face — each written separately for its own category. A NEW type got none of
// it without new C++. Here it is one table lookup plus one resolver, so adding
// a type is a catalog row.
//
// THE FIELD IS `mount`, WHICH ALREADY EXISTED. Its vocabulary is widened rather
// than a second field added beside it: `mount` and a new `surface` would be two
// fields answering one question, which is the mistake `category` vs `tags`
// deliberately avoids (there they answer DIFFERENT questions — see Catalog.h).
// Per-category defaults mean most entries never author it at all.
//
// THE ONE RULE THAT MAKES THE GHOST HONEST: Resolve() is called TWICE — once by
// the hover preview and once by the commit. A preview computed by a second
// implementation can disagree with the real placement, and a preview that lies
// is worse than no preview at all. So nothing may resolve a placement except
// through here.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Game/DungeonMap.h"
#include "Game/Entity.h"

#include <string_view>

namespace dungeon::game {

struct CatalogEntry; // a struct, and C4099 is loud about the difference

// What a type attaches to. The authored `mount` token, parsed.
enum class Mount : u8 {
	Floor,   // the square itself: monsters, standing props, buttons, features
	Wall,    // a wall FACE: sconces, banners, niches, bores
	Doorway, // a cell with solid walls flanking exactly one axis: doors
};

// Where a placement actually lands, and whether it may happen at all.
//
// `x`/`z` is the cell the record is authored at — for a Wall mount that is the
// WALKABLE cell the face belongs to, not the block behind it, which is why it
// can differ from the cell under the pointer.
struct Placement {
	bool valid = false;
	// Loc key explaining a refusal, so the ghost can say why rather than just
	// vanishing — "nothing to hang this on" is a different problem from "that
	// square is taken", and a preview that only disappears teaches neither.
	const char* refusalKey = nullptr;
	Mount mount = Mount::Floor;
	int x = 0, z = 0;
	Direction facing = Direction::South;
	// Whether `facing` was DERIVED (a wall to hang on, a doorway axis) rather
	// than defaulted. The ghost draws an arrow only when it means something.
	bool facingDerived = false;
};

// The `mount` an entry declares, or the category's default when it is silent.
// `catalogKey` is the project catalog key ("decorations", "doors", ...).
Mount MountFor(std::string_view catalogKey, const CatalogEntry* entry);

// Resolve a hovered cell (and the wall face the pointer picked, if any) to the
// placement it would produce. Pure: reads the map, mutates nothing, and is safe
// to call every frame from the hover path.
//
// `face` may be invalid — that is itself an answer for a Wall mount (the
// pointer is not over a floor/rock boundary, so there is nothing to hang on).
Placement Resolve(const DungeonMap& map, Mount mount, int cx, int cz,
				  const WallFace& face);

} // namespace dungeon::game
