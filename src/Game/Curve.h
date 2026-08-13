// ============================================================================
// Game/Curve.h — diminishing-returns curves: skill level -> roll bonus.
//
// In Rolemaster each point spent on a skill gave +5 to the attack roll, up to
// a point, after which diminishing returns set in. THIS GAME SPENDS NO POINTS
// — a skill rises slowly and continuously by use (Character::skillXp, level =
// sqrt(xp)) — so the taper cannot be a per-rank table. It has to be a curve
// over a continuous value, which is what this is.
//
// WHY THE SHAPE IS A KNOB AND NOT A DECISION. Three forms are offered, all
// sharing the same two meaningful knobs (initial SLOPE and a CAP), because
// which one feels right is a judgement made by looking at the graph — the
// Balance dialog draws it live — rather than an argument won in a comment:
//
//   Hyperbolic   approaches the cap and never reaches it. "Nobody is ever
//                better than +cap" is then a TRUE statement to balance around.
//   Exponential  saturates harder and earlier, then flattens almost dead.
//   Logarithmic  ignores the cap as a ceiling and keeps creeping forever, so
//                a lifetime skill always pays a little more.
//
// WHY A CAP MATTERS AT ALL: skill here is trained by use and never stops
// rising, so an unbounded bonus eventually swamps everything else in the
// formula — including the dice it is meant to argue with.
//
// AND WHY THE DICE ARE THE UNIT: tools/RollTest measures an opposed d100 at a
// combined standard deviation of ~41 points. A curve whose whole range is much
// under that is decoration — the dice decide the fight. That number is the one
// to hold these knobs against, and it is why the graph draws it as a line.
//
// Pure by design (no catalog, no game state), so RollTest compiles it straight
// in and holds the shapes to their promises.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string_view>

namespace dungeon::game {

enum class CurveForm : u8 { Hyperbolic, Exponential, Logarithmic, Count };

// Catalog/dropdown token for a form ("hyperbolic", ...) and its parse.
const char* CurveFormId(CurveForm form);
bool ParseCurveForm(std::string_view token, CurveForm& out);

struct CurveRules {
	CurveForm form = CurveForm::Hyperbolic;
	// The rise per point of input AT THE ORIGIN — the "+5 per level" figure,
	// true only for the first point and tapering from there.
	float slope = 5.0f;
	// The asymptote for the bounded forms; a scale factor for the logarithmic
	// one, which passes it rather than approaching it.
	float cap = 120.0f;
	// The input that yields ZERO. Stats want 10 here (an average stat is worth
	// nothing either way, and a poor one is a PENALTY); skills want 0, since
	// an untrained skill is simply no help rather than a handicap.
	float baseline = 0.0f;
};

// The bonus `x` earns under `rules`. ODD-SYMMETRIC about the baseline: an
// input as far below it as another is above earns the same magnitude with the
// opposite sign, so one curve covers both the gifted and the hopeless without
// a second set of knobs.
float CurveValue(float x, const CurveRules& rules);

} // namespace dungeon::game
