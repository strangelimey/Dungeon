// ============================================================================
// Game/LoadQueue.h — staged loading.
//
// A list of named tasks run one per rendered frame, so a loading screen can
// present between them (loading is staged, not threaded). Both the boot load
// (menu essentials) and the heavy dungeon load use one queue; the loading
// screens read Progress() and CurrentLabel() for the bar and step name. The
// caller decides when to run a task (Game gates on the loading screen having
// been presented at least once).
//
// Each task is also MEASURED — wall time, allocations, bytes (Core/AllocTrack)
// — because load-time cost was the one part of the memory strategy nobody had
// ever put a number on. One task per frame is exactly the seam for it: the
// tasks are already named and already isolated from each other. Stats cover
// the CURRENT queue only; Clear() drops them, so after a level transition the
// table describes that transition.
//
// Two names per task: the LABEL is localized and shown to the player, the DEV
// NAME is English and appears in the log (dev-facing text stays English, and
// three tasks legitimately share one player-facing label).
// ============================================================================
#pragma once

#include "Core/AllocTrack.h"
#include "Core/Types.h"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dungeon::game {

class LoadQueue {
public:
	// What one task cost. Allocation counts are main-thread only, which is what
	// a staged load is — a task that spawns work elsewhere is not measured here.
	struct TaskStat {
		std::string name; // dev name, or the label when a task gave none
		double ms = 0.0;
		u64 allocs = 0;
		u64 bytes = 0;
	};

	void Clear() {
		m_tasks.clear();
		m_stats.clear();
		m_index = 0;
	}

	void Add(std::string label, std::function<void()> task, std::string devName = {}) {
		m_tasks.push_back({std::move(label), std::move(devName), std::move(task)});
	}

	// Runs the next task — one frame's worth of blocking work.
	void RunOne() {
		if (m_index >= m_tasks.size()) return;
		const Task& task = m_tasks[m_index];

		const alloc::Counters before = alloc::ThisThread();
		const auto start = std::chrono::steady_clock::now();
		task.fn();
		const auto end = std::chrono::steady_clock::now();
		const alloc::Counters after = alloc::ThisThread();

		// Recorded after the measurement, so this push is not in its own numbers.
		m_stats.push_back({task.devName.empty() ? task.label : task.devName,
						   std::chrono::duration<double, std::milli>(end - start).count(),
						   after.allocs - before.allocs, after.bytes - before.bytes});
		++m_index;
	}

	bool Done() const { return m_index == m_tasks.size(); }

	float Progress() const {
		return m_tasks.empty() ? 1.0f
							   : static_cast<float>(m_index) /
									 static_cast<float>(m_tasks.size());
	}

	// The closing line CurrentLabel shows once every task has run; localized
	// by whoever builds the queue (this header stays loc-free).
	void SetDoneLabel(std::string label) { m_doneLabel = std::move(label); }

	// The step name the loading screens show; the closing line once done.
	std::string_view CurrentLabel() const {
		return m_index < m_tasks.size() ? std::string_view(m_tasks[m_index].label)
										: std::string_view(m_doneLabel);
	}

	// One entry per task that has RUN, in run order.
	const std::vector<TaskStat>& Stats() const { return m_stats; }

private:
	struct Task {
		std::string label;   // localized, shown on the loading screen
		std::string devName; // English, shown in the log
		std::function<void()> fn;
	};

	std::vector<Task> m_tasks;
	std::vector<TaskStat> m_stats;
	std::string m_doneLabel;
	size_t m_index = 0;
};

} // namespace dungeon::game
