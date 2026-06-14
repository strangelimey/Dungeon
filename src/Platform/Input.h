// ============================================================================
// Platform/Input.h — polled keyboard/mouse state with per-frame edges.
//
// The Window feeds this from its message loop; game and UI code only read it.
// "Down" queries reflect the current physical state; "Pressed"/"Released"
// queries are edge-triggered and valid for exactly one frame — Main calls
// EndFrame() once per loop iteration to clear them. Key codes are Win32
// virtual-key values ('W', VK_ESCAPE, ...).
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <array>
#include <string>
#include <utility>

namespace dungeon {

// The handful of Win32 virtual-key codes the UI and game logic compare
// against by value. Centralized here so code that must not pull in <Windows.h>
// (the UI library) still has names instead of bare hex literals; matches the
// VK_* macros Win32-facing code uses.
namespace vk {
inline constexpr int Back = 0x08;
inline constexpr int Return = 0x0D;
inline constexpr int Escape = 0x1B;
inline constexpr int Space = 0x20;
inline constexpr int Up = 0x26;
inline constexpr int Down = 0x28;
} // namespace vk

enum class MouseButton { Left, Right, Middle, Count };

// Keyboard/mouse state with per-frame edge detection. The Window feeds this
// from its message loop; game code only reads it.
class Input {
public:
	// --- queries -----------------------------------------------------------
	bool IsKeyDown(int vkey) const { return m_keys[vkey & 0xFF]; }
	bool WasKeyPressed(int vkey) const { return m_keysPressed[vkey & 0xFF]; }
	bool WasKeyReleased(int vkey) const { return m_keysReleased[vkey & 0xFF]; }

	// First keyboard key that went down this frame (virtual-key code), or -1.
	// Lets the UI capture "press any key" rebinding. Starts at VK_BACK (0x08)
	// so the low mouse-button codes can never alias as keys.
	int FirstPressedKey() const {
		for (int vkey = vk::Back; vkey < 256; ++vkey)
			if (m_keysPressed[static_cast<size_t>(vkey)]) return vkey;
		return -1;
	}

	bool IsMouseDown(MouseButton b) const { return m_mouse[std::to_underlying(b)]; }
	bool WasMousePressed(MouseButton b) const { return m_mousePressed[std::to_underlying(b)]; }
	bool WasMouseReleased(MouseButton b) const { return m_mouseReleased[std::to_underlying(b)]; }

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

// Human-readable name for a virtual-key code ("W", "Space", "Caps Lock"),
// from the active keyboard layout — what the Settings key-bind boxes show.
std::string KeyName(int vkey);

} // namespace dungeon
