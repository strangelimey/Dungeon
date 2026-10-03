// ============================================================================
// Game/Trail.h - what a thing in flight SHEDS, as data (lighting-updates
// Phase 4, docs/lighting-updates-plan.md).
//
// A spell bolt, a thrown flask, later a magic arrow can trail particles as it
// flies - fire's rising embers, water's falling droplets, air's swirling motes,
// earth's falling grit. Each look is a named TRAIL in the project's trails.cat;
// a source names one (spells.cat `trail = <id>`, items.cat `trail = <id>`) or
// takes its school's default (`trail_fire` and so on). The particles go
// through the projectile system's spark pool (Game/Projectiles.h), the same
// additive billboards an impact bursts into.
//
// PURE, like LightProfile.h: Core types and the standard library only, so
// RollTest links the shipping parser and the shape defaults.
//
// UNITS: `rate` is particles per SQUARE flown - they are shed by DISTANCE, so a
// fast bolt leaves as dense a trail as a slow one; `size` is the billboard's
// half-extent and `spread` the scatter speed, both in metres (a particle is far
// smaller than a square); `fall` is m/s^2 downward, negative to rise.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game::trail {

// The kind of particle, which sets every other field's default:
//   spark - short, bright, flung back and falling (a struck flint)
//   ember - drifting up and flickering (fire)
//   mote  - hanging in the air, swirling (air)
//   puff  - a soft blob that swells as it fades (smoke, gas)
//   drip  - small and falling fast (water, grit)
enum class Shape : u8 { Spark, Ember, Mote, Puff, Drip };

const char* ShapeName(Shape shape);
// False (and `out` untouched) for a word that names no shape.
bool ParseShape(std::string_view word, Shape& out);

// Everything a projectile needs to shed one: plain numbers, no strings, so it
// is COPIED into each projectile (as its payload is) and survives a catalog
// reload under a bolt still in flight.
struct Spec {
	Shape shape = Shape::Spark;
	float rate = 0.0f;   // particles per square flown; 0 = sheds nothing
	float life = 0.35f;  // seconds
	float size = 0.05f;  // metres (half-extent)
	float spread = 0.6f; // m/s of random scatter
	float fall = 3.0f;   // m/s^2 downward; < 0 rises
	bool swell = false;  // grows as it fades (a puff)
	float flicker = 0.0f; // 0..1, how deep its brightness flickers (an ember)
	float swirl = 0.0f;  // radians a second its drift turns about the vertical
	Vec3 color{1.0f, 1.0f, 1.0f};
	// false = it takes its SOURCE'S light colour, so a trail and its light
	// always agree (the default).
	bool hasColor = false;
	bool Any() const { return rate > 0.0f; }
};

// What `shape` looks like with nothing else said (rate stays 0: a trail with
// no rate sheds nothing, so the catalog must say how dense).
Spec ShapeDefaults(Shape shape);

struct Profile {
	std::string id;
	Spec spec;
};

// Reads one profile: `shape` first (its defaults), then each field over them.
// `get(key)` returns the field's text, empty when absent. Anything unreadable
// is described in `problems` and the default kept, as lights.cat is read.
Profile Parse(std::string id, const std::function<std::string(std::string_view)>& get,
			  std::vector<std::string>* problems = nullptr);

} // namespace dungeon::game::trail
