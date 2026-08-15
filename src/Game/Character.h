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
#include "Game/Effect/Effect.h" // fx::Inst (the status-effect list)
#include "Game/Inventory.h"
#include "Game/Resource.h" // the aptitude/practice pool formulas
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
// (Character::effects): the Protect wards, poison and bleed, the Sight mark,
// and whatever comes next. An entry is an fx::Inst (Effect/Effect.h) — a POD
// pointing at the shared kind that carries the behaviour. The HUD's portrait
// effect strip and the character sheet read the list; behaviour code queries
// it by kind (the ward helpers below). The world tick ages timeLeft and
// removes an expired effect with its kind's fade message; spend-to-die effects
// (the water pool, the air charges) are removed at their SPEND site instead,
// so their burst/still lines replace the fade line.
//
// A MONSTER will carry the same list (docs/effects.md P3) — that symmetry is
// the point of the effects system, and why the type lives in its own module
// instead of here.
// ============================================================================

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

	// SUPPLIES (docs/health-and-healing.md). Not pools: nothing regenerates
	// them, they only fall, and only an item refills them. Their maximum is a
	// flat balance knob rather than a field here — the size of a stomach is not
	// an attribute — so only the current level rides the save (v25).
	//
	// PER CHARACTER, and deliberately so: Michael plans to split the party for
	// sub-quests, and a shared food pool would read as a harmless simplification
	// today and be the expensive thing to unpick the day someone walks off alone.
	//
	// The MAXIMA are a flat balance knob (the size of a stomach is not an
	// attribute), mirrored here as DERIVED fields — unsaved, refreshed by
	// RecomputePartyMaxima — so that a reader with no Balance in reach can still
	// draw the bar. Exactly what maxHealth is to its own knobs.
	float food = 100.0f, water = 100.0f;
	float maxFood = 100.0f, maxWater = 100.0f;

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

	// THE OFFENSE SHARE (docs/damage-system.md): how much of this character's
	// skill goes into ATTACKING. 1.0 spends it all and guards with nothing;
	// 0.4 puts 40% behind every swing and holds 60% back to defend with. It is
	// ONE stance for the character, not one per hand. Against a PHYSICAL blow
	// the two hands still guard differently — each parries off its own weapon
	// class and the better of them answers — but a MAGICAL one is warded with
	// the skill in ITS OWN school (knowing fire is what turns fire aside), and
	// the hands play no part in that at all. It applies while a hand is on COOLDOWN
	// too: a stance is not an action, so recovering from a swing does not drop
	// your guard.
	//
	// NOT clamped to 1: over-exertion (spending past 100% by burning stamina,
	// health or a stat) pushes this ABOVE 1, so nothing may assume the offense
	// and defense shares sum to the skill.
	float offenseShare = 1.0f;

	// Hit-feedback splat shown over this member's portrait when a monster lands
	// a blow: hitFlash counts down (seconds) while the party bar draws the icon,
	// and hitSeverity picks which one (0 = small, 1 = medium, 2 = hard). The
	// severity thresholds are placeholder — "what a hit means" is TBD. A fresh
	// party (CreateDefaultParty) clears these.
	float hitFlash = 0.0f;
	int hitSeverity = 0;

	// Stamina exertion state (docs/combat.md Phase 4), live transients like
	// the hand cooldowns (not saved; a load re-derives `exhausted` from the
	// saved stamina). staminaHoldoff blocks regen for a beat after any spend
	// so sustained marching genuinely drains; `exhausted` latches when the
	// bar empties (weaker, slower swings) and clears with hysteresis once it
	// climbs back past the exhaust_recover knob.
	float staminaHoldoff = 0.0f;
	bool exhausted = false;

	// Whether the member is still standing (health > 0): acting, targetable,
	// regenerating. At 0 they are DOWN — unconscious, or dead if the `dead`
	// flag is set (docs/combat.md Phase 5). An unconscious member self-
	// stabilizes: `stabilize` accrues seconds while no monster is in aggro of
	// the party (any danger resets it, live transient) and at the
	// stabilize_time knob they wake at stabilize_health of max. Death takes
	// deliberate OVERKILL (one blow ≥ overkill × maxHealth, or a hit landing
	// on a member already at 0); the dead never wake — a future resurrection
	// mechanic is the only way back. `dead` rides the save ("dead" line,
	// v18); the timer does not.
	bool IsAlive() const { return health > 0.0f; }
	bool dead = false;
	float stabilize = 0.0f;

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
	// --- the three pools (docs/health-and-healing.md) -------------------------
	// THE APTITUDE MAPPING LIVES HERE AND NOWHERE ELSE. Which stat drives which
	// pool is a fact about a character, not about the knob sheet, and it is
	// needed by both formulas — so the maximum and the regen rate both ask this
	// rather than each spelling out `0.5 * (strength + vitality)` and drifting
	// apart the day a race or a class shades one of them.
	float Aptitude(resource::Kind kind) const {
		switch (kind) {
		case resource::Kind::Health:
			return static_cast<float>(vitality);
		case resource::Kind::Stamina:
			return 0.5f * static_cast<float>(strength + vitality);
		case resource::Kind::Mana:
			return 0.5f * static_cast<float>(intelligence + willpower);
		default:
			return 0.0f;
		}
	}
	// This member's live level in the practice that feeds `kind`.
	float PracticeLevel(resource::Kind kind) const {
		return static_cast<float>(SkillLevel(resource::SkillId(kind)));
	}
	// A supply meter by id, so the two can be walked in a loop instead of every
	// site writing the food case and then the water case beside it.
	float& SupplyLevel(resource::Supply which) {
		return which == resource::Supply::Water ? water : food;
	}
	float SupplyLevel(resource::Supply which) const {
		return which == resource::Supply::Water ? water : food;
	}
	// The pool's live maximum, for whoever needs it before RecomputeMaxima has
	// stored it (the regen rate's per-max term wants the CURRENT ceiling).
	float ResourceMax(resource::Kind kind) const {
		switch (kind) {
		case resource::Kind::Health: return maxHealth;
		case resource::Kind::Stamina: return maxStamina;
		case resource::Kind::Mana: return maxMana;
		default: return 0.0f;
		}
	}
	// Points per second at FULL FLOW. The caller still owns the state gate
	// (exerting / idle / resting) and the "only while below maximum" rule —
	// this answers only "how fast does this body recover", which is the part
	// that depends on the character rather than on the situation.
	float RegenPerSec(resource::Kind kind, const resource::Rules& rules,
					  const CurveRules& statCurve) const {
		return resource::RegenPerSec(rules, statCurve, Aptitude(kind),
									 ResourceMax(kind), PracticeLevel(kind));
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

	// --- status effects (see the banner above) --------------------------------
	// The list holds only ACTIVE effects — expiry/spend removes the entry, so
	// presence IS the active check. Landing one goes through fx::Apply, which
	// owns the stacking rule. Saved per slot ("effect" lines, v14; v13 "shield"
	// lines load as the matching ward).
	std::vector<fx::Inst> effects;
	fx::Inst* FindEffect(std::string_view id) {
		for (fx::Inst& e : effects)
			if (e.Is(id)) return &e;
		return nullptr;
	}
	const fx::Inst* FindEffect(std::string_view id) const {
		for (const fx::Inst& e : effects)
			if (e.Is(id)) return &e;
		return nullptr;
	}
	void RemoveEffect(std::string_view id) {
		std::erase_if(effects, [id](const fx::Inst& e) { return e.Is(id); });
	}
	// Remove only the entry of `id` carrying `school` — the school-keyed stack
	// semantics (a same-school recast replaces just its own, like the sights).
	void RemoveEffect(std::string_view id, SpellSymbol school) {
		std::erase_if(effects, [id, school](const fx::Inst& e) {
			return e.Is(id) && e.school == school;
		});
	}

	// --- ward queries (the Protect form rune, docs/spells.md "Protect") ------
	// Wards STACK across schools: a member may carry all four at once, because
	// each school's ward is its OWN kind and a kind only refreshes itself.
	// Their BEHAVIOUR lives in those kinds now (Game/Effect/WardEffect.cpp) —
	// each hooks the pipeline stage it acts at, and nothing outside asks a
	// ward to do anything. These queries remain for the UI and for tests:
	// "is this member warded?" is still a fair question to ask.
	fx::Inst* FindWard(SpellSymbol school) {
		for (fx::Inst& e : effects)
			if (e.IsWard() && e.school == school) return &e;
		return nullptr;
	}
	const fx::Inst* FindWard(SpellSymbol school) const {
		for (const fx::Inst& e : effects)
			if (e.IsWard() && e.school == school) return &e;
		return nullptr;
	}
	void RemoveWard(SpellSymbol school) {
		std::erase_if(effects, [school](const fx::Inst& e) {
			return e.IsWard() && e.school == school;
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

	// Re-derives the resource maxima from the authored bases, the APTITUDES
	// (Aptitude above) and the PRACTICES — the aptitude/practice model in
	// docs/health-and-healing.md, whose arithmetic lives in the pure
	// Game/Resource.h so it can be measured. Call after any stat change, any
	// resource-skill gain, or a balance apply.
	//
	// GROWTH CARRIES THE CURRENT VALUE with it, so a vitality point or a
	// constitution level reads as growth rather than as damage; a SHRUNK max (a
	// knob turned down in the editor) clamps instead. And growth must never
	// revive a downed member — a bigger pool is not a resurrection, so 0 health
	// is restored after the derive.
	void RecomputeMaxima(const resource::PoolRules& rules) {
		const auto derive = [](float& current, float& max, float grown) {
			current += grown > max ? grown - max : 0.0f;
			max = grown;
			if (current > max) current = max;
		};
		const auto grown = [&](resource::Kind kind, float base) {
			return resource::Maximum(rules.For(kind), base, Aptitude(kind),
									 PracticeLevel(kind));
		};
		const bool down = health <= 0.0f;
		derive(health, maxHealth, grown(resource::Kind::Health, baseHealth));
		if (down) health = 0.0f;
		derive(stamina, maxStamina, grown(resource::Kind::Stamina, baseStamina));
		derive(mana, maxMana, grown(resource::Kind::Mana, baseMana));
	}

	// AUTHORED movement pace (1 = baseline, lower = slower) — class identity,
	// like baseHealth. Sera is fleet-footed at 1.2, Tilo the anchor at 0.9.
	// Read MoveSpeed() rather than this: conditioning adds to it.
	float moveSpeed = 1.0f;
	// The pace this member actually walks at: the authored base plus what
	// CONDITIONING has added (docs/health-and-healing.md "Movement").
	//
	// The party moves at the pace of its SLOWEST member, so the benefit is
	// invisible until the worst-trained one has it — one unconditioned mage
	// still caps the whole party. That rule now has teeth it did not have
	// before, because conditioning makes members genuinely diverge.
	float MoveSpeed(const CurveRules& paceCurve) const {
		return moveSpeed +
			   resource::SkillTerm(paceCurve, PracticeLevel(resource::Kind::Stamina));
	}

	// Baked portrait (portrait_<name>.png), wired by the Game after the
	// texture loads; null draws the tinted-initial fallback instead.
	const gfx::Texture* portrait = nullptr;
	// Fallback portrait tint, also the slot's identity color.
	Vec4 portraitColor{0.3f, 0.3f, 0.3f, 1.0f};
};

// The default four-member starting party, fresh at full health.
std::vector<Character> CreateDefaultParty();

} // namespace dungeon::game
