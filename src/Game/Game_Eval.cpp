// ============================================================================
// Game_Eval.cpp — the eval harness's half of Game (docs/eval-harness.md).
//
// The batch script runner, the sim-time clock it drives the world with, and the
// world recycling that makes a long run affordable. Split out for the reason
// every other Game_*.cpp was: Game.cpp is the app state machine plus wiring, and
// this is a subsystem that happens to hang off it.
//
// IT IS NOT COMPILED OUT, and that is a decision rather than an oversight
// (Michael, 2026-08-15, reviewing exactly this question). Behind `#ifdef DN_EVAL`
// all of this would exist only in a build nobody ships — and the harness's whole
// value is that it measures the SHIPPING binary, which is the same rule
// tools/RollTest's CMakeLists states for the roll engine: the real thing linked
// straight in, never a copy. It would also add a fourth build configuration to a
// project that already runs `build-release` and `build-profile` checks precisely
// because a configuration nobody builds rots.
//
// What it costs when nobody is running a script: PumpEvalScript returns on its
// first line, and StepWorld and ResetForEval are simply never called.
// ============================================================================
#include "Core/Log.h"
#include "Game/Game.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace dungeon::game {

// PUT THE GAME WHERE `newgame` WOULD, WITHOUT THE LEVEL LOAD — the world
// recycling a long run rests on (~340 ms against ~12 s).
bool Game::ResetForEval() {
	if (!m_gameLoaded) {
		// THE FIRST TEST IN A BATCH: nothing to recycle, so run a real new game
		// — THROUGH THE UI CALLBACK, not StartNewGame(). From a cold boot the HUD
		// has never been built (it is a load task), and setting AppState::Playing
		// with no widgets crashed the very first unattended eval run. The
		// callback is where the "already loaded, or load first?" decision lives.
		// (This function reached for StartNewGame first and re-introduced it; the
		// lesson is docs/eval-harness.md P2's — a dev path that duplicates a UI
		// action drifts from it.)
		if (!m_ui.onStartNewGame) return false;
		m_ui.onStartNewGame();
		return true;
	}
	m_world.ResetForEval();
	ResetRoster();  // fresh members, keeping each slot's loaded portrait
	m_ui.RefreshSheet();
	m_ui.ClearLog();
	ApplyPartySpeed();
	m_ui.ResetHudStatus(); // the compass/position labels re-derive next frame
	// A wipe left the app on the TITLE SCREEN, and dev commands answer perfectly
	// normally from there — so a reset that did not come back to Playing would
	// hand the next test a world that never simulates, reported as clean rungs.
	m_state = AppState::Playing;
	return true;
}

int Game::StepWorld(float seconds, StepStop& why) {
	why = StepStop::Complete;
	if (m_state != AppState::Playing || seconds <= 0.0f) {
		why = StepStop::NotPlaying;
		return 0;
	}
	// The eval's tick. 60Hz because that is the rate the game's timers were
	// tuned against, not because anything requires it — but it is FIXED, and
	// that is what makes two runs of the same script comparable.
	constexpr float kTick = 1.0f / kStepTicksPerSecond;
	const int want = static_cast<int>(seconds / kTick + 0.5f);
	const int steps = want < kMaxStepTicks ? want : kMaxStepTicks;
	// SAID, not left to be inferred from two numbers the caller must subtract.
	if (steps < want) why = StepStop::Ceiling;
	static const Input kNoInput; // no keys, no mouse: the script is driving
	// A step that BEGINS while resting ENDS when the rest does — because that is
	// what the player does, and because it is the only way to measure what a
	// rest actually cost. Rest stops itself the moment there is nothing left to
	// gain, but `step` would keep the world running past that point, and the
	// supplies drained by the overshoot are indistinguishable in the table from
	// the supplies the rest itself spent.
	//
	// Only when it STARTS resting: an ordinary step must not acquire a new way
	// to end early, or every other measurement in the harness changes meaning.
	const bool restingStep = m_world.Resting();
	int ran = 0;
	for (; ran < steps; ++ran) {
		m_world.Update(kNoInput, kTick, m_time, /*acceptInput=*/false);
		m_time += kTick; // lights, flicker and rune pulses ride sim time too
		// Followed nowhere: see the header. Reported by the caller as a short run.
		if (m_world.ConsumeLevelTransition()) {
			++ran;
			why = StepStop::LevelChange;
			break;
		}
		if (restingStep && !m_world.Resting()) {
			++ran;
			why = StepStop::RestEnded;
			break;
		}
	}
	return ran;
}

bool Game::ReadEvalLines(const std::string& path, std::vector<std::string>& out) {
	std::ifstream in(path);
	if (!in) {
		log::Error("eval: cannot open script '{}'", path);
		return false;
	}
	std::string line;
	while (std::getline(in, line)) {
		// Strip a trailing CR (a script edited on Windows and read as text can
		// still carry one through some editors), then comments, then whitespace.
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const size_t hash = line.find_first_of(";#");
		if (hash != std::string::npos) line.erase(hash);
		const size_t from = line.find_first_not_of(" \t");
		if (from == std::string::npos) continue; // blank or comment-only
		const size_t to = line.find_last_not_of(" \t");
		out.push_back(line.substr(from, to - from + 1));
	}
	return true;
}

std::string Game::ResolveEvalPath(std::string_view spec) const {
	// Trim, then resolve RELATIVE TO THE ROOT SCRIPT'S FOLDER. A suite lives in
	// one directory and refers to its presets by a short name; making that
	// relative to the process's working directory instead would tie every script
	// to wherever the exe happened to be launched from, which for this project is
	// a build folder several levels away from the scripts.
	const size_t from = spec.find_first_not_of(" \t");
	if (from == std::string_view::npos) return {};
	const size_t to = spec.find_last_not_of(" \t");
	const std::string rel(spec.substr(from, to - from + 1));
	const std::filesystem::path p(rel);
	if (p.is_absolute() || m_evalDir.empty()) return rel;
	return (std::filesystem::path(m_evalDir) / p).string();
}

bool Game::LoadEvalScript(const std::string& path) {
	// CLEAR FIRST. ReadEvalLines APPENDS — which is right for `include`, its
	// other caller, and wrong here: without this a batch's second script ran the
	// first one again ahead of itself, the third ran both, and the tenth ran all
	// ten. It came back PASS every time and simply took longer, which is the only
	// reason it was caught (the cumulative `lines=` in the verdicts).
	m_evalLines.clear();
	if (!ReadEvalLines(path, m_evalLines)) return false;
	const std::filesystem::path p(path);
	m_evalName = p.filename().string();
	m_evalDir = p.parent_path().string(); // what `include` resolves against
	// PER-SCRIPT state, reset here rather than at the call site — this is also
	// the mid-batch entry point, and a second script inheriting the first's
	// line index would start half way through itself.
	m_evalIndex = 0;
	m_evalUnknown = 0;
	m_evalRefused = 0;
	m_evalDeadline = kEvalScriptTimeout; // the budget is per script, not per run
	// Mirroring is forced ON rather than left to the script: a run whose author
	// forgot the line would produce no readable record of itself, which is the
	// one outcome a harness must not have.
	m_console.SetMirrorToLog(true);
	log::Info("eval: '{}' queued, {} line(s)", m_evalName, m_evalLines.size());
	return true;
}

void Game::PumpEvalScript(float dt) {
	// NOT `m_evalLines.empty()`: an empty script has to reach the completion
	// branch below and hand the batch on, or one blank file stalls the whole run
	// until the timeout.
	if (m_evalFinished || m_evalName.empty()) return;
	m_evalDeadline -= dt;

	if (m_evalIndex >= m_evalLines.size()) {
		// A SCRIPT MUST END IN PLAY. A party wipe returns to the title screen and
		// every dev command keeps answering perfectly normally from there, so a
		// run that died on its last encounter prints a full set of plausible
		// readouts and exits 0. That is not hypothetical: expedition.eval has
		// been ending at the menu, with its closing "what the expedition cost"
		// readouts taken after the party was already dead
		// (docs/eval-audit.md F19). The existing guard — `step` refusing when
		// not playing — only fires on the NEXT step, and a script's last
		// encounter has no next step.
		const bool endedOutOfPlay = m_state != AppState::Playing;
		if (endedOutOfPlay)
			log::Error("eval: script {} ended in state '{}', not playing — the "
					   "run did not survive its own last encounter, so whatever "
					   "it printed after that point describes a dead party",
					   m_evalName, StateName());

		// THE VERDICT, in the house format every other checker in this project
		// emits, so one reader handles them all. ONE PER SCRIPT — a batch that
		// reported only at the end would make a reader count lines to work out
		// which of twenty scripts went wrong.
		const int problems =
			m_evalUnknown + m_evalRefused + (endedOutOfPlay ? 1 : 0);
		++m_evalScripts;
		if (problems != 0) ++m_evalFailed;
		log::Info("eval RESULT={} script={} lines={} unknown={} refused={} "
				  "endstate={}",
				  problems == 0 ? "PASS" : "FAIL", m_evalName,
				  m_evalLines.size(), m_evalUnknown, m_evalRefused, StateName());
		// NEXT SCRIPT IN THE SAME PROCESS, if there is one — the whole point of
		// the batch form. Deliberately WITHOUT a reset: the script decides
		// whether it wants a clean baseline (`reset` at the top) or to inherit
		// what the last one left, which is how a progression series is written.
		if (!m_evalPending.empty()) {
			const std::string next = m_evalPending.front();
			m_evalPending.erase(m_evalPending.begin());
			if (LoadEvalScript(next)) return;
			// Unreadable mid-batch. Counted as a failure and the run CARRIES ON:
			// the remaining scripts are still worth measuring, and stopping here
			// would lose them to a typo in one filename.
			++m_evalScripts;
			++m_evalFailed;
			log::Error("eval RESULT=FAIL script={} — could not be read", next);
			if (!m_evalPending.empty()) return; // try the rest next frame
		}
		m_evalFinished = true;
		log::Info("eval BATCH RESULT={} scripts={} failed={}",
				  m_evalFailed == 0 ? "PASS" : "FAIL", m_evalScripts, m_evalFailed);
		m_quitRequested = true; // a batch run exits; nobody is watching the window
		return;
	}
	if (m_evalDeadline <= 0.0f) {
		// Deliberately NOT marked finished: EvalExitCode reads that, so a timed-
		// out run fails even though every line it managed to run succeeded.
		log::Error("eval RESULT=FAIL script={} lines={} unknown={} — TIMED OUT at "
				   "line {} ('{}')",
				   m_evalName, m_evalLines.size(), m_evalUnknown, m_evalIndex + 1,
				   m_evalLines[m_evalIndex]);
		// A TIMEOUT ENDS THE WHOLE BATCH, unlike an unreadable script. Something
		// is wedged — a load that never finished, a state the script cannot get
		// out of — and the remaining scripts would inherit it, so they would not
		// be measuring what they claim. Better one honest failure than twenty
		// plausible ones.
		++m_evalScripts;
		++m_evalFailed;
		log::Error("eval BATCH RESULT=FAIL scripts={} failed={} — abandoned after "
				   "a timeout ({} script(s) never ran)",
				   m_evalScripts, m_evalFailed, m_evalPending.size());
		m_quitRequested = true;
		m_evalPending.clear();
		m_evalLines.clear();
		return;
	}
	// The load gate does the waiting for free: while a staged load runs, commands
	// are disabled, so a script that starts a new game simply resumes when the
	// world is actually there. No `wait` directive, and nothing to tune.
	if (!m_console.CommandsEnabled()) return;
	// ONE LINE PER FRAME. A command that changes state (newgame, a level
	// transition, a quality swap) needs the frame to land before the next line
	// reasons about the result.
	const std::string line = m_evalLines[m_evalIndex++]; // by value: splicing below
	// `include <path>` is handled HERE rather than as a console command, because
	// it edits the queue the pump is walking — something a command handler has
	// no business reaching into. It is what makes a PRESET a reusable script
	// fragment (tools/EvalScripts/presets/*.eval) instead of a hardcoded table
	// of loadouts in C++: a rung says `include presets/veteran.eval` and the
	// preset stays editable, diffable and composable like the rest of the suite.
	// `sweep <count> <path>` — the same rung run `count` times, each preceded by
	// its own `seed`. Combat is dice, so ONE encounter is an anecdote: a 5%
	// fumble or a 6% critical only shows up honestly in a distribution, and this
	// is what produces one without a process launch per sample.
	//
	// Seeds are 1..count, so a sweep is reproducible and two sweeps of the same
	// rung are comparable sample-for-sample — which is the whole point when the
	// thing being compared is a knob change.
	if (line.starts_with("sweep ")) {
		const size_t sp = line.find(' ', 6);
		const int count = std::atoi(line.substr(6, sp - 6).c_str());
		const std::string path =
			sp == std::string::npos ? std::string() : ResolveEvalPath(line.substr(sp));
		std::vector<std::string> body;
		if (count < 1 || path.empty() || !ReadEvalLines(path, body)) {
			++m_evalUnknown;
			log::Error("eval: line {} bad sweep ('{}')", m_evalIndex, line);
			return;
		}
		std::vector<std::string> spliced;
		spliced.reserve(body.size() * static_cast<size_t>(count) + count);
		for (int i = 1; i <= count; ++i) {
			spliced.push_back(std::format("seed {}", i));
			spliced.insert(spliced.end(), body.begin(), body.end());
		}
		m_evalLines.insert(m_evalLines.begin() + static_cast<ptrdiff_t>(m_evalIndex),
						   spliced.begin(), spliced.end());
		log::Info("eval: sweeping '{}' x{} ({} line(s))", path, count, spliced.size());
		return;
	}
	if (line.starts_with("include ")) {
		const std::string path = ResolveEvalPath(line.substr(8));
		std::vector<std::string> nested;
		if (!ReadEvalLines(path, nested)) {
			++m_evalUnknown;
			log::Error("eval: line {} cannot include '{}'", m_evalIndex, path);
			return;
		}
		// SPLICED IN PLACE, so an included line is indistinguishable from one
		// written inline — same one-per-frame pacing, same load gate, same
		// counting. Nesting works for free and needs no depth tracking of its
		// own; a cycle is bounded by the deadline rather than by a guard, which
		// is deliberate — the deadline exists precisely to catch a run that will
		// not end, whatever the reason.
		m_evalLines.insert(m_evalLines.begin() + static_cast<ptrdiff_t>(m_evalIndex),
						   nested.begin(), nested.end());
		log::Info("eval: included '{}' ({} line(s))", path, nested.size());
		return;
	}
	if (!m_console.RunLine(line)) {
		++m_evalUnknown;
		log::Error("eval: line {} matched no command: '{}'", m_evalIndex, line);
	} else if (m_console.ConsumeRefusal()) {
		// THE OTHER HALF of "did this line do what it said". RunLine's bool only
		// answers whether a command by that name exists, which catches typos and
		// nothing else. A `spawn` onto rock, a `tp` into a wall and a `step` past
		// its ceiling all returned true here and the run reported PASS over an
		// encounter it never set up (docs/eval-audit.md F11/F15).
		++m_evalRefused;
		log::Error("eval: line {} REFUSED: '{}'", m_evalIndex, line);
	}
}

} // namespace dungeon::game
