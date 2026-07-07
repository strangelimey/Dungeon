// ============================================================================
// Game/Character.h — party member data.
//
// One Character per party slot (up to four). The numbers feed the HUD party
// bar and the character sheet (see PartyHud.h) and now drive melee combat:
// the derived-stat accessors below (AttackDamage/Accuracy/Evasion/...) turn
// the attributes into an AttackProfile/DefenseProfile (Combat.h) for the
// strike resolver, and `health` actually drains when a monster lands a blow.
// Portraits are baked by AssetBaker (portrait_<name>.png); the tinted square
// stamped with the character's initial remains as the fallback when the
// texture is missing.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/Inventory.h"
#include "Game/Spells.h"

#include <flat_map>
#include <flat_set>
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

	float health = 1.0f, maxHealth = 1.0f;
	float stamina = 1.0f, maxStamina = 1.0f;
	float mana = 1.0f, maxMana = 1.0f;

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

	// The member's remembered DEFAULT USE per item type (catalog id → command
	// id): picking a use from a hand's right-click menu records it here, and a
	// left-click on a hand holding that type executes it. Falls back to the
	// type's first defaultable command when absent/stale (GameUI resolves).
	// Saved per slot (a member's weapon habits survive a reload).
	std::flat_map<std::string, std::string> useDefaults;

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
	// Most-recently-CAST spells, front = newest, deduplicated — the hand
	// menu's Magic quick-cast list shows the first N (N = GameSettings::
	// spellMruCount, the Controls → Hands setting). Bounded by the number of
	// distinct spells ever cast, so it stores whole and trims at display.
	// Saved per slot ("mru" save lines, order preserved).
	std::vector<std::string> spellMru;
	void TouchSpellMru(const std::string& id) {
		std::erase(spellMru, id);
		spellMru.insert(spellMru.begin(), id);
	}
	// Mana points regenerated per second, scaled by intelligence. STUB: a real
	// "mana draw efficiency / capacity" stat will drive this later; intelligence
	// is the stand-in until that system is designed.
	float ManaRegenPerSec() const {
		return 0.4f + static_cast<float>(intelligence) * 0.08f;
	}

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
	// ONE active ward per member — casting another replaces it (a rule the cast
	// site enforces, not a storage limit). The school keys the behaviour AND
	// how the ward's magnitude reads: earth = +armor via Armor() below, fire =
	// melee attackers burn for it (both timed); water = an absorb POOL it
	// spends soaking damage (DungeonWorld::WoundMember), air = deflect CHARGES
	// it spends turning bolts aside (ResolveMonsterProjectileHit).
	StatusEffect* Ward() { return FindEffect(StatusKind::Ward); }
	const StatusEffect* Ward() const { return FindEffect(StatusKind::Ward); }
	bool ShieldActive() const { return Ward() != nullptr; }
	bool HasShield(SpellSymbol school) const {
		const StatusEffect* w = Ward();
		return w && w->school == school;
	}

	// --- combat (derived from attributes; unarmed baseline) -----------------
	// Weapons/spells will scale these later. Tuned so the class spreads read
	// distinctly: the fighter hits hard, the rogue lands often and dodges,
	// the mage is fragile in melee.
	float AttackDamage() const { return 4.0f + static_cast<float>(strength) * 0.5f; }
	// Maximum carry weight (kg) before the member is encumbered. STUB formula —
	// strength-driven; an over-load penalty (slowed movement) is a later thread.
	float MaxCarryLoad() const { return static_cast<float>(strength) * 5.0f; }
	float Accuracy() const { return 0.55f + static_cast<float>(dexterity) * 0.02f; }
	float Evasion() const { return 0.05f + static_cast<float>(dexterity) * 0.015f; }
	// No equipment yet — an earth ward (Stone Skin) is the one armor source.
	float Armor() const {
		const StatusEffect* w = Ward();
		return w && w->school == SpellSymbol::Earth ? w->magnitude : 0.0f;
	}
	// Seconds between swings for the given hand (0 = left, 1 = right). STUB: the
	// real interval is computed from several inputs — the weapon held in that
	// hand (its attack speed), an off-hand / two-handed penalty, and the
	// member's stats — but with no inventory yet it only reflects dexterity and
	// ignores `hand` (both hands swing alike). Higher dexterity swings faster.
	// Fold the hand's weapon speed in here once items exist.
	float AttackInterval(size_t hand) const {
		(void)hand; // per-hand weapon speed plugs in here once items exist
		const float t = 1.8f - static_cast<float>(dexterity) * 0.04f;
		return t < 0.6f ? 0.6f : (t > 2.0f ? 2.0f : t);
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
