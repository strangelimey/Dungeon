// ============================================================================
// Game/Spell/Spell.h — the spell base class: THE home of spell behaviour.
//
// Every castable spell is a concrete class in this folder (one file pair per
// spell) deriving from Spell — directly or through the shared-form bases
// (BoltSpell, WardSpell). The base carries what every spell has: the catalog
// id, the display-name / description loc keys, the SYMBOL RECIPE (whose first
// rune IS the school, per the school rule), the mana cost, and the base
// power. What a cast DOES lives in the pure virtual Cast() override — plain
// C++, deliberately (see the content-stays-data-driven decision: typed data +
// C++ behaviour, no scripting), but ISOLATED here so each spell's effect has
// exactly one home. Numbers stay tunable without a rebuild: the project's
// spells.cat entries are numeric OVERRIDES laid over the class defaults
// (ApplyOverrides), applied by SpellBook::Build.
//
// Spells reach the world only through CastServices — a capability surface the
// host (DungeonWorld) wires once, like the MagicSystem's projectile hooks —
// so the module stays walled off from map/monsters/HUD.
// ============================================================================
#pragma once

#include "Game/Projectiles.h"
#include "Game/Spells.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dungeon::game {

struct Character;
struct CatalogEntry;

// What a Cast() may DO beyond touching the caster — wired by the host once
// (DungeonWorld ctor) and handed to every cast.
struct CastServices {
	// Spawn a bolt into the moving-item engine ("onto the map").
	std::function<void(const ProjectileSpec&)> spawnBolt;
	// A log line ABOUT a member, tinted with their identity color.
	std::function<void(const Character&, const std::string&)> message;
};

// Everything a single cast knows: who, from where, at what strength. The
// gates (vocabulary, mana, the skill/fumble roll) have already passed by the
// time a Cast() sees this.
struct CastContext {
	Character& caster;
	Vec3 origin;      // the party eye — where a bolt is born
	Vec3 dir;         // the faced cardinal — where it flies
	float power;      // the spell's base power scaled by school skill
	int schoolLevel;  // the caster's level in the spell's school
	CastServices& services;
};

class Spell {
public:
	// `sequence` must obey the school rule (exactly one school rune, first) —
	// SpellBook::Build asserts it, so a malformed recipe fails loudly in the
	// registry, not silently at match time.
	Spell(std::string id, std::vector<SpellSymbol> sequence, float power,
		  float mana);
	virtual ~Spell() = default;

	// What happens when the cast lands — THE isolated hard-coding. Each
	// concrete spell's effect lives in its override (a bolt spawns, a ward
	// settles, a future wall rises...).
	virtual void Cast(CastContext& ctx) const = 0;

	// The spell thrown by a MONSTER caster (monsters.cat `spell = <id>`):
	// its bolt aimed at the party at the monster's accuracy, or nullopt for
	// a spell with no thrown form (a ward) — the caller falls back to its
	// plain shot. Monsters cast at base power (no school skill).
	virtual std::optional<ProjectileSpec> MonsterBolt(const Vec3& origin,
													  const Vec3& dir,
													  float accuracy) const;

	// Lay the project's spells.cat NUMBERS over the class defaults (matching
	// entry by id; SpellBook::Build calls this once per load). The base takes
	// name/power/mana; derived classes add their own fields. The RECIPE is
	// class-only — identity never comes from data.
	virtual void ApplyOverrides(const CatalogEntry& e);

	const std::string& Id() const { return m_id; }
	const std::string& NameKey() const { return m_nameKey; } // "spell.<id>"
	const std::string& DescKey() const { return m_descKey; } // "spell.<id>.desc"
	std::span<const SpellSymbol> Sequence() const { return m_sequence; }
	// The school rule makes the first rune the spell's school (and colour).
	SpellSymbol School() const { return m_sequence.front(); }
	// The skill roll's difficulty: the recipe's rune count (docs/skills.md).
	int Difficulty() const { return static_cast<int>(m_sequence.size()); }
	float Power() const { return m_power; }
	float Mana() const { return m_mana; }

protected:
	std::string m_id;
	std::string m_nameKey;
	std::string m_descKey;
	std::vector<SpellSymbol> m_sequence;
	float m_power;
	float m_mana;
};

// Every concrete spell, freshly constructed at class defaults — the registry
// SpellBook::Build starts from (AllSpells.cpp includes each spell's header;
// adding a spell = its file pair + one line there + a CMakeLists entry).
std::vector<std::unique_ptr<Spell>> MakeAllSpells();

} // namespace dungeon::game
