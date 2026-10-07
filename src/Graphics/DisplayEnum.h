// ============================================================================
// Graphics/DisplayEnum.h - device-independent display catalog.
//
// Builds, from a throwaway DXGI factory (no D3D12 device), the installed
// ADAPTERS (GPUs) and, SEPARATELY, the MONITORS - every adapter's outputs,
// deduplicated by their GDI device name, plus any monitor Windows knows that no
// adapter listed. The two are independent on purpose (code-review C198): on a
// hybrid laptop the auto-picked GPU is the discrete one, which has no outputs at
// all - the panels hang off the integrated GPU - so a monitor list read off the
// RENDERING adapter was empty, Borderless placed nothing and Apply saved 0x0.
// Under `-warp` the running adapter has no outputs either, which is how
// tools\DisplayTest.py checks this on any machine.
//
// The Settings -> Video tab holds ONE such list (GameUI::RefreshDisplays),
// re-read when the page opens and on WM_DISPLAYCHANGE (C199); Main reads it at
// boot only to turn the saved adapter's IDENTITY into this run's LUID (C197 -
// a LUID does not survive a reboot, see Graphics/AdapterIdentity.h). A monitor
// is saved by its device name ("\\.\DISPLAY2"), never by its place in a list.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Graphics/AdapterIdentity.h"

#include <string>
#include <string_view>
#include <vector>

namespace dungeon::gfx {

// Window vs. full-screen presentation. Windowed/Borderless are driven by the
// window's style + geometry (Window::SetWindowed / SetBorderless); Exclusive
// uses DXGI SetFullscreenState on the chosen output.
enum class FullscreenMode { Windowed, Borderless, Exclusive };

// A unique resolution a monitor supports (deduped across refresh rates).
struct DisplayMode {
	u32 width = 0;
	u32 height = 0;
};

// One monitor. The desktop rect (virtual-screen pixels) positions a borderless
// window; `modes` are largest-first.
struct OutputInfo {
	std::string name;                 // friendly label, e.g. "Display 1 (2560x1440)"
	// The GDI device name ("\\.\DISPLAY1", UTF-8): what settings.ini saves as
	// `monitor=` and what Exclusive full-screen finds its DXGI output by. Stable
	// while the display layout stands; a list index is not (C199).
	std::string device;
	std::string adapter;              // the GPU it hangs off ("" when none said)
	bool primary = false;             // the desktop's primary monitor
	int x = 0, y = 0;                 // desktop position (DesktopCoordinates)
	int width = 0, height = 0;        // current desktop size
	// The WORK AREA - the desktop less the taskbar and docked bars - which a
	// Windowed window is centred in and must fit (Window::SetWindowed, code-
	// review C196). Read with GetMonitorInfoW, in SetWindowPos's coordinates.
	int workX = 0, workY = 0;
	int workWidth = 0, workHeight = 0;
	// The monitor as an opaque HMONITOR, for telling which output a window is
	// on (Window::Monitor); valid while the display layout stands.
	void* monitor = nullptr;
	std::vector<DisplayMode> modes;
};

// One GPU. No outputs here: a monitor belongs to the desktop, not to the GPU
// that renders the game (see the banner).
struct AdapterInfo {
	u64 luid = 0;                     // PackLuid(DXGI_ADAPTER_DESC1.AdapterLuid) - THIS run's
	std::string name;                 // DXGI_ADAPTER_DESC1.Description
	AdapterIdentity identity;         // what is saved (adapter_id=), stable across reboots
	bool software = false;            // WARP: listed only when it is the one running
};

// The Video tab's one list.
struct DisplayList {
	std::vector<AdapterInfo> adapters; // hardware GPUs, in DXGI order
	std::vector<OutputInfo> monitors;  // every monitor, primary first

	// The monitor with that device name, or with that HMONITOR; -1 for none.
	int MonitorIndex(std::string_view device) const;
	int MonitorIndexOf(const void* hmonitor) const;
	// The adapter running with that LUID; -1 for none.
	int AdapterIndex(u64 luid) const;
};

// Packs a Win32 LUID's HighPart/LowPart into one comparable 64-bit value.
u64 PackLuid(i32 highPart, u32 lowPart);

// Enumerates the hardware adapters (skips pure-software/WARP) and every monitor,
// each with its unique resolutions for the back-buffer format. Either list may
// be empty if DXGI and GDI both have nothing to say.
DisplayList EnumerateDisplays();

// The LUID this run's adapter with the saved identity (settings.ini
// `adapter_id=`, AdapterIdentity's spelling) has; 0 for "auto" - nothing saved,
// a spelling that does not read, or that GPU is no longer installed (logged).
u64 ResolveAdapterLuid(std::string_view savedIdentity);

} // namespace dungeon::gfx
