// ============================================================================
// Graphics/FrameRate.h - a monitor's refresh rate as the fraction it is, and
// the ONE formula for what a present interval makes of it (code-review C200).
//
// The frame cap used to work in whole hertz: the refresh was DEVMODE's
// dmDisplayFrequency, which reports 59.94 Hz as 59, and the cap divided that by
// the present interval in integers, so 165 Hz at interval 2 capped at 82 against
// a real 82.5. A cap slower than the display leaves a vblank with no new frame
// every second or two, and the last one repeats - a hitch. The Video tab's
// labels rounded instead (83), so the two did not even agree with each other.
//
// Now the refresh is the display's exact RATIONAL rate (QueryDisplayConfig's
// numerator / denominator: 60000/1001 for "59.94"), and the cap's slice, the
// Frame Rate labels, the `framecap` readout and the console's FPS ceiling all go
// through the functions below. The cap aims a hair FAST (kCapBias): the cap then
// never starves a vblank, and on a display that does pace the swapchain the
// vblank sets the rate, not our clock.
//
// PURE - Core/Types.h and the standard library - so RollTest links it and checks
// the arithmetic (the Defense.h bargain). GraphicsDevice reads the rate.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <cstdio>
#include <string>

namespace dungeon::gfx {

// A refresh rate as the display reports it: numerator / denominator hertz. A
// zero numerator or denominator is "unknown".
struct RefreshRate {
	u32 numerator = 0;
	u32 denominator = 1;

	bool Valid() const { return numerator > 0 && denominator > 0; }
	double Hz() const {
		return Valid() ? static_cast<double>(numerator) / static_cast<double>(denominator) : 0.0;
	}
};

// How much faster than the display the cap aims: a slice 0.1% short. A clock
// drift between the CPU's counter and the display's is parts per million, so
// this always clears it, and it costs at most one frame in a thousand drawn
// and not shown on a desktop whose compositor runs faster than the window's
// monitor (the case the cap exists for, GraphicsDevice::WaitFrameCap).
inline constexpr double kCapBias = 0.001;

// The frame rate a present interval (every Nth vblank, 1..4) gives - what the
// Frame Rate dropdown names and the cap aims at. Zero for an unknown rate.
inline double FrameRateFor(RefreshRate rate, u32 interval) {
	if (!rate.Valid()) return 0.0;
	const u32 n = interval < 1 ? 1 : interval;
	return rate.Hz() / static_cast<double>(n);
}

// The cap's slice in counter ticks (`ticksPerSecond` = the QPC frequency):
// ticksPerSecond * interval * denominator / numerator, biased fast by kCapBias.
// Zero for an unknown rate or counter, which the cap reads as "do not cap".
inline i64 CapSliceTicks(RefreshRate rate, u32 interval, i64 ticksPerSecond) {
	const double fps = FrameRateFor(rate, interval);
	if (fps <= 0.0 || ticksPerSecond <= 0) return 0;
	return static_cast<i64>(static_cast<double>(ticksPerSecond) / fps * (1.0 - kCapBias));
}

// A frame rate as the labels write it: to two decimals with the zeros after
// the point dropped - "60", "59.94", "82.5". One spelling for the Video tab, the
// console's FPS ceiling and the `framecap` readout.
inline std::string FrameRateText(double fps) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f", fps < 0.0 ? 0.0 : fps);
	std::string out(text);
	while (!out.empty() && out.back() == '0') out.pop_back();
	if (!out.empty() && out.back() == '.') out.pop_back();
	return out;
}

} // namespace dungeon::gfx
