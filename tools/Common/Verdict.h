// ============================================================================
// tools/Common/Verdict.h - the native judges' tally and their last line.
//
// RollTest, DiagTest, ThreadStress and Bc7Test each carried their own copy of
// the same three things (code-review C425): a check counter, the self-test's
// exact-failures comparison (the tools/SpellTest.py rule) and a verdict line -
// and two of the four printed that line in a format of their own. This is the
// one copy. A tool still prints its check rows its own way (RollTest's carry a
// measurement beside its expectation); only the TALLY goes through here.
//
// THE LAST LINE, in both modes:
//   <tool> RESULT=PASS|FAIL checks=N failures=M [fields] self_test=0
//   <tool> RESULT=PASS|FAIL checks=N failures=M [fields] self_test=1 caught=0|1
// RESULT is the CHECKS' verdict either way, so a self-test reads RESULT=FAIL -
// the fault was given, and the checks failed on it - while `caught` says whether
// exactly the expected checks were the ones that did. The exit code is 0 for a
// PASS, or under a self-test for caught=1. tools\CheckAll.ps1 reads this line
// back (tools\Verdict.ps1) and fails a run whose last line is missing, malformed
// or disagrees with the exit code.
//
// Header-only and Core-free, so a tool includes it with nothing to link.
// ============================================================================
#pragma once

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::verdict {

// The run's tally. One per process: each tool is a single run.
struct Tally {
	int checks = 0;
	int failures = 0;
	// The label of every check that failed, in order - what a self-test compares
	// against its expected list.
	std::vector<std::string> failedLabels;
};

inline Tally& Totals() {
	static Tally tally;
	return tally;
}

// Counts one check and returns `ok`; the caller prints its own row.
inline bool Count(bool ok, std::string_view what) {
	Tally& t = Totals();
	++t.checks;
	if (!ok) {
		++t.failures;
		t.failedLabels.emplace_back(what);
	}
	return ok;
}

// Counts one check and prints the plain row: `<indent>[ok  ] what` / `[FAIL]`.
inline bool Check(bool ok, std::string_view what, const char* indent = "  ") {
	Count(ok, what);
	std::printf("%s[%s] %.*s\n", indent, ok ? "ok  " : "FAIL",
				static_cast<int>(what.size()), what.data());
	return ok;
}

// THE SELF-TEST RULE: exactly the checks in `expected` failed, and every other
// check passed. "Anything failed" passes a broken run as readily as a caught
// fault, and cannot say WHERE the fault was caught. Compared as sorted
// multisets, so a check that runs twice under one label is listed twice. Prints
// every mismatch by name and a one-line summary; returns whether it was caught.
//
// `otherMisses` counts a tool's OWN self-test conditions that did not hold
// (Bc7Test's raised baseline, which must be caught on every image, not merely
// once). The caller prints those by name BEFORE calling this, and any one of
// them fails the self-test here - so the summary is always the whole
// self-test's verdict and never reads PASS above a run that then says caught=0.
inline bool CaughtExactly(std::span<const char* const> expected, int otherMisses = 0) {
	std::vector<std::string> want(expected.begin(), expected.end());
	std::vector<std::string> got = Totals().failedLabels;
	std::sort(want.begin(), want.end());
	std::sort(got.begin(), got.end());
	std::vector<std::string> unexpected, missed;
	std::set_difference(got.begin(), got.end(), want.begin(), want.end(),
						std::back_inserter(unexpected));
	std::set_difference(want.begin(), want.end(), got.begin(), got.end(),
						std::back_inserter(missed));
	for (const std::string& s : unexpected)
		std::printf("  self-test: '%s' FAILED but is not an expected failure\n", s.c_str());
	for (const std::string& s : missed)
		std::printf("  self-test: '%s' was expected to FAIL and passed\n", s.c_str());
	const bool caught = unexpected.empty() && missed.empty() && otherMisses == 0;
	std::printf("SELF-TEST %s - %d of %d expected failures, %d unexpected",
				caught ? "PASS" : "FAIL", static_cast<int>(want.size() - missed.size()),
				static_cast<int>(want.size()), static_cast<int>(unexpected.size()));
	if (otherMisses > 0) std::printf(", %d other miss%s (named above)", otherMisses,
									 otherMisses == 1 ? "" : "es");
	std::printf("\n");
	return caught;
}

// Prints the last line and returns the process exit code. `fields` sits between
// failures= and self_test=: empty, or " name=value ..." with its leading space.
// `caught` is read only under a self-test.
inline int Finish(const char* tool, bool selfTest, bool caught,
				  std::string_view fields = {}) {
	const Tally& t = Totals();
	std::printf("%s RESULT=%s checks=%d failures=%d%.*s self_test=%d", tool,
				t.failures == 0 ? "PASS" : "FAIL", t.checks, t.failures,
				static_cast<int>(fields.size()), fields.data(), selfTest ? 1 : 0);
	if (selfTest) std::printf(" caught=%d", caught ? 1 : 0);
	std::printf("\n");
	std::fflush(stdout);
	if (selfTest) return caught ? 0 : 1;
	return t.failures == 0 ? 0 : 1;
}

} // namespace dungeon::verdict
