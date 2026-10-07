// ============================================================================
// Platform/Window.h — the Win32 application window and message pump.
//
// Owns the single game window. Raw window messages are translated in
// Window::HandleMessage and fed into the Input object, which the game polls;
// nothing outside the Platform module ever sees a WM_* constant. Resize is
// surfaced through the onResize callback (Main wires it to the swapchain).
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Platform/Input.h"

#include <functional>
#include <string>

// Forward-declare so consumers don't need Windows.h.
struct HWND__;

namespace dungeon {

struct WindowDesc {
	std::string title = "Dungeon";
	u32 width = 1600;
	u32 height = 900;
	// HEADLESS (`-headless`, docs/eval-harness.md): the window is created but
	// never shown. It still EXISTS, because the swapchain is bound to an HWND and
	// removing that would mean removing the graphics device with it — which is a
	// refactor across every gfx call site for no gain, since what costs a headless
	// run is drawing every frame, not owning a device. So the window is real,
	// pumps messages, and is simply invisible.
	bool hidden = false;
};

// A rectangle of the virtual screen, in the pixels SetWindowPos takes: a
// monitor's desktop or work area, a window's frame.
struct ScreenRect {
	int x = 0, y = 0;
	int width = 0, height = 0;
};

// Owns the Win32 window and message pump, and feeds the Input state.
class Window {
public:
	explicit Window(const WindowDesc& desc);
	~Window();

	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;

	// Pumps pending messages; returns false once the window has been closed.
	bool PumpMessages();

	HWND__* Handle() const { return m_hwnd; }
	u32 Width() const { return m_width; }
	u32 Height() const { return m_height; }

	Input& GetInput() { return m_input; }
	const Input& GetInput() const { return m_input; }

	// Display-mode geometry (Settings → Video). SetWindowed restores a bordered,
	// resizable window of the given client size, CENTRED IN `workArea` - the
	// chosen monitor's work area (the desktop less the taskbar), or with none
	// given the work area of the monitor the window is on - and SHRUNK to fit it
	// when the frame round that size would not (code-review C196: it centred on
	// the primary monitor whatever Monitor said, and a native-size client put the
	// title bar off the top of the screen and the frame over the taskbar).
	// SetBorderless makes a frameless window covering the given desktop rect (a
	// monitor's virtual-screen coordinates). Both raise WM_SIZE, so the swapchain
	// resizes through the usual onResize path. Neither SHOWS a hidden window:
	// while IsHidden, they only resize it (code-review C391).
	void SetWindowed(u32 width, u32 height, const ScreenRect* workArea = nullptr);
	void SetBorderless(int x, int y, u32 width, u32 height);
	// Which of the two the window was last given (a new window is Windowed).
	bool IsBorderless() const { return m_borderless; }
	// The window's frame on the virtual screen (GetWindowRect: the client plus
	// its border and title bar), and the monitor it is mostly on - as an opaque
	// HMONITOR, matched against gfx::OutputInfo::monitor, and its work area.
	ScreenRect FrameRect() const;
	void* Monitor() const;
	ScreenRect MonitorWorkArea() const;

	// Created hidden (`-headless`) and never to be shown. Game skips applying a
	// saved display mode while this holds - the only guard that also covers
	// Exclusive fullscreen, which goes through the swapchain, not this window.
	bool IsHidden() const { return m_hidden; }

	// Invoked when the client area changes size (not called for minimize).
	std::function<void(u32, u32)> onResize;

	// How many WM_DISPLAYCHANGE messages have arrived - a monitor plugged in or
	// out, a dock reordering them, a resolution or refresh changed. A COUNT the
	// game polls once a frame (Game::UpdateStates -> GameUI::RefreshDisplays,
	// code-review C199) rather than a callback, so the display list is re-read
	// at the top of a frame, never inside the message pump or a widget's walk.
	// PostDisplayChange posts one to this window, as Windows would (`video
	// displaychange`): the real message, through the real pump.
	u32 DisplayChanges() const { return m_displayChanges; }
	void PostDisplayChange() const;

	// How many times the application has been ACTIVATED (WM_ACTIVATEAPP with
	// TRUE) - polled like DisplayChanges, so Game can re-enter an Exclusive
	// full-screen DXGI dropped on a focus loss (code-review C194) at the top of a
	// frame. PostActivate posts one, as Windows would (`video activate`).
	u32 Activations() const { return m_activations; }
	void PostActivate() const;
	// (Not "IsMinimized": Windows.h defines that as a macro for IsIconic.)
	bool Minimized() const;

	// DPI (code-review C201). The exe is per-monitor-v2 aware (its manifest),
	// so every size here is PHYSICAL pixels; Dpi() is the window's monitor's
	// (96 = 100%), kept current by WM_DPICHANGED. DpiAwarenessName says what the
	// process is: "permonitorv2" unless the manifest was lost ("unaware").
	u32 Dpi() const { return m_dpi; }
	static const char* DpiAwarenessName();

	// The pointer's shape over the client area. The game sets it every frame
	// from what the pointer is over (a floating HUD panel's move grip wants the
	// four-way arrow, its resize grip the diagonal; the map editor's dock edges
	// the left-right one); a change applies at once, even mid-drag, when mouse
	// capture keeps Windows from asking (WM_SETCURSOR).
	enum class Cursor : u8 { Arrow, SizeWE, SizeAll, SizeNWSE };
	void SetCursorShape(Cursor shape);

private:
	static i64 __stdcall WndProcThunk(HWND__* hwnd, u32 msg, u64 wparam, i64 lparam);
	i64 HandleMessage(u32 msg, u64 wparam, i64 lparam);
	// Holds mouse capture while ANY button is down (so drags that leave the
	// client area — free-look, scrollbar thumbs — still deliver their button-up
	// to us) and releases it when the last button lifts.
	void UpdateCapture();
	// SetWindowPos flags for a display-mode change (see Window.cpp).
	u32 ShowFlags() const;
	// The effective DPI of the monitor most of `area` is on.
	u32 DpiAt(const ScreenRect& area) const;

	HWND__* m_hwnd = nullptr;
	u32 m_width = 0;
	u32 m_height = 0;
	bool m_closed = false;
	bool m_hidden = false; // WindowDesc::hidden, kept: see IsHidden
	bool m_borderless = false; // see IsBorderless
	u32 m_displayChanges = 0;  // see DisplayChanges
	u32 m_activations = 0;     // see Activations
	u32 m_dpi = 96;            // see Dpi
	// True while SetWindowed / SetBorderless place the window: the frame was
	// measured for the target monitor's DPI, so the WM_DPICHANGED that a move
	// across monitors sends must not rescale it.
	bool m_placing = false;
	// True while UpdateCapture runs our own ReleaseCapture — Windows SENDS
	// WM_CAPTURECHANGED to the releasing window synchronously, and that
	// self-inflicted one must NOT clear the button edges (the release edge
	// that triggered it is still unread this frame). Only genuine theft
	// (another window taking capture mid-hold) clears.
	bool m_releasingCapture = false;
	Cursor m_cursor = Cursor::Arrow; // see SetCursorShape
	Input m_input;
};

} // namespace dungeon
