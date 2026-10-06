// ============================================================================
// Game/Spell/AllSpells.cpp — the spell registry: every concrete spell class,
// constructed at its defaults. SpellBook::Build starts from this list, then
// lays the project's spells.cat numeric overrides on top. Adding a spell:
// its file pair in this folder, one line here, one in Game's CMakeLists.
// (MakeAllSpells is declared in Spell.h — no separate header for one list.)
// ============================================================================
#include "Game/Spell/Spell.h"

#include "Game/Spell/BoltSpell.h"
#include "Game/Spell/ModifiedSpell.h"
#include "Game/Spell/WardSpell.h"
#include "Game/Spell/LightSpell.h"

#include "Game/Spell/Embersight.h"
#include "Game/Spell/Farsight.h"
#include "Game/Spell/Firebolt.h"
#include "Game/Spell/Firelight.h"
#include "Game/Spell/Fireshield.h"
#include "Game/Spell/Flame.h"
#include "Game/Spell/Gust.h"
#include "Game/Spell/Airbolt.h"
#include "Game/Spell/Rock.h"
#include "Game/Spell/Scrying.h"
#include "Game/Spell/Skylight.h"
#include "Game/Spell/Earthbolt.h"
#include "Game/Spell/Splash.h"
#include "Game/Spell/Stonelight.h"
#include "Game/Spell/Stonesight.h"
#include "Game/Spell/Stoneskin.h"
#include "Game/Spell/Tidelight.h"
#include "Game/Spell/Waterbolt.h"
#include "Game/Spell/Waterveil.h"
#include "Game/Spell/Windward.h"

#include <memory>

namespace dungeon::game {

std::vector<std::unique_ptr<Spell>> MakeAllSpells() {
	std::vector<std::unique_ptr<Spell>> all;
	// Tier 1 - the four one-rune HAND spells (HandSpell.h): a small thing in the hand.
	all.push_back(std::make_unique<spells::Flame>());
	all.push_back(std::make_unique<spells::Rock>());
	all.push_back(std::make_unique<spells::Gust>());
	all.push_back(std::make_unique<spells::Splash>());
	// Tier 2 — the Project form ("throw it ahead") behind each school.
	all.push_back(std::make_unique<spells::Firebolt>());
	all.push_back(std::make_unique<spells::Earthbolt>());
	all.push_back(std::make_unique<spells::Waterbolt>());
	all.push_back(std::make_unique<spells::Airbolt>());
	// Tier 2 — the Protect form ("guard the caster") behind each school.
	all.push_back(std::make_unique<spells::Stoneskin>());
	all.push_back(std::make_unique<spells::Fireshield>());
	all.push_back(std::make_unique<spells::Waterveil>());
	all.push_back(std::make_unique<spells::Windward>());
	// Tier 2 — the Sight form ("see through the wall ahead") behind each school.
	all.push_back(std::make_unique<spells::Embersight>());
	all.push_back(std::make_unique<spells::Farsight>());
	all.push_back(std::make_unique<spells::Stonesight>());
	all.push_back(std::make_unique<spells::Scrying>());
	// Tier 2 - the Light form, Sowilo ("make light"), behind each school
	// (lighting-updates Phase 6).
	all.push_back(std::make_unique<spells::Firelight>());
	all.push_back(std::make_unique<spells::Stonelight>());
	all.push_back(std::make_unique<spells::Skylight>());
	all.push_back(std::make_unique<spells::Tidelight>());
	// Tier 3 - each Project, Protect and Light spell with each MODIFIER rune
	// (ModifiedSpell.h): Ingwaz makes a volley / a party ward / one bigger light,
	// Hagalaz a bursting bolt / a burst round the caster / a dazzling flare. Sight
	// takes no modifier. Built from the
	// list above (the registry owns every spell, so the borrowed base outlives
	// its modified forms). Each takes its defaults from its form only once the
	// form is TUNED - SpellBook::Build calls DeriveFromForm between the forms'
	// spells.cat entries and its own - since here they are class defaults.
	const size_t forms = all.size();
	for (size_t i = 0; i < forms; ++i) {
		const Spell& base = *all[i];
		if (!dynamic_cast<const BoltSpell*>(&base) && !dynamic_cast<const WardSpell*>(&base) &&
			!dynamic_cast<const LightSpell*>(&base))
			continue;
		for (const SpellSymbol m : {SpellSymbol::Multiple, SpellSymbol::Explode})
			all.push_back(std::make_unique<ModifiedSpell>(base, m));
	}
	return all;
}

} // namespace dungeon::game
