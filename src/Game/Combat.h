// ============================================================================
// Game/Combat.h — the strike resolver and the DAMAGE TYPE REGISTRY.
//
// Combat is pure data here: the attacker flattens into an AttackProfile (typed
// damage + accuracy), the defender into a DefenseProfile (evasion + soak + the
// resist already resolved for the incoming type), and ResolveAttack rolls one
// strike under the StrikeRules knobs (Balance::Strike()). Keeping it state-free
// means melee, monster blows, and spell bolts reuse the same math — they just
// build the profiles differently. The formula lives in docs/combat.md ("The
// attack formula"); the knobs live in the project's balance.cat (Balance.h).
//
// DAMAGE TYPES ARE DATA (damagetypes.cat), not a compiled enum. A DamageType
// is an opaque INDEX into the loaded table — it has no names in C++, because a
// name here would be a type the project could not remove and a project type
// the code could not see. Everything that used to be a fact about the enum's
// ORDER is now a FIELD on the type:
//
//   IsPhysical   was `t <= Bash`, an ordering trick — now the `physical` flag,
//                asked of the book. Its one caller is Stone Skin deciding what
//                it hardens against, and "the first three of seven" was never
//                really what that meant.
//   ForSchool    was a switch from SpellSymbol to the four elements — now the
//                `school` field, so a project can add a damage type and give
//                it a school without touching C++.
//
// THE CEILING, not a count: kMaxDamageTypes sizes every ResistTable, and a
// ResistTable rides Character, MonsterKind and ItemKind. Making it a runtime
// vector would put a heap allocation on those and break the steady-state rule
// (docs/ARCHITECTURE.md "Memory strategy"), so it is a fixed ceiling with a
// runtime count — the same trade as kMaxPointLights and kMaxSkinJoints. The
// live count is 7; arriving at the ceiling is an authoring error the book
// reports, not a limit to design around.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Game/Spells.h" // SpellSymbol (a type's school, below)

#include <array>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

class Catalog;

// The most damage types one project may define. See "THE CEILING" above.
inline constexpr size_t kMaxDamageTypes = 16;

// One damage type, as an index into the loaded table. Deliberately NOT an enum
// and deliberately not implicitly convertible to an integer: the whole point is
// that no C++ site can name a particular type, and an accidental `int` would
// let the old ordering assumptions creep back in.
struct DamageType {
	u8 index = 0;
	friend bool operator==(DamageType, DamageType) = default;
};

// ARMOR WEIGHT CLASS (docs/damage-system.md). The trade the whole system turns
// on: heavier armor blunts more and is HARDER TO AVOID IN, and the penalty has
// a FLOOR that training can never reach past — so plate is a permanent
// commitment rather than a phase you grow out of. `None` is not a gap in the
// list: going unarmored is the only way to use the `avoid` skill at all.
enum class ArmorClass : u8 { None, Light, Medium, Heavy, Count };
const char* ArmorClassId(ArmorClass c);
bool ParseArmorClass(std::string_view token, ArmorClass& out);
// The skill trained by wearing one ("light_armor" ...), and the skill trained
// by dodging in none ("avoid"). Both are ordinary skill ids: they level, creep
// their stat, and read out of the sheet like every weapon class.
const char* ArmorSkillId(ArmorClass c);
inline constexpr const char* kAvoidSkill = "avoid";

// A defender's per-type resistance cells: the fraction of that type shrugged
// off (0.5 = half), NEGATIVE = vulnerability (-0.5 = half again). Sources
// (nature, equipment, wards) each hold one of these and SUM into the defense;
// the clamp rule lives in Balance::ClampResist.
struct ResistTable {
	std::array<float, kMaxDamageTypes> cells{};
	float& operator[](DamageType t) { return cells[t.index]; }
	float operator[](DamageType t) const { return cells[t.index]; }
	void Add(const ResistTable& o) {
		for (size_t i = 0; i < kMaxDamageTypes; ++i) cells[i] += o.cells[i];
	}
};

// The registry: every damage type the project defines, loaded from
// damagetypes.cat. Built once per load and owned by DungeonWorld beside the
// EffectBook — every DamageType index in the world refers into it, so it must
// outlive them.
//
// An ABSENT or EMPTY catalog seeds the seven the game shipped with, so a
// project that predates the file still runs (the en.lang fallback idiom). That
// is a compatibility floor, not a default to rely on: a project that wants its
// own types authors them.
class DamageTypeBook {
public:
	struct Entry {
		std::string id;      // catalog token: "fire", "slash", ...
		std::string nameKey; // loc key for the UI
		bool physical = false;
		bool hasSchool = false;
		SpellSymbol school = SpellSymbol::Fire;
	};

	void Build(const Catalog& catalog);

	// Resolve a catalog/record token. Returns false (and leaves `out`) for an
	// unknown id, so every caller can warn with its own context — a silent
	// fallback to index 0 would quietly retype content.
	bool Find(std::string_view id, DamageType& out) const;
	// The same, for the handful of sites with a sensible fallback.
	DamageType FindOr(std::string_view id, DamageType fallback = {}) const;

	const std::string& Id(DamageType t) const;
	const std::string& NameKey(DamageType t) const;
	bool IsPhysical(DamageType t) const;

	// The damage type a school's magic deals, or the fallback when no type
	// claims that school (the form runes never deal damage on their own).
	DamageType ForSchool(SpellSymbol school) const;
	// The reverse: the school a type belongs to, if any. False for the
	// physical types and for any elemental one a project leaves school-less.
	// This is what makes a PER-SCHOOL magic defense expressible — a ward held
	// in a casting hand has to know whether the incoming damage is its
	// business.
	bool SchoolOf(DamageType t, SpellSymbol& out) const;

	size_t Count() const { return m_entries.size(); }
	const std::vector<Entry>& Entries() const { return m_entries; }
	bool Empty() const { return m_entries.empty(); }

private:
	std::vector<Entry> m_entries;
	// school -> type, resolved once at Build so a per-tick lookup is an index.
	std::array<DamageType, kSymbolCount> m_bySchool{};
	std::array<bool, kSymbolCount> m_hasSchool{};
};

// A combatant's offensive profile for one strike (built from the weapon +
// attack + stats formula, a monster's catalog stats, or a spell's power).
struct AttackProfile {
	float damage = 1.0f; // base damage on a clean hit (formula-assembled)
	// The attacker's side of the opposed roll, in d100 POINTS — skill curve +
	// stat curve + the verb's own modifier for a party member, an authored
	// figure for a monster. NOT a probability: it is added to a d100 and
	// compared against the defender's total, so 40 points is roughly the
	// deviation of the dice themselves and 100 is decisive.
	float attackBonus = 50.0f;
	DamageType type{}; // what the defender resists it AS
};

// A combatant's defensive response to ONE incoming strike. The caller resolves
// the per-type resist (summed + clamped) before building this, so the resolver
// stays a pure function of numbers.
struct DefenseProfile {
	// The defender's side of the opposed roll, in the same POINTS.
	float defenseBonus = 0.0f;
	float soak = 0.0f;   // small flat damage soaked on a hit (armor)
	float resist = 0.0f; // resolved resist[type]: fraction shrugged (+) / amplified (−)
};

// The resolver's knobs, filled from Balance (balance.cat) by Balance::Strike().
struct StrikeRules {
	float hitFloor = 0.05f, hitCeil = 0.95f; // (legacy, unused by the roll)
	float damageJitter = 0.15f;              // ± roll on every hit
	float woundFloor = 1.0f;                 // a landed blow always stings

	// --- the opposed roll (docs/damage-system.md) ---------------------------
	// Attacker and defender each add a d100 to a bonus and the higher total
	// wins. The bonuses arrive already in POINTS — rollScale is gone, and with
	// it the last of the 0..1 accuracy model it was bridging.
	float critThreshold = 95.0f;  // >= this face re-rolls and adds
	float fumbleThreshold = 5.0f; // <= this on the first face is a fumble
	float maxEscalations = 20.0f; // termination guard, NOT balance (Roll.h)

	// The margin (attack total - defense total) MULTIPLIES the hit: a
	// massively superior attack does not merely land, it lands harder. Capped
	// because the two compound — the roll is open-ended, so a lucky swing
	// widens the margin AND the margin scales the damage, and RollTest
	// measures the extreme margin at ~9x the typical winning one.
	float marginDamage = 0.01f; // + this much multiplier per point of margin
	float marginCap = 3.0f;     // ceiling on the resulting multiplier
};

// One resolved strike.
struct AttackResult {
	bool hit = false;
	float damage = 0.0f; // damage to apply (>= 0; 0 on a miss)
	// What the dice did, for the narration and for the crit/fumble hooks (P8).
	bool crit = false;   // the attack roll went open-ended
	bool fumble = false; // the attack's first face was <= fumbleThreshold
	int margin = 0;      // attack total - defense total (negative on a miss)
};

// Resolves a single strike with `rng`. Hit chance is (accuracy - evasion)
// clamped to the rules' floor/ceiling; on a hit the damage is jittered, then
// soaked and resisted — final = (rolled − soak) × (1 − resist) — and floored
// so a landed blow always stings. No game state is touched — the caller
// applies the result.
AttackResult ResolveAttack(const AttackProfile& atk, const DefenseProfile& def,
						   const StrikeRules& rules, std::mt19937& rng);

} // namespace dungeon::game
