// ============================================================================
// Game/LightProfile.h - what a light LOOKS like, as data (lighting-updates
// Phase 2, docs/lighting-updates-plan.md).
//
// Every light in the dungeon is described by a named PROFILE in the project's
// lights.cat: its colour, how bright, how far it reaches, how it pulses, how
// far its origin wanders (the fires' dancing shadows) and whether it casts a
// shadow at all. A SOURCE names its profile - fixtures.cat `light =
// fire_sconce`, items.cat `light = torch`, effects.cat `light = burning` - so
// a sconce is orange-red because its TYPE says so, not because one global
// palette did. Nothing here knows what a sconce is.
//
// PURE, like Defense.h and Style.h: Core types and the standard library, no
// catalog, no file layer, no world. That is what lets RollTest link it and
// measure the shipping pulse maths rather than a copy. The adapter that reads
// a catalog entry into a Profile hands Parse a field lookup.
//
// UNITS: `radius` and `wander` are SQUARES (1.0 = one dungeon square), like
// every authored size in the project (CLAUDE.md "SCALE"); the world multiplies
// kCellSize in at the one place a profile becomes a gfx::PointLight.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game::light {

// How a light's brightness moves over time. `rate` scales the pulse's base
// frequencies and `depth` is how far it dips, 0..1:
//   steady  - never moves.
//   flicker - a product of two sines (fire): between 1 - 2 x depth and 1.
//   breathe - one slow sine, 1 +/- depth (a rune's glow).
//   strobe  - full for a short beat each 1 / rate seconds, 1 - depth between.
//   storm   - mostly 1 - depth, with sudden aperiodic flashes to 1.
enum class Pulse : u8 { Steady, Flicker, Breathe, Strobe, Storm };

const char* PulseName(Pulse pulse);
// False (and `out` untouched) for a word that names no pulse.
bool ParsePulse(std::string_view word, Pulse& out);

struct Profile {
	std::string id;
	Vec3 color{1.0f, 1.0f, 1.0f};
	// `color = source`: the light takes its SOURCE'S colour (a burn's school, a
	// rune's element) instead of one of its own.
	bool sourceColor = false;
	float intensity = 1.0f;
	float radius = 3.0f; // squares
	Pulse pulse = Pulse::Steady;
	float pulseRate = 1.0f;
	float pulseDepth = 0.1f;
	float wander = 0.0f; // squares the origin dances (0 = still)
	bool shadow = true;  // may take a shadow cube
	bool longFade = false; // its shadow fades over most of its reach (braziers)
};

// Reads one profile. `get(key)` returns the field's text, empty when absent;
// an absent field keeps the default above. Anything unreadable is described
// in `problems` (one line each) and the default is kept, so a typo dims one
// light instead of failing the load.
Profile Parse(std::string id, const std::function<std::string(std::string_view)>& get,
			  std::vector<std::string>* problems = nullptr);

// The pulse multiplier at `time` (seconds) for a light whose phase is `phase`.
float PulseAt(Pulse pulse, float rate, float depth, float time, float phase);

// How far the origin has wandered at `time`, in squares (zero when wander is 0).
Vec3 WanderAt(float wander, float time, float phase);

// A profile at a moment: brightness (intensity x pulse) and the origin's
// offset, in squares. Colour and radius need no evaluation.
struct Sample {
	float intensity = 0.0f;
	Vec3 offset{};
};
Sample Evaluate(const Profile& profile, float time, float phase);

// The profile used when a source names one the project does not have: plain
// warm firelight, so a missing entry is visibly a light rather than darkness.
const Profile& Fallback();

} // namespace dungeon::game::light
