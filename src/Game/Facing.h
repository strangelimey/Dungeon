// ============================================================================
// Game/Facing.h - which way a facing index turns: the one statement of it.
//
// A facing is a compass index in Direction's order, N=0 E=1 S=2 W=3. The world
// compass is east = +X, north = -Z, +Y up, and a camera looks along
// (sin yaw, 0, cos yaw) (Graphics/Camera.h, which negates clip X so the view is
// not mirrored and matches the map overlay). So +1 is CLOCKWISE seen from above
// - north to east - and that is the on-screen RIGHT: TurnRight and StrafeRight
// step +1, and the left-hand roster column (slots 0 and 2) stands on the +3
// side.
//
// It used to be four copies of `(f + 1) & 3` and `+ (even ? 3 : 1)`, while
// CLAUDE.md said +1 was on-screen LEFT - a session trusting that would write
// strafe or lane code inverted. RollTest's "Facing" section pins every function
// here against the GAME camera, so a sign flip in the step, the step table, the
// yaw table, the camera's un-mirror or the action binding (ForAction, which is
// Party::BeginAction's whole decision) fails there.
//
// Pure and header-only: no world, no map, no Platform (RollTest links it).
// ============================================================================
#pragma once

#include "Core/MathTypes.h" // kPi

#include <cstddef>

namespace dungeon::game {

// One discrete party action. Party::HandleInput maps the bound keys onto these;
// the HUD's movement buttons feed them straight into Party::Act. Declared here,
// not in Party.h, so the binding below stays pure.
enum class MoveAction { Forward, Back, StrafeLeft, StrafeRight, TurnLeft, TurnRight };

} // namespace dungeon::game

namespace dungeon::game::facing {

// One quarter turn clockwise (right) and counter-clockwise (left).
constexpr int Right(int f) { return (f + 1) & 3; }
constexpr int Left(int f) { return (f + 3) & 3; }

// The grid step a facing walks: N = -Z, E = +X, S = +Z, W = -X.
constexpr int StepX(int f) {
	constexpr int kX[4] = {0, 1, 0, -1};
	return kX[f & 3];
}
constexpr int StepZ(int f) {
	constexpr int kZ[4] = {-1, 0, 1, 0};
	return kZ[f & 3];
}

// The camera yaw that looks along facing f: N = pi, E = pi/2, S = 0,
// W = -pi/2. A quarter turn RIGHT changes the yaw by kTurnRightYaw - the yaw
// FALLS as the facing climbs.
constexpr float Yaw(int f) {
	constexpr float kYaw[4] = {kPi, kPi * 0.5f, 0.0f, -kPi * 0.5f};
	return kYaw[f & 3];
}
inline constexpr float kTurnRightYaw = -kPi * 0.5f;
inline constexpr float kTurnLeftYaw = -kTurnRightYaw;

// The side of the facing a roster slot stands on: the LEFT column (slots 0 and
// 2) or the right (1 and 3) - the portraits' quadrants, and the lane a member's
// cast, throw and held light leave from.
constexpr int SlotSide(int f, std::size_t slot) { return slot % 2 == 0 ? Left(f) : Right(f); }

// What a party action does from facing f - Party::BeginAction reads nothing
// else. A STEP walks the grid step of facing `step` and keeps the facing; a
// TURN makes the facing `after` and swings the camera yaw by `yaw`, with
// `step` = -1. TurnRight and StrafeRight both go Right (+1), TurnLeft and
// StrafeLeft both go Left.
struct Move {
	int after = 0;    // the facing once the action is done
	int step = -1;    // the facing whose grid step it walks; -1 = a turn
	float yaw = 0.0f; // a turn's change of yaw
};
constexpr Move ForAction(int f, MoveAction a) {
	const int at = f & 3;
	switch (a) {
	case MoveAction::Forward: return {at, at, 0.0f};
	case MoveAction::Back: return {at, Right(Right(at)), 0.0f};
	case MoveAction::StrafeLeft: return {at, Left(at), 0.0f};
	case MoveAction::StrafeRight: return {at, Right(at), 0.0f};
	case MoveAction::TurnLeft: return {Left(at), -1, kTurnLeftYaw};
	case MoveAction::TurnRight: return {Right(at), -1, kTurnRightYaw};
	}
	return {at, -1, 0.0f};
}

} // namespace dungeon::game::facing
