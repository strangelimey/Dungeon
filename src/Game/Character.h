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

#include <string>
#include <vector>

namespace dungeon::gfx {
class Texture;
}

namespace dungeon::game {

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

	// --- spells -------------------------------------------------------------
	// Spell symbols this member has committed to memory (bitmask of SymbolBit).
	// Empty at the start — a symbol is learned by memorizing a Rune tablet held
	// in a hand (the hand-slot Memorize action). Saved per slot (runtime state).
	u32 knownSymbols = 0;
	bool Knows(SpellSymbol s) const { return (knownSymbols & SymbolBit(s)) != 0; }
	void Learn(SpellSymbol s) { knownSymbols |= SymbolBit(s); }
	// Mana points regenerated per second, scaled by intelligence. STUB: a real
	// "mana draw efficiency / capacity" stat will drive this later; intelligence
	// is the stand-in until that system is designed.
	float ManaRegenPerSec() const {
		return 0.4f + static_cast<float>(intelligence) * 0.08f;
	}

	// --- combat (derived from attributes; unarmed baseline) -----------------
	// Weapons/spells will scale these later. Tuned so the class spreads read
	// distinctly: the fighter hits hard, the rogue lands often and dodges,
	// the mage is fragile in melee.
	float AttackDamage() const { return 4.0f + static_cast<float>(strength) * 0.5f; }
	float Accuracy() const { return 0.55f + static_cast<float>(dexterity) * 0.02f; }
	float Evasion() const { return 0.05f + static_cast<float>(dexterity) * 0.015f; }
	float Armor() const { return 0.0f; } // no equipment yet
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
