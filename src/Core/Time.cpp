#include "Core/Time.h"

#include <algorithm>

namespace dungeon {

float Timer::Tick() {
	const auto now = std::chrono::steady_clock::now();
	double dt = std::chrono::duration<double>(now - m_last).count();
	m_last = now;
	dt = std::clamp(dt, 0.0, 0.1);
	m_total += dt;
	return static_cast<float>(dt);
}

} // namespace dungeon
