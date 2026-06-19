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

// User-tunable right-mouse free-look feel (Settings → Controls; ini look_*).
// The Game owns the master copy and pushes it into the Party via SetLook;
// sensitivity is read by the Game's drag handler, the rest by the Party's
// return animation. Defaults match the values dialled in during playtesting.
struct LookSettings {
	float sensitivity = 1.0f; // multiplier on the base drag rate (rad/pixel)
	float returnHold = 0.8f;  // seconds the view holds still before easing back
	float returnTime = 2.0f;  // seconds for the hands-off return to orthogonal
	float moveTime = 0.3f;    // seconds for the quicker move-triggered straighten
	Easing snapEasing = Easing::EaseInCubic; // hands-off return curve
	Easing moveEasing = Easing::EaseInOut;   // move-triggered straighten curve
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

	// Free-look feel (hold/return durations + curves); pushed from GameSettings.
	void SetLook(const LookSettings& look) { m_look = look; }

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

	// Begin/end a free-look drag (right mouse button down/up). Releasing the
	// button starts a gentle auto-return (StartReturn): the view lingers near
	// where it was left — long enough to click an awkward item off the floor —
	// then sweeps back to orthogonal (slow-in, fast-out). A movement/turn action
	// triggers the same return (BeginAction); re-grabbing (BeginLook) cancels it.
	void BeginLook() { m_looking = true; m_returning = false; }
	void EndLook() { m_looking = false; StartReturn(false); } // release: slow return
	bool IsLooking() const { return m_looking; }
	// Raw free-look offset (radians) for the save layer — the parked angle the
	// view is swung off the grid facing, plus whether a look drag is in progress.
	float LookYaw() const { return m_lookYaw; }
	float LookPitch() const { return m_lookPitch; }
	// Restores the saved free-look offset on top of the current facing (call AFTER
	// SetFacing, which clears it). The offset is parked (not returning), so the
	// loaded view sits at the exact saved angle until the player looks or moves.
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
	// Fired once per blocked move, at the instant the recoil lunge reaches its
	// peak (~30% toward the obstacle) and turns back — the "impact". The game
	// uses it to jar the party (small damage + portrait splat + grunt).
	std::function<void()> onBumpImpact;

	// Extra occupancy check (monsters, props). Return true to block the step;
	// the callback is responsible for its own feedback (onBlocked not fired).
	std::function<bool(int, int)> isOccupied;

private:
	bool TryStep(int dx, int dz);
	// Kicks off the blocked-move recoil toward cell (bx,bz) from the current
	// resting position. No-op if a bump is already running.
	void StartBump(int bx, int bz);
	// Performs one action immediately (the grid must be free). startLinear
	// leads the tween in at constant velocity (set when replaying a buffered
	// same-kind continuation) instead of easing in from rest.
	void BeginAction(MoveAction action, bool startLinear);
	// Re-derives the in-flight tween's curve from the current buffer: a queued
	// same-kind action flattens the tail to a linear exit.
	void RefreshChainEasing();
	// Begins the cubic ease of the free-look offset back to orthogonal. fast=false
	// is the slow hands-off return (release: dwell, then sweep); fast=true is the
	// quick walking-straighten (move/turn). A fast request OVERTAKES an in-flight
	// slow return; no-op if already square-on, or if an equal/faster return runs.
	void StartReturn(bool fast);

	const DungeonMap& m_map;
	int m_x, m_z;
	int m_facing = 2; // start facing south (into the dungeon)

	// Interpolation state.
	Vec3 m_currentPos;
	Vec3 m_targetPos;
	bool m_moving = false;
	float m_moveT = 0.0f;
	Vec3 m_moveFrom;

	// Blocked-move recoil ("bump"). The logical position never changes; the
	// VISUAL position lunges toward the blocked cell at the normal step rate,
	// reaches ~30% of the way, then bounces rapidly back to the start cell. Two
	// phases: forward lunge (m_bumpPhase 0, replays the real step curve until the
	// eased distance hits kBumpPeak) then the bounce return (phase 1).
	bool m_bumping = false;
	int m_bumpPhase = 0;     // 0 = forward lunge, 1 = bounce back
	float m_bumpT = 0.0f;    // forward: normal step progress; back: 0..1 return
	float m_bumpPeak = 0.0f; // eased distance reached when the lunge handed off
	Vec3 m_bumpTo;           // blocked cell center (the lunge direction/distance)

	float m_currentYaw = 0.0f;
	float m_targetYaw = 0.0f;
	float m_turnFrom = 0.0f;
	float m_turnT = 0.0f;
	bool m_turning = false;

	// Free-look offset layered on top of the grid facing yaw. When the player
	// stops looking (EndLook) or commits a move/turn (BeginAction) it eases back
	// to 0 over kLookReturnTime with a slow-in/fast-out curve: m_returnT is the
	// 0..1 tween progress, m_returnFrom* the offset captured when the return began.
	bool m_looking = false;   // a look drag is active (accumulating mouse motion)
	bool m_returning = false; // easing the offset back to orthogonal
	float m_lookYaw = 0.0f;
	float m_lookPitch = 0.0f;
	float m_returnT = 0.0f;
	float m_returnTime = 1.0f;                 // active return duration (release vs move)
	float m_returnHold = 0.0f;                 // pause left before the return tween eases
	Easing m_returnEasing = Easing::EaseInOut; // active return curve (shared EaseLerp)
	float m_returnFromYaw = 0.0f;
	float m_returnFromPitch = 0.0f;

	float m_bobPhase = 0.0f;
	float m_blockCooldown = 0.0f; // throttles repeated blocked-move feedback
	float m_speed = 1.0f;         // pace multiplier (slowest member)
	bool m_noclip = false;        // dev console: walk through walls
	MoveKeys m_moveKeys;
	LookSettings m_look; // free-look feel (durations + curves), set from settings

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
