// ============================================================================
// Game/DevConsole_Snapshots.cpp - `profile snap` / `snaps` / `diff`: recording
// the profile over a few seconds under a name, and comparing two recordings.
// DevConsole.h says why a snapshot records over seconds and keys by path.
// ============================================================================
#include "Game/DevConsole_Panel.h"

#include "Core/Log.h"
#include "Core/Profile.h"

#include <algorithm>
#include <cmath>
#include <cstring> // strcmp / memcpy, matching and naming rows by path
#include <format>

namespace dungeon::game {

using namespace devcon;

// The command's three verbs. Compiled in every build because the `profile`
// command's lambda is - a discarded `if constexpr` branch in a non-template is
// still compiled - but without DN_PROFILE that command never gets this far.
void DevConsole::SnapCommand(const std::vector<std::string>& args) {
	if (args[0] == "snap") {
		if (args.size() < 2) {
			Print("usage: profile snap <name> [seconds]");
			return;
		}
		if (m_snapTarget >= 0) {
			Print("a recording is already running");
			return;
		}
		float secs = 3.0f;
		if (args.size() > 2) {
			try {
				secs = std::stof(args[2]);
			} catch (const std::exception&) {
				Print("seconds must be a number");
				return;
			}
		}
		// Reuse the slot of the same name so re-taking a reading
		// after a tweak does not silently fill the table with
		// near-identical entries.
		int slot = SnapSlot(args[1]);
		if (slot < 0) slot = SnapFreeSlot();
		if (slot < 0) {
			Print(std::format("all {} snapshot slots are used - "
							  "'profile snap <existing name>' overwrites one",
							  kSnapSlots));
			return;
		}
		m_snaps[slot] = Snapshot{};
		m_snaps[slot].used = true;
		CopyName(m_snaps[slot].name, args[1].c_str());
		m_snapTarget = slot;
		m_snapLeft = std::clamp(secs, 0.25f, 60.0f);
		Print(std::format("recording '{}' for {:.1f}s...", args[1], m_snapLeft));
	} else if (args[0] == "snaps") {
		int n = 0;
		for (const Snapshot& s : m_snaps) {
			if (!s.used) continue;
			++n;
			Print(std::format("  {:<12} {} rows, {:.1f}s, frame {:.3f} ms", s.name,
							  s.rows, s.seconds, s.frameMs));
		}
		if (n == 0) Print("no snapshots; 'profile snap <name> [secs]'");
	} else { // diff
		if (args.size() < 3) {
			Print("usage: profile diff <before> <after>");
			return;
		}
		const int a = SnapSlot(args[1]), b = SnapSlot(args[2]);
		if (a < 0 || b < 0) {
			Print(std::format("unknown snapshot '{}'", a < 0 ? args[1] : args[2]));
			return;
		}
		SnapDiff(m_snaps[a], m_snaps[b]);
	}
}

// Same preprocessor split as BuildProfileRows, and for the same reason.
#if DN_PROFILE
int DevConsole::SnapSlot(std::string_view name) const {
	for (int i = 0; i < kSnapSlots; ++i)
		if (m_snaps[i].used && name == m_snaps[i].name) return i;
	return -1;
}

int DevConsole::SnapFreeSlot() {
	for (int i = 0; i < kSnapSlots; ++i)
		if (!m_snaps[i].used) return i;
	return -1;
}

void DevConsole::SnapAccumulate(float dt) {
	if (m_snapTarget < 0) return;
	Snapshot& s = m_snaps[m_snapTarget];

	ProfRow rows[kMaxProfRows];
	const int n = BuildProfileRows(rows, kMaxProfRows);

	// Same pre-order path rebuild the list view uses: ancestors are whatever is
	// sitting at the shallower depths when this row comes up.
	const char* nameAtDepth[prof::kMaxDepth] = {};
	const char* thread = "";
	for (int i = 0; i < n; ++i) {
		const ProfRow& r = rows[i];
		if (r.header) {
			thread = r.name;
			continue;
		}
		if (r.depth >= static_cast<int>(prof::kMaxDepth)) continue;
		nameAtDepth[r.depth] = r.name;

		char path[128];
		size_t w = 0;
		for (int d = 0; d <= r.depth && w + 1 < sizeof(path); ++d) {
			if (!nameAtDepth[d]) continue;
			if (w > 0) path[w++] = '/';
			for (const char* c = nameAtDepth[d]; *c && w + 1 < sizeof(path); ++c)
				path[w++] = *c;
		}
		path[w] = '\0';

		SnapRow* dst = nullptr;
		for (int j = 0; j < s.rows; ++j)
			if (std::strcmp(s.row[j].thread, thread) == 0 &&
				std::strcmp(s.row[j].path, path) == 0) {
				dst = &s.row[j];
				break;
			}
		if (!dst) {
			if (s.rows >= kSnapRows) continue; // full; the deepest rows drop
			dst = &s.row[s.rows++];
			CopyName(dst->thread, thread);
			std::memcpy(dst->path, path, w + 1);
		}
		dst->incl += r.inclMs;
		dst->excl += r.exclMs;
		dst->calls += static_cast<double>(r.calls);
		dst->worst = std::max(dst->worst, r.maxMs); // MAX, not a mean of maxima
	}

	const FrameBudget fb =
		MeasureFrameBudget(rows, n, [](const ProfRow& r) { return r.inclMs; });
	if (fb.valid) {
		s.frameMs += fb.frameMs;
		s.cpuMs += fb.cpuMs;
		s.waitMs += fb.waitGpuMs;
		s.presentMs += fb.presentMs;
		s.capMs += fb.capMs;
		s.gpuMs += fb.gpuBusyMs;
		s.budgetValid = true;
	}

	++s.samples;
	s.seconds += dt;
	m_snapLeft -= dt;
	if (m_snapLeft <= 0.0f) SnapFinish();
}

void DevConsole::SnapFinish() {
	if (m_snapTarget < 0) return;
	Snapshot& s = m_snaps[m_snapTarget];
	m_snapTarget = -1;

	if (s.samples <= 0) {
		s.used = false;
		Print("snapshot recorded nothing (is the profiler compiled in?)");
		return;
	}
	const double inv = 1.0 / static_cast<double>(s.samples);
	for (int i = 0; i < s.rows; ++i) {
		s.row[i].incl *= inv;
		s.row[i].excl *= inv;
		s.row[i].calls *= inv;
		// worst is already a max over the window
	}
	s.frameMs *= inv;
	s.cpuMs *= inv;
	s.waitMs *= inv;
	s.presentMs *= inv;
	s.capMs *= inv;
	s.gpuMs *= inv;

	// Logged as well as printed. A snapshot's summary is the only durable record
	// of a state you have already left — by the time it is interesting, the
	// setting has been changed and the console line has scrolled — and a
	// recording that survives nowhere is a measurement you have to retake.
	auto say = [&](const std::string& line) {
		Print(line);
		log::Write(log::Level::Info, line);
	};
	say(std::format("snapshot '{}' recorded: {} rows over {:.1f}s ({} frames)", s.name, s.rows,
					s.seconds, s.samples));
	if (s.budgetValid)
		say(std::format("  frame {:.3f}  cpu {:.3f}  wait {:.3f}  present {:.3f}  cap {:.3f}  "
						"gpu {:.3f}",
						s.frameMs, s.cpuMs, s.waitMs, s.presentMs, s.capMs, s.gpuMs));

	// A MACHINE-READABLE twin, log only. The line above is laid out for a person
	// and a harness parsing it would break the moment a column is widened or a
	// term added — which is exactly what just happened to it when the frame cap
	// landed. key=value survives both.
	if (s.budgetValid)
		log::Write(log::Level::Info,
				   std::format("profilesnap {} frame={:.4f} cpu={:.4f} wait={:.4f} "
							   "present={:.4f} cap={:.4f} gpu={:.4f} rows={} samples={} "
							   "secs={:.2f}",
							   s.name, s.frameMs, s.cpuMs, s.waitMs, s.presentMs, s.capMs,
							   s.gpuMs, s.rows, s.samples, s.seconds));
}

void DevConsole::SnapDiff(const Snapshot& a, const Snapshot& b) {
	// ALSO TO THE LOG, and this is not a nicety. A diff is twenty-odd lines and
	// the console shows three of them above the prompt, so the answer scrolls
	// past the moment it is printed — the first real use of this feature
	// produced a correct comparison that could not be read. dungeon.log is
	// where a multi-line answer belongs; the console keeps the headline.
	auto say = [&](const std::string& s) {
		Print(s);
		log::Write(log::Level::Info, s);
	};

	// Budget first: it is the headline, and the only part that answers "is it
	// faster" rather than "what moved".
	say(std::format("diff '{}' -> '{}'  ({:.1f}s vs {:.1f}s)", a.name, b.name, a.seconds,
					b.seconds));
	auto delta = [&](const char* label, double x, double y) {
		const double d = y - x;
		const double pct = x > 0.0001 ? d / x * 100.0 : 0.0;
		say(std::format("  {:<9} {:>8.3f} -> {:>8.3f}   {:+8.3f} ms  {:+7.1f}%", label, x, y,
						d, pct));
	};
	if (a.budgetValid && b.budgetValid) {
		delta("frame", a.frameMs, b.frameMs);
		delta("cpu", a.cpuMs, b.cpuMs);
		delta("wait.gpu", a.waitMs, b.waitMs);
		delta("present", a.presentMs, b.presentMs);
		delta("cap", a.capMs, b.capMs);
		delta("gpu", a.gpuMs, b.gpuMs);
	}

	// Then the rows that MOVED, biggest absolute change first — a diff sorted by
	// name buries the one line worth reading among forty that did not budge.
	struct Change {
		const SnapRow* from;
		const SnapRow* to;
		double d;
	};
	std::vector<Change> changes;
	for (int j = 0; j < b.rows; ++j) {
		const SnapRow& to = b.row[j];
		const SnapRow* from = nullptr;
		for (int i = 0; i < a.rows; ++i)
			if (std::strcmp(a.row[i].thread, to.thread) == 0 &&
				std::strcmp(a.row[i].path, to.path) == 0) {
				from = &a.row[i];
				break;
			}
		changes.push_back({from, &to, to.incl - (from ? from->incl : 0.0)});
	}
	// Rows that VANISHED matter as much as rows that appeared: a scope that
	// stopped running is a change, and a diff that only walks `b` would miss it.
	for (int i = 0; i < a.rows; ++i) {
		const SnapRow& from = a.row[i];
		bool found = false;
		for (int j = 0; j < b.rows && !found; ++j)
			found = std::strcmp(b.row[j].thread, from.thread) == 0 &&
					std::strcmp(b.row[j].path, from.path) == 0;
		if (!found) changes.push_back({&from, nullptr, -from.incl});
	}
	std::ranges::sort(changes, [](const Change& x, const Change& y) {
		return std::abs(x.d) > std::abs(y.d);
	});

	int shown = 0;
	for (const Change& c : changes) {
		if (shown >= 16) break;
		if (std::abs(c.d) < 0.0005) break; // below the readout's own precision
		const char* thread = c.to ? c.to->thread : c.from->thread;
		const char* path = c.to ? c.to->path : c.from->path;
		const double x = c.from ? c.from->incl : 0.0;
		const double y = c.to ? c.to->incl : 0.0;
		say(std::format("  {:<7} {:<28} {:>7.3f} -> {:>7.3f}  {:+7.3f} ms{}", thread, path, x,
						y, c.d, !c.from ? "  (new)" : (!c.to ? "  (gone)" : "")));
		++shown;
	}
	if (shown == 0) say("  no row moved by more than 0.001 ms");
	Print("  (full diff also written to dungeon.log)");
}
#else
int DevConsole::SnapSlot(std::string_view) const { return -1; }
int DevConsole::SnapFreeSlot() { return -1; }
void DevConsole::SnapAccumulate(float) {}
void DevConsole::SnapFinish() {}
void DevConsole::SnapDiff(const Snapshot&, const Snapshot&) {}
#endif

} // namespace dungeon::game
