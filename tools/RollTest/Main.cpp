// ============================================================================
// tools/RollTest — the dice, measured rather than assumed.
//
// Game/Roll.cpp is compiled straight into this harness (the Bc7Test pattern),
// so what is checked is the SHIPPING engine and not a copy that can drift from
// it. Every expectation below is derived analytically and stated beside the
// measurement, because the whole point is to catch an engine that is subtly
// wrong — a fumble rate of 5% when the design said 5% is worth nothing if the
// number was read off the same constant the engine used.
//
// WHY THE TAIL MATTERS HERE. The design multiplies a landed hit by the margin
// (attack total - defense total), and the rolls are open-ended, so a lucky
// swing widens the margin AND the margin multiplies the damage. Those two
// compound. The mean says nothing useful about that; the percentiles do. The
// tail section is INFORMATIONAL — it exists so the balance numbers are chosen
// against measured behaviour instead of intuition.
//
// THE ARMOR AND STANCE SECTION covers the DEFENDER's half, which nothing
// covered until 2026-08-11: the penalty floor training can never reach past, the
// two hands combining by MAX and never by sum, the two training loops keying on
// opposite outcomes, `avoid` applying only while unarmored, and TYPED defense
// (physical -> the hands, magical -> the incoming school with the hands playing
// no part, neither -> nothing). Its arithmetic was lifted out of DungeonWorld
// into Game/Defense.h for exactly this reason — so the shipping rules could be
// linked without the map and the catalogs coming with them. What stayed in the
// world is only RESOLUTION: an inventory to a worn class, a damage type to its
// two flags, a skill id to a level. Those are lookups with nothing a test would
// catch; every DECISION is measured here.
//
//   RollTest.exe [--self-test]   — one verdict line, exit 0 = PASS
//
// --self-test feeds the checks a 90-sided die while they still expect 100, so
// a harness that cannot catch a broken distribution FAILS instead of passing
// vacuously. It ALSO swaps the armor offset curve for the logarithmic form,
// which passes its cap — because a self-test that only breaks the dice would
// leave every armor check below unproven.
// ============================================================================
#include "Game/Blast.h"
#include "Game/Combat.h"
#include "Game/Curve.h"
#include "Game/DamageLedger.h"
#include "Game/Defense.h"
#include "Game/Mishap.h"
#include "Game/PartyRules.h"
#include "Game/Power.h"
#include "Game/Style.h"
#include "Game/Carve.h"
#include "Game/Generate.h"
#include "Game/LightProfile.h"
#include "Game/Trail.h"
#include "Game/Resource.h"
#include "Game/Roll.h"
#include "Graphics/Camera.h"
#include "Graphics/LightTiles.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dungeon::game;

namespace {

int g_checks = 0;
int g_failed = 0;

// One expectation. `expected` is derived, never read from the engine's own
// constants; `tol` is set generously wide against the sample count (every
// tolerance below is >= 7 sigma) so a PASS means "right", not "lucky".
void Check(const char* what, double measured, double expected, double tol) {
	++g_checks;
	const bool ok = std::fabs(measured - expected) <= tol;
	if (!ok) ++g_failed;
	std::printf("  %-46s %10.4f  expect %9.4f +/- %-7.4f %s\n", what, measured,
				expected, tol, ok ? "ok" : "FAIL");
}

void CheckTrue(const char* what, bool ok) {
	++g_checks;
	if (!ok) ++g_failed;
	std::printf("  %-46s %10s %-28s %s\n", what, ok ? "true" : "false", "",
				ok ? "ok" : "FAIL");
}

double Pct(const std::vector<int>& sorted, double p) {
	if (sorted.empty()) return 0.0;
	const size_t i = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
	return sorted[i];
}

} // namespace

int main(int argc, char** argv) {
	bool selfTest = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;

	constexpr int kSamples = 2'000'000;

	RollRules rules;
	if (selfTest) {
		// THE INJECTED FAULT: a 90-sided die. Every expectation below still
		// assumes 100, so the distribution checks must trip.
		rules.sides = 90;
	}

	std::printf("RollTest — %d samples, seed 1234%s\n\n", kSamples,
				selfTest ? "  [SELF-TEST: 90-sided die injected]" : "");

	// --- the plain die: no escalation, no fumble ----------------------------
	// Establishes the underlying uniform is actually uniform before anything
	// built on top of it is trusted.
	{
		std::mt19937 rng(1234);
		RollRules flat = rules;
		flat.critThreshold = 1000; // unreachable
		flat.fumbleThreshold = 0;  // unreachable

		double sum = 0;
		int deciles[10] = {};
		for (int i = 0; i < kSamples; ++i) {
			const Roll r = RollOpenEnded(flat, rng);
			sum += r.total;
			const int d = std::min(9, (r.first - 1) * 10 / 100);
			++deciles[d];
		}
		std::printf("the plain die\n");
		Check("mean", sum / kSamples, 50.5, 0.15);
		double worst = 0;
		for (int d = 0; d < 10; ++d)
			worst = std::max(worst, std::fabs(deciles[d] / double(kSamples) - 0.10));
		Check("worst decile deviation from 0.1000", worst, 0.0, 0.004);
	}

	// --- the open-ended die -------------------------------------------------
	// Expectations, all derived from p(trigger) = 6/100 and p(fumble) = 5/100:
	//   crit rate       = 0.06
	//   fumble rate     = 0.05
	//   P(>= 2 escal.)  = 0.06^2 = 0.0036
	//   mean total      = 50.5 / (1 - 0.06) = 53.7234...
	// The mean is the one worth stating: an open-ended die is not "50.5 plus a
	// bit", it is a geometric series, and getting it wrong by a point moves
	// every hit rate in the game.
	std::vector<int> totals;
	{
		std::mt19937 rng(1234);
		totals.reserve(kSamples);
		double sum = 0;
		long long crits = 0, fumbles = 0, deep = 0, capped = 0, fumbleAfterEsc = 0;
		for (int i = 0; i < kSamples; ++i) {
			const Roll r = RollOpenEnded(rules, rng);
			totals.push_back(r.total);
			sum += r.total;
			crits += r.crit;
			fumbles += r.fumble;
			deep += (r.escalations >= 2);
			capped += r.capped;
			fumbleAfterEsc += (r.fumble && r.escalations > 0);
		}
		std::printf("\nthe open-ended die\n");
		Check("crit rate (first >= 95)", double(crits) / kSamples, 0.06, 0.002);
		Check("fumble rate (first <= 5)", double(fumbles) / kSamples, 0.05, 0.002);
		Check("P(2+ escalations)", double(deep) / kSamples, 0.0036, 0.0006);
		Check("mean total", sum / kSamples, 50.5 / (1.0 - 0.06), 0.20);
		CheckTrue("never hit the escalation cap", capped == 0);
		CheckTrue("fumble only ever on the first roll", fumbleAfterEsc == 0);
	}

	// --- the termination guard ---------------------------------------------
	// The cap is not balance, it is what stops authored nonsense from hanging
	// the process. Prove it engages rather than trusting the branch.
	{
		std::mt19937 rng(99);
		RollRules mad = rules;
		mad.critThreshold = 1; // EVERY roll re-triggers
		mad.maxEscalations = 8;
		const Roll r = RollOpenEnded(mad, rng);
		std::printf("\nthe termination guard (critThreshold = 1)\n");
		CheckTrue("capped flag set", r.capped);
		CheckTrue("stopped at maxEscalations", r.escalations == 8);
	}

	// --- the opposed roll ---------------------------------------------------
	// At equal bonuses the two sides are identically distributed, so:
	//   P(hit) = P(attack > defense) = (1 - P(tie)) / 2
	// P(tie) is dominated by both sides landing the same non-escalated face,
	// 100 * (0.94/100)^2 ~= 0.008836, giving P(hit) ~= 0.4956. Measuring this
	// is how a tie-handling mistake shows up — an engine that let ties hit
	// would read ~0.5044 and look fine to the eye.
	std::vector<int> margins;
	{
		std::mt19937 rng(1234);
		margins.reserve(kSamples);
		long long hits = 0;
		double marginSum = 0;
		for (int i = 0; i < kSamples; ++i) {
			const Opposed o = Resolve(0, 0, rules, rng);
			hits += o.hit;
			margins.push_back(o.margin);
			marginSum += o.margin;
		}
		const double pTie = 100.0 * (0.94 / 100.0) * (0.94 / 100.0);
		std::printf("\nthe opposed roll (equal bonuses)\n");
		Check("hit rate", double(hits) / kSamples, (1.0 - pTie) / 2.0, 0.004);
		Check("mean margin (symmetric, so ~0)", marginSum / kSamples, 0.0, 0.15);
	}

	// --- a bonus actually helps --------------------------------------------
	// Monotonicity is the cheapest possible guard against a sign error, and a
	// sign error here would be catastrophic and entirely plausible.
	//
	// The FLAT case is exactly derivable and so is checked tightly: with no
	// escalation, P(A + 30 > D) counts the pairs with d <= a+29, which is
	// sum(a=1..71) (a+29) + 29*100 = 4615 + 2900 = 7515 out of 10'000.
	//
	// The OPEN-ENDED case has no closed form worth deriving, so it is not
	// pinned to a number — a made-up "expected" here would be worse than no
	// check at all (this section originally carried one, and it was wrong).
	// What IS assertable is the DIRECTION: escalation fattens both sides'
	// tails, which dilutes a fixed bonus, so the open-ended hit rate must sit
	// below the flat one while still comfortably beating even odds.
	{
		std::printf("\nbonuses point the right way\n");
		constexpr int kN = 400'000;

		RollRules flat = rules;
		flat.critThreshold = 1000; // unreachable
		flat.fumbleThreshold = 0;

		std::mt19937 rng(7);
		long long flatHi = 0, openHi = 0, openLo = 0;
		for (int i = 0; i < kN; ++i) flatHi += Resolve(30, 0, flat, rng).hit;
		for (int i = 0; i < kN; ++i) openHi += Resolve(30, 0, rules, rng).hit;
		for (int i = 0; i < kN; ++i) openLo += Resolve(0, 30, rules, rng).hit;

		const double flatRate = double(flatHi) / kN;
		const double openRate = double(openHi) / kN;
		Check("attacker +30, no escalation", flatRate, 0.7515, 0.004);
		CheckTrue("+30 attack beats +30 defense", openHi > openLo);
		CheckTrue("escalation dilutes a flat bonus", openRate < flatRate);
		CheckTrue("+30 still well ahead of even odds", openRate > 0.65);
		std::printf("  (open-ended +30 measured at %.4f)\n", openRate);
	}

	// --- THE TAIL (informational) ------------------------------------------
	// Not pass/fail — this is the section that exists to inform the balance
	// numbers, because margin multiplies damage and the rolls are unbounded.
	{
		std::sort(totals.begin(), totals.end());
		std::sort(margins.begin(), margins.end());
		std::printf("\nthe tail — what margin multiplies (informational)\n");
		std::printf("  roll total   p50 %4.0f  p99 %4.0f  p99.9 %4.0f  "
					"p99.99 %4.0f  max %d\n",
					Pct(totals, 0.50), Pct(totals, 0.99), Pct(totals, 0.999),
					Pct(totals, 0.9999), totals.back());
		std::printf("  margin       p50 %4.0f  p99 %4.0f  p99.9 %4.0f  "
					"p99.99 %4.0f  max %d\n",
					Pct(margins, 0.50), Pct(margins, 0.99), Pct(margins, 0.999),
					Pct(margins, 0.9999), margins.back());
		// The number the damage side has to survive: how much bigger the
		// extreme margin is than the typical winning one.
		const double typical = Pct(margins, 0.75);
		if (typical > 0)
			std::printf("  extreme margin is %.1fx the typical winning margin "
						"(p99.99 / p75)\n",
						Pct(margins, 0.9999) / typical);
	}

	// --- the strike, end to end (informational + guards) --------------------
	// ResolveAttack is the ONE place damage is rolled (fx::Deal's strike
	// stage), so this measures the real thing: what the opposed roll did to
	// hit rates, and what the margin multiplier does to damage.
	//
	// The OLD model was a one-sided probability: clamp(accuracy - evasion) and
	// a flat damage jitter. The new one is an opposed d100 with a margin
	// multiplier. They are different shapes, so the point is not that the
	// numbers match — it is to SEE the change rather than discover it in play.
	{
		std::printf("\nthe strike, end to end — a real fighter against a monster\n");
		StrikeRules sr; // the shipped defaults

		// The shipped curve values, restated. RollTest cannot link Balance
		// (that would drag the catalog reader and the file layer in behind it),
		// so these are the DEFAULTS UNDER TEST rather than a live read — if
		// balance.cat is tuned, the shapes below move and this table describes
		// the shipped starting point, which is what it is for.
		CurveRules skill;
		skill.slope = 5.0f;
		skill.cap = 120.0f;
		CurveRules stat;
		stat.slope = 2.0f;
		stat.cap = 35.0f;
		stat.baseline = 10.0f;

		// A party attacker's bonus is skill + stat + the verb's points; a
		// monster's is simply authored (monsters.cat accuracy/defense).
		const auto attacker = [&](float level, float dex, float verb) {
			return CurveValue(level, skill) + CurveValue(dex, stat) + verb;
		};

		struct Case { const char* what; float atk, def; };
		const Case cases[] = {
			{"green (skill 1, DEX 10) vs plain (10)", attacker(1, 10, 0), 10},
			{"trained (skill 10, DEX 12) vs plain", attacker(10, 12, 0), 10},
			{"veteran (skill 30, DEX 14) vs plain", attacker(30, 14, 0), 10},
			{"green vs a nimble monster (50)", attacker(1, 10, 0), 50},
			{"veteran vs a nimble monster (50)", attacker(30, 14, 0), 50},
			// The party's defense is an innate base plus DEX until the dodge
			// and armor skills land; without the base this measured 0.88.
			{"a monster (60) vs a party member (DEX 12)", 60,
			 25.0f + CurveValue(12, stat)},
		};
		std::printf("  %-40s %6s %6s %6s %8s %7s\n", "", "atk", "def", "hit",
					"dmg x1.0", "p99");
		for (const Case& c : cases) {
			std::mt19937 rng(4242);
			constexpr int kN = 200'000;
			long long hits = 0;
			std::vector<int> dmg;
			dmg.reserve(kN);
			for (int i = 0; i < kN; ++i) {
				const AttackResult r = ResolveAttack({10.0f, c.atk, {}},
													 {c.def, 0.0f, 0.0f}, sr, rng);
				if (!r.hit) continue;
				++hits;
				dmg.push_back(static_cast<int>(r.damage + 0.5f));
			}
			std::sort(dmg.begin(), dmg.end());
			double mean = 0;
			for (int d : dmg) mean += d;
			mean = dmg.empty() ? 0 : mean / dmg.size();
			std::printf("  %-40s %6.0f %6.0f %6.3f %8.2f %7.0f\n", c.what, c.atk,
						c.def, double(hits) / kN, mean, Pct(dmg, 0.99));
		}
		std::printf("  (base damage 10, no soak, no resist; \"dmg\" is the mean "
					"LANDED hit)\n");

		// THE DEFENDING SIDE — what a monster swinging at 70 actually achieves
		// against a party member, which is the number the armor trade lives or
		// dies by. The defense terms are restated here (RollTest cannot link
		// Balance); they are the shipped defaults.
		{
			constexpr float kBase = 45.0f;   // defense_base
			constexpr float kMonster = 70.0f; // a typical monsters.cat accuracy
			CurveRules avoid;
			avoid.slope = 3.0f;
			avoid.cap = 60.0f;
			// Armor: floor + (offsettable - offset), the offset curve capped at
			// what training may ever claw back (Balance::ArmorRules).
			const auto armorPenalty = [&](float penalty, float floor, float level) {
				CurveRules off = skill;
				off.slope = 2.0f;
				off.cap = penalty - floor;
				return floor + (off.cap - CurveValue(level, off));
			};
			// `resist` is the FRACTIONAL half of mitigation, which the armor
			// content already carries (armor.cat `resists`) and which the first
			// pass of this table wrongly ignored. It is the half that SCALES:
			// flat soak is a fixed subtraction and shrinks to nothing beside a
			// big blow, while a fraction is worth the same proportion however
			// hard the monster hits.
			struct Def { const char* what; float bonus; float soak; float resist; };
			// Soak and resist are the pieces authored in armor.cat: brigandine
			// 3.5 / slash 0.25, plate 7.0 / slash 0.5.
			const Def defs[] = {
				{"fresh, unarmored (DEX 11)", kBase + CurveValue(11, stat), 0.0f, 0.0f},
				{"trained dodger (avoid 20)",
				 kBase + CurveValue(11, stat) + CurveValue(20, avoid), 0.0f, 0.0f},
				{"veteran dodger (avoid 60)",
				 kBase + CurveValue(11, stat) + CurveValue(60, avoid), 0.0f, 0.0f},
				{"brigandine, untrained", kBase + CurveValue(11, stat) - armorPenalty(25, 10, 0), 3.5f, 0.25f},
				{"brigandine, skill 20", kBase + CurveValue(11, stat) - armorPenalty(25, 10, 20), 3.5f, 0.25f},
				{"plate, untrained", kBase + CurveValue(11, stat) - armorPenalty(45, 20, 0), 7.0f, 0.5f},
				{"plate, skill 30", kBase + CurveValue(11, stat) - armorPenalty(45, 20, 30), 7.0f, 0.5f},
			};
			std::printf("\n  a monster (attack 70) against a party member\n");
			std::printf("    %-30s %6s %6s %8s %9s\n", "", "def", "hit", "soak",
						"dmg/swing");
			for (const Def& d : defs) {
				std::mt19937 rng(31337);
				constexpr int kN = 200'000;
				long long hits = 0;
				double total = 0;
				for (int i = 0; i < kN; ++i) {
					const AttackResult r = ResolveAttack(
						{6.0f, kMonster, {}}, {d.bonus, d.soak, d.resist}, sr, rng);
					if (!r.hit) continue;
					++hits;
					total += r.damage;
				}
				std::printf("    %-30s %6.0f %6.3f %8.1f %9.2f\n", d.what, d.bonus,
							double(hits) / kN, d.soak, total / kN);
			}
			std::printf("    (monster damage 6, armor.cat soak + slash resist; \"dmg/swing\" "
						"misses in — what the fight actually costs)\n");

			// HOW HARD SHOULD A MONSTER HIT? Flat soak is a fixed subtraction,
			// so its worth is entirely relative to the blow: at damage 6 plate
			// erases most of a hit; at damage 30 it barely dents one. This
			// sweep is the crossover — the number deciding whether armor is a
			// WALL or a DISCOUNT, and whether medium is worth its penalty.
			std::printf("\n  dmg/swing by monster damage (the flat-soak crossover)\n");
			std::printf("    %-26s %8s %8s %8s %8s\n", "", "dmg 6", "dmg 12",
						"dmg 20", "dmg 30");
			for (const Def& d : defs) {
				std::printf("    %-26s", d.what);
				for (const float dmg : {6.0f, 12.0f, 20.0f, 30.0f}) {
					std::mt19937 rng(4711);
					constexpr int kN = 120'000;
					double total = 0;
					for (int i = 0; i < kN; ++i) {
						const AttackResult r = ResolveAttack(
							{dmg, kMonster, {}}, {d.bonus, d.soak, d.resist}, sr, rng);
						if (r.hit) total += r.damage;
					}
					std::printf(" %8.2f", total / kN);
				}
				std::printf("\n");
			}
		}

		// WHAT A LIFETIME OF TRAINING IS WORTH — the question the whole design
		// turns on, now answerable in one column: how much does the hit rate
		// actually move as a skill grows, against a fixed opponent?
		std::printf("\n  hit rate by skill level (DEX 10, vs a defense of 30)\n    ");
		for (const float lvl : {0.0f, 1.0f, 5.0f, 10.0f, 20.0f, 40.0f, 80.0f}) {
			std::mt19937 rng(777);
			long long h = 0;
			constexpr int kN = 100'000;
			for (int i = 0; i < kN; ++i)
				h += ResolveAttack({10.0f, attacker(lvl, 10, 0), {}},
								   {30.0f, 0, 0}, sr, rng)
						 .hit;
			std::printf("L%-3.0f %.3f   ", lvl, double(h) / kN);
		}
		std::printf("\n");

		// GUARDS, not observations. These are the properties the swap must not
		// break however the knobs are later tuned.
		std::mt19937 rng(11);
		long long hi = 0, lo = 0;
		float worst = 0.0f;
		for (int i = 0; i < 200'000; ++i) {
			const AttackResult a =
				ResolveAttack({10.0f, attacker(30, 14, 0), {}}, {10.0f, 0.0f, 0.0f},
							  sr, rng);
			const AttackResult b =
				ResolveAttack({10.0f, attacker(1, 8, 0), {}}, {60.0f, 0.0f, 0.0f},
							  sr, rng);
			hi += a.hit;
			lo += b.hit;
			if (a.hit) worst = std::max(worst, a.damage);
		}
		// --- a fumble is AUTOMATIC -----------------------------------------
		// The rule that makes fumbles worth having: they decide the exchange
		// rather than contributing a low number to it, so a veteran with an
		// overwhelming bonus can still drop his guard. Both halves are exactly
		// derivable, which is why they are checked tightly:
		//
		//   attacker cannot lose  -> hits everything EXCEPT its own fumbles,
		//                            = 1 - 0.05 = 0.95
		//   attacker cannot win   -> lands only when the DEFENDER fumbles and
		//                            it does not, = 0.95 x 0.05 = 0.0475
		{
			std::mt19937 rng(90210);
			constexpr int kN = 400'000;
			long long sure = 0, hopeless = 0;
			for (int i = 0; i < kN; ++i) {
				sure += ResolveAttack({10.0f, 5000.0f, {}}, {0, 0, 0}, sr, rng).hit;
				hopeless +=
					ResolveAttack({10.0f, 0.0f, {}}, {5000.0f, 0, 0}, sr, rng).hit;
			}
			std::printf("\nfumbles decide the exchange\n");
			Check("an unloseable attack still fumbles", double(sure) / kN, 0.95,
				  0.004);
			Check("a hopeless attack lands on a fumbled guard",
				  double(hopeless) / kN, 0.0475, 0.003);
		}

		std::printf("\nthe swap's invariants\n");
		CheckTrue("a better attacker hits more often", hi > lo);
		CheckTrue("even the outmatched sometimes land", lo > 0);
		CheckTrue("even the skilled sometimes miss", hi < 200'000);
		// The cap is the whole reason marginCap exists: uncapped, the margin
		// multiplier and the open-ended roll compound without limit.
		CheckTrue("margin multiplier respects marginCap",
				  worst <= 10.0f * sr.marginCap * (1.0f + sr.damageJitter) + 0.01f);
	}

	// --- the contribution curves --------------------------------------------
	// Skill and stat reach the roll through a diminishing-returns curve
	// (Game/Curve.h). Each form makes PROMISES, and the promises are what is
	// checked — not the arithmetic, which would just be the code restated:
	//
	//   every form   rises at `slope` from the origin, so "+5 a level" means
	//                the same thing whichever is picked and the graph can be
	//                compared without re-tuning
	//   every form   is monotonic (more skill is never worse) and odd about
	//                the baseline (a poor stat is a penalty of equal size)
	//   bounded ones stay under the cap FOREVER, which is the whole reason a
	//                cap is worth having: "nobody is ever better than +120"
	//                has to be true to be balanced around
	//   logarithmic  passes the cap — it is the unbounded one on purpose
	{
		std::printf("\nthe contribution curves\n");
		const CurveForm forms[] = {CurveForm::Hyperbolic, CurveForm::Exponential,
								   CurveForm::Logarithmic};
		for (const CurveForm f : forms) {
			CurveRules cr;
			cr.form = f;
			cr.slope = 5.0f;
			cr.cap = 120.0f;

			// The slope at the origin, measured as a secant over a tiny step.
			const float rise = (CurveValue(0.01f, cr) - CurveValue(0.0f, cr)) / 0.01f;
			bool monotonic = true, oddSym = true;
			float last = CurveValue(0.0f, cr);
			for (float x = 0.5f; x <= 400.0f; x += 0.5f) {
				const float v = CurveValue(x, cr);
				if (v < last) monotonic = false;
				if (std::fabs(v + CurveValue(-x, cr)) > 0.001f) oddSym = false;
				last = v;
			}
			char label[96];
			std::snprintf(label, sizeof label, "%s: rises at slope",
						  CurveFormId(f));
			Check(label, rise, 5.0, 0.05);
			std::snprintf(label, sizeof label, "%s: monotonic to level 400",
						  CurveFormId(f));
			CheckTrue(label, monotonic);
			std::snprintf(label, sizeof label, "%s: odd about the baseline",
						  CurveFormId(f));
			CheckTrue(label, oddSym);

			// NEVER EXCEEDS, not "never reaches". The bounded forms approach
			// the cap asymptotically in maths, but in float the exponential
			// ARRIVES: by x ~ 100 the e-term has underflown to zero and the
			// result is exactly `cap`. That is harmless — the promise worth
			// balancing around is that nobody ever gets BETTER than the cap —
			// but the strict phrasing was wrong, and the check caught it.
			const float far = CurveValue(100'000.0f, cr);
			std::snprintf(label, sizeof label, "%s: %s the cap", CurveFormId(f),
						  f == CurveForm::Logarithmic ? "passes" : "never exceeds");
			CheckTrue(label, f == CurveForm::Logarithmic ? far > cr.cap
														 : far <= cr.cap);
		}

		// The shape table — what a player's skill is actually worth, against
		// the number that decides whether it matters (the ~41-point combined
		// deviation of two d100s, measured above).
		std::printf("\n  bonus by skill level (slope 5, cap 120)\n");
		std::printf("  %-14s %6s %6s %6s %6s %6s %6s\n", "form", "L5", "L10",
					"L20", "L40", "L80", "L160");
		for (const CurveForm f : forms) {
			CurveRules cr;
			cr.form = f;
			cr.slope = 5.0f;
			cr.cap = 120.0f;
			std::printf("  %-14s %6.0f %6.0f %6.0f %6.0f %6.0f %6.0f\n",
						CurveFormId(f), CurveValue(5, cr), CurveValue(10, cr),
						CurveValue(20, cr), CurveValue(40, cr),
						CurveValue(80, cr), CurveValue(160, cr));
		}
		std::printf("  (two opposed d100s deviate by ~41 points; a gap much "
					"under that is noise)\n");

		// A stat's contribution, with the baseline that makes 10 worth nothing.
		CurveRules st;
		st.slope = 2.0f;
		st.cap = 35.0f;
		st.baseline = 10.0f;
		std::printf("\n  stat bonus (slope 2, cap 35, baseline 10): "
					"4 %+.0f   7 %+.0f   10 %+.0f   14 %+.0f   20 %+.0f   "
					"40 %+.0f\n",
					CurveValue(4, st), CurveValue(7, st), CurveValue(10, st),
					CurveValue(14, st), CurveValue(20, st), CurveValue(40, st));
		Check("an average stat is worth nothing", CurveValue(10, st), 0.0, 0.001);
		CheckTrue("a poor stat is a penalty", CurveValue(4, st) < 0.0f);
		CheckTrue("stats stay far under skill at the defaults",
				  CurveValue(40, st) < 40.0f);
	}

	// --- armor and the stance ------------------------------------------------
	// The DEFENDER's half (docs/damage-system.md "Armor", "The stance"). The dice
	// and the curves were already covered; these three rules were covered by
	// nothing, and each is a DECISION rather than a sum:
	//   * the penalty floor training can never reach past,
	//   * the two hands combining by MAX and never by sum,
	//   * the two training loops keying on OPPOSITE outcomes.
	//
	// Written against the RULES, not against particular knob values, and swept
	// over several armor profiles including the shipping three — so balance.cat
	// can be retuned freely without falsifying any of it. Only the printed table
	// goes stale, and it is marked informational.
	{
		std::printf("\n--- armor and the stance ---\n");

		struct Profile {
			const char* name;
			float penalty, floor, strength;
		};
		// The shipping defaults (Balance.h) plus two deliberately awkward ones: a
		// class whose floor IS its whole penalty (nothing offsettable at all) and
		// a featherweight. The rules must hold for all of them.
		const Profile profiles[] = {
			{"light", 10.0f, 3.0f, 8.0f},
			{"medium", 25.0f, 10.0f, 11.0f},
			{"heavy", 45.0f, 20.0f, 14.0f},
			{"floor==penalty", 12.0f, 12.0f, 10.0f},
			{"tiny", 1.0f, 0.25f, 5.0f},
		};
		constexpr float kOffsetSlope = 2.0f;  // Balance::armorOffsetSlope
		constexpr float kShortPenalty = 4.0f; // Balance::armorShortPenalty

		// The offset curve. Under --self-test it becomes the LOGARITHMIC form,
		// which by design passes its cap — so the floor stops holding and these
		// checks MUST catch it. Without that the armor section would pass
		// vacuously while only the dice section had proved itself.
		CurveRules offsetCurve;
		offsetCurve.form = selfTest ? CurveForm::Logarithmic : CurveForm::Hyperbolic;
		offsetCurve.slope = kOffsetSlope;

		// Trained to absurdity: if the floor survives this it is a property of the
		// maths, not of a plausible level range.
		constexpr float kSaturated = 100'000.0f;

		for (const Profile& p : profiles) {
			const float offsettable = std::max(0.0f, p.penalty - p.floor);
			const float met = p.strength; // STR met, so this is purely training
			const auto penaltyAt = [&](float level, float strength) {
				return defense::ArmorPenalty(p.floor, offsettable, offsetCurve,
											 level, p.strength, strength,
											 kShortPenalty);
			};
			char label[96];

			// THE FLOOR: training never gets past it, however absurd the level.
			// Strictly ABOVE it while anything is offsettable; exactly AT it when
			// nothing is, which is the degenerate class whose cost cannot be
			// trained away at all.
			std::snprintf(label, sizeof label, "%s: floor never passed", p.name);
			CheckTrue(label, penaltyAt(kSaturated, met) >= p.floor);
			std::snprintf(label, sizeof label, "%s: floor never reached", p.name);
			CheckTrue(label, offsettable > 0.0f
								 ? penaltyAt(kSaturated, met) > p.floor
								 : penaltyAt(kSaturated, met) == p.floor);

			std::snprintf(label, sizeof label, "%s: untrained pays in full", p.name);
			Check(label, penaltyAt(0.0f, met), p.penalty, 0.001);

			// Training only ever helps, and never past the floor.
			bool monotonic = true, aboveFloor = true;
			float prev = penaltyAt(0.0f, met);
			for (float level = 1.0f; level <= 400.0f; level += 1.0f) {
				const float now = penaltyAt(level, met);
				if (now > prev + 1e-4f) monotonic = false;
				if (now < p.floor) aboveFloor = false; // never BELOW it
				prev = now;
			}
			std::snprintf(label, sizeof label, "%s: training only helps", p.name);
			CheckTrue(label, monotonic);
			std::snprintf(label, sizeof label, "%s: never below the floor", p.name);
			CheckTrue(label, aboveFloor);

			// ARMOR ALWAYS COSTS YOU THE ROLL — the trade, not an imbalance.
			std::snprintf(label, sizeof label, "%s: never free", p.name);
			CheckTrue(label, penaltyAt(kSaturated, met) > 0.0f);

			// A STRENGTH SHORTFALL IS PAID TWICE; this is the roll half, exactly
			// armor_short_penalty per missing point. A surplus buys nothing.
			std::snprintf(label, sizeof label, "%s: 3 STR short costs 3x", p.name);
			Check(label, penaltyAt(0.0f, met - 3.0f) - penaltyAt(0.0f, met),
				  3.0 * kShortPenalty, 0.001);
			std::snprintf(label, sizeof label, "%s: STR surplus buys nothing",
						  p.name);
			Check(label, penaltyAt(0.0f, met + 6.0f), penaltyAt(0.0f, met), 0.001);
		}

		// Heavier armor costs more, at equal (zero) training with STR met.
		const auto bare = [&](const Profile& p) {
			return defense::ArmorPenalty(p.floor,
										 std::max(0.0f, p.penalty - p.floor),
										 offsetCurve, 0.0f, p.strength, p.strength,
										 kShortPenalty);
		};
		CheckTrue("heavier armor costs more on the roll",
				  bare(profiles[0]) < bare(profiles[1]) &&
					  bare(profiles[1]) < bare(profiles[2]));

		// --- the stance: the hands combine by MAX, never by sum ---------------
		CurveRules skillCurve;
		skillCurve.form = CurveForm::Hyperbolic;
		skillCurve.slope = 5.0f;
		skillCurve.cap = 120.0f;

		const float lo = 4.0f, hi = 30.0f; // two unequal hands
		const float guardBoth = defense::HandGuard(1.0f, skillCurve, lo, hi);
		const float guardBest = defense::HandGuard(1.0f, skillCurve, hi, 0.0f);
		const double sum = static_cast<double>(CurveValue(lo, skillCurve)) +
						   CurveValue(hi, skillCurve);

		Check("the better hand answers the blow", guardBoth,
			  CurveValue(hi, skillCurve), 0.001);
		CheckTrue("two hands are NOT summed", guardBoth < sum - 1.0);
		// THE ANTI-EXPLOIT: a second, weaker hand held back adds exactly nothing,
		// which is what stops the stance slider being a free defense button.
		Check("a weaker second hand adds nothing", guardBoth, guardBest, 0.001);
		CheckTrue("order does not matter",
				  defense::HandGuard(1.0f, skillCurve, hi, lo) == guardBoth);

		// All-out attack guards with nothing; the guard is linear in the share.
		Check("all-out attack guards with nothing",
			  defense::HandGuard(0.0f, skillCurve, hi, hi), 0.0, 0.0);
		Check("half held back guards half as well",
			  defense::HandGuard(0.5f, skillCurve, lo, hi), guardBoth * 0.5, 0.001);
		// OVER-EXERTION: a NEGATIVE held is a penalty, not a floor at zero. The
		// linearity above is what makes this a continuation of one rule rather
		// than a second one bolted on at the sign change.
		Check("an over-exerted share guards WORSE than nothing",
			  defense::HandGuard(-0.5f, skillCurve, hi, hi),
			  -CurveValue(hi, skillCurve) * 0.5, 0.001);
		CheckTrue("over-exertion's penalty is strictly negative",
				  defense::HandGuard(-0.5f, skillCurve, hi, hi) < 0.0f);
		// The max rule is applied UNBRANCHED at negative held, so the BETTER hand
		// is also the one that over-commits furthest. Paired with the positive
		// case above, this fails if anyone re-introduces a sign-dependent branch:
		// taking the min instead would make the skilled hand the safer one.
		CheckTrue("the better hand also over-commits the furthest",
				  defense::HandGuard(-1.0f, skillCurve, lo, hi) <
					  defense::HandGuard(-1.0f, skillCurve, lo, lo));
		// An empty hand parries `unarmed` — bare-handed, but not nothing. At level
		// 0 that is worth 0, so the claim worth checking is that a TRAINED bare
		// hand still guards.
		CheckTrue("a trained empty hand still guards",
				  defense::HandGuard(1.0f, skillCurve, 12.0f, 0.0f) > 0.0f);

		// --- the two training loops key on OPPOSITE outcomes ------------------
		using defense::Lesson;
		const ArmorClass armored[] = {ArmorClass::Light, ArmorClass::Medium,
									  ArmorClass::Heavy};
		bool unrolledTeaches = false, armorTaughtAvoid = false, wrongLoop = false;
		for (const bool hit : {false, true}) {
			for (const float soak : {0.0f, 5.0f}) {
				// Never rolled, never taught: a bump, a fall, a poison tick.
				if (defense::LessonFrom(false, hit, ArmorClass::None, soak) !=
						Lesson::Nothing ||
					defense::LessonFrom(false, hit, ArmorClass::Heavy, soak) !=
						Lesson::Nothing)
					unrolledTeaches = true;

				// Unarmored: a miss teaches avoid, a landed blow teaches nothing.
				if (defense::LessonFrom(true, hit, ArmorClass::None, soak) !=
					(hit ? Lesson::Nothing : Lesson::Avoid))
					wrongLoop = true;

				for (const ArmorClass c : armored) {
					// Armored: a miss teaches nothing (the armor was not tested);
					// a landed blow teaches the class only if it blunted anything.
					const Lesson want = !hit ? Lesson::Nothing
											 : (soak > 0.0f ? Lesson::Armor
															: Lesson::Nothing);
					const Lesson got = defense::LessonFrom(true, hit, c, soak);
					if (got != want) wrongLoop = true;
					if (got == Lesson::Avoid) armorTaughtAvoid = true;
				}
			}
		}
		CheckTrue("an unrolled event teaches nothing", !unrolledTeaches);
		CheckTrue("each outcome feeds the one right loop", !wrongLoop);
		CheckTrue("armor never trains avoidance", !armorTaughtAvoid);
		// The claim that makes going bare a BUILD rather than the poor man's
		// option: a loadout trains exactly one loop, so you cannot practise both.
		bool oneLoopPerLoadout = true;
		for (const float soak : {0.0f, 5.0f}) {
			const bool bareTrainsAvoid =
				defense::LessonFrom(true, false, ArmorClass::None, soak) ==
				Lesson::Avoid;
			const bool bareTrainsArmor =
				defense::LessonFrom(true, true, ArmorClass::None, soak) ==
				Lesson::Armor;
			const bool wornTrainsArmor =
				defense::LessonFrom(true, true, ArmorClass::Heavy, soak) ==
				Lesson::Armor;
			const bool wornTrainsAvoid =
				defense::LessonFrom(true, false, ArmorClass::Heavy, soak) ==
				Lesson::Avoid;
			if (!bareTrainsAvoid || bareTrainsArmor) oneLoopPerLoadout = false;
			if (wornTrainsAvoid) oneLoopPerLoadout = false;
			if (wornTrainsArmor != (soak > 0.0f)) oneLoopPerLoadout = false;
		}
		CheckTrue("a loadout trains exactly one loop", oneLoopPerLoadout);

		// The shape table — INFORMATIONAL, and the one thing here that goes stale
		// if balance.cat is retuned. Penalty in d100 points, STR met.
		std::printf("\n  armor penalty by training (INFORMATIONAL, shipping "
					"defaults; ~41 pts = the dice deviation)\n");
		std::printf("  %-14s %7s %7s %7s %7s %7s %7s\n", "class", "L0", "L5",
					"L20", "L50", "L200", "floor");
		for (int i = 0; i < 3; ++i) {
			const Profile& p = profiles[i];
			const float off = std::max(0.0f, p.penalty - p.floor);
			const auto at = [&](float level) {
				return defense::ArmorPenalty(p.floor, off, offsetCurve, level,
											 p.strength, p.strength, kShortPenalty);
			};
			std::printf("  %-14s %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f\n", p.name,
						at(0), at(5), at(20), at(50), at(200), p.floor);
		}
		std::printf("  (a floor column never reached is the point — training "
					"cannot make plate agile)\n");

		// --- avoid is UNARMORED-ONLY, and defense is TYPED --------------------
		// The last two rules that lived in PartyTarget::Evasion. What the world
		// still owns is only the RESOLUTION (an inventory to a worn class, a type
		// to its two flags, a skill id to a level); every decision is here.
		//
		// THESE CHECKS CANNOT PASS VACUOUSLY, and that is by construction rather
		// than by assertion: every "ignores X" is PAIRED with a "reads Y" against
		// the same defender, so a Guard that ignored everything would fail the
		// second half of each pair. Confirmed by mutation — making the magical
		// branch also add HandGuard fails "magical ignores the hands entirely" and
		// nothing else. (The --self-test curve injection cannot reach these: they
		// are dispatch decisions, not curve shapes.)
		std::printf("\n  avoid is unarmored-only, and defense is typed\n");

		// A defender with real training in everything, so any rule that wrongly
		// lets a term through shows up as a difference rather than a zero.
		const auto inputs = [&](ArmorClass worn, defense::GuardKind kind) {
			defense::GuardInputs in;
			in.base = 30.0f;
			in.dexterity = 14.0f;
			in.statCurve = CurveRules{CurveForm::Hyperbolic, 2.0f, 35.0f, 10.0f};
			in.worn = worn;
			in.armorPenalty = 25.0f; // as if medium, untrained
			in.avoidLevel = 40.0f;
			in.avoidCurve = CurveRules{CurveForm::Hyperbolic, 3.0f, 60.0f, 0.0f};
			in.held = 1.0f;
			in.skillCurve = skillCurve;
			in.kind = kind;
			in.schoolLevel = 25.0f;
			in.leftLevel = 30.0f;
			in.rightLevel = 4.0f;
			return in;
		};

		// THE PRECEDENCE: a school wins over physical, and only a type that is
		// neither leaves the defender nothing to parry with.
		using defense::GuardKind;
		CheckTrue("a school guards magically",
				  defense::GuardKindFor({true, false}) == GuardKind::Magical);
		CheckTrue("a school beats physical",
				  defense::GuardKindFor({true, true}) == GuardKind::Magical);
		CheckTrue("physical without a school parries",
				  defense::GuardKindFor({false, true}) == GuardKind::Physical);
		CheckTrue("neither leaves nothing to parry with",
				  defense::GuardKindFor({false, false}) == GuardKind::Neither);

		// AVOID IS UNARMORED-ONLY. Armored, the avoid skill must be invisible to
		// the roll however high it is trained — otherwise light armor plus a
		// trained dodge would stack the two loops that cannot both be practised.
		{
			defense::GuardInputs bareIn = inputs(ArmorClass::None, GuardKind::Physical);
			defense::GuardInputs wornIn = inputs(ArmorClass::Medium, GuardKind::Physical);
			const float bareGuard = defense::Guard(bareIn);
			const float wornGuard = defense::Guard(wornIn);

			// Unarmored: the avoid skill is worth something.
			defense::GuardInputs bareUntrained = bareIn;
			bareUntrained.avoidLevel = 0.0f;
			CheckTrue("unarmored, avoid training helps",
					  bareGuard > defense::Guard(bareUntrained) + 1.0f);

			// Armored: it is worth exactly nothing, at any level.
			defense::GuardInputs wornSaturated = wornIn;
			wornSaturated.avoidLevel = 100'000.0f;
			Check("armored, avoid training is worth nothing",
				  defense::Guard(wornSaturated), wornGuard, 0.0);

			// And the armor penalty is SUBTRACTED where avoid is added — the trade.
			defense::GuardInputs noPenalty = wornIn;
			noPenalty.armorPenalty = 0.0f;
			Check("the armor penalty comes off the roll",
				  wornGuard - defense::Guard(noPenalty), -25.0, 0.001);
			// Conversely the unarmored defender pays no penalty however heavy the
			// number handed in — the branch, not the value, decides.
			defense::GuardInputs barePenalised = bareIn;
			barePenalised.armorPenalty = 999.0f;
			Check("unarmored, an armor penalty is ignored",
				  defense::Guard(barePenalised), bareGuard, 0.0);
		}

		// TYPED DEFENSE: each kind reads its own term and no other.
		{
			const defense::GuardInputs phys = inputs(ArmorClass::None, GuardKind::Physical);
			const defense::GuardInputs magi = inputs(ArmorClass::None, GuardKind::Magical);
			const defense::GuardInputs none = inputs(ArmorClass::None, GuardKind::Neither);

			// Physical: the hands decide, the school is irrelevant.
			defense::GuardInputs physNoSchool = phys;
			physNoSchool.schoolLevel = 0.0f;
			Check("physical ignores the school skill", defense::Guard(physNoSchool),
				  defense::Guard(phys), 0.0);
			defense::GuardInputs physNoHands = phys;
			physNoHands.leftLevel = physNoHands.rightLevel = 0.0f;
			CheckTrue("physical reads the hands",
					  defense::Guard(physNoHands) < defense::Guard(phys) - 1.0f);

			// MAGICAL: THE HANDS PLAY NO PART. This is the rule most easily got
			// wrong, and the one the old comment described incorrectly.
			defense::GuardInputs magiNoHands = magi;
			magiNoHands.leftLevel = magiNoHands.rightLevel = 0.0f;
			Check("magical ignores the hands entirely", defense::Guard(magiNoHands),
				  defense::Guard(magi), 0.0);
			defense::GuardInputs magiNoSchool = magi;
			magiNoSchool.schoolLevel = 0.0f;
			CheckTrue("magical reads the incoming school",
					  defense::Guard(magiNoSchool) < defense::Guard(magi) - 1.0f);
			// A fire specialist shrugs off fire and is no better than anyone else
			// against frost — the same term read at two different levels.
			defense::GuardInputs specialist = magi;
			specialist.schoolLevel = 80.0f;
			CheckTrue("a specialist turns their own school aside better",
					  defense::Guard(specialist) > defense::Guard(magi) + 1.0f);

			// NEITHER: nothing parries it, so the stance contributes nothing —
			// but armor and DEX still do, which is what makes it a guard and not
			// an auto-hit.
			defense::GuardInputs noneLoaded = none;
			noneLoaded.schoolLevel = 999.0f;
			noneLoaded.leftLevel = noneLoaded.rightLevel = 999.0f;
			Check("nothing parries an unschooled non-physical blow",
				  defense::Guard(noneLoaded), defense::Guard(none), 0.0);
			CheckTrue("but DEX and the base still guard", defense::Guard(none) > 0.0f);

			// An all-out attacker guards with none of the three, whatever arrives.
			for (const GuardKind k : {GuardKind::Physical, GuardKind::Magical,
									  GuardKind::Neither}) {
				defense::GuardInputs allOut = inputs(ArmorClass::None, k);
				allOut.held = 0.0f;
				defense::GuardInputs allOutBare = allOut;
				allOutBare.schoolLevel = 0.0f;
				allOutBare.leftLevel = allOutBare.rightLevel = 0.0f;
				Check("all-out attack guards with no skill at all",
					  defense::Guard(allOut), defense::Guard(allOutBare), 0.0);
			}

			// OVER-EXERTION reaches the whole guard, not just HandGuard: a share
			// past 1 must come out the far side of Guard() as a number BELOW what
			// an all-out attacker gets, in every branch that reads the stance.
			// Paired with the "guards with no skill at all" check above, so a
			// Guard() that clamped the sign would fail one of the two.
			for (const GuardKind k : {GuardKind::Physical, GuardKind::Magical}) {
				defense::GuardInputs allOut = inputs(ArmorClass::None, k);
				allOut.held = 0.0f;
				defense::GuardInputs over = allOut;
				over.held = -0.5f; // share 1.5
				CheckTrue("over-exerting guards worse than all-out attacking",
						  defense::Guard(over) < defense::Guard(allOut) - 1.0f);
			}
			// ...and Neither still ignores it, because nothing parries that at all
			// — the stance can only make you worse at a defense you HAVE.
			{
				defense::GuardInputs allOut = inputs(ArmorClass::None,
													 GuardKind::Neither);
				allOut.held = 0.0f;
				defense::GuardInputs over = allOut;
				over.held = -0.5f;
				Check("over-exertion cannot worsen a guard nothing parries",
					  defense::Guard(over), defense::Guard(allOut), 0.0);
			}
		}
	}

	// --- the stance trades both sides ------------------------------------------
	// docs/damage-system.md "The stance" + "Over-exertion". One number moves the
	// attack and the guard together, in opposite directions. It used to be an
	// exact coupling (attack + guard constant); since 2026-09-28 both ends CURVE
	// (Michael: over-exertion climbs to x5 attack, a full guard to x2 defense),
	// so what is pinned now is the shape: the ends reach their knobs, the steps
	// steepen toward each extreme, the trade still runs one way, and a multiple
	// of 1 puts the plain line back.
	{
		std::printf("\n--- the stance trades both sides ---\n");
		CurveRules skillCurve;
		skillCurve.form = CurveForm::Hyperbolic;
		skillCurve.slope = 5.0f;
		skillCurve.cap = 120.0f;
		const float lvl = 20.0f;
		const double full = CurveValue(lvl, skillCurve);
		const defense::StanceRules stance{2.0f, 5.0f, 2.0f, 5.0f}; // the shipped defaults

		// The honest range's ATTACK is still the plain share.
		Check("a full commitment is the plain curve value",
			  defense::StanceAttack(1.0f, lvl, skillCurve, stance), full, 0.001);
		Check("half the share puts half the skill behind the swing",
			  defense::StanceAttack(0.5f, lvl, skillCurve, stance), full * 0.5, 0.001);
		Check("guarding with everything attacks with nothing",
			  defense::StanceAttack(0.0f, lvl, skillCurve, stance), 0.0, 0.0);

		// THE TWO ENDS reach their knobs, and the middle meets them seamlessly.
		Check("100% over-exertion attacks at exert_attack_max",
			  defense::AttackWeight(2.0f, stance), 5.0, 0.0001);
		Check("0% attack guards at guard_defense_max",
			  defense::GuardWeight(0.0f, stance), 2.0, 0.0001);
		Check("a full commitment guards with nothing",
			  defense::GuardWeight(1.0f, stance), 0.0, 0.0);
		Check("the attack is continuous at the full-commitment mark",
			  defense::AttackWeight(1.0001f, stance), 1.0, 0.001);
		Check("an over-exerted guard is still the plain penalty",
			  defense::GuardWeight(1.5f, stance), -0.5, 0.0001);

		// NOT LINEAR: each end's last step is worth more than its first. Checked
		// on both sides, so a curve slipped onto one side alone cannot pass.
		CheckTrue("the attack steepens toward 100% over-exertion",
				  defense::AttackWeight(2.0f, stance) - defense::AttackWeight(1.9f, stance) >
					  defense::AttackWeight(1.1f, stance) - defense::AttackWeight(1.0f, stance));
		// ...and each is the SQUARE, pinned at the half-way point. Without these a
		// shallower curve (h x (1 + h) on the guard) passed every other check - it
		// still reaches its knob, still steepens, still trades one way. The attack
		// side's twin is "half way to 100% buys one more skill's worth" below.
		Check("half held back guards at 0.5 x (1 + 0.25)",
			  defense::GuardWeight(0.5f, stance), 0.625, 0.0001);
		CheckTrue("the guard steepens toward 0% attack",
				  defense::GuardWeight(0.0f, stance) - defense::GuardWeight(0.1f, stance) >
					  defense::GuardWeight(0.9f, stance) - defense::GuardWeight(1.0f, stance));

		// The TRADE still runs one way: more attack always costs guard.
		bool traded = true;
		double lastAttack = -1.0, lastGuard = 1e9;
		for (const float share : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f}) {
			const double attack = defense::AttackWeight(share, stance);
			const double guard = defense::GuardWeight(share, stance);
			if (attack <= lastAttack || guard >= lastGuard) traded = false;
			lastAttack = attack;
			lastGuard = guard;
		}
		CheckTrue("more attack always means less guard, at every share", traded);

		// A multiple of 1 is the plain line - the knob's "off" setting.
		const defense::StanceRules plainGuard{2.0f, 5.0f, 1.0f};
		Check("guard_defense_max = 1 is the plain held share",
			  defense::GuardWeight(0.3f, plainGuard), 0.7, 0.0001);

		// --- what over-exertion BUYS -----------------------------------------
		Check("an honest stance buys nothing",
			  defense::ExertionPoints(1.0f, lvl, skillCurve, stance), 0.0, 0.0);
		Check("a defensive stance buys nothing either",
			  defense::ExertionPoints(0.3f, lvl, skillCurve, stance), 0.0, 0.0);
		// Half way to exert_max is a QUARTER of the way up the curve: 1 + 4 x 0.25.
		Check("half way to 100% buys one more skill's worth",
			  defense::ExertionPoints(1.5f, lvl, skillCurve, stance), full, 0.001);
		Check("100% over-exertion buys four more skills' worth",
			  defense::ExertionPoints(2.0f, lvl, skillCurve, stance), full * 4.0, 0.001);
		// The points bought are exactly the attack ABOVE an honest full swing —
		// stated against StanceAttack rather than re-derived, because the bill is
		// charged against this number and the two must not drift apart.
		Check("the points bought are the attack past a full commitment",
			  defense::ExertionPoints(1.7f, lvl, skillCurve, stance),
			  defense::StanceAttack(1.7f, lvl, skillCurve, stance) -
				  defense::StanceAttack(1.0f, lvl, skillCurve, stance),
			  0.001);
		// THE FLOOR (exert_floor, 2026-09-28). An untrained skill's term is zero,
		// which used to make its over-exertion free AND useless - a 100% kick at
		// unarmed 0 bought nothing and cost nothing. Now the stretch past 1
		// multiplies at least the floor, so it buys (and bills) 4 x floor at 100%.
		Check("an untrained fighter borrows the floor's worth",
			  defense::ExertionPoints(2.0f, 0.0f, skillCurve, stance),
			  4.0 * stance.exertFloor, 0.001);
		Check("...and swings with it",
			  defense::StanceAttack(2.0f, 0.0f, skillCurve, stance),
			  4.0 * stance.exertFloor, 0.001);
		Check("an untrained HONEST swing still gets nothing from skill",
			  defense::StanceAttack(1.0f, 0.0f, skillCurve, stance), 0.0, 0.0);
		// ...a skill already worth more than the floor is untouched by it (the
		// "four more skills' worth" check above is the same claim at lvl)...
		CheckTrue("a trained skill outweighs the floor here",
				  CurveValue(lvl, skillCurve) > stance.exertFloor);
		// ...and a floor of 0 is the old rule, which is how to switch it off.
		const defense::StanceRules noFloor{2.0f, 5.0f, 2.0f, 0.0f};
		Check("exert_floor = 0: an untrained fighter borrows nothing",
			  defense::ExertionPoints(2.0f, 0.0f, skillCurve, noFloor), 0.0, 0.001);
		CheckTrue("...while a trained one borrows plenty",
				  defense::ExertionPoints(2.0f, lvl, skillCurve, stance) > 1.0f);
		CheckTrue("a deeper skill borrows more at the same share",
				  defense::ExertionPoints(1.5f, 60.0f, skillCurve, stance) >
					  defense::ExertionPoints(1.5f, lvl, skillCurve, stance));

		// --- the drunken haymaker: over-exertion widens an UNTRAINED fumble ----
		// (exert_fumble / exert_skilled_level, 2026-09-28.) The extra band is
		// exert_fumble x p x inexperience; each factor is pinned at a point
		// where the others are held, so none can pass for another.
		Check("an honest stance adds no fumble faces",
			  defense::ExertionFumbleFaces(1.0f, 0.0f, stance), 0.0, 0.0);
		Check("untrained at 100% over-exertion: +45 faces",
			  defense::ExertionFumbleFaces(2.0f, 0.0f, stance), 45.0, 0.0001);
		Check("...half way to 100%: half the band",
			  defense::ExertionFumbleFaces(1.5f, 0.0f, stance), 22.5, 0.0001);
		Check("...half way to skilled: half the band",
			  defense::ExertionFumbleFaces(2.0f, 2.5f, stance), 22.5, 0.0001);
		Check("...at exert_skilled_level: none at all",
			  defense::ExertionFumbleFaces(2.0f, 5.0f, stance), 0.0, 0.0);
		Check("past exert_max counts as 100%, never more",
			  defense::ExertionFumbleFaces(3.0f, 0.0f, stance), 45.0, 0.0001);

		// And the resolver actually USES it: measured, not assumed. A +45 band on
		// the plain 5 fumbles on a first face of 50 or less - half the swings -
		// against a plain swing's 5%. Seeded, so the numbers are stable.
		{
			StrikeRules sr;
			constexpr int kN = 100'000;
			const auto fumbleRate = [&](int extra) {
				std::mt19937 rng(8080);
				long long fumbles = 0;
				for (int i = 0; i < kN; ++i) {
					AttackProfile atk{10.0f, 40.0f, {}};
					atk.fumbleExtra = extra;
					if (ResolveAttack(atk, {40.0f, 0.0f, 0.0f}, sr, rng).fumble)
						++fumbles;
				}
				return double(fumbles) / kN;
			};
			Check("a plain swing fumbles 5% of the time", fumbleRate(0), 0.05, 0.005);
			Check("an untrained 100% haymaker fumbles half the time", fumbleRate(45),
				  0.50, 0.01);
		}
	}

	// --- when it goes wrong: fumble consequences --------------------------------
	// docs/damage-system.md "When it goes wrong". Two things are measured here and
	// neither is arithmetic: WHICH entries a table parses to (the surface content
	// authors actually touch) and WHETHER a given fumble was a severe one. The
	// consequences themselves need the world and are executed in DungeonWorld.
	{
		std::printf("\n--- when it goes wrong: fumble consequences ---\n");
		using namespace dungeon::game::mishap;

		// --- severity comes from the die face, not a second draw --------------
		// Face 0 is "no fumble was recorded", NOT a catastrophic roll. Without
		// that guard every unrolled event in the game reads as a severe fumble,
		// which is why it is a function and not an inline `face <= knob`.
		CheckTrue("no fumble is never severe", !Severe(0, 1));
		CheckTrue("the worst face is severe", Severe(1, 1));
		CheckTrue("a mild fumble is not", !Severe(5, 1));
		CheckTrue("the knob widens the severe band", Severe(2, 2));
		CheckTrue("...and only that far", !Severe(3, 2));
		// A knob of 0 turns the severe table OFF entirely rather than making
		// every fumble severe — the failure mode a naive comparison would have.
		CheckTrue("a knob of zero disables severity", !Severe(1, 0));

		// --- the parser: what an author writes is what fires ------------------
		{
			std::vector<Entry> out;
			Parse("recover 2.5, drop", out, "test");
			Check("two entries parsed", static_cast<double>(out.size()), 2.0, 0.0);
			CheckTrue("the first is recover", !out.empty() &&
												  out[0].kind == Kind::Recover);
			Check("...with its value", out.empty() ? 0.0 : out[0].value, 2.5, 0.001);
			CheckTrue("the second is drop",
					  out.size() > 1 && out[1].kind == Kind::Drop);
			// Drop takes no value and does not need one: an entry with nothing
			// after it must still parse, or half the vocabulary is unwritable.
			Check("...and needs no value of its own",
				  out.size() > 1 ? out[1].value : -1.0, 0.0, 0.0);
		}
		{
			// EVERY token round-trips. Paired with TokenFor so a Kind added
			// without its token — or a table whose two directions disagree —
			// fails here rather than in a fight.
			bool allRoundTrip = true;
			for (const Kind k : {Kind::Recover, Kind::Stumble, Kind::Drop,
								 Kind::Fling, Kind::SelfHit, Kind::Wild}) {
				Kind back{};
				if (!KindFromToken(TokenFor(k), back) || back != k)
					allRoundTrip = false;
			}
			CheckTrue("every consequence round-trips through its token",
					  allRoundTrip);
		}
		{
			// A TYPO IS DROPPED, NOT GUESSED AT. The failure this prevents is
			// silent: a table that fell back to `recover` would look authored
			// and do something else forever.
			std::vector<Entry> out;
			Parse("recovr 2.0", out, "test");
			Check("an unknown token adds nothing",
				  static_cast<double>(out.size()), 0.0, 0.0);
			// ...and the same for a value-taking token with no value, which
			// would otherwise land as a zero-multiplier no-op.
			out.clear();
			Parse("recover", out, "test");
			Check("a value-taking token needs its value",
				  static_cast<double>(out.size()), 0.0, 0.0);
			// Non-vacuous by pairing: the valueless three must NOT be rejected
			// by that same rule, or the check above passes for the wrong reason.
			out.clear();
			Parse("drop; fling; wild", out, "test");
			Check("the valueless three parse bare",
				  static_cast<double>(out.size()), 3.0, 0.0);
		}
		{
			// Blank entries are not errors — a trailing comma is how a list gets
			// edited, and an empty spec is how most weapons say "use the default".
			std::vector<Entry> out;
			Parse("drop,", out, "test");
			Check("a trailing comma is harmless",
				  static_cast<double>(out.size()), 1.0, 0.0);
			out.clear();
			Parse("", out, "test");
			Check("an empty table parses to nothing",
				  static_cast<double>(out.size()), 0.0, 0.0);
		}

		// --- the defaults -----------------------------------------------------
		// The mild default is TEMPO and nothing else: at 5% of every swing, what
		// happens on most fumbles has to be survivable enough to shrug at.
		{
			const DefaultTable mild = DefaultFumble(2.2f);
			Check("the default fumble is one consequence",
				  static_cast<double>(mild.size()), 1.0, 0.0);
			CheckTrue("...and it is tempo, not damage",
					  !mild.empty() && mild[0].kind == Kind::Recover);
			Check("...carrying the knob it was given",
				  mild.empty() ? 0.0 : mild[0].value, 2.2, 0.001);
			const DefaultTable bad = DefaultSevere();
			CheckTrue("the severe default disarms you",
					  bad.size() == 1 && bad[0].kind == Kind::Drop);
		}
	}

	// --- a critical that pierces ------------------------------------------------
	// The one crit consequence. Measured through the SHIPPING resolver rather than
	// by inspection, because what it has to skip (the soak subtraction) sits in the
	// middle of the damage expression, and it must skip it ONLY on a critical.
	{
		std::printf("\n--- a critical that pierces ---\n");
		StrikeRules rules;
		rules.damageJitter = 0.0f; // measure the rule, not the noise
		DefenseProfile def{/*defenseBonus=*/0.0f, /*soak=*/8.0f, /*resist=*/0.0f};

		// A bonus high enough that the defender never wins, so every sample is a
		// landed blow and the only variable left is whether the roll went
		// open-ended. Crits are ~6% of rolls, so a few thousand finds plenty.
		std::mt19937 rng(20260813u);
		double plainCrit = 0.0, plainNormal = 0.0, pierceCrit = 0.0;
		int nPlainCrit = 0, nPlainNormal = 0, nPierceCrit = 0;
		for (int i = 0; i < 20000; ++i) {
			const AttackResult a =
				ResolveAttack({20.0f, 400.0f, DamageType{}, false}, def, rules, rng);
			if (!a.hit) continue;
			if (a.crit) { plainCrit += a.damage; ++nPlainCrit; }
			else { plainNormal += a.damage; ++nPlainNormal; }
		}
		for (int i = 0; i < 20000; ++i) {
			const AttackResult a =
				ResolveAttack({20.0f, 400.0f, DamageType{}, true}, def, rules, rng);
			if (a.hit && a.crit) { pierceCrit += a.damage; ++nPierceCrit; }
		}
		CheckTrue("the sample found criticals of both kinds",
				  nPlainCrit > 50 && nPierceCrit > 50 && nPlainNormal > 100);
		// THE RULE: a piercing critical keeps the soak an ordinary one loses.
		CheckTrue("a piercing critical beats an ordinary one",
				  nPlainCrit && nPierceCrit &&
					  pierceCrit / nPierceCrit > plainCrit / nPlainCrit + 1.0);
		// ...and does it by exactly the soak, not by some other multiplier that
		// happened to be applied. The margins differ between the two samples, so
		// this is bounded rather than exact — but a change of the RIGHT SIZE is
		// what distinguishes "skipped the soak" from "got a bonus".
		CheckTrue("...by about the soak it ignored",
				  nPlainCrit && nPierceCrit &&
					  std::abs((pierceCrit / nPierceCrit) -
							   (plainCrit / nPlainCrit) - 8.0) < 2.0);
		// NON-VACUOUS BY PAIRING: pierce must do nothing at all on a NON-critical,
		// or the flag is just a damage bonus wearing a crit's name. Same seed,
		// same rolls, so the two normal-hit averages are comparable.
		{
			std::mt19937 a(777u), b(777u);
			double normalPlain = 0.0, normalPierce = 0.0;
			int nA = 0, nB = 0;
			for (int i = 0; i < 8000; ++i) {
				const AttackResult ra =
					ResolveAttack({20.0f, 400.0f, DamageType{}, false}, def, rules, a);
				const AttackResult rb =
					ResolveAttack({20.0f, 400.0f, DamageType{}, true}, def, rules, b);
				if (ra.hit && !ra.crit) { normalPlain += ra.damage; ++nA; }
				if (rb.hit && !rb.crit) { normalPierce += rb.damage; ++nB; }
			}
			CheckTrue("pierce changes nothing on an ordinary hit",
					  nA == nB && std::abs(normalPlain - normalPierce) < 0.001);
		}
	}

	// --- the fumble face travels ------------------------------------------------
	// The plumbing the whole severity rule stands on: ResolveAttack must report
	// WHICH face fumbled, and must report 0 when nothing did. A silent 0 here would
	// make every fumble mild and the severe table dead code that still passes its
	// own unit checks.
	{
		std::printf("\n--- the fumble face travels ---\n");
		StrikeRules rules;
		DefenseProfile def{0.0f, 0.0f, 0.0f};
		std::mt19937 rng(4242u);
		int fumbles = 0, faceInBand = 0, faceOnNonFumble = 0;
		for (int i = 0; i < 20000; ++i) {
			const AttackResult a =
				ResolveAttack({10.0f, 50.0f, DamageType{}, false}, def, rules, rng);
			if (a.fumble) {
				++fumbles;
				if (a.fumbleFace >= 1 &&
					a.fumbleFace <= static_cast<int>(rules.fumbleThreshold))
					++faceInBand;
			} else if (a.fumbleFace != 0) {
				++faceOnNonFumble;
			}
		}
		CheckTrue("the sample fumbled at all", fumbles > 200);
		Check("every fumble reported a face in the band",
			  static_cast<double>(faceInBand), static_cast<double>(fumbles), 0.0);
		Check("...and nothing else reported one at all",
			  static_cast<double>(faceOnNonFumble), 0.0, 0.0);
	}


	// --- the area blast ------------------------------------------------------
	// Michael's model (docs/damage-system.md "The area blast"): a WAVEFRONT that
	// expands over ticks, deflecting sideways off walls and REFLECTING back when
	// there is nowhere sideways to go, with units converging on one square
	// MULTIPLYING it. Every one of those is geometric, which is exactly the kind of
	// rule that reads correctly in a comment and is wrong in a corridor.
	{
		std::printf("\n--- the area blast ---\n");
		using namespace dungeon::game::blast;

		const auto hitAt = [](const Result& r, int x, int z, int tick) -> const Hit* {
			for (int i = 0; i < r.count; ++i)
				if (r.hits[i].x == x && r.hits[i].z == z && r.hits[i].tick == tick)
					return &r.hits[i];
			return nullptr;
		};
		const auto hitsOnTick = [](const Result& r, int tick) {
			int n = 0;
			for (int i = 0; i < r.count; ++i) n += (r.hits[i].tick == tick);
			return n;
		};

		Rules fire;
		fire.damage = 20.0f;
		fire.falloff = 4.0f;
		fire.force = 5;
		fire.rate = 0.05f;

		// AN OPEN ROOM GIVES A RING. Everything passable, so nothing is deflected
		// and nothing converges: four neighbours, one unit each.
		const PassableFn open = [](int, int) { return true; };
		{
			const Result r = Propagate(10, 10, fire, open);
			const Hit* centre = hitAt(r, 10, 10, 0);
			CheckTrue("the detonation square is hit on tick 0", centre != nullptr);
			if (centre) {
				Check("...for the full figure", centre->damage, 20.0, 0.001);
				Check("...at distance 0", centre->distance, 0.0, 0.0);
			}
			Check("open room: a ring of four on tick 1", hitsOnTick(r, 1), 4.0, 0.0);
			bool ring = true;
			for (const auto& [dx, dz] : {std::pair{0, -1}, std::pair{0, 1},
										 std::pair{-1, 0}, std::pair{1, 0}}) {
				const Hit* h = hitAt(r, 10 + dx, 10 + dz, 1);
				if (!h || h->arrivals != 1 || h->distance != 1) ring = false;
			}
			CheckTrue("open room: each ring square takes one unit", ring);
			const Hit* side = hitAt(r, 11, 10, 1);
			if (side)
				Check("a ring square costs one step of falloff", side->damage,
					  20.0 - 4.0, 0.001);
			Check("open room: the force is spent in full", r.spent, 5.0, 0.0);
		}

		// A DEAD-END CORRIDOR: the blast goes off at the closed end of a 1-wide
		// passage running east. THE CASE THE WHOLE MODEL EXISTS FOR — three of the
		// four units cannot go their way, two deflect east and the one facing the
		// closed end reflects east, so ALL FOUR converge on the first open square
		// and it takes a x4 tick. That is the firewall.
		const PassableFn deadEnd = [](int x, int z) { return z == 10 && x >= 10; };
		{
			const Result r = Propagate(10, 10, fire, deadEnd);
			Check("dead end: one square reached on tick 1", hitsOnTick(r, 1), 1.0, 0.0);
			const Hit* h = hitAt(r, 11, 10, 1);
			CheckTrue("dead end: it is the one open neighbour", h != nullptr);
			if (h) {
				Check("dead end: all four units converge there", h->arrivals, 4.0, 0.0);
				Check("dead end: so it takes a x4 tick", h->damage,
					  (20.0 - 4.0) * 4.0, 0.001);
				Check("dead end: still only one step out", h->distance, 1.0, 0.0);
			}
			// Nothing leaks into the stone either side.
			bool inCorridor = true;
			for (int i = 0; i < r.count; ++i)
				if (r.hits[i].z != 10 || r.hits[i].x < 10) inCorridor = false;
			CheckTrue("dead end: nothing leaks through the walls", inCorridor);
			// ...and the confined blast hurts far more than the open one did.
			const Hit* openSide = nullptr;
			const Result openR = Propagate(10, 10, fire, open);
			openSide = hitAt(openR, 11, 10, 1);
			CheckTrue("a confined tick beats an open one",
					  h && openSide && h->damage > openSide->damage * 3.0f);
		}

		// A T-JUNCTION splits three ways: the branch north, and east/west each
		// taking a deflected unit as well as their own.
		const PassableFn tee = [](int x, int z) {
			return (z == 10 && x >= 8 && x <= 12) || (x == 10 && z <= 10 && z >= 8);
		};
		{
			Rules wide = fire;
			wide.force = 9;
			const Result r = Propagate(10, 10, wide, tee);
			Check("T-junction: three ways out on tick 1", hitsOnTick(r, 1), 3.0, 0.0);
			const Hit* north = hitAt(r, 10, 9, 1);
			const Hit* west = hitAt(r, 9, 10, 1);
			const Hit* east = hitAt(r, 11, 10, 1);
			CheckTrue("T-junction: all three branches are reached",
					  north && west && east);
			if (north) Check("T-junction: the open branch takes one unit",
							 north->arrivals, 1.0, 0.0);
			// The unit that would have gone south has nowhere to go but sideways,
			// and BOTH perpendiculars are open, so it splits rather than picking a
			// side — there is no honest handedness to pick.
			CheckTrue("T-junction: a blocked unit splits both ways",
					  west && east && west->arrivals == 2 && east->arrivals == 2);
		}

		// PERSISTENCE. A gas cloud fills squares and keeps biting, and a unit
		// re-entering a filled square adds to its CONCENTRATION — so poison
		// contained is more poisonous, exactly as fire contained is (Michael,
		// 2026-08-11).
		Rules gas = fire;
		gas.persistence = Persistence::Persistent;
		gas.rate = 1.5f; // creeps, where the fire rushed
		gas.force = 12;
		{
			const Result pocket = Propagate(10, 10, gas, deadEnd);
			// The detonation square is bitten again on a later tick, which a
			// transient front would never do.
			bool reBitten = false;
			for (int i = 0; i < pocket.count; ++i)
				if (pocket.hits[i].x == 10 && pocket.hits[i].z == 10 &&
					pocket.hits[i].tick > 0)
					reBitten = true;
			CheckTrue("a persistent cloud keeps biting its own square", reBitten);

			// Concentration BUILDS: the most units ever seen in one square grows
			// past the one it started with.
			int peak = 0;
			for (int i = 0; i < pocket.count; ++i)
				peak = std::max(peak, pocket.hits[i].arrivals);
			CheckTrue("confinement concentrates a cloud", peak > 1);

			// A transient front VACATES — tested in the OPEN, because in a dead end
			// it rightly does come back: reflection returning to the square behind
			// is the firewall, not lingering. So the two have to be told apart by
			// geometry, and only open ground isolates "does it stay of its own
			// accord". (This check first ran on the corridor and failed for exactly
			// that reason, which is the distinction worth pinning.)
			const Result front = Propagate(10, 10, fire, open);
			bool transientLingered = false;
			for (int i = 0; i < front.count; ++i)
				if (front.hits[i].x == 10 && front.hits[i].z == 10 &&
					front.hits[i].tick > 0)
					transientLingered = true;
			CheckTrue("a transient front does not linger in the open",
					  !transientLingered);
			// ...and in a dead end it DOES return, which is the firewall.
			const Result wall = Propagate(10, 10, fire, deadEnd);
			bool firewallReturned = false;
			for (int i = 0; i < wall.count; ++i)
				if (wall.hits[i].x == 10 && wall.hits[i].z == 10 &&
					wall.hits[i].tick > 0)
					firewallReturned = true;
			CheckTrue("a reflected front sweeps back over its origin",
					  firewallReturned);
		}

		// Ordering and geometry invariants over every shape above.
		{
			const PassableFn* shapes[] = {&open, &deadEnd, &tee};
			bool ordered = true, orthogonal = true, nonNegative = true;
			for (const PassableFn* fn : shapes) {
				for (const Rules& rr : {fire, gas}) {
					const Result r = Propagate(10, 10, rr, *fn);
					for (int i = 1; i < r.count; ++i)
						if (r.hits[i].tick < r.hits[i - 1].tick) ordered = false;
					for (int i = 0; i < r.count; ++i) {
						if (r.hits[i].damage < 0.0f) nonNegative = false;
						// Every hit is a Manhattan-reachable square: no diagonal
						// ever appears, whatever deflection did.
						const int md = std::abs(r.hits[i].x - 10) +
									   std::abs(r.hits[i].z - 10);
						if (md > r.hits[i].tick + 1) orthogonal = false;
					}
				}
			}
			CheckTrue("hits come back in tick order", ordered);
			CheckTrue("damage never goes negative", nonNegative);
			CheckTrue("nothing outruns orthogonal steps", orthogonal);
		}

		// Degenerate inputs, which content can produce.
		Check("no force does nothing", Propagate(0, 0, Rules{}, open).count, 0.0, 0.0);
		{
			Rules noDamage = fire;
			noDamage.damage = 0.0f;
			Check("no damage is not an area effect",
				  Propagate(0, 0, noDamage, open).count, 0.0, 0.0);
		}
		{
			// Entombed: nowhere to go at all. It must still burn its own square and
			// then stop, rather than spinning on an empty frontier.
			const PassableFn sealed = [](int x, int z) { return x == 10 && z == 10; };
			const Result r = Propagate(10, 10, sealed ? fire : fire, sealed);
			Check("entombed: only its own square", r.count, 1.0, 0.0);
			CheckTrue("entombed: force is left unspent", r.leftover > 0);
		}
		{
			// A burst whose centre is INSIDE stone, as when a bolt breaks on a wall.
			const PassableFn beyond = [](int x, int z) { return x >= 11 && z == 10; };
			const Result r = Propagate(10, 10, fire, beyond);
			CheckTrue("a burst in stone burns no wall square",
					  hitAt(r, 10, 10, 0) == nullptr);
			CheckTrue("...but the room beyond is reached", hitAt(r, 11, 10, 1));
		}
		{
			Rules huge = fire;
			huge.force = kMaxCells + 40;
			const Result r = Propagate(0, 0, huge, open);
			CheckTrue("force past the ceiling is clamped and says so", r.clamped);
		}

		// The shape table — INFORMATIONAL: one fire blast in three geometries.
		std::printf("\n  one fire blast, force 9, full 20, falloff 4 "
					"(INFORMATIONAL)\n");
		std::printf("  %-18s %6s %6s %9s %11s\n", "geometry", "hits", "ticks",
					"peak x", "peak dmg");
		Rules show = fire;
		show.force = 9;
		const std::pair<const char*, const PassableFn*> named[] = {
			{"open room", &open}, {"dead-end corridor", &deadEnd},
			{"T-junction", &tee}};
		for (const auto& [name, fn] : named) {
			const Result r = Propagate(10, 10, show, *fn);
			int peak = 0;
			float worst = 0.0f;
			for (int i = 0; i < r.count; ++i) {
				peak = std::max(peak, r.hits[i].arrivals);
				worst = std::max(worst, r.hits[i].damage);
			}
			std::printf("  %-18s %6d %6d %9d %11.1f\n", name, r.count, r.ticks, peak,
						worst);
		}
		std::printf("  (same blast throughout — the geometry decides whether it "
					"rings, splits or reflects)\n");
	}

	// --- the attacker's type axis --------------------------------------------
	// docs/damage-system.md "Two axes". The defender's half (resists) was always
	// there; this is its mirror, and the rules worth pinning are the ones that make
	// it a MIRROR rather than a second resist table: it is clamped BOTH ways with no
	// escapes (unlike a resist, where 1.0 is immunity and past it absorption), and
	// it can never turn a blow into healing.
	{
		std::printf("\n--- the attacker's type axis ---\n");
		// The shipping clamps, mirrored from Balance's defaults. The arithmetic is
		// the real defense::Potent; only these two numbers are restated, and the
		// checks below are written against the RULES rather than the values.
		constexpr float kPotencyClamp = 0.6f, kResistClamp = 0.8f;
		const DamageType fire{2}, slash{0};
		ResistTable p;

		Check("no potency leaves a blow alone", defense::Potent(20.0f, p, fire, kPotencyClamp), 20.0, 0.001);
		p[fire] = 0.5f;
		Check("potent in fire hits harder", defense::Potent(20.0f, p, fire, kPotencyClamp), 30.0, 0.001);
		Check("...and only in that type", defense::Potent(20.0f, p, slash, kPotencyClamp), 20.0, 0.001);
		p[fire] = -0.5f;
		Check("feeble in fire hits softer", defense::Potent(20.0f, p, fire, kPotencyClamp), 10.0, 0.001);

		// CLAMPED BOTH WAYS, with none of the resist side's escapes. A resist of 1.0
		// means immunity and past it absorption — identity, not stacking — but
		// "I deal 150% fire" is stacking, so there is nothing to exempt.
		p[fire] = 5.0f;
		Check("an absurd potency is clamped up", defense::Potent(20.0f, p, fire, kPotencyClamp),
			  20.0 * (1.0 + kPotencyClamp), 0.001);
		p[fire] = 1.0f; // the resist side's IMMUNITY value: no meaning here
		Check("1.0 is not special on the attack side", defense::Potent(20.0f, p, fire, kPotencyClamp),
			  20.0 * (1.0 + kPotencyClamp), 0.001);
		p[fire] = -5.0f;
		Check("an absurd feebleness is clamped down", defense::Potent(20.0f, p, fire, kPotencyClamp),
			  20.0 * (1.0 - kPotencyClamp), 0.001);
		CheckTrue("the clamp is tighter than the resist clamp",
				  kPotencyClamp < kResistClamp);

		// A blow never becomes healing, however feeble — that is the ABSORB stage's
		// business on the defender's side, and it must not be reachable from here.
		p[fire] = -50.0f;
		CheckTrue("a feeble blow never heals", defense::Potent(20.0f, p, fire, kPotencyClamp) >= 0.0f);
		Check("zero damage stays zero", defense::Potent(0.0f, p, fire, kPotencyClamp), 0.0, 0.0);

		// Potency SUMS across sources (a weapon plus each worn piece), which is what
		// ResistTable::Add gives both halves for free.
		ResistTable weapon, worn;
		weapon[fire] = 0.2f;
		worn[fire] = 0.1f;
		ResistTable total = weapon;
		total.Add(worn);
		Check("potency sums across weapon and worn", total[fire], 0.3, 0.001);
		Check("...and the sum is what scales the blow", defense::Potent(10.0f, total, fire, kPotencyClamp),
			  13.0, 0.001);

		std::printf("  (the two axes meet in one multiplication: potency scales the "
					"blow, the resist answers it)\n");
	}

	// --- the resource pools: aptitude and practice ------------------------------
	// docs/health-and-healing.md. Every pool takes a LINEAR share from its
	// aptitude and a TAPERING one from its practice, and the same pair drives
	// the regeneration rate. The arithmetic is small; what is worth measuring is
	// the edge it shares with the armor floor, and the fact that the two
	// formulas cannot drift apart from the save loader's inverse of one of them.
	{
		std::printf("\n--- the resource pools ---\n");
		using namespace dungeon::game::resource;
		CurveRules statCurve;
		statCurve.form = CurveForm::Hyperbolic;
		statCurve.slope = 2.0f;
		statCurve.cap = 35.0f;
		statCurve.baseline = 10.0f;

		Rules r;
		r.perAptitude = 1.0f;
		r.skillMax = {CurveForm::Hyperbolic, 1.0f, 25.0f, 0.0f};
		r.regenBase = 0.15f;
		r.regenPerAptitude = 0.01f;
		r.regenPerMax = 0.0f;
		r.skillRegen = {CurveForm::Hyperbolic, 0.02f, 0.45f, 0.0f};

		// The two ends of the practice term. An untrained one is worth exactly
		// nothing (so an unplayed character is unchanged by the whole system),
		// and a preposterously trained one still has not reached the cap — which
		// is what makes "nobody is ever better than +cap" a true sentence to
		// balance around rather than an aspiration.
		Check("an untrained practice adds nothing to the pool",
			  Maximum(r, 20.0f, 10.0f, 0.0f), 30.0, 0.001);
		CheckTrue("a deep practice approaches the cap without reaching it",
				  Maximum(r, 20.0f, 10.0f, 100000.0f) < 30.0 + r.skillMax.cap);
		CheckTrue("...and gets most of the way there",
				  Maximum(r, 20.0f, 10.0f, 100000.0f) > 30.0 + 0.99 * r.skillMax.cap);

		// THE ZERO-CAP RULE, and it is the reason this TU exists. CurveValue
		// answers a non-positive cap with the straight line its slope describes
		// — right for a curve in general, catastrophic for a resource, and
		// EXACTLY the shape of the armor-floor bug this project already paid for
		// once. A cap of zero must switch the term off, not unbound it.
		Rules capless = r;
		capless.skillMax.cap = 0.0f;
		Check("a zero cap switches the practice OFF",
			  Maximum(capless, 20.0f, 10.0f, 400.0f), 30.0, 0.001);
		// Non-vacuous by pairing: the same skill level through the raw curve is
		// enormous, so the check above cannot be passing because 400 is small.
		CheckTrue("...and is NOT the unbounded line the raw curve would give",
				  CurveValue(400.0f, capless.skillMax) > 100.0f);
		Rules regenCapless = r;
		regenCapless.skillRegen.cap = 0.0f;
		Check("the same rule holds for the regen term",
			  RegenPerSec(regenCapless, statCurve, 10.0f, 30.0f, 400.0f),
			  RegenPerSec(regenCapless, statCurve, 10.0f, 30.0f, 0.0f), 0.0001);

		// THE SAVE LOADER'S INVERSE. A pre-v17 save stored maxima and no bases,
		// and Game.cpp recovers each base by subtracting Contribution — so if the
		// two ever disagree, every such save loads with the wrong pool, silently,
		// because a wrong amount of health still looks like an amount of health.
		//
		// BE HONEST ABOUT WHAT THIS CHECK IS: it CANNOT fail today, because
		// Maximum is *defined* as base + Contribution and a tautology is what
		// that delegation buys. Mutating Contribution moves both sides together
		// and the round trip still holds (measured). Its job is the day someone
		// inlines the arithmetic back into Maximum and adds a fourth term to it
		// alone — which is exactly how a formula and its inverse drift apart, and
		// exactly what the delegation exists to prevent. A regression check for a
		// property currently guaranteed by construction, and no more than that.
		bool roundTrips = true;
		for (const float apt : {4.0f, 10.0f, 17.0f})
			for (const float lvl : {0.0f, 3.0f, 25.0f}) {
				const float base = 21.0f;
				const float max = Maximum(r, base, apt, lvl);
				if (std::fabs((max - Contribution(r, apt, lvl)) - base) > 0.001f)
					roundTrips = false;
			}
		CheckTrue("max minus Contribution recovers the authored base", roundTrips);

		// A pool is a capacity and a rate is a performance, so the aptitude
		// enters them differently — linearly and through the (baselined) stat
		// curve. The consequence worth pinning: an AVERAGE aptitude is worth
		// nothing to the RATE, while it is worth plenty to the MAXIMUM.
		Check("an average aptitude adds nothing to the rate",
			  RegenPerSec(r, statCurve, 10.0f, 0.0f, 0.0f), r.regenBase, 0.001);
		CheckTrue("...while it adds its whole self to the maximum",
				  Maximum(r, 0.0f, 10.0f, 0.0f) > 9.99f);
		CheckTrue("a poor aptitude is a real penalty to the rate",
				  RegenPerSec(r, statCurve, 4.0f, 0.0f, 0.0f) < r.regenBase);

		// Neither formula may go negative. A hopeless aptitude empties a pool; it
		// does not invert one, and a rate that drained the bar would be a DoT
		// wearing regeneration's clothes — that mechanic exists, and it lives in
		// the effects pipeline where everything else that hurts you lives.
		Rules cruel = r;
		cruel.regenBase = 0.0f;
		cruel.regenPerAptitude = 5.0f;
		CheckTrue("a savage aptitude penalty floors the rate at zero",
				  RegenPerSec(cruel, statCurve, 1.0f, 0.0f, 0.0f) >= 0.0f);
		CheckTrue("a savage aptitude penalty floors the pool at zero",
				  Maximum(r, 0.0f, -500.0f, 0.0f) >= 0.0f);

		// Each pool names ONE practice and the mapping is total — a resource with
		// no skill id would train nothing and never grow, silently.
		CheckTrue("every pool names a practice",
				  *SkillId(Kind::Health) && *SkillId(Kind::Stamina) &&
					  *SkillId(Kind::Mana));
		CheckTrue("...and they are three different ones",
				  std::strcmp(SkillId(Kind::Health), SkillId(Kind::Stamina)) &&
					  std::strcmp(SkillId(Kind::Stamina), SkillId(Kind::Mana)) &&
					  std::strcmp(SkillId(Kind::Health), SkillId(Kind::Mana)));
		// PoolRules::For must not alias — three pools sharing one knob set would
		// make every balance change move all three together.
		PoolRules pools;
		pools.health.perAptitude = 1.0f;
		pools.stamina.perAptitude = 2.0f;
		pools.mana.perAptitude = 3.0f;
		CheckTrue("PoolRules hands each pool its own knobs",
				  pools.For(Kind::Health).perAptitude == 1.0f &&
					  pools.For(Kind::Stamina).perAptitude == 2.0f &&
					  pools.For(Kind::Mana).perAptitude == 3.0f);

		// --- supplies ---------------------------------------------------------
		// The same zero-cap trap, and it is WORSE here: an unbounded conditioning
		// term would make the fitter member's drain rise forever, which is a
		// runaway inside the mechanism that exists to prevent runaways.
		SupplyRules food;
		food.perSecond = 0.0035f;
		food.condDrain = {CurveForm::Hyperbolic, 0.0002f, 0.0035f, 0.0f};

		Check("an untrained member drains at the base rate",
			  DrainPerSec(food, 0.0f), food.perSecond, 1e-6);
		CheckTrue("conditioning costs more, which is the brake",
				  DrainPerSec(food, 25.0f) > DrainPerSec(food, 0.0f));
		CheckTrue("...and that cost is bounded",
				  DrainPerSec(food, 100000.0f) <
					  food.perSecond + food.condDrain.cap);
		SupplyRules freeCond = food;
		freeCond.condDrain.cap = 0.0f;
		Check("a zero cap makes conditioning free, not unbounded",
			  DrainPerSec(freeCond, 400.0f), freeCond.perSecond, 1e-6);
		// A meter that filled itself by standing still would undo the whole
		// point of supplies, so the rate floors at zero however the knobs are set.
		SupplyRules perverse;
		perverse.perSecond = -5.0f;
		CheckTrue("a negative rate cannot refill a meter",
				  DrainPerSec(perverse, 10.0f) >= 0.0f);

		// Refill reports what it RESTORED, and "nothing" has to be answerable —
		// it is what lets the caller keep the item instead of eating it for no
		// effect.
		CheckTrue("an empty refill is not Any()", !Refill{}.Any());
		CheckTrue("water alone counts", (Refill{0.0f, 1.0f}).Any());
		CheckTrue("food alone counts", (Refill{1.0f, 0.0f}).Any());

		std::printf("  (two orderings live in the AUTHORED knobs, not in this\n"
					"   arithmetic — stamina > mana > health per second, and water\n"
					"   draining faster than food — so the eval harness checks both)\n");
	}

	// --- the one-pipeline check's own arithmetic ----------------------------
	// Game/DamageLedger.h is what turns docs/effects.md's invariant from a hand
	// sweep into a standing rule, so it gets the same treatment every other rule
	// in this file gets: the SHIPPING ledger linked straight in, and the
	// properties the check depends on stated one at a time.
	//
	// NON-VACUOUS BY MUTATION, not by --self-test: the injected 90-sided die
	// cannot reach a float comparison, so these were confirmed the way the
	// defense section's were — by breaking the ledger on purpose and MEASURING.
	// Making Credit a no-op fails 6; comparing exactly instead of against the
	// epsilon fails 1; reporting an unmatched address as a violation fails 1.
	//
	// The third of those is why the mutations were run rather than reasoned
	// about: it passed CLEAN the first time. The test watched a prefix of the
	// array, so every value that came back with no baseline arrived after the
	// end of the baseline run and fell out of the merge instead of reaching the
	// branch being mutated. A test can cover the RULE and still miss the BRANCH.
	{
		using namespace dungeon::game::ledger;
		std::printf("\nThe one-pipeline ledger (Game/DamageLedger.h)\n");

		// Members of a fixed array, so the addresses the ledger keys on are
		// stable for the whole section — the same property the real roster and
		// monster vectors have between two checkpoints.
		float hp[4] = {30.0f, 30.0f, 30.0f, 30.0f};
		Violation found[4];
		const auto sweep = [&](Ledger& led, int count) {
			led.BeginSweep();
			for (int i = 0; i < count; ++i)
				led.Observe(hp[i], Key{Subject::Member, i});
		};

		{
			Ledger led;
			led.Arm(true);
			sweep(led, 4);
			led.Checkpoint("baseline", found); // first sweep: nothing to judge

			// The rule itself: a move nobody claimed is a violation, and a move
			// somebody claimed is not. Both in ONE checkpoint, because a check
			// that only ever sees the failing case cannot tell a working ledger
			// from one that reports everything.
			hp[0] -= 5.0f;
			led.Credit(hp[0], -5.0f, Reason::Pipeline);
			hp[1] -= 5.0f; // ...and this one went around it
			sweep(led, 4);
			const int n = led.Checkpoint("blow", found);
			Check("one unexplained move, one explained", n, 1.0, 0.0);
			CheckTrue("...and it names the member who was not accounted for",
					  n == 1 && found[0].key.id == 1);
			Check("...reporting the whole unexplained amount",
				  n == 1 ? found[0].Unexplained() : 0.0, -5.0, 1e-4);
			// FOUR, not eight: the checkpoint that TAKES the first baseline
			// judges nothing, because there is nothing yet to judge it against.
			// That is the property that makes `pipelineguard on` safe to type in
			// the middle of a fight — arming can never manufacture a violation
			// out of whatever happened before it.
			Check("only the second checkpoint judged anything",
				  static_cast<double>(led.GetStats().valuesChecked), 4.0, 0.0);
		}
		{
			// TWO reasons on one value in one region — a blow and a regen tick
			// land on the same member between two checkpoints all the time, and
			// they have to SUM rather than the last one winning.
			Ledger led;
			led.Arm(true);
			hp[2] = 20.0f;
			sweep(led, 4);
			led.Checkpoint("baseline", found);
			{
				const Explained a{led, hp[2], Reason::Pipeline};
				hp[2] -= 8.0f;
			}
			{
				const Explained b{led, hp[2], Reason::Regen};
				hp[2] += 3.0f;
			}
			sweep(led, 4);
			Check("two reasons on one value sum instead of racing",
				  led.Checkpoint("mixed", found), 0.0, 0.0);
			Check("...and each is credited to its own route",
				  led.GetStats().credited[static_cast<size_t>(Reason::Regen)],
				  3.0, 1e-4);
		}
		{
			// THE PROPERTY THE WHOLE `Explained` FORM EXISTS FOR: it measures
			// what the float DID, not what the caller meant. A wound of 40 on a
			// member with 20 left moves 20 — and a scope told "40" would report
			// a 20-point violation on the one line that is behaving perfectly.
			Ledger led;
			led.Arm(true);
			hp[3] = 20.0f;
			sweep(led, 4);
			led.Checkpoint("baseline", found);
			{
				const Explained clamped{led, hp[3], Reason::Pipeline};
				hp[3] -= 40.0f;
				if (hp[3] < 0.0f) hp[3] = 0.0f; // WoundMember's clamp
			}
			sweep(led, 4);
			Check("a clamped wound explains what it actually took",
				  led.Checkpoint("overkill", found), 0.0, 0.0);
		}
		{
			// A value that appears between checkpoints (a spawned monster) has
			// no baseline, and one that disappears (a level swap, a container
			// that moved) has nothing left to compare. NEITHER is a violation —
			// but a vanished baseline IS counted, because silently losing a
			// vector's worth of coverage is the failure this check would
			// otherwise hide from itself.
			Ledger led;
			led.Arm(true);
			// Watch the two MIDDLE values, so what comes back later sorts on
			// BOTH SIDES of the baseline. That detail is the check, not
			// decoration: entries are matched by walking two sorted runs
			// together, and a new address before the baseline takes a different
			// branch from one after it. A first version of this test watched a
			// prefix, so every new value arrived at the tail — and a mutation
			// that reported unmatched addresses as violations passed it clean.
			led.BeginSweep();
			led.Observe(hp[1], Key{Subject::Member, 1});
			led.Observe(hp[2], Key{Subject::Member, 2});
			led.Checkpoint("baseline", found);

			hp[0] -= 7.0f; // both of these moved while nobody was watching
			hp[3] -= 7.0f;
			sweep(led, 4); // ...and are now watched, with no baseline
			Check("a value with no baseline is not judged, either side of it",
				  led.Checkpoint("grown", found), 0.0, 0.0);

			led.BeginSweep();
			led.Observe(hp[1], Key{Subject::Member, 1});
			Check("a vanished value is not a violation",
				  led.Checkpoint("shrunk", found), 0.0, 0.0);
			Check("...but it is counted as coverage lost",
				  static_cast<double>(led.GetStats().dropped), 3.0, 0.0);
		}
		{
			// Float noise must not read as a write. The epsilon sits far above a
			// few ulps of a health bar and far below the smallest damage the
			// game can deal, so both halves are stated.
			Ledger led;
			led.Arm(true);
			hp[0] = 30.0f;
			sweep(led, 4);
			led.Checkpoint("baseline", found);
			hp[0] -= kEpsilon * 0.5f;
			sweep(led, 4);
			Check("noise below the epsilon is not a violation",
				  led.Checkpoint("noise", found), 0.0, 0.0);
			hp[0] -= kEpsilon * 4.0f;
			sweep(led, 4);
			Check("...and a real move just above it is",
				  led.Checkpoint("small", found), 1.0, 0.0);
		}
		{
			// Rebase is what every load, respawn and dev-console fiat calls. It
			// must take the new baseline WITHOUT judging the old one, or turning
			// a save on would report the whole party as unexplained.
			Ledger led;
			led.Arm(true);
			hp[0] = 30.0f;
			sweep(led, 4);
			led.Checkpoint("baseline", found);
			hp[0] = 1.0f; // a load replacing state wholesale
			sweep(led, 4);
			led.Rebase();
			sweep(led, 4);
			Check("a rebase forgives the state it replaced",
				  led.Checkpoint("after load", found), 0.0, 0.0);
			CheckTrue("...and a rebase is not counted as a checkpoint",
					  led.GetStats().checkpoints == 2);
		}
		{
			// A standing violation must not drown the log: the first of a given
			// (phase, subject) is reported and the rest are counted only. Same
			// rule Core/Diagnostics follows for stacks, and for the same reason.
			Ledger led;
			Violation v{"phase", Key{Subject::Member, 0}, -1.0f, 0.0f};
			CheckTrue("the first of a kind is reported", led.ShouldReport(v));
			CheckTrue("...and the second is not", !led.ShouldReport(v));
			Violation other{"phase", Key{Subject::Monster, 0}, -1.0f, 0.0f};
			CheckTrue("a different subject is its own report",
					  led.ShouldReport(other));
		}
		{
			// Disarmed, it costs nothing and claims nothing — including the
			// stats, which is what makes a disarmed release build's `pipeline`
			// readout say checks=0 rather than a confident PASS it did not earn.
			Ledger led;
			led.Arm(false);
			sweep(led, 4);
			led.Checkpoint("baseline", found);
			hp[0] -= 9.0f;
			sweep(led, 4);
			Check("a disarmed ledger finds nothing",
				  led.Checkpoint("ignored", found), 0.0, 0.0);
			Check("...and claims to have checked nothing",
				  static_cast<double>(led.GetStats().valuesChecked), 0.0, 0.0);
		}
	}

	// --- monster power: the override and the bands -------------------------------
	// Game/Power.h (tool-refinement Phase 2). What the generator ranks by and the
	// palette's pips show, so its two rules are stated one at a time: an authored
	// power replaces the derived one and a non-positive one does not, and a band
	// is the fifth of the range a power falls in, with the edges where the header
	// says. EditorTest phase 13 checks the same numbers reach the game.
	{
		using namespace dungeon::game::power;
		std::printf("\nMonster power (Game/Power.h)\n");
		Check("unset, the power is the derived one", Resolve(7.5, 0.0), 7.5, 0.0);
		Check("an authored power replaces it", Resolve(7.5, 20.0), 20.0, 0.0);
		Check("...downward too", Resolve(7.5, 2.0), 2.0, 0.0);
		Check("a negative override is not an override", Resolve(7.5, -3.0), 7.5, 0.0);
		Check("a negative derived value reads as 0", Resolve(-1.0, 0.0), 0.0, 0.0);

		Range r;
		for (const double p : {0.0, 4.0, 10.0}) r.Add(p);
		Check("the range spans the lowest to the highest", r.hi - r.lo, 10.0, 0.0);
		Check("the bottom of the range is band 1", Band(0.0, r), 1, 0);
		Check("just under a fifth is still band 1", Band(1.99, r), 1, 0);
		Check("a fifth up is band 2", Band(2.0, r), 2, 0);
		Check("the middle is band 3", Band(5.0, r), 3, 0);
		Check("just under the top is band 5", Band(9.99, r), 5, 0);
		Check("the top itself is band 5, not a sixth", Band(10.0, r), 5, 0);
		Check("above the range clamps to 5", Band(40.0, r), 5, 0);
		Check("below the range clamps to 1", Band(-2.0, r), 1, 0);
		Range one;
		one.Add(6.0);
		Check("one kind alone is the middle band", Band(6.0, one), 3, 0);
		Check("no kinds at all: the middle band", Band(6.0, Range{}), 3, 0);
	}

	// --- a style's monster list ----------------------------------------------------
	// Game/Style.h (tool-refinement Phase 5): `<id> [weight]`, comma-separated.
	// The rename sweep goes through RenameMonster and the type editor's rows
	// through Parse/Format, so a list that does not round-trip would rewrite
	// every style it touched.
	{
		using namespace dungeon::game::style;
		std::printf("\nStyle monster lists (Game/Style.h)\n");
		const std::vector<Pick> p = ParseMonsters(" skeleton 3, skel_archer ,mummy 0.5,, ");
		Check("three entries, the empty ones skipped", static_cast<double>(p.size()), 3.0, 0.0);
		Check("a written weight is read", p.size() > 0 ? p[0].weight : -1.0f, 3.0, 0.0);
		Check("an absent weight is 1", p.size() > 1 ? p[1].weight : -1.0f, 1.0, 0.0);
		Check("an id is trimmed", p.size() > 1 && p[1].id == "skel_archer" ? 1 : 0, 1, 0);
		Check("a fractional weight is read", p.size() > 2 ? p[2].weight : -1.0f, 0.5, 0.0);
		Check("it writes back as written, a weight of 1 left out",
			  FormatMonsters(p) == "skeleton 3, skel_archer, mummy 0.5" ? 1 : 0, 1, 0);
		Check("a weight of 0 can never be chosen, so it is dropped",
			  static_cast<double>(ParseMonsters("blob 0, mummy").size()), 1.0, 0.0);
		Check("a bad weight reads as 1", ParseMonsters("blob x")[0].weight, 1.0, 0.0);
		Check("a repeated id keeps its first entry",
			  ParseMonsters("blob 2, blob 5")[0].weight, 2.0, 0.0);
		std::vector<Pick> r = ParseMonsters("skeleton 3, mummy");
		Check("a rename finds the one entry", RenameMonster(r, "mummy", "wrapped"), 1, 0);
		Check("...and keeps every weight",
			  FormatMonsters(r) == "skeleton 3, wrapped" ? 1 : 0, 1, 0);
	}

	// --- the shape brushes' geometry ------------------------------------------------
	// Game/Carve.h (tool-refinement Phase 6). What each brush opens, checked
	// without a map: a corridor reaches its far end however it winds, a room is
	// its rectangle, a stamp turned four times is itself, a region is joined to
	// what touches it.
	{
		using namespace dungeon::game::carve;
		std::printf("\nShape brushes (Game/Carve.h)\n");
		// Every open square reachable from (ax,az), 4-connected, within the shape.
		const auto connects = [](const Shape& s, int ax, int az, int bx, int bz) {
			std::vector<std::pair<int, int>> todo{{ax, az}}, seen{{ax, az}};
			if (!s.Opens(ax, az)) return false;
			while (!todo.empty()) {
				const auto [x, z] = todo.back();
				todo.pop_back();
				if (x == bx && z == bz) return true;
				for (const auto [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
					const std::pair<int, int> n{x + dx, z + dz};
					if (!s.Opens(n.first, n.second) ||
						std::find(seen.begin(), seen.end(), n) != seen.end())
						continue;
					seen.push_back(n);
					todo.push_back(n);
				}
			}
			return false;
		};
		const Shape l = Corridor(2, 3, 9, 7, 1, 0.0f, 5);
		Check("a straight-ish corridor is one L: |dx| + |dz| + 1 squares",
			  static_cast<double>(l.open.size()), 12.0, 0.0);
		Check("...from one end to the other", connects(l, 2, 3, 9, 7) ? 1 : 0, 1, 0);
		bool allMeanderConnect = true, someWound = false;
		for (dungeon::u32 seed = 1; seed <= 40; ++seed) {
			const Shape m = Corridor(5, 5, 20, 14, 1, 0.9f, seed);
			allMeanderConnect = allMeanderConnect && connects(m, 5, 5, 20, 14);
			someWound = someWound || m.open.size() > 25;
		}
		Check("a winding corridor always arrives (40 seeds)", allMeanderConnect ? 1 : 0, 1, 0);
		Check("...and does wind (some longer than the L)", someWound ? 1 : 0, 1, 0);
		const Shape wide = Corridor(0, 0, 6, 0, 2, 0.0f, 1);
		Check("a two-wide corridor is two rows of squares", static_cast<double>(wide.open.size()),
			  14.0, 0.0);
		Check("every corridor square is a corridor's",
			  std::all_of(l.open.begin(), l.open.end(),
						  [](const Square& q) { return q.role == Role::Corridor; }) ? 1 : 0, 1, 0);
		Check("a room is its rectangle, corners either way round",
			  static_cast<double>(Room(7, 6, 3, 2).open.size()), 25.0, 0.0);
		const Stamp st = ParseStamp(" .#.. | .... |##");
		Check("a stamp reads its rows", static_cast<double>(st.Height()), 3.0, 0.0);
		Stamp four = st;
		for (int i = 0; i < 4; ++i) four = Turned(four);
		// Turning pads short rows with '-', so compare what each places.
		const Shape s0 = StampAt(st, 10, 10, 0), s4 = StampAt(four, 10, 10, 0);
		Check("four quarter turns give the stamp back",
			  s0.open.size() == s4.open.size() && s0.solid == s4.solid ? 1 : 0, 1, 0);
		Check("a stamp opens its '.' squares and marks its '#' ones",
			  static_cast<double>(s0.open.size() * 10 + s0.solid.size()), 73.0, 0.0);
		const Stamp t1 = Turned(st);
		Check("a quarter turn stands a 4-wide, 3-tall stamp 4 tall",
			  static_cast<double>(t1.Height() * 10 + t1.Width()), 43.0, 0.0);
		// Clockwise: the old bottom-left '#' becomes the new top-left.
		Check("...turned clockwise", t1.rows[0][0] == '#' ? 1 : 0, 1, 0);
		std::vector<dungeon::u8> floor(16, 0);
		floor[5] = floor[6] = floor[9] = floor[10] = 1; // the middle 2x2 of a 4x4
		const Shape reg = Region(floor, 4, 4, 10, 10,
								 [](int x, int z) { return x == 9 && z == 11; }, 3);
		Check("a region's generated floor is its room squares",
			  reg.Opens(11, 11) && reg.Opens(12, 12) ? 1 : 0, 1, 0);
		Check("...joined to the open square touching its edge",
			  connects(reg, 10, 11, 12, 12) ? 1 : 0, 1, 0);
		const Shape none = Region(floor, 4, 4, 10, 10, [](int, int) { return false; }, 3);
		Check("...and to nothing when nothing touches it",
			  static_cast<double>(none.open.size()), 4.0, 0.0);
		Check("a lone square's rim is its eight neighbours",
			  static_cast<double>(Rim(Room(0, 0, 0, 0)).size()), 8.0, 0.0);

		// Dress (Phase 7): a style laid over a whole grid. A 3x3 room with a
		// 2-square passage off its east side.
		const char* kDressGrid[] = {"#######", "#...###", "#.....#", "#...###", "#######"};
		std::vector<dungeon::u8> dg(7 * 5, 0);
		for (int z = 0; z < 5; ++z)
			for (int x = 0; x < 7; ++x) dg[static_cast<size_t>(z) * 7 + x] = kDressGrid[z][x] == '.';
		const Dressing dr = Dress(dg, 7, 5);
		const auto roleAt = [](const std::vector<Square>& v, int x, int z) {
			for (const Square& q : v)
				if (q.x == x && q.z == z) return static_cast<int>(q.role);
			return -1;
		};
		int roomSquares = 0;
		for (const Square& q : dr.open) roomSquares += q.role == Role::Room;
		Check("dress: the 3x3 is room, the passage is not", static_cast<double>(roomSquares), 9.0, 0.0);
		Check("...the passage is corridor", roleAt(dr.open, 5, 2) == static_cast<int>(Role::Corridor) ? 1 : 0,
			  1, 0);
		Check("...a wall touching the room wears the room's",
			  roleAt(dr.walls, 4, 1) == static_cast<int>(Role::Room) ? 1 : 0, 1, 0);
		Check("...a wall touching only the passage wears the corridor's",
			  roleAt(dr.walls, 6, 1) == static_cast<int>(Role::Corridor) ? 1 : 0, 1, 0);
		Check("...and every solid square beside an open one is a wall (20 here)",
			  static_cast<double>(dr.walls.size()), 20.0, 0.0);
	}

	// --- populate only ----------------------------------------------------------
	// Game/Generate.h's Populate (tool-refinement Phase 7): content for a level
	// that is already built. Two rooms joined by a passage, the start in the west
	// one; three kinds ranked by threat.
	{
		using namespace dungeon::game;
		std::printf("\nPopulate only (Game/Generate.h)\n");
		const char* kRows[] = {
			"########################",
			"#####################..#",
			"#....###########.......#",
			"#..................... #",
			"#....###########.......#",
			"#....###########.......#",
			"################.......#",
			"########################",
		};
		constexpr int W = 24, H = 8;
		std::vector<dungeon::u8> walk(W * H, 0);
		for (int z = 0; z < H; ++z)
			for (int x = 0; x < W; ++x) walk[static_cast<size_t>(z) * W + x] = kRows[z][x] == '.';
		generate::Params p;
		p.monsterIds = {"weak", "mid", "strong"};
		p.monsterThreat = {1.0, 2.0, 3.0};
		p.difficulty = 1.0f;
		p.density = 2.0f;
		p.ramp = 0.0f;
		p.reward = 1.0f;
		p.lootIds = {"coin"};
		p.seed = 7;
		const generate::Level a = generate::Populate(p, W, H, walk, walk, 2, 3);
		bool inStartRoom = false, nearStart = false, offFree = false;
		int monsters = 0;
		for (const Entity& e : a.entities) {
			offFree = offFree || !walk[static_cast<size_t>(e.z) * W + e.x];
			if (e.kind != EntityKind::Monster) continue;
			++monsters;
			inStartRoom = inStartRoom || e.x <= 4;
			nearStart = nearStart || std::abs(e.x - 2) + std::abs(e.z - 3) <= 3;
		}
		Check("populate places monsters in a built level", monsters > 0 ? 1 : 0, 1, 0);
		Check("...none in the start's room", inStartRoom ? 0 : 1, 1, 0);
		Check("...none within three steps of the start", nearStart ? 0 : 1, 1, 0);
		Check("...and nothing off the free squares", offFree ? 0 : 1, 1, 0);
		Check("...loot too, at reward 1", a.report.loot > 0 ? 1 : 0, 1, 0);
		const generate::Level again = generate::Populate(p, W, H, walk, walk, 2, 3);
		bool same = again.entities.size() == a.entities.size();
		for (size_t i = 0; same && i < a.entities.size(); ++i)
			same = again.entities[i].type == a.entities[i].type && again.entities[i].x == a.entities[i].x &&
				   again.entities[i].z == a.entities[i].z;
		Check("the same knobs and seed populate the same way", same ? 1 : 0, 1, 0);
		// A zero weight takes a kind out of the choice near the target rank.
		generate::Params pw = p;
		pw.monsterWeight = {1.0f, 1.0f, 0.0f};
		const generate::Level w = generate::Populate(pw, W, H, walk, walk, 2, 3);
		bool onlyMid = true;
		for (const Entity& e : w.entities)
			if (e.kind == EntityKind::Monster) onlyMid = onlyMid && e.type == "mid";
		Check("a weight of 0 leaves a kind out (difficulty 1: all 'mid')", onlyMid ? 1 : 0, 1, 0);
		// The boss: the strongest, in the deepest room, whatever its weight.
		pw.boss = true;
		pw.density = 0.0f;
		const generate::Level b = generate::Populate(pw, W, H, walk, walk, 2, 3);
		Check("the boss stands in the far room",
			  b.report.bossPlaced && b.entities.size() >= 1 && b.entities.front().type == "strong" &&
					  b.entities.front().x >= 16
				  ? 1
				  : 0,
			  1, 0);
		// Squares that are not free stay empty.
		std::vector<dungeon::u8> none(W * H, 0);
		const generate::Level n = generate::Populate(p, W, H, walk, none, 2, 3);
		Check("no free square, no content", static_cast<double>(n.entities.size()), 0.0, 0.0);
		// A level of passages only still gets its monsters.
		std::vector<dungeon::u8> line(W * H, 0);
		for (int x = 1; x < W - 1; ++x) line[static_cast<size_t>(3) * W + x] = 1;
		const generate::Level c = generate::Populate(p, W, H, line, line, 1, 3);
		int lineMonsters = 0;
		for (const Entity& e : c.entities) lineMonsters += e.kind == EntityKind::Monster;
		Check("a level with no room is populated along its passages", lineMonsters > 0 ? 1 : 0, 1, 0);
		// A two-wide passage is ROOM by the 2x2 rule, so it joins the far room to
		// the start's: one room holding the start, and nothing else to fill. The
		// margin is then the rule, and the far end still gets its monsters.
		std::vector<dungeon::u8> joined = walk;
		for (int x = 5; x <= 15; ++x) joined[static_cast<size_t>(4) * W + x] = 1;
		const generate::Level j = generate::Populate(p, W, H, joined, joined, 2, 3);
		int joinedMonsters = 0;
		bool joinedNear = false;
		for (const Entity& e : j.entities)
			if (e.kind == EntityKind::Monster) {
				++joinedMonsters;
				joinedNear = joinedNear || std::abs(e.x - 2) + std::abs(e.z - 3) <= 3;
			}
		Check("a level that is all the start's room is still populated, past three steps",
			  joinedMonsters > 0 && !joinedNear ? 1 : 0, 1, 0);
		// A density too low to roll anyone still meets someone - and 0 means none.
		generate::Params low = p;
		low.density = 0.01f;
		low.lootIds.clear();
		const generate::Level lo = generate::Populate(low, W, H, walk, walk, 2, 3);
		Check("a low density still places one monster", static_cast<double>(lo.report.monsters), 1.0, 0.0);
		low.density = 0.0f;
		Check("...and density 0 places none",
			  static_cast<double>(generate::Populate(low, W, H, walk, walk, 2, 3).report.monsters), 0.0, 0.0);
	}

	// --- party creation (Game/PartyRules.h) ---------------------------------
	// The numbers a created member is made from. Expectations are written out
	// by hand from the rules in docs/party-creation-plan.md, never read back
	// from the header's constants.
	{
		std::printf("\nParty creation (Game/PartyRules.h)\n");
		using namespace party;
		const RaceStats human{{0, 0, 0, 0, 0}, 2};
		const RaceStats elf{{-1, 2, -2, 0, 1}, 0};
		const RaceStats orc{{3, 0, 1, -2, -2}, 0};
		Check("a human has 7 points (5 + 2 extra)", PointBudget(human), 7, 0);
		Check("an elf has 5", PointBudget(elf), 5, 0);
		const StatArray none{};
		Check("an unspent elf's DEX is 12", StatValue(elf, none, 1), 12, 0);
		Check("an unspent elf's VIT is 8", StatValue(elf, none, 2), 8, 0);
		const StatArray spent{0, 2, 0, 3, 0};
		const StatArray aria = Stats(elf, spent);
		CheckTrue("elf + 0,2,0,3,0 is 9,14,8,13,11",
				  aria == StatArray{9, 14, 8, 13, 11});
		CheckTrue("5 spent of an elf's 5 is valid", SpendingValid(elf, spent));
		CheckTrue("...and no more can be spent", !CanSpend(elf, spent));
		CheckTrue("6 spent is not valid", !SpendingValid(elf, StatArray{1, 2, 0, 3, 0}));
		CheckTrue("a negative spend is not valid", !SpendingValid(human, StatArray{-1, 0, 0, 0, 0}));
		CheckTrue("a human can still spend after 5", CanSpend(human, StatArray{5, 0, 0, 0, 0}));
		CheckTrue("a spent point can come back", CanRefund(spent, 1));
		CheckTrue("an unspent one cannot", !CanRefund(spent, 0));
		// The floor: a race that took 9 from a stat would leave 1; it stops at 3.
		const RaceStats harsh{{0, 0, 0, 0, -9}, 0};
		Check("a stat never falls below 3", StatValue(harsh, none, 4), 3, 0);
		Check("an orc's INT is 8", StatValue(orc, none, 4), 8, 0);
		Check("a boosted skill is level 2, xp 4", kSkillBoostXp, 4.0, 0.0);
		CheckTrue("two different skills are a valid pick",
				  SkillPicksValid({"blade", "conditioning"}));
		CheckTrue("the same skill twice is not", !SkillPicksValid({"blade", "blade"}));
		CheckTrue("three skills are too many", !SkillPicksValid({"a", "b", "c"}));
		CheckTrue("'Old Tom' is a name", NameValid("Old Tom"));
		CheckTrue("an underscore is not (saves use it for spaces)", !NameValid("Old_Tom"));
		CheckTrue("all spaces is not a name", !NameValid("   "));
		CheckTrue("17 characters is too long", !NameValid("Abcdefghijklmnopq"));

		// The member WORDS `newparty` and the page's `partypage set` share.
		MemberSpec m;
		std::string why;
		CheckTrue("name=Old_Tom applies", ApplySpecField(m, "name", "Old_Tom", why));
		CheckTrue("...as 'Old Tom' (underscores are spaces)", m.name == "Old Tom");
		CheckTrue("color=c04040 applies", ApplySpecField(m, "color", "c04040", why));
		Check("...red 0xc0 is 0.753", m.color[0], 0.7529, 0.001);
		CheckTrue("...and marks the colour as set", m.colorSet);
		CheckTrue("color=red is refused", !ApplySpecField(m, "color", "red", why));
		CheckTrue("points= with four numbers is refused",
				  !ApplySpecField(m, "points", "1,2,3,4", why));
		CheckTrue("points=2,0,3,0,0 applies", ApplySpecField(m, "points", "2,0,3,0,0", why));
		CheckTrue("...into the five stats in order", m.spent == StatArray{2, 0, 3, 0, 0});
		CheckTrue("skills=blade,,conditioning drops the empty part",
				  ApplySpecField(m, "skills", "blade,,conditioning", why) &&
					  m.skills == std::vector<std::string>{"blade", "conditioning"});
		CheckTrue("an unknown key is refused", !ApplySpecField(m, "class", "mage", why));
	}

	// --- light profiles (Game/LightProfile.h) ---------------------------------
	// A light's look is parsed from lights.cat and its pulse is maths every
	// light goes through. Phase 2 moved the hand-written fire flicker, the rune
	// breath and the fires' wander into it, so the expectations below are the
	// OLD formulas written out by hand: the move must not have changed a light.
	{
		std::printf("\nLight profiles (Game/LightProfile.h)\n");
		const auto fields = [](std::vector<std::pair<std::string, std::string>> kv) {
			return [kv](std::string_view key) -> std::string {
				for (const auto& [k, v] : kv)
					if (k == key) return v;
				return {};
			};
		};
		std::vector<std::string> problems;
		const light::Profile p = light::Parse(
			"brazier",
			fields({{"color", "1.0, 0.527, 0.224"}, {"intensity", "2.3"}, {"radius", "6"},
					{"pulse", "flicker"}, {"pulse_depth", "0.1"}, {"wander", "0.0168"},
					{"shadow", "1"}, {"long_fade", "1"}}),
			&problems);
		CheckTrue("a full profile parses with no problems", problems.empty());
		Check("...its colour's green is 0.527", p.color.y, 0.527, 1e-6);
		Check("...intensity 2.3", p.intensity, 2.3, 1e-6);
		Check("...radius 6 squares", p.radius, 6.0, 1e-6);
		CheckTrue("...flickers, casts a shadow, fades long",
				  p.pulse == light::Pulse::Flicker && p.shadow && p.longFade);
		const light::Profile src = light::Parse("burning", fields({{"color", "source"}}));
		CheckTrue("`color = source` takes the source's colour", src.sourceColor);
		problems.clear();
		const light::Profile typo = light::Parse(
			"typo", fields({{"color", "1, 0.5"}, {"pulse", "wobble"}, {"radius", "far"}}), &problems);
		Check("three unreadable fields are three problems", static_cast<double>(problems.size()), 3, 0);
		CheckTrue("...and each keeps its default",
				  typo.radius == light::Profile{}.radius && typo.pulse == light::Pulse::Steady &&
					  typo.color.x == light::Profile{}.color.x);

		// The pulses, against the formulas they replaced.
		double worstFlicker = 0.0, worstBreath = 0.0, worstWander = 0.0;
		double flickerMin = 9.0, flickerMax = -9.0;
		int strobeFull = 0, stormHigh = 0;
		const int kSamples = 20000;
		for (int i = 0; i < kSamples; ++i) {
			const float t = static_cast<float>(i) * 0.0137f;
			const float ph = 3.4f; // a fire's phase (seed 2 x 1.7)
			const float f = light::PulseAt(light::Pulse::Flicker, 1.0f, 0.1f, t, ph);
			const float old = 0.9f + 0.1f * std::sin(t * 11.0f + ph) * std::sin(t * 7.3f + ph);
			worstFlicker = std::max(worstFlicker, static_cast<double>(std::fabs(f - old)));
			flickerMin = std::min(flickerMin, static_cast<double>(f));
			flickerMax = std::max(flickerMax, static_cast<double>(f));
			// A rune's light: 2.3 x (1.05 + 0.85 sin(3t + id)), now 2.415 x breathe 0.81.
			const float breath = 2.415f * light::PulseAt(light::Pulse::Breathe, 1.0f, 0.81f, t, 7.0f);
			const float oldBreath = 2.3f * (1.05f + 0.85f * std::sin(t * 3.0f + 7.0f));
			worstBreath = std::max(worstBreath, static_cast<double>(std::fabs(breath - oldBreath)));
			// A brazier's wander was 0.042 m; 0.0168 squares x 2.5 m is the same.
			const dungeon::Vec3 w = light::WanderAt(0.0168f, t, ph);
			const float oldX = 0.042f * std::sin(t * 7.3f + ph) * std::sin(t * 3.1f + ph * 2.0f);
			worstWander = std::max(worstWander, static_cast<double>(std::fabs(w.x * 2.5f - oldX)));
			if (light::PulseAt(light::Pulse::Strobe, 2.0f, 0.8f, t, 0.0f) > 0.99f) ++strobeFull;
			if (light::PulseAt(light::Pulse::Storm, 1.0f, 0.8f, t, 1.0f) > 0.9f) ++stormHigh;
		}
		Check("flicker is the old fire flicker, exactly", worstFlicker, 0.0, 1e-5);
		CheckTrue("...and stays within 0.8 .. 1.0", flickerMin >= 0.8 - 1e-5 && flickerMax <= 1.0 + 1e-5);
		Check("a rune's breath is the old one (worst gap)", worstBreath, 0.0, 0.01);
		Check("a brazier's wander is the old 0.042 m (worst gap, m)", worstWander, 0.0, 1e-5);
		Check("a strobe is full 15% of the time",
			  static_cast<double>(strobeFull) / kSamples, 0.15, 0.01);
		const double stormShare = static_cast<double>(stormHigh) / kSamples;
		CheckTrue("a storm flashes, but rarely (under 10% of the time)",
				  stormShare > 0.0 && stormShare < 0.10);
		Check("steady never moves",
			  light::PulseAt(light::Pulse::Steady, 3.0f, 0.9f, 12.3f, 1.0f), 1.0, 0.0);
		const dungeon::Vec3 still = light::WanderAt(0.0f, 5.0f, 1.0f);
		CheckTrue("no wander, no movement", still.x == 0.0f && still.y == 0.0f && still.z == 0.0f);
		light::Pulse parsed{};
		CheckTrue("every pulse name round-trips",
				  light::ParsePulse(light::PulseName(light::Pulse::Storm), parsed) &&
					  parsed == light::Pulse::Storm);
		CheckTrue("the fallback is a light, not darkness", light::Fallback().intensity > 0.0f);
	}

	// --- trails (Game/Trail.h) -------------------------------------------------
	// What a thing in flight sheds, parsed from trails.cat: the shape sets the
	// defaults, a field overrides one, a typo keeps its default and says so.
	{
		std::printf("\nTrails (Game/Trail.h)\n");
		const auto fields = [](std::vector<std::pair<std::string, std::string>> kv) {
			return [kv](std::string_view key) -> std::string {
				for (const auto& [k, v] : kv)
					if (k == key) return v;
				return {};
			};
		};
		namespace trail = dungeon::game::trail;
		std::vector<std::string> problems;
		const trail::Profile ember =
			trail::Parse("trail_fire", fields({{"shape", "ember"}, {"rate", "12"}}), &problems);
		CheckTrue("an ember trail parses with no problems", problems.empty());
		const trail::Spec emberDefaults = trail::ShapeDefaults(trail::Shape::Ember);
		CheckTrue("...takes the ember's defaults: it rises and flickers",
				  ember.spec.fall < 0.0f && ember.spec.flicker > 0.0f &&
					  ember.spec.life == emberDefaults.life);
		Check("...at 12 a square", ember.spec.rate, 12.0, 1e-6);
		CheckTrue("...and the colour of its light", !ember.spec.hasColor);
		const trail::Profile grit = trail::Parse(
			"trail_earth", fields({{"shape", "drip"}, {"rate", "8"}, {"fall", "9"},
								   {"color", "0.5, 0.4, 0.3"}}));
		Check("a field overrides its shape's default (fall 9)", grit.spec.fall, 9.0, 1e-6);
		CheckTrue("...and a colour of its own is its own",
				  grit.spec.hasColor && grit.spec.color.y == 0.4f);
		CheckTrue("a drip falls, a mote swirls, a puff swells",
				  trail::ShapeDefaults(trail::Shape::Drip).fall > 0.0f &&
					  trail::ShapeDefaults(trail::Shape::Mote).swirl > 0.0f &&
					  trail::ShapeDefaults(trail::Shape::Puff).swell);
		CheckTrue("no rate, no trail (the catalog must say how dense)",
				  !trail::Parse("bare", fields({{"shape", "mote"}})).spec.Any());
		problems.clear();
		const trail::Profile typo = trail::Parse(
			"typo", fields({{"shape", "comet"}, {"rate", "lots"}, {"life", "0"}}), &problems);
		Check("two unreadable fields are two problems", static_cast<double>(problems.size()), 2, 0);
		CheckTrue("...a bad shape is a spark, a bad rate none",
				  typo.spec.shape == trail::Shape::Spark && !typo.spec.Any());
		CheckTrue("...and no life is floored (the fade divides by it)", typo.spec.life >= 0.05f);
		trail::Shape shape{};
		CheckTrue("every shape name round-trips",
				  trail::ParseShape(trail::ShapeName(trail::Shape::Drip), shape) &&
					  shape == trail::Shape::Drip);
	}

	// --- tiled light lists (Graphics/LightTiles.h) ----------------------------
	// The scene shader shades a pixel with ONLY the lights its tile's mask
	// names, so the one promise that matters is that a light is never missing
	// from a tile it reaches. Checked by sampling points inside random spheres
	// and asking whether the tile each lands in has that sphere's bit.
	{
		std::printf("\nTiled light lists (Graphics/LightTiles.h)\n");
		using namespace DirectX;
		namespace gfx = dungeon::gfx;
		using dungeon::u64;
		const XMMATRIX view = XMMatrixLookToLH(XMVectorSet(3.0f, 1.6f, 2.0f, 1.0f),
											   XMVectorSet(0.3f, -0.1f, 1.0f, 0.0f),
											   XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		const XMMATRIX proj = XMMatrixPerspectiveFovLH(1.1f, 16.0f / 9.0f, 0.05f, 100.0f);
		dungeon::Mat4 vp;
		XMStoreFloat4x4(&vp, view * proj);
		const auto project = [&](const dungeon::Vec3& p, float& nx, float& ny) {
			const float cx = p.x * vp._11 + p.y * vp._21 + p.z * vp._31 + vp._41;
			const float cy = p.x * vp._12 + p.y * vp._22 + p.z * vp._32 + vp._42;
			const float cw = p.x * vp._14 + p.y * vp._24 + p.z * vp._34 + vp._44;
			if (cw <= 0.05f) return false;
			nx = cx / cw;
			ny = cy / cw;
			return nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f;
		};
		const gfx::LightTiler tiler(vp);
		gfx::TileRange t;
		CheckTrue("a light ahead is on screen", tiler.Range({3.5f, 1.5f, 8.0f}, 1.0f, t));
		CheckTrue("...and covers only part of it",
				  (t.c1 - t.c0 + 1) * (t.r1 - t.r0 + 1) < static_cast<int>(gfx::kLightTileCount) / 4);
		CheckTrue("a light wholly behind the eye is not",
				  !tiler.Range({2.0f, 1.6f, -6.0f}, 1.0f, t));
		CheckTrue("a light far off to the side is not", !tiler.Range({40.0f, 1.6f, 4.0f}, 1.0f, t));
		const bool around = tiler.Range({3.0f, 1.6f, 2.5f}, 2.0f, t);
		CheckTrue("a light round the eye covers the whole screen",
				  around && t.c0 == 0 && t.r0 == 0 && t.c1 == static_cast<int>(gfx::kLightTilesX) - 1 &&
					  t.r1 == static_cast<int>(gfx::kLightTilesY) - 1);
		// Beside the eye but not round it - the case the old box-corner test
		// called "the whole screen" because its box straddled the eye's plane.
		const bool beside = tiler.Range({1.0f, 1.6f, 2.2f}, 1.5f, t);
		CheckTrue("a light beside the eye covers only its side",
				  beside && (t.c1 - t.c0 + 1) < static_cast<int>(gfx::kLightTilesX));
		// The GAME's camera (Graphics/Camera.h), which mirrors clip-space X to
		// un-mirror its left-handed view: crypt1's brazier, 5 m ahead with a 15 m
		// reach, seen from the square the party stands on.
		{
			gfx::Camera cam;
			cam.SetPosition({7.5f * 2.5f, 1.6f, 4.5f * 2.5f});
			cam.SetYawPitch(dungeon::kPi * 0.5f, 0.0f); // east
			cam.SetLens(60.0f * dungeon::kPi / 180.0f, 16.0f / 9.0f, 0.05f, 100.0f);
			const gfx::LightTiler game(cam.ViewProj());
			const bool brazier = game.Range({9.5f * 2.5f, 0.9f, 4.5f * 2.5f}, 15.0f, t);
			CheckTrue("the game camera: a brazier round the eye covers the screen",
					  brazier && t.c0 == 0 && t.r0 == 0 &&
						  t.c1 == static_cast<int>(gfx::kLightTilesX) - 1 &&
						  t.r1 == static_cast<int>(gfx::kLightTilesY) - 1);
			const bool ahead = game.Range({12.0f * 2.5f, 1.0f, 4.5f * 2.5f}, 1.0f, t);
			CheckTrue("the game camera: a small light straight ahead is mid-screen",
					  ahead && t.c0 <= 16 && t.c1 >= 15 && t.r0 <= 9 && t.r1 >= 8);
		}
		Check("the top-left corner is tile 0", gfx::TileOf(-1.0f, 1.0f), 0, 0);
		Check("the bottom-right corner is the last tile", gfx::TileOf(1.0f, -1.0f),
			  gfx::kLightTileCount - 1, 0);

		std::mt19937 rng(0x7115u);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		std::vector<gfx::LightSphere> spheres(64);
		std::vector<u64> masks(gfx::kLightTileCount);
		long missing = 0, samples = 0, setBits = 0;
		for (int trial = 0; trial < 40; ++trial) {
			for (gfx::LightSphere& s : spheres)
				s = {{3.0f + (u(rng) - 0.5f) * 24.0f, u(rng) * 3.0f, 2.0f + (u(rng) - 0.4f) * 30.0f},
					 0.3f + u(rng) * 6.0f};
			gfx::BinLights(vp, spheres, masks);
			for (u64 m : masks) setBits += std::popcount(m);
			for (size_t i = 0; i < spheres.size(); ++i)
				for (int k = 0; k < 60; ++k) {
					// A point inside the sphere (rejection-sampled in its cube).
					dungeon::Vec3 off{u(rng) * 2.0f - 1.0f, u(rng) * 2.0f - 1.0f, u(rng) * 2.0f - 1.0f};
					if (off.x * off.x + off.y * off.y + off.z * off.z > 1.0f) continue;
					const dungeon::Vec3 p{spheres[i].center.x + off.x * spheres[i].radius,
										  spheres[i].center.y + off.y * spheres[i].radius,
										  spheres[i].center.z + off.z * spheres[i].radius};
					float nx = 0.0f, ny = 0.0f;
					if (!project(p, nx, ny)) continue; // not a visible pixel
					++samples;
					if (!(masks[gfx::TileOf(nx, ny)] & (u64{1} << i))) ++missing;
				}
		}
		CheckTrue("thousands of points sampled inside lights", samples > 10000);
		Check("a light is never missing from a tile it reaches", static_cast<double>(missing), 0, 0);
		// Informational: how much of the full cost the tiles keep (all 64 lights
		// in all 576 tiles = 1.0). Scattered lights cover a fraction of the view.
		std::printf("  (tiles keep %.1f%% of the untiled loop for 64 scattered lights)\n",
					100.0 * static_cast<double>(setBits) / (40.0 * 64.0 * gfx::kLightTileCount));
	}

	// --- verdict ------------------------------------------------------------
	const bool pass = (g_failed == 0);
	std::printf("\n%s — %d checks, %d failed\n", pass ? "PASS" : "FAIL",
				g_checks, g_failed);

	if (selfTest) {
		// The harness must CATCH the injected fault. A clean run here means
		// the checks are vacuous and the whole file is worthless.
		const bool caught = !pass;
		std::printf("SELF-TEST %s — a broken die %s caught\n",
					caught ? "PASS" : "FAIL", caught ? "was" : "was NOT");
		return caught ? 0 : 1;
	}
	return pass ? 0 : 1;
}
