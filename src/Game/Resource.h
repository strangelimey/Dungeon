// ============================================================================
// Game/Resource.h — how big a pool is, and how fast it comes back.
//
// docs/health-and-healing.md is the model. Each of the three resources has an
// APTITUDE (a stat — what you are) and a PRACTICE (a skill — what you have
// done), in the relationship the attack formula already uses:
//
//   max  = base  + per_aptitude x aptitude + Curve_max(skill)
//   /sec = regen + regen_apt x Curve(aptitude) + regen_max x max
//                + Curve_regen(skill)
//
// The aptitude enters MAX linearly and REGEN through the stat curve. That is
// deliberate and not an oversight: a pool is a capacity, and capacity should
// scale with the body it belongs to, while a RATE is a performance and every
// other performance in this game tapers. Both skill terms taper, because a
// skill here never stops rising (level = sqrt(xp)) and an untapered term would
// eventually swamp the stat it is meant to shade.
//
// `regen_max` is the odd one out and it is the one term that PRE-DATES this
// model: stamina has always regenerated partly in proportion to its own pool
// (`stamina_regen_max`). It is kept and generalised to all three rather than
// folded away, because "a bigger pool refills proportionally" is a real and
// separate statement from "a fitter body refills faster" — the first is about
// the container, the second about the owner. The other two default it to zero,
// so nothing changes for them until somebody authors it.
//
// Pure by design — Curve.h and nothing else — so tools/RollTest links the
// SHIPPING arithmetic rather than a copy of it. Keep it that way: a Character
// reference or a Balance lookup in here puts the wall back up. The adapters
// that gather the inputs (a member's skill level, a knob sheet's floats) stay
// with the things that own them: Balance::Resource and Character::RecomputeMaxima.
//
// WHAT IS ACTUALLY WORTH TESTING HERE — and it is not the addition. A skill
// term is a CurveValue, and CurveValue treats a non-positive cap as "no
// meaningful shape, use the straight line the slope describes". For the general
// curve that is the right reading. For a resource it is a disaster: an
// unauthored or zeroed `<r>_skill_cap` would make constitution add health
// LINEARLY AND FOREVER, which is precisely the shape of the armor-floor bug
// (docs/damage-system.md — a cap of zero turned armor into a huge evasion
// bonus). So a cap of zero means THIS SKILL CONTRIBUTES NOTHING here, and that
// rule lives in one place — SkillTerm — rather than at each of the six sites
// that would otherwise have to remember it.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Game/Curve.h" // CurveRules, CurveValue

namespace dungeon::game::resource {

// The three pools, in the order the HUD bars draw them. Used to index the knob
// sheet (Balance::Resource) — the ArmorClass idiom.
enum class Kind : u8 { Health, Stamina, Mana, Count };

// THE PRACTICE THAT FEEDS EACH POOL (docs/health-and-healing.md). Ordinary
// skill ids, so they ride Character::skillXp and its v15 save lines with no new
// storage and no version bump — the whole reason "constitution" came out as a
// SKILL rather than the sixth stat it is in every other game.
//
// THEY TRAIN AND THEY CREEP NOTHING, which is the one rule this system exists
// to protect. Every other skill in the game drips its associated stat forward
// through DungeonWorld::GrantSkillXp; route these three the same way and the
// runaway walks straight back in — stamina spent would feed conditioning AND
// vitality, and vitality feeds max stamina. Award them with an empty stat list.
inline constexpr const char* kConditioning = "conditioning"; // stamina
inline constexpr const char* kAttunement = "attunement";     // mana
inline constexpr const char* kConstitution = "constitution"; // health

// The skill id that practices `kind` ("" for an out-of-range value).
const char* SkillId(Kind kind);

// One resource's knobs, gathered so the two formulas read as one lookup rather
// than seven loose floats at the call site (the Balance::ArmorRules pattern).
// Assembled by Balance::Resource from the flat balance.cat knobs.
struct Rules {
	// MAX. `perAptitude` is the long-standing k_<r>: points of maximum per
	// point of the driving stat, linear. `skillMax`'s slope and cap are in
	// POINTS OF MAXIMUM — the cap is the most the practice can ever add, so it
	// is a number to balance around rather than a scale factor needing a second
	// knob beside it.
	float perAptitude = 1.0f;
	CurveRules skillMax{};

	// REGEN, all in points per second. `regenBase` is what anyone recovers;
	// `regenPerAptitude` is per point of the STAT curve's output (so an average
	// stat is worth nothing and a poor one is a real penalty — the curve's
	// baseline does that); `regenPerMax` is per point of the pool's own
	// ceiling; `skillRegen`'s slope and cap are again in the unit being
	// produced, here points per second.
	float regenBase = 0.0f;
	float regenPerAptitude = 0.0f;
	float regenPerMax = 0.0f;
	CurveRules skillRegen{};
};

// --- supplies (docs/health-and-healing.md "Food and water") -----------------
// The two meters that make rest cost something, and therefore make every rate
// above matter. They are NOT pools: nothing regenerates them, they only ever
// fall, and they are refilled by an ITEM. So they get their own small shape
// rather than being bent into Rules.
enum class Supply : u8 { Food, Water, Count };

// One meter's knobs. `max` is a flat knob and not a derived maximum on purpose
// — the size of a stomach is not an attribute, and making it one would put two
// more fields in every save for a number nobody would tune per character.
struct SupplyRules {
	float max = 100.0f;
	float perSecond = 0.0f; // drained by time passing
	// CONDITIONING'S PRICE, in extra units per second, tapering to its cap. This
	// is the brake on the whole design: every other loop compounds upward, and
	// this is the one that taxes the compounding — the fitter member burns more
	// food and water, so training is not free.
	CurveRules condDrain{};
	float perExertion = 0.0f;  // drained per point of stamina SPENT
	float starveDamage = 0.0f; // health per second once the meter is empty
};

// Units per second at conditioning level `practice`, before exertion. Never
// negative: a meter that filled itself by standing still would undo the point.
float DrainPerSec(const SupplyRules& rules, float practice);

// What consuming something actually RESTORED — not what it was worth. A full
// member gains nothing from an apple, and the caller needs to know that to
// refuse the action and keep the item rather than eat it for no effect.
//
// It lives here, in the pure header, rather than nested in DungeonWorld: the
// UI raises the action and the world answers it, so the type sits on a seam
// between them and belongs to neither.
struct Refill {
	float food = 0.0f, water = 0.0f;
	bool Any() const { return food > 0.0f || water > 0.0f; }
};

// All three pools' knobs together. Bundled because they are always needed
// together: one stat point moves more than one pool, so RecomputeMaxima can
// never usefully be handed just one of these — and a single argument keeps the
// dozen call sites from each spelling out the same three lookups.
struct PoolRules {
	Rules health, stamina, mana;
	const Rules& For(Kind kind) const;
};

// What a skill term contributes, with the zero-cap rule above applied. A cap of
// zero (or less) means the practice does not feed this resource at all — NOT a
// straight line rising without limit.
float SkillTerm(const CurveRules& rules, float skillLevel);

// EVERYTHING THE MAXIMUM GETS FROM THE CHARACTER — aptitude plus practice, and
// no base. Exposed separately because the save loader has to run the formula
// BACKWARDS: a pre-v17 save stored maxima and no bases, and recovers each base
// as `savedMax - Contribution(...)`. Doing that by subtracting a hand-written
// copy of the stat term is how the two would drift the day a term is added —
// which is exactly what happened here, since that back-solve was written when
// the aptitude was the only term there was.
float Contribution(const Rules& rules, float aptitude, float skillLevel);

// The pool's ceiling. `base` is the member's authored value (class identity,
// saved since v17) and is not a knob, so it is an argument rather than a field.
// Never negative: a savagely negative aptitude empties the pool, it does not
// invert it.
float Maximum(const Rules& rules, float base, float aptitude, float skillLevel);

// Points per second at full flow — before the state gate (exerting/idle/
// resting) that DungeonWorld's regen tick applies, and before the "only while
// below maximum" rule that makes the constitution skill unfarmable. Never
// negative, for the same reason Maximum is not.
float RegenPerSec(const Rules& rules, const CurveRules& statCurve,
				  float aptitude, float maximum, float skillLevel);

} // namespace dungeon::game::resource
