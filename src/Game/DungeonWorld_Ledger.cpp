// ============================================================================
// DungeonWorld_Ledger.cpp — the world's half of the one-pipeline check.
//
// Game/DamageLedger.h holds the rule and knows nothing about this game; this is
// the part that knows where the health in a dungeon actually lives. Two jobs:
// WALK every value the damage pipeline can reach (members, monsters, and the
// three kinds of breakable), and turn a violation's key back into something a
// person can read.
//
// The sweep is deliberately the only place that enumerates targets, so a future
// fourth kind of thing that can be hurt is caught by adding it HERE rather than
// by everyone remembering to. If it is not in this file, it is not checked —
// which is why the report says how many values it looked at.
// ============================================================================
#include "Core/Assert.h"
#include "Core/Log.h"
#include "Game/DungeonWorld.h"

#include <array>
#include <format>

namespace dungeon::game {

using ledger::Key;
using ledger::Subject;

void DungeonWorld::SweepDamageLedger() {
	m_damageLedger.BeginSweep();
	if (!m_damageLedger.Armed()) return;

	if (m_roster)
		for (size_t i = 0; i < m_roster->size(); ++i)
			m_damageLedger.Observe((*m_roster)[i].health,
								   Key{Subject::Member, static_cast<int>(i)});
	// The runtimeId, not the index: it is the id everything else in the world
	// already keys a monster by, and an index would rename every monster the
	// moment one is erased.
	for (const Monster& m : m_monsters)
		m_damageLedger.Observe(m.hp, Key{Subject::Monster,
										 static_cast<int>(m.runtimeId)});
	// maxHp == 0 means "not a target" (docs/damage-system.md), so an indestructible
	// prop is not watched — it has no hit points to write, and watching it would
	// pad the count with values nothing can move.
	for (size_t i = 0; i < m_decorations.size(); ++i)
		if (m_decorations[i].brk.maxHp > 0.0f)
			m_damageLedger.Observe(m_decorations[i].brk.hp,
								   Key{Subject::Decoration, static_cast<int>(i)});
	for (size_t i = 0; i < m_doors.size(); ++i)
		if (m_doors[i].brk.maxHp > 0.0f)
			m_damageLedger.Observe(m_doors[i].brk.hp,
								   Key{Subject::Door, static_cast<int>(i)});
	for (size_t i = 0; i < m_fixtureBreaks.size(); ++i)
		if (m_fixtureBreaks[i].brk.maxHp > 0.0f)
			m_damageLedger.Observe(m_fixtureBreaks[i].brk.hp,
								   Key{Subject::Fixture, static_cast<int>(i)});
}

void DungeonWorld::CheckDamageLedger(const char* phase) {
	if (!m_damageLedger.Armed()) return;
	SweepDamageLedger();
	// A frame that violates the rule at all usually violates it for one value;
	// four is room for a blast that went around the pipeline without turning the
	// log into a wall. The count is exact regardless — only the DETAIL is capped.
	std::array<ledger::Violation, 4> found;
	const int n = m_damageLedger.Checkpoint(phase, found);
	for (int i = 0; i < n; ++i) {
		const ledger::Violation& v = found[i];
		if (!m_damageLedger.ShouldReport(v)) continue;
		const std::string line = std::format(
			"one-pipeline violation: {} health moved {:+.3f} during \"{}\", of "
			"which {:+.3f} was accounted for — {:+.3f} went around fx::Deal",
			LedgerSubjectName(v.key), v.moved, v.phase, v.explained,
			v.Unexplained());
		log::Warn("{}", line);
		DN_ASSERT(!m_damageLedger.Strict(), line.c_str());
	}
}

void DungeonWorld::RebaseDamageLedger() {
	if (!m_damageLedger.Armed()) return;
	SweepDamageLedger();
	m_damageLedger.Rebase();
}

std::string DungeonWorld::LedgerSubjectName(ledger::Key key) const {
	switch (key.subject) {
	case Subject::Member:
		if (m_roster && static_cast<size_t>(key.id) < m_roster->size())
			return (*m_roster)[key.id].name;
		break;
	case Subject::Monster:
		for (const Monster& m : m_monsters)
			if (static_cast<int>(m.runtimeId) == key.id)
				return std::format("{}#{} @{},{}", m.kind ? m.kind->name : "?",
								   key.id, m.x, m.z);
		break;
	case Subject::Decoration:
		if (static_cast<size_t>(key.id) < m_decorations.size())
			return std::format("prop @{},{}", m_decorations[key.id].x,
							   m_decorations[key.id].z);
		break;
	case Subject::Door:
		if (static_cast<size_t>(key.id) < m_doors.size())
			return std::format("door {} @{},{}", m_doors[key.id].type,
							   m_doors[key.id].x, m_doors[key.id].z);
		break;
	case Subject::Fixture:
		if (static_cast<size_t>(key.id) < m_fixtureBreaks.size())
			return std::format("fixture {} @{},{}", m_fixtureBreaks[key.id].type,
							   m_fixtureBreaks[key.id].x,
							   m_fixtureBreaks[key.id].z);
		break;
	}
	// The subject died, or its container moved, between the violation and the
	// report. Say so rather than inventing a name.
	return std::format("{} #{} (gone)", ledger::SubjectName(key.subject), key.id);
}

std::vector<std::string> DungeonWorld::DamageLedgerReport() const {
	const ledger::Stats& s = m_damageLedger.GetStats();
	std::vector<std::string> out;
	// The house result-line convention every checker in the project emits, so
	// tools/PipelineTest.ps1 reads this the same way it reads the others.
	out.push_back(std::format("PIPELINE RESULT={} checks={} violations={}",
							  s.violations == 0 ? "PASS" : "FAIL",
							  s.valuesChecked, s.violations));
	out.push_back(std::format("  armed={} strict={} checkpoints={} dropped={}",
							  m_damageLedger.Armed() ? "on" : "off",
							  m_damageLedger.Strict() ? "on" : "off",
							  s.checkpoints, s.dropped));
	// WHERE THE HEALTH WENT, by route. This is the half that makes the invariant
	// visible rather than merely asserted: a run whose `pipeline` total is zero
	// did not exercise the pipeline at all, which is a thing worth knowing about
	// a check that just reported PASS.
	for (size_t i = 0; i < static_cast<size_t>(ledger::Reason::Count); ++i) {
		const float total = s.credited[i];
		if (total == 0.0f) continue;
		out.push_back(std::format(
			"  {:<10} {:+.2f}",
			ledger::ReasonName(static_cast<ledger::Reason>(i)), total));
	}
	return out;
}

} // namespace dungeon::game
