#pragma once

namespace dungeon {

// High-resolution frame timer based on QueryPerformanceCounter.
class Timer {
public:
    Timer();

    // Advances the timer; returns the delta time in seconds, clamped to avoid
    // huge steps after debugger pauses or window drags.
    float Tick();

    double TotalSeconds() const { return m_total; }

private:
    long long m_frequency = 0;
    long long m_last = 0;
    double m_total = 0.0;
};

} // namespace dungeon
