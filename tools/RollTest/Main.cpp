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
//   RollTest.exe [--self-test]   — one verdict line, exit 0 = PASS
//
// --self-test feeds the checks a 90-sided die while they still expect 100, so
// a harness that cannot catch a broken distribution FAILS instead of passing
// vacuously.
// ============================================================================
#include "Game/Combat.h"
#include "Game/Curve.h"
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
			struct Def { const char* what; float bonus; float soak; };
			const Def defs[] = {
				{"fresh, unarmored (DEX 11)", kBase + CurveValue(11, stat), 0.0f},
				{"trained dodger (avoid 20)",
				 kBase + CurveValue(11, stat) + CurveValue(20, avoid), 0.0f},
				{"veteran dodger (avoid 60)",
				 kBase + CurveValue(11, stat) + CurveValue(60, avoid), 0.0f},
				{"brigandine, untrained", kBase + CurveValue(11, stat) - armorPenalty(25, 10, 0), 3.5f},
				{"brigandine, skill 20", kBase + CurveValue(11, stat) - armorPenalty(25, 10, 20), 3.5f},
				{"plate, untrained", kBase + CurveValue(11, stat) - armorPenalty(45, 20, 0), 7.0f},
				{"plate, skill 30", kBase + CurveValue(11, stat) - armorPenalty(45, 20, 30), 7.0f},
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
					const AttackResult r = ResolveAttack({6.0f, kMonster, {}},
														 {d.bonus, d.soak, 0.0f},
														 sr, rng);
					if (!r.hit) continue;
					++hits;
					total += r.damage;
				}
				std::printf("    %-30s %6.0f %6.3f %8.1f %9.2f\n", d.what, d.bonus,
							double(hits) / kN, d.soak, total / kN);
			}
			std::printf("    (monster damage 6, no resists; \"dmg/swing\" averages "
						"misses in — what the fight actually costs)\n");
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
