// ============================================================================
// Platform/Input.h — polled keyboard/mouse/gamepad state with per-frame edges.
//
// The Window feeds keyboard/mouse from its message loop and polls the XInput
// gamepad once per frame; game and UI code only read it. "Down" queries
// reflect the current physical state; "Pressed"/"Released" queries are
// edge-triggered and valid for exactly one frame — Main calls EndFrame()
// once per loop iteration to clear them (gamepad edges recompute each poll
// instead). Key codes are Win32 virtual-key values ('W', VK_ESCAPE, ...).
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <array>
#include <utility>

namespace dungeon {

enum class MouseButton { Left, Right, Middle, Count };

// Gamepad controls the engine cares about (XInput pad 0). Up/Down fold the
// left stick into the d-pad so menu navigation works with either.
enum class PadButton { Up, Down, A, Count };

// Keyboard/mouse state with per-frame edge detection. The Window feeds this
// from its message loop; game code only reads it.
class Input {
public:
	// --- queries -----------------------------------------------------------
	bool IsKeyDown(int vkey) const { return m_keys[vkey & 0xFF]; }
	bool WasKeyPressed(int vkey) const { return m_keysPressed[vkey & 0xFF]; }
	bool WasKeyReleased(int vkey) const { return m_keysReleased[vkey & 0xFF]; }

	bool IsMouseDown(MouseButton b) const { return m_mouse[std::to_underlying(b)]; }
	bool WasMousePressed(MouseButton b) const { return m_mousePressed[std::to_underlying(b)]; }
	bool WasMouseReleased(MouseButton b) const { return m_mouseReleased[std::to_underlying(b)]; }

	bool WasPadPressed(PadButton b) const { return m_padPressed[std::to_underlying(b)]; }

	float MouseX() const { return m_mouseX; }
	float MouseY() const { return m_mouseY; }
	float WheelDelta() const { return m_wheel; }

	// --- driven by Window --------------------------------------------------
	void OnKey(int vkey, bool down);
	void OnMouseButton(MouseButton b, bool down);
	void OnMouseMove(float x, float y);
	void OnWheel(float delta);

	// Polls XInput pad 0 and recomputes press edges; Window calls this once
	// per frame. A missing controller simply reports nothing pressed.
	void PollGamepad();

	// Clears one-frame edge state; call once per frame after the game reads input.
	void EndFrame();

private:
	std::array<bool, 256> m_keys{};
	std::array<bool, 256> m_keysPressed{};
	std::array<bool, 256> m_keysReleased{};
	std::array<bool, 3> m_mouse{};
	std::array<bool, 3> m_mousePressed{};
	std::array<bool, 3> m_mouseReleased{};
	std::array<bool, 3> m_padPressed{};
	std::array<bool, 3> m_padPrev{};
	float m_mouseX = 0.0f;
	float m_mouseY = 0.0f;
	float m_wheel = 0.0f;
};

} // namespace dungeon
