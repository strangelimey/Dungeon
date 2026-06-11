#include "Platform/Input.h"

namespace dungeon {

void Input::OnKey(int vkey, bool down) {
    vkey &= 0xFF;
    if (down && !m_keys[vkey]) m_keysPressed[vkey] = true;
    if (!down && m_keys[vkey]) m_keysReleased[vkey] = true;
    m_keys[vkey] = down;
}

void Input::OnMouseButton(MouseButton b, bool down) {
    const auto i = std::to_underlying(b);
    if (down && !m_mouse[i]) m_mousePressed[i] = true;
    if (!down && m_mouse[i]) m_mouseReleased[i] = true;
    m_mouse[i] = down;
}

void Input::OnMouseMove(float x, float y) {
    m_mouseX = x;
    m_mouseY = y;
}

void Input::OnWheel(float delta) { m_wheel += delta; }

void Input::EndFrame() {
    m_keysPressed.fill(false);
    m_keysReleased.fill(false);
    m_mousePressed.fill(false);
    m_mouseReleased.fill(false);
    m_wheel = 0.0f;
}

} // namespace dungeon
