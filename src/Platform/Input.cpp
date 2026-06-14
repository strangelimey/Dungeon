#include "Platform/Input.h"

#include <Windows.h>

#include <format>

namespace dungeon {

void Input::OnKey(int vkey, bool down) {
	vkey &= 0xFF;
	if (down && !m_keys[vkey]) m_keysPressed[vkey] = true;
	if (!down && m_keys[vkey]) m_keysReleased[vkey] = true;
	m_keys[vkey] = down;
}

void Input::OnChar(unsigned int codepoint) {
	// Keep printable characters only; control codes (Enter, Backspace, Esc,
	// Tab) reach the consumer through the edge-triggered key queries instead.
	if (codepoint >= 32 && codepoint != 127)
		m_typed.push_back(static_cast<char>(codepoint & 0xFF));
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

std::string KeyName(int vkey) {
	vkey &= 0xFF;
	LONG scan = static_cast<LONG>(MapVirtualKeyA(static_cast<UINT>(vkey),
												 MAPVK_VK_TO_VSC))
				<< 16;
	switch (vkey) {
	// Navigation keys share scan codes with the numpad; without the extended
	// bit GetKeyNameText would report e.g. VK_LEFT as "Num 4".
	case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
	case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
	case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
		scan |= 1 << 24;
		break;
	}
	char name[64];
	if (scan && GetKeyNameTextA(scan, name, sizeof(name)) > 0) return name;
	return std::format("Key {:#04x}", vkey);
}

void Input::EndFrame() {
	m_keysPressed.fill(false);
	m_keysReleased.fill(false);
	m_mousePressed.fill(false);
	m_mouseReleased.fill(false);
	m_typed.clear();
	m_wheel = 0.0f;
}

} // namespace dungeon
