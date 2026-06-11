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

    // Invoked when the client area changes size (not called for minimize).
    std::function<void(u32, u32)> onResize;

private:
    static i64 __stdcall WndProcThunk(HWND__* hwnd, u32 msg, u64 wparam, i64 lparam);
    i64 HandleMessage(u32 msg, u64 wparam, i64 lparam);

    HWND__* m_hwnd = nullptr;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_closed = false;
    Input m_input;
};

} // namespace dungeon
