#include "Platform/Window.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/StringUtil.h"

#include <Windows.h>
#include <ShellScalingApi.h> // GetDpiForMonitor
#include <windowsx.h>

#include <algorithm>

namespace dungeon {

namespace {
constexpr wchar_t kClassName[] = L"DungeonWindowClass";

// The system cursor for each shape SetCursorShape takes.
const wchar_t* CursorId(Window::Cursor shape) {
	switch (shape) {
	case Window::Cursor::SizeWE: return IDC_SIZEWE;
	case Window::Cursor::SizeAll: return IDC_SIZEALL;
	case Window::Cursor::SizeNWSE: return IDC_SIZENWSE;
	default: return IDC_ARROW;
	}
}
}

Window::Window(const WindowDesc& desc)
	: m_width(desc.width), m_height(desc.height), m_hidden(desc.hidden) {
	const HINSTANCE instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::WndProcThunk);
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = kClassName;
	RegisterClassExW(&wc);

	// The frame round the client at the DPI the window will be made at (the
	// system's, the primary monitor's at sign-in), corrected below once the
	// window knows the monitor it actually landed on.
	RECT rect{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
	const DWORD style = WS_OVERLAPPEDWINDOW;
	AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());

	m_hwnd = CreateWindowExW(0, kClassName, str::Widen(desc.title).c_str(), style,
							 CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
							 rect.bottom - rect.top, nullptr, nullptr, instance, this);
	DN_ASSERT(m_hwnd != nullptr, "CreateWindowExW failed");

	// THE CLIENT IS IN PHYSICAL PIXELS (code-review C201): the exe declares
	// per-monitor-v2 DPI awareness (src/Main/DpiAware.manifest), so the size
	// asked for is the swapchain's, unscaled - and a frame sized for the system
	// DPI is resized for the monitor's own if that differs.
	m_dpi = GetDpiForWindow(m_hwnd);
	if (m_dpi != GetDpiForSystem()) {
		RECT fit{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
		AdjustWindowRectExForDpi(&fit, style, FALSE, 0, m_dpi);
		SetWindowPos(m_hwnd, nullptr, 0, 0, fit.right - fit.left, fit.bottom - fit.top,
					 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	ShowWindow(m_hwnd, desc.hidden ? SW_HIDE : SW_SHOW);
	log::Info("Window created ({}x{}){}", m_width, m_height,
			  desc.hidden ? " [hidden — headless]" : "");
	// The boot's DPI line (InGameTest reads it): what the process is - an exe
	// that lost its manifest says "unaware" here, and Windows then stretches a
	// logical-size swapchain over a scaled monitor, blurred - and the DPI of the
	// monitor the window is on.
	log::Info("dpi: awareness={} window={} scale={}%", DpiAwarenessName(), m_dpi,
			  m_dpi * 100 / 96);
}

const char* Window::DpiAwarenessName() {
	const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
	if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
		return "permonitorv2";
	switch (GetAwarenessFromDpiAwarenessContext(ctx)) {
	case DPI_AWARENESS_PER_MONITOR_AWARE: return "permonitor";
	case DPI_AWARENESS_SYSTEM_AWARE: return "system";
	case DPI_AWARENESS_UNAWARE: return "unaware";
	default: return "invalid";
	}
}

bool Window::Minimized() const { return IsIconic(m_hwnd) != FALSE; }

void Window::PostActivate() const {
	PostMessageW(reinterpret_cast<HWND>(m_hwnd), WM_ACTIVATEAPP, TRUE, 0);
}

Window::~Window() {
	if (m_hwnd) DestroyWindow(m_hwnd);
	UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

void Window::SetWindowed(u32 width, u32 height, const ScreenRect* workArea) {
	const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
	SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
	m_borderless = false;

	// Where it goes: the chosen monitor's work area, else the one it is on now.
	const ScreenRect work = workArea ? *workArea : MonitorWorkArea();

	// The frame round a client: AdjustWindowRectExForDpi grows an empty rect by
	// exactly the border and title bar, so the outer size is the client plus
	// this - at the DPI of the monitor the window is GOING to (C201: a frame
	// measured at the primary's DPI is the wrong size on a scaled second one).
	const UINT dpi = DpiAt(work);
	RECT frame{0, 0, 0, 0};
	AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
	const int frameW = frame.right - frame.left;
	const int frameH = frame.bottom - frame.top;

	// SHRUNK TO FIT: a client whose frame would not fit the work area is cut
	// down until it does, so the title bar is never off the screen and the
	// taskbar never covered. The size is the CLIENT's (the swapchain's), and
	// what it was cut to is what a caller reads back from Width / Height.
	const int clientW = std::clamp(static_cast<int>(width), 1, std::max(1, work.width - frameW));
	const int clientH = std::clamp(static_cast<int>(height), 1, std::max(1, work.height - frameH));
	if (clientW != static_cast<int>(width) || clientH != static_cast<int>(height))
		log::Info("windowed {}x{} does not fit the monitor's work area ({}x{} with a {}x{} "
				  "frame) - {}x{}",
				  width, height, work.width, work.height, frameW, frameH, clientW, clientH);
	const int ww = clientW + frameW;
	const int wh = clientH + frameH;
	// Placed by us, at a size already measured for the target's DPI: the
	// WM_DPICHANGED a cross-monitor move sends is not to rescale it.
	m_placing = true;
	SetWindowPos(hwnd, HWND_TOP, work.x + (work.width - ww) / 2, work.y + (work.height - wh) / 2,
				 ww, wh, ShowFlags());
	m_placing = false;
}

void Window::SetBorderless(int x, int y, u32 width, u32 height) {
	const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
	SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | (m_hidden ? 0 : WS_VISIBLE));
	m_borderless = true;
	m_placing = true; // a monitor's desktop rect, in physical pixels: not rescaled
	SetWindowPos(hwnd, HWND_TOP, x, y, static_cast<int>(width),
				 static_cast<int>(height), ShowFlags());
	m_placing = false;
}

u32 Window::DpiAt(const ScreenRect& area) const {
	const RECT r{area.x, area.y, area.x + area.width, area.y + area.height};
	UINT dx = 0, dy = 0;
	if (SUCCEEDED(GetDpiForMonitor(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST),
								   MDT_EFFECTIVE_DPI, &dx, &dy)) &&
		dx > 0)
		return dx;
	return GetDpiForWindow(m_hwnd);
}

void Window::PostDisplayChange() const {
	// What Windows sends: the primary's bit depth and size as they are now.
	const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
	PostMessageW(reinterpret_cast<HWND>(m_hwnd), WM_DISPLAYCHANGE, 32, MAKELPARAM(w, h));
}

ScreenRect Window::FrameRect() const {
	RECT r{};
	GetWindowRect(reinterpret_cast<HWND>(m_hwnd), &r);
	return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

void* Window::Monitor() const {
	return MonitorFromWindow(reinterpret_cast<HWND>(m_hwnd), MONITOR_DEFAULTTONEAREST);
}

ScreenRect Window::MonitorWorkArea() const {
	MONITORINFO info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(static_cast<HMONITOR>(Monitor()), &info)) {
		// No monitor answered (there is always a nearest one, but say what the
		// fallback is): the primary's work area.
		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		return {work.left, work.top, work.right - work.left, work.bottom - work.top};
	}
	const RECT& w = info.rcWork;
	return {w.left, w.top, w.right - w.left, w.bottom - w.top};
}

// A shown window is shown (and raised) by a mode change; a hidden one is only
// resized - no SWP_SHOWWINDOW, and no activation to take focus from whoever is
// working (C391: every headless run used to show its window).
u32 Window::ShowFlags() const {
	return m_hidden ? (SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER)
					: (SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

bool Window::PumpMessages() {
	MSG msg{};
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	m_input.BeginFrame(); // the frame's typed text is what arrived up to here
	return !m_closed;
}

// ----------------------------------------------------------------------------
// Message routing. Win32 calls a free function; we stash the Window* in the
// window's user data during WM_NCCREATE (it arrives via CREATESTRUCT from the
// CreateWindowExW lpParam) and forward every later message to HandleMessage.
// ----------------------------------------------------------------------------
i64 __stdcall Window::WndProcThunk(HWND__* hwnd, u32 msg, u64 wparam, i64 lparam) {
	Window* self = nullptr;
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
		self = static_cast<Window*>(cs->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		self->m_hwnd = hwnd;
	} else {
		self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	}
	if (self) return self->HandleMessage(msg, wparam, lparam);
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

i64 Window::HandleMessage(u32 msg, u64 wparam, i64 lparam) {
	switch (msg) {
	case WM_CLOSE:
	case WM_DESTROY:
		m_closed = true;
		return 0;

	case WM_SIZE: {
		const u32 w = LOWORD(lparam);
		const u32 h = HIWORD(lparam);
		if (wparam != SIZE_MINIMIZED && w > 0 && h > 0 && (w != m_width || h != m_height)) {
			m_width = w;
			m_height = h;
			if (onResize) onResize(w, h);
		}
		return 0;
	}

	// The window crossed onto a monitor of another scale, or the scale changed
	// under it (code-review C201). A window the PLAYER dragged takes the rect
	// Windows suggests, which keeps it the same apparent size there; one we are
	// placing ourselves (SetWindowed / SetBorderless measured it for that DPI
	// already) or a Borderless one covering a monitor keeps its physical size.
	case WM_DPICHANGED: {
		const u32 dpi = HIWORD(wparam);
		const bool keep = m_placing || m_borderless;
		log::Info("dpi: the window is now at {} dpi ({}%) - {}", dpi, dpi * 100 / 96,
				  keep ? "its size is kept" : "it takes the suggested size");
		m_dpi = dpi;
		if (!keep) {
			const RECT* s = reinterpret_cast<const RECT*>(lparam);
			SetWindowPos(m_hwnd, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top,
						 SWP_NOZORDER | SWP_NOACTIVATE);
		}
		return 0;
	}

	// The application was activated again (Alt+Tab back, a click on it). A COUNT
	// the game polls, as for a display change: an Exclusive full-screen DXGI took
	// away on the way out is re-entered at the top of a frame, never in here -
	// SetFullscreenState from inside the window procedure can deadlock (C194).
	case WM_ACTIVATEAPP:
		if (wparam) ++m_activations;
		return 0;

	case WM_DISPLAYCHANGE:
		++m_displayChanges; // Game re-reads the display list next frame (C199)
		log::Info("display change: {}x{} at {} bpp (the display list is re-read)",
				  LOWORD(lparam), HIWORD(lparam), static_cast<u32>(wparam));
		return 0;

	case WM_SYSKEYDOWN:
		// Alt+F4 goes on to DefWindowProc, which makes it WM_CLOSE - in every
		// display mode, and the only close a Borderless window (WS_POPUP, no close
		// box) or Exclusive one has (code-review C392). Every other system key
		// stays swallowed, so a bare Alt or F10 cannot put the window in menu mode.
		if (wparam == VK_F4) return DefWindowProcW(reinterpret_cast<HWND>(m_hwnd), msg, wparam, lparam);
		[[fallthrough]];
	case WM_KEYDOWN:
		m_input.OnKey(static_cast<int>(wparam), true);
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		m_input.OnKey(static_cast<int>(wparam), false);
		return 0;
	case WM_CHAR:
		// A Unicode window: one UTF-16 unit, half of a surrogate pair for a
		// character past U+FFFF. Input::OnChar joins the halves and encodes UTF-8.
		m_input.OnChar(static_cast<unsigned int>(wparam));
		return 0;

	case WM_LBUTTONDOWN: m_input.OnMouseButton(MouseButton::Left, true); UpdateCapture(); return 0;
	case WM_LBUTTONUP:   m_input.OnMouseButton(MouseButton::Left, false); UpdateCapture(); return 0;
	case WM_RBUTTONDOWN: m_input.OnMouseButton(MouseButton::Right, true); UpdateCapture(); return 0;
	case WM_RBUTTONUP:   m_input.OnMouseButton(MouseButton::Right, false); UpdateCapture(); return 0;
	case WM_MBUTTONDOWN: m_input.OnMouseButton(MouseButton::Middle, true); UpdateCapture(); return 0;
	case WM_MBUTTONUP:   m_input.OnMouseButton(MouseButton::Middle, false); UpdateCapture(); return 0;

	// Focus loss: the up-events for anything held right now go to whoever took
	// focus (Alt+Tab, a stealing debug console, a popup) - drop every DOWN state
	// so no key or button stays wedged "down". Fresh messages re-arm it on
	// return. This is deliberate whole-state hygiene, not per-key bookkeeping -
	// but a press or a character that already arrived is kept (Input::ClearAll).
	case WM_KILLFOCUS:
		m_input.ClearAll();
		return 0;

	// Capture torn away mid-drag (a modal popup, another window's SetCapture):
	// the button-up will never reach us, so unlatch the mouse buttons. Our OWN
	// ReleaseCapture also lands here (sent synchronously from UpdateCapture) —
	// that one is guarded by m_releasingCapture, because clearing then would
	// wipe the release EDGE that triggered it before the frame reads it.
	case WM_CAPTURECHANGED:
		if (!m_releasingCapture) m_input.ClearMouseButtons();
		return 0;

	case WM_MOUSEMOVE:
		m_input.OnMouseMove(static_cast<float>(GET_X_LPARAM(lparam)),
							static_cast<float>(GET_Y_LPARAM(lparam)));
		return 0;

	case WM_MOUSEWHEEL:
		m_input.OnWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA);
		return 0;

	// Over the client area the pointer is the GAME's to shape (SetCursorShape);
	// over the frame, Windows keeps its own sizing arrows.
	case WM_SETCURSOR:
		if (LOWORD(lparam) == HTCLIENT) {
			SetCursor(LoadCursorW(nullptr, CursorId(m_cursor)));
			return TRUE;
		}
		return DefWindowProcW(m_hwnd, msg, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));

	default:
		return DefWindowProcW(m_hwnd, msg, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
	}
}

void Window::SetCursorShape(Cursor shape) {
	if (shape == m_cursor) return;
	m_cursor = shape;
	// Applied now: WM_SETCURSOR only comes with the next move, and not at all
	// while a drag holds capture - a drag must keep the arrow it started with.
	SetCursor(LoadCursorW(nullptr, CursorId(shape)));
}

void Window::UpdateCapture() {
	const bool anyDown = m_input.IsMouseDown(MouseButton::Left) ||
						 m_input.IsMouseDown(MouseButton::Right) ||
						 m_input.IsMouseDown(MouseButton::Middle);
	if (anyDown) {
		SetCapture(m_hwnd); // idempotent while we already hold it
	} else if (GetCapture() == m_hwnd) {
		m_releasingCapture = true; // our own WM_CAPTURECHANGED is not theft
		ReleaseCapture();
		m_releasingCapture = false;
	}
}

} // namespace dungeon
