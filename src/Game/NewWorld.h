// ============================================================================
// Game/NewWorld.h - how a new world is made (docs/editor-updates-plan.md, P4).
//
// A world is a project folder (W7). Game::CreateWorld makes one from a spec,
// and the NewWorldDialog and the `worlds new` console command are the two
// ways to ask it - so the spec is the one vocabulary both speak.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string>

namespace dungeon::game {

struct NewWorldSpec {
	enum class Source : u8 {
		// The TEMPLATE's content (assets/templates/default, tools/BuildTemplate.py)
		// and one starter room with a way out: a fresh world to build in.
		Blank,
		// THIS world, whole - catalogs, levels, dungeons, overworld, quests and
		// opening - with its UNSAVED edits written into the copy rather than
		// saved here first.
		CopyWorld,
		// This world's content and ONE of its levels, as the only floor of a
		// one-level dungeon; its stairs (which lead to levels the copy lacks)
		// give way to one exit stair out to the new world's overworld.
		CopyLevel,
		// The WIZARD (P5): the template's content, and a first dungeon of one
		// floor GENERATED from the knobs below (the level generator, docs/
		// level-building.md) - themed, sized and as dangerous as asked, with an
		// exit out to the overworld. More floors come after with the editor's [+].
		Wizard,
	};
	Source source = Source::Blank;
	std::string level; // CopyLevel: which of this world's levels
	// Wizard: the content tag the monsters, loot and surfaces are drawn by (""
	// = any), the map's side in squares, how dangerous (0..1), and the seed -
	// the same four give the same floor.
	std::string theme;
	int size = 32;
	float difficulty = 0.4f;
	u32 seed = 1;
};

} // namespace dungeon::game
