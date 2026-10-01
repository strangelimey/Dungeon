// ============================================================================
// Game/StyleLook.h - what a STYLE makes of a level that is written as text
// (docs/tool-refinement-plan.md Phase 7).
//
// The shape brushes paint a style square by square as they carve (MapEditor_
// Shapes.cpp). Three things write a whole level at once instead - a generated
// floor, the [+] box, a new world's first floor - and none of them has a brush
// to do it, so this answers the same question for a grid: which surfaces the
// level's palettes need, and the `theme` records that lay the style's room theme
// on its rooms and its corridor theme on its passages (carve::Dress decides
// which is which). Records name the theme BY ID, so the level stays linked to it:
// editing the theme later repaints the level, the themes system's rule.
//
// Also the style's other two answers, so every caller reads them one way: its
// tags, and its monster list as a pool (the ids the project has, each with its
// weight).
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Game/Catalog.h"

#include <array>
#include <string>
#include <vector>

namespace dungeon::game {

struct Project;

namespace stylelook {

// The project's style `id`, or null ("" or unknown).
const CatalogEntry* Find(const Project& project, const std::string& id);

// The style's `tags`, lowercased (Catalog.h's parse).
std::vector<std::string> Tags(const CatalogEntry& style);

// The style's monster list, as the ids the project HAS (one it lacks is the
// checker's to report, not a reason to place nothing) with their weights, in
// list order. Empty when none of them exists.
void Monsters(const Project& project, const CatalogEntry& style, std::vector<std::string>& ids,
			  std::vector<float>& weights);

// The look laid on a grid (row-major, 1 = open, w x h).
struct Look {
	// Per surface (wall, floor, ceiling): the style's theme members the level's
	// palette must hold for its records to show - the room theme's first. A
	// surface neither theme names is empty; the caller keeps its own palette there.
	std::array<std::vector<std::string>, 3> palettes;
	// `theme <surface> <x> <z> <id>` lines, one per square and surface the style
	// paints, in row-major order. Empty when the style names no theme the
	// project has.
	std::string records;
};
Look Lay(const Project& project, const CatalogEntry& style, const std::vector<u8>& floor, int w,
		 int h);

// A FRESH level's palettes under a style: per surface the style's members where
// it names any, else `donor`'s - not both, since every square the style reaches
// wears its theme and each palette entry is a texture set the level loads.
std::array<std::vector<std::string>, 3>
Palettes(const Look& look, const std::array<std::vector<std::string>, 3>& donor);

} // namespace stylelook
} // namespace dungeon::game
