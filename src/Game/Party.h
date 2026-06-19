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

#include "Core/Easing.h"
#include "Core/MathTypes.h"
#include "Game/DungeonMap.h"
#include "Platform/Input.h"

#include <functional>
#include <optional>

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

// One discrete party action. HandleInput maps the bound keys onto these; the
// HUD's movement buttons feed them straight into Party::Act.
enum class MoveAction { Forward, Back, StrafeLeft, StrafeRight, TurnLeft, TurnRight };

class Party {
public:
	Party(const DungeonMap& map, int x, int z);

	// Snaps the party back to a cell facing south, clearing all interpolation
	// state. Callbacks stay wired — used by "Start New Game".
	void Reset(int x, int z);

	// Teleports to a cell, keeping the current facing, clearing interpolation.
	// Returns false (and does nothing) if the cell is not walkable — used by
	// the dev console's `tp` command.
	bool SetGridPosition(int x, int z);

	// Snaps to a compass facing (0=N 1=E 2=S 3=W), no turn animation — the dev
	// console's `face` command.
	void SetFacing(int facing);

	// Dev console `noclip`: when set, TryStep ignores walls and occupancy.
	void SetNoclip(bool on) { m_noclip = on; }
	bool Noclip() const { return m_noclip; }

	void HandleInput(const Input& input);
	// Requests one discrete action under the same rules as a key press:
	// ignored while a move or turn is in flight or during the blocked-move
	// cooldown, so a mashed HUD button can't outrun the grid.
	void Act(MoveAction action);
	void Update(float dt);

	// Pace multiplier from the slowest party member (1 = baseline). Scales
	// both the step rate (shortens/stretches kMoveDuration) and kTurnSpeed.
	void SetSpeed(float speed) { m_speed = speed; }

	void SetKeys(const MoveKeys& keys) { m_moveKeys = keys; }

	// Easing curves for the visual move/turn tweens (default EaseInOut — the
	// gentle slow-start/slow-stop that gives the grid step its weight).
	void SetMoveEasing(Easing e) { m_moveEasing = e; }
	void SetTurnEasing(Easing e) { m_turnEasing = e; }

	// Camera pose (eye position includes a subtle head bob while moving).
	Vec3 EyePosition() const;
	float Yaw() const { return m_currentYaw; }

	// --- free-look (right-mouse "mouse look") -----------------------------------
	// The camera yaw/pitch the renderer should use: the grid-snapped facing yaw
	// plus the transient free-look offset (EyeYaw), and the look pitch (EyePitch,
	// 0 unless looking). The compass/HUD still read Yaw()/Facing() (the grid pose).
	float EyeYaw() const { return m_currentYaw + m_lookYaw; }
	float EyePitch() const { return m_lookPitch; }

	// Begin/end a free-look drag (right mouse button down/up). The offset PARKS
	// where the player leaves it — releasing the button does NOT snap the view
	// back (so an off-axis grab to reach an awkward item stays put). Only a
	// movement/turn action starts the ease back to orthogonal (BeginAction).
	void BeginLook() { m_looking = true; m_returning = false; }
	void EndLook() { m_looking = false; } // parks the offset; no auto-return
	bool IsLooking() const { return m_looking; }
	// Raw free-look offset (radians) for the save layer — the parked angle the
	// view is swung off the grid facing, plus whether a look drag is in progress.
	float LookYaw() const { return m_lookYaw; }
	float LookPitch() const { return m_lookPitch; }
	// Restores the saved free-look offset on top of the current facing (call AFTER
	// SetFacing, which clears it). The offset is parked (not returning), so the
	// loaded view sits at the exact saved angle until the player moves.
	void SetLookState(float yaw, float pitch, bool looking) {
		m_lookYaw = yaw;
		m_lookPitch = pitch;
		m_looking = looking;
		m_returning = false;
	}
	// Accumulate a mouse drag into the look offset (radians). Pitch is clamped;
	// yaw past ±kLookSnap snaps the ordinal facing one quarter and folds the
	// inverse back into the offset, so the view stays continuous while the grid
	// facing follows — the player can look (and walk) around corners.
	void AddLook(float dYaw, float dPitch);

	int GridX() const { return m_x; }
	int GridZ() const { return m_z; }
	int Facing() const { return m_facing; }
	bool IsMoving() const { return m_moving || m_turning; }

	// The compass direction as a loc:: key ("facing.north"); callers pass it
	// through loc::Tr for display.
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
	// Performs one action immediately (the grid must be free). startLinear
	// leads the tween in at constant velocity (set when replaying a buffered
	// same-kind continuation) instead of easing in from rest.
	void BeginAction(MoveAction action, bool startLinear);
	// Re-derives the in-flight tween's curve from the current buffer: a queued
	// same-kind action flattens the tail to a linear exit.
	void RefreshChainEasing();

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
	float m_turnFrom = 0.0f;
	float m_turnT = 0.0f;
	bool m_turning = false;

	// Free-look offset layered on top of the grid facing yaw. It PARKS at whatever
	// the player leaves it (releasing the mouse does nothing to it) and only eases
	// back to 0 once m_returning is set — which BeginAction does when a move/turn
	// commits, so the view migrates to orthogonal as the party sets off.
	bool m_looking = false;   // a look drag is active (accumulating mouse motion)
	bool m_returning = false; // easing the parked offset back to orthogonal
	float m_lookYaw = 0.0f;
	float m_lookPitch = 0.0f;

	float m_bobPhase = 0.0f;
	float m_blockCooldown = 0.0f; // throttles repeated blocked-move feedback
	float m_speed = 1.0f;         // pace multiplier (slowest member)
	bool m_noclip = false;        // dev console: walk through walls
	MoveKeys m_moveKeys;

	Easing m_moveEasing = Easing::EaseInOut; // base curve (overridable)
	Easing m_turnEasing = Easing::EaseInOut;

	// Single-slot input buffer: an action requested while a move/turn is in
	// flight is held here (newest overwrites) and replayed the instant the
	// grid frees up. The "active" easings are the curves the live tween is
	// actually drawn with — the base curve, or a linear-edge variant while a
	// same-kind action is chained in front of/behind it.
	std::optional<MoveAction> m_buffered;
	Easing m_activeMoveEasing = Easing::EaseInOut;
	Easing m_activeTurnEasing = Easing::EaseInOut;
	bool m_moveStartLinear = false; // current move began as a linear continuation
	bool m_turnStartLinear = false;
};

} // namespace dungeon::game
