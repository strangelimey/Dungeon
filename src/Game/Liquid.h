// ============================================================================
// Game/Liquid.h - the liquid inside a glass container, GENERATED from the
// container's own glass (transparency Phase 3; docs/transparency-plan.md).
//
// Michael's call: one bottle model serves every potion, so the liquid is not
// authored per model - its SHAPE comes from the glass and its COLOUR from the
// item (items.cat `liquid_color`). Pure: MeshData in, MeshData out, no device.
//
// THE SHAPE is the glass's INNER WALL, turned inside out. A double-walled
// bottle (tools/BuildPotion.py, and most bought glass) has an inside surface
// whose faces point INTO the cavity: toward the axis on the walls, up on the
// cavity floor. Those faces, pushed a hair further into the cavity, reversed
// and given the opposite normal, are exactly a liquid that fills the bottle.
// A single-walled model has no inside, so the fallback is the whole glass
// shrunk toward its axis.
//
// THE LEVEL is not cut into the mesh - the shell runs to the lip, and the
// shader clips it at a plane (MaterialParams::liquidLevel, object space), so
// the fill can move without rebuilding anything. The cut's open top is closed
// by the shader too: a back face of the shell, seen through the opening, is
// lit as the flat surface it stands in for.
// ============================================================================
#pragma once

#include "Assets/Model.h"

namespace dungeon::game::liquid {

struct Shell {
	assets::MeshData mesh;  // the liquid surface, outward-facing, in the glass's space
	float bottom = 0.0f;    // the cavity's lowest point (object-space y)
	float top = 0.0f;       // and its highest (the inside of the lip)
	bool fromInnerWall = false; // false = the single-wall fallback was used
	size_t triangles = 0;
};

// Builds the liquid shell for `glass` (node transform already baked into its
// vertices). Empty mesh if the glass has no triangles.
Shell Build(const assets::MeshData& glass);

// Object-space y of a fill `fraction` (0..1) of the shell's cavity height.
inline float Level(const Shell& s, float fraction) {
	return s.bottom + (s.top - s.bottom) * fraction;
}

} // namespace dungeon::game::liquid
