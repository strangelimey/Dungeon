#pragma once

#include "Core/Types.h"

#include <array>

namespace dungeon {

enum class MouseButton { Left, Right, Middle, Count };

// Keyboard/mouse state with per-frame edge detection. The Window feeds this
// from its message loop; game code only reads it.
class Input {
public:
    // --- queries -----------------------------------------------------------
    bool IsKeyDown(int vkey) const { return m_keys[vkey & 0xFF]; }
    bool WasKeyPressed(int vkey) const { return m_keysPressed[vkey & 0xFF]; }
    bool WasKeyReleased(int vkey) const { return m_keysReleased[vkey & 0xFF]; }

    bool IsMouseDown(MouseButton b) const { return m_mouse[static_cast<int>(b)]; }
    bool WasMousePressed(MouseButton b) const { return m_mousePressed[static_cast<int>(b)]; }
    bool WasMouseReleased(MouseButton b) const { return m_mouseReleased[static_cast<int>(b)]; }

    float MouseX() const { return m_mouseX; }
    float MouseY() const { return m_mouseY; }
    float WheelDelta() const { return m_wheel; }

    // --- driven by Window --------------------------------------------------
    void OnKey(int vkey, bool down);
    void OnMouseButton(MouseButton b, bool down);
    void OnMouseMove(float x, float y);
    void OnWheel(float delta);

    // Clears one-frame edge state; call once per frame after the game reads input.
    void EndFrame();

private:
    std::array<bool, 256> m_keys{};
    std::array<bool, 256> m_keysPressed{};
    std::array<bool, 256> m_keysReleased{};
    std::array<bool, 3> m_mouse{};
    std::array<bool, 3> m_mousePressed{};
    std::array<bool, 3> m_mouseReleased{};
    float m_mouseX = 0.0f;
    float m_mouseY = 0.0f;
    float m_wheel = 0.0f;
};

} // namespace dungeon
