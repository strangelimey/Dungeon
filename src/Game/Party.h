// ============================================================================
// Game/Party.h — grid-locked party movement, Grimrock-style.
//
// The party occupies exactly one cell and faces one of four directions
// (0 = north/-Z, 1 = east/+X, 2 = south/+Z, 3 = west/-X). Moves and turns
// are discrete — the LOGICAL position snaps instantly, then the VISUAL
// position/yaw interpolates over ~0.3s (smoothstep + head bob), which is
// what gives the genre its feel. One action at a time; blocked attempts set
// a short cooldown so holding a key doesn't spam bump feedback.
//
// The Party knows the map (walls) but not monsters; the Game injects
// occupancy checks through the isOccupied callback.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/DungeonMap.h"
#include "Platform/Input.h"

#include <functional>

namespace dungeon::game {

// User-configurable movement keys (Win32 virtual-key codes; defaults are the
// classic QWEASD layout). The Game owns the master copy — persisted in
// settings.ini and edited on the Settings → Game tab — and pushes it here
// via SetKeys.
struct MoveKeys {
	int forward = 'W';
	int back = 'S';
	int strafeLeft = 'A';
	int strafeRight = 'D';
	int turnLeft = 'Q';
	int turnRight = 'E';
};

class Party {
public:
	Party(const DungeonMap& map, int x, int z);

	// Snaps the party back to a cell facing south, clearing all interpolation
	// state. Callbacks stay wired — used by "Start New Game".
	void Reset(int x, int z);

	void HandleInput(const Input& input);
	void Update(float dt);

	// Pace multiplier from the slowest party member (1 = baseline). Scales
	// both the step rate (shortens/stretches kMoveDuration) and kTurnSpeed.
	void SetSpeed(float speed) { m_speed = speed; }

	void SetKeys(const MoveKeys& keys) { m_moveKeys = keys; }

	// Camera pose (eye position includes a subtle head bob while moving).
	Vec3 EyePosition() const;
	float Yaw() const { return m_currentYaw; }

	int GridX() const { return m_x; }
	int GridZ() const { return m_z; }
	int Facing() const { return m_facing; }
	bool IsMoving() const { return m_moving || m_turning; }

	static const char* FacingName(int facing);

	// Fired on completed steps / blocked moves so the game can play sounds
	// and log messages.
	std::function<void()> onStep;
	std::function<void()> onBlocked;
	std::function<void()> onTurn;

	// Extra occupancy check (monsters, props). Return true to block the step;
	// the callback is responsible for its own feedback (onBlocked not fired).
	std::function<bool(int, int)> isOccupied;

private:
	bool TryStep(int dx, int dz);

	const DungeonMap& m_map;
	int m_x, m_z;
	int m_facing = 2; // start facing south (into the dungeon)

	// Interpolation state.
	Vec3 m_currentPos;
	Vec3 m_targetPos;
	bool m_moving = false;
	float m_moveT = 0.0f;
	Vec3 m_moveFrom;

	float m_currentYaw = 0.0f;
	float m_targetYaw = 0.0f;
	bool m_turning = false;

	float m_bobPhase = 0.0f;
	float m_blockCooldown = 0.0f; // throttles repeated blocked-move feedback
	float m_speed = 1.0f;         // pace multiplier (slowest member)
	MoveKeys m_moveKeys;
};

} // namespace dungeon::game
