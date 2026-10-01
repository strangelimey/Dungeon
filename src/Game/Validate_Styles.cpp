// ============================================================================
// Game/Validate_Styles.cpp - the STYLE checks (docs/tool-refinement-plan.md
// Phase 5; see CheckStyles in Validate.h).
//
// A style is a bundle of references - two themes and a list of monsters - and
// one added from the library can name a monster this world does not have (the
// add reports it, and does not copy monsters). These are WARNINGS: a style with
// a missing monster still paints, it only cannot place that one, and saying so
// once in the report beats a silent gap in what the generator offers.
// ============================================================================
#include "Game/Validate.h"

#include <algorithm>

namespace dungeon::game::validate {

void CheckStyles(const WorldView& world, std::vector<Issue>& issues) {
	for (const StyleView& s : world.styles) {
		for (const std::string& t : s.themes)
			if (!world.themeIds.count(t))
				issues.push_back({Severity::Warning, "", -1, -1, "map.check.stylenotheme", s.id, t});
		for (const std::string& m : s.monsters)
			if (!world.monsterIds.count(m))
				issues.push_back({Severity::Warning, "", -1, -1, "map.check.stylenomonster", s.id, m});
	}
	for (const DungeonView& d : world.dungeons) {
		if (d.style.empty()) continue;
		const bool found = std::any_of(world.styles.begin(), world.styles.end(),
									   [&](const StyleView& s) { return s.id == d.style; });
		if (!found)
			issues.push_back({Severity::Warning, "", -1, -1, "map.check.dungeonnostyle", d.id, d.style});
	}
}

} // namespace dungeon::game::validate
