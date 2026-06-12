// ============================================================================
// Game/LoadQueue.h — staged loading.
//
// A list of named tasks run one per rendered frame, so a loading screen can
// present between them (loading is staged, not threaded). Both the boot load
// (menu essentials) and the heavy dungeon load use one queue; the loading
// screens read Progress() and CurrentLabel() for the bar and step name. The
// caller decides when to run a task (Game gates on the loading screen having
// been presented at least once).
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dungeon::game {

class LoadQueue {
public:
	void Clear() {
		m_tasks.clear();
		m_index = 0;
	}

	void Add(std::string label, std::function<void()> task) {
		m_tasks.emplace_back(std::move(label), std::move(task));
	}

	// Runs the next task — one frame's worth of blocking work.
	void RunOne() {
		if (m_index < m_tasks.size()) m_tasks[m_index++].second();
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
		return m_index < m_tasks.size() ? std::string_view(m_tasks[m_index].first)
										: std::string_view(m_doneLabel);
	}

private:
	std::vector<std::pair<std::string, std::function<void()>>> m_tasks;
	std::string m_doneLabel;
	size_t m_index = 0;
};

} // namespace dungeon::game
