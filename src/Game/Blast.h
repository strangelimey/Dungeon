// ============================================================================
// Game/Blast.h — how an area effect PROPAGATES through the dungeon.
//
// Michael's model (2026-08-11). A blast is not a radius and not a flood: it is a
// WAVEFRONT that expands over TIME, and the geometry it expands into is most of
// what decides how dangerous it is.
//
//   * It detonates on one square, which takes the full effect at once.
//   * Every tick it tries to spread from its frontier into the 4 orthogonal
//     neighbours. The tick interval is the spell's EXPANSION SPEED — a fireball
//     ticks fast, a poison cloud slowly.
//   * A spread blocked by wall DEFLECTS sideways if a perpendicular square is
//     open, and REFLECTS back the way it came if neither is.
//   * Units arriving at the same square in the same tick MULTIPLY it: six units
//     converging is a x6 tick. That is what makes a confined blast devastating,
//     and it is the whole reason reflection exists rather than the force simply
//     being lost.
//   * Effect falls off with DISTANCE from the detonation — shortest open-path
//     steps, so a blast that came round a corner arrives weakened, while a
//     firewall reflecting back onto a near square is still lethal.
//   * Spread costs FORCE. When the force is spent, expansion stops.
//
// PERSISTENCE is the one per-spell axis that changes the shape of the whole thing:
//
//   TRANSIENT (fire) — the front moves on and the squares behind it VACATE. A
//       reflected unit re-entering a burned square is a second hit: the firewall
//       sweeping back down the corridor.
//   PERSISTENT (gas) — squares FILL and stay, re-applying every tick until they
//       dissipate, and a unit re-entering a filled square adds to its
//       CONCENTRATION. So poison contained is more poisonous, the same way fire
//       contained is: density instead of a returning front.
//
// Fire that "catches" needs nothing here: a transient front leaves its `on_hit`
// procs on what it touches, and a `burn` already ticks, resists and saves on its
// own through docs/effects.md. The blast simulates the front; the effects system
// simulates what the front set alight.
//
// Geometry, therefore: an open room gives an expanding ring, a dead end reflects
// into a firewall coming back at you, a T-junction splits three ways.
//
// PURE, like Game/Defense.h — no map, no catalogs, no combatants. The caller says
// which squares are open and gets back what happened, tick by tick. That is what
// lets tools/RollTest measure the propagation, and a rule this geometric is not
// worth trusting unmeasured.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <array>
#include <functional>

namespace dungeon::game::blast {

// Ceilings, not balance knobs. The propagation runs on fixed arrays so a
// detonation allocates nothing (docs/ARCHITECTURE.md "Memory strategy"), and
// persistence plus reflection plus multiplication is otherwise unbounded in a
// sealed room — which hangs a frame rather than looking wrong.
inline constexpr int kMaxCells = 64;  // squares one blast may ever occupy
inline constexpr int kMaxTicks = 32;  // expansion steps before it is cut off

// How a blast behaves once a square has been reached.
enum class Persistence : u8 {
	Transient,  // fire: the front passes through and the square behind vacates
	Persistent, // gas: the square fills, keeps biting, and can be concentrated
};

// What a blast is, as content authors it.
struct Rules {
	float damage = 0.0f;   // the full effect, at the detonation square
	float falloff = 0.0f;  // effect lost per step of distance
	int force = 0;         // total spread budget, in squares entered; 0 = not an area effect
	float rate = 0.1f;     // seconds per expansion tick — the expansion SPEED
	Persistence persistence = Persistence::Transient;
	float linger = 0.0f;   // seconds a filled square keeps biting (persistent only)

	bool Any() const { return force > 0 && damage > 0.0f; }
};

// One square being affected, on one tick.
struct Hit {
	int x = 0, z = 0;
	int tick = 0;         // which expansion step this happened on (0 = detonation)
	int distance = 0;     // shortest OPEN-PATH steps from the detonation square
	int arrivals = 1;     // units that landed here this tick — the multiplier
	float damage = 0.0f;  // what lands, falloff and arrivals applied
};

// Everything one blast does, in order. A caller walks it and applies each Hit at
// `tick * rules.rate` seconds after the detonation — so this is the whole
// propagation computed up front, and the host only has to schedule it.
//
// Computed rather than stepped live on purpose: the geometry cannot change
// mid-blast (walls do not move, and a door opening mid-blast is not worth the
// complexity), so one pass is both simpler and measurable.
struct Result {
	std::array<Hit, kMaxCells * 4> hits{}; // a square can be hit on several ticks
	int count = 0;
	int ticks = 0;        // expansion steps actually run
	int spent = 0;        // force consumed
	int leftover = 0;     // force that had nowhere to go at all
	bool clamped = false; // hit kMaxCells / kMaxTicks / the hit ceiling
};

// May the blast enter this square? A wall, off-map and a closed door all say no.
// The detonation square is asked too — a bolt that burst against stone has its
// centre inside the wall, and nothing stands there.
using PassableFn = std::function<bool(int x, int z)>;

// Propagate `rules` from (cx, cz).
Result Propagate(int cx, int cz, const Rules& rules, const PassableFn& passable);

} // namespace dungeon::game::blast
