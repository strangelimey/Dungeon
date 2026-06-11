#include "Core/Time.h"

#include <Windows.h>

#include <algorithm>

namespace dungeon {

Timer::Timer() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    m_frequency = freq.QuadPart;
    m_last = now.QuadPart;
}

float Timer::Tick() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = static_cast<double>(now.QuadPart - m_last) / static_cast<double>(m_frequency);
    m_last = now.QuadPart;
    dt = std::clamp(dt, 0.0, 0.1);
    m_total += dt;
    return static_cast<float>(dt);
}

} // namespace dungeon
