#pragma once

#include "Core/MathTypes.h"
#include "Game/DungeonMap.h"
#include "Platform/Input.h"

#include <functional>

namespace dungeon::game {

// Grid-locked party with smooth interpolated movement, Grimrock-style.
// Facing: 0 = north (-Z), 1 = east (+X), 2 = south (+Z), 3 = west (-X).
class Party {
public:
    Party(const DungeonMap& map, int x, int z);

    void HandleInput(const Input& input);
    void Update(float dt);

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
};

} // namespace dungeon::game
