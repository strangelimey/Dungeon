// ============================================================================
// Game/Carve.h - the SHAPE BRUSHES' geometry (docs/tool-refinement-plan.md
// Phase 6): which squares a corridor, a room, a stamp or a region opens.
//
// Michael wanted all four ways to lay shape - drag a path, drag a room, stamp a
// shape, generate a region - each in the current STYLE (Game/Style.h). This
// module answers only "which squares": it opens floor and, for a stamp, marks
// squares solid; MapEditor_Shapes.cpp turns that into one undoable edit and
// paints the style's themes on what opened and the walls around it.
//
// A BRUSH NEVER RAISES A WALL ROUND WHAT IT CARVES. It opens rock and paints the
// rock it borders; a corridor dragged across open floor leaves that floor open.
// Raising walls would let a careless drag cut an existing room in two, and the
// map's rock is already the wall everywhere a brush carves into it. (A stamp's
// '#' squares are the exception, and say so: a pillar is part of its shape.)
//
// PURE and DETERMINISTIC: squares in, squares out, a seed for anything random -
// so the hover preview and the commit are the same squares, and RollTest can
// check each shape without a map.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dungeon::game::carve {

// What a square opened as: the style paints a room's theme and a corridor's
// differently.
enum class Role : u8 { Room, Corridor };

struct Square {
	int x = 0, z = 0;
	Role role = Role::Room;
};

// A brush's whole effect: the squares to open (each with its role), and the
// squares a stamp makes solid. Each square once, in a stable order.
struct Shape {
	std::vector<Square> open;
	std::vector<std::pair<int, int>> solid;
	bool Opens(int x, int z) const;
};

// A CORRIDOR from (ax,az) to (bx,bz), `width` squares wide (1..3). A low
// `winding` (< 0.5) is one L bend - which way round from the seed; a higher one
// meanders: a walk that steps toward the far end, and sideways (never back)
// with a chance that rises with winding, so it always arrives. Both ends are
// part of it.
Shape Corridor(int ax, int az, int bx, int bz, int width, float winding, u32 seed);

// A ROOM: every square of the rectangle between the two corners, inclusive.
Shape Room(int ax, int az, int bx, int bz);

// A STAMP: a small grid of rows (shapes.cat `rows`, rows split by '|'):
//   '.' opens a room square   '#' makes a square solid   anything else leaves
// it as it is. Placed CENTRED on (cx,cz), turned `turns` quarter turns
// clockwise.
struct Stamp {
	std::vector<std::string> rows;
	int Width() const;
	int Height() const { return static_cast<int>(rows.size()); }
};
Stamp ParseStamp(std::string_view text);
// The grid turned one quarter clockwise (four turns give the grid back).
Stamp Turned(const Stamp& s);
Shape StampAt(const Stamp& s, int cx, int cz, int turns);

// A REGION: a generated floor grid (row-major, 1 = floor, `w` x `h`) laid with
// its top-left square on (x0,z0) - its squares are room squares - and then
// JOINED to what is already open around it: each run of open squares touching
// the region's border from outside gets one corridor from the border to the
// nearest generated floor. `isOpen` asks the map. The join corridors are one
// square wide and L-bent (from `seed`).
Shape Region(const std::vector<u8>& floor, int w, int h, int x0, int z0,
			 const std::function<bool(int, int)>& isOpen, u32 seed);

// The squares round a shape's open ones (all eight neighbours) that it does
// not open itself, each with the role of the square it borders: where the
// style's WALL paint goes, on whichever of them is solid.
std::vector<Square> Rim(const Shape& s);

// A STYLE LAID OVER A WHOLE GRID (Phase 7): what a generated level, a styled
// starter room and a new world's first floor paint their themes by, since none
// of them was carved by a brush that knew which squares were which.
// `floor` is row-major, 1 = open, `w` x `h`. Every open square comes back with
// its role - a ROOM's when it sits in any 2x2 block of open squares, else a
// CORRIDOR's (Game/Area.h's rule, on a bare grid) - and every solid square of
// the grid with an open square among its eight neighbours comes back as a WALL,
// wearing a room's theme when it borders any room square and a corridor's only
// when it borders nothing else. Both lists in row-major order.
struct Dressing {
	std::vector<Square> open, walls;
};
Dressing Dress(const std::vector<u8>& floor, int w, int h);

} // namespace dungeon::game::carve
