// ============================================================================
// Game/Defense.h — the guard arithmetic, on its own so it can be MEASURED.
//
// The defender's half of docs/damage-system.md used to live entirely inside
// DungeonWorld: ArmorPenalty as a member, the two-hand rule inside
// PartyTarget::Evasion, the training rule inside TrainDefense. All three carry
// DECISIONS rather than mere sums — the floor training can never reach past, the
// hands combining by max and not sum, the two loops keying on opposite outcomes
// — and none of them could be tested, because reaching them meant linking the
// map, the monsters and the catalogs.
//
// So the arithmetic moved here and the world keeps only the ADAPTERS that gather
// its inputs (an inventory to a worn class, a weapon id to a skill level). This
// TU is deliberately pure — Combat.h for ArmorClass, Curve.h for the curves,
// nothing else — which is what lets tools/RollTest compile the SHIPPING code
// straight in rather than a copy of it. Keep it that way: a catalog lookup or a
// Character reference in here would put the wall back up.
//
// Deliberately NOT here: the parts that are lookups rather than decisions —
// WornArmorClass (walks equipment), Soak (sums armour), Resist (sums nature +
// worn + effects). Those have nothing to get wrong that a test would catch.
// ============================================================================
#pragma once

#include "Game/Combat.h" // ArmorClass
#include "Game/Curve.h"  // CurveRules, CurveValue

namespace dungeon::game::defense {

// What wearing armor costs its wearer on the defense roll, training included.
//
// THE FLOOR ENFORCES ITSELF. The skill offset is a CurveValue whose cap is
// `offsettable` (= penalty - floor), and a hyperbolic curve approaches its cap
// without ever reaching it — so "no matter how much you practise, plate still
// makes you easier to hit" is a property of maths already built and tested, not
// a clamp bolted on beside it. Pass the curve you mean; only a form that
// respects its cap keeps that guarantee (see the harness).
//
// A STRENGTH SHORTFALL IS PAID TWICE: points off the roll here, and a steeper
// stamina bill on every swing and step (SpendStamina). The same story told
// twice, which is the point — so this half must not quietly become the only one.
float ArmorPenalty(float floor, float offsettable, CurveRules offsetCurve,
				   float skillLevel, float strengthNeeded, float strength,
				   float shortPenalty);

// What the held-back share of skill is worth against a PHYSICAL blow, given what
// each hand can parry with (levels already resolved from the held weapon, or the
// `unarmed` level for an empty hand — bare-handed, but not nothing).
//
// THE HANDS COMBINE BY MAX, NOT SUM. Summing would make holding both hands back
// strictly better than one and turn the stance slider into a free defense
// button; the better hand answering the blow keeps it an honest trade.
//
// `held` MAY BE NEGATIVE (over-exertion, below), and the max rule is applied
// unchanged rather than switched on the sign: a negative held scaled by the
// BETTER hand is the largest penalty, which says the right thing — the more
// skilled the fighter, the further they can over-commit, and the wider open it
// leaves them. A sign-dependent rule would quietly hand the two-weapon fighter
// a discount on recklessness.
float HandGuard(float held, CurveRules skillCurve, float leftLevel,
				float rightLevel);

// THE STANCE'S ATTACK HALF (docs/damage-system.md "The stance"). The share
// scales the SKILL term of the attack bonus and nothing else — a stat is not
// skill, so DEX (or a school's stat) rides at full weight whatever the stance.
//
// This is what couples the two sides from ONE number: the points the share
// takes off the guard are the same points it puts behind the swing. Before it
// existed a character's slider only ever SUBTRACTED — pressing the attack cost
// you your guard and bought nothing — while a monster's `offense` already
// coupled both. Characters and monsters now trade on the same terms.
float StanceAttack(float share, float skillLevel, CurveRules skillCurve);

// OVER-EXERTION (docs/damage-system.md "Over-exertion"): the attack points
// bought by pushing the share PAST 1.0 — everything `StanceAttack` returns
// above what a full-committed-but-honest swing would. Zero at or below 1.0.
//
// This is the quantity the bill is charged against, so it is deliberately the
// same expression the attack roll used, not a re-derivation that could drift
// from it.
float ExertionPoints(float share, float skillLevel, CurveRules skillCurve);
//
// Deliberately NOT here: the SPLIT of the bill across stamina and health. It
// looks like pure arithmetic and is not — the bill is scaled by the wearer's
// armor shortfall on its way out of the bar, so a split computed against the raw
// points would lose that scaling's excess instead of passing it on to health.
// It therefore lives at the one place that already owns the scaling
// (DungeonWorld::SpendStamina, which reports its shortfall). A pure copy here
// would be a second rule that agrees with the shipping one only by luck.

// THE ATTACKER'S HALF OF THE TYPE AXIS (docs/damage-system.md "Two axes"), the
// mirror of the defender's resists: how much harder — or, negative, more feebly —
// an attacker strikes with a given damage type. `clamp` bounds it BOTH ways.
//
// Deliberately WITHOUT the two escapes a resist has. A resist of 1.0 means immunity
// and past it absorption, because those say what a thing IS; "I deal 150% fire" is
// stacking, not identity, so there is nothing here to exempt from the clamp. And it
// never returns less than zero: a feeble blow does nothing, it does not heal —
// healing is the ABSORB stage's business and must not be reachable from the attack
// side.
//
// Lives here rather than on Balance because Balance drags the catalog and file
// layers with it, which tools/RollTest cannot link — the same reason the armor floor
// lives here, with Balance::Potent as the thin adapter that passes its knob.
float Potent(float amount, const ResistTable& potency, DamageType type,
			 float clamp);

// What the damage-type BOOK says about an incoming type: the two flags, passed
// in rather than looked up. DamageTypes.cpp includes Game/Catalog.h, and linking
// it into tools/RollTest would drag the catalog layer through the purity wall
// that harness deliberately keeps up — so the world asks the book and hands the
// answers here, and what stays testable is the DECISION made from them.
struct TypeFacts {
	bool hasSchool = false; // DamageTypeBook::SchoolOf found a school
	bool physical = false;  // DamageTypeBook::IsPhysical
};

// How a defender answers an incoming blow.
enum class GuardKind : u8 {
	Physical, // parried with a HAND, off its weapon class
	Magical,  // warded with the skill in the INCOMING SCHOOL; hands play no part
	Neither,  // no school and not physical: nothing parries it at all
};

// A SCHOOL WINS OVER PHYSICAL. Worth pinning rather than leaving to the order two
// ifs happen to sit in: nothing stops a project marking a school's type physical
// as well, and the two branches guard with completely different skills.
GuardKind GuardKindFor(TypeFacts facts);

// Everything the defender's side of the roll needs, resolved from the world by
// PartyTarget::Evasion — which is now only that resolution, so the arithmetic and
// the two rules below can be measured (docs/damage-system.md).
struct GuardInputs {
	float base = 0.0f;    // Balance::defenseBase, the innate floor
	float dexterity = 0.0f;
	CurveRules statCurve;

	// AVOID IS UNARMORED-ONLY, which is what makes going bare a BUILD rather than
	// the poor man's option — and it also settles a design problem: light and
	// medium armor creep DEX and so does avoid, and since you cannot practise
	// both, exactly one loop runs at a time (see LessonFrom).
	ArmorClass worn = ArmorClass::None;
	float armorPenalty = 0.0f; // from ArmorPenalty(); ignored when unarmored
	float avoidLevel = 0.0f;   // the `avoid` skill; ignored when ARMORED
	CurveRules avoidCurve;

	// The stance's held-back share (1 - offenseShare) and what it can guard with.
	// NEGATIVE when the wearer is over-exerting (share > 1): the guard becomes a
	// PENALTY rather than merely nothing, because a fighter spending past
	// everything they have is not just failing to defend — they are wide open.
	float held = 0.0f;
	CurveRules skillCurve;
	GuardKind kind = GuardKind::Physical;
	float schoolLevel = 0.0f;             // the INCOMING school's skill
	float leftLevel = 0.0f, rightLevel = 0.0f; // each hand's parry skill
};

// The defender's total, in d100 points.
float Guard(const GuardInputs& in);

// What a resolved defensive event teaches its defender.
//
// THE TWO LOOPS TRAIN ON OPPOSITE OUTCOMES, so no single blow can train both: a
// miss while unarmored teaches `avoid`, a landed blow that armor actually
// blunted teaches that armor class. You learn what you actually survive. Only
// ROLLED events count — a bump, a fall and a poison tick were never evaded and
// never turned, so they teach nothing.
enum class Lesson : u8 {
	Nothing, // rolled nothing, or the outcome teaches neither loop
	Avoid,   // a miss while wearing nothing
	Armor,   // a landed blow the worn class blunted
};
Lesson LessonFrom(bool rolled, bool hit, ArmorClass worn, float soak);

} // namespace dungeon::game::defense
