#pragma once

#include <chrono>

namespace dungeon {

// High-resolution frame timer (std::chrono::steady_clock is QPC-backed on
// Windows).
class Timer {
public:
    Timer() : m_last(std::chrono::steady_clock::now()) {}

    // Advances the timer; returns the delta time in seconds, clamped to avoid
    // huge steps after debugger pauses or window drags.
    float Tick();

    double TotalSeconds() const { return m_total; }

private:
    std::chrono::steady_clock::time_point m_last;
    double m_total = 0.0;
};

} // namespace dungeon
