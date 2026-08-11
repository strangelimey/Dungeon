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
