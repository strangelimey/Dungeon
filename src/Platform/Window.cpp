#include "Platform/Window.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/StringUtil.h"

#include <Windows.h>
#include <windowsx.h>

namespace dungeon {

namespace {
constexpr wchar_t kClassName[] = L"DungeonWindowClass";
}

Window::Window(const WindowDesc& desc) : m_width(desc.width), m_height(desc.height) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::WndProcThunk);
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    RECT rect{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);

    m_hwnd = CreateWindowExW(0, kClassName, str::Widen(desc.title).c_str(), style,
                             CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                             rect.bottom - rect.top, nullptr, nullptr, instance, this);
    DN_ASSERT(m_hwnd != nullptr, "CreateWindowExW failed");

    ShowWindow(m_hwnd, SW_SHOW);
    log::Info("Window created ({}x{})", m_width, m_height);
}

Window::~Window() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

bool Window::PumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
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

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        m_input.OnKey(static_cast<int>(wparam), true);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        m_input.OnKey(static_cast<int>(wparam), false);
        return 0;

    case WM_LBUTTONDOWN: m_input.OnMouseButton(MouseButton::Left, true); SetCapture(m_hwnd); return 0;
    case WM_LBUTTONUP:   m_input.OnMouseButton(MouseButton::Left, false); ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN: m_input.OnMouseButton(MouseButton::Right, true); return 0;
    case WM_RBUTTONUP:   m_input.OnMouseButton(MouseButton::Right, false); return 0;
    case WM_MBUTTONDOWN: m_input.OnMouseButton(MouseButton::Middle, true); return 0;
    case WM_MBUTTONUP:   m_input.OnMouseButton(MouseButton::Middle, false); return 0;

    case WM_MOUSEMOVE:
        m_input.OnMouseMove(static_cast<float>(GET_X_LPARAM(lparam)),
                            static_cast<float>(GET_Y_LPARAM(lparam)));
        return 0;

    case WM_MOUSEWHEEL:
        m_input.OnWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA);
        return 0;

    default:
        return DefWindowProcW(m_hwnd, msg, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    }
}

} // namespace dungeon
