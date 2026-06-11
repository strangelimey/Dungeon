#include "Game/Party.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
constexpr float kMoveDuration = 0.28f;  // seconds per step
constexpr float kTurnSpeed = 7.0f;      // radians/s toward the target yaw
constexpr int kDirX[4] = {0, 1, 0, -1}; // N E S W
constexpr int kDirZ[4] = {-1, 0, 1, 0};

float YawForFacing(int facing) {
	// Camera forward is (sin(yaw), 0, cos(yaw)): N=-Z, E=+X, S=+Z, W=-X.
	return kPi - static_cast<float>(facing) * (kPi * 0.5f);
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
	m_bobPhase = 0.0f;
	m_blockCooldown = 0.0f;
}

const char* Party::FacingName(int facing) {
	switch (facing & 3) {
	case 0: return "North";
	case 1: return "East";
	case 2: return "South";
	default: return "West";
	}
}

bool Party::TryStep(int dx, int dz) {
	const int nx = m_x + dx;
	const int nz = m_z + dz;
	if (!m_map.IsWalkable(nx, nz)) {
		if (onBlocked) onBlocked();
		return false;
	}
	if (isOccupied && isOccupied(nx, nz)) return false;
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
	if (IsMoving()) return; // one action at a time keeps the grid feel
	if (m_blockCooldown > 0.0f) return;

	auto step = [this](int dx, int dz) {
		if (!TryStep(dx, dz)) m_blockCooldown = 0.4f;
	};

	const int f = m_facing;
	if (input.IsKeyDown('W')) {
		step(kDirX[f], kDirZ[f]);
	} else if (input.IsKeyDown('S')) {
		step(-kDirX[f], -kDirZ[f]);
	} else if (input.IsKeyDown('A')) { // strafe left
		const int left = (f + 3) & 3;
		step(kDirX[left], kDirZ[left]);
	} else if (input.IsKeyDown('D')) { // strafe right
		const int right = (f + 1) & 3;
		step(kDirX[right], kDirZ[right]);
	} else if (input.WasKeyPressed('Q')) {
		m_facing = (m_facing + 3) & 3;
		m_targetYaw = m_currentYaw + kPi * 0.5f;
		m_turning = true;
		if (onTurn) onTurn();
	} else if (input.WasKeyPressed('E')) {
		m_facing = (m_facing + 1) & 3;
		m_targetYaw = m_currentYaw - kPi * 0.5f;
		m_turning = true;
		if (onTurn) onTurn();
	}
}

void Party::Update(float dt) {
	m_blockCooldown = std::max(0.0f, m_blockCooldown - dt);
	if (m_moving) {
		m_moveT += dt / kMoveDuration;
		if (m_moveT >= 1.0f) {
			m_moveT = 1.0f;
			m_moving = false;
			m_currentPos = m_targetPos;
		} else {
			// Smoothstep for gentle acceleration/deceleration.
			const float t = m_moveT * m_moveT * (3.0f - 2.0f * m_moveT);
			m_currentPos = Lerp(m_moveFrom, m_targetPos, t);
		}
		m_bobPhase += dt * 11.0f;
	}

	if (m_turning) {
		const float diff = m_targetYaw - m_currentYaw;
		const float step = kTurnSpeed * dt;
		if (std::fabs(diff) <= step) {
			m_currentYaw = m_targetYaw;
			m_turning = false;
		} else {
			m_currentYaw += (diff > 0 ? step : -step);
		}
	}
}

Vec3 Party::EyePosition() const {
	Vec3 eye = m_currentPos;
	eye.y = kEyeHeight;
	if (m_moving) eye.y += std::sin(m_bobPhase) * 0.035f;
	return eye;
}

} // namespace dungeon::game
