#include "Platform/Input.h"

#include "Core/StringUtil.h"
#include "Core/Utf8.h"

#include <Windows.h>

#include <format>
#include <iterator>

namespace dungeon {

void Input::OnKey(int vkey, bool down) {
	vkey &= 0xFF;
	if (down && !m_keys[vkey]) {
		m_keysPressed[vkey] = true;
		// Backspace and Enter also join the typed text, where they fall among
		// the characters (see TypedChars). From the KEY, not from the WM_CHAR
		// TranslateMessage makes of it: OnChar drops control codes, so a real
		// keyboard does not count them twice, and a posted key-down (every
		// harness) arrives with no WM_CHAR at all. On the press only, like the
		// edge - a held Enter must not submit over and over.
		if (vkey == vk::Back) m_typed.push_back(kTypedBack);
		if (vkey == vk::Return) m_typed.push_back(kTypedEnter);
	}
	if (!down && m_keys[vkey]) m_keysReleased[vkey] = true;
	m_keys[vkey] = down;
}

void Input::OnChar(unsigned int unit) {
	// The window is Unicode, so WM_CHAR carries a UTF-16 unit, and a character
	// past U+FFFF comes as TWO of them: a high surrogate, then a low one. Hold
	// the first until the second; a half that arrives alone is dropped, since it
	// is not a character.
	if (unit >= 0xD800 && unit <= 0xDBFF) {
		m_highSurrogate = unit;
		return;
	}
	char32_t cp = unit;
	if (unit >= 0xDC00 && unit <= 0xDFFF) {
		if (m_highSurrogate == 0) return;
		cp = 0x10000 + ((m_highSurrogate - 0xD800) << 10) + (unit - 0xDC00);
	}
	m_highSurrogate = 0;
	// Keep printable characters only, judged by CODE POINT (C0, DEL and C1); Enter
	// and Backspace join the stream from OnKey, and the other control codes (Esc,
	// Tab) reach the consumer through the edge-triggered key queries. Then the
	// whole character, as UTF-8 (TypedChars says why never one byte of it).
	if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return;
	utf8::Append(m_typed, cp);
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
	LONG scan = static_cast<LONG>(MapVirtualKeyW(static_cast<UINT>(vkey),
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
	// The wide call, narrowed to UTF-8: the A form answers in the ANSI code page,
	// so a Cyrillic layout's key names reached the font as bytes it drew as '?'.
	wchar_t name[64];
	if (scan && GetKeyNameTextW(scan, name, static_cast<int>(std::size(name))) > 0)
		return str::Narrow(name);
	return std::format("Key {:#04x}", vkey);
}

void Input::ClearEdges() {
	m_keysPressed.fill(false);
	m_keysReleased.fill(false);
	m_mousePressed.fill(false);
	m_mouseReleased.fill(false);
	m_wheel = 0.0f;
}

void Input::EndFrame() {
	ClearEdges();
	// Only what this frame SHOWED. A character that arrived after BeginFrame -
	// dispatched mid-frame by anything that pumps messages - was never offered
	// to a reader, so it waits for the next frame instead of vanishing. (No
	// allocation: erase keeps the capacity.)
	m_typed.erase(0, m_typedFrame);
	m_typedFrame = 0;
}

void Input::ClearAll() {
	m_keys.fill(false);
	m_mouse.fill(false);
	ClearEdges(); // and every one-frame edge with them - but not the typed text
}

void Input::ClearMouseButtons() {
	m_mouse.fill(false);
	m_mousePressed.fill(false);
	m_mouseReleased.fill(false);
}

} // namespace dungeon
