// ============================================================================
// Game/DamageLedger.h — the one-pipeline invariant, CHECKED.
//
// docs/effects.md's whole point is that every source of damage builds an
// fx::DamageEvent and goes through fx::Deal, so that outside the fx::ITarget
// adapters nothing writes health at all. That was verified ONCE, by hand, on
// 2026-07-25. A hand sweep dates the moment it is written; this is the
// machinery that makes it a standing rule.
//
// WHY THIS IS A RUNTIME CHECK AND NOT A GREP. The obvious cheap version scans
// the source for writes to `health`/`hp` against an allowlist. It was measured
// against the code as it actually stands and it does not work: the resource
// regeneration added by docs/health-and-healing.md writes health through a
// lambda taking `float&` (`value += gained`), so the identifier never appears
// on the assignment. A scan reports that file CLEAN. An invariant checked by
// something that cannot see the write is the project's oldest failure — absent
// and correct reporting identically — installed inside the check meant to
// prevent it.
//
// WHAT IT DOES. A watched value carries a BASELINE. Anything allowed to move it
// says so, through Ledger::Credit or the Explained scope, which credits
// whatever changed inside it to a named Reason. At a CHECKPOINT the caller
// re-walks the world and every watched value must have moved by exactly what
// was credited. Whatever is left over is a write that went around the pipeline.
//
// IT NAMES THE PHASE, NOT THE LINE, and that is a real limit worth stating. A
// violation is discovered at the checkpoint, by which time the stack that did
// it is long gone — the same problem Core/Diagnostics has at a catch site,
// without the vectored-handler trick to solve it (there is no OS event for
// "someone stored a float"). Catching the culprit red-handed would mean making
// health a guarded type, which ripples through save/load, the UI and every
// piece of arithmetic in combat. So the checkpoints are placed at PHASE
// boundaries instead and a violation reports the phase, the victim and the
// amount, which between them have always been enough to find the line.
//
// SCOPE IS HEALTH ONLY. Stamina, mana, food and water are spent and regained by
// rules of their own and are not damage; the pipeline does not own them. What
// it owns is the quantity a blow takes off, on all three kinds of target —
// members, monsters and breakables alike.
//
// PURE TU: Core/Types.h and the standard library, nothing else. That is what
// lets tools/RollTest link the shipping ledger rather than a copy of it — the
// Game/Defense.h rule, for the same reason.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <span>
#include <vector>

namespace dungeon::game::ledger {

// How much of a change is noise. Credits are computed as differences of the
// very floats they explain, so an explained move cancels to zero or to a few
// ulps of one; the smallest damage the game can deal is orders above this.
inline constexpr float kEpsilon = 1e-4f;

// WHO a watched value belongs to. The ledger stores the id and nothing else —
// resolving it to "Brand" or "skeleton #7" needs the world, which this TU
// deliberately cannot see, so the reporting side does it at report time. The
// three breakable kinds are kept apart rather than folded into one so an id can
// simply be an index into its own container.
enum class Subject : u8 { Member, Monster, Decoration, Door, Fixture };

const char* SubjectName(Subject subject);

struct Key {
	Subject subject = Subject::Member;
	int id = 0; // roster slot / monster runtimeId / index in its container
};

// WHY a value was allowed to move. Every one of these is a decision recorded
// somewhere in the docs, and the per-reason totals are printed by the `pipeline`
// command — so the invariant is VISIBLE (this much health moved, and by which
// route) rather than merely asserted.
enum class Reason : u8 {
	Pipeline,  // an fx::ITarget adapter — the rule itself
	Exertion,  // over-exertion's health half (docs/damage-system.md), a
			   // DECLARED exception: self-inflicted collapse is not resistable,
			   // so it deliberately does not build a DamageEvent
	Regen,     // resource regeneration (docs/health-and-healing.md)
	Growth,    // a stat point landed and the pool GREW under its owner
			   // (Character::RecomputeMaxima). This one fires mid-fight — VIT
			   // creeps off exertion — which is why it is a reason of its own
			   // rather than being lumped in with the setup paths
	Stabilize, // the unconscious waking by themselves (docs/combat.md Phase 5)
	Count
};
// There is deliberately NO `Setup` reason. A load, a new game, a respawn or a
// `heal` does not EXPLAIN its writes, it REBASES — the values it overwrote no
// longer exist to be reconciled, so crediting them would be arithmetic about a
// world that is gone. One was written and removed when the pipeline suite showed
// its row could never be anything but zero: a route nothing can take is a lie in
// a readout whose whole job is saying which routes ran.

const char* ReasonName(Reason reason);

struct Violation {
	const char* phase = "";  // the region between two checkpoints
	Key key;
	float moved = 0.0f;      // what the value actually did
	float explained = 0.0f;  // what was credited for it
	float Unexplained() const { return moved - explained; }
};

struct Stats {
	u64 checkpoints = 0;
	u64 valuesChecked = 0;
	u64 violations = 0;
	u64 dropped = 0;   // baselines abandoned because their container moved
	float credited[static_cast<size_t>(Reason::Count)] = {};
};

// A sweep is the caller walking every watched value; the ledger cannot walk the
// world itself and must not learn how. The order does not matter — entries are
// matched by ADDRESS, so a container that reorders is handled for free, and one
// that REALLOCATES is noticed rather than silently losing its baselines.
class Ledger {
public:
	void Arm(bool on) { m_armed = on; }
	bool Armed() const { return m_armed; }
	void SetStrict(bool on) { m_strict = on; }
	bool Strict() const { return m_strict; }

	// --- the sweep ----------------------------------------------------------
	// Called once per watched value between BeginSweep and Checkpoint.
	void Observe(const float& value, Key key);
	void BeginSweep();
	// Verifies everything observed against the standing baseline, then takes the
	// new one. Writes up to `out.size()` violations and returns how many were
	// WRITTEN. `phase` must outlive the call — a string literal in practice.
	int Checkpoint(const char* phase, std::span<Violation> out);
	// Takes the new baseline WITHOUT judging the old one. For a path that
	// legitimately replaces state wholesale — a level load, a save restore, a
	// respawn, a dev-console fiat — where sanctioning each of the dozen writes
	// it makes would be pedantry: the values it replaced no longer exist to be
	// compared against. Unlike DropAll it costs no coverage, because the sweep
	// it consumes becomes the baseline everything after is judged against.
	void Rebase();

	// --- explaining a move --------------------------------------------------
	// `delta` is what this reason moved the value by. A value that is not
	// watched is a silent no-op: a member off the roster, a monster spawned
	// since the last checkpoint, or the ledger simply not armed.
	void Credit(const float& value, float delta, Reason reason);

	// Every baseline is stale at once — a level load, a new game, an editor
	// respawn. Cheaper and more honest than sanctioning each of the dozen
	// writes such a path makes: the state it replaced no longer exists to be
	// compared against.
	void DropAll();

	// True the first time a given (phase, subject, id) is seen. The caller logs
	// only on true, so a standing violation cannot drown the log — the rule
	// Core/Diagnostics follows for stacks.
	bool ShouldReport(const Violation& v);

	const Stats& GetStats() const { return m_stats; }
	void ResetStats();

private:
	struct Entry {
		const float* addr = nullptr;
		Key key;
		float baseline = 0.0f;
		float explained = 0.0f;
	};

	// Sorted by address; binary-searched by Credit.
	std::vector<Entry> m_baseline;
	// This sweep's observations, sorted at Checkpoint and merged against
	// m_baseline. Two buffers reused forever: a steady-state frame allocates
	// nothing (docs/ARCHITECTURE.md "Memory strategy"), and this runs inside the
	// frame the alloc guard is watching.
	std::vector<Entry> m_current;

	struct Seen {
		const char* phase = "";
		Key key;
	};
	static constexpr int kMaxReported = 16;
	std::vector<Seen> m_reported;

	int Sweep(const char* phase, std::span<Violation> out, bool verify);

	Stats m_stats;
	// ON in a debug build, OFF in release, and `pipelineguard` moves it either
	// way. The same default the allocation guard takes, for the same reason: the
	// cost is a few dozen float reads and one sort per checkpoint, which is
	// nothing next to a debug build's other costs, and a rule nobody runs is a
	// rule that drifts. The eval harness arms it explicitly, so the check does
	// not depend on which configuration the suite happened to be built in.
#ifdef NDEBUG
	bool m_armed = false;
#else
	bool m_armed = true;
#endif
	bool m_strict = false;
	bool m_sweeping = false;
};

// RAII: whatever `value` does while this is alive is credited to `reason`.
//
// This is the form nearly every sanctioned site uses, because it measures the
// move rather than being told about it — a caller that computes the delta
// itself is a second rule that agrees with the shipping one only by luck (the
// Defense.h note on why the exertion split was not lifted out). A clamp at zero,
// a cap at maximum, a growth spurt landing mid-spend: all of it is simply
// whatever the float did.
class Explained {
public:
	Explained(Ledger& ledger, float& value, Reason reason)
		: m_ledger(ledger), m_value(value), m_before(value), m_reason(reason) {}
	~Explained() { m_ledger.Credit(m_value, m_value - m_before, m_reason); }

	Explained(const Explained&) = delete;
	Explained& operator=(const Explained&) = delete;

private:
	Ledger& m_ledger;
	float& m_value;
	float m_before;
	Reason m_reason;
};

} // namespace dungeon::game::ledger
