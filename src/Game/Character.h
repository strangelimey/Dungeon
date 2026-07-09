// ============================================================================
// Game/Character.h — party member data.
//
// One Character per party slot (up to four). The numbers feed the HUD party
// bar and the character sheet (see PartyHud.h) and the attack formula
// (docs/combat.md): the stat accessors below (StatValue/StatAvg/Evasion) are
// the character's inputs to the strike DungeonWorld assembles, and `health`
// actually drains when a monster lands a blow.
// Portraits are baked by AssetBaker (portrait_<name>.png); the tinted square
// stamped with the character's initial remains as the fallback when the
// texture is missing.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/Combat.h" // ResistTable (the race/nature defense layer)
#include "Game/Inventory.h"
#include "Game/Spells.h"

#include <cmath>
#include <flat_map>
#include <flat_set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::gfx {
class Texture;
}

namespace dungeon::game {

// ============================================================================
// Status effects — every transient condition on a member lives in ONE list
// (Character::effects): a Protect ward today; poison, injuries, item buffs
// later. The HUD's portrait effect strip and the character sheet read the
// list; behaviour code queries it by kind (the ward helpers below). The
// world tick ages timeLeft and removes an expired effect with its kind's
// fade message; spend-to-die effects (the water pool, the air charges) are
// removed at their SPEND site instead, so their burst/still lines replace
// the fade line.
// ============================================================================
enum class StatusKind : u8 {
	Ward, // the Protect shields — school keys the behaviour (see Character)
};

struct StatusEffect {
	StatusKind kind = StatusKind::Ward;
	// School flavour: a ward's behaviour key and every effect's HUD tint
	// (ElementColor). Non-school effects (a future poison) pick a school
	// purely for the tint until a richer palette exists.
	SpellSymbol school = SpellSymbol::Fire;
	std::string nameKey;    // loc key for the display name ("spell.stoneskin")
	float timeLeft = 0.0f;  // seconds; the world tick removes at <= 0
	float duration = 0.0f;  // starting timeLeft (the HUD's depletion fraction)
	float magnitude = 0.0f; // kind-keyed number (armor / burn / pool / charges)
};

// Save/record token names for StatusKind ("ward"). Unknown tokens on load are
// skipped, so a newer save's effect kinds degrade to "not present".
const char* StatusKindId(StatusKind kind);
bool ParseStatusKind(std::string_view token, StatusKind& out);

struct Character {
	std::string name; // proper noun — not localized

	// Resources. The maxima are DERIVED (docs/combat.md "The resource
	// formula"): max = base + k × statAvg, recomputed by RecomputeMaxima
	// whenever a stat or a balance knob changes — the base is the authored
	// per-member value (class identity), saved since v17.
	float health = 1.0f, maxHealth = 1.0f;
	float stamina = 1.0f, maxStamina = 1.0f;
	float mana = 1.0f, maxMana = 1.0f;
	float baseHealth = 1.0f, baseStamina = 1.0f, baseMana = 1.0f;

	int strength = 10;
	int dexterity = 10;
	int vitality = 10;
	int willpower = 10;
	int intelligence = 10; // drives mana regen (see ManaRegenPerSec)

	// --- combat (live) ------------------------------------------------------
	// Seconds until each hand can swing again (index 0 = left, 1 = right),
	// ticked down per-hand by DungeonWorld while playing. A hand-slot click only
	// lands a blow when THAT hand's timer is <= 0, so the hands swing
	// independently (dual-wield, weapon + shield, etc.).
	float handCooldown[2] = {0.0f, 0.0f};

	// Hit-feedback splat shown over this member's portrait when a monster lands
	// a blow: hitFlash counts down (seconds) while the party bar draws the icon,
	// and hitSeverity picks which one (0 = small, 1 = medium, 2 = hard). The
	// severity thresholds are placeholder — "what a hit means" is TBD. A fresh
	// party (CreateDefaultParty) clears these.
	float hitFlash = 0.0f;
	int hitSeverity = 0;

	// Whether the member is still standing. health <= 0 = down (out of the
	// fight, no longer a valid monster target) — there is no death/revive
	// system yet, so a downed member simply stops acting.
	bool IsAlive() const { return health > 0.0f; }

	// --- items --------------------------------------------------------------
	// Backpack + the two held-item hand slots (see Inventory.h). A rune lands
	// here when dropped on this member's portrait / hand; memorizing one (the
	// hand context menu) consumes it. Empty for a fresh party.
	Inventory inventory;

	// The member's remembered DEFAULT USE, PER HAND (index 0 = left, 1 =
	// right) and per item type (catalog id → command id): picking a use from
	// a hand's right-click menu records it for THAT hand, and a left-click on
	// that hand holding that type executes it — so the left hand can be armed
	// with one spell and the right with another (Michael's rule, 2026-07-07).
	// Falls back to the type's first defaultable command when absent/stale
	// (GameUI resolves). Saved per slot+hand ("usedef" v16 lines; a pre-v16
	// flat line seeds BOTH hands).
	std::flat_map<std::string, std::string> useDefaults[2];

	// --- spells -------------------------------------------------------------
	// Spell symbols this member has committed to memory (bitmask of SymbolBit).
	// Empty at the start — a symbol is learned by memorizing a Rune tablet held
	// in a hand (the hand-slot Memorize action). Saved per slot (runtime state).
	u32 knownSymbols = 0;
	bool Knows(SpellSymbol s) const { return (knownSymbols & SymbolBit(s)) != 0; }
	void Learn(SpellSymbol s) { knownSymbols |= SymbolBit(s); }
	// Spells this member has LEARNED — earned the first time they successfully
	// CAST the recipe (built in the spellbook; a failed cast teaches nothing,
	// and higher-tier spells will fail without the skill for them). Only
	// learned spells appear in the hand menu's Magic quick-cast list / can be
	// armed as a hand default. Saved per slot ("learned" save lines).
	// (Transparent comparator so string_view ids can query without a copy.)
	std::flat_set<std::string, std::less<>> learnedSpells;
	bool HasLearnedSpell(std::string_view id) const {
		return learnedSpells.contains(id);
	}
	// Most-recently-CAST spells PER HAND (0 = left, 1 = right), front =
	// newest, deduplicated — each hand's Magic quick-cast list shows its own
	// first N (N = GameSettings::spellMruCount, the Controls → Hands
	// setting), so the two hands develop independent repertoires. A cast
	// credits the hand it was fired from (a hand-less cast — dev console —
	// touches neither). Bounded by the number of distinct spells ever cast,
	// so it stores whole and trims at display. Saved per slot+hand ("mru"
	// v16 lines; a pre-v16 flat line seeds BOTH hands).
	std::vector<std::string> spellMru[2];
	void TouchSpellMru(size_t hand, const std::string& id) {
		if (hand > 1) return;
		std::erase(spellMru[hand], id);
		spellMru[hand].insert(spellMru[hand].begin(), id);
	}
	// Mana points regenerated per second, scaled by intelligence. STUB: a real
	// "mana draw efficiency / capacity" stat will drive this later; intelligence
	// is the stand-in until that system is designed.
	float ManaRegenPerSec() const {
		return 0.4f + static_cast<float>(intelligence) * 0.08f;
	}

	// --- skills (docs/skills.md) ---------------------------------------------
	// Raw XP per skill id — the four school ids ("fire"...) plus the weapon
	// classes (items.cat `skill`; "unarmed" for bare hands). Levels derive:
	// floor(sqrt(xp)), so early levels come fast. XP is awarded by
	// DungeonWorld::GrantSkillXp (successful casts, landed blows), which also
	// drips the skill's associated stat forward through statProgress — the
	// per-stat creep pool that grants a stat point when it passes 1. Both
	// saved per slot (v15 "skill"/"statxp" lines).
	std::flat_map<std::string, float, std::less<>> skillXp;
	std::flat_map<std::string, float, std::less<>> statProgress;
	static int LevelForXp(float xp) {
		return xp <= 0.0f ? 0 : static_cast<int>(std::sqrt(xp));
	}
	float SkillXpOf(std::string_view id) const {
		const auto it = skillXp.find(id);
		return it == skillXp.end() ? 0.0f : it->second;
	}
	int SkillLevel(std::string_view id) const { return LevelForXp(SkillXpOf(id)); }

	// --- status effects (see the StatusEffect banner above) ------------------
	// The list holds only ACTIVE effects — expiry/spend removes the entry, so
	// presence IS the active check. Saved per slot ("effect" lines, v14; v13
	// "shield" lines load as the matching ward).
	std::vector<StatusEffect> effects;
	StatusEffect* FindEffect(StatusKind kind) {
		for (StatusEffect& e : effects)
			if (e.kind == kind) return &e;
		return nullptr;
	}
	const StatusEffect* FindEffect(StatusKind kind) const {
		for (const StatusEffect& e : effects)
			if (e.kind == kind) return &e;
		return nullptr;
	}
	void RemoveEffect(StatusKind kind) {
		std::erase_if(effects,
					  [kind](const StatusEffect& e) { return e.kind == kind; });
	}

	// --- ward queries (the Protect form rune, docs/spells.md "Protect") ------
	// Effects STACK across identities: a member may carry all four wards at
	// once — only recasting the SAME school replaces (the cast site removes
	// that school's ward before landing the new one). The school keys the
	// behaviour AND how the ward's magnitude reads: earth = +armor via Armor()
	// below, fire = melee attackers burn for it (both timed); water = an
	// absorb POOL it spends soaking damage (DungeonWorld::WoundMember), air =
	// deflect CHARGES it spends turning bolts aside
	// (ResolveMonsterProjectileHit). Each behaviour queries ITS school here.
	StatusEffect* FindWard(SpellSymbol school) {
		for (StatusEffect& e : effects)
			if (e.kind == StatusKind::Ward && e.school == school) return &e;
		return nullptr;
	}
	const StatusEffect* FindWard(SpellSymbol school) const {
		for (const StatusEffect& e : effects)
			if (e.kind == StatusKind::Ward && e.school == school) return &e;
		return nullptr;
	}
	void RemoveWard(SpellSymbol school) {
		std::erase_if(effects, [school](const StatusEffect& e) {
			return e.kind == StatusKind::Ward && e.school == school;
		});
	}
	bool HasShield(SpellSymbol school) const { return FindWard(school) != nullptr; }

	// --- combat (docs/combat.md "The attack formula") ------------------------
	// The damage/to-hit/pace numbers are assembled by DungeonWorld::PartyAttack
	// from the Balance knobs + the held weapon + these stat inputs — Character
	// only provides the stat side (it can't see ItemKind or Balance; layering).
	// Maximum carry weight (kg) before the member is encumbered. STUB formula —
	// strength-driven; an over-load penalty (slowed movement) is a later thread.
	float MaxCarryLoad() const { return static_cast<float>(strength) * 5.0f; }
	// Dodging is agility (settled: party evasion derives from DEX; monsters
	// stay authored). Type-agnostic — the first defense gate.
	float Evasion() const { return 0.05f + static_cast<float>(dexterity) * 0.015f; }

	// The RACE/NATURE defense layer (docs/combat.md part 4): per-damage-type
	// resists summed with equipment and wards. All zero for the default human
	// party; the proper race system arrives with party creation.
	ResistTable natureResists;

	// A stat's live value by its id ("strength", ... "intelligence"); 0 for an
	// unknown id (a catalog typo warns at parse, not here).
	int StatValue(std::string_view id) const {
		if (id == "strength") return strength;
		if (id == "dexterity") return dexterity;
		if (id == "vitality") return vitality;
		if (id == "willpower") return willpower;
		if (id == "intelligence") return intelligence;
		return 0;
	}
	// The associated-stat AVERAGE (docs/combat.md part 2) — the stat input to
	// the attack bonus. Empty list = 0 (an unclassed source gets no bonus).
	float StatAvg(std::span<const std::string> ids) const {
		if (ids.empty()) return 0.0f;
		float sum = 0.0f;
		for (const std::string& id : ids)
			sum += static_cast<float>(StatValue(id));
		return sum / static_cast<float>(ids.size());
	}

	// Re-derives the resource maxima from the bases + stats (the resource
	// formula: health ← VIT, stamina ← (STR+VIT)/2, mana ← (INT+WIL)/2; the
	// k's are Balance knobs). Growth carries the CURRENT value with it (a
	// stat point shows as growth, not damage); a shrunk max (a knob turned
	// down) clamps. Call after any stat change or balance apply.
	void RecomputeMaxima(float kHealth, float kStamina, float kMana) {
		const auto derive = [](float& current, float& max, float base, float stat) {
			const float grown = base + stat;
			current += grown > max ? grown - max : 0.0f;
			max = grown;
			if (current > max) current = max;
		};
		const bool down = health <= 0.0f; // growth must not revive a downed member
		derive(health, maxHealth, baseHealth,
			   kHealth * static_cast<float>(vitality));
		if (down) health = 0.0f;
		derive(stamina, maxStamina, baseStamina,
			   kStamina * 0.5f * static_cast<float>(strength + vitality));
		derive(mana, maxMana, baseMana,
			   kMana * 0.5f * static_cast<float>(intelligence + willpower));
	}

	// Movement-pace multiplier (1 = baseline, lower = slower). The party
	// moves at the pace of its slowest member: the Game feeds the roster
	// minimum into Party::SetSpeed, which scales step and turn rates.
	float moveSpeed = 1.0f;

	// Baked portrait (portrait_<name>.png), wired by the Game after the
	// texture loads; null draws the tinted-initial fallback instead.
	const gfx::Texture* portrait = nullptr;
	// Fallback portrait tint, also the slot's identity color.
	Vec4 portraitColor{0.3f, 0.3f, 0.3f, 1.0f};
};

// The default four-member starting party, fresh at full health.
std::vector<Character> CreateDefaultParty();

} // namespace dungeon::game
