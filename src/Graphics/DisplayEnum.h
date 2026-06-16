// ============================================================================
// Graphics/DisplayEnum.h — device-independent DXGI display catalog.
//
// Builds the list of installed adapters (GPUs), each adapter's outputs
// (monitors), and the display modes (resolutions) each output supports, from a
// throwaway DXGI factory — no D3D12 device required. The Settings → Video tab
// drives its adapter/monitor/resolution dropdowns from this, and Main reads it
// at boot to honor a saved borderless/exclusive target. Adapters are keyed by
// a packed 64-bit LUID, which is stable across runs so a chosen GPU survives a
// restart (the only way to actually switch adapters — see GraphicsDevice).
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string>
#include <vector>

namespace dungeon::gfx {

// Window vs. full-screen presentation. Windowed/Borderless are driven by the
// window's style + geometry (Window::SetWindowed / SetBorderless); Exclusive
// uses DXGI SetFullscreenState on the chosen output.
enum class FullscreenMode { Windowed, Borderless, Exclusive };

// A unique resolution an output supports (deduped across refresh rates).
struct DisplayMode {
	u32 width = 0;
	u32 height = 0;
};

// One monitor attached to an adapter. The desktop rect (virtual-screen pixels)
// positions a borderless window; `modes` are largest-first.
struct OutputInfo {
	std::string name;                 // friendly label, e.g. "Display 1 (2560x1440)"
	int x = 0, y = 0;                 // desktop position (DesktopCoordinates)
	int width = 0, height = 0;        // current desktop size
	std::vector<DisplayMode> modes;
};

// One GPU and its outputs.
struct AdapterInfo {
	u64 luid = 0;                     // PackLuid(DXGI_ADAPTER_DESC1.AdapterLuid)
	std::string name;                 // DXGI_ADAPTER_DESC1.Description
	std::vector<OutputInfo> outputs;
};

// Packs a Win32 LUID's HighPart/LowPart into one comparable 64-bit value.
u64 PackLuid(i32 highPart, u32 lowPart);

// Enumerates hardware adapters (skips pure-software/WARP), their outputs, and
// each output's unique resolutions for the back-buffer format. May be empty if
// DXGI is unavailable.
std::vector<AdapterInfo> EnumerateAdapters();

} // namespace dungeon::gfx
