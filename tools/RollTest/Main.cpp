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
#include "Game/Defense.h"
#include "Game/Mishap.h"
#include "Game/Resource.h"
#include "Game/Roll.h"

#include <algorithm>
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

	// --- the stance couples both sides ------------------------------------------
	// docs/damage-system.md "The stance" + "Over-exertion". One number moves the
	// attack and the guard together: the points the share takes off one it puts
	// onto the other. Over-exertion is that same line continued past 1, bought
	// with stamina and then hide.
	{
		std::printf("\n--- the stance couples both sides ---\n");
		CurveRules skillCurve;
		skillCurve.form = CurveForm::Hyperbolic;
		skillCurve.slope = 5.0f;
		skillCurve.cap = 120.0f;
		const float lvl = 20.0f;
		const double full = CurveValue(lvl, skillCurve);

		Check("a full commitment is the plain curve value",
			  defense::StanceAttack(1.0f, lvl, skillCurve), full, 0.001);
		Check("half the share puts half the skill behind the swing",
			  defense::StanceAttack(0.5f, lvl, skillCurve), full * 0.5, 0.001);
		Check("guarding with everything attacks with nothing",
			  defense::StanceAttack(0.0f, lvl, skillCurve), 0.0, 0.0);

		// THE COUPLING ITSELF, which is the whole point of the change: what the
		// share adds to the attack is exactly what it takes off the guard. Checked
		// as an identity across several shares rather than at one point, so a
		// factor slipped into one side alone cannot pass.
		bool coupled = true;
		for (const float share : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f}) {
			const double attack = defense::StanceAttack(share, lvl, skillCurve);
			const double guard =
				defense::HandGuard(1.0f - share, skillCurve, lvl, lvl);
			if (std::abs((attack + guard) - full) > 0.001) coupled = false;
		}
		CheckTrue("attack + guard is constant across every share", coupled);

		// --- what over-exertion BUYS -----------------------------------------
		Check("an honest stance buys nothing",
			  defense::ExertionPoints(1.0f, lvl, skillCurve), 0.0, 0.0);
		Check("a defensive stance buys nothing either",
			  defense::ExertionPoints(0.3f, lvl, skillCurve), 0.0, 0.0);
		Check("half again buys half a skill's worth",
			  defense::ExertionPoints(1.5f, lvl, skillCurve), full * 0.5, 0.001);
		Check("double buys a whole second skill's worth",
			  defense::ExertionPoints(2.0f, lvl, skillCurve), full, 0.001);
		// The points bought are exactly the attack ABOVE an honest full swing —
		// stated against StanceAttack rather than re-derived, because the bill is
		// charged against this number and the two must not drift apart.
		Check("the points bought are the attack past a full commitment",
			  defense::ExertionPoints(1.7f, lvl, skillCurve),
			  defense::StanceAttack(1.7f, lvl, skillCurve) -
				  defense::StanceAttack(1.0f, lvl, skillCurve),
			  0.001);
		// A skill you do not have cannot be over-spent: there is nothing to
		// borrow, so an untrained fighter's reckless stance is free AND useless.
		// Non-vacuous by pairing — the same share on a trained fighter is not.
		Check("an untrained fighter borrows nothing",
			  defense::ExertionPoints(2.0f, 0.0f, skillCurve), 0.0, 0.001);
		CheckTrue("...while a trained one borrows plenty",
				  defense::ExertionPoints(2.0f, lvl, skillCurve) > 1.0f);
		CheckTrue("a deeper skill borrows more at the same share",
				  defense::ExertionPoints(1.5f, 60.0f, skillCurve) >
					  defense::ExertionPoints(1.5f, lvl, skillCurve));
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
			const std::vector<Entry> mild = DefaultFumble(2.2f);
			Check("the default fumble is one consequence",
				  static_cast<double>(mild.size()), 1.0, 0.0);
			CheckTrue("...and it is tempo, not damage",
					  !mild.empty() && mild[0].kind == Kind::Recover);
			Check("...carrying the knob it was given",
				  mild.empty() ? 0.0 : mild[0].value, 2.2, 0.001);
			const std::vector<Entry> bad = DefaultSevere();
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

		std::printf("  (the ORDERING the model asks for — stamina > mana > health\n"
					"   at equal investment — is a property of the AUTHORED knobs,\n"
					"   not of this arithmetic, so the eval harness checks it)\n");
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
