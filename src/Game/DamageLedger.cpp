#include "Game/DamageLedger.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game::ledger {

const char* ReasonName(Reason reason) {
	switch (reason) {
	case Reason::Pipeline: return "pipeline";
	case Reason::Exertion: return "exertion";
	case Reason::Regen: return "regen";
	case Reason::Growth: return "growth";
	case Reason::Stabilize: return "stabilize";
	default: return "?";
	}
}

void Ledger::BeginSweep() {
	if (!m_armed) return;
	m_current.clear(); // capacity is kept: this runs inside a guarded frame
	m_sweeping = true;
}

void Ledger::Observe(const float& value, Key key) {
	if (!m_armed || !m_sweeping) return;
	m_current.push_back(Entry{&value, key, value, 0.0f});
}

const char* SubjectName(Subject subject) {
	switch (subject) {
	case Subject::Member: return "member";
	case Subject::Monster: return "monster";
	case Subject::Decoration: return "prop";
	case Subject::Door: return "door";
	case Subject::Fixture: return "fixture";
	default: return "?";
	}
}

int Ledger::Checkpoint(const char* phase, std::span<Violation> out) {
	return Sweep(phase, out, true);
}

void Ledger::Rebase() { Sweep("", {}, false); }

int Ledger::Sweep(const char* phase, std::span<Violation> out, bool verify) {
	if (!m_armed) return 0;
	m_sweeping = false;
	std::sort(m_current.begin(), m_current.end(),
			  [](const Entry& a, const Entry& b) { return a.addr < b.addr; });

	int found = 0;
	int written = 0;
	size_t vanished = 0;

	// Two sorted runs, walked together. Matched addresses are the check; an
	// address only in the sweep is something that came into existence since the
	// last checkpoint (a spawned monster) and has no baseline to be judged
	// against; an address only in the baseline is gone, or its container moved
	// under it — either way there is nothing left to compare, which is counted
	// rather than passed over, so a reallocation cannot quietly take a whole
	// vector's worth of coverage with it.
	size_t i = 0, j = 0;
	while (i < m_baseline.size() && j < m_current.size()) {
		const Entry& was = m_baseline[i];
		Entry& now = m_current[j];
		if (was.addr < now.addr) {
			++vanished;
			++i;
			continue;
		}
		if (now.addr < was.addr) {
			++j; // newly watched — no baseline, no verdict
			continue;
		}
		++found;
		const float moved = now.baseline - was.baseline;
		if (verify && std::fabs(moved - was.explained) > kEpsilon) {
			++m_stats.violations;
			if (written < static_cast<int>(out.size()))
				out[written++] = Violation{phase, was.key, moved, was.explained};
		}
		++i;
		++j;
	}
	vanished += m_baseline.size() - i;

	if (verify) {
		m_stats.dropped += vanished;
		m_stats.valuesChecked += static_cast<u64>(found);
		++m_stats.checkpoints;
	}

	// The sweep becomes the baseline; the old buffer is recycled as next
	// sweep's scratch, so neither vector ever allocates again.
	m_baseline.swap(m_current);
	return written;
}

void Ledger::Credit(const float& value, float delta, Reason reason) {
	if (!m_armed || delta == 0.0f) return;
	m_stats.credited[static_cast<size_t>(reason)] += delta;
	const float* addr = &value;
	const auto it = std::lower_bound(
		m_baseline.begin(), m_baseline.end(), addr,
		[](const Entry& e, const float* a) { return e.addr < a; });
	// Not watched: a value outside the sweep, or one that first appeared after
	// the last checkpoint. Silently fine — it has no baseline to reconcile.
	if (it != m_baseline.end() && it->addr == addr) it->explained += delta;
}

void Ledger::DropAll() {
	m_stats.dropped += m_baseline.size();
	m_baseline.clear();
	m_current.clear();
	m_sweeping = false;
}

bool Ledger::ShouldReport(const Violation& v) {
	for (const Seen& s : m_reported)
		if (s.phase == v.phase && s.key.subject == v.key.subject &&
			s.key.id == v.key.id)
			return false;
	if (static_cast<int>(m_reported.size()) >= kMaxReported) return false;
	m_reported.push_back(Seen{v.phase, v.key});
	return true;
}

void Ledger::ResetStats() {
	m_stats = Stats{};
	m_reported.clear();
}

} // namespace dungeon::game::ledger
