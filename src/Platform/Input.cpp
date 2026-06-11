#include "Platform/Input.h"

#include <Windows.h>
#include <Xinput.h>

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

void Input::PollGamepad() {
	constexpr SHORT kStickThreshold = 16000; // ~half deflection

	bool down[3] = {false, false, false};
	XINPUT_STATE state{};
	if (XInputGetState(0, &state) == ERROR_SUCCESS) {
		const XINPUT_GAMEPAD& pad = state.Gamepad;
		down[std::to_underlying(PadButton::Up)] =
			(pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) || pad.sThumbLY > kStickThreshold;
		down[std::to_underlying(PadButton::Down)] =
			(pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || pad.sThumbLY < -kStickThreshold;
		down[std::to_underlying(PadButton::A)] = (pad.wButtons & XINPUT_GAMEPAD_A) != 0;
	}
	for (int i = 0; i < 3; ++i) {
		m_padPressed[i] = down[i] && !m_padPrev[i];
		m_padPrev[i] = down[i];
	}
}

void Input::EndFrame() {
	m_keysPressed.fill(false);
	m_keysReleased.fill(false);
	m_mousePressed.fill(false);
	m_mouseReleased.fill(false);
	m_wheel = 0.0f;
	// Gamepad edges are recomputed by PollGamepad each frame.
}

} // namespace dungeon
