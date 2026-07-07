// ============================================================================
// Game/Spell/AllSpells.cpp — the spell registry: every concrete spell class,
// constructed at its defaults. SpellBook::Build starts from this list, then
// lays the project's spells.cat numeric overrides on top. Adding a spell:
// its file pair in this folder, one line here, one in Game's CMakeLists.
// (MakeAllSpells is declared in Spell.h — no separate header for one list.)
// ============================================================================
#include "Game/Spell/Spell.h"

#include "Game/Spell/Fireburst.h"
#include "Game/Spell/Fireshield.h"
#include "Game/Spell/Flame.h"
#include "Game/Spell/Gust.h"
#include "Game/Spell/Push.h"
#include "Game/Spell/Rock.h"
#include "Game/Spell/Slingshot.h"
#include "Game/Spell/Splash.h"
#include "Game/Spell/Stoneskin.h"
#include "Game/Spell/Waterbolt.h"
#include "Game/Spell/Waterveil.h"
#include "Game/Spell/Windward.h"

#include <memory>

namespace dungeon::game {

std::vector<std::unique_ptr<Spell>> MakeAllSpells() {
	std::vector<std::unique_ptr<Spell>> all;
	// Tier 1 — the four one-rune school bolts.
	all.push_back(std::make_unique<spells::Flame>());
	all.push_back(std::make_unique<spells::Rock>());
	all.push_back(std::make_unique<spells::Gust>());
	all.push_back(std::make_unique<spells::Splash>());
	// Tier 2 — the Project form ("throw it ahead") behind each school.
	all.push_back(std::make_unique<spells::Fireburst>());
	all.push_back(std::make_unique<spells::Slingshot>());
	all.push_back(std::make_unique<spells::Waterbolt>());
	all.push_back(std::make_unique<spells::Push>());
	// Tier 2 — the Protect form ("guard the caster") behind each school.
	all.push_back(std::make_unique<spells::Stoneskin>());
	all.push_back(std::make_unique<spells::Fireshield>());
	all.push_back(std::make_unique<spells::Waterveil>());
	all.push_back(std::make_unique<spells::Windward>());
	return all;
}

} // namespace dungeon::game
