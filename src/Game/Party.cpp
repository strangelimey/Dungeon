#include "Game/Party.h"

#include "Core/Easing.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
constexpr float kMoveDuration = 0.28f;  // seconds per step
constexpr float kTurnDuration = 0.25f;  // seconds per 90° turn
constexpr int kDirX[4] = {0, 1, 0, -1}; // N E S W
constexpr int kDirZ[4] = {-1, 0, 1, 0};

// Free-look tuning. kLookSnap is the yaw the view may swing past the grid
// facing before that facing snaps one quarter (45° = halfway to the next
// cardinal). kLookPitchMax clamps the up/down look. kLookReturn is the
// exponential rate the offset relaxes back to 0 once the player stops looking.
constexpr float kLookSnap = kPi * 0.25f;
constexpr float kLookPitchMax = kPi * 0.40f;
constexpr float kLookReturn = 12.0f;

float YawForFacing(int facing) {
	// Camera forward is (sin(yaw), 0, cos(yaw)): N=-Z, E=+X, S=+Z, W=-X.
	return kPi - static_cast<float>(facing) * (kPi * 0.5f);
}

bool IsTurnAction(MoveAction a) {
	return a == MoveAction::TurnLeft || a == MoveAction::TurnRight;
}

// Picks the tween curve for a segment given whether its start and/or end abut a
// same-kind neighbour (a chained step). A linear edge has the segment's average
// velocity, so two linear edges meet without a velocity break; otherwise fall
// back to the caller's base curve (the gentle EaseInOut by default).
Easing ChainEasing(bool startLinear, bool endLinear, Easing base) {
	if (startLinear && endLinear) return Easing::Linear;
	if (startLinear) return Easing::LinearStart;
	if (endLinear) return Easing::LinearEnd;
	return base;
}
} // namespace

Party::Party(const DungeonMap& map, int x, int z) : m_map(map), m_x(x), m_z(z) {
	m_currentPos = m_targetPos = m_moveFrom = map.CellCenter(x, z);
	m_currentYaw = m_targetYaw = YawForFacing(m_facing);
}

void Party::Reset(int x, int z) {
	m_x = x;
	m_z = z;
	m_facing = 2; // south, into the dungeon
	m_currentPos = m_targetPos = m_moveFrom = m_map.CellCenter(x, z);
	m_currentYaw = m_targetYaw = YawForFacing(m_facing);
	m_moving = false;
	m_turning = false;
	m_moveT = 0.0f;
	m_turnT = 0.0f;
	m_bobPhase = 0.0f;
	m_blockCooldown = 0.0f;
	m_buffered.reset();
	m_looking = false;
	m_returning = false;
	m_lookYaw = m_lookPitch = 0.0f;
}

void Party::SetFacing(int facing) {
	m_facing = facing & 3;
	m_currentYaw = m_targetYaw = YawForFacing(m_facing);
	m_turning = false;
	m_buffered.reset();
	m_looking = false;
	m_returning = false;
	m_lookYaw = m_lookPitch = 0.0f;
}

bool Party::SetGridPosition(int x, int z) {
	if (!m_map.IsWalkable(x, z)) return false;
	m_x = x;
	m_z = z;
	m_currentPos = m_targetPos = m_moveFrom = m_map.CellCenter(x, z);
	m_moving = false;
	m_turning = false;
	m_moveT = 0.0f;
	m_turnT = 0.0f;
	m_blockCooldown = 0.0f;
	m_buffered.reset();
	return true;
}

const char* Party::FacingName(int facing) {
	switch (facing & 3) {
	case 0: return "facing.north";
	case 1: return "facing.east";
	case 2: return "facing.south";
	default: return "facing.west";
	}
}

void Party::AddLook(float dYaw, float dPitch) {
	if (!m_looking) return;
	m_lookPitch = std::clamp(m_lookPitch + dPitch, -kLookPitchMax, kLookPitchMax);
	m_lookYaw += dYaw;

	// Swinging the view past the half-quarter snaps the grid facing one quarter
	// in that direction and folds the inverse back into the offset, so the camera
	// yaw (m_currentYaw + m_lookYaw) is unchanged at the seam — the view glides on
	// while the ordinal facing has rotated under it. A loop covers a big delta.
	while (m_lookYaw >= kLookSnap) { // looked left past 45° -> turn left
		m_facing = (m_facing + 3) & 3;
		m_currentYaw += kPi * 0.5f;
		m_lookYaw -= kPi * 0.5f;
		m_targetYaw = m_currentYaw;
		m_turning = false;
		if (onTurn) onTurn();
	}
	while (m_lookYaw <= -kLookSnap) { // looked right past 45° -> turn right
		m_facing = (m_facing + 1) & 3;
		m_currentYaw -= kPi * 0.5f;
		m_lookYaw += kPi * 0.5f;
		m_targetYaw = m_currentYaw;
		m_turning = false;
		if (onTurn) onTurn();
	}
}

bool Party::TryStep(int dx, int dz) {
	const int nx = m_x + dx;
	const int nz = m_z + dz;
	if (!m_noclip) {
		if (!m_map.IsWalkable(nx, nz)) {
			if (onBlocked) onBlocked();
			return false;
		}
		if (isOccupied && isOccupied(nx, nz)) return false;
	}
	m_x = nx;
	m_z = nz;
	m_moveFrom = m_currentPos;
	m_targetPos = m_map.CellCenter(m_x, m_z);
	m_moving = true;
	m_moveT = 0.0f;
	if (onStep) onStep();
	return true;
}

void Party::HandleInput(const Input& input) {
	// One action per frame, priority by listing order (matches the old else-if
	// chain). Movement keys repeat while held (the idle branch below); turn
	// keys fire once per press. A key that lands mid-motion is queued through
	// Act's buffer, but only on a FRESH press — a held key must not keep
	// re-arming a stale buffered step that would fire after release.
	const struct {
		int vk;
		MoveAction act;
	} binds[] = {
		{m_moveKeys.forward, MoveAction::Forward},
		{m_moveKeys.back, MoveAction::Back},
		{m_moveKeys.strafeLeft, MoveAction::StrafeLeft},
		{m_moveKeys.strafeRight, MoveAction::StrafeRight},
		{m_moveKeys.turnLeft, MoveAction::TurnLeft},
		{m_moveKeys.turnRight, MoveAction::TurnRight},
	};
	for (const auto& b : binds) {
		const bool fresh = input.WasKeyPressed(b.vk);
		const bool active = IsTurnAction(b.act) ? fresh : input.IsKeyDown(b.vk);
		if (!active) continue;
		if (IsMoving()) {
			if (fresh) Act(b.act); // queue only a fresh press
		} else {
			Act(b.act); // grid free: start now
		}
		break;
	}
}

void Party::Act(MoveAction action) {
	if (IsMoving()) {
		// A move/turn is in flight: queue this into the single-slot buffer (a
		// newer key overwrites an older queued one) and replay it the instant
		// the current motion finishes (see Update). Queuing a same-kind action
		// also flattens the live tween's tail so it doesn't brake at the seam.
		m_buffered = action;
		RefreshChainEasing();
		return;
	}
	if (m_blockCooldown > 0.0f) return;
	BeginAction(action, false);
}

void Party::BeginAction(MoveAction action, bool startLinear) {
	// Committing to a discrete move/turn ends any active look AND arms the return:
	// the parked offset now eases to 0 (Update) so the view migrates to orthogonal
	// as the party sets off. (Releasing the mouse alone never arms this.)
	m_looking = false;
	m_returning = true;
	auto stepStart = [&](int dx, int dz) {
		if (!TryStep(dx, dz)) {
			m_blockCooldown = 0.4f;
			return;
		}
		m_moveStartLinear = startLinear;
		m_activeMoveEasing = ChainEasing(startLinear, false, m_moveEasing);
	};
	auto turnStart = [&](float deltaYaw) {
		m_turnFrom = m_currentYaw;
		m_targetYaw = m_currentYaw + deltaYaw;
		m_turning = true;
		m_turnT = 0.0f;
		m_turnStartLinear = startLinear;
		m_activeTurnEasing = ChainEasing(startLinear, false, m_turnEasing);
		if (onTurn) onTurn();
	};

	const int f = m_facing;
	switch (action) {
	case MoveAction::Forward:
		stepStart(kDirX[f], kDirZ[f]);
		break;
	case MoveAction::Back:
		stepStart(-kDirX[f], -kDirZ[f]);
		break;
	case MoveAction::StrafeLeft: { // strafe left
		// The camera is un-mirrored (Camera::ViewProj), so facing turns the
		// natural way: +1 is clockwise (the on-screen RIGHT direction), -1
		// (== +3) is counter-clockwise (left). Compass offsets apply directly.
		const int left = (f + 3) & 3;
		stepStart(kDirX[left], kDirZ[left]);
		break;
	}
	case MoveAction::StrafeRight: { // strafe right
		const int right = (f + 1) & 3;
		stepStart(kDirX[right], kDirZ[right]);
		break;
	}
	case MoveAction::TurnLeft: // turn left (counter-clockwise)
		m_facing = (m_facing + 3) & 3;
		turnStart(kPi * 0.5f);
		break;
	case MoveAction::TurnRight: // turn right (clockwise)
		m_facing = (m_facing + 1) & 3;
		turnStart(-kPi * 0.5f);
		break;
	}
}

void Party::RefreshChainEasing() {
	// Re-derive the in-flight tween's curve from whether a SAME-KIND action is
	// queued behind it. A matching continuation flattens the tail to a linear
	// exit so it meets the next step's linear lead-in without braking.
	if (m_moving) {
		const bool endLinear = m_buffered && !IsTurnAction(*m_buffered);
		m_activeMoveEasing = ChainEasing(m_moveStartLinear, endLinear, m_moveEasing);
	}
	if (m_turning) {
		const bool endLinear = m_buffered && IsTurnAction(*m_buffered);
		m_activeTurnEasing = ChainEasing(m_turnStartLinear, endLinear, m_turnEasing);
	}
}

void Party::Update(float dt) {
	m_blockCooldown = std::max(0.0f, m_blockCooldown - dt);

	// Free-look eases back to orthogonal only while RETURNING — armed by a move/
	// turn action, never by merely releasing the button (so a parked off-axis view
	// holds until the player walks). Exponential decay; tiny residuals clamp to 0
	// and disarm the return.
	if (m_returning) {
		const float a = 1.0f - std::exp(-kLookReturn * dt);
		m_lookYaw -= m_lookYaw * a;
		m_lookPitch -= m_lookPitch * a;
		if (std::abs(m_lookYaw) < 1e-4f) m_lookYaw = 0.0f;
		if (std::abs(m_lookPitch) < 1e-4f) m_lookPitch = 0.0f;
		if (m_lookYaw == 0.0f && m_lookPitch == 0.0f) m_returning = false;
	}

	const bool wasMoving = m_moving;
	const bool wasTurning = m_turning;

	if (m_moving) {
		m_moveT += dt * m_speed / kMoveDuration;
		if (m_moveT >= 1.0f) {
			m_moveT = 1.0f;
			m_moving = false;
			m_currentPos = m_targetPos;
		} else {
			m_currentPos = EaseLerp(m_activeMoveEasing, m_moveFrom, m_targetPos, m_moveT);
		}
		m_bobPhase += dt * 11.0f;
	}

	if (m_turning) {
		m_turnT += dt * m_speed / kTurnDuration;
		if (m_turnT >= 1.0f) {
			m_turnT = 1.0f;
			m_turning = false;
			m_currentYaw = m_targetYaw;
		} else {
			m_currentYaw = EaseLerp(m_activeTurnEasing, m_turnFrom, m_targetYaw, m_turnT);
		}
	}

	// Replay the buffered action the moment the grid frees up, in the SAME
	// frame the current motion ended so the two tweens meet with no gap. If
	// that motion exited linearly (a same-kind chain), lead the next one in
	// linearly too — Linear-End meets Linear-Start for a seamless glide.
	if (m_buffered && !IsMoving() && m_blockCooldown <= 0.0f) {
		const MoveAction next = *m_buffered;
		m_buffered.reset();
		bool startLinear = false;
		if (wasMoving) {
			startLinear =
				m_activeMoveEasing == Easing::LinearEnd || m_activeMoveEasing == Easing::Linear;
		} else if (wasTurning) {
			startLinear =
				m_activeTurnEasing == Easing::LinearEnd || m_activeTurnEasing == Easing::Linear;
		}
		BeginAction(next, startLinear);
	}
}

Vec3 Party::EyePosition() const {
	Vec3 eye = m_currentPos;
	eye.y = kEyeHeight;
	if (m_moving) eye.y += std::sin(m_bobPhase) * 0.035f;
	return eye;
}

} // namespace dungeon::game
