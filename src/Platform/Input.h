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
#include <string_view>
#include <utility>

namespace dungeon {

// The handful of Win32 virtual-key codes the UI and game logic compare
// against by value. Centralized here so code that must not pull in <Windows.h>
// (the UI library) still has names instead of bare hex literals; matches the
// VK_* macros Win32-facing code uses.
namespace vk {
inline constexpr int Back = 0x08;
inline constexpr int Return = 0x0D;
inline constexpr int Control = 0x11;
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

	// The text typed this frame, IN THE ORDER IT HAPPENED: printable characters
	// as UTF-8 plus kTypedBack for each Backspace press and kTypedEnter for each
	// Enter press. Lets a text field (the dev console) accumulate input without
	// decoding virtual keys itself.
	//
	// WHOLE UTF-8 CHARACTERS (code-review C383). OnChar encodes each WM_CHAR,
	// a surrogate pair included, so a u-umlaut is two bytes and a Cyrillic
	// letter two, never one byte cut from the UTF-16 unit - which drew '?', put
	// control bytes in a Russian name, and made c-caron (U+010D) an Enter. The
	// only bytes under 0x20 are kTypedBack and kTypedEnter, which no multi-byte
	// sequence contains, so a consumer walks utf8::CharAt (Core/Utf8.h), tests
	// a character's first byte for the two, and deletes with utf8::PopBack.
	//
	// ONE stream rather than characters plus key edges, because the edges carry
	// no order: a frame that took `...t<Enter>s` (a heavy frame batches them)
	// used to read as `...ts` then Enter, so the line ran with the next line's
	// first letter on it. A consumer walks the stream and applies Backspace and
	// Enter where they fall, skipping any control character it has no use for.
	//
	// The view is fixed for the whole frame (BeginFrame sets it), so every reader
	// sees the same text even if a character arrives mid-frame - that one is
	// held for the NEXT frame rather than cleared unread by EndFrame. Index it
	// afresh rather than holding the view across code that might pump messages.
	std::string_view TypedChars() const { return std::string_view(m_typed).substr(0, m_typedFrame); }
	static constexpr char kTypedBack = '\b';
	static constexpr char kTypedEnter = '\r';

	bool IsMouseDown(MouseButton b) const { return m_mouse[std::to_underlying(b)]; }
	bool WasMousePressed(MouseButton b) const { return m_mousePressed[std::to_underlying(b)]; }
	bool WasMouseReleased(MouseButton b) const { return m_mouseReleased[std::to_underlying(b)]; }

	float MouseX() const { return m_mouseX; }
	float MouseY() const { return m_mouseY; }
	float WheelDelta() const { return m_wheel; }

	// --- driven by Window --------------------------------------------------
	void OnKey(int vkey, bool down);
	void OnChar(unsigned int unit); // WM_CHAR (a UTF-16 unit): appends printable chars
	void OnMouseButton(MouseButton b, bool down);
	void OnMouseMove(float x, float y);
	void OnWheel(float delta);

	// Fixes this frame's typed text (TypedChars) at what has arrived so far. The
	// Window calls it once the frame's messages are pumped.
	void BeginFrame() { m_typedFrame = m_typed.size(); }

	// Clears one-frame edge state and the typed text this frame showed; call
	// once per frame after the game reads input. Text that arrived after
	// BeginFrame is kept for the next frame.
	void EndFrame();

	// Drops every keyboard/mouse DOWN-state, mouse position kept. The Window
	// calls this on focus loss: the matching up-events go to whoever took
	// focus, so anything still "down" here would be stuck down (a party that
	// walks forever on a swallowed W-up). Everything re-arms from fresh
	// messages when focus returns.
	//
	// TYPED TEXT AND THIS FRAME'S PRESS/RELEASE EDGES SURVIVE IT. A character or
	// a press is a finished event with nothing left to arrive, so there is
	// nothing to wedge - and clearing it lost whatever arrived in the same
	// message pump as the focus change (any window taking the foreground: a
	// notification, another harness launching its own game). That is how
	// `sheet status` reached the console as `shee status`, and how a console
	// toggle posted by a harness vanished, leaving the console shut while every
	// later command went nowhere (tools\TypingTest.ps1 TOGGLE phase).
	void ClearAll();

	// Throws this frame's typed text away unread: all of it, or with
	// `throughEnter` only up to and including its first Enter (the rest stays
	// for this frame's readers). Returns whether what it threw away ENDED a
	// line. For `inputpoke` ONLY - the deliberate loss tools\TypingTest.ps1
	// -SelfTest must be seen to catch.
	bool DiscardTypedForTest(bool throughEnter) {
		size_t n = m_typedFrame;
		if (throughEnter) {
			const size_t enter = TypedChars().find(kTypedEnter);
			if (enter != std::string_view::npos) n = enter + 1;
		}
		const bool endsLine = n > 0 && m_typed[n - 1] == kTypedEnter;
		m_typed.erase(0, n);
		m_typedFrame -= n;
		return endsLine;
	}
	// Drops the mouse-button down-states + edges only (keyboard untouched).
	// The Window calls this when mouse capture is torn away mid-drag — the
	// button-up will never arrive, so the drag must not stay latched.
	void ClearMouseButtons();

private:
	void ClearEdges(); // the one-frame key/mouse edges and the wheel

	std::array<bool, 256> m_keys{};
	std::array<bool, 256> m_keysPressed{};
	std::array<bool, 256> m_keysReleased{};
	std::array<bool, 3> m_mouse{};
	std::array<bool, 3> m_mousePressed{};
	std::array<bool, 3> m_mouseReleased{};
	std::string m_typed;     // typed text not yet cleared, in arrival order
	size_t m_typedFrame = 0; // how much of it this frame shows (BeginFrame)
	unsigned int m_highSurrogate = 0; // the first half of a pair, until the second
	float m_mouseX = 0.0f;
	float m_mouseY = 0.0f;
	float m_wheel = 0.0f;
};

// Human-readable name for a virtual-key code ("W", "Space", "Caps Lock"),
// from the active keyboard layout - what the Settings key-bind boxes show. UTF-8,
// like every string the font draws (a Russian layout names its keys in Cyrillic).
std::string KeyName(int vkey);

} // namespace dungeon
